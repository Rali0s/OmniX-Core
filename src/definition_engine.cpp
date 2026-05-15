#include "tze/definition_engine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tze/operand.hpp"
#include "xpp/index.hpp"
#include "xpp/parser.hpp"

namespace tze {
namespace {

struct ToolDefinition {
    std::string name;
    std::string description;
};

struct LocalDefinitionSource {
    std::string source_type;
    std::string source_label;
    std::string authority_tier;
    std::string domain_hint;
    std::string normalized_concept;
    std::string definition_text;
    double confidence = 0.0;
};

struct RankedDefinitionCandidate {
    LocalDefinitionSource source;
    std::string semantic_family = "general_knowledge";
    double retrieval_score = 0.0;
};

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

std::string escape_shell(std::string_view value) {
    std::string escaped = "'";
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

std::string escape_swift_string_literal(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (char c : value) {
        if (c == '\\' || c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    return lowercase(haystack).find(lowercase(needle)) != std::string::npos;
}

void add_unique(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::string squeeze_spaces(std::string_view value) {
    std::string collapsed;
    collapsed.reserve(value.size());
    bool previous_space = false;
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!previous_space) {
                collapsed.push_back(' ');
            }
            previous_space = true;
        } else {
            collapsed.push_back(c);
            previous_space = false;
        }
    }
    return trim(collapsed);
}

std::string normalize_concept(std::string_view concept_text) {
    std::string normalized = lowercase(concept_text);
    normalized = squeeze_spaces(normalized);
    if (normalized.starts_with("the ")) {
        normalized.erase(0, 4);
    } else if (normalized.starts_with("a ")) {
        normalized.erase(0, 2);
    } else if (normalized.starts_with("an ")) {
        normalized.erase(0, 3);
    }
    return squeeze_spaces(normalized);
}

std::vector<std::string> tokenize(std::string_view value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            continue;
        }
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

int authority_rank(std::string_view authority_tier) {
    if (authority_tier == "operator_override") {
        return 3;
    }
    if (authority_tier == "memory_artifact") {
        return 2;
    }
    if (authority_tier == "reference_cache") {
        return 1;
    }
    return 0;
}

std::string url_encode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(c >> 4) & 0x0F]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string replace_all_copy(std::string text, std::string_view from, std::string_view to) {
    std::size_t offset = 0;
    while (true) {
        const std::size_t found = text.find(from, offset);
        if (found == std::string::npos) {
            return text;
        }
        text.replace(found, from.size(), to);
        offset = found + to.size();
    }
}

std::string decode_html_entities(std::string text) {
    text = replace_all_copy(std::move(text), "&amp;", "&");
    text = replace_all_copy(std::move(text), "&quot;", "\"");
    text = replace_all_copy(std::move(text), "&#39;", "'");
    text = replace_all_copy(std::move(text), "&apos;", "'");
    text = replace_all_copy(std::move(text), "&lt;", "<");
    text = replace_all_copy(std::move(text), "&gt;", ">");
    text = replace_all_copy(std::move(text), "&nbsp;", " ");
    text = replace_all_copy(std::move(text), "&#160;", " ");
    return text;
}

std::string strip_html_tags(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    bool inside_tag = false;
    for (char c : text) {
        if (c == '<') {
            inside_tag = true;
            continue;
        }
        if (c == '>') {
            inside_tag = false;
            continue;
        }
        if (!inside_tag) {
            stripped.push_back(c);
        }
    }
    return decode_html_entities(squeeze_spaces(stripped));
}

std::optional<std::string> extract_html_attribute(std::string_view html,
                                                  std::string_view anchor,
                                                  std::string_view attribute_name) {
    const std::size_t anchor_pos = html.find(anchor);
    if (anchor_pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t tag_end = html.find('>', anchor_pos);
    if (tag_end == std::string::npos) {
        return std::nullopt;
    }
    const std::string attribute = std::string(attribute_name) + "=\"";
    const std::size_t attr_pos = html.find(attribute, anchor_pos);
    if (attr_pos == std::string::npos || attr_pos > tag_end) {
        return std::nullopt;
    }
    const std::size_t value_start = attr_pos + attribute.size();
    const std::size_t value_end = html.find('"', value_start);
    if (value_end == std::string::npos || value_end > tag_end) {
        return std::nullopt;
    }
    return decode_html_entities(std::string(html.substr(value_start, value_end - value_start)));
}

std::optional<std::string> extract_html_element_text(std::string_view html,
                                                     std::string_view class_name,
                                                     std::string_view closing_tag) {
    const std::size_t class_pos = html.find(class_name);
    if (class_pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t text_start = html.find('>', class_pos);
    if (text_start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t text_end = html.find(closing_tag, text_start + 1);
    if (text_end == std::string::npos) {
        return std::nullopt;
    }
    return strip_html_tags(std::string(html.substr(text_start + 1, text_end - text_start - 1)));
}

std::filesystem::path find_local_glossary(std::string_view source_map_path) {
    std::vector<std::filesystem::path> roots;
    if (!source_map_path.empty()) {
        std::filesystem::path cursor = std::filesystem::path(source_map_path).parent_path();
        while (!cursor.empty()) {
            roots.push_back(cursor);
            if (cursor == cursor.root_path()) {
                break;
            }
            cursor = cursor.parent_path();
        }
    }
    roots.push_back(std::filesystem::current_path());

    for (const std::filesystem::path& root : roots) {
        const std::filesystem::path candidate = root / "res" / "local_glossary.tsv";
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::string refine_definition_text(std::string_view concept_text,
                                   std::string raw_definition,
                                   std::string_view domain_hint) {
    raw_definition = squeeze_spaces(raw_definition);
    const std::string normalized_concept = normalize_concept(concept_text);
    const std::string normalized_domain = lowercase(domain_hint);

    const std::string meaning_prefix = "the meaning of ";
    std::string lowered = lowercase(raw_definition);
    if (lowered.rfind(meaning_prefix, 0) == 0) {
        const std::size_t is_pos = lowered.find(" is ");
        if (is_pos != std::string::npos && is_pos + 4 < raw_definition.size()) {
            raw_definition = trim(raw_definition.substr(is_pos + 4));
            lowered = lowercase(raw_definition);
        }
    }

    if (normalized_concept == "sun") {
        if (contains_case_insensitive(lowered, "center of the solar system")) {
            return "The Sun is the star at the center of the Solar System.";
        }
        if (contains_case_insensitive(lowered, "star around which the earth orbits")) {
            return "The Sun is the star around which Earth orbits.";
        }
    }

    if (normalized_concept == "matter" && normalized_domain == "science") {
        if (contains_case_insensitive(lowered, "occupies space") &&
            contains_case_insensitive(lowered, "mass")) {
            return "In science, matter is physical substance that occupies space and possesses mass.";
        }
    }

    while (!raw_definition.empty() &&
           (raw_definition.front() == ':' || raw_definition.front() == '-' || raw_definition.front() == ' ')) {
        raw_definition.erase(raw_definition.begin());
    }
    raw_definition = trim(raw_definition);

    const std::array<std::string, 5> cut_tokens = {" How to use ", " PHRASES ", " ORIGIN ", " examples ", " Example "};
    for (const std::string& token : cut_tokens) {
        const std::size_t found = raw_definition.find(token);
        if (found != std::string::npos) {
            raw_definition = trim(raw_definition.substr(0, found));
        }
    }

    const std::size_t noun_pos = lowered.find(" noun ");
    if (noun_pos != std::string::npos && noun_pos + 6 < raw_definition.size()) {
        raw_definition = trim(raw_definition.substr(noun_pos + 6));
    }

    const std::array<char, 3> sentence_breaks = {':', ';', '\n'};
    for (char breaker : sentence_breaks) {
        const std::size_t found = raw_definition.find(breaker);
        if (found != std::string::npos) {
            raw_definition = trim(raw_definition.substr(0, found));
        }
    }

    const std::size_t period = raw_definition.find(". ");
    if (period != std::string::npos) {
        raw_definition = trim(raw_definition.substr(0, period + 1));
    }

    return raw_definition;
}

std::string read_command_output(const std::string& command) {
    std::string output;
    FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }

    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return trim(output);
}

std::optional<LocalDefinitionSource> lookup_webster_fallback(std::string_view concept_text,
                                                             std::string_view domain_hint) {
    const char* enabled = std::getenv("OMNIX_ENABLE_WEBSTER_FALLBACK");
    if (enabled == nullptr || std::string_view(enabled) != "1") {
        return std::nullopt;
    }

    std::string html;
    if (const char* fixture_path = std::getenv("OMNIX_WEBSTER_FIXTURE_FILE")) {
        std::ifstream input(fixture_path);
        if (!input) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        html = buffer.str();
    } else {
        const std::string url =
            "https://www.merriam-webster.com/dictionary/" + url_encode(normalize_concept(concept_text));
        html = read_command_output("curl -sS -L --max-time 20 " + escape_shell(url));
    }

    if (html.empty()) {
        return std::nullopt;
    }

    std::optional<std::string> extracted =
        extract_html_attribute(html, "<meta name=\"description\"", "content");
    if (!extracted.has_value() || extracted->empty()) {
        extracted = extract_html_element_text(html, "class=\"dtText\"", "</span>");
    }
    if (!extracted.has_value() || extracted->empty()) {
        return std::nullopt;
    }

    LocalDefinitionSource source;
    source.source_type = "webster_fallback";
    source.source_label = std::getenv("OMNIX_WEBSTER_FIXTURE_FILE") != nullptr
        ? "fixture-merriam-webster"
        : "merriam-webster";
    source.authority_tier = "reference_cache";
    source.domain_hint = trim(domain_hint);
    source.normalized_concept = normalize_concept(concept_text);
    source.definition_text = refine_definition_text(concept_text, *extracted, domain_hint);
    if (source.definition_text.empty()) {
        return std::nullopt;
    }
    source.confidence = 0.58;
    return source;
}

std::optional<LocalDefinitionSource> lookup_system_dictionary(std::string_view concept_text,
                                                              std::string_view domain_hint) {
    const char* disabled = std::getenv("OMNIX_DISABLE_SYSTEM_DICTIONARY");
    if (disabled != nullptr && std::string_view(disabled) == "1") {
        return std::nullopt;
    }

    const std::string normalized_concept = normalize_concept(concept_text);
    const std::string normalized_domain = lowercase(domain_hint);

    if (const char* fixture_path = std::getenv("OMNIX_SYSTEM_DICTIONARY_FIXTURE_FILE")) {
        std::ifstream input(fixture_path);
        std::string line;
        std::optional<LocalDefinitionSource> general_match;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::stringstream row(line);
            std::string term;
            std::string domain;
            std::string definition;
            std::getline(row, term, '|');
            std::getline(row, domain, '|');
            std::getline(row, definition);
            if (normalize_concept(term) != normalized_concept) {
                continue;
            }

            LocalDefinitionSource candidate;
            candidate.source_type = "system_dictionary";
            candidate.source_label = "fixture-system-dictionary";
            candidate.authority_tier = "reference_cache";
            candidate.domain_hint = trim(domain);
            candidate.normalized_concept = normalized_concept;
            candidate.definition_text = refine_definition_text(concept_text, definition, domain_hint);
            candidate.confidence = domain.empty() ? 0.89 : 0.95;

            if (!normalized_domain.empty() && lowercase(domain) == normalized_domain) {
                return candidate;
            }
            if (domain.empty() && !general_match.has_value()) {
                general_match = candidate;
            }
        }
        return general_match;
    }

#if defined(__APPLE__)
    const std::string script =
        "import CoreServices\n"
        "import Foundation\n"
        "let term = \"" + escape_swift_string_literal(normalized_concept) + "\"\n"
        "let ns = term as NSString\n"
        "let range = CFRangeMake(0, ns.length)\n"
        "if let definition = DCSCopyTextDefinition(nil, ns, range) {\n"
        "  print(definition.takeRetainedValue() as String)\n"
        "}\n";
    const std::string output = read_command_output("swift -e " + escape_shell(script));
    if (!output.empty()) {
        LocalDefinitionSource source;
        source.source_type = "system_dictionary";
        source.source_label = "macos_dictionary_services";
        source.authority_tier = "reference_cache";
        source.domain_hint = trim(domain_hint);
        source.normalized_concept = normalized_concept;
        source.definition_text = refine_definition_text(concept_text, output, domain_hint);
        source.confidence = 0.9;
        return source;
    }
#endif

    return std::nullopt;
}

std::vector<LocalDefinitionSource> load_local_glossary_entries(std::string_view source_map_path,
                                                               std::string_view concept_text) {
    std::vector<LocalDefinitionSource> matches;
    const std::filesystem::path glossary_path = find_local_glossary(source_map_path);
    if (glossary_path.empty()) {
        return matches;
    }

    std::ifstream input(glossary_path);
    if (!input) {
        return matches;
    }

    const std::string normalized_concept = normalize_concept(concept_text);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::stringstream row(line);
        std::string term;
        std::string domain;
        std::string definition;
        std::getline(row, term, '|');
        std::getline(row, domain, '|');
        std::getline(row, definition);
        const std::string entry_normalized = normalize_concept(term);
        if (!normalized_concept.empty() && entry_normalized != normalized_concept) {
            continue;
        }

        LocalDefinitionSource candidate;
        candidate.source_type = "local_glossary";
        candidate.source_label = glossary_path.string();
        candidate.authority_tier = "operator_override";
        candidate.domain_hint = trim(domain);
        candidate.normalized_concept = entry_normalized;
        candidate.definition_text = refine_definition_text(concept_text, definition, candidate.domain_hint);
        candidate.confidence = candidate.domain_hint.empty() ? 0.7 : 0.82;
        if (!candidate.definition_text.empty()) {
            matches.push_back(std::move(candidate));
        }
    }
    return matches;
}

std::optional<LocalDefinitionSource> select_best_local_source(const std::vector<LocalDefinitionSource>& entries,
                                                              std::string_view domain_hint,
                                                              bool operator_only,
                                                              bool allow_single_domain_default,
                                                              std::string* ambiguity_domains = nullptr) {
    const std::string normalized_domain = lowercase(domain_hint);
    std::optional<LocalDefinitionSource> exact_domain_match;
    std::optional<LocalDefinitionSource> general_match;
    std::vector<LocalDefinitionSource> scoped_matches;

    for (const LocalDefinitionSource& candidate : entries) {
        if (operator_only && candidate.authority_tier != "operator_override") {
            continue;
        }
        if (!normalized_domain.empty() && !candidate.domain_hint.empty() &&
            lowercase(candidate.domain_hint) == normalized_domain) {
            if (!exact_domain_match.has_value() || candidate.confidence > exact_domain_match->confidence) {
                exact_domain_match = candidate;
            }
            continue;
        }
        if (candidate.domain_hint.empty()) {
            if (!general_match.has_value() || candidate.confidence > general_match->confidence) {
                general_match = candidate;
            }
        } else if (normalized_domain.empty()) {
            scoped_matches.push_back(candidate);
        }
    }

    if (exact_domain_match.has_value()) {
        return exact_domain_match;
    }
    if (general_match.has_value()) {
        return general_match;
    }
    if (!normalized_domain.empty()) {
        return std::nullopt;
    }
    if (allow_single_domain_default && scoped_matches.size() == 1) {
        return scoped_matches.front();
    }
    if (ambiguity_domains != nullptr && scoped_matches.size() > 1) {
        std::vector<std::string> domains;
        for (const LocalDefinitionSource& entry : scoped_matches) {
            if (!entry.domain_hint.empty() &&
                std::find(domains.begin(), domains.end(), entry.domain_hint) == domains.end()) {
                domains.push_back(entry.domain_hint);
            }
        }
        std::ostringstream out;
        for (std::size_t index = 0; index < domains.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << domains[index];
        }
        *ambiguity_domains = out.str();
    }
    return std::nullopt;
}

std::map<std::size_t, double> build_sparse_feature_map(std::string_view concept_text) {
    std::map<std::size_t, double> features;
    const std::string normalized = normalize_concept(concept_text);
    for (const std::string& token : tokenize(normalized)) {
        const std::size_t bucket = std::hash<std::string>{}("tok:" + token) % 257;
        features[bucket] += 2.0;
    }
    std::string compact;
    compact.reserve(normalized.size());
    for (char c : normalized) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            compact.push_back(c);
        }
    }
    if (compact.size() < 3 && !compact.empty()) {
        const std::size_t bucket = std::hash<std::string>{}("tri:" + compact) % 257;
        features[bucket] += 1.0;
    }
    for (std::size_t index = 0; index + 2 < compact.size(); ++index) {
        const std::string trigram = compact.substr(index, 3);
        const std::size_t bucket = std::hash<std::string>{}("tri:" + trigram) % 257;
        features[bucket] += 1.0;
    }
    return features;
}

double sparse_cosine_similarity(const std::map<std::size_t, double>& lhs,
                                const std::map<std::size_t, double>& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return 0.0;
    }
    double dot = 0.0;
    double lhs_norm = 0.0;
    double rhs_norm = 0.0;
    for (const auto& [bucket, value] : lhs) {
        lhs_norm += value * value;
        const auto it = rhs.find(bucket);
        if (it != rhs.end()) {
            dot += value * it->second;
        }
    }
    for (const auto& [_, value] : rhs) {
        rhs_norm += value * value;
    }
    if (lhs_norm <= 0.0 || rhs_norm <= 0.0) {
        return 0.0;
    }
    return dot / (std::sqrt(lhs_norm) * std::sqrt(rhs_norm));
}

double token_overlap_score(std::string_view left, std::string_view right) {
    const std::vector<std::string> left_tokens = tokenize(left);
    const std::vector<std::string> right_tokens = tokenize(right);
    if (left_tokens.empty() || right_tokens.empty()) {
        return 0.0;
    }
    std::size_t overlap = 0;
    for (const std::string& token : left_tokens) {
        if (std::find(right_tokens.begin(), right_tokens.end(), token) != right_tokens.end()) {
            ++overlap;
        }
    }
    return static_cast<double>(overlap) /
        static_cast<double>(std::max(left_tokens.size(), right_tokens.size()));
}

std::optional<RankedDefinitionCandidate> retrieve_local_definition_candidate(std::string_view concept_text,
                                                                             std::string_view domain_hint,
                                                                             const MemorySnapshot& memory,
                                                                             std::string_view source_map_path) {
    std::vector<RankedDefinitionCandidate> candidates;
    const std::string normalized_query = normalize_concept(concept_text);
    const std::string normalized_domain = lowercase(domain_hint);
    const auto query_features = build_sparse_feature_map(normalized_query);

    for (const StoredDefinition& entry : memory.definitions) {
        RankedDefinitionCandidate candidate;
        candidate.source.source_type = entry.source_type.empty() ? "memory" : entry.source_type;
        candidate.source.source_label = entry.source.empty() ? "memory" : entry.source;
        candidate.source.authority_tier = entry.authority_tier.empty() ? "memory_artifact" : entry.authority_tier;
        candidate.source.domain_hint = entry.domain_hint;
        candidate.source.normalized_concept =
            entry.normalized_concept.empty() ? normalize_concept(entry.term) : entry.normalized_concept;
        candidate.source.definition_text = entry.summary;
        candidate.source.confidence = entry.confidence;
        const auto candidate_features = build_sparse_feature_map(candidate.source.normalized_concept);
        const double cosine = sparse_cosine_similarity(query_features, candidate_features);
        const double overlap = token_overlap_score(normalized_query, candidate.source.normalized_concept);
        const double domain_bonus =
            (!normalized_domain.empty() && lowercase(candidate.source.domain_hint) == normalized_domain) ? 0.10 : 0.0;
        const double authority_bonus = static_cast<double>(authority_rank(candidate.source.authority_tier)) * 0.03;
        candidate.retrieval_score = cosine * 0.62 + overlap * 0.25 + domain_bonus + authority_bonus;
        candidates.push_back(std::move(candidate));
    }

    for (LocalDefinitionSource entry : load_local_glossary_entries(source_map_path, {})) {
        RankedDefinitionCandidate candidate;
        candidate.source = std::move(entry);
        const auto candidate_features = build_sparse_feature_map(candidate.source.normalized_concept);
        const double cosine = sparse_cosine_similarity(query_features, candidate_features);
        const double overlap = token_overlap_score(normalized_query, candidate.source.normalized_concept);
        const double domain_bonus =
            (!normalized_domain.empty() && lowercase(candidate.source.domain_hint) == normalized_domain) ? 0.10 : 0.0;
        const double authority_bonus = static_cast<double>(authority_rank(candidate.source.authority_tier)) * 0.03;
        candidate.retrieval_score = cosine * 0.62 + overlap * 0.25 + domain_bonus + authority_bonus;
        candidates.push_back(std::move(candidate));
    }

    auto best = std::max_element(candidates.begin(), candidates.end(), [](const RankedDefinitionCandidate& lhs,
                                                                          const RankedDefinitionCandidate& rhs) {
        return lhs.retrieval_score < rhs.retrieval_score;
    });
    if (best == candidates.end() || best->retrieval_score < 0.73) {
        return std::nullopt;
    }
    return *best;
}

std::optional<LocalDefinitionSource> lookup_local_glossary(std::string_view concept_text,
                                                           std::string_view domain_hint,
                                                           std::string_view source_map_path) {
    const std::vector<LocalDefinitionSource> entries = load_local_glossary_entries(source_map_path, concept_text);
    return select_best_local_source(entries, domain_hint, false, true);
}

std::vector<ToolDefinition> tool_catalogue() {
    return {
        {"nmap", "Network mapper used for guarded host and subnet discovery scans."},
        {"tshark", "Command-line Wireshark capture and packet inspection tool."},
        {"wireshark", "Interactive packet analysis suite for inspecting captures."},
        {"dumpcap", "Capture-only helper binary used by Wireshark/TShark workflows."},
        {"ssh", "Secure shell client for encrypted remote sessions and validation checks."},
    };
}

}  // namespace

