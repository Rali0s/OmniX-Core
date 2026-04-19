#include "xpp/emitter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xpp {
namespace {

std::string escape_cpp(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }
    return escaped;
}

std::string sanitize_identifier(std::string_view value) {
    std::string identifier;
    bool previous_underscore = false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            identifier.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            previous_underscore = false;
            continue;
        }
        if (!identifier.empty() && !previous_underscore) {
            identifier.push_back('_');
            previous_underscore = true;
        }
    }

    while (!identifier.empty() && identifier.back() == '_') {
        identifier.pop_back();
    }
    if (identifier.empty()) {
        return "section";
    }
    if (std::isdigit(static_cast<unsigned char>(identifier.front()))) {
        identifier.insert(identifier.begin(), '_');
    }
    return identifier;
}

std::string status_ctor(MappingStatus status) {
    switch (status) {
        case MappingStatus::Mapped:
            return "mapped_symbol";
        case MappingStatus::Abstracted:
            return "abstracted_symbol";
        case MappingStatus::Stubbed:
            return "stubbed_symbol";
        case MappingStatus::Unsupported:
            return "unsupported_symbol";
    }
    return "stubbed_symbol";
}

std::string support_header(std::string_view root_namespace) {
    std::ostringstream out;
    out << "#pragma once\n\n";
    out << "#include <string_view>\n";
    out << "#include <vector>\n\n";
    out << "namespace " << root_namespace << " {\n\n";
    out << "enum class MappingStatus {\n";
    out << "    mapped,\n";
    out << "    abstracted,\n";
    out << "    stubbed,\n";
    out << "    unsupported,\n";
    out << "};\n\n";
    out << "struct GeneratedSymbol {\n";
    out << "    std::string_view raw_symbol;\n";
    out << "    std::string_view normalized_symbol;\n";
    out << "    std::string_view mapped_cpp_target;\n";
    out << "    std::string_view inferred_meaning;\n";
    out << "    std::string_view semantic_family;\n";
    out << "    MappingStatus status;\n";
    out << "};\n\n";
    out << "struct SectionManifest {\n";
    out << "    std::string_view title;\n";
    out << "    std::vector<GeneratedSymbol> (*builder)();\n";
    out << "};\n\n";
    out << "inline GeneratedSymbol mapped_symbol(std::string_view raw_symbol,\n";
    out << "                                   std::string_view normalized_symbol,\n";
    out << "                                   std::string_view mapped_cpp_target,\n";
    out << "                                   std::string_view inferred_meaning,\n";
    out << "                                   std::string_view semantic_family) {\n";
    out << "    return {raw_symbol, normalized_symbol, mapped_cpp_target, inferred_meaning, semantic_family, MappingStatus::mapped};\n";
    out << "}\n\n";
    out << "inline GeneratedSymbol abstracted_symbol(std::string_view raw_symbol,\n";
    out << "                                       std::string_view normalized_symbol,\n";
    out << "                                       std::string_view mapped_cpp_target,\n";
    out << "                                       std::string_view inferred_meaning,\n";
    out << "                                       std::string_view semantic_family) {\n";
    out << "    return {raw_symbol, normalized_symbol, mapped_cpp_target, inferred_meaning, semantic_family, MappingStatus::abstracted};\n";
    out << "}\n\n";
    out << "inline GeneratedSymbol stubbed_symbol(std::string_view raw_symbol,\n";
    out << "                                    std::string_view normalized_symbol,\n";
    out << "                                    std::string_view mapped_cpp_target,\n";
    out << "                                    std::string_view inferred_meaning,\n";
    out << "                                    std::string_view semantic_family) {\n";
    out << "    return {raw_symbol, normalized_symbol, mapped_cpp_target, inferred_meaning, semantic_family, MappingStatus::stubbed};\n";
    out << "}\n\n";
    out << "inline GeneratedSymbol unsupported_symbol(std::string_view raw_symbol,\n";
    out << "                                        std::string_view normalized_symbol,\n";
    out << "                                        std::string_view mapped_cpp_target,\n";
    out << "                                        std::string_view inferred_meaning,\n";
    out << "                                        std::string_view semantic_family) {\n";
    out << "    return {raw_symbol, normalized_symbol, mapped_cpp_target, inferred_meaning, semantic_family, MappingStatus::unsupported};\n";
    out << "}\n\n";
    out << "}  // namespace " << root_namespace << "\n";
    return out.str();
}

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Unable to write generated file: " + path.string());
    }
    output << content;
}

}  // namespace

