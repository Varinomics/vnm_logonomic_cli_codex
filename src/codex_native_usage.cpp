#include "codex_native_usage.h"
#include "codex_accounts.h"
#include <logonomic_cli_addon/sha256.h>

#include <logonomic_cli_addon/package_source.h>
#include <charconv>
#include <cmath>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value missing;
    const auto* value = object.find(name);
    return value ? *value : missing;
}

std::string text(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    return value.is_string() ? value.as_string() : std::string{};
}

std::optional<std::int64_t> integer(const addon::Json_value& value)
{
    if (value.is_string()) {
        std::int64_t number = 0;
        const auto& bytes = value.as_string();
        const auto parsed = std::from_chars(bytes.data(), bytes.data() + bytes.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == bytes.data() + bytes.size() && !bytes.empty()) return number;
    }
    if (value.is_number()) {
        const auto number = value.as_number();
        if (std::isfinite(number) && std::floor(number) == number && std::abs(number) <= 9007199254740991.0) {
            return static_cast<std::int64_t>(number);
        }
    }
    return std::nullopt;
}

addon::Usage_amount unknown_amount()
{
    return {true, 0, addon::Usage_field_coverage{0, 1, false, addon::Usage_support::UNKNOWN}};
}

std::optional<addon::Usage_amount> native_amount(const addon::Json_value& value)
{
    if (!value.is_object()) return std::nullopt;
    addon::Usage_field_coverage coverage;
    const auto covered = integer(member(value, "covered_records"));
    const auto total = integer(member(value, "total_records"));
    const auto support = text(value, "support");
    if (!covered || !total || *covered < 0 || *total < *covered) return std::nullopt;
    if (support == "supported") coverage.support = addon::Usage_support::SUPPORTED;
    else if (support == "unsupported") coverage.support = addon::Usage_support::UNSUPPORTED;
    else if (support != "unknown") return std::nullopt;
    coverage.covered_records = static_cast<std::uint64_t>(*covered);
    coverage.total_records = static_cast<std::uint64_t>(*total);
    const auto& overflow = member(value, "overflow");
    if (!overflow.is_null() && !overflow.is_boolean()) return std::nullopt;
    coverage.overflow = overflow.is_boolean() && overflow.as_boolean();
    addon::Usage_amount amount;
    amount.applicable = coverage.support != addon::Usage_support::UNSUPPORTED;
    amount.coverage = coverage;
    if (amount.applicable && !coverage.overflow) {
        const auto count = integer(member(value, "known_tokens"));
        if (!count || *count < 0) return std::nullopt;
        amount.value = static_cast<std::uint64_t>(*count);
    }
    return amount;
}

} // namespace

std::optional<Native_cache_file> convert_native_cache_file(const addon::Json_value& wrapper,
    const std::function<bool(const Rollout_usage_record&)>& emit, std::string* error)
{
    const auto fail = [&]() -> std::optional<Native_cache_file> { *error = "native_usage_cache_invalid"; return std::nullopt; };
    Native_cache_file file;
    file.path = text(wrapper, "path");
    const auto size = integer(member(wrapper, "size"));
    const auto modified = integer(member(wrapper, "modified_msecs"));
    const auto& records = member(wrapper, "records");
    if (!addon::path_from_utf8(file.path).is_absolute() || !size || *size < 0 || !modified || !records.is_array()) return fail();
    file.size = static_cast<std::uint64_t>(*size);
    file.modified_msecs = *modified;
    const auto& capabilities = member(wrapper, "capabilities");
    const auto& marker = member(capabilities, "token_breakdown_cache");
    // These are native parser revisions, not add-on capability versions. A
    // stale native marker must not bless cached category evidence as current.
    const bool trusted_breakdown = integer(member(marker, "version")) == 2 && integer(member(marker, "parser_revision")) == 2;
    const auto& repository_marker = member(capabilities, "repository_usage_cache");
    file.parser_current = trusted_breakdown && integer(member(repository_marker, "version")) == 1 &&
        integer(member(repository_marker, "parser_revision")) == 1;
    for (const auto& native : records.as_array()) {
        Rollout_usage_record record;
        record.usage.record_id = text(native, "session_id");
        record.usage.completed_date = text(native, "completed_date");
        const auto count = integer(member(native, "total_tokens"));
        if (!native.is_object() || !count || *count < 0) return fail();
        record.usage.total_tokens = static_cast<std::uint64_t>(*count);
        const auto source = text(native, "source_path");
        if (source.empty() || profile_account_key(source) != profile_account_key(file.path)) return fail();
        record.usage.source_path = source;
        const auto rollout = text(native, "rollout_id");
        if (!rollout.empty()) record.usage.session_key = rollout;
        const auto& identity = member(member(native, "capabilities"), "usage-record-identity");
        if (!identity.is_null()) {
            const auto public_id = text(identity, "record_id");
            if (!identity.is_object() || integer(member(identity, "version")) != 1 || public_id.empty() ||
                record.usage.record_id != addon::to_hex(reinterpret_cast<const std::uint8_t*>(public_id.data()), public_id.size())) return fail();
            record.usage.record_id = public_id;
            const auto session_key = text(identity, "session_key");
            if (!session_key.empty()) record.usage.session_key = session_key;
        }
        if (text(native, "counter_semantics") == "cumulative_turn_delta") {
            const auto turn = text(native, "turn_id");
            if (rollout.empty() || turn.empty() || record.usage.record_id != "codex:" + rollout + ":" + turn) return fail();
        }
        for (const auto& entry : {std::pair{"model", &record.usage.model},
                std::pair{"counter_field", &record.usage.counter_field}}) {
            const auto value = text(native, entry.first);
            if (!value.empty()) *entry.second = value;
        }
        const auto semantics = text(native, "counter_semantics");
        if (semantics == "cumulative_final" || semantics == "per_event_sum") {
            record.usage.source_counter_semantics = semantics;
        }
        record.usage.input = unknown_amount();
        record.usage.output = unknown_amount();
        record.usage.cache_read = unknown_amount();
        record.usage.cache_creation = unknown_amount();
        record.usage.coverage_unit = addon::Usage_coverage_unit::SESSIONS;
        const auto& breakdown = member(member(native, "capabilities"), "token_breakdown");
        if (trusted_breakdown && breakdown.is_object()) {
            if (integer(member(breakdown, "version")) != 2) return fail();
            const auto unit = text(breakdown, "coverage_unit");
            if (unit != "sessions" && unit != "records") return fail();
            record.usage.coverage_unit = unit == "sessions" ? addon::Usage_coverage_unit::SESSIONS : addon::Usage_coverage_unit::RECORDS;
            for (const auto& entry : {std::pair{"input_tokens", &record.usage.input},
                    std::pair{"output_tokens", &record.usage.output},
                    std::pair{"cache_read_input_tokens", &record.usage.cache_read},
                    std::pair{"cache_creation_input_tokens", &record.usage.cache_creation}}) {
                const auto amount = native_amount(member(breakdown, entry.first));
                if (!amount) return fail();
                *entry.second = *amount;
            }
        }
        record.recorded_cwd = text(native, "recorded_cwd");
        // Native Codex refreshes repository membership from recorded_cwd even
        // for retained files, rather than trusting yesterday's allocation.
        refresh_repository(&record);
        addon::Contract_failure failure;
        if (!addon::decode_usage_record(addon::encode_usage_record(record.usage, 2), &failure, 2) || !emit(record)) return fail();
    }
    return file;
}

} // namespace codex
