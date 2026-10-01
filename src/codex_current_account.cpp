#include "codex_current_account.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/sha256.h>

#include <cmath>
#include <cstdint>
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

std::string_view text(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    return value.is_string() ? std::string_view(value.as_string()) : std::string_view{};
}

bool matches_id(const addon::Json_value& value, int id)
{
    return (value.is_number() && value.as_number() == id) ||
        (value.is_string() && value.as_string() == std::to_string(id));
}

std::optional<addon::Json_value> token_claims(std::string_view token)
{
    const auto first = token.find('.');
    const auto last = token.find('.', first == std::string_view::npos ? 0 : first + 1);
    if (first == std::string_view::npos || first == 0 || last == std::string_view::npos ||
        last == first + 1 || last + 1 == token.size() || token.find('.', last + 1) != std::string_view::npos)
    {
        return {};
    }
    const auto payload = token.substr(first + 1, last - first - 1);
    if (payload.size() % 4 == 1) {
        return {};
    }
    std::string decoded;
    std::uint32_t bits = 0;
    int available = 0;
    for (const char character : payload) {
        int value = -1;
        if (character >= 'A' && character <= 'Z') value = character - 'A';
        else
        if (character >= 'a' && character <= 'z') value = character - 'a' + 26;
        else
        if (character >= '0' && character <= '9') value = character - '0' + 52;
        else
        if (character == '-') value = 62;
        else
        if (character == '_') value = 63;
        if (value < 0) {
            return {};
        }
        bits = (bits << 6) | static_cast<std::uint32_t>(value);
        available += 6;
        if (available >= 8) {
            available -= 8;
            decoded += static_cast<char>((bits >> available) & 0xff);
        }
    }
    if (available != 0 && (bits & ((1u << available) - 1)) != 0) {
        return {};
    }
    return addon::parse_json(decoded, {}, nullptr);
}

std::string current_identity(const addon::Json_value& account, const addon::Json_value& auth)
{
    const auto method = text(auth, "authMethod");
    if ((method != "chatgpt" && method != "chatgptAuthTokens") ||
        text(member(account, "account"), "type") != "chatgpt")
    {
        return {};
    }
    const auto claims = token_claims(text(auth, "authToken"));
    if (!claims || !claims->is_object()) {
        return {};
    }
    const auto& native_auth = member(*claims, "https://api.openai.com/auth");
    const auto& routing = member(account, "workspaceRouting");
    const auto workspace = text(routing, "chatgptAccountId");
    const auto backend = text(routing, "backendOrigin");
    const auto issuer = text(*claims, "iss");
    const auto subject = text(*claims, "sub");
    const auto chatgpt_user = text(native_auth, "chatgpt_user_id");
    const auto alternate_user = text(native_auth, "user_id");
    const auto user = chatgpt_user.empty() ? alternate_user : chatgpt_user;
    if (workspace.empty() || backend.empty() || issuer.empty() || subject.empty() || user.empty() ||
        (!chatgpt_user.empty() && !alternate_user.empty() && chatgpt_user != alternate_user) ||
        text(native_auth, "chatgpt_account_id") != workspace)
    {
        return {};
    }
    // The credential came from this native process's selected AuthManager,
    // not an arbitrary local auth.json. Its claims are not a login verifier.
    // Include the backend and principal as well as workspace: a shared team
    // workspace must not collapse different users, and custom origins stay apart.
    auto identity = addon::Json_value::array();
    for (const auto part : {issuer, subject, user, workspace, backend}) {
        identity.push_back(addon::Json_value(part));
    }
    return "codex-current-" + addon::to_hex(addon::Sha256::digest(addon::serialize_json(identity)));
}

} // namespace

void Current_account_exchange::request_current_account()
{
    m_account_received = false;
    m_auth_received = false;
    m_account_changed = false;
    m_account.reset();
    m_auth.reset();
    m_input += "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(m_account_request_id) +
        ",\"method\":\"account/read\",\"params\":{\"refreshToken\":false}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(m_account_request_id + 1) +
        ",\"method\":\"getAuthStatus\",\"params\":{\"includeToken\":true,\"refreshToken\":false}}\n";
}

