#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/legacy_state_import.h>

#include <filesystem>
#include <functional>

namespace codex {

// One GiB is the adopted host candidate bound. Eight GiB accommodates retained
// two generations of per-file cache and snapshots, merge inputs/output, and
// an imported replacement. Limits count the whole owned tree; history is never
// evicted to make space. The runtime always uses these defaults; smaller limits let pure
// filesystem fixtures exercise the same production admission checks.
struct Usage_scan_limits
{
    std::uint64_t disk_bytes = 8ull * 1024 * 1024 * 1024;
    std::uint64_t filesystem_entries = 1024 * 1024;
    std::uint64_t persisted_lines = 8ull * 1024 * 1024;
    std::uint64_t candidate_bytes = 1024ull * 1024 * 1024;
    std::uint64_t candidate_records = 1024 * 1024;
};

std::optional<logonomic::cli_addon::Usage_scan_page> scan_usage(
    const logonomic::cli_addon::Usage_scan_request& request,
    const std::filesystem::path& vendor_profile,
    const std::filesystem::path& storage,
    const std::function<bool()>& cancelled,
    std::string* error, const Usage_scan_limits& limits = {});
void handle_usage(logonomic::cli_addon::Invocation_context& context);
logonomic::cli_addon::Legacy_import_result import_usage(
    const logonomic::cli_addon::Import_record_request& request);

} // namespace codex
