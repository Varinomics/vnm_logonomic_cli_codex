#include "codex_probes.h"
#include "codex_accounts.h"
#include "codex_launcher.h"

#include <chrono>
#include <cmath>
#include <cctype>

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

std::optional<addon::Json_value> response_for_id(std::string_view output, int id)
{
    while (!output.empty()) {
        const auto end = output.find('\n');
        const auto line = trimmed(output.substr(0, end));
        const auto value = addon::parse_json(line, {}, nullptr);
        if (value && value->is_object()) {
            const auto& identifier = member(*value, "id");
            if ((identifier.is_number() && identifier.as_number() == id) ||
                (identifier.is_string() && identifier.as_string() == std::to_string(id))) return value;
        }
        if (end == std::string_view::npos) break;
        output.remove_prefix(end + 1);
    }
    return std::nullopt;
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

} // namespace

addon::Auth_inspect_result inspect_auth(const addon::Probe_inspect_request& request)
{
    addon::Auth_inspect_result result;
    result.complete = request.inspection.final;
    if (!result.complete) { result.protocol_state = request.inspection.protocol_state; return result; }
    addon::Auth_observation observation;
    observation.scope = scope(request);
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
    const auto response = response_for_id(request.inspection.stdout_bytes, 2);
    if (response) {
        result.complete = true;
        addon::Quota_observation observation;
        observation.scope = scope(request);
        observation.state = addon::Quota_status::FAILED;
        const auto& error = member(*response, "error");
        const auto& payload = member(*response, "result");
        const auto& limits = member(payload, "rateLimits");
        if (response->find("error")) {
            if (authentication_required(error)) *failure_reason = "authentication_required";
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
        result.observation = observation;
        return result;
    }
    if (!request.inspection.final && request.inspection.protocol_state == "await_initialize" &&
        response_for_id(request.inspection.stdout_bytes, 1)) {
        result.protocol_state = "await_quota";
        result.stdin_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":null}\n";
        return result;
    }
    result.complete = request.inspection.final;
    if (result.complete) {
        addon::Quota_observation observation;
        observation.scope = scope(request);
        observation.state = addon::Quota_status::FAILED;
        result.observation = std::move(observation);
    } else result.protocol_state = request.inspection.protocol_state;
    return result;
}

void handle_probes(addon::Invocation_context& context)
{
    if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
    const bool quota = context.request().capability.capability_id == "quota";
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_prepare_operation) {
        const auto request = addon::decode_probe_prepare_request(context.request().payload, &failure, 2);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
        const auto selected = selected_profile(request->context, context);
        if (!selected) return;
        const auto command = recognize_launcher(request->context.launcher.script_body);
        if (!command) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized"); return; }
        const auto& environment = *request->context.launch_environment;
        const auto argv = resolve_probe_launcher(*command, quota ? std::vector<std::string>{"app-server", "--stdio"} :
            std::vector<std::string>{"login", "status"}, environment.effective_environment, request->context.working_directory);
        if (!argv) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
        addon::Structured_prepare_result result;
#if defined(_WIN32)
        result.process.executable = "helpers/codex_launch_bridge.exe";
#else
        result.process.executable = "helpers/codex_launch_bridge";
#endif
        result.process.argv = *argv;
        const bool windows = environment.platform_id.starts_with("windows-");
        for (const auto& item : environment.effective_environment.as_object()) {
            assign_environment(&result.process.environment, item.name, item.value.as_string(), windows);
        }
        for (const auto& assignment : command->environment) {
            assign_environment(&result.process.environment, assignment.first, assignment.second, windows);
        }
        assign_environment(&result.process.environment, "CODEX_HOME", selected->profile_path, windows);
        result.profile_storage = request->context.profile_storage;
        result.account_context = request->context.account_context;
        result.inspection.protocol_state = quota ? "await_initialize" : "await_exit";
        if (quota) {
            result.inspection.incremental = true;
            result.inspection.keep_stdin_open = true;
            result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
            result.process.stdin_payload = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
                "\"clientInfo\":{\"name\":\"logonomic\",\"version\":\"0\"},\"capabilities\":{\"experimentalApi\":true}}}\n";
        }
        context.succeed(addon::encode_structured_prepare_result(result, 2));
        return;
    }
    if (context.request().operation != addon::k_inspect_operation) { context.reject_unknown_operation(); return; }
    const auto request = addon::decode_probe_inspect_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (!quota) { context.succeed(addon::encode_auth_inspect_result(inspect_auth(*request))); return; }
    std::string reason;
    auto result = inspect_quota(*request, &reason);
    if (!reason.empty()) result.failure_reason = reason;
    context.succeed(addon::encode_quota_inspect_result(result, 2));
}

} // namespace codex
