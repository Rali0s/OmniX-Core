#include "tze/preprocessor_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <vector>

#include "tze/query_runtime.hpp"

namespace tze {
namespace {

std::string prefix(std::string_view label, std::string_view value) {
    return std::string(label) + ":" + std::string(value);
}

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

std::string instruction_family_hint_for(const std::vector<std::string>& tokens) {
    if (contains_token(tokens, "build") || contains_token(tokens, "cmake") || contains_token(tokens, "make")) {
        return "build";
    }
    if (contains_token(tokens, "define") || contains_token(tokens, "what") || contains_token(tokens, "who")) {
        return "definition";
    }
    if (contains_token(tokens, "tool") || contains_token(tokens, "nmap") || contains_token(tokens, "tshark")) {
        return "tool";
    }
    if (contains_token(tokens, "case") || contains_token(tokens, "incident") || contains_token(tokens, "analyze")) {
        return "analyst";
    }
    if (contains_token(tokens, "provider") || contains_token(tokens, "assist") || contains_token(tokens, "ollama")) {
        return "provider";
    }
    return "general";
}

std::string env_or(std::string_view name, std::string_view fallback = {}) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        return std::string(fallback);
    }
    return value;
}

std::string current_epoch_marker() {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    std::ostringstream out;
    out << (local_time.tm_year + 1900);
    if (local_time.tm_mon + 1 < 10) {
        out << '0';
    }
    out << (local_time.tm_mon + 1);
    if (local_time.tm_mday < 10) {
        out << '0';
    }
    out << local_time.tm_mday;
    return out.str();
}

std::string machine_identifier() {
    const std::string host = env_or("HOSTNAME", env_or("HOST", "unknown-host"));
    const std::string user = env_or("USER", "unknown-user");
    return host + ":" + user;
}

void add_trace(std::vector<std::string>& trace, std::string value) {
    if (!value.empty()) {
        trace.push_back(std::move(value));
    }
}

void add_trait(std::vector<UacTraitRecord>& traits,
               std::string name,
               std::string value,
               std::string source,
               int weight,
               bool recovery_relevant = false) {
    UacTraitRecord trait;
    trait.trait_name = std::move(name);
    trait.trait_value = std::move(value);
    trait.source = std::move(source);
    trait.weight = weight;
    trait.recovery_relevant = recovery_relevant;
    traits.push_back(std::move(trait));
}

}  // namespace

