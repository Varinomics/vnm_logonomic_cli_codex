#include "codex_probes.h"
#include "codex_accounts.h"
#include "codex_launcher.h"
#include "codex_quota_exchange.h"

#include <logonomic_cli_addon/current_account.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <chrono>
#include <cctype>
#include <filesystem>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value missing;
    const auto* value = object.find(name);
    return value ? *value : missing;
}

std::string_view trimmed(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

addon::Observation_scope scope(const addon::Probe_inspect_request& request)
{
    addon::Observation_scope result;
    if (request.account_context) result.source_account_key = request.account_context->adapter_account_key;
    result.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return result;
}

addon::Quota_inspect_result failed_quota_inspection(const addon::Probe_inspect_request& request)
{
    if (request.inspection.timed_out) {
        return addon::make_quota_failure(addon::Quota_failure::TIMED_OUT, scope(request));
    }
    if (request.inspection.exit_code != 0) {
        return addon::make_quota_failure(addon::Quota_failure::PROCESS_FAILED, scope(request),
            addon::Quota_diagnostic{addon::Quota_failure_stage::PROCESS_EXIT, request.inspection.exit_code});
    }
    return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE, scope(request));
}

bool auth_record_valid(const addon::Json_value& value)
{
    if (!value.is_object() || member(value, "version") != addon::Json_value(1)) return false;
    for (const auto& entry : value.as_object()) {
        if (entry.name != "version" && entry.name != "state" && entry.name != "identity_key") return false;
    }
    const auto& state = member(value, "state");
    if (state != addon::Json_value("authenticated") && state != addon::Json_value("not_logged_in") &&
        state != addon::Json_value("unknown")) return false;
    const auto* key = value.find("identity_key");
    constexpr std::string_view prefix = "codex-current-";
    return !key || (state == addon::Json_value("authenticated") && key->is_string() &&
        key->as_string().starts_with(prefix) &&
        addon::sha256_from_hex(std::string_view(key->as_string()).substr(prefix.size())).has_value());
}

void assign_environment(std::vector<std::pair<std::string, std::string>>* environment,
    std::string name, std::string value, bool windows)
{
    for (auto& assignment : *environment) {
        if (windows ? addon::ascii_lowercase(assignment.first) == addon::ascii_lowercase(name) : assignment.first == name) {
            assignment = {std::move(name), std::move(value)};
            return;
        }
    }
    environment->emplace_back(std::move(name), std::move(value));
}


} // namespace

addon::Auth_inspect_result inspect_auth(const addon::Probe_inspect_request& request)
{
    addon::Auth_inspect_result result;
    result.complete = request.inspection.final;
    if (!result.complete) { result.protocol_state = request.inspection.protocol_state; return result; }
    addon::Auth_observation observation;
    observation.scope = scope(request);
    if (request.observe_current_account) {
        if (!request.inspection.timed_out && request.inspection.exit_code == 0) {
            const auto payload = addon::parse_json(request.inspection.stdout_bytes, {}, nullptr);
            if (payload && member(*payload, "version") == addon::Json_value(1)) {
                const auto& state = member(*payload, "state");
                if (state == addon::Json_value("authenticated")) {
                    observation.state = addon::Auth_status::AUTHENTICATED;
                    const auto& key = member(*payload, "identity_key");
                    constexpr std::string_view prefix = "codex-current-";
                    if (key.is_string() && key.as_string().starts_with(prefix) &&
                        addon::sha256_from_hex(std::string_view(key.as_string()).substr(prefix.size())) &&
                        request.account_context && request.account_context->profile_path)
                    {
                        addon::Current_account_observation identity;
                        identity.scope = observation.scope;
                        identity.identity_key = key.as_string();
                        const auto filename = addon::path_from_utf8(*request.account_context->profile_path)
                            .lexically_normal().filename().generic_u8string();
                        std::string name(filename.begin(), filename.end());
#ifdef _WIN32
                        name = addon::ascii_lowercase(name);
#endif
                        identity.is_default_profile = name == ".codex";
                        result.current_account = std::move(identity);
                    }
                }
                else
                if (state == addon::Json_value("not_logged_in")) {
                    observation.state = addon::Auth_status::NOT_LOGGED_IN;
                }
            }
        }
        result.observation = std::move(observation);
        return result;
    }
    if (!request.inspection.timed_out && request.inspection.exit_code == 0) {
        observation.state = addon::Auth_status::AUTHENTICATED;
    }
    else
    if (!request.inspection.timed_out && request.inspection.exit_code == 1 &&
        trimmed(request.inspection.stdout_bytes).empty() && trimmed(request.inspection.stderr_bytes) == "Not logged in") {
        observation.state = addon::Auth_status::NOT_LOGGED_IN;
    }
    result.observation = observation;
    return result;
}

