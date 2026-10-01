#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <cstddef>
#include <optional>
#include <functional>
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

struct Probe_output
{
    std::string output;
    std::string errors;
};

struct Probe_exchange
{
    std::string initial_input;
    std::function<std::optional<std::string>(std::string_view)> on_output;
    std::function<void()> tick;
    bool complete = false;
};

std::optional<Probe_output> run_probe(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& environment,
    std::string_view working_directory, int timeout_ms,
    logonomic::cli_addon::Invocation_context& context, Probe_exchange* exchange = nullptr);

bool supported_session_arguments(const std::vector<std::string>& argv, bool resume);

std::optional<std::vector<std::string>> resolve_probe_launcher(
    const Launcher_command& command,
    const std::vector<std::string>& operation_arguments,
    const logonomic::cli_addon::Json_value& effective_environment,
    std::string_view working_directory,
    bool direct_batch);

std::optional<Launcher_command> recognize_launcher(std::string_view script);
std::optional<std::string> codex_profile_environment_text(std::string_view profile_path);
std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request);
std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request,
    Codex_profile_discovery_stats* out_profile_stats);

// Literal argv follows the host's direct-launch grammar; it never evaluates a
// shell expression. Recognition is deliberately separate from executable proof.
std::vector<std::string> literal_launcher_argv(std::string_view script);

bool version_is_at_least(std::string_view version, int major, int minor, int patch);
std::string parse_version_output(std::string_view output, std::string_view errors);

// Probes only a native image or a verified stock npm installation. Delegated
// probes execute the selected shim, preserving its behavior and version evidence.
std::optional<Verified_launch> verified_launch(
    const logonomic::cli_addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const logonomic::cli_addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    bool                            delegated,
    logonomic::cli_addon::Invocation_context&     context);

} // namespace codex