bool Current_account_exchange::record(std::string_view line)
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
        if (!method->is_string() || method->as_string().empty() || id || result || error) {
            return false;
        }
        if (m_initialized && method->as_string() == "account/updated") {
            m_account_changed = true;
        }
        return true;
    }
    if (!id || (result == nullptr) == (error == nullptr)) {
        return false;
    }
    if (error) {
        const auto& code = member(*error, "code");
        if (!error->is_object() || !code.is_number() || !std::isfinite(code.as_number()) ||
            std::floor(code.as_number()) != code.as_number() || !member(*error, "message").is_string())
        {
            return false;
        }
    }
    else
    if (!result->is_object()) {
        return false;
    }
    if (!m_initialized) {
        if (!matches_id(*id, 1) || error) {
            return false;
        }
        m_initialized = true;
        m_input = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}\n";
        request_current_account();
        return true;
    }
    if (matches_id(*id, m_account_request_id) && !m_account_received) {
        m_account_received = true;
        if (result) m_account = *result;
        return true;
    }
    if (matches_id(*id, m_account_request_id + 1) && !m_auth_received) {
        m_auth_received = true;
        if (result) m_auth = *result;
        return true;
    }
    return false;
}

bool Current_account_exchange::consume(std::string_view bytes)
{
    if (m_failed) {
        return false;
    }
    // Keep the existing structured observation stream boundary inside the
    // helper now that raw output must no longer reach the host's stream guard.
    if (bytes.size() > addon::limits::k_max_structured_stream_bytes - m_output_bytes) {
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
        if (m_account_received && m_auth_received && m_account_changed && !m_input_closed) {
            // account/updated also announces initial workspace routing. Read a
            // fresh pair after either event instead of treating readiness as an
            // unknown account or publishing evidence from before a real change.
            // The existing native deadline and stream bound govern this exchange.
            m_account_request_id += 2;
            request_current_account();
        }
        begin = end + 1;
    }
    m_pending.erase(0, begin);
    return true;
}

bool Current_account_exchange::finish()
{
    m_input_closed = true;
    m_failed = m_failed || !record(m_pending);
    m_pending.clear();
    return !m_failed && m_account_received && m_auth_received;
}

std::string Current_account_exchange::take_input()
{
    return std::exchange(m_input, {});
}

bool Current_account_exchange::close_input()
{
    m_input_closed = m_input_closed || m_failed || (m_account_received && m_auth_received);
    return m_input_closed;
}

addon::Json_value Current_account_exchange::observation() const
{
    auto observation = addon::Json_value::object();
    observation.set("version", addon::Json_value(1));
    observation.set("state", addon::Json_value("unknown"));
    if (m_failed || !m_auth || !m_account_received || !m_auth_received) {
        return observation;
    }
    const auto& required = member(*m_auth, "requiresOpenaiAuth");
    if (!required.is_boolean() || !required.as_boolean()) {
        return observation;
    }
    const auto* method = m_auth->find("authMethod");
    const auto* token = m_auth->find("authToken");
    if (method && method->is_null() && token && token->is_null()) {
        observation.set("state", addon::Json_value("not_logged_in"));
    }
    else
    if (method && method->is_string() && !method->as_string().empty() &&
        token && (token->is_null() || token->is_string()))
    {
        // Native authentication remains valid for credentials that Codex
        // deliberately refuses to export. Only optional identity needs a token.
        observation.set("state", addon::Json_value("authenticated"));
        if (m_account && !m_account_changed) {
            const auto identity = current_identity(*m_account, *m_auth);
            if (!identity.empty()) {
                observation.set("identity_key", addon::Json_value(identity));
            }
        }
    }
    return observation;
}

} // namespace codex