addon::Quota_inspect_result inspect_quota(const addon::Probe_inspect_request& request, std::string* failure_reason)
{
    addon::Quota_inspect_result result;
    result.complete = request.inspection.final;
    if (!result.complete) {
        result.protocol_state = request.inspection.protocol_state;
        return result;
    }
    Quota_exchange exchange;
    if (!request.inspection.timed_out && request.inspection.exit_code == 0 &&
        exchange.consume(request.inspection.stdout_bytes) && exchange.finish())
    {
        result = quota_response(*exchange.response());
    }
    else {
        result = failed_quota_inspection(request);
    }
    *failure_reason = result.failure_reason.value_or("");
    result.observation->scope = scope(request);
    return result;
}

std::optional<addon::Account_refresh_inspect_result> inspect_account_refresh(const addon::Probe_inspect_request& request)
{
    bool quota_received = false;
    addon::Json_value auth;
    std::string pending;
    if (!request.inspection.protocol_state.empty()) {
        const auto state = addon::parse_json(request.inspection.protocol_state, {}, nullptr);
        if (!state || !state->is_object() || state->as_object().size() != 3 ||
            !member(*state, "quota_received").is_boolean() || !member(*state, "pending").is_string() ||
            !state->find("auth")) return {};
        quota_received = member(*state, "quota_received").as_boolean();
        pending = member(*state, "pending").as_string();
        auth = member(*state, "auth");
        if (!auth.is_null() && (!quota_received || !auth_record_valid(auth))) return {};
    }
    if (pending.size() > addon::limits::k_max_structured_stream_bytes ||
        request.inspection.stdout_bytes.size() > addon::limits::k_max_structured_stream_bytes - pending.size()) return {};
    pending += request.inspection.stdout_bytes;
    addon::Account_refresh_inspect_result result;
    result.complete = request.inspection.final;
    std::size_t begin = 0;
    for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n', begin)) {
        const auto record = addon::parse_json(std::string_view(pending).substr(begin, end - begin), {}, nullptr);
        if (!record || !record->is_object() || record->as_object().size() != 2 || !record->find("result")) return {};
        const auto& kind = member(*record, "kind");
        const auto& payload = member(*record, "result");
        if (kind == addon::Json_value("quota") && !quota_received) {
            auto quota = addon::decode_quota_inspect_result(payload, true, nullptr, 2);
            if (!quota || !quota->complete || !quota->observation || !quota->protocol_state.empty() || quota->stdin_bytes) return {};
            quota->observation->scope = scope(request);
            result.quota = std::move(quota);
            quota_received = true;
        }
        else
        if (kind == addon::Json_value("auth") && quota_received && auth.is_null() && auth_record_valid(payload)) {
            auth = payload;
        }
        else {
            return {};
        }
        begin = end + 1;
    }
    pending.erase(0, begin);
    if (result.complete) {
        if (!pending.empty()) return {};
        if (!quota_received) {
            result.quota = failed_quota_inspection(request);
        }
        auto auth_request = request;
        auth_request.observe_current_account = true;
        auth_request.inspection.stdout_bytes = auth.is_null() ? "" : addon::serialize_json(auth);
        result.auth = inspect_auth(auth_request);
        if (!request.observe_current_account) result.auth->current_account.reset();
    }
    else {
        auto state = addon::Json_value::object();
        state.set("quota_received", addon::Json_value(quota_received));
        state.set("auth", std::move(auth));
        state.set("pending", addon::Json_value(std::move(pending)));
        result.protocol_state = addon::serialize_json(state);
        if (result.protocol_state.size() > addon::limits::k_max_protocol_state_bytes) return {};
    }
    return result;
}

