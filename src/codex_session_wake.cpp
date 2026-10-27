#include "codex_session_wake.h"
#include "codex_conversation_source.h"
#include "codex_preparation.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string_view>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;
using Environment = std::vector<std::pair<std::string, std::string>>;

Json object(std::initializer_list<std::pair<std::string, Json>> members)
{
    auto result = Json::object();
    for (const auto& [name, value] : members) result.set(name, value);
    return result;
}

std::string text(const Json& object, const char* key)
{
    const auto* value = object.find(key);
    return value && value->is_string() ? value->as_string() : std::string{};
}

bool same_environment_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    const auto fold = [](unsigned char c) { return c >= 'a' && c <= 'z' ? c - 32 : c; };
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [&](char a, char b) { return fold(a) == fold(b); });
#else
    return left == right;
#endif
}

std::string environment_value(const Environment& environment, std::string_view name)
{
    for (const auto& [key, value] : environment) if (same_environment_name(key, name)) return value;
    return {};
}

bool canonical_uuid(const std::string& id)
{
    static const std::regex expression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return std::regex_match(id, expression);
}

std::string normalized_absolute(std::string_view name)
{
    const auto path = addon::path_from_utf8(name);
    if (!path.is_absolute()) return {};
    const auto value = path.lexically_normal().generic_u8string();
    return {value.begin(), value.end()};
}

std::optional<Json> file_evidence(const std::vector<std::string>& files)
{
    if (files.empty()) return std::nullopt;
    auto result = Json::array();
    for (const auto& name : files) {
        const auto path = addon::path_from_utf8(name);
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error) return std::nullopt;
        const auto modified = fs::last_write_time(path, error);
        if (error) return std::nullopt;
        result.push_back(object({{"path", Json(name)}, {"size", Json(std::to_string(size))},
            {"modified", Json(addon::file_time_ticks_text(modified))}}));
    }
    return result;
}

struct Rpc_response
{
    std::optional<Json> result;
    bool request_sent = false;
};

// This control process never loads a thread. Queue/list and queue/add operate
// on persisted metadata; the original process observes the native queue's
// revision and starts its own next turn. Direct stdio deliberately bypasses
// `codex queue`'s default-daemon discovery, whose store may differ from the TUI.
Rpc_response call(
    std::vector<std::string> argv, const Environment& environment, const std::string& directory,
    const std::string& method, const Json& parameters, addon::Invocation_context& context,
    const std::string& expected_store = {})
{
    argv.push_back("app-server");
    Rpc_response response;
    addon::Probe_exchange exchange;
    exchange.initial_input = addon::serialize_json(object({{"id", Json(1)}, {"method", Json("initialize")},
        {"params", object({{"clientInfo", object({{"name", Json("logonomic-session-wake")}, {"version", Json("1")}})},
            {"capabilities", object({{"experimentalApi", Json(true)}})}})}})) + '\n';
    std::size_t consumed = 0;
    bool initialized = false;
    const int operation_id = expected_store.empty() ? 2 : 3;
    exchange.on_output = [&](std::string_view bytes) -> std::optional<std::string> {
        std::string input;
        for (;;) {
            const auto end = bytes.find('\n', consumed);
            if (end == std::string_view::npos) break;
            const auto message = addon::parse_json(bytes.substr(consumed, end - consumed), {}, nullptr);
            consumed = end + 1;
            if (!message || !message->is_object()) return std::nullopt;
            const auto* id = message->find("id");
            if (!id || !id->is_integer()) continue;
            if (message->find("error")) return std::nullopt;
            const auto* result = message->find("result");
            if (!result || !result->is_object()) return std::nullopt;
            if (id->as_integer() == 1 && !initialized) {
                initialized = true;
                input += "{\"method\":\"initialized\"}\n";
                if (!expected_store.empty()) {
                    input += addon::serialize_json(object({{"id", Json(2)}, {"method", Json("config/read")},
                        {"params", object({{"cwd", Json(directory)}, {"includeLayers", Json(false)}})}})) + '\n';
                }
                else {
                    response.request_sent = true;
                    input += addon::serialize_json(object({{"id", Json(2)}, {"method", Json(method)}, {"params", parameters}})) + '\n';
                }
            }
            else
            if (id->as_integer() == 2 && initialized && !expected_store.empty() && !response.request_sent) {
                const auto* config = result->find("config");
                const auto* backend = config ? config->find("experimental_thread_store") : nullptr;
                // Managed native requirements outrank even -c. Verify in the
                // very process which will enqueue, before giving it any input.
                if (!config || normalized_absolute(text(*config, "sqlite_home")) != expected_store ||
                    (backend && !backend->is_null()))
                {
                    exchange.complete = true;
                    return input;
                }
                // Once handed to the pipe writer, failure cannot prove that
                // the mutating request was not accepted. Never retry it.
                response.request_sent = true;
                input += addon::serialize_json(object({{"id", Json(operation_id)}, {"method", Json(method)}, {"params", parameters}})) + '\n';
            }
            else
            if (id->as_integer() == operation_id && initialized && response.request_sent && !response.result) {
                response.result = *result;
                exchange.complete = true;
            }
            else return std::nullopt;
        }
        return input;
    };
    if (!run_probe(argv, environment, directory, 10000, context, &exchange) || !exchange.complete) response.result.reset();
    return response;
}

