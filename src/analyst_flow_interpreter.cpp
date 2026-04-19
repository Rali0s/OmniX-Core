#include "tze/analyst_flow_interpreter.hpp"
#include "tze/query_runtime.hpp"
#include "tze/unix_evidence_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>

namespace tze {
namespace {

struct CommandCapture {
    int exit_code = 0;
    std::string output;
};

struct DecisionMetrics {
    bool valid = true;
    int validity_score = 100;
    int evidence_coverage = 0;
    int prior_success_score = 50;
    double confidence = 0.0;
    double probability_likelihood = 0.0;
    std::vector<std::string> supporting_signals;
    std::vector<std::string> validation_checks;
    std::vector<std::string> score_trace;
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

std::string now_timestamp() {
    using clock = std::chrono::system_clock;
    const std::time_t raw = clock::to_time_t(clock::now());
    std::tm local{};
#if defined(__APPLE__) || defined(__unix__)
    localtime_r(&raw, &local);
#else
    local = *std::localtime(&raw);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

std::string sanitize_token(std::string_view value) {
    std::string token;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!token.empty() && token.back() != '-') {
            token.push_back('-');
        }
    }
    while (!token.empty() && token.back() == '-') {
        token.pop_back();
    }
    return token.empty() ? "item" : token;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

int clamp_score(int value) {
    return std::max(0, std::min(100, value));
}

double clamp_probability(double value) {
    return std::max(0.0, std::min(1.0, value));
}

std::string make_id(std::string_view prefix, std::string_view seed) {
    const std::size_t hash_value = std::hash<std::string>{}(std::string(seed));
    std::ostringstream out;
    out << prefix << "-" << hash_value;
    return out.str();
}

void push_unique(std::vector<std::string>& values, std::string value, std::size_t limit) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) != values.end()) {
        return;
    }
    if (values.size() >= limit) {
        return;
    }
    values.push_back(std::move(value));
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

CommandCapture run_command_capture(std::string_view command) {
    CommandCapture capture;
    const std::string full_command = std::string(command) + " 2>&1";
    FILE* pipe = popen(full_command.c_str(), "r");
    if (pipe == nullptr) {
        capture.exit_code = -1;
        capture.output = "Unable to execute command.";
        return capture;
    }

    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        capture.output.append(buffer.data());
    }

    const int status = pclose(pipe);
    if (status == -1) {
        capture.exit_code = -1;
    } else if (WIFEXITED(status)) {
        capture.exit_code = WEXITSTATUS(status);
    } else {
        capture.exit_code = status;
    }
    return capture;
}

std::vector<std::string> split_lines(std::string_view text, std::size_t max_lines = 8) {
    std::vector<std::string> lines;
    std::stringstream stream{std::string(text)};
    for (std::string line; std::getline(stream, line);) {
        if (!trim(line).empty()) {
            lines.push_back(trim(line));
        }
        if (lines.size() >= max_lines) {
            break;
        }
    }
    return lines;
}

std::string summarize_text(std::string_view text, std::size_t max_len = 180) {
    const std::vector<std::string> lines = split_lines(text, 1);
    if (lines.empty()) {
        return "No textual content captured.";
    }
    std::string summary = lines.front();
    if (summary.size() > max_len) {
        summary = summary.substr(0, max_len - 3) + "...";
    }
    return summary;
}

std::size_t line_count(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
}

PermissionContext permission_for(const RequestProfile& profile) {
    PermissionContext context;
    if (profile.operator_is_admin) {
        context.role = "admin";
        context.can_view_raw = true;
        context.can_run_actions = true;
        context.can_store_feedback = true;
    } else {
        context.role = "analyst";
        context.can_view_raw = true;
        context.can_run_actions = true;
        context.can_store_feedback = true;
    }
    return context;
}

CaseRecord* find_case(MemorySnapshot& memory, std::string_view id_or_source) {
    for (CaseRecord& entry : memory.case_records) {
        if (entry.id == id_or_source || entry.primary_source == id_or_source) {
            return &entry;
        }
    }
    return nullptr;
}

const CaseRecord* find_case(const MemorySnapshot& memory, std::string_view id_or_source) {
    for (const CaseRecord& entry : memory.case_records) {
        if (entry.id == id_or_source || entry.primary_source == id_or_source) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<ObservationRecord> observations_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<ObservationRecord> entries;
    for (const ObservationRecord& entry : memory.observations) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::string command_token(std::string_view source_ref) {
    std::string token = trim(source_ref);
    if (token.empty()) {
        return {};
    }
    const std::size_t first_space = token.find_first_of(" \t\r\n");
    if (first_space != std::string::npos) {
        token = token.substr(0, first_space);
    }
    std::filesystem::path path(token);
    if (!path.filename().empty()) {
        token = path.filename().string();
    }
    return lowercase(token);
}

std::string attribute_value(std::string_view attribute, std::string_view prefix) {
    if (!starts_with(attribute, prefix)) {
        return {};
    }
    return std::string(attribute.substr(prefix.size()));
}

std::vector<NormalizedObject> objects_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<NormalizedObject> entries;
    for (const NormalizedObject& entry : memory.normalized_objects) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<EvidenceLink> evidence_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<EvidenceLink> entries;
    for (const EvidenceLink& entry : memory.evidence_links) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<AnalystComment> comments_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<AnalystComment> entries;
    for (const AnalystComment& entry : memory.analyst_comments) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<DecisionCandidate> decisions_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<DecisionCandidate> entries;
    for (const DecisionCandidate& entry : memory.decision_candidates) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const DecisionCandidate& lhs, const DecisionCandidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        if (lhs.probability_likelihood != rhs.probability_likelihood) {
            return lhs.probability_likelihood > rhs.probability_likelihood;
        }
        return lhs.confidence > rhs.confidence;
    });
    return entries;
}

std::vector<CaseLink> links_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<CaseLink> links;
    for (const CaseLink& link : memory.case_links) {
        if (link.left_case_id == case_id || link.right_case_id == case_id) {
            links.push_back(link);
        }
    }
    std::stable_sort(links.begin(), links.end(), [](const CaseLink& lhs, const CaseLink& rhs) {
        return lhs.strength > rhs.strength;
    });
    return links;
}

std::vector<std::string> relevant_history(const MemorySnapshot& memory, std::string_view case_or_source) {
    std::vector<std::string> matches;
    if (case_or_source.empty()) {
        return matches;
    }
    for (auto it = memory.history.rbegin(); it != memory.history.rend(); ++it) {
        if (it->project.find(case_or_source) == std::string::npos &&
            it->prompt.find(case_or_source) == std::string::npos) {
            continue;
        }
        matches.push_back(it->timestamp + " | " + it->status + " | " + it->summary);
        if (matches.size() >= 5) {
            break;
        }
    }
    return matches;
}

