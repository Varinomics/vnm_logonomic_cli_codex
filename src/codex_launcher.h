#pragma once

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codex {

inline constexpr std::size_t k_max_codex_profile_scan_entries = 256;

struct Codex_profile_discovery_stats
{
    std::size_t entries_examined = 0;
    std::size_t profiles_retained = 0;
};

struct Launcher_command
{
    std::string                                      program;
    std::vector<std::string>                         prefix_arguments;
    std::vector<std::pair<std::string, std::string>> environment;
};

struct Verified_launch
{
    std::vector<std::string>                         argv;
    std::string                                      version;
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<std::string>                         files;
    std::size_t                                     prefix_size = 0;
};

std::optional<logonomic::cli_addon::Probe_output> run_probe(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& environment,
    std::string_view working_directory, int timeout_ms,
    logonomic::cli_addon::Invocation_context& context, logonomic::cli_addon::Probe_exchange* exchange = nullptr);

std::optional<logonomic::cli_addon::Probe_output> probe_cli_options(
    const std::vector<std::string>& argv, const logonomic::cli_addon::Environment& environment,
    std::string_view working_directory, logonomic::cli_addon::Invocation_context& context,
    bool* out_timed_out = nullptr);

bool supported_session_arguments(const std::vector<std::string>& argv, bool resume,
    std::string* approval_policy = nullptr);

std::optional<std::vector<std::string>> resolve_probe_launcher(
    const Launcher_command& command,
    const std::vector<std::string>& operation_arguments,
    const logonomic::cli_addon::Json_value& effective_environment,
    std::string_view working_directory,
    bool direct_batch);

std::optional<Launcher_command> recognize_launcher(std::string_view script, std::string_view runner);
std::optional<std::string> codex_profile_environment_text(std::string_view profile_path);
std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request);
std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request,
    Codex_profile_discovery_stats* out_profile_stats);

bool version_is_at_least(std::string_view version, int major, int minor, int patch);
std::string parse_version_output(std::string_view output, std::string_view errors);

// Probes only a native image or a verified Codex npm installation. Delegated
// probes execute the selected shim, preserving its behavior and version evidence.
std::optional<Verified_launch> verified_launch(
    const logonomic::cli_addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const logonomic::cli_addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    bool                            delegated,
    logonomic::cli_addon::Invocation_context&     context,
    logonomic::cli_addon::Contract_failure*       out_failure = nullptr);

// Each verified launch file's path, size and modification time. Results
// remembered for an executable chain are invalidated by a change in this
// metadata; the files are not read, because the native Codex image alone is
// hundreds of megabytes. The limit of that choice: contents replaced at the
// same path with equal size and modification time give equal evidence.
// Absent when the list is empty or a file cannot be examined.
std::optional<logonomic::cli_addon::Json_value> file_evidence(const std::vector<std::string>& files);

} // namespace codex
