#include "tze/language_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "tze/query_runtime.hpp"

namespace tze {
namespace {

constexpr int kMaxLanguageCandidates = 4;

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

bool contains_token(const std::vector<std::string>& tokens, std::string_view needle) {
    return std::find(tokens.begin(), tokens.end(), needle) != tokens.end();
}

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    return value;
}

std::string fallback_language() {
    const std::string lc_all = env_value("LC_ALL");
    if (!lc_all.empty()) {
        return lc_all;
    }
    const std::string lc_messages = env_value("LC_MESSAGES");
    if (!lc_messages.empty()) {
        return lc_messages;
    }
    const std::string lang = env_value("LANG");
    if (!lang.empty()) {
        return lang;
    }
    return "en_US.UTF-8";
}

std::string normalize_locale(std::string_view locale) {
    std::string value = trim(locale);
    const std::size_t dot = value.find('.');
    if (dot != std::string::npos) {
        value.erase(dot);
    }
    const std::size_t at = value.find('@');
    if (at != std::string::npos) {
        value.erase(at);
    }
    if (value.empty()) {
        value = "en_US";
    }
    return value;
}

std::string language_family_for(std::string_view locale) {
    const std::string lowered = lowercase(locale);
    if (lowered.rfind("en", 0) == 0) {
        return "English";
    }
    if (lowered.rfind("es", 0) == 0) {
        return "Spanish";
    }
    if (lowered.rfind("fr", 0) == 0) {
        return "French";
    }
    if (lowered.rfind("de", 0) == 0) {
        return "German";
    }
    if (lowered.rfind("pt", 0) == 0) {
        return "Portuguese";
    }
    if (lowered.rfind("ja", 0) == 0) {
        return "Japanese";
    }
    if (lowered.rfind("zh", 0) == 0) {
        return "Chinese";
    }
    if (lowered.rfind("it", 0) == 0) {
        return "Italian";
    }
    return "UnknownLanguage";
}

std::string family_for_os(std::string_view os) {
    const std::string lowered = lowercase(os);
    if (lowered == "macos" || lowered == "apple") {
        return "Unix";
    }
    if (lowered == "linux") {
        return "Unix";
    }
    if (lowered == "windows") {
        return "Win32";
    }
    return "Generic";
}

void add_reason(LanguageCandidate& candidate, std::string reason) {
    if (!reason.empty()) {
        candidate.evidence.push_back(std::move(reason));
    }
}

void clamp_and_sort(std::vector<LanguageCandidate>& candidates) {
    for (LanguageCandidate& candidate : candidates) {
        candidate.probability = std::max(0.0, std::min(0.99, candidate.probability));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const LanguageCandidate& lhs, const LanguageCandidate& rhs) {
        if (lhs.probability != rhs.probability) {
            return lhs.probability > rhs.probability;
        }
        return lhs.label < rhs.label;
    });
    if (candidates.size() > kMaxLanguageCandidates) {
        candidates.resize(kMaxLanguageCandidates);
    }
}

std::vector<LanguageCandidate> build_os_candidates(std::string_view native_os,
                                                   const std::vector<std::string>& query_tokens,
                                                   const MemorySnapshot& memory) {
    std::vector<LanguageCandidate> candidates = {
        {"os", "Linux", 0.18, "candidate", {},},
        {"os", "macOS", 0.18, "candidate", {},},
        {"os", "Windows", 0.18, "candidate", {},},
        {"os", "Unix", 0.12, "candidate", {},},
    };

    for (LanguageCandidate& candidate : candidates) {
        if (candidate.label == native_os) {
            candidate.probability += 0.48;
            add_reason(candidate, "native-os");
        }
        if (contains_token(query_tokens, lowercase(candidate.label))) {
            candidate.probability += 0.18;
            add_reason(candidate, "query-token");
        }
        if (candidate.label == "Unix" && family_for_os(native_os) == "Unix") {
            candidate.probability += 0.12;
            add_reason(candidate, "os-family");
        }
        for (const LanguageResolutionRecord& record : memory.language_contexts) {
            if (record.selected_os == candidate.label) {
                candidate.probability += 0.05;
                add_reason(candidate, "memory-hit");
                break;
            }
        }
    }

    clamp_and_sort(candidates);
    return candidates;
}