std::vector<std::string> unique_sorted(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::vector<std::string> case_signal_values(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<std::string> values;
    for (const NormalizedObject& object : memory.normalized_objects) {
        if (object.case_id != case_id) {
            continue;
        }
        for (const std::string& attribute : object.attributes) {
            if (const std::string value = attribute_value(attribute, "signal="); !value.empty()) {
                values.push_back(value);
            }
        }
    }
    for (const DecisionCandidate& decision : memory.decision_candidates) {
        if (decision.case_id != case_id) {
            continue;
        }
        for (const std::string& signal : decision.supporting_signals) {
            values.push_back(signal);
        }
    }
    return unique_sorted(std::move(values));
}

std::vector<std::string> case_host_values(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<std::string> values;
    for (const NormalizedObject& object : memory.normalized_objects) {
        if (object.case_id != case_id) {
            continue;
        }
        for (const std::string& attribute : object.attributes) {
            if (const std::string ip = attribute_value(attribute, "ip="); !ip.empty()) {
                values.push_back(ip);
            }
            if (const std::string host = attribute_value(attribute, "host="); !host.empty()) {
                values.push_back(host);
            }
        }
    }
    return unique_sorted(std::move(values));
}

std::vector<std::string> case_command_values(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<std::string> values;
    for (const ObservationRecord& observation : memory.observations) {
        if (observation.case_id != case_id || observation.source_kind != "command") {
            continue;
        }
        if (const std::string token = command_token(observation.source_ref); !token.empty()) {
            values.push_back(token);
        }
    }
    for (const NormalizedObject& object : memory.normalized_objects) {
        if (object.case_id != case_id) {
            continue;
        }
        for (const std::string& attribute : object.attributes) {
            if (const std::string command = attribute_value(attribute, "command="); !command.empty()) {
                values.push_back(lowercase(command));
            }
            if (const std::string tool = attribute_value(attribute, "tool="); !tool.empty()) {
                values.push_back(lowercase(tool));
            }
        }
    }
    return unique_sorted(std::move(values));
}

std::vector<std::string> case_source_values(const CaseRecord& case_record) {
    std::vector<std::string> values;
    if (!case_record.primary_source.empty()) {
        values.push_back(lowercase(case_record.primary_source));
        const std::filesystem::path path(case_record.primary_source);
        if (!path.filename().empty()) {
            values.push_back(lowercase(path.filename().string()));
        }
    }
    return unique_sorted(std::move(values));
}

std::vector<std::string> overlap_values(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs) {
    std::vector<std::string> overlap;
    for (const std::string& left : lhs) {
        if (std::find(rhs.begin(), rhs.end(), left) != rhs.end()) {
            overlap.push_back(left);
        }
    }
    return unique_sorted(std::move(overlap));
}

std::vector<CaseLink> derive_case_links(const MemorySnapshot& memory) {
    std::vector<CaseLink> links;
    for (std::size_t left_index = 0; left_index < memory.case_records.size(); ++left_index) {
        const CaseRecord& left = memory.case_records[left_index];
        const std::vector<std::string> left_sources = case_source_values(left);
        const std::vector<std::string> left_signals = case_signal_values(memory, left.id);
        const std::vector<std::string> left_hosts = case_host_values(memory, left.id);
        const std::vector<std::string> left_commands = case_command_values(memory, left.id);

        for (std::size_t right_index = left_index + 1; right_index < memory.case_records.size(); ++right_index) {
            const CaseRecord& right = memory.case_records[right_index];
            const std::vector<std::string> right_sources = case_source_values(right);
            const std::vector<std::string> right_signals = case_signal_values(memory, right.id);
            const std::vector<std::string> right_hosts = case_host_values(memory, right.id);
            const std::vector<std::string> right_commands = case_command_values(memory, right.id);

            const auto add_links = [&](std::string_view type,
                                       const std::vector<std::string>& overlaps,
                                       int strength,
                                       std::string_view rationale_prefix) {
                for (const std::string& value : overlaps) {
                    CaseLink link;
                    link.id = make_id("case-link", left.id + "|" + right.id + "|" + std::string(type) + "|" + value);
                    link.left_case_id = left.id;
                    link.right_case_id = right.id;
                    link.link_type = std::string(type);
                    link.link_value = value;
                    link.rationale = std::string(rationale_prefix) + " `" + value + "`.";
                    link.strength = strength;
                    links.push_back(std::move(link));
                }
            };

            add_links("shared_host",
                      overlap_values(left_hosts, right_hosts),
                      92,
                      "Linked by shared host or IP");
            add_links("shared_source",
                      overlap_values(left_sources, right_sources),
                      85,
                      "Linked by shared source");
            add_links("shared_command",
                      overlap_values(left_commands, right_commands),
                      72,
                      "Linked by shared command or tool");
            add_links("shared_signal",
                      overlap_values(left_signals, right_signals),
                      60,
                      "Linked by shared signal");
        }
    }

    std::stable_sort(links.begin(), links.end(), [](const CaseLink& lhs, const CaseLink& rhs) {
        if (lhs.strength != rhs.strength) {
            return lhs.strength > rhs.strength;
        }
        if (lhs.link_type != rhs.link_type) {
            return lhs.link_type < rhs.link_type;
        }
        return lhs.link_value < rhs.link_value;
    });
    return links;
}

std::string join_values(const std::vector<std::string>& values, std::string_view separator = ", ") {
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << separator;
        }
        out << values[index];
    }
    return out.str();
}

