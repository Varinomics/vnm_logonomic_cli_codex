#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_incremental.h>
#include <logonomic_cli_addon/usage_session.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace codex {

// A scan's memory does not grow with the history: records and rollout
// metadata are sorted on disk, holding at most one run of lines in memory, and
// are reconciled as key-ordered streams; a parsed rollout's records wait on
// disk. Its storage grows with the history it holds, and the package sets it
// no ceiling: running out of disk fails the scan without publishing it. The
// runtime always uses the default run; a smaller one lets pure filesystem
// fixtures sort through merged runs.
struct Usage_scan_limits
{
    std::uint64_t sort_run_bytes = 64ull * 1024 * 1024;
};

enum class Usage_scan_authority
{
    STANDARD,
    REPOSITORY_ATTRIBUTION
};

// Reports each stage immediately and coalesces working-item updates to one
// frame per second, or a quarter of the host's tolerated silence when shorter.
// Progress continues for as long as the scan works.
class Usage_scan_progress final
{
public:
    using Sink  = std::function<void(std::uint64_t completed, const std::string& note)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    Usage_scan_progress(
        Sink                      sink,
        std::chrono::milliseconds tolerated_silence,
        Clock                     clock = [] { return std::chrono::steady_clock::now(); });

    // Starts a stage: its item count restarts at zero and a frame reports it.
    void begin(std::string note);
    void advance() { ++m_completed; }
    // Reports the running stage once the interval since the last frame passed.
    void poll();

private:
    void emit();

    Sink                                  m_sink;
    Clock                                 m_clock;
    std::chrono::milliseconds             m_interval;
    std::string                           m_note;
    std::uint64_t                         m_completed = 0;
    std::chrono::steady_clock::time_point m_last_frame;
};

// Reports through `progress` only while it works: waiting for another scan of
// this package to finish sends nothing, so that scan's own frames alone show
// the host whether it still works.
std::optional<logonomic::cli_addon::Usage_scan_page> scan_usage(
    const logonomic::cli_addon::Usage_scan_request& request,
    const std::filesystem::path& vendor_profile,
    const std::filesystem::path& storage,
    const std::function<bool()>& cancelled,
    Usage_scan_progress& progress,
    std::string* error, const Usage_scan_limits& limits = {},
    Usage_scan_authority authority = Usage_scan_authority::STANDARD);
void handle_usage(logonomic::cli_addon::Invocation_context& context);
std::optional<logonomic::cli_addon::Usage_incremental_result> scan_usage_incremental(
    const logonomic::cli_addon::Usage_scan_request& request,
    const std::filesystem::path& vendor_profile,
    const std::filesystem::path& storage,
    const std::function<bool()>& cancelled,
    Usage_scan_progress& progress,
    std::string* error, const Usage_scan_limits& limits = {},
    Usage_scan_authority authority = Usage_scan_authority::STANDARD);
void handle_usage_incremental(logonomic::cli_addon::Invocation_context& context);
void handle_usage_repository_attribution(logonomic::cli_addon::Invocation_context& context);
void handle_usage_session(logonomic::cli_addon::Invocation_context& context);
std::optional<logonomic::cli_addon::Usage_session_fact> describe_usage_session(
    const logonomic::cli_addon::Usage_session_identity& identity);

} // namespace codex
