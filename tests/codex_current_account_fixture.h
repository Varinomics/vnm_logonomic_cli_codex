#pragma once

#include <logonomic_cli_addon/json.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace codex_fixture {

inline std::string token(std::string_view user = "user-fixture", std::string_view workspace = "workspace-fixture")
{
    namespace addon = logonomic::cli_addon;
    auto claims = addon::Json_value::object();
    claims.set("iss", addon::Json_value("https://auth.example.test"));
    claims.set("sub", addon::Json_value(user));
    auto auth = addon::Json_value::object();
    auth.set("chatgpt_user_id", addon::Json_value(user));
    auth.set("chatgpt_account_id", addon::Json_value(workspace));
    claims.set("https://api.openai.com/auth", std::move(auth));
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const auto bytes = addon::serialize_json(claims);
    std::string payload;
    std::uint32_t bits = 0;
    int available = 0;
    for (const unsigned char byte : bytes) {
        bits = (bits << 8) | byte;
        available += 8;
        while (available >= 6) {
            available -= 6;
            payload += alphabet[(bits >> available) & 63];
        }
    }
    if (available != 0) payload += alphabet[(bits << (6 - available)) & 63];
    return "eyJhbGciOiJSUzI1NiJ9." + payload + ".secret-signature-canary";
}

inline logonomic::cli_addon::Json_value account(
    std::string_view workspace = "workspace-fixture", std::string_view backend = "https://chatgpt.com")
{
    namespace addon = logonomic::cli_addon;
    auto result = addon::Json_value::object();
    auto login = addon::Json_value::object();
    login.set("type", addon::Json_value("chatgpt"));
    login.set("email", addon::Json_value("private-email-canary@example.test"));
    auto routing = addon::Json_value::object();
    routing.set("chatgptAccountId", addon::Json_value(workspace));
    routing.set("backendOrigin", addon::Json_value(backend));
    result.set("account", std::move(login));
    result.set("workspaceRouting", std::move(routing));
    return result;
}

inline logonomic::cli_addon::Json_value authentication(std::string_view credential = token())
{
    namespace addon = logonomic::cli_addon;
    auto result = addon::Json_value::object();
    result.set("authMethod", addon::Json_value("chatgpt"));
    result.set("authToken", addon::Json_value(credential));
    result.set("requiresOpenaiAuth", addon::Json_value(true));
    return result;
}

} // namespace codex_fixture