std::vector<CaseCluster> derive_case_clusters(const MemorySnapshot& memory) {
    std::vector<CaseCluster> clusters;
    if (memory.case_records.size() < 2 || memory.case_links.empty()) {
        return clusters;
    }

    std::map<std::string, std::vector<const CaseLink*>> adjacency;
    for (const CaseLink& link : memory.case_links) {
        adjacency[link.left_case_id].push_back(&link);
        adjacency[link.right_case_id].push_back(&link);
    }

    std::set<std::string> visited;
    for (const CaseRecord& case_record : memory.case_records) {
        if (visited.find(case_record.id) != visited.end() || adjacency.find(case_record.id) == adjacency.end()) {
            continue;
        }

        std::vector<std::string> component_cases;
        std::vector<const CaseLink*> component_links;
        std::vector<std::string> queue = {case_record.id};
        visited.insert(case_record.id);

        for (std::size_t index = 0; index < queue.size(); ++index) {
            const std::string& current = queue[index];
            component_cases.push_back(current);
            for (const CaseLink* link : adjacency[current]) {
                component_links.push_back(link);
                const std::string& neighbor = link->left_case_id == current ? link->right_case_id : link->left_case_id;
                if (visited.insert(neighbor).second) {
                    queue.push_back(neighbor);
                }
            }
        }

        component_cases = unique_sorted(std::move(component_cases));
        if (component_cases.size() < 2) {
            continue;
        }

        std::set<std::string> case_set(component_cases.begin(), component_cases.end());
        std::vector<const CaseLink*> internal_links;
        std::set<std::string> seen_links;
        std::vector<std::string> link_types;
        std::vector<std::string> indicators;
        int total_strength = 0;
        int strongest_link = 0;
        bool has_shared_host = false;

        for (const CaseLink* link : component_links) {
            if (case_set.find(link->left_case_id) == case_set.end() || case_set.find(link->right_case_id) == case_set.end()) {
                continue;
            }
            if (!seen_links.insert(link->id).second) {
                continue;
            }
            internal_links.push_back(link);
            total_strength += link->strength;
            strongest_link = std::max(strongest_link, link->strength);
            if (link->link_type == "shared_host") {
                has_shared_host = true;
            }
            if (std::find(link_types.begin(), link_types.end(), link->link_type) == link_types.end()) {
                link_types.push_back(link->link_type);
            }
            if (!link->link_value.empty() &&
                std::find(indicators.begin(), indicators.end(), link->link_value) == indicators.end()) {
                indicators.push_back(link->link_value);
            }
        }

        if (internal_links.empty()) {
            continue;
        }

        const double average_strength = static_cast<double>(total_strength) / static_cast<double>(internal_links.size());
        const double max_edges = std::max(1.0,
            static_cast<double>(component_cases.size() * (component_cases.size() - 1)) / 2.0);
        const double density = static_cast<double>(internal_links.size()) / max_edges;
        const double diversity = static_cast<double>(link_types.size()) / 4.0;
        const double indicator_coverage = std::min(
            1.0,
            static_cast<double>(indicators.size()) / static_cast<double>(component_cases.size()));
        double likelihood = clamp_probability(
            (0.48 * (average_strength / 100.0)) + (0.24 * density) + (0.14 * diversity) + (0.14 * indicator_coverage));
        if (has_shared_host) {
            likelihood = clamp_probability(likelihood + 0.08);
        }

        CaseCluster cluster;
        cluster.id = make_id("case-cluster", join_values(component_cases, "|"));
        cluster.case_ids = component_cases;
        cluster.case_count = static_cast<int>(component_cases.size());
        cluster.link_types = unique_sorted(std::move(link_types));
        cluster.shared_indicators = unique_sorted(std::move(indicators));
        cluster.likelihood = likelihood;
        cluster.correlation_score = clamp_score(static_cast<int>(std::round(likelihood * 100.0)));
        cluster.cluster_type = component_cases.size() >= 3 ? "campaign_cluster" : "incident_cluster";
        const std::string leading_indicator = cluster.shared_indicators.empty() ? "case-graph overlap" : cluster.shared_indicators.front();
        cluster.title = cluster.cluster_type == "campaign_cluster"
            ? "Campaign cluster around `" + leading_indicator + "`"
            : "Incident cluster around `" + leading_indicator + "`";

        std::ostringstream summary;
        summary << "Derived from " << internal_links.size() << " correlated link(s) across "
                << component_cases.size() << " case(s); strongest link=" << strongest_link
                << ", average=" << std::fixed << std::setprecision(1) << average_strength
                << ", density=" << std::setprecision(2) << density
                << ", likelihood=" << likelihood;
        if (!cluster.link_types.empty()) {
            summary << "; link types: " << join_values(cluster.link_types);
        }
        if (!cluster.shared_indicators.empty()) {
            summary << "; indicators: " << join_values(cluster.shared_indicators);
        }
        cluster.summary = summary.str();

        for (const CaseLink* link : internal_links) {
            cluster.link_ids.push_back(link->id);
        }
        clusters.push_back(std::move(cluster));
    }

    std::stable_sort(clusters.begin(), clusters.end(), [](const CaseCluster& lhs, const CaseCluster& rhs) {
        if (lhs.correlation_score != rhs.correlation_score) {
            return lhs.correlation_score > rhs.correlation_score;
        }
        if (lhs.case_count != rhs.case_count) {
            return lhs.case_count > rhs.case_count;
        }
        return lhs.id < rhs.id;
    });
    return clusters;
}

std::vector<CaseCluster> clusters_for_case(const std::vector<CaseCluster>& clusters, std::string_view case_id) {
    std::vector<CaseCluster> filtered;
    for (const CaseCluster& cluster : clusters) {
        if (std::find(cluster.case_ids.begin(), cluster.case_ids.end(), case_id) != cluster.case_ids.end()) {
            filtered.push_back(cluster);
        }
    }
    return filtered;
}

std::vector<CaseCluster> clusters_for_cases(const std::vector<CaseCluster>& clusters,
                                            const std::vector<CaseRecord>& cases) {
    std::set<std::string> case_ids;
    for (const CaseRecord& case_record : cases) {
        case_ids.insert(case_record.id);
    }

    std::vector<CaseCluster> filtered;
    for (const CaseCluster& cluster : clusters) {
        const auto overlaps = std::count_if(cluster.case_ids.begin(),
                                            cluster.case_ids.end(),
                                            [&case_ids](const std::string& id) {
                                                return case_ids.find(id) != case_ids.end();
                                            });
        if (overlaps > 0) {
            filtered.push_back(cluster);
        }
    }
    return filtered;
}

std::vector<CaseRecord> sorted_cases(const MemorySnapshot& memory) {
    std::vector<CaseRecord> cases = memory.case_records;
    std::stable_sort(cases.begin(), cases.end(), [](const CaseRecord& lhs, const CaseRecord& rhs) {
        if (lhs.updated_at != rhs.updated_at) {
            return lhs.updated_at > rhs.updated_at;
        }
        return lhs.id < rhs.id;
    });
    return cases;
}

std::string case_search_blob(const MemorySnapshot& memory, const CaseRecord& case_record) {
    std::ostringstream blob;
    blob << lowercase(case_record.id) << '\n'
         << lowercase(case_record.title) << '\n'
         << lowercase(case_record.primary_source) << '\n'
         << lowercase(case_record.status) << '\n'
         << lowercase(case_record.latest_summary) << '\n';
    for (const ObservationRecord& observation : memory.observations) {
        if (observation.case_id != case_record.id) {
            continue;
        }
        blob << lowercase(observation.source_ref) << '\n'
             << lowercase(observation.summary) << '\n';
    }
    for (const NormalizedObject& object : memory.normalized_objects) {
        if (object.case_id != case_record.id) {
            continue;
        }
        blob << lowercase(object.object_type) << '\n'
             << lowercase(object.title) << '\n'
             << lowercase(object.summary) << '\n';
        for (const std::string& attribute : object.attributes) {
            blob << lowercase(attribute) << '\n';
        }
    }
    for (const AnalystComment& comment : memory.analyst_comments) {
        if (comment.case_id == case_record.id) {
            blob << lowercase(comment.text) << '\n';
        }
    }
    for (const DecisionCandidate& decision : memory.decision_candidates) {
        if (decision.case_id == case_record.id) {
            blob << lowercase(decision.title) << '\n'
                 << lowercase(decision.recommended_command) << '\n';
        }
    }
    return blob.str();
}

std::vector<CaseRecord> search_cases(const MemorySnapshot& memory, std::string_view query) {
    const std::string lowered_query = lowercase(trim(query));
    if (lowered_query.empty()) {
        return {};
    }

    std::vector<CaseRecord> matches;
    for (const CaseRecord& case_record : memory.case_records) {
        if (case_search_blob(memory, case_record).find(lowered_query) != std::string::npos) {
            matches.push_back(case_record);
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const CaseRecord& lhs, const CaseRecord& rhs) {
        if (lhs.updated_at != rhs.updated_at) {
            return lhs.updated_at > rhs.updated_at;
        }
        return lhs.id < rhs.id;
    });
    return matches;
}

