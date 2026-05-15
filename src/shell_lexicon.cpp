#include "tze/shell_lexicon.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tze {
namespace {

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

std::string squeeze_spaces(std::string_view value) {
    std::string out;
    bool prior_space = false;
    for (char c : value) {
        const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (is_space) {
            if (!prior_space) {
                out.push_back(' ');
            }
            prior_space = true;
        } else {
            out.push_back(c);
            prior_space = false;
        }
    }
    return trim(out);
}

std::string normalize_phrase(std::string_view value) {
    const std::string lowered = lowercase(value);
    std::string filtered;
    filtered.reserve(lowered.size());
    for (char c : lowered) {
        const bool keep = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '/' || c == '.' || c == '-' || c == '_';
        filtered.push_back(keep ? c : ' ');
    }
    return squeeze_spaces(filtered);
}

using BuiltinDictionary = std::map<std::string, ShellLexiconEntry>;

BuiltinDictionary build_dictionary() {
    BuiltinDictionary dict;
    const auto add = [&dict](std::string phrase,
                             std::string canonical,
                             std::string category,
                             double confidence,
                             std::vector<std::string> notes = {},
                             bool clarification = false) {
        ShellLexiconEntry entry;
        entry.phrase = phrase;
        entry.canonical = canonical;
        entry.category = category;
        entry.confidence = confidence;
        entry.correction_notes = std::move(notes);
        entry.clarification_required = clarification;
        dict.emplace(normalize_phrase(phrase), std::move(entry));
    };

    add("hello", "hello", "greeting", 0.99);
    add("hi", "hello", "greeting", 0.98);
    add("hey", "hello", "greeting", 0.97);
    add("who are you", "who are you", "omni_identity", 0.99);
    add("what are you", "who are you", "omni_identity", 0.96);
    add("who am i", "who am i", "operator_identity", 0.99);
    add("what matters", "what matters", "context_summary_request", 0.99);
    add("what matters here", "what matters", "context_summary_request", 0.99);
    add("tell me what matters", "what matters", "context_summary_request", 0.97);
    add("what should i care about", "what matters", "context_summary_request", 0.97);
    add("what is matter", "matter", "general_definition_query", 0.94);
    add("what is the sun", "sun", "general_definition_query", 0.97);
    add("define matter", "matter", "general_definition_query", 0.94);
    add("matter in science", "matter", "general_definition_query", 0.91, {"domain_hint=science"});
    add("what next", "next", "next_action", 0.98);
    add("next", "next", "next_action", 0.97);
    add("next?", "next", "next_action", 0.97);
    add("what should i do next", "next", "next_action", 0.99);
    add("fix this", "fix this", "repair", 0.99);
    add("commands", "/help", "shell_help", 0.99);
    add("omni commands", "/help", "shell_help", 0.99);
    add("help", "/help", "shell_help", 0.98);
    add("history", "memory history", "memory_history", 0.99);
    add("exit", "/quit", "shell_exit", 0.99);
    add("quit", "/quit", "shell_exit", 0.99);
    add("close", "/quit", "shell_exit", 0.97);
    add("leave shell", "/quit", "shell_exit", 0.98);
    add("omni exit", "/quit", "shell_exit", 0.99);
    add("omni quit shell", "/quit", "shell_exit", 0.99);
    add("always -v", "__shell_pref_full__", "verbosity_full", 0.99);
    add("always verbose", "__shell_pref_full__", "verbosity_full", 0.99);
    add("show full results", "__shell_pref_full__", "verbosity_full", 0.99);
    add("be verbose", "__shell_pref_full__", "verbosity_full", 0.97);
    add("compact", "__shell_pref_compact__", "verbosity_compact", 0.98);
    add("be brief", "__shell_pref_compact__", "verbosity_compact", 0.97);
    add("short output", "__shell_pref_compact__", "verbosity_compact", 0.97);
    add("premise mode", "persona mode premise", "persona_mode", 0.99);
    add("cynic mode", "persona mode cynic", "persona_mode", 0.99);
    add("professional mode", "persona mode professional", "persona_mode", 0.99);
    add("neutral mode", "persona mode neutral", "persona_mode", 0.98);
    add("default mode", "persona mode neutral", "persona_mode", 0.97);
    add("use nmap", "Run NMAP", "safe_tool_nmap", 0.94);
    add("use nmap against local /24", "run nmap of local /24 and output all results", "safe_tool_nmap", 0.99);
    add("run nmap on localhost", "Run NMAP Scan", "safe_tool_nmap", 0.97);
    add("use namp locally 127.0.0.1",
        "Run NMAP Scan",
        "safe_tool_nmap",
        0.93,
        {"corrected namp -> nmap"});
    return dict;
}

const BuiltinDictionary& dictionary() {
    static const BuiltinDictionary dict = build_dictionary();
    return dict;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

std::optional<ShellLexiconEntry> ShellLexicon::normalize(std::string_view input,
                                                         const MemorySnapshot& memory,
                                                         bool shell_mode) const {
    const std::string normalized = normalize_phrase(input);
    if (normalized.empty()) {
        return std::nullopt;
    }

    for (const ShellLexiconEntry& overlay : memory.shell_lexicon_overlay) {
        if (normalize_phrase(overlay.phrase) == normalized) {
            return overlay;
        }
    }

    const auto exact = dictionary().find(normalized);
    if (exact != dictionary().end()) {
        return exact->second;
    }

    if (shell_mode) {
        if (contains(normalized, "namp") &&
            (contains(normalized, "127.0.0.1") || contains(normalized, "localhost") ||
             contains(normalized, "local") || contains(normalized, "locally"))) {
            ShellLexiconEntry entry;
            entry.phrase = std::string(input);
            entry.canonical = "Run NMAP Scan";
            entry.category = "safe_tool_nmap";
            entry.confidence = 0.91;
            entry.correction_notes = {"corrected namp -> nmap"};
            return entry;
        }
        if (contains(normalized, "nmap") &&
            (contains(normalized, "local /24") || contains(normalized, "local/24") ||
             contains(normalized, "/24 local") || contains(normalized, "/24 locally") ||
             contains(normalized, "locally /24"))) {
            ShellLexiconEntry entry;
            entry.phrase = std::string(input);
            entry.canonical = "run nmap of local /24 and output all results";
            entry.category = "safe_tool_nmap";
            entry.confidence = 0.97;
            if (contains(normalized, "-v") || contains(normalized, "verbose")) {
                entry.correction_notes.push_back("preserve-full-results");
            }
            return entry;
        }
        if (contains(normalized, "localhost") && contains(normalized, "nmap")) {
            ShellLexiconEntry entry;
            entry.phrase = std::string(input);
            entry.canonical = "Run NMAP Scan";
            entry.category = "safe_tool_nmap";
            entry.confidence = 0.95;
            return entry;
        }
    }

    return std::nullopt;
}

}  // namespace tze
