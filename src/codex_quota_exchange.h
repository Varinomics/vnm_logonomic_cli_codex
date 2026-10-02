#pragma once

#include <logonomic_cli_addon/quota.h>

#include <optional>
#include <string>
#include <string_view>

namespace codex {

// Interpret one validated native response without assigning a host source scope.
logonomic::cli_addon::Quota_inspect_result quota_response(
    const logonomic::cli_addon::Json_value& response);

// The helper drives this exchange; final inspection validates the same complete
// transcript. A quota response ends requests, not the native output stream.
class Quota_exchange
{
public:
    bool consume(std::string_view bytes);
    bool finish();
    std::string take_input();
    bool close_input() const { return m_failed || m_response.has_value(); }
    const std::optional<logonomic::cli_addon::Json_value>& response() const { return m_response; }

private:
    bool record(std::string_view line);

    bool m_initialized = false;
    bool m_failed = false;
    std::string m_pending;
    std::string m_input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"clientInfo\":{\"name\":\"logonomic\",\"version\":\"0\"},\"capabilities\":{\"experimentalApi\":true}}}\n";
    std::optional<logonomic::cli_addon::Json_value> m_response;
};

} // namespace codex
