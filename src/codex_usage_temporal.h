#pragma once

#include <logonomic_cli_addon/usage_scan.h>

#include <optional>
#include <string>
#include <string_view>

namespace codex {

struct Temporal_observation
{
    std::optional<std::string> instant;
    std::string reason = "missing-evidence";
    std::optional<logonomic::cli_addon::Json_value> opaque;
};

bool valid_usage_instant(std::string_view instant);
logonomic::cli_addon::Json_value encode_temporal_observation(
    const Temporal_observation& observation,
    const logonomic::cli_addon::Json_value& accounting);
Temporal_observation decode_temporal_observation(
    const logonomic::cli_addon::Json_value* carrier,
    const logonomic::cli_addon::Json_value& accounting);
void reconcile_temporal_observations(Temporal_observation& retained,
    const Temporal_observation& observed);
bool valid_temporal_record(const logonomic::cli_addon::Usage_record& record);
void apply_temporal_observation(logonomic::cli_addon::Usage_record& record,
    const Temporal_observation& observation, const std::string& snapshot_id,
    const std::optional<std::string>& account);

} // namespace codex