std::vector<KnowledgeReference> analyst_references(RequestIntent intent, const PermissionContext& permission) {
    std::vector<KnowledgeReference> references = {
        {"case_store", "Single source of truth for cases, evidence, comments, and decision history.", 1},
        {"normalized_objects", "Deterministic normalized objects derived from raw observations.", 2},
        {"feedback_loop", "Prior Omni outcomes and analyst summaries used for guidance.", 3},
        {"permission_context", "Role-aware access controls for raw evidence and safe local actions.", permission.can_run_actions ? 4 : 2},
    };
    if (intent == RequestIntent::DecideAction) {
        references.push_back({"safe_actions", "Native tool, build, and local search modules available for safe execution.", 2});
    }
    return references;
}

std::vector<std::string> detect_signals(const std::string& text, const std::string& source_ref) {
    const UnixEvidenceParser parser;
    return parser.detect_signals(text, source_ref);
}

std::vector<std::string> intersect_signals(const std::vector<std::string>& detected,
                                           std::initializer_list<std::string_view> relevant) {
    std::vector<std::string> matches;
    for (std::string_view wanted : relevant) {
        if (std::find(detected.begin(), detected.end(), wanted) != detected.end()) {
            matches.push_back(std::string(wanted));
        }
    }
    return matches;
}

bool status_is_success(std::string_view status) {
    return status == "ok" || status == "native_ready" || status == "build_ready" || status == "built" ||
        status == "installed" || status == "analyzed" || status == "decided" || status == "case_loaded";
}

bool status_is_failure(std::string_view status) {
    return status == "failed" || status == "build_failed" || status == "install_failed" ||
        status == "artifact_missing" || status == "doctor_attention_needed" || status == "case_not_found";
}

