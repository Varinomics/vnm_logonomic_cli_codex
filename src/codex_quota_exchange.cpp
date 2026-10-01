#include "codex_quota_exchange.h"

#include <cmath>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

bool matches_id(const addon::Json_value& value, int id)
{
    return (value.is_number() && value.as_number() == id) ||
        (value.is_string() && value.as_string() == std::to_string(id));
}

} // namespace

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