UacStateRecord PreprocessorRuntime::resolve_uac_state(std::string_view query,
                                                      const MemorySnapshot& memory,
                                                      QuerySessionRecord* query_session) {
    UacStateRecord state;
    state.query = trim(query);
    state.normalized_prompt = lowercase(state.query);
    state.epoch_marker = current_epoch_marker();
    state.machine_identifier = machine_identifier();
    state.chapter_reference = "ChapterIndex.uAC";
    state.chapter_series_label = "Series::LegacyRecovery";
    state.store_namespace = "xMap_Perm_uAC";
    state.search_namespace = "x.index::uAC_Search";
    state.epoch_tier_label = "epoch-tier::bounded-local";
    state.operational_usage_habit = "uAC::OperationalUsageHabit";
    state.genx_token_value = genx_token(state.query.empty() ? "GENx" : state.query);
    state.compression_label = compression_profile(state.query.empty() ? "GenXCompression" : state.query);
    state.encoded_value = encode_value(state.chapter_reference);
    state.encrypted_value = encrypt_value(state.chapter_reference + "(" + state.machine_identifier + ")");
    state.key_store_address_value = key_store_address(state.machine_identifier);
    state.key_budget_value = key_budget(state.epoch_marker);
    state.id = "uac-" + std::to_string(std::hash<std::string>{}(
        state.query + "|" + state.machine_identifier + "|" + state.epoch_marker));

    const std::vector<std::string> tokens = tokenize(state.query);
    state.query_tokens = tokens;
    state.instruction_family_hint = instruction_family_hint_for(tokens);
    add_trait(state.indexed_traits, "uAC_Traits", state.store_namespace, "static", 90, true);
    add_trait(state.indexed_traits, "Epoch", state.epoch_marker, "runtime", 88, true);
    add_trait(state.indexed_traits, "MachineIdentifier", state.machine_identifier, "runtime", 82, true);
    add_trait(state.indexed_traits, "ChapterIndex", state.chapter_reference, "static", 78, false);
    add_trait(state.indexed_traits, "GENx", state.genx_token_value, "generated", 86, false);
    add_trait(state.indexed_traits, "Compression", state.compression_label, "generated", 74, false);

    if (contains_token(tokens, "regenx")) {
        add_trait(state.indexed_traits, "Regeneration", regenerate_token(state.query), "query", 91, true);
        add_trace(state.reasoning_trace, "trait=regenx");
    }
    if (contains_token(tokens, "binarypreprocessor") || contains_token(tokens, "bpp")) {
        add_trait(state.indexed_traits, "BinaryPreProcessor", binary_preprocessor(state.query), "query", 93, true);
        add_trace(state.reasoning_trace, "trait=binary-preprocessor");
    }
    if (contains_token(tokens, "encode")) {
        add_trait(state.indexed_traits, "Encode", state.encoded_value, "query", 79, false);
        add_trace(state.reasoning_trace, "trait=encode");
    }
    if (contains_token(tokens, "encrypt")) {
        add_trait(state.indexed_traits, "Encrypt", state.encrypted_value, "query", 81, true);
        add_trace(state.reasoning_trace, "trait=encrypt");
    }
    if (contains_token(tokens, "compression") || contains_token(tokens, "genxcompression")) {
        add_trait(state.indexed_traits, "CompressionProfile", state.compression_label, "query", 80, false);
        add_trace(state.reasoning_trace, "trait=compression");
    }

    state.recovery_hints = {
        "restoreDeleted",
        "restoreAutoSave",
        "restoreSystemHiddenCache",
        "restoreAllSearchData(KeyStoneClass())",
        "try::USB_Tail(uAC(systemRecovery ~~x))",
        "store(uAC(SystemRecovery))",
    };
    state.deletion_discrepancies = {
        "deleted-artifact::pending-correlation",
        "autosave-shadow::review",
    };
    state.search_context_habits = {
        "search(uAC_Traits)",
        "compare(epoch -> priorEpoch)",
        "reindex(recoveryEvidence)",
    };
    state.time_on_site_traits = {
        "local-session-bounded",
        "epoch-sensitive",
        "operator-habit-aware",
    };
    add_trace(state.reasoning_trace, "recovery-hints=" + std::to_string(state.recovery_hints.size()));
    add_trace(state.reasoning_trace, "chapter-series=" + state.chapter_series_label);
    add_trace(state.reasoning_trace, "epoch-tier=" + state.epoch_tier_label);
    add_trace(state.reasoning_trace, "instruction-family=" + state.instruction_family_hint);
    add_trace(state.reasoning_trace, "token-count=" + std::to_string(state.query_tokens.size()));

    const auto prior = std::find_if(memory.uac_states.rbegin(), memory.uac_states.rend(), [&state](const UacStateRecord& entry) {
        return entry.query == state.query || entry.machine_identifier == state.machine_identifier;
    });
    if (prior != memory.uac_states.rend()) {
        add_trait(state.indexed_traits, "PriorEpoch", prior->epoch_marker, "memory", 72, true);
        add_trace(state.reasoning_trace, "memory-hit=" + prior->id);
    }

    if (query_session != nullptr) {
        QueryRuntime runtime;
        runtime.index_values(*query_session,
                             "uac-evidence",
                             {state.store_namespace,
                              state.search_namespace,
                              state.chapter_series_label,
                              state.epoch_tier_label,
                              state.epoch_marker,
                              state.machine_identifier,
                              state.query,
                              state.genx_token_value});
    }

    std::stable_sort(state.indexed_traits.begin(), state.indexed_traits.end(), [](const UacTraitRecord& lhs, const UacTraitRecord& rhs) {
        if (lhs.weight != rhs.weight) {
            return lhs.weight > rhs.weight;
        }
        return lhs.trait_name < rhs.trait_name;
    });

    add_trace(state.reasoning_trace, "traits=" + std::to_string(state.indexed_traits.size()));
    add_trace(state.reasoning_trace, "epoch=" + state.epoch_marker);
    add_trace(state.reasoning_trace, "machine=" + state.machine_identifier);
    return state;
}

std::string PreprocessorRuntime::genx_token(std::string_view label) {
    return prefix("genx", label);
}

std::string PreprocessorRuntime::genx_engine(std::string_view label) {
    return prefix("genx-engine", label);
}

std::string PreprocessorRuntime::binary_preprocessor(std::string_view label) {
    return prefix("binary-preprocessor", label);
}

std::string PreprocessorRuntime::regenerate_token(std::string_view label) {
    return prefix("regenx", label);
}

std::string PreprocessorRuntime::encode_value(std::string_view label) {
    return prefix("encode", label);
}

std::string PreprocessorRuntime::encrypt_value(std::string_view label) {
    return prefix("encrypt", label);
}

std::string PreprocessorRuntime::compression_profile(std::string_view label) {
    return prefix("compression", label);
}

std::string PreprocessorRuntime::key_store_address(std::string_view label) {
    return prefix("key-store", label);
}

std::string PreprocessorRuntime::key_budget(std::string_view label) {
    return prefix("genx-budget", label);
}

}  // namespace tze
