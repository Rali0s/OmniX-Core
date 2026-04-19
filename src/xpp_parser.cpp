#include "xpp/parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace xpp {
namespace {

constexpr std::string_view kBannerNeedle = ":::";

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

std::size_t indent_of(std::string_view value) {
    std::size_t indent = 0;
    for (char c : value) {
        if (c == ' ') {
            ++indent;
            continue;
        }
        if (c == '\t') {
            indent += 4;
            continue;
        }
        break;
    }
    return indent;
}

std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

bool is_separator_line(std::string_view line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }

    return trimmed.find_first_not_of("<>=-") == std::string::npos;
}

bool is_comment_line(std::string_view line, PseudoDialect dialect) {
    const std::string trimmed = trim(line);
    if (dialect == PseudoDialect::PythonLike) {
        return starts_with(trimmed, "#");
    }
    return starts_with(trimmed, "//") || starts_with(trimmed, "/*") || starts_with(trimmed, "*/");
}

bool is_condition_line(std::string_view line, PseudoDialect dialect) {
    const std::string trimmed = trim(line);
    if (dialect == PseudoDialect::PythonLike) {
        return starts_with(trimmed, "if ") || trimmed == "if" || starts_with(trimmed, "elif ") ||
               starts_with(trimmed, "else") || starts_with(trimmed, "match ") || starts_with(trimmed, "case ");
    }
    return starts_with(trimmed, "if") || starts_with(trimmed, "else") || starts_with(trimmed, "switch");
}

bool is_loop_line(std::string_view line, PseudoDialect dialect) {
    const std::string trimmed = trim(line);
    if (dialect == PseudoDialect::PythonLike) {
        return starts_with(trimmed, "for ") || starts_with(trimmed, "while ");
    }
    return starts_with(trimmed, "for") || starts_with(trimmed, "while");
}

bool is_directive_line(std::string_view line, PseudoDialect dialect) {
    if (dialect == PseudoDialect::PythonLike) {
        return false;
    }
    return starts_with(trim(line), "#");
}

bool contains_map_namespace(std::string_view line) {
    return line.find("xMap_") != std::string::npos || line.find("x.Map_") != std::string::npos ||
           line.find("x_Map_") != std::string::npos || line.find("core_xMap_") != std::string::npos;
}

bool contains_call_syntax(std::string_view line, PseudoDialect dialect) {
    if (dialect == PseudoDialect::PythonLike) {
        return line.find('(') != std::string::npos && (line.find(')') != std::string::npos || line.find(':') != std::string::npos);
    }
    return line.find('(') != std::string::npos && line.find(')') != std::string::npos;
}

std::string sanitize_title(std::string_view title) {
    return trim(title);
}

std::string make_slug(std::string_view title) {
    std::string slug;
    bool previous_underscore = false;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            previous_underscore = false;
            continue;
        }
        if (!previous_underscore && !slug.empty()) {
            slug.push_back('_');
            previous_underscore = true;
        }
    }

    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }

    return slug.empty() ? "section" : slug;
}

bool contains_harmful_semantics(std::string_view line) {
    static const std::vector<std::string> keywords = {
        "penetrat",
        "exploit",
        "rat,",
        "rat ",
        "zer0",
        "overwrite",
        "shred",
        "burnout",
        "kill",
        "destroy",
        "selfpurg",
        "anti-forensic",
        "retaliat",
        "download(",
        "obtainlock",
        "extirpat",
        "honeypot",
        "overclock",
        "ghostengine",
        "predator_fix",
        "self destructor",
        "[ redacted ]",
    };

    const std::string lowered = lowercase(line);
    return std::any_of(keywords.begin(), keywords.end(), [&lowered](const std::string& keyword) {
        return lowered.find(keyword) != std::string::npos;
    });
}

bool contains_symbol(const PseudoNode& node, std::string_view symbol) {
    return std::find(node.symbols.begin(), node.symbols.end(), symbol) != node.symbols.end();
}

const SectionNode* find_section(const MappingUnit& unit, std::string_view title) {
    for (const SectionNode& section : unit.sections) {
        if (section.title == title) {
            return &section;
        }
    }
    return nullptr;
}

std::optional<StageNode> find_stage_after(const SectionNode& section,
                                          std::size_t minimum_line,
                                          std::string_view stage_id,
                                          std::initializer_list<std::string_view> symbols) {
    for (const PseudoNode& node : section.nodes) {
        if (node.line <= minimum_line) {
            continue;
        }
        for (std::string_view symbol : symbols) {
            if (!contains_symbol(node, symbol)) {
                continue;
            }

            StageNode stage;
            stage.stage_id = std::string(stage_id);
            stage.source_symbol = std::string(symbol);
            stage.section_title = section.title;
            stage.line = node.line;
            stage.source_excerpt = trim(node.text);
            if (stage_id == "x.Store") {
                stage.storage_effects.push_back("persist");
            }
            return stage;
        }
    }
    return std::nullopt;
}

