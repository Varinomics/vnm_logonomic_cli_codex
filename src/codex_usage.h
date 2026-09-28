#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/legacy_state_import.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace codex {

// One GiB is the adopted host candidate bound. Eight GiB accommodates retained
// two generations of per-file cache and snapshots, merge inputs/output, and
// an imported replacement. Limits count the whole owned tree; history is never
// evicted to make space. Sorting holds at most one run of encoded records in
// memory. The runtime always uses these defaults; smaller limits let pure
// filesystem fixtures exercise the same production admission checks.
struct Usage_scan_limits
{
    std::uint64_t disk_bytes = 8ull * 1024 * 1024 * 1024;
    std::uint64_t filesystem_entries = 1024 * 1024;
    std::uint64_t persisted_lines = 8ull * 1024 * 1024;
    std::uint64_t candidate_bytes = 1024ull * 1024 * 1024;
    std::uint64_t candidate_records = 1024 * 1024;
    std::uint64_t sort_run_bytes = 64ull * 1024 * 1024;
};

// Reports a usage scan's progress. A frame names the running stage and counts
// the items it has handled. A stage reports when it begins; while it works, a
// frame follows the previous one once an interval has passed that starts at
// one second and doubles every 256 frames up to a quarter of the silence the
// host tolerates. So a scan that keeps working reports within that silence for
// days before the core protocol's bound of 4,096 frames per request is spent;
// the scan never sends more, and one still working then falls silent. That
// horizon, about six days under a 15-minute silence, is the protocol's.
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
    std::chrono::milliseconds             m_longest_interval;
    std::string                           m_note;
    std::uint64_t                         m_completed = 0;
    std::uint64_t                         m_frames    = 0;
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
    std::string* error, const Usage_scan_limits& limits = {});
void handle_usage(logonomic::cli_addon::Invocation_context& context);
logonomic::cli_addon::Legacy_import_result import_usage(
    const logonomic::cli_addon::Import_record_request& request);

} // namespace codex