EmitReport emit_cpp(const MappingUnit& unit, const SymbolIndex& index, const EmitOptions& options) {
    if (options.output_dir.empty()) {
        throw std::runtime_error("EmitOptions.output_dir must not be empty.");
    }

    std::filesystem::create_directories(options.output_dir);

    EmitReport report;
    const std::filesystem::path support_path = options.output_dir / "xpp_generated_support.hpp";
    write_text(support_path, support_header(options.root_namespace));

    std::vector<std::string> manifest_entries;
    struct ManifestSeed {
        std::string title;
        std::string function_name;
        std::string file_name;
    };
    std::vector<ManifestSeed> manifest_seeds;

    for (const SectionNode& section : unit.sections) {
        const std::string identifier = sanitize_identifier(section.slug);
        const std::string function_name = identifier + "_section";
        const std::string file_name = identifier + ".cpp";
        const std::filesystem::path section_path = options.output_dir / file_name;

        std::set<std::string> seen_symbols;
        std::vector<const SymbolMapping*> section_mappings;
        bool contains_unsupported = false;
        for (const SymbolMapping& mapping : index.mappings) {
            const bool in_section = std::any_of(mapping.occurrences.begin(), mapping.occurrences.end(), [&section](const SymbolOccurrence& occurrence) {
                return occurrence.section_title == section.title;
            });
            if (!in_section) {
                continue;
            }
            if (!seen_symbols.insert(mapping.normalized_symbol).second) {
                continue;
            }
            section_mappings.push_back(&mapping);
            contains_unsupported = contains_unsupported || mapping.status == MappingStatus::Unsupported;
        }

        std::ostringstream out;
        out << "#include \"xpp_generated_support.hpp\"\n\n";
        out << "#include <vector>\n\n";
        out << "namespace " << options.root_namespace << " {\n\n";

        for (const PseudoNode& node : section.nodes) {
            if (node.kind != NodeKind::Comment || node.harmful) {
                continue;
            }
            out << "// " << escape_cpp(node.text) << "\n";
        }
        if (contains_unsupported) {
            out << "// Harmful or destructive source branches were intentionally reduced to inert metadata stubs.\n";
        }
        out << "std::vector<GeneratedSymbol> " << function_name << "() {\n";
        out << "    std::vector<GeneratedSymbol> symbols;\n";
        out << "    symbols.reserve(" << section_mappings.size() << ");\n";
        for (const SymbolMapping* mapping : section_mappings) {
            out << "    symbols.push_back(" << status_ctor(mapping->status) << "("
                << "\"" << escape_cpp(mapping->raw_symbol) << "\", "
                << "\"" << escape_cpp(mapping->normalized_symbol) << "\", "
                << "\"" << escape_cpp(mapping->mapped_cpp_target) << "\", "
                << "\"" << escape_cpp(mapping->inferred_meaning) << "\", "
                << "\"" << escape_cpp(to_string(mapping->family)) << "\"));\n";
        }
        out << "    return symbols;\n";
        out << "}\n\n";
        out << "}  // namespace " << options.root_namespace << "\n";

        write_text(section_path, out.str());

        EmitArtifact artifact;
        artifact.path = section_path;
        artifact.section_title = section.title;
        artifact.contains_unsupported = contains_unsupported;
        for (const SymbolMapping* mapping : section_mappings) {
            artifact.exported_symbols.push_back(mapping->raw_symbol);
        }
        report.artifacts.push_back(std::move(artifact));
        manifest_entries.push_back(section_path.string());
        manifest_seeds.push_back({section.title, function_name, file_name});
    }

    std::ostringstream manifest_cpp;
    manifest_cpp << "#include \"xpp_generated_support.hpp\"\n\n";
    for (const ManifestSeed& seed : manifest_seeds) {
        manifest_cpp << "namespace " << options.root_namespace << " {\n";
        manifest_cpp << "std::vector<GeneratedSymbol> " << seed.function_name << "();\n";
        manifest_cpp << "}  // namespace " << options.root_namespace << "\n\n";
    }
    manifest_cpp << "namespace " << options.root_namespace << " {\n\n";
    manifest_cpp << "std::vector<SectionManifest> generated_manifest() {\n";
    manifest_cpp << "    return {\n";
    for (const ManifestSeed& seed : manifest_seeds) {
        manifest_cpp << "        {\"" << escape_cpp(seed.title) << "\", &" << seed.function_name << "},\n";
    }
    manifest_cpp << "    };\n";
    manifest_cpp << "}\n\n";
    manifest_cpp << "}  // namespace " << options.root_namespace << "\n";

    const std::filesystem::path manifest_cpp_path = options.output_dir / "xpp_generated_manifest.cpp";
    write_text(manifest_cpp_path, manifest_cpp.str());
    manifest_entries.push_back(manifest_cpp_path.string());

    report.manifest_path = options.output_dir / "manifest.txt";
    std::ostringstream manifest_text;
    for (const std::string& entry : manifest_entries) {
        manifest_text << entry << '\n';
    }
    write_text(report.manifest_path, manifest_text.str());

    report.diagnostics.push_back("Wrote " + std::to_string(report.artifacts.size()) + " section artifacts.");
    report.diagnostics.push_back("Manifest: " + report.manifest_path.string());
    return report;
}

}  // namespace xpp
