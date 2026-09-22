#pragma once

#include <logonomic_cli_addon/usage_scan.h>

#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

namespace codex {

struct Rollout_usage_record
{
    logonomic::cli_addon::Usage_record usage;
    std::string recorded_cwd;
};

std::optional<std::string> repository_for_cwd(std::string_view cwd);
void refresh_repository(Rollout_usage_record* record);
// Attributes the record to `repository`, already resolved for its recorded cwd.
void refresh_repository(Rollout_usage_record* record, const std::optional<std::string>& repository);
bool read_rollout_usage(std::istream& input, const std::filesystem::path& path,
    const std::function<bool(const Rollout_usage_record&)>& emit,
    const std::function<bool()>& cancelled, std::string* error);

} // namespace codex