std::optional<std::vector<std::string>> configuration_arguments(const Verified_launch& launch)
{
    std::vector<std::string> literal{"codex"};
    literal.insert(literal.end(), launch.argv.begin() + launch.prefix_size, launch.argv.end());
    if (!supported_session_arguments(literal, false)) return std::nullopt;
    std::vector<std::string> result;
    for (std::size_t index = 1; index < literal.size(); ++index) {
        const auto& argument = literal[index];
        const auto equal = argument.find('=');
        const auto option = argument.substr(0, equal);
        // Native app-server rejects the TUI's v2 profile selector. Dropping it
        // would inspect another configuration, so keep this launch unavailable.
        if (option == "-p" || option == "--profile") return std::nullopt;
        if (option == "-c" || option == "--config" || option == "-p" || option == "--profile" ||
            option == "--enable" || option == "--disable")
        {
            result.push_back(argument);
            if (equal == std::string::npos) result.push_back(literal[++index]);
        }
        else
        if (argument.starts_with("-c") && !argument.starts_with("--")) result.push_back(argument);
        else
        if (equal == std::string::npos && (option == "-m" || option == "--model" || option == "--local-provider" ||
            option == "-i" || option == "--image" || option == "--add-dir")) ++index;
    }
    return result;
}

struct Bound_queue
{
    std::vector<std::string> argv;
    Environment environment;
    std::string sqlite_home;
};

constexpr std::string_view k_notification_prefix = "logonomic-wake:";

std::optional<bool> notification_is_queued(
    const Bound_queue& bound,
    const addon::Session_wake_target& target,
    addon::Invocation_context& context)
{
    auto parameters = object({{"threadId", Json(target.provider_session_id)}, {"limit", Json(100)}});
    std::set<std::string> cursors;
    while (!context.cancel_requested()) {
        const auto response = call(bound.argv, bound.environment, target.working_directory,
            "thread/queue/list", parameters, context, bound.sqlite_home);
        const auto* data = response.result ? response.result->find("data") : nullptr;
        if (!data || !data->is_array()) {
            return std::nullopt;
        }
        for (const auto& submission : data->as_array()) {
            const auto* client_id = submission.find("clientUserMessageId");
            if (!submission.is_object() || !client_id || !client_id->is_string() ||
                !canonical_uuid(text(submission, "id")))
            {
                return std::nullopt;
            }
            if (client_id->as_string().starts_with(k_notification_prefix)) {
                return true;
            }
        }
        const auto* cursor = response.result->find("nextCursor");
        if (!cursor || cursor->is_null()) {
            return false;
        }
        if (!cursor->is_string() || cursor->as_string().empty() || !cursors.insert(cursor->as_string()).second) {
            return std::nullopt;
        }
        parameters.set("cursor", *cursor);
    }
    return std::nullopt;
}

