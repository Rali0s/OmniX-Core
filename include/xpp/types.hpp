#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpp {

enum class PseudoDialect {
    XppCppLike,
    PythonLike,
};

enum class NodeKind {
    Comment,
    ClassDeclaration,
    FunctionDeclaration,
    CallExpression,
    Conditional,
    Loop,
    Directive,
    MapNamespace,
    RawStatement,
};

enum class MappingStatus {
    Mapped,
    Abstracted,
    Stubbed,
    Unsupported,
};

enum class SemanticFamily {
    Unknown,
    Io,
    Query,
    Storage,
    Preprocessor,
    Language,
    OmniBridge,
    SecuritySafe,
    SecurityBlocked,
};

struct RawStatement {
    std::string text;
};

struct PseudoNode {
    NodeKind kind = NodeKind::RawStatement;
    std::size_t line = 0;
    std::size_t indent = 0;
    std::string text;
    std::string subject;
    std::vector<std::string> symbols;
    bool harmful = false;
    std::optional<RawStatement> raw_statement;
};

struct SectionNode {
    std::string title;
    std::string slug;
    std::size_t line_start = 0;
    std::size_t line_end = 0;
    std::vector<PseudoNode> nodes;
};

struct StageNode {
    std::string stage_id;
    std::string source_symbol;
    std::string section_title;
    std::size_t line = 0;
    std::string source_excerpt;
    std::vector<std::string> storage_effects;
    std::optional<std::string> next_stage_id;
};

struct StageGraph {
    std::string graph_id;
    std::string section_title;
    std::vector<StageNode> stages;
};

struct MappingUnit {
    std::string source_name;
    PseudoDialect dialect = PseudoDialect::XppCppLike;
    std::vector<std::string> lines;
    std::vector<PseudoNode> preamble;
    std::vector<SectionNode> sections;
    std::vector<StageGraph> stage_graphs;
};

struct SymbolOccurrence {
    std::string raw_symbol;
    std::string normalized_symbol;
    std::string section_title;
    std::size_t line = 0;
    NodeKind node_kind = NodeKind::RawStatement;
    bool harmful = false;
};

struct SymbolMapping {
    std::string raw_symbol;
    std::string normalized_symbol;
    std::string inferred_meaning;
    std::string mapped_cpp_target;
    MappingStatus status = MappingStatus::Stubbed;
    SemanticFamily family = SemanticFamily::Unknown;
    std::vector<SymbolOccurrence> occurrences;
};

struct SymbolIndex {
    std::vector<SymbolMapping> mappings;
};

struct EmitOptions {
    std::filesystem::path output_dir;
    std::string root_namespace = "generated::xpp";
    bool overwrite = true;
};

struct EmitArtifact {
    std::filesystem::path path;
    std::string section_title;
    std::vector<std::string> exported_symbols;
    bool contains_unsupported = false;
};

struct EmitReport {
    std::vector<EmitArtifact> artifacts;
    std::vector<std::string> diagnostics;
    std::filesystem::path manifest_path;
};

std::string_view to_string(NodeKind kind);
std::string_view to_string(PseudoDialect dialect);
std::string_view to_string(MappingStatus status);
std::string_view to_string(SemanticFamily family);

}  // namespace xpp