std::vector<StageGraph> extract_stage_graphs(const MappingUnit& unit) {
    std::vector<StageGraph> graphs;
    const SectionNode* build_cmake = find_section(unit, "Build CMake");
    if (build_cmake == nullptr) {
        return graphs;
    }

    StageGraph graph;
    graph.graph_id = "build_cmake_stage_graph";
    graph.section_title = build_cmake->title;

    std::size_t last_line = 0;
    const std::array<std::pair<std::string_view, std::initializer_list<std::string_view>>, 5> kStageMatchers = {{
        {"xProcessingCache", {"xProcessingCache"}},
        {"x.Define.Low", {"x.Define.Low"}},
        {"x.DisplayPriorityProcessingGate", {"x.DisplayPriorityProcessingGate"}},
        {"x.DisplayFeedBackLoop", {"x.DisplayFeedBackLoop"}},
        {"x.Store", {"x.Store", "x.store"}},
    }};

    for (const auto& [stage_id, symbols] : kStageMatchers) {
        const std::optional<StageNode> stage = find_stage_after(*build_cmake, last_line, stage_id, symbols);
        if (!stage.has_value()) {
            continue;
        }
        last_line = stage->line;
        graph.stages.push_back(*stage);
    }

    for (std::size_t index = 0; index < graph.stages.size(); ++index) {
        if (index + 1 < graph.stages.size()) {
            graph.stages[index].next_stage_id = graph.stages[index + 1].stage_id;
        }
    }

    if (!graph.stages.empty()) {
        graphs.push_back(std::move(graph));
    }

    return graphs;
}

bool is_symbol_token(std::string_view token) {
    if (token.empty()) {
        return false;
    }

    const std::string lowered = lowercase(token);
    const bool x_prefixed = token[0] == 'x' || token[0] == 'X';
    const bool az_slot = starts_with(token, "aZ::");
    const bool contains_genx = lowered.find("genx") != std::string::npos;
    return x_prefixed || az_slot || contains_genx;
}

std::string trim_symbol(std::string_view token) {
    std::size_t start = 0;
    while (start < token.size()) {
        const char c = token[start];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == 'x' || c == 'X' || c == 'a' || c == 'G') {
            break;
        }
        ++start;
    }

    std::size_t end = token.size();
    while (end > start) {
        const char c = token[end - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':') {
            break;
        }
        --end;
    }

    return std::string(token.substr(start, end - start));
}

std::vector<std::string> extract_symbols(std::string_view line) {
    static const std::vector<std::string> operators = {
        "<~>",
        "-~>",
        "~~>",
        "~~",
    };

    std::vector<std::string> symbols;
    for (const std::string& op : operators) {
        if (line.find(op) != std::string::npos) {
            symbols.push_back(op);
        }
    }

    std::string current;
    auto flush_token = [&symbols, &current]() {
        if (current.empty()) {
            return;
        }

        const std::string trimmed = trim_symbol(current);
        current.clear();
        if (!is_symbol_token(trimmed)) {
            return;
        }

        if (std::find(symbols.begin(), symbols.end(), trimmed) == symbols.end()) {
            symbols.push_back(trimmed);
        }
    };

    for (char c : line) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || c == '+') {
            current.push_back(c);
            continue;
        }
        flush_token();
    }
    flush_token();

    return symbols;
}

NodeKind classify_line(std::string_view line,
                       const std::vector<std::string>& lines,
                       std::size_t index,
                       PseudoDialect dialect) {
    const std::string trimmed = trim(line);
    if (is_comment_line(trimmed, dialect)) {
        return NodeKind::Comment;
    }
    if (starts_with(trimmed, "class ")) {
        return NodeKind::ClassDeclaration;
    }
    if (dialect == PseudoDialect::PythonLike && starts_with(trimmed, "def ")) {
        return NodeKind::FunctionDeclaration;
    }
    if (is_condition_line(trimmed, dialect)) {
        return NodeKind::Conditional;
    }
    if (is_loop_line(trimmed, dialect)) {
        return NodeKind::Loop;
    }
    if (is_directive_line(trimmed, dialect)) {
        return NodeKind::Directive;
    }
    if (contains_map_namespace(trimmed)) {
        return NodeKind::MapNamespace;
    }

    if (contains_call_syntax(trimmed, dialect)) {
        if (dialect == PseudoDialect::PythonLike) {
            return NodeKind::CallExpression;
        }

        for (std::size_t lookahead = index + 1; lookahead < lines.size(); ++lookahead) {
            const std::string next_trimmed = trim(lines[lookahead]);
            if (next_trimmed.empty() || is_comment_line(next_trimmed, dialect)) {
                continue;
            }
            if (next_trimmed == "{") {
                return NodeKind::FunctionDeclaration;
            }
            break;
        }
        return NodeKind::CallExpression;
    }

    return NodeKind::RawStatement;
}