std::optional<Bound_queue> bound_queue(const addon::Session_wake_target& target)
{
    const auto& binding = target.binding;
    const auto* schema = binding.find("schema");
    const auto* prefix = binding.find("prefix");
    const auto* evidence = binding.find("files");
    if (!canonical_uuid(target.provider_session_id) || !schema || *schema != Json(1) ||
        !version_is_at_least(text(binding, "version"), 0, 149, 0) ||
        text(binding, "cwd") != target.working_directory ||
        !prefix || !prefix->is_array() || prefix->as_array().empty() ||
        !evidence || !evidence->is_array() || evidence->as_array().empty()) return std::nullopt;
    std::vector<std::string> files;
    for (const auto& file : evidence->as_array()) {
        const auto name = text(file, "path");
        if (normalized_absolute(name).empty()) return std::nullopt;
        files.push_back(name);
    }
    const auto current = file_evidence(files);
    // The host's JSON object/map bridge can reorder members without changing
    // evidence. Compare every field and array entry with object order removed.
    if (!current || addon::serialize_json_canonical(*current) != addon::serialize_json_canonical(*evidence))
        return std::nullopt;
    const auto home = text(binding, "codex_home");
    const auto sqlite_home = text(binding, "sqlite_home");
    if (normalized_absolute(home).empty() || normalized_absolute(sqlite_home).empty()) return std::nullopt;
    Bound_queue result;
    result.sqlite_home = sqlite_home;
    for (const auto& argument : prefix->as_array()) {
        if (!argument.is_string() || argument.as_string().empty() || argument.as_string().find('\0') != std::string::npos)
            return std::nullopt;
        result.argv.push_back(argument.as_string());
    }
    if (normalized_absolute(result.argv.front()).empty()) return std::nullopt;
    result.argv.insert(result.argv.end(), {"-c", "sqlite_home=" + toml_string(sqlite_home)});
    for (const auto& member : target.launch_environment.effective_environment.as_object()) {
        if (!same_environment_name(member.name, "CODEX_HOME")) result.environment.emplace_back(member.name, member.value.as_string());
    }
    result.environment.emplace_back("CODEX_HOME", home);
    return result;
}

} // namespace

addon::Session_wake_preparation prepare_session_wake(
    Verified_launch& launch, const Environment& environment,
    const std::string& working_directory, addon::Invocation_context& context)
{
    // Clear an inherited marker even when optional native configuration fails.
    // Only this launch's resolved and pinned state store may authorize export.
    std::erase_if(launch.environment, [](const auto& entry) {
        return same_environment_name(entry.first, k_export_sqlite_home_environment);
    });
    launch.environment.emplace_back(k_export_sqlite_home_environment, "");
    const auto unavailable = [](std::string reason) {
        return addon::Session_wake_preparation{false, Json::object(), std::move(reason)};
    };
    // 0.149 introduced both the writer and the external queue watcher. A new
    // executable found later cannot retroactively add the watcher to an old TUI.
    if (!version_is_at_least(launch.version, 0, 149, 0)) return unavailable("native_queue_watcher_unavailable");
    const auto configuration = configuration_arguments(launch);
    if (!configuration) return unavailable("wake_launcher_unsupported");
    const auto home = normalized_absolute(environment_value(environment, "CODEX_HOME"));
    if (home.empty()) return unavailable("wake_profile_unavailable");
    const auto evidence = file_evidence(launch.files);
    if (!evidence) return unavailable("wake_executable_unavailable");
    std::vector<std::string> argv(launch.argv.begin(), launch.argv.begin() + launch.prefix_size);
    argv.insert(argv.end(), configuration->begin(), configuration->end());
    const auto response = call(argv, environment, working_directory, "config/read",
        object({{"cwd", Json(working_directory)}, {"includeLayers", Json(false)}}), context);
    const auto* config = response.result ? response.result->find("config") : nullptr;
    if (!config || !config->is_object()) return unavailable("wake_configuration_unavailable");
    // A nondefault persistence backend needs its own native queue contract.
    if (const auto* store = config->find("experimental_thread_store"); store && !store->is_null())
        return unavailable("wake_store_unsupported");
    std::string sqlite_home;
    if (const auto* configured = config->find("sqlite_home"); configured && !configured->is_null()) {
        if (!configured->is_string()) return unavailable("wake_store_unsupported");
        sqlite_home = normalized_absolute(configured->as_string());
    }
    else {
        auto value = environment_value(environment, "CODEX_SQLITE_HOME");
        const auto begin = value.find_first_not_of(" \t\r\n");
        value = begin == std::string::npos ? std::string{} : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
        auto path = value.empty() ? addon::path_from_utf8(home) : addon::path_from_utf8(value);
        if (!path.is_absolute()) path = addon::path_from_utf8(working_directory) / path;
        const auto name = path.lexically_normal().generic_u8string();
        sqlite_home = normalized_absolute(std::string(name.begin(), name.end()));
    }
    if (sqlite_home.empty()) return unavailable("wake_store_unsupported");
    const auto after = file_evidence(launch.files);
    if (!after || *after != *evidence) return unavailable("wake_executable_changed");
    auto prefix = Json::array();
    for (std::size_t index = 0; index < launch.prefix_size; ++index) prefix.push_back(Json(launch.argv[index]));
    auto binding = object({{"schema", Json(1)}, {"version", Json(launch.version)}, {"prefix", std::move(prefix)},
        {"files", *evidence}, {"cwd", Json(working_directory)}, {"codex_home", Json(home)}, {"sqlite_home", Json(sqlite_home)}});
    if (addon::serialize_json_canonical(binding).size() > addon::k_max_session_wake_binding_bytes)
        return unavailable("wake_binding_too_large");
    // Pin the recipient too: later edits to config.toml cannot move the queue
    // writer away from the process-scoped store the recipient opened at birth.
    launch.argv.insert(launch.argv.end(), {"-c", "sqlite_home=" + toml_string(sqlite_home)});
    for (auto& [name, value] : launch.environment) {
        if (same_environment_name(name, k_export_sqlite_home_environment)) {
            value = sqlite_home;
        }
    }
    return {true, std::move(binding), {}};
}