std::vector<LanguageCandidate> build_language_candidates(std::string_view normalized_locale,
                                                         const std::vector<std::string>& query_tokens,
                                                         const MemorySnapshot& memory) {
    const std::string locale = std::string(normalized_locale);
    const std::string family = language_family_for(locale);
    std::vector<LanguageCandidate> candidates = {
        {"language", locale, 0.46, "candidate", {},},
        {"language", family, family == "UnknownLanguage" ? 0.16 : 0.32, "candidate", {},},
        {"language", "system-default", 0.18, "candidate", {},},
        {"language", "manual-language-review", 0.08, "candidate", {},},
    };

    for (LanguageCandidate& candidate : candidates) {
        if (candidate.label == locale) {
            add_reason(candidate, "observed-locale");
        }
        if (candidate.label == family && family != "UnknownLanguage") {
            add_reason(candidate, "locale-family");
        }
        if (contains_token(query_tokens, "language") || contains_token(query_tokens, "translate")) {
            candidate.probability += 0.07;
            add_reason(candidate, "language-query");
        }
        if (contains_token(query_tokens, lowercase(candidate.label))) {
            candidate.probability += 0.14;
            add_reason(candidate, "query-token");
        }
        for (const LanguageResolutionRecord& record : memory.language_contexts) {
            if (record.selected_language == candidate.label || record.observed_locale == candidate.label) {
                candidate.probability += 0.05;
                add_reason(candidate, "memory-hit");
                break;
            }
        }
    }

    clamp_and_sort(candidates);
    return candidates;
}

double gap_between_top_two(const std::vector<LanguageCandidate>& candidates) {
    if (candidates.size() < 2) {
        return candidates.empty() ? 0.0 : candidates.front().probability;
    }
    return candidates.front().probability - candidates[1].probability;
}

void narrow_candidates(std::vector<LanguageCandidate>& candidates,
                       std::vector<std::string>& trace,
                       std::string_view label,
                       int pass_index) {
    if (candidates.size() <= 2) {
        return;
    }

    const std::size_t keep = pass_index == 1 ? 3 : 2;
    candidates.resize(std::min<std::size_t>(keep, candidates.size()));
    trace.push_back(std::string(label) + ":pass-" + std::to_string(pass_index) +
                    " narrowed-to=" + std::to_string(candidates.size()));
}

void mark_candidate_statuses(std::vector<LanguageCandidate>& candidates, std::string_view selected_label) {
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].label == selected_label) {
            candidates[index].status = "selected";
        } else if (index < 2) {
            candidates[index].status = "narrowed";
        } else {
            candidates[index].status = "candidate";
        }
    }
}

void apply_manual_confirmation(LanguageResolutionRecord& record, std::string_view confirmation_mode) {
    const std::string mode = lowercase(confirmation_mode);
    if (mode == "yes" || mode == "y") {
        record.manual_confirmation_used = true;
        record.manual_confirmation_response = "yes";
        record.manual_confirmation_required = false;
        record.confidence = std::min(0.99, record.confidence + 0.10);
        record.reasoning_trace.push_back("manual-confirmation=yes");
        return;
    }
    if (mode == "no" || mode == "n") {
        record.manual_confirmation_used = true;
        record.manual_confirmation_response = "no";
        record.manual_confirmation_required = true;
        record.reasoning_trace.push_back("manual-confirmation=no");
        if (record.language_candidates.size() > 1) {
            record.selected_language = record.language_candidates[1].label;
        }
        record.combined_context = record.selected_os + ":" + record.selected_language;
        record.confidence = std::max(0.35, record.confidence - 0.18);
        return;
    }
}

}  // namespace