std::pair<int, int> action_history_counts(const MemorySnapshot& memory, const std::vector<std::string>& keys) {
    int successes = 0;
    int failures = 0;
    if (keys.empty()) {
        return {successes, failures};
    }

    for (const MemoryHistoryEntry& entry : memory.history) {
        bool matched = false;
        for (const std::string& key : keys) {
            if ((!entry.prompt.empty() && entry.prompt.find(key) != std::string::npos) ||
                (!entry.project.empty() && entry.project.find(key) != std::string::npos) ||
                (!entry.summary.empty() && entry.summary.find(key) != std::string::npos)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (status_is_success(entry.status)) {
            ++successes;
        } else if (status_is_failure(entry.status)) {
            ++failures;
        }
    }

    for (const TzeRunRecord& entry : memory.tze_runs) {
        if (entry.feedback_status.empty()) {
            continue;
        }
        bool matched = false;
        for (const std::string& key : keys) {
            if ((!entry.prompt.empty() && entry.prompt.find(key) != std::string::npos) ||
                (!entry.target.empty() && entry.target.find(key) != std::string::npos) ||
                (!entry.next_action.empty() && entry.next_action.find(key) != std::string::npos) ||
                (!entry.linked_case_id.empty() && entry.linked_case_id.find(key) != std::string::npos)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (entry.feedback_status == "helpful") {
            successes += 2;
        } else if (entry.feedback_status == "not-helpful") {
            failures += 2;
        }
    }

    for (const DecisionCandidate& entry : memory.decision_candidates) {
        bool matched = false;
        for (const std::string& key : keys) {
            if ((!entry.title.empty() && entry.title.find(key) != std::string::npos) ||
                (!entry.recommended_command.empty() && entry.recommended_command.find(key) != std::string::npos) ||
                std::find(entry.supporting_signals.begin(), entry.supporting_signals.end(), key) != entry.supporting_signals.end()) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (entry.operator_feedback == "helpful") {
            successes += 2;
        } else if (entry.operator_feedback == "not-helpful") {
            failures += 2;
        }

        if (entry.outcome_status == "success") {
            successes += 3;
        } else if (entry.outcome_status == "failed") {
            failures += 3;
        } else if (entry.outcome_status == "partial") {
            ++successes;
            ++failures;
        }
    }
    return {successes, failures};
}

DecisionMetrics compute_decision_metrics(const std::vector<std::string>& all_signals,
                                         const std::vector<std::string>& supporting_signals,
                                         const std::vector<std::pair<std::string, bool>>& checks,
                                         const MemorySnapshot& memory,
                                         const std::vector<std::string>& history_keys,
                                         double fallback_evidence_probability = 0.25) {
    DecisionMetrics metrics;
    metrics.supporting_signals = supporting_signals;

    int passed_checks = 0;
    for (const auto& [name, passed] : checks) {
        metrics.validation_checks.push_back(name + ":" + (passed ? "pass" : "fail"));
        if (passed) {
            ++passed_checks;
        }
    }

    const int total_checks = std::max<int>(1, static_cast<int>(checks.size()));
    metrics.validity_score = static_cast<int>(std::round((static_cast<double>(passed_checks) / total_checks) * 100.0));
    metrics.valid = checks.empty() || passed_checks == static_cast<int>(checks.size());

    if (all_signals.empty()) {
        metrics.evidence_coverage = supporting_signals.empty() ? 0 : 100;
    } else {
        metrics.evidence_coverage = static_cast<int>(
            std::round((static_cast<double>(supporting_signals.size()) / static_cast<double>(all_signals.size())) * 100.0));
    }

    const auto [history_successes, history_failures] = action_history_counts(memory, history_keys);
    const double prior = static_cast<double>(history_successes + 1) /
        static_cast<double>(history_successes + history_failures + 2);
    metrics.prior_success_score = static_cast<int>(std::round(prior * 100.0));

    const double evidence_probability = all_signals.empty()
        ? fallback_evidence_probability
        : clamp_probability(
              0.20 + (0.75 * (static_cast<double>(supporting_signals.size()) / std::max<std::size_t>(1, all_signals.size()))));
    const double validity_probability = clamp_probability(static_cast<double>(metrics.validity_score) / 100.0);

    const double prior_odds = prior <= 0.0 ? 0.0 : prior / std::max(0.0001, 1.0 - prior);
    const double likelihood_ratio = std::max(0.20, 0.45 + (evidence_probability * 1.35) + (validity_probability * 1.10));
    double posterior = prior_odds <= 0.0 ? prior : (prior_odds * likelihood_ratio) / (1.0 + (prior_odds * likelihood_ratio));
    if (!metrics.valid) {
        posterior *= 0.45;
    }
    metrics.probability_likelihood = clamp_probability(posterior);
    metrics.confidence = clamp_probability(
        (0.40 * validity_probability) + (0.35 * evidence_probability) + (0.25 * prior));

    metrics.score_trace.push_back("signals=" + std::to_string(supporting_signals.size()) + "/" + std::to_string(all_signals.size()));
    metrics.score_trace.push_back("validity=" + std::to_string(passed_checks) + "/" + std::to_string(checks.size()));
    metrics.score_trace.push_back("history_success=" + std::to_string(history_successes));
    metrics.score_trace.push_back("history_failure=" + std::to_string(history_failures));
    {
        std::ostringstream posterior_line;
        posterior_line << std::fixed << std::setprecision(3) << "posterior=" << metrics.probability_likelihood;
        metrics.score_trace.push_back(posterior_line.str());
    }
    {
        std::ostringstream confidence_line;
        confidence_line << std::fixed << std::setprecision(3) << "confidence=" << metrics.confidence;
        metrics.score_trace.push_back(confidence_line.str());
    }

    return metrics;
}

std::vector<NormalizedObject> normalize_observation(const ObservationRecord& observation) {
    const UnixEvidenceParser parser;
    return parser.parse(observation);
}

std::vector<DecisionCandidate> build_decisions(const CaseRecord& case_record,
                                               const std::vector<ObservationRecord>& observations,
                                               const std::vector<NormalizedObject>& objects,
                                               const NativeToolRegistry& tools,
                                               MemorySnapshot& memory,
                                               QuerySessionRecord* query_session) {
    std::vector<DecisionCandidate> decisions;
    std::string combined_text = case_record.primary_source + "\n" + case_record.latest_summary;
    for (const ObservationRecord& observation : observations) {
        combined_text += "\n" + observation.raw_content;
    }
    const std::vector<std::string> signals = detect_signals(combined_text, case_record.primary_source);
    const bool has_build_object = std::any_of(objects.begin(), objects.end(), [](const NormalizedObject& object) {
        return object.object_type == "build_log_summary" || object.object_type == "build_issue";
    });
    const bool has_structured_log_object = std::any_of(objects.begin(), objects.end(), [](const NormalizedObject& object) {
        return object.object_type == "ssh_auth_event" || object.object_type == "json_field_summary" ||
            object.object_type == "json_record_stream" || object.object_type == "tool_output_summary" ||
            object.object_type == "nmap_output_summary";
    });

    auto add_decision = [&decisions, &case_record, &signals, &memory](std::string_view slug,
                                                                      std::string title,
                                                                      std::string rationale,
                                                                      std::string command,
                                                                      const std::vector<std::string>& matched_signals,
                                                                      const std::vector<std::pair<std::string, bool>>& checks,
                                                                      const std::vector<std::string>& history_keys,
                                                                      double fallback_evidence_probability = 0.25) {
        const DecisionMetrics metrics = compute_decision_metrics(
            signals, matched_signals, checks, memory, history_keys, fallback_evidence_probability);

        DecisionCandidate candidate;
        candidate.id = make_id("decision", case_record.id + "-" + std::string(slug));
        candidate.case_id = case_record.id;
        candidate.title = std::move(title);
        candidate.rationale = std::move(rationale);
        candidate.recommended_command = std::move(command);
        candidate.valid = metrics.valid;
        candidate.status = metrics.valid ? "recommended" : "conditional";
        candidate.validity_score = metrics.validity_score;
        candidate.evidence_coverage = metrics.evidence_coverage;
        candidate.prior_success_score = metrics.prior_success_score;
        candidate.confidence = metrics.confidence;
        candidate.probability_likelihood = metrics.probability_likelihood;
        candidate.supporting_signals = metrics.supporting_signals;
        candidate.validation_checks = metrics.validation_checks;
        candidate.score_trace = metrics.score_trace;
        candidate.score = clamp_score(static_cast<int>(std::round(
            ((candidate.probability_likelihood * 0.55) + (candidate.confidence * 0.30) +
             ((static_cast<double>(candidate.evidence_coverage) / 100.0) * 0.15)) *
            100.0)));
        if (!candidate.valid) {
            candidate.score = clamp_score(candidate.score - 20);
        }
        decisions.push_back(std::move(candidate));
    };

    const std::filesystem::path source_path(case_record.primary_source);
    if (std::filesystem::exists(source_path) && std::filesystem::is_regular_file(source_path)) {
        add_decision("inspect-log",
                     "Inspect the structured log view",
                     "Use Omni's structured log inspector to refresh normalized evidence objects and highlight extracted fields.",
                     "omnix tool inspect-log -- " + case_record.primary_source,
                     has_structured_log_object
                         ? intersect_signals(signals, {"ssh", "auth", "json", "log", "build", "nmap"})
                         : signals,
                     {
                         {"source_exists", true},
                         {"regular_file", std::filesystem::is_regular_file(source_path)},
                     },
                     {"tool inspect-log", "inspect-log", "analyze " + case_record.id},
                     has_structured_log_object ? 0.72 : 0.58);

        const ToolResolution grep = tools.resolve("grep", memory, false);
        const ToolResolution perl = tools.resolve("perl", memory, false);
        add_decision("regex",
                     "Search for high-signal terms in the source",
                     "Use Omni's regex-search module to highlight errors, warnings, and auth/network terms in the primary evidence.",
                     "omnix tool regex-search -- '(error|warn|fail|critical|auth|ssh|nmap)' " + case_record.primary_source,
                     signals,
                     {
                         {"source_exists", true},
                         {"regex_provider(grep|perl)", grep.found || perl.found},
                     },
                     {"tool regex-search", "regex-search", "grep", "perl"},
                     0.45);

        add_decision("text-pipeline",
                     "Run a safe text pipeline over the source",
                     "Use Omni's built-in grep/sed/awk pipeline wrapper to extract or transform high-signal lines without leaving the analyst flow.",
                     "omnix tool text-pipeline -- " + case_record.primary_source +
                         " --grep '(error|warn|fail|critical|auth|ssh|nmap|build)'",
                     signals,
                     {
                         {"source_exists", true},
                         {"pipeline_provider(grep|sed|awk)", grep.found || tools.resolve("sed", memory, false).found ||
                                                                tools.resolve("awk", memory, false).found},
                     },
                     {"tool text-pipeline", "text-pipeline", "grep", "sed", "awk"},
                     0.52);
    } else if (std::filesystem::exists(source_path) && std::filesystem::is_directory(source_path)) {
        const ToolResolution grep = tools.resolve("grep", memory, false);
        const ToolResolution perl = tools.resolve("perl", memory, false);
        add_decision("deep-grep",
                     "Recursively search the directory for high-signal terms",
                     "Use Omni's deep-grep module to sweep the directory for error, warning, and auth/network terms.",
                     "omnix tool deep-grep -- '(error|warn|fail|critical|auth|ssh|nmap)' " + case_record.primary_source,
                     signals,
                     {
                         {"source_exists", true},
                         {"recursive_search_provider(grep|perl)", grep.found || perl.found},
                     },
                     {"tool deep-grep", "deep-grep", "grep", "perl"},
                     0.50);

        add_decision("inspect-build-dir",
                     "Inspect build readiness for the directory",
                     "Use Omni's build inspector to detect the build system, missing modules, and project readiness.",
                     "omnix tool inspect-build -- " + case_record.primary_source,
                     intersect_signals(signals, {"build", "cmake", "make", "compile"}),
                     {
                         {"source_exists", true},
                         {"directory_source", true},
                     },
                     {"tool inspect-build", "inspect-build", "preflight"},
                     0.55);
    }

    if (std::find(signals.begin(), signals.end(), "nmap") != signals.end() ||
        std::find(signals.begin(), signals.end(), "network") != signals.end()) {
        const ToolResolution nmap = tools.resolve("nmap", memory, false);
        add_decision("nmap",
                     "Inspect the local Nmap capability path",
                     nmap.found
                         ? "A native Nmap provider is already available, so validating it is the fastest safe next step."
                         : "Network-oriented evidence was detected, so checking or preparing the Nmap path is a strong next step.",
                     nmap.found ? "omnix tool nmap -- -V" : "omnix doctor nmap",
                     intersect_signals(signals, {"nmap", "network"}),
                     nmap.found
                         ? std::vector<std::pair<std::string, bool>>{{"native_nmap_available", true}, {"version_probe_ready", true}}
                         : std::vector<std::pair<std::string, bool>>{{"doctor_available", true}},
                     nmap.found ? std::vector<std::string>{"tool nmap", "Build NMAP", "doctor nmap", "nmap"}
                                : std::vector<std::string>{"doctor nmap", "Build NMAP", "nmap"},
                     nmap.found ? 0.70 : 0.55);
    }

    if (std::find(signals.begin(), signals.end(), "ssh") != signals.end() ||
        std::find(signals.begin(), signals.end(), "auth") != signals.end() ||
        std::find(signals.begin(), signals.end(), "access") != signals.end()) {
        const ToolResolution ssh = tools.resolve("ssh", memory, false);
        add_decision("ssh",
                     "Inspect the local SSH client path",
                     "Authentication and access signals were detected, so validating the local SSH client and related evidence is a safe next step.",
                     "omnix tool ssh -- -V",
                     intersect_signals(signals, {"ssh", "auth", "access", "denied", "login"}),
                     {
                         {"ssh_client_available", ssh.found},
                         {"auth_or_access_signal_present", true},
                     },
                     {"tool ssh", "ssh", "auth", "access"},
                     0.50);
    }

    if (std::find(signals.begin(), signals.end(), "host") != signals.end() ||
        std::find(signals.begin(), signals.end(), "auth") != signals.end() ||
        std::find(signals.begin(), signals.end(), "access") != signals.end() ||
        std::find(signals.begin(), signals.end(), "build") != signals.end() ||
        std::find(signals.begin(), signals.end(), "network") != signals.end()) {
        add_decision("inspect-host",
                     "Inspect local host deployment state",
                     "Use Omni's Linux-first host inspector to inventory users, sudoers, mirrors, logs, cron, systemd, initrd, and native tools.",
                     "omnix tool inspect-host -- --linux",
                     intersect_signals(signals, {"host", "auth", "access", "build", "network", "tool"}),
                     {
                         {"host_inspection_module_ready", true},
                     },
                     {"tool inspect-host", "inspect-host", "doctor nmap", "build nmap"},
                     0.58);
    }

    if (std::find(signals.begin(), signals.end(), "build") != signals.end()) {
        std::filesystem::path build_target = source_path;
        if (std::filesystem::is_regular_file(build_target)) {
            build_target = build_target.parent_path();
        }
        const std::string target = build_target.empty() ? "." : build_target.string();
        add_decision("build",
                     "Inspect build readiness for the source context",
                     "Build-oriented signals were detected, so checking the local build path is a safe deterministic next step.",
                     "omnix preflight " + target,
                     intersect_signals(signals, {"build"}),
                     {
                         {"build_target_exists", std::filesystem::exists(build_target.empty() ? std::filesystem::current_path() : build_target)},
                     },
                     {"preflight", "build", "cmake", "make", "compile"},
                     0.48);

        add_decision("inspect-build",
                     "Inspect the build context with Omni's analyzer",
                     "Use the build inspection module to summarize detected build files, missing modules, and readiness.",
                     "omnix tool inspect-build -- " + target,
                     has_build_object
                         ? intersect_signals(signals, {"build", "error", "warning", "cmake", "make", "compile"})
                         : intersect_signals(signals, {"build", "cmake", "make", "compile"}),
                     {
                         {"build_target_exists", std::filesystem::exists(build_target.empty() ? std::filesystem::current_path() : build_target)},
                     },
                     {"tool inspect-build", "inspect-build", "preflight", "build"},
                     has_build_object ? 0.70 : 0.54);
    }

    add_decision("report-case",
                 "Generate a saved analyst report for this case",
                 "Use Omni's report generator to persist a portable analyst summary with evidence, decisions, and case links.",
                 "omnix tool report-case -- " + case_record.id,
                 signals,
                 {
                     {"case_record_available", true},
                     {"report_storage_available", true},
                 },
                 {"tool report-case", "report-case", "case " + case_record.id},
                 0.62);

    if (decisions.empty()) {
        add_decision("case",
                     "Inspect the case details",
                     "No specialized domain signal dominated this case, so reviewing the normalized case state is the best next step.",
                     "omnix case " + case_record.id,
                     {},
                     {{"case_record_available", true}},
                     {"case " + case_record.id, "case_loaded"},
                     0.40);
    }

    if (query_session != nullptr) {
        QueryRuntime query_runtime;
        query_runtime.index_values(*query_session, "case-signals", signals);
        std::vector<std::string> object_types;
        object_types.reserve(objects.size());
        for (const NormalizedObject& object : objects) {
            push_unique(object_types, object.object_type, 12);
        }
        query_runtime.index_values(*query_session, "case-objects", object_types);
        query_runtime.rank_decisions(*query_session, "decision-ranking", decisions, signals);
    } else {
        std::stable_sort(decisions.begin(), decisions.end(), [](const DecisionCandidate& lhs, const DecisionCandidate& rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            if (lhs.probability_likelihood != rhs.probability_likelihood) {
                return lhs.probability_likelihood > rhs.probability_likelihood;
            }
            return lhs.confidence > rhs.confidence;
        });
    }
    return decisions;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out << '\n';
        }
        out << lines[index];
    }
    return out.str();
}

ObservationRecord capture_observation(std::string_view reference) {
    const std::filesystem::path path(reference);
    ObservationRecord observation;
    observation.source_ref = std::string(reference);
    observation.collected_at = now_timestamp();

    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) {
        observation.source_kind = "file";
        observation.raw_content = read_text_file(path);
        observation.summary = "Ingested file `" + path.filename().string() + "` with " +
            std::to_string(line_count(observation.raw_content)) + " lines.";
    } else if (std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec)) {
        observation.source_kind = "directory";
        std::ostringstream listing;
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            listing << entry.path().filename().string() << '\n';
            if (++count >= 24 || ec) {
                break;
            }
        }
        observation.raw_content = listing.str();
        observation.summary = "Captured directory snapshot for `" + path.string() + "`.";
    } else {
        observation.source_kind = "command";
        const CommandCapture command = run_command_capture(reference);
        observation.raw_content = command.output;
        observation.summary = "Captured command output from `" + std::string(reference) + "` (exit=" +
            std::to_string(command.exit_code) + ").";
    }

    observation.content_hash = std::to_string(std::hash<std::string>{}(observation.raw_content));
    return observation;
}