void handle_probes(addon::Invocation_context& context)
{
    if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
    const bool quota = context.request().capability.capability_id == "quota";
    const bool account_refresh = context.request().capability.capability_id == addon::k_account_refresh_capability_id;
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_prepare_operation) {
        const auto request = account_refresh
            ? addon::decode_account_refresh_prepare_request(context.request().payload, &failure)
            : addon::decode_probe_prepare_request(context.request().payload, &failure, 2);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
        if (account_refresh && !request->observe_current_account) {
            // Native app-server AUTH and `login status` have different meanings
            // for custom providers. Only combine the existing app-server mode.
            context.fail(addon::Result_status::UNAVAILABLE, "account_refresh_requires_current_account");
            return;
        }
        const auto selected = selected_profile(request->context, context);
        if (!selected) return;
        const auto command = recognize_launcher(request->context.launcher.script_body, request->context.launcher.runner_key);
        if (!command) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized"); return; }
        const auto& environment = *request->context.launch_environment;
        const bool current_account = !quota && request->observe_current_account;
        const bool native_server = quota || current_account || account_refresh;
        // Account observations need native authentication and routing, but do
        // not use plugins or their background marketplace synchronization.
        const auto argv = resolve_probe_launcher(*command, native_server
            ? std::vector<std::string>{"-c", "features.plugins=false", "app-server", "--stdio"}
            : std::vector<std::string>{"login", "status"}, environment.effective_environment,
            request->context.working_directory, quota || account_refresh);
        if (!argv) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
        addon::Structured_prepare_result result;
#if defined(_WIN32)
        result.process.executable = native_server ? "helpers/codex_quota_helper.exe" : "helpers/codex_launch_bridge.exe";
#else
        result.process.executable = native_server ? "helpers/codex_quota_helper" : "helpers/codex_launch_bridge";
#endif
        result.process.argv = *argv;
        if (account_refresh) {
            result.process.argv.insert(result.process.argv.begin(), "--account-refresh-current");
        }
        else
        if (current_account) {
            result.process.argv.insert(result.process.argv.begin(), "--current-account");
        }
        const bool windows = environment.platform_id.starts_with("windows-");
        for (const auto& item : environment.effective_environment.as_object()) {
            assign_environment(&result.process.environment, item.name, item.value.as_string(), windows);
        }
        // Keep the host's resolved prefix values, including quoting and
        // expansion, instead of replaying recognizer text into the probe.
        assign_environment(&result.process.environment, "CODEX_HOME", selected->profile_path, windows);
        result.profile_storage = request->context.profile_storage;
        result.account_context = request->context.account_context;
        result.inspection.incremental = account_refresh;
        result.inspection.protocol_state = account_refresh ? "" : "await_exit";
        context.succeed(addon::encode_structured_prepare_result(result));
        return;
    }
    if (context.request().operation != addon::k_inspect_operation) { context.reject_unknown_operation(); return; }
    const auto request = account_refresh
        ? addon::decode_account_refresh_inspect_request(context.request().payload, &failure)
        : addon::decode_probe_inspect_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (account_refresh) {
        const auto result = inspect_account_refresh(*request);
        if (!result) { context.fail(addon::Result_status::FAILED, "invalid_account_refresh_output"); return; }
        context.succeed(addon::encode_account_refresh_inspect_result(*result, request->report_quota_failure));
        return;
    }
    if (!quota) { context.succeed(addon::encode_auth_inspect_result(inspect_auth(*request))); return; }
    std::string reason;
    auto result = inspect_quota(*request, &reason);
    if (!reason.empty()) result.failure_reason = reason;
    context.succeed(addon::encode_quota_inspect_result(result, 2, request->report_quota_failure));
}

} // namespace codex
