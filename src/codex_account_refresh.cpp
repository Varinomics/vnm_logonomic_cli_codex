#include "codex_account_refresh.h"

#include <logonomic_cli_addon/limits.h>

#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

std::string observation_record(std::string_view kind, addon::Json_value result)
{
    auto record = addon::Json_value::object();
    record.set("kind", addon::Json_value(kind));
    record.set("result", std::move(result));
    return addon::serialize_json(record) + '\n';
}

} // namespace

bool Account_refresh_exchange::record(std::string_view line)
{
    if (m_auth) {
        return m_auth->consume(std::string(line) + '\n');
    }
    if (!m_quota.consume(std::string(line) + '\n')) {
        return false;
    }
    if (m_quota.response()) {
        auto quota = quota_response(*m_quota.response());
        auto payload = addon::encode_quota_inspect_result(quota, 2);
        if (!addon::decode_quota_inspect_result(payload, true, nullptr, 2)) {
            // Invalid native values are a failed quota disposition, not private
            // diagnostics or a reason to manufacture another native process.
            quota.observation = addon::Quota_observation{};
            quota.observation->state = addon::Quota_status::FAILED;
            payload = addon::encode_quota_inspect_result(quota, 2);
        }
        m_output += observation_record("quota", std::move(payload));
        m_auth.emplace(3);
    }
    return true;
}

bool Account_refresh_exchange::consume(std::string_view bytes)
{
    if (m_failed || bytes.size() > addon::limits::k_max_structured_stream_bytes - m_output_bytes) {
        m_failed = true;
        return false;
    }
    m_output_bytes += bytes.size();
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

bool Account_refresh_exchange::finish()
{
    m_failed = m_failed || !record(m_pending);
    m_pending.clear();
    if (m_failed || !m_auth || !m_auth->finish()) {
        return false;
    }
    m_output += observation_record("auth", m_auth->observation());
    return true;
}

std::string Account_refresh_exchange::take_input()
{
    return m_auth ? m_auth->take_input() : m_quota.take_input();
}

std::string Account_refresh_exchange::take_output()
{
    return std::exchange(m_output, {});
}

bool Account_refresh_exchange::close_input()
{
    return m_failed || (m_auth && m_auth->close_input());
}

} // namespace codex
