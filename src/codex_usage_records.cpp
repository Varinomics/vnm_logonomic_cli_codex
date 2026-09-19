#include "codex_usage_records.h"
#include "codex_jsonl_projector.h"

#include <logonomic_cli_addon/package_source.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::uint64_t k_max_exact_count = 9007199254740991;

std::string path_text(const fs::path& path)
{
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value absent;
    const auto* value = object.find(name);
    return value ? *value : absent;
}

std::string text(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    return value.is_string() ? value.as_string() : std::string{};
}

std::optional<std::uint64_t> count(const addon::Json_value& value)
{
    if (!value.is_number()) {
        return std::nullopt;
    }
    const double number = value.as_number();
    if (!std::isfinite(number) || number < 0 || number > k_max_exact_count || std::floor(number) != number) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(number);
}

std::optional<std::int64_t> timestamp(std::string_view iso)
{
    // Keep the native ISO parser's local-time interpretation when no zone is
    // supplied. Explicit offsets are converted without depending on local DST.
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (iso.size() < 19 || std::sscanf(std::string(iso.substr(0, 19)).c_str(),
            "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || year < 1 || year > 9999 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
    {
        return std::nullopt;
    }
    std::size_t offset = 19;
    std::int64_t milliseconds = 0;
    if (offset < iso.size() && iso[offset] == '.') {
        ++offset;
        const auto begin = offset;
        while (offset < iso.size() && iso[offset] >= '0' && iso[offset] <= '9') {
            if (offset - begin < 3) {
                milliseconds = milliseconds * 10 + (iso[offset] - '0');
            }
            ++offset;
        }
        if (offset == begin) {
            return std::nullopt;
        }
        for (auto digits = offset - begin; digits < 3; ++digits) milliseconds *= 10;
    }
    if (offset == iso.size()) {
        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon = month - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = second;
        local.tm_isdst = -1;
        const auto seconds = std::mktime(&local);
        if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
        return static_cast<std::int64_t>(seconds) * 1000 + milliseconds;
    }
    int zone_minutes = 0;
    if (offset < iso.size() && iso[offset] == 'Z' && offset + 1 == iso.size()) {
        ++offset;
    }
    else
    if (offset + 6 == iso.size() && (iso[offset] == '+' || iso[offset] == '-') && iso[offset + 3] == ':') {
        int zone_hour = 0, zone_minute = 0;
        if (std::sscanf(std::string(iso.substr(offset + 1)).c_str(), "%2d:%2d", &zone_hour, &zone_minute) != 2 ||
            zone_hour > 23 || zone_minute > 59)
        {
            return std::nullopt;
        }
        zone_minutes = (zone_hour * 60 + zone_minute) * (iso[offset] == '-' ? -1 : 1);
        offset = iso.size();
    }
    else {
        return std::nullopt;
    }
    if (offset != iso.size()) {
        return std::nullopt;
    }
    const std::int64_t days = std::chrono::sys_days(date).time_since_epoch().count();
    return ((days * 24 + hour) * 3600 + minute * 60 + second - zone_minutes * 60) * 1000 + milliseconds;
}

std::optional<std::int64_t> turn_started(const addon::Json_value& payload, const std::string& turn)
{
    if (const auto seconds = count(member(payload, "started_at"));
        seconds && *seconds <= std::numeric_limits<std::int64_t>::max() / 1000)
    {
        return static_cast<std::int64_t>(*seconds) * 1000;
    }
    if (turn.size() < 15 || turn[8] != '-' || turn[13] != '-' || turn[14] != '7') {
        return std::nullopt;
    }
    const std::string hex = turn.substr(0, 8) + turn.substr(9, 4);
    std::int64_t result = 0;
    const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), result, 16);
    return parsed.ec == std::errc{} && parsed.ptr == hex.data() + hex.size() && result > 0
        ? std::optional(result) : std::nullopt;
}

std::optional<std::string> completion_date(const addon::Json_value& event, const addon::Json_value& payload)
{
    std::optional<std::int64_t> seconds;
    if (const auto completed = count(member(payload, "completed_at"))) {
        seconds = static_cast<std::int64_t>(*completed);
    }
    else
    if (const auto completed = timestamp(text(event, "timestamp"))) {
        seconds = *completed / 1000;
    }
    if (!seconds) {
        return std::nullopt;
    }
    const auto instant = static_cast<std::time_t>(*seconds);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &instant) != 0) return std::nullopt;
#else
    if (!localtime_r(&instant, &local)) return std::nullopt;
#endif
    if (local.tm_year < -1899 || local.tm_year > 8099) return std::nullopt;
    std::array<char, 11> result{};
    if (std::strftime(result.data(), result.size(), "%Y-%m-%d", &local) == 0) return std::nullopt;
    return std::string(result.data());
}

struct Cumulative_category
{
    std::uint64_t latest = 0;
    std::uint64_t pending = 0;
    bool have_latest = false;
    bool complete = true;
    bool observed = false;

    void reset() { pending = 0; complete = true; observed = false; }
    addon::Usage_amount amount() const
    {
        const bool covered = complete && observed;
        return {true, covered ? pending : 0, addon::Usage_field_coverage{
            covered ? 1u : 0u, 1, false, covered ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN}};
    }
    void accumulate(const addon::Json_value& value, bool active, bool preceding_total, bool total_reset)
    {
        const auto sample = count(value);
        if (!sample) {
            have_latest = false;
            if (active) complete = false;
            return;
        }
        if (active) {
            observed = true;
            if (!have_latest && preceding_total) complete = false;
            const auto delta = !have_latest || total_reset || *sample < latest ? *sample : *sample - latest;
            if (delta > k_max_exact_count - pending) complete = false;
            else pending += delta;
        }
        latest = *sample;
        have_latest = true;
    }
};

} // namespace