std::string subject_for(std::string_view line, NodeKind kind, PseudoDialect dialect) {
    const std::string trimmed = trim(line);
    switch (kind) {
        case NodeKind::ClassDeclaration: {
            const std::size_t start = trimmed.find(' ');
            std::size_t end = trimmed.find('(');
            if (dialect == PseudoDialect::PythonLike) {
                end = trimmed.find(':');
            }
            if (start != std::string::npos && end != std::string::npos && end > start) {
                return trim(trimmed.substr(start + 1, end - start - 1));
            }
            return trimmed;
        }
        case NodeKind::FunctionDeclaration:
        case NodeKind::CallExpression: {
            const std::size_t prefix = starts_with(trimmed, "def ") ? 4 : 0;
            const std::size_t end = trimmed.find('(');
            if (end != std::string::npos) {
                return trim(trimmed.substr(prefix, end - prefix));
            }
            return trimmed;
        }
        case NodeKind::Directive: {
            const std::size_t end = trimmed.find_first_of(" \t");
            return end == std::string::npos ? trimmed : trimmed.substr(0, end);
        }
        default:
            return trimmed;
    }
}

bool try_parse_section_header(std::string_view line, PseudoDialect dialect, std::string* title) {
    const std::string trimmed = trim(line);
    if (dialect == PseudoDialect::PythonLike) {
        if (starts_with(trimmed, "# Section:")) {
            *title = sanitize_title(trimmed.substr(std::string_view("# Section:").size()));
            return !title->empty();
        }
        if (starts_with(trimmed, "## ")) {
            *title = sanitize_title(trimmed.substr(3));
            return !title->empty();
        }
        return false;
    }

    if (starts_with(trimmed, "X$:")) {
        *title = sanitize_title(trimmed.substr(3));
        return true;
    }

    const std::size_t first = trimmed.find(kBannerNeedle);
    const std::size_t last = trimmed.rfind(kBannerNeedle);
    if (first == std::string::npos || last == std::string::npos || last <= first) {
        return false;
    }

    const std::string candidate = sanitize_title(trimmed.substr(first + kBannerNeedle.size(),
                                                                last - first - kBannerNeedle.size()));
    if (candidate.empty()) {
        return false;
    }

    *title = candidate;
    return true;
}

void finalize_previous_section(MappingUnit& unit, std::size_t current_index, std::size_t line_number) {
    if (current_index >= unit.sections.size()) {
        return;
    }
    unit.sections[current_index].line_end = line_number == 0 ? 0 : line_number - 1;
}

}  // namespace

std::string_view to_string(NodeKind kind) {
    switch (kind) {
        case NodeKind::Comment:
            return "comment";
        case NodeKind::ClassDeclaration:
            return "class";
        case NodeKind::FunctionDeclaration:
            return "function";
        case NodeKind::CallExpression:
            return "call";
        case NodeKind::Conditional:
            return "conditional";
        case NodeKind::Loop:
            return "loop";
        case NodeKind::Directive:
            return "directive";
        case NodeKind::MapNamespace:
            return "map";
        case NodeKind::RawStatement:
            return "raw";
    }
    return "raw";
}

std::string_view to_string(PseudoDialect dialect) {
    switch (dialect) {
        case PseudoDialect::XppCppLike:
            return "xpp_cpp_like";
        case PseudoDialect::PythonLike:
            return "python_like";
    }
    return "xpp_cpp_like";
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to read file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

MappingUnit parse_pseudo(std::string_view source, std::string source_name, PseudoDialect dialect) {
    MappingUnit unit;
    unit.source_name = std::move(source_name);
    unit.dialect = dialect;

    std::istringstream stream{std::string(source)};
    for (std::string line; std::getline(stream, line);) {
        unit.lines.push_back(line);
    }

    std::size_t current_section = unit.sections.size();
    for (std::size_t index = 0; index < unit.lines.size(); ++index) {
        const std::string& line = unit.lines[index];
        const std::string trimmed = trim(line);
        const std::size_t line_number = index + 1;

        std::string section_title;
        if (try_parse_section_header(trimmed, dialect, &section_title)) {
            finalize_previous_section(unit, current_section, line_number);
            SectionNode section;
            section.title = section_title;
            section.slug = make_slug(section_title);
            section.line_start = line_number;
            unit.sections.push_back(std::move(section));
            current_section = unit.sections.size() - 1;
            continue;
        }

        if (trimmed.empty() || is_separator_line(trimmed)) {
            continue;
        }

        PseudoNode node;
        node.kind = classify_line(line, unit.lines, index, dialect);
        node.line = line_number;
        node.indent = indent_of(line);
        node.text = line;
        node.subject = subject_for(line, node.kind, dialect);
        node.symbols = extract_symbols(line);
        node.harmful = contains_harmful_semantics(line);
        if (node.kind == NodeKind::RawStatement) {
            node.raw_statement = RawStatement{line};
        }

        if (current_section < unit.sections.size()) {
            unit.sections[current_section].nodes.push_back(std::move(node));
            continue;
        }

        unit.preamble.push_back(std::move(node));
    }

    finalize_previous_section(unit, current_section, unit.lines.size() + 1);
    unit.stage_graphs = extract_stage_graphs(unit);
    return unit;
}

MappingUnit parse_xpp(std::string_view source) {
    return parse_xpp(source, "memory");
}

MappingUnit parse_xpp(std::string_view source, std::string source_name) {
    return parse_pseudo(source, std::move(source_name), PseudoDialect::XppCppLike);
}

}  // namespace xpp
