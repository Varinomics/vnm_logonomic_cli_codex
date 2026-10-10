#include "codex_usage_temporal.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/sha256.h>

#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

std::string accounting_digest(const addon::Json_value& accounting)
{
    return addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(accounting)));
}

} // namespace

bool valid_usage_instant(std::string_view instant)
{
    if (instant.size() > addon::k_max_usage_temporal_bytes) return false;
    addon::Usage_temporal evidence;
    evidence.parent = {"native-proof", 1, "native-proof", std::nullopt};
    evidence.portions.push_back({"instant", std::string(instant), {}, {}, 1});
    return addon::decode_usage_temporal(addon::encode_usage_temporal(evidence), nullptr).has_value();
}

addon::Json_value encode_temporal_observation(const Temporal_observation& observation,
    const addon::Json_value& accounting)
{
    if (observation.opaque) return *observation.opaque;
    auto carrier = addon::Json_value::object();
    carrier.set("version", addon::Json_value(1));
    carrier.set("accounting_sha256_hex", addon::Json_value(accounting_digest(accounting)));
    carrier.set("instant", observation.instant ? addon::Json_value(*observation.instant) : addon::Json_value{});
    carrier.set("reason", addon::Json_value(observation.reason));
    return carrier;
}

Temporal_observation decode_temporal_observation(const addon::Json_value* carrier,
    const addon::Json_value& accounting)
{
    Temporal_observation observation;
    if (!carrier) return observation;
    observation.reason = "unsupported";
    observation.opaque = *carrier;
    if (!carrier->is_object() || carrier->as_object().size() != 4 ||
        addon::serialize_json(*carrier).size() > addon::k_max_usage_temporal_bytes) return observation;
    const auto* version = carrier->find("version");
    const auto* digest = carrier->find("accounting_sha256_hex");
    const auto* instant = carrier->find("instant");
    const auto* reason = carrier->find("reason");
    if (!version || !version->is_integer() || version->as_integer() != 1 ||
        !digest || !digest->is_string() || digest->as_string() != accounting_digest(accounting) ||
        !instant || !reason || !reason->is_string()) return observation;
    if (instant->is_string()) {
        if (reason->as_string() != "complete" || !valid_usage_instant(instant->as_string())) return observation;
        observation.instant = instant->as_string();
    }
    else {
        if (!instant->is_null() || (reason->as_string() != "missing-evidence" &&
            reason->as_string() != "capacity" && reason->as_string() != "unsupported")) return observation;
    }
    observation.reason = reason->as_string();
    observation.opaque.reset();
    return observation;
}

void reconcile_temporal_observations(Temporal_observation& retained,
    const Temporal_observation& observed)
{
    if (retained.opaque || observed.opaque) {
        retained.instant.reset();
        retained.reason = "unsupported";
        return;
    }
    if (retained.instant && observed.instant && retained.instant != observed.instant) {
        retained.instant.reset();
        retained.reason = "unsupported";
        return;
    }
    if (retained.reason == "unsupported" || observed.reason == "unsupported") {
        retained.instant.reset();
        retained.reason = "unsupported";
        return;
    }
    if (!retained.instant && observed.instant) retained = observed;
}

bool valid_temporal_record(const addon::Usage_record& record)
{
    if (!record.temporal || !addon::decode_usage_temporal(
        addon::encode_usage_temporal(*record.temporal), nullptr)) return false;
    const auto encoded = addon::encode_usage_record(record, 3);
    return addon::decode_usage_record(encoded, nullptr, 3).has_value() &&
        addon::parse_json(addon::serialize_json(encoded), {}, nullptr).has_value();
}

void apply_temporal_observation(addon::Usage_record& record,
    const Temporal_observation& observation, const std::string& snapshot_id,
    const std::optional<std::string>& account)
{
    if (record.capabilities.find(addon::k_usage_temporal_capability_id)) return;
    addon::Usage_temporal evidence;
    evidence.parent = {record.record_id, record.total_tokens, snapshot_id, account};
    if (observation.instant) {
        if (record.total_tokens != 0) {
            evidence.portions.push_back({"instant", *observation.instant, {}, {}, record.total_tokens});
        }
    }
    else {
        evidence.unavailable_tokens = record.total_tokens;
        evidence.coverage_reason = observation.reason;
    }
    record.temporal = std::move(evidence);
    if (!valid_temporal_record(record)) record.temporal.reset();
}

} // namespace codex