std::optional<std::string> repository_for_cwd(std::string_view cwd)
{
    fs::path candidate = addon::path_from_utf8(cwd).lexically_normal();
    std::error_code error;
    if (!candidate.is_absolute() || !fs::is_directory(candidate, error)) return std::nullopt;
    while (!candidate.empty()) {
        const auto marker = candidate / ".git";
        if (fs::is_directory(marker, error) || fs::is_regular_file(marker, error)) return path_text(candidate);
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return std::nullopt;
}

void refresh_repository(Rollout_usage_record* record)
{
    const auto path = repository_for_cwd(record->recorded_cwd);
    record->usage.repository_path.reset();
    addon::Usage_repository repository;
    if (path) repository.allocations.push_back({*path, record->usage.total_tokens});
    else repository.unattributed_tokens = record->usage.total_tokens;
    record->usage.repository_usage = std::move(repository);
}

bool read_rollout_usage(std::istream& input, const fs::path& path,
    const std::function<bool(const Rollout_usage_record&)>& emit,
    const std::function<bool()>& cancelled, std::string* error)
{
    std::string rollout, cwd, turn, model;
    std::int64_t started_ms = 0;
    std::uint64_t latest = 0, pending = 0;
    Cumulative_category input_tokens, output_tokens, cache_tokens;
    bool have_identity = false, have_total = false, forked = false, local_activity = false;
    const auto fail = [error](const char* reason) { *error = reason; return false; };
    Codex_jsonl_projector projector([&](const addon::Json_value& event) {
        const auto& payload = member(event, "payload");
        const auto event_type = text(event, "type");
        if (event_type == "session_meta") {
            if (have_identity) return true;
            rollout = text(payload, "id");
            const auto instant = timestamp(text(payload, "timestamp"));
            if (rollout.empty() || !instant) return fail("rollout_identity_invalid");
            started_ms = *instant;
            forked = !text(payload, "forked_from_id").empty();
            cwd = text(payload, "cwd");
            have_identity = true;
            return true;
        }
        if (event_type == "turn_context") {
            const auto event_model = text(payload, "model");
            if (!event_model.empty()) model = event_model;
            return true;
        }
        if (event_type != "event_msg") return true;
        const auto type = text(payload, "type");
        if (type == "task_started") {
            const auto next_turn = text(payload, "turn_id");
            if (!have_identity || next_turn.empty()) return fail("turn_identity_invalid");
            if (forked) {
                const auto instant = turn_started(payload, next_turn);
                if (!instant) return fail("fork_start_time_invalid");
                if (*instant < started_ms) return true;
            }
            local_activity = true;
            turn = next_turn;
            input_tokens.reset(); output_tokens.reset(); cache_tokens.reset();
            return true;
        }
        if (type == "token_count") {
            const auto& usage = member(member(payload, "info"), "total_token_usage");
            const auto& value = member(usage, "total_tokens");
            if (value.is_null()) return true;
            const auto total = count(value);
            if (!total) return fail("token_total_invalid");
            const bool reset = have_total && *total < latest;
            if (!turn.empty()) {
                const auto delta = !have_total || reset ? *total : *total - latest;
                if (delta > k_max_exact_count - pending) return fail("token_total_overflow");
                pending += delta;
            }
            input_tokens.accumulate(member(usage, "input_tokens"), !turn.empty(), have_total, reset);
            output_tokens.accumulate(member(usage, "output_tokens"), !turn.empty(), have_total, reset);
            cache_tokens.accumulate(member(usage, "cached_input_tokens"), !turn.empty(), have_total, reset);
            latest = *total;
            have_total = true;
            return true;
        }
        if (type != "task_complete") return true;
        const auto completed_turn = text(payload, "turn_id");
        if (completed_turn.empty()) return fail("completion_turn_invalid");
        if (!local_activity) return true;
        if (completed_turn != turn) {
            if (!turn.empty() && pending == 0) { turn.clear(); return true; }
            return fail("completion_turn_mismatch");
        }
        const auto date = completion_date(event, payload);
        if (!date) return fail("completion_timestamp_invalid");
        Rollout_usage_record record;
        record.usage.record_id = "codex:" + rollout + ":" + turn;
        record.usage.session_key = rollout;
        record.usage.completed_date = *date;
        record.usage.total_tokens = pending;
        record.usage.source_path = path_text(path);
        record.usage.input = input_tokens.amount();
        record.usage.output = output_tokens.amount();
        record.usage.cache_read = cache_tokens.amount();
        record.usage.cache_creation = {false, std::nullopt, addon::Usage_field_coverage{0, 1, false, addon::Usage_support::UNSUPPORTED}};
        record.usage.coverage_unit = addon::Usage_coverage_unit::SESSIONS;
        if (!model.empty()) record.usage.model = model;
        record.usage.counter_field = "total_token_usage.total_tokens";
        record.recorded_cwd = cwd;
        refresh_repository(&record);
        if (!emit(record)) return false;
        pending = 0;
        input_tokens.reset(); output_tokens.reset(); cache_tokens.reset();
        turn.clear();
        return true;
    });
    std::array<char, 64 * 1024> bytes{};
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        if (cancelled()) return fail("cancelled");
        if (!projector.feed(std::string_view(bytes.data(), static_cast<std::size_t>(input.gcount())), error)) {
            if (error->starts_with("Codex JSONL")) *error = "rollout_record_malformed";
            return false;
        }
    }
    if (input.bad()) return fail("rollout_read_failed");
    projector.finish_file();
    return have_identity || fail("rollout_identity_invalid");
}

} // namespace codex