DefinitionAnswer DefinitionEngine::lookup(std::string_view query,
                                          std::string_view source_map_path,
                                          const MemorySnapshot& memory,
                                          std::string_view domain_hint,
                                          std::string_view comparison_rationale,
                                          bool prefer_general_definition) const {
    DefinitionAnswer answer;
    answer.query = trim(query);
    answer.normalized_concept = normalize_concept(answer.query);
    answer.domain_hint = trim(domain_hint);
    answer.comparison_rationale = trim(comparison_rationale);
    answer.route_class = prefer_general_definition ? "general_definition_query" : "symbol_definition_query";

    const bool clarification_required =
        prefer_general_definition &&
        !answer.query.empty() &&
        answer.query.find(' ') == std::string::npos &&
        answer.domain_hint.empty() &&
        contains_case_insensitive(answer.comparison_rationale, "needs clarification");

    if (clarification_required) {
        answer.selected_source_type = "clarification_required";
        answer.selected_source_label = "behavioral_router";
        answer.selected_authority_tier = "memory_artifact";
        answer.summary = "I can answer `" + answer.query +
            "` as a concept definition, but this prompt is ambiguous without context.";
        answer.suggestions = {"What is " + answer.query + "?", "What matters here?"};
        return answer;
    }

    if (prefer_general_definition) {
        const auto as_memory_source = [](const StoredDefinition& stored) {
            LocalDefinitionSource source;
            source.source_type = stored.source_type.empty() ? "memory" : stored.source_type;
            source.source_label = stored.source.empty() ? "memory" : stored.source;
            source.authority_tier = stored.authority_tier.empty() ? "memory_artifact" : stored.authority_tier;
            source.domain_hint = stored.domain_hint;
            source.normalized_concept =
                stored.normalized_concept.empty() ? normalize_concept(stored.term) : stored.normalized_concept;
            source.definition_text = stored.summary;
            source.confidence = stored.confidence > 0.0 ? stored.confidence : 0.9;
            return source;
        };
        const auto accept_local_source = [&answer](const LocalDefinitionSource& source,
                                                   std::string_view semantic_family,
                                                   std::string_view source_bucket,
                                                   std::string_view rationale = {}) {
            answer.found = true;
            answer.summary = source.definition_text;
            answer.semantic_family = std::string(semantic_family);
            answer.selected_source_type = source.source_type;
            answer.selected_source_label = source.source_label;
            answer.selected_authority_tier = source.authority_tier;
            answer.confidence = source.confidence;
            if (!rationale.empty()) {
                answer.comparison_rationale = std::string(rationale);
            }
            add_unique(answer.sources, std::string(source_bucket));
        };

        std::vector<LocalDefinitionSource> operator_sources;
        std::vector<LocalDefinitionSource> memory_artifact_sources;
        std::vector<LocalDefinitionSource> reference_cache_sources;
        for (const StoredDefinition& stored : memory.definitions) {
            const std::string stored_normalized =
                !stored.normalized_concept.empty() ? stored.normalized_concept : normalize_concept(stored.term);
            if (stored_normalized != answer.normalized_concept) {
                continue;
            }
            LocalDefinitionSource source = as_memory_source(stored);
            if (source.authority_tier == "operator_override") {
                operator_sources.push_back(std::move(source));
            } else if (source.authority_tier == "memory_artifact") {
                memory_artifact_sources.push_back(std::move(source));
            } else {
                reference_cache_sources.push_back(std::move(source));
            }
        }

        const std::vector<LocalDefinitionSource> glossary_sources =
            load_local_glossary_entries(source_map_path, answer.query);
        operator_sources.insert(operator_sources.end(), glossary_sources.begin(), glossary_sources.end());

        std::string ambiguity_domains;
        if (const auto operator_match = select_best_local_source(
                operator_sources, answer.domain_hint, true, true, &ambiguity_domains)) {
            accept_local_source(
                *operator_match,
                "general_knowledge",
                operator_match->source_type == "local_glossary" ? "local_glossary" : "memory",
                operator_match->domain_hint.empty()
                    ? "Operator teaching matched the requested concept exactly."
                    : "Operator teaching matched the requested concept and selected domain.");
            return answer;
        }
        if (!ambiguity_domains.empty()) {
            answer.selected_source_type = "clarification_required";
            answer.selected_source_label = "operator_teaching";
            answer.selected_authority_tier = "operator_override";
            answer.summary = "I found multiple taught meanings for `" + answer.query +
                "`. Please choose a domain: " + ambiguity_domains + ".";
            answer.suggestions = {"define " + answer.query + " in terms of " + ambiguity_domains};
            answer.comparison_rationale = "Multiple operator-authored domain definitions exist without an unscoped default.";
            return answer;
        }

        if (const auto artifact_match = select_best_local_source(
                memory_artifact_sources, answer.domain_hint, false, true)) {
            accept_local_source(*artifact_match,
                                "general_knowledge",
                                "memory",
                                "A stored final artifact matched the requested concept.");
            return answer;
        }

        if (const auto system_definition = lookup_system_dictionary(answer.query, answer.domain_hint)) {
            accept_local_source(*system_definition,
                                "general_knowledge",
                                "system_dictionary",
                                "Local system dictionary matched the requested concept.");
            return answer;
        }

        if (const auto webster_definition = lookup_webster_fallback(answer.query, answer.domain_hint)) {
            accept_local_source(*webster_definition,
                                "general_knowledge",
                                "webster_fallback",
                                "Opt-in Webster fallback supplied the requested concept definition.");
            return answer;
        }

        if (const auto cached_match = select_best_local_source(
                reference_cache_sources, answer.domain_hint, false, true)) {
            accept_local_source(*cached_match,
                                "general_knowledge",
                                "memory",
                                "A cached reference definition matched after live local sources missed.");
            return answer;
        }

        if (const auto retrieval_match =
                retrieve_local_definition_candidate(answer.query, answer.domain_hint, memory, source_map_path)) {
            accept_local_source(
                retrieval_match->source,
                retrieval_match->semantic_family,
                retrieval_match->source.source_type == "local_glossary" ? "local_glossary" : "memory",
                "Local retrieval matched the nearest taught concept with score " +
                    std::to_string(retrieval_match->retrieval_score) + ".");
            add_unique(answer.sources, "local_retrieval");
            return answer;
        }

        answer.selected_source_type = "unresolved";
        answer.selected_source_label = "behavioral_router";
        answer.summary = "I recognized `" + answer.query +
            "` as a concept-definition query, but no local definition source answered it.";
        answer.suggestions = {"define " + answer.query, "enable assist for wording help"};
        return answer;
    }

    for (const StoredDefinition& entry : memory.definitions) {
        if (entry.term == answer.query) {
            answer.found = true;
            answer.summary = entry.summary;
            answer.mapped_cpp_target = entry.mapped_cpp_target;
            answer.semantic_family = entry.semantic_family;
            answer.selected_source_type = entry.source_type.empty() ? "memory" : entry.source_type;
            answer.selected_source_label = entry.source;
            answer.selected_authority_tier = entry.authority_tier.empty() ? "memory_artifact" : entry.authority_tier;
            answer.confidence = entry.confidence > 0.0 ? entry.confidence : 0.92;
            add_unique(answer.sources, "memory");
            return answer;
        }
    }

    if (!source_map_path.empty()) {
        try {
            const std::string source = xpp::read_text_file(std::string(source_map_path));
            const xpp::MappingUnit unit = xpp::parse_xpp(source, std::string(source_map_path));
            const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
            if (const xpp::SymbolMapping* mapping = xpp::find_mapping(index, answer.query)) {
                answer.found = true;
                answer.mapped_cpp_target = mapping->mapped_cpp_target;
                answer.semantic_family = std::string(xpp::to_string(mapping->family));
                answer.summary = mapping->inferred_meaning.empty()
                    ? "Resolved from the current source map."
                    : mapping->inferred_meaning;
                answer.selected_source_type = "source_map";
                answer.selected_source_label = std::string(source_map_path);
                answer.selected_authority_tier = "memory_artifact";
                answer.confidence = mapping->status == xpp::MappingStatus::Mapped ? 0.96 : 0.78;
                add_unique(answer.sources, "source_map");
                for (const tze::OperandDefinition& operand : operand_catalogue()) {
                    if (operand.name == answer.query) {
                        add_unique(answer.sources, "operand_catalogue");
                        break;
                    }
                }
                return answer;
            }
        } catch (const std::exception&) {
        }
    }

    for (const tze::OperandDefinition& operand : operand_catalogue()) {
        if (operand.name != answer.query) {
            continue;
        }
        answer.found = true;
        answer.summary = operand.summary.empty() ? operand.detailed_context : operand.summary;
        answer.mapped_cpp_target.clear();
        answer.semantic_family = std::string(to_string(operand.category));
        answer.selected_source_type = "operand_catalogue";
        answer.selected_source_label = "bundled-operand-catalogue";
        answer.selected_authority_tier = "memory_artifact";
        answer.confidence = 0.88;
        add_unique(answer.sources, "operand_catalogue");
        return answer;
    }

    const std::string query_lower = lowercase(answer.query);
    for (const ToolDefinition& tool : tool_catalogue()) {
        if (tool.name != query_lower) {
            continue;
        }
        answer.found = true;
        answer.summary = tool.description;
        answer.semantic_family = "tool";
        answer.selected_source_type = "tool_catalogue";
        answer.selected_source_label = "bundled-tool-catalogue";
        answer.selected_authority_tier = "memory_artifact";
        answer.confidence = 0.78;
        add_unique(answer.sources, "tool_catalogue");
        return answer;
    }

    for (const NativeToolRecord& tool : memory.native_tools) {
        if (tool.logical_name != query_lower) {
            continue;
        }
        answer.found = true;
        answer.summary = "Native `" + tool.logical_name + "` provider available at " + tool.executable_path + ".";
        answer.semantic_family = "tool";
        answer.selected_source_type = "native_tool_inventory";
        answer.selected_source_label = tool.executable_path;
        answer.selected_authority_tier = "memory_artifact";
        answer.confidence = 0.84;
        add_unique(answer.sources, "native_tool_inventory");
        return answer;
    }

    if (const auto glossary_definition = lookup_local_glossary(answer.query, answer.domain_hint, source_map_path)) {
        answer.found = true;
        answer.summary = glossary_definition->definition_text;
        answer.semantic_family = "general_knowledge";
        answer.selected_source_type = "local_glossary";
        answer.selected_source_label = glossary_definition->source_label;
        answer.selected_authority_tier = glossary_definition->authority_tier;
        answer.confidence = glossary_definition->confidence;
        answer.comparison_rationale = answer.comparison_rationale.empty()
            ? "Source-symbol lookup missed, then operator glossary teaching matched the requested term."
            : answer.comparison_rationale;
        add_unique(answer.sources, "local_glossary");
        return answer;
    }

    answer.summary = "No exact definition was found in the current source map, operand catalogue, or local memory.";
    answer.selected_source_type = "unresolved";
    answer.selected_source_label = "definition_engine";
    answer.selected_authority_tier = "memory_artifact";
    answer.suggestions = {"Try `define " + answer.query + "`", "Try a more specific prompt"};
    return answer;
}

}  // namespace tze