CaseRecord create_case(const RequestProfile& profile,
                       std::string_view source_reference,
                       const ObservationRecord& observation) {
    CaseRecord case_record;
    case_record.id = make_id("case", std::string(source_reference) + "|" + observation.content_hash);
    case_record.title = observation.source_kind == "command"
        ? "Command capture: " + std::string(source_reference)
        : "Source capture: " + std::filesystem::path(std::string(source_reference)).filename().string();
    case_record.primary_source = std::string(source_reference);
    case_record.status = "ingested";
    case_record.created_at = observation.collected_at;
    case_record.updated_at = observation.collected_at;
    case_record.permission = permission_for(profile);
    case_record.latest_summary = observation.summary;
    case_record.observation_ids.push_back(observation.id);
    return case_record;
}

void attach_case_to_report(const MemorySnapshot& memory,
                           const std::vector<CaseCluster>& clusters,
                           const CaseRecord& case_record,
                           ProcessingReport& report) {
    report.permission_context = case_record.permission;
    report.case_record = case_record;
    report.observations = observations_for_case(memory, case_record.id);
    report.normalized_objects = objects_for_case(memory, case_record.id);
    report.evidence_links = evidence_for_case(memory, case_record.id);
    report.analyst_comments = comments_for_case(memory, case_record.id);
    report.decision_candidates = decisions_for_case(memory, case_record.id);
    report.case_links = links_for_case(memory, case_record.id);
    report.case_clusters = clusters_for_case(clusters, case_record.id);
}