LanguageResolutionRecord LanguageEngine::resolve_context(std::string_view query,
                                                         std::string_view source_map_path,
                                                         std::string_view confirmation_mode,
                                                         const MemorySnapshot& memory,
                                                         QuerySessionRecord* query_session) {
    LanguageResolutionRecord record;
    record.query = trim(query);
    record.native_os = read_native_os();
    record.observed_locale = normalize_locale(detect_native_language());
    record.id = "lang-" + std::to_string(std::hash<std::string>{}(
        record.query + "|" + record.native_os + "|" + record.observed_locale));
    record.persisted_at = {};

    const std::vector<std::string> query_tokens = tokenize(record.query + " " + std::string(source_map_path));
    record.os_candidates = build_os_candidates(record.native_os, query_tokens, memory);
    record.language_candidates = build_language_candidates(record.observed_locale, query_tokens, memory);

    if (query_session != nullptr) {
        QueryRuntime runtime;
        runtime.index_values(*query_session,
                             "language-evidence",
                             {record.native_os, record.observed_locale, std::string(source_map_path), record.query});
    }

    for (int pass = 1; pass <= 3; ++pass) {
        record.passes = pass;
        if (!record.os_candidates.empty()) {
            record.selected_os = record.os_candidates.front().label;
        }
        if (!record.language_candidates.empty()) {
            record.selected_language = record.language_candidates.front().label;
        }

        const double os_gap = gap_between_top_two(record.os_candidates);
        const double lang_gap = gap_between_top_two(record.language_candidates);
        const double os_top = record.os_candidates.empty() ? 0.0 : record.os_candidates.front().probability;
        const double lang_top = record.language_candidates.empty() ? 0.0 : record.language_candidates.front().probability;
        record.confidence = std::min(0.99, (os_top * 0.55) + (lang_top * 0.45) + ((os_gap + lang_gap) * 0.20));

        std::ostringstream trace;
        trace << "pass-" << pass << " os=" << record.selected_os << "(" << os_top << ")"
              << " lang=" << record.selected_language << "(" << lang_top << ")"
              << " confidence=" << record.confidence;
        record.reasoning_trace.push_back(trace.str());

        if (record.confidence >= 0.84 && os_gap >= 0.10 && lang_gap >= 0.10) {
            break;
        }

        narrow_candidates(record.os_candidates, record.reasoning_trace, "os", pass);
        narrow_candidates(record.language_candidates, record.reasoning_trace, "language", pass);
    }

    record.manual_confirmation_prompt = "Can You Read This Prompt? [Y/n]";
    record.manual_confirmation_required =
        record.confidence < 0.78 || contains_token(query_tokens, "dnlio") || contains_token(query_tokens, "translation");
    apply_manual_confirmation(record, confirmation_mode);
    if (record.manual_confirmation_required && record.manual_confirmation_response.empty()) {
        record.reasoning_trace.push_back("manual-confirmation=pending");
    }

    record.combined_context = record.selected_os + ":" + record.selected_language;
    mark_candidate_statuses(record.os_candidates, record.selected_os);
    mark_candidate_statuses(record.language_candidates, record.selected_language);
    return record;
}

std::string LanguageEngine::read_native_os() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(_WIN32)
    return "Windows";
#else
    return "UnknownOS";
#endif
}

std::string LanguageEngine::detect_native_language() {
    return fallback_language();
}

std::string LanguageEngine::detect_native_language_index_operating() {
    return read_native_os() + ":" + normalize_locale(detect_native_language());
}

std::string LanguageEngine::determine_os_language() {
    return detect_native_language_index_operating();
}

std::string LanguageEngine::map_native_os_language() {
    return "NativeOSLanguage<" + determine_os_language() + ">";
}

std::string LanguageEngine::native_language_io(std::string_view label) {
    return "dnlio:" + std::string(label);
}

bool LanguageEngine::permit_unbound_parse() {
    return true;
}

std::string LanguageEngine::check_file_coherence(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return "missing";
    }
    return std::filesystem::is_regular_file(path) ? "coherent" : "non-regular";
}

std::string LanguageEngine::decompress_artifact(std::string_view label) {
    return "decompress:" + std::string(label);
}

std::string LanguageEngine::postprocess_language_detection(std::string_view label) {
    return "postprocess:" + std::string(label);
}

std::string LanguageEngine::store_os_type(std::string_view os_type) {
    return "os-type:" + std::string(os_type);
}

std::string LanguageEngine::translate_deep_language_parse(std::string_view label) {
    return "translate:" + std::string(label);
}

std::string LanguageEngine::trans_language_input(std::string_view input) {
    return std::string(input);
}

std::string LanguageEngine::encrypt_marker(std::string_view label) {
    return "encrypt:" + std::string(label);
}

}  // namespace tze