void handle_session_wake(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_session_wake_inspect_operation) {
        const auto request = addon::decode_session_wake_inspect_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        const auto bound = bound_queue(request->target);
        addon::Session_wake_readiness readiness{false, "wake_binding_unavailable"};
        if (bound) {
            const auto pending = notification_is_queued(*bound, request->target, context);
            readiness = pending
                ? addon::Session_wake_readiness{!*pending, *pending ? "native_notification_pending" : ""}
                : addon::Session_wake_readiness{false, "native_queue_unavailable"};
        }
        context.succeed(addon::encode_session_wake_readiness(readiness));
        return;
    }
    if (context.request().operation != addon::k_session_wake_notify_operation) {
        context.fail(addon::Result_status::UNAVAILABLE, "unknown_operation");
        return;
    }
    const auto request = addon::decode_session_wake_notify_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    const auto bound = bound_queue(request->target);
    addon::Session_wake_notify_result result{addon::Session_wake_disposition::UNAVAILABLE, "wake_binding_unavailable", std::nullopt};
    if (bound) {
        // Mailbox reads cannot retract a prompt already retained by the native queue.
        // Check the queue again at submission, independently of earlier readiness.
        const auto pending = notification_is_queued(*bound, request->target, context);
        if (!pending || *pending) {
            result.disposition = pending ? addon::Session_wake_disposition::DEFERRED
                                         : addon::Session_wake_disposition::UNAVAILABLE;
            result.reason = pending ? "native_notification_pending" : "native_queue_unavailable";
            context.succeed(addon::encode_session_wake_notify_result(result));
            return;
        }
        const auto client_id = std::string(k_notification_prefix) + request->notification_id;
        const auto input = Json(Json::Array{object({{"type", Json("text")}, {"text", Json(request->instruction)}, {"text_elements", Json::array()}})});
        const auto response = call(bound->argv, bound->environment, request->target.working_directory, "thread/queue/add",
            object({{"threadId", Json(request->target.provider_session_id)}, {"input", input},
                {"clientUserMessageId", Json(client_id)}}), context, bound->sqlite_home);
        result.reason = response.request_sent ? "native_queue_acceptance_unknown" : "native_queue_unavailable";
        result.disposition = response.request_sent ? addon::Session_wake_disposition::UNCERTAIN : addon::Session_wake_disposition::UNAVAILABLE;
        const auto* submission = response.result ? response.result->find("queuedSubmission") : nullptr;
        const auto* echo = submission ? submission->find("input") : nullptr;
        if (submission && canonical_uuid(text(*submission, "id")) &&
            text(*submission, "clientUserMessageId") == client_id && echo && *echo == input)
        {
            result = {addon::Session_wake_disposition::ACCEPTED, {}, text(*submission, "id")};
        }
    }
    context.succeed(addon::encode_session_wake_notify_result(result));
}

} // namespace codex
