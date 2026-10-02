#pragma once

#include <logonomic_cli_addon/json.h>

#include <optional>
#include <string>
#include <string_view>

namespace codex {

// Token-bearing native records stay inside the helper. Only the allowlisted
// authentication state and an opaque account identity may cross its stdout.
class Current_account_exchange
{
public:
    Current_account_exchange() = default;
    explicit Current_account_exchange(int first_request_id);
    bool consume(std::string_view bytes);
    bool finish();
    std::string take_input();
    bool close_input();
    logonomic::cli_addon::Json_value observation() const;

private:
    bool record(std::string_view line);
    void request_current_account();

    bool m_initialized = false;
    bool m_failed = false;
    bool m_account_received = false;
    bool m_auth_received = false;
    bool m_account_changed = false;
    bool m_input_closed = false;
    int m_account_request_id = 2;
    std::size_t m_output_bytes = 0;
    std::string m_pending;
    std::string m_input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"clientInfo\":{\"name\":\"logonomic\",\"version\":\"0\"},\"capabilities\":{\"experimentalApi\":true}}}\n";
    std::optional<logonomic::cli_addon::Json_value> m_account;
    std::optional<logonomic::cli_addon::Json_value> m_auth;
};

} // namespace codex