std::string analysis_summary(const CaseRecord& case_record,
                             const std::vector<NormalizedObject>& objects,
                             const std::vector<DecisionCandidate>& decisions,
                             const std::vector<CaseCluster>& clusters) {
    std::ostringstream out;
    out << case_record.title << "\n";
    out << "Status: " << case_record.status << "\n";
    out << "Primary source: " << case_record.primary_source << "\n";
    out << "Normalized objects: " << objects.size() << "\n";
    if (!objects.empty()) {
        out << "Top object: " << objects.front().object_type << " -> " << objects.front().summary << "\n";
    }
    if (!decisions.empty()) {
        out << "Top decision: " << decisions.front().title;
        if (!decisions.front().recommended_command.empty()) {
            out << " => " << decisions.front().recommended_command;
        }
        out << " [likelihood=" << std::fixed << std::setprecision(2) << decisions.front().probability_likelihood
            << ", confidence=" << decisions.front().confidence << "]";
    }
    const std::vector<CaseCluster> related_clusters = clusters_for_case(clusters, case_record.id);
    if (!related_clusters.empty()) {
        out << "\nClusters: " << related_clusters.size() << " related cluster(s), strongest score="
            << related_clusters.front().correlation_score;
    }
    return out.str();
}

}  // namespace

void AnalystFlowInterpreter::run(const RequestProfile& profile,
                                 std::string_view target,
                                 MemorySnapshot& memory,
                                 ProcessingReport& report,
                                 const NativeToolRegistry& tools,
                                 const MemoryStore& memory_store) const {
    const std::string effective_target = trim(target.empty() ? profile.analyst_reference : target);
    const std::string inferred_mode = profile.analyst_mode == "inspect"
        ? (effective_target == "list" ? "list" : starts_with(effective_target, "search ") ? "search" : "inspect")
        : profile.analyst_mode;
    const std::string search_query = !profile.analyst_query.empty()
        ? profile.analyst_query
        : (starts_with(effective_target, "search ") ? trim(effective_target.substr(7)) : std::string{});
    const PermissionContext permission = permission_for(profile);
    report.permission_context = permission;
    report.references = analyst_references(profile.resolved_intent, permission);
    report.feedback_loop = relevant_history(memory, search_query.empty() ? effective_target : search_query);
    if (memory.case_links.empty() && memory.case_records.size() > 1) {
        memory.case_links = derive_case_links(memory);
    }
    auto current_clusters = [&memory]() {
        return derive_case_clusters(memory);
    };

    auto persist_case_state = [&](const CaseRecord& case_record) {
        memory_store.remember_case_record(memory, case_record);
        memory.case_links = derive_case_links(memory);
        report.memory_writes.push_back(memory.paths.cases_path.string());
        report.storage_writes.push_back("x.Store(case -> " + case_record.id + ")");
    };

    auto persist_analysis = [&](const CaseRecord& case_record,
                                const std::vector<NormalizedObject>& objects,
                                const std::vector<EvidenceLink>& links,
                                const std::vector<AnalystComment>& comments,
                                const std::vector<DecisionCandidate>& decisions) {
        for (const NormalizedObject& entry : objects) {
            memory_store.remember_normalized_object(memory, entry);
        }
        for (const EvidenceLink& entry : links) {
            memory_store.remember_evidence_link(memory, entry);
        }
        for (const AnalystComment& entry : comments) {
            memory_store.remember_analyst_comment(memory, entry);
        }
        for (const DecisionCandidate& entry : decisions) {
            memory_store.remember_decision_candidate(memory, entry);
        }
        memory_store.remember_case_record(memory, case_record);
        memory.case_links = derive_case_links(memory);
        report.memory_writes.push_back(memory.paths.cases_path.string());
    };

    auto ingest_source = [&]() -> std::optional<CaseRecord> {
        if (effective_target.empty()) {
            report.answer_status = "ingest_target_missing";
            report.answer_explanation = "No ingest source was provided.";
            report.next_action = "Provide a file path, directory path, or safe local command to ingest.";
            return std::nullopt;
        }

        ObservationRecord observation = capture_observation(effective_target);
        observation.id = make_id("observation", effective_target + "|" + observation.content_hash);

        CaseRecord case_record = create_case(profile, effective_target, observation);
        case_record.created_by_run_id = report.tze_run_id;
        observation.case_id = case_record.id;

        memory_store.remember_observation(memory, observation);
        report.memory_writes.push_back(memory.paths.cases_path.string());
        report.storage_writes.push_back("x.Store(observation -> " + observation.id + ")");

        const std::vector<NormalizedObject> initial_objects = normalize_observation(observation);
        std::vector<EvidenceLink> initial_links;
        for (const NormalizedObject& object : initial_objects) {
            memory_store.remember_normalized_object(memory, object);
            initial_links.push_back({
                make_id("evidence", observation.id + "|" + object.id),
                case_record.id,
                observation.id,
                object.id,
                "derived_from",
                "Deterministic normalization from the ingested observation.",
            });
        }
        for (const EvidenceLink& link : initial_links) {
            memory_store.remember_evidence_link(memory, link);
        }

        case_record.object_ids.clear();
        for (const NormalizedObject& object : initial_objects) {
            case_record.object_ids.push_back(object.id);
        }
        for (const EvidenceLink& link : initial_links) {
            case_record.evidence_link_ids.push_back(link.id);
        }
        persist_case_state(case_record);

        report.resolved_project = case_record.id;
        report.answer_status = "ingested";
        report.answer_explanation = "Created case `" + case_record.id + "` from " + observation.source_kind +
            " source `" + effective_target + "`.";
        report.next_action = "Run `omnix analyze " + case_record.id + "` to normalize evidence and generate deterministic findings.";
        attach_case_to_report(memory, current_clusters(), case_record, report);
        return case_record;
    };

    auto analyze_case = [&](CaseRecord case_record, bool include_decisions) {
        const std::vector<ObservationRecord> observations = observations_for_case(memory, case_record.id);
        std::vector<NormalizedObject> objects;
        std::vector<EvidenceLink> links;
        for (const ObservationRecord& observation : observations) {
            const std::vector<NormalizedObject> derived = normalize_observation(observation);
            for (const NormalizedObject& object : derived) {
                objects.push_back(object);
                links.push_back({
                    make_id("evidence", observation.id + "|" + object.id),
                    case_record.id,
                    observation.id,
                    object.id,
                    "supports",
                    "Deterministic analyst normalization linked this object back to the raw observation.",
                });
            }
        }

        std::vector<AnalystComment> comments;
        comments.push_back({
            make_id("comment", case_record.id + "-analysis"),
            case_record.id,
            "omnix-system",
            "Analyzed the case deterministically and refreshed normalized objects from the latest observations.",
            now_timestamp(),
        });

        std::vector<DecisionCandidate> decisions;
        case_record.object_ids.clear();
        for (const NormalizedObject& object : objects) {
            case_record.object_ids.push_back(object.id);
        }
        case_record.evidence_link_ids.clear();
        for (const EvidenceLink& link : links) {
            case_record.evidence_link_ids.push_back(link.id);
        }
        case_record.comment_ids.clear();
        for (const AnalystComment& comment : comments) {
            case_record.comment_ids.push_back(comment.id);
        }

        if (include_decisions) {
            decisions = build_decisions(case_record, observations, objects, tools, memory,
                                        report.query_session ? &(*report.query_session) : nullptr);
            case_record.decision_ids.clear();
            for (const DecisionCandidate& decision : decisions) {
                case_record.decision_ids.push_back(decision.id);
            }
        }

        case_record.status = include_decisions ? "decided" : "analyzed";
        case_record.updated_at = now_timestamp();
        if (include_decisions) {
            case_record.decided_by_run_id = report.tze_run_id;
        } else {
            case_record.analyzed_by_run_id = report.tze_run_id;
        }
        persist_analysis(case_record, objects, links, comments, decisions);
        case_record.latest_summary = analysis_summary(case_record, objects, decisions, current_clusters());
        memory_store.remember_case_record(memory, case_record);
        report.memory_writes.push_back(memory.paths.cases_path.string());

        report.resolved_project = case_record.id;
        report.answer_status = include_decisions ? "decided" : "analyzed";
        report.answer_explanation = case_record.latest_summary;
        report.next_action = include_decisions
            ? (decisions.empty() ? "Run `omnix case " + case_record.id + "` to inspect the case." : "Run `" + decisions.front().recommended_command + "` or inspect `omnix case " + case_record.id + "`.")
            : "Run `omnix decide " + case_record.id + "` to rank safe local next actions.";
        report.storage_writes.push_back("x.Store(case.summary -> " + case_record.id + ")");
        attach_case_to_report(memory, current_clusters(), case_record, report);
    };

    if (profile.resolved_intent == RequestIntent::IngestData) {
        ingest_source();
        return;
    }

    if (profile.resolved_intent == RequestIntent::InspectCase && inferred_mode == "list") {
        report.case_clusters = current_clusters();
        report.answer_status = "case_listed";
        report.answer_explanation = "Loaded the current Omni case catalogue with " +
            std::to_string(report.case_clusters.size()) + " derived cluster(s).";
        report.case_matches = sorted_cases(memory);
        report.case_links = memory.case_links;
        report.next_action = report.case_matches.empty()
            ? "Use `omnix ingest <path|command>` to create the first case."
            : "Run `omnix case <id>` to inspect a case or `omnix case search <term>` to find related work.";
        return;
    }

    if (profile.resolved_intent == RequestIntent::InspectCase && inferred_mode == "search") {
        report.case_matches = search_cases(memory, search_query);
        const std::vector<CaseCluster> clusters = current_clusters();
        for (const CaseRecord& match : report.case_matches) {
            const std::vector<CaseLink> linked = links_for_case(memory, match.id);
            report.case_links.insert(report.case_links.end(), linked.begin(), linked.end());
        }
        std::stable_sort(report.case_links.begin(), report.case_links.end(), [](const CaseLink& lhs, const CaseLink& rhs) {
            return lhs.strength > rhs.strength;
        });
        std::vector<CaseLink> deduped;
        for (const CaseLink& link : report.case_links) {
            const auto existing = std::find_if(deduped.begin(), deduped.end(), [&link](const CaseLink& candidate) {
                return candidate.id == link.id;
            });
            if (existing == deduped.end()) {
                deduped.push_back(link);
            }
        }
        report.case_links = std::move(deduped);
        report.case_clusters = clusters_for_cases(clusters, report.case_matches);
        report.answer_status = report.case_matches.empty() ? "case_search_empty" : "case_search_results";
        report.answer_explanation = report.case_matches.empty()
            ? "No cases matched `" + search_query + "`."
            : "Found " + std::to_string(report.case_matches.size()) + " case(s) matching `" + search_query +
                "` across " + std::to_string(report.case_clusters.size()) + " related cluster(s).";
        report.next_action = report.case_matches.empty()
            ? "Try `omnix case list` to browse all cases or ingest a new source."
            : "Run `omnix case " + report.case_matches.front().id + "` to inspect the top match.";
        return;
    }

    CaseRecord* existing_case = find_case(memory, effective_target);
    if (existing_case == nullptr && (profile.resolved_intent == RequestIntent::AnalyzeCase ||
                                     profile.resolved_intent == RequestIntent::DecideAction)) {
        const std::optional<CaseRecord> ingested = ingest_source();
        if (!ingested.has_value()) {
            return;
        }
        existing_case = find_case(memory, ingested->id);
    }

    if (existing_case == nullptr) {
        report.answer_status = "case_not_found";
        report.answer_explanation = "No case or source matched `" + effective_target + "`.";
        report.next_action = "Use `omnix ingest <path|command>` first, then rerun the analyst command.";
        return;
    }

    if (profile.resolved_intent == RequestIntent::AnalyzeCase) {
        analyze_case(*existing_case, false);
        return;
    }

    if (profile.resolved_intent == RequestIntent::DecideAction) {
        analyze_case(*existing_case, true);
        return;
    }

    if (profile.resolved_intent == RequestIntent::InspectCase) {
        report.resolved_project = existing_case->id;
        report.answer_status = "case_loaded";
        report.answer_explanation = existing_case->latest_summary.empty()
            ? "Loaded case `" + existing_case->id + "`."
            : existing_case->latest_summary;
        report.next_action = "Run `omnix analyze " + existing_case->id + "` or `omnix decide " + existing_case->id + "` for the next analyst step.";
        attach_case_to_report(memory, current_clusters(), *existing_case, report);
    }
}

}  // namespace tze
