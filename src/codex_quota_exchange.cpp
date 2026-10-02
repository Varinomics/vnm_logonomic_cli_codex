#include "codex_quota_exchange.h"

#include <cmath>
#include <cctype>
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

bool authentication_required(const addon::Json_value& error)
{
    const auto& value = member(error, "message");
    if (!value.is_string()) return false;
    std::string message = value.as_string().substr(0, 4096);
    for (auto& character : message) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    const auto contains = [&message](std::string_view needle) { return message.find(needle) != std::string::npos; };
    bool status_401 = false;
    for (auto offset = message.find("401"); offset != std::string::npos; offset = message.find("401", offset + 3)) {
        const auto word = [](unsigned char value) { return std::isalnum(value) || value == '_'; };
        if ((offset == 0 || !word(message[offset - 1])) &&
            (offset + 3 == message.size() || !word(message[offset + 3]))) status_401 = true;
    }
    return (contains("token") || contains("credential") || contains("authentication") ||
        contains("authorization") || contains("login") || contains("log in") || contains("logged in")) &&
        (status_401 || contains("unauthorized") || contains("revoked") || contains("expired") || contains("not logged in"));
}

bool whole_number(const addon::Json_value& value, double* number)
{
    if (!value.is_number()) return false;
    *number = value.as_number();
    return std::isfinite(*number) && *number >= 0 && *number <= 9007199254740991.0 && std::floor(*number) == *number;
}

bool quota_window(const addon::Json_value& value, std::optional<addon::Quota_window>* output)
{
    if (value.is_null()) return true;
    const auto& used = member(value, "usedPercent");
    if (!value.is_object() || !used.is_number() || !std::isfinite(used.as_number())) return false;
    addon::Quota_window window;
    window.used_percent = used.as_number();
    const auto& duration = member(value, "windowDurationMins");
    double number = 0;
    if (!duration.is_null()) {
        if (!whole_number(duration, &number)) return false;
        if (number > 0) window.window_minutes = static_cast<std::uint64_t>(number);
    }
    if (whole_number(member(value, "resetsAt"), &number) && number > 0 && number <= 9007199254740.0) {
        window.resets_at_ms = static_cast<std::uint64_t>(number) * 1000;
    }
    *output = window;
    return true;
}

bool matches_id(const addon::Json_value& value, int id)
{
    return (value.is_number() && value.as_number() == id) ||
        (value.is_string() && value.as_string() == std::to_string(id));
}

} // namespace

addon::Quota_inspect_result quota_response(const addon::Json_value& response)
{
    addon::Quota_inspect_result result;
    result.complete = true;
    addon::Quota_observation observation;
    observation.state = addon::Quota_status::FAILED;
    const auto& error = member(response, "error");
    const auto& payload = member(response, "result");
    const auto& limits = member(payload, "rateLimits");
    if (response.find("error")) {
        if (authentication_required(error)) result.failure_reason = "authentication_required";
    }
    else
    if (limits.is_object() && quota_window(member(limits, "primary"), &observation.primary) &&
        quota_window(member(limits, "secondary"), &observation.secondary)) {
        observation.state = addon::Quota_status::READY;
        const auto& plan = member(limits, "planType");
        if (plan.is_string() && !plan.as_string().empty()) observation.plan = plan.as_string();
        const auto& balance = member(member(limits, "credits"), "balance");
        if (balance.is_string()) observation.credits_balance = balance.as_string();
        double count = 0;
        if (whole_number(member(member(payload, "rateLimitResetCredits"), "availableCount"), &count)) {
            observation.reset_credits = static_cast<std::int64_t>(count);
        }
    }
    result.observation = std::move(observation);
    return result;
}

bool Quota_exchange::record(std::string_view line)
{
    if (line.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return true;
    }
    const auto value = addon::parse_json(line, {}, nullptr);
    if (!value || !value->is_object()) {
        return false;
    }
    const auto* version = value->find("jsonrpc");
    if (version && (!version->is_string() || version->as_string() != "2.0")) {
        return false;
    }
    const auto* id = value->find("id");
    const auto* method = value->find("method");
    const auto* result = value->find("result");
    const auto* error = value->find("error");
    if (method) {
        // Server requests need an answer; they cannot be treated as notifications.
        return method->is_string() && !method->as_string().empty() && !id && !result && !error;
    }
    if (!id || (result == nullptr) == (error == nullptr) || m_response ||
        !matches_id(*id, m_initialized ? 2 : 1))
    {
        return false;
    }
    if (error) {
        const auto* code = error->find("code");
        const auto* message = error->find("message");
        if (!error->is_object() || !code || !code->is_number() ||
            !std::isfinite(code->as_number()) || std::floor(code->as_number()) != code->as_number() ||
            !message || !message->is_string())
        {
            return false;
        }
    }
    else
    if (!result->is_object()) {
        return false;
    }
    if (m_initialized) {
        m_response = *value;
        return true;
    }
    if (error) {
        return false;
    }
    m_initialized = true;
    m_input = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":null}\n";
    return true;
}

bool Quota_exchange::consume(std::string_view bytes)
{
    if (m_failed) {
        return false;
    }
    m_pending.append(bytes);
    std::size_t begin = 0;
    for (auto end = m_pending.find('\n'); end != std::string::npos; end = m_pending.find('\n', begin)) {
        if (!record(std::string_view(m_pending).substr(begin, end - begin))) {
            m_failed = true;
            return false;
        }
        begin = end + 1;
    }
    m_pending.erase(0, begin);
    return true;
}

bool Quota_exchange::finish()
{
    m_failed = m_failed || !record(m_pending);
    m_pending.clear();
    return !m_failed && m_response.has_value();
}

std::string Quota_exchange::take_input()
{
    return std::exchange(m_input, {});
}

} // namespace codex
