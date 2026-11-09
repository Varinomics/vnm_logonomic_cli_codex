#include "codex_preparation.h"
#include "codex_jsonl.h"
#include "codex_accounts.h"
#include "codex_conversation.h"
#include "codex_conversation_source.h"
#include "codex_launcher.h"
#include "codex_startup_probe.h"
#include "codex_session_wake.h"
#include "codex_resume_record.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

bool environment_name_equal(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    const auto fold = [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? value + 'a' - 'A' : value;
    };
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [&](char a, char b) { return fold(a) == fold(b); });
#else
    return left == right;
#endif
}

std::string toml_array(const std::vector<std::string>& values)
{
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) {
            result += ',';
        }
        result += toml_string(value);
    }
    return result + ']';
}

bool add_terminal_title(
    std::vector<std::string>& argv, std::size_t configuration_end,
    const std::optional<std::vector<std::string>>& selected)
{
    if (!selected) {
        return false;
    }
    addon::Launch_patch candidate;
    candidate.argv = argv;
    candidate.argv->insert(candidate.argv->begin() + configuration_end,
        {"-c", "tui.terminal_title=" + toml_array(*selected)});
    // Native title lists can exceed the host's argv contract. Optional title
    // evidence must not invalidate a launch, including its prompt or resume ID.
    if (addon::validate_launch_patch(candidate, {}, {}, nullptr)) {
        argv = std::move(*candidate.argv);
        return true;
    }
    return false;
}

std::vector<std::string> hook_arguments()
{
#ifdef _WIN32
    const std::string script = "$p=Join-Path $env:LOGONOMIC_PROVIDER_HELPERS_DIR 'codex_helper.exe';"
        "if($env:LOGONOMIC_SESSION_CAPABILITY -and (Test-Path -LiteralPath $p -PathType Leaf)){& $p};exit 0";
    std::string utf16;
    for (const char character : script) { utf16 += character; utf16 += '\0'; }
    const std::string command = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + addon::encode_base64(utf16);
#else
    const std::string command = "sh -c 'if [ -n \"$LOGONOMIC_SESSION_CAPABILITY\" ] && [ -x \"$LOGONOMIC_PROVIDER_HELPERS_DIR/codex_helper\" ]; then \"$LOGONOMIC_PROVIDER_HELPERS_DIR/codex_helper\"; fi; exit 0'";
#endif
    std::vector<std::string> result;
    for (const std::string name : {
            "SessionStart", "SessionEnd", "UserPromptSubmit", "Stop", "Interrupt", "PermissionRequest",
            "SubagentStart", "SubagentStop", "PostCompact"})
    {
        result.push_back("-c");
        result.push_back("hooks." + name + "=[{hooks=[{type=\"command\",command=" + toml_string(command) +
            ",timeout=" + (name == "SessionEnd" || name == "Interrupt" ? "3" : "10") +
            (name == "SessionStart" || name == "UserPromptSubmit" ? ",additionalContextLimit=12000" : "") + "}]}]");
    }
    result.push_back("-c");
    result.push_back("hooks.PreToolUse=[{hooks=[{type=\"command\",command=" + toml_string(command) +
        ",timeout=10,additionalContextLimit=12000}]}]");
    result.push_back("-c");
    result.push_back("hooks.PostToolUse=[{matcher=\"^request_user_input(_async)?$\",hooks=[{type=\"command\",command=" +
        toml_string(command) + ",timeout=10}]}]");
    return result;
}

// Codex's own test: the directory or an ancestor holds a `.git` entry.
bool inside_git_repository(std::filesystem::path directory)
{
    for (;;) {
        std::error_code error;
        if (std::filesystem::exists(directory / ".git", error)) {
            return true;
        }
        if (!directory.has_relative_path()) {
            return false;
        }
        directory = directory.parent_path();
    }
}

bool add_hooks(Verified_launch& launch, addon::Invocation_context& context)
{
    if (hooks_configuration_conflicts(launch.argv, launch.prefix_size)) {
        context.fail(addon::Result_status::UNAVAILABLE, "hooks_configuration_conflict",
            "Codex hooks configuration conflicts with Logonomic-owned hooks.");
        return false;
    }
    const auto hooks = hook_arguments();
    launch.argv.insert(launch.argv.begin() + launch.prefix_size, hooks.begin(), hooks.end());
    return true;
}

// The environment a prepared Codex process starts with: the host's effective
// environment, the verified launch's updates, and the selected profile.
std::vector<std::pair<std::string, std::string>> launch_environment(
    const addon::Json_value& effective_environment, const Verified_launch& launch, const std::string& codex_home)
{
    std::vector<std::pair<std::string, std::string>> environment;
    for (const auto& member : effective_environment.as_object()) {
        environment.emplace_back(member.name, member.value.as_string());
    }
    for (const auto& update : launch.environment) {
        std::erase_if(environment, [&](const auto& entry) { return entry.first == update.first; });
        environment.push_back(update);
    }
    std::erase_if(environment, [](const auto& entry) {
        std::string name = entry.first;
#ifdef _WIN32
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : char(c); });
#endif
        return name == "CODEX_HOME";
    });
    environment.emplace_back("CODEX_HOME", codex_home);
    return environment;
}

// Detects the positional PROMPT of Codex's interactive session: its help
// states the usage `codex [OPTIONS] [PROMPT]` when its parser takes one.
bool takes_initial_prompt(
    const Verified_launch&                                  launch,
    const std::vector<std::pair<std::string, std::string>>& environment,
    std::string_view                                        working_directory,
    addon::Invocation_context&                              context)
{
    std::vector<std::string> argv(launch.argv.begin(), launch.argv.begin() + launch.prefix_size);
    argv.push_back("--help");
    const auto output = run_probe(argv, environment, working_directory, 3000, context);
    return output && output->output.find("[PROMPT]") != std::string::npos;
}

// Codex submits its positional PROMPT as the first user message of a new
// interactive session. The text can take that place only behind launcher
// arguments that are all options of the session grammar: a subcommand or an
// earlier positional would claim it, and --image attaches files to that first
// message.
bool leaves_prompt_position(const addon::Prepare_request& request)
{
    const auto literal = request.launcher.runner_key == addon::k_runner_direct
        ? request.launcher_argv : addon::literal_argv(request.launcher.script_body);
    return supported_session_arguments(literal, false) &&
        std::none_of(literal.begin() + 1, literal.end(), [](const std::string& argument) {
            const auto option = argument.substr(0, argument.find('='));
            return option == "-i" || option == "--image";
        });
}

void refuse_initial_prompt(addon::Invocation_context& context, std::string detail)
{
    context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable, std::move(detail));
}

void prepare_interactive(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 3);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Session_launch_context account;
    account.launcher           = request->launcher;
    account.account_context    = request->account_context;
    account.launch_environment = request->launch_environment;
    const auto profile = selected_profile(account, context);
    if (!profile) return;
    const auto& prompt = request->initial_prompt;
    // The TUI runs a first message that starts with '!' as a local shell
    // command instead of sending it to the model.
    if (prompt && prompt->text.starts_with('!')) {
        refuse_initial_prompt(context, "Codex runs an initial prompt that starts with '!' as a shell command.");
        return;
    }
    if (prompt && !leaves_prompt_position(*request)) {
        refuse_initial_prompt(context, "The launcher's arguments leave Codex no place for the initial prompt.");
        return;
    }
    auto launch = verified_launch(request->launcher, request->launcher_argv,
        request->launch_environment->effective_environment, request->working_directory, false, context);
    if (!launch) {
        if (prompt && !context.cancel_requested()) {
            refuse_initial_prompt(context, "Codex cannot be prepared from this launcher.");
            return;
        }
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "launcher_program_unavailable");
        return;
    }
    if (prompt && !takes_initial_prompt(*launch,
            launch_environment(request->launch_environment->effective_environment, *launch, profile->profile_path),
            request->working_directory, context))
    {
        if (context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        refuse_initial_prompt(context, "This Codex does not take an initial prompt for an interactive session.");
        return;
    }
    auto native_preparation = prepare_native_launch(*launch,
        launch_environment(request->launch_environment->effective_environment, *launch, profile->profile_path),
        request->working_directory, context, request->terminal_identity.has_value());
    std::optional<addon::Session_wake_preparation> wake;
    if (request->session_wake) {
        wake = std::move(native_preparation.session_wake);
    }
    if (!add_hooks(*launch, context)) return;
    addon::Launch_patch patch;
    patch.set             = launch->environment;
    patch.set.emplace_back("CODEX_HOME", profile->profile_path);
    patch.argv            = launch->argv;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    patch.session_wake    = std::move(wake);
    patch.disposition     = addon::Launch_disposition{true, version_is_at_least(launch->version, 0, 146, 0), true};
    if (request->codex_invocation) {
        patch.codex_invocation = addon::Codex_invocation{launch->prefix_size};
    }
    const auto configuration_end = patch.argv->size();
    if (prompt) {
        // "--" ends option parsing, so the text is the positional PROMPT
        // whatever its first character. The replacement invocation runs the
        // verified program directly, with no shell layer to quote for.
        patch.argv->insert(patch.argv->end(), {"--", prompt->text});
        patch.initial_prompt = addon::Initial_prompt_acknowledgment{
            addon::initial_prompt_text_digest_hex(prompt->text)};
    }
    if (add_terminal_title(*patch.argv, configuration_end, native_preparation.terminal_title)) {
        patch.terminal_identity = std::move(native_preparation.terminal_identity);
    }
    if (prompt && !addon::validate_launch_patch(patch, {}, {}, &failure)) {
        refuse_initial_prompt(context,
            "The launch with the initial prompt is not a valid replacement: " + failure.detail);
        return;
    }
    context.succeed(addon::encode_launch_patch(patch, 3));
}

void prepare_resume(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto record = decode_resume_record(request->resume_record);
    const auto profile = selected_profile(request->context, context);
    if (!profile) return;
    const auto directory = addon::path_from_utf8(request->context.working_directory);
    if (!record ||
        !directory.is_absolute() || directory.lexically_normal() != directory || !request->startup_observer ||
        request->startup_observer->confirmation != "prepared-identity")
    {
        context.fail(addon::Result_status::FAILED, "invalid_resume_request",
            "Exact Codex resume requires a canonical session UUID, workspace and private startup observer.");
        return;
    }
    if (!record->sqlite_home.empty()) {
        if (record->codex_home != profile->profile_path) {
            context.fail(addon::Result_status::UNAVAILABLE, "resume_scope_unavailable");
            return;
        }
        // Inspection's digest accepts only read-only context. Keep preparation
        // offers on the original request for the resumed process to acknowledge.
        addon::Resume_inspect_request inspection_request{request->context, request->resume_record};
        inspection_request.context.declared_assets.clear();
        inspection_request.context.codex_invocation.reset();
        inspection_request.context.session_wake.reset();
        inspection_request.context.terminal_identity.reset();
        const auto inspection = inspect_resume(inspection_request, [&] { return context.cancel_requested(); });
        if (context.cancel_requested() || inspection.availability == addon::Resume_availability::ABSENT) {
            context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
                context.cancel_requested() ? "cancelled" : "resume_source_unavailable");
            return;
        }
        // UNKNOWN must remain explicitly recoverable. Native Codex may support
        // a compressed/referenced source or a live writer the reader cannot
        // inspect. The verified scope and private startup observer still bind
        // the actual launch to this exact ID; no last-session fallback is used.
    }
    const auto literal = request->context.launcher.runner_key == addon::k_runner_direct
        ? request->context.launcher_argv : addon::literal_argv(request->context.launcher.script_body);
    if (!supported_session_arguments(literal, true)) {
        context.fail(addon::Result_status::UNAVAILABLE, "resume_launcher_unsupported");
        return;
    }
    auto launch = verified_launch(request->context.launcher, request->context.launcher_argv,
        request->context.launch_environment->effective_environment, request->context.working_directory, false, context);
    if (!launch || !version_is_at_least(launch->version, 0, 146, 0)) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "resume_launcher_unavailable");
        return;
    }
    if (!record->sqlite_home.empty()) {
        // Resume the saved native scope even if the launcher's current defaults changed.
        launch->argv.insert(launch->argv.end(), {"-c", "sqlite_home=" + toml_string(record->sqlite_home)});
    }
    auto native_preparation = prepare_native_launch(*launch,
        launch_environment(request->context.launch_environment->effective_environment, *launch, profile->profile_path),
        request->context.working_directory, context, request->context.terminal_identity.has_value());
    if (!record->sqlite_home.empty() &&
        addon::environment_value(launch->environment, k_export_sqlite_home_environment) != record->sqlite_home)
    {
        context.fail(addon::Result_status::UNAVAILABLE, "resume_scope_unavailable");
        return;
    }
    std::optional<addon::Session_wake_preparation> wake;
    if (request->context.session_wake) {
        wake = std::move(native_preparation.session_wake);
    }
    if (!add_hooks(*launch, context)) return;
    std::vector<std::string> configuration;
    const std::vector<std::string> options{"-c", "--config", "-p", "--profile", "--enable", "--disable"};
    for (std::size_t index = launch->prefix_size; index < launch->argv.size(); ++index) {
        const auto& argument = launch->argv[index];
        const auto equal = argument.find('=');
        if (std::find(options.begin(), options.end(), argument.substr(0, equal)) != options.end()) {
            configuration.push_back(argument);
            if (equal == std::string::npos) configuration.push_back(launch->argv[++index]);
        }
        else if (argument.starts_with("-c") && !argument.starts_with("--")) configuration.push_back(argument);
    }
    const auto environment = launch_environment(
        request->context.launch_environment->effective_environment, *launch, profile->profile_path);
    std::string error;
    if (!startup_supported(*launch, configuration, environment, request->context.working_directory, context, &error)) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "startup_observer_unavailable", error);
        return;
    }
    const auto configuration_end = launch->argv.size();
    launch->argv.insert(launch->argv.end(), {"-c", "otel.exporter={otlp-http={endpoint=" +
        toml_string(request->startup_observer->endpoint) + ",protocol=\"json\"}}", "-c", "otel.log_user_prompt=false",
        "resume", "-C", request->context.working_directory, record->session_id});
    const bool title_applied = add_terminal_title(launch->argv, configuration_end, native_preparation.terminal_title);
    addon::Resume_prepare_result result;
#ifdef _WIN32
    result.process.executable = "helpers/codex_launch_bridge.exe";
#else
    result.process.executable = "helpers/codex_launch_bridge";
#endif
    result.process.argv           = launch->argv;
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.set = launch->environment;
    result.patch.set.emplace_back("CODEX_HOME", profile->profile_path);
    std::string authorization = request->startup_observer->authorization;
    std::string encoded;
    for (const char character : authorization) encoded += character == ' ' ? "%20" : std::string(1, character);
    result.patch.set.emplace_back("OTEL_EXPORTER_OTLP_LOGS_HEADERS", "Authorization=" + encoded);
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.patch.session_wake = std::move(wake);
    if (title_applied) {
        result.patch.terminal_identity = std::move(native_preparation.terminal_identity);
    }
    if (request->context.codex_invocation) {
        // The terminal launches the bridge ahead of this verified invocation.
        // Its prefix belongs with Codex's executable when inserting CLI options.
        result.patch.codex_invocation = addon::Codex_invocation{1 + launch->prefix_size};
    }
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    result.startup_observer = request->startup_observer;
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!request->control.connection) {
        context.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
        return;
    }
    const auto profile = selected_profile(request->context, context);
    if (!profile) return;
    if (request->purpose != addon::Structured_purpose::IMPLEMENTATION) {
        context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
        return;
    }
    if (request->prompt.empty() || !addon::path_from_utf8(request->context.working_directory).is_absolute()) {
        context.fail(addon::Result_status::FAILED, "invalid_implementation_request");
        return;
    }
    const auto literal = request->context.launcher.runner_key == addon::k_runner_direct
        ? request->context.launcher_argv : addon::literal_argv(request->context.launcher.script_body);
    std::string approval;
    if (!supported_session_arguments(literal, false, &approval)) {
        context.fail(addon::Result_status::UNAVAILABLE, "delegated_launcher_options_unavailable");
        return;
    }
    if (!approval.empty() && approval != "never") {
        context.fail(addon::Result_status::UNAVAILABLE, "delegated_launcher_options_unavailable");
        return;
    }
    const auto launch = verified_launch(request->context.launcher, request->context.launcher_argv,
        request->context.launch_environment->effective_environment, request->context.working_directory, true, context);
    if (!launch) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "launcher_program_unavailable");
        return;
    }
    // `codex exec` refuses a directory outside a Git repository unless the
    // launcher's --yolo lifts its sandbox; its own message reaches only stderr.
    if (std::none_of(launch->argv.begin() + static_cast<std::ptrdiff_t>(launch->prefix_size), launch->argv.end(),
            [](const auto& argument) {
                return argument == "--yolo" || argument == "--dangerously-bypass-approvals-and-sandbox";
            }) &&
        !inside_git_repository(addon::path_from_utf8(request->context.working_directory)))
    {
        context.fail(addon::Result_status::UNAVAILABLE, "untrusted_working_directory",
            "Codex runs a non-interactive task only inside a Git repository. Delegate the task into a Git "
            "working directory; a launcher that passes --yolo also lifts this check, and with it Codex's "
            "sandbox and approvals.");
        return;
    }
    addon::Delegated_prepare_result result;
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
#ifdef _WIN32
    result.process.executable = "helpers/codex_launch_bridge.exe";
#else
    result.process.executable = "helpers/codex_launch_bridge";
#endif
    // Keep the launcher's options verbatim. Native exec cannot ask for human
    // approval; an explicit policy requiring it was refused above.
    // The package's only grant is approval of its own Task tools below.
    result.process.argv = launch->argv;
    result.process.argv.insert(result.process.argv.end(), {"exec", "--json", "--ephemeral"});
    if (approval == "never") {
        // Exec does not inherit the TUI's -a flag. Its default is never, but
        // automatic review can restore a configured policy. Preserve the
        // explicit flag's precedence over that configuration as well.
        result.process.argv.insert(result.process.argv.end(), {"-c", "approval_policy=\"never\""});
    }
    for (const auto& server : request->control.servers) {
        const auto nonce = addon::random_nonce_hex();
        const std::string key = "mcp_servers.logonomic_" + nonce + ".";
        const auto& connection = *request->control.connection;
        const std::vector<std::string> arguments{
            "--descriptor", connection.descriptor_path, "--server", server.server_id};
        result.acknowledgment.server_ids.push_back(server.server_id);
        const auto overlay = [&](std::string value) {
            result.process.argv.push_back("-c");
            result.process.argv.push_back(key + std::move(value));
        };
        overlay("command=" + toml_string(connection.relay_program));
        overlay("args=" + toml_array(arguments));
        overlay("required=true");
        overlay("enabled_tools=" + toml_array(server.actions));
        overlay("default_tools_approval_mode=\"approve\"");
    }
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    result.process.argv.push_back("-");
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload  = request->prompt;
    result.process.environment    = launch->environment;
    result.process.environment.emplace_back("CODEX_HOME", profile->profile_path);
    result.process.environment_names = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "SystemRoot", "WINDIR", "TEMP", "TMP", "CODEX_HOME", "OPENAI_API_KEY", "OPENAI_BASE_URL", "NODE_OPTIONS", "NODE_PATH", "OTEL_EXPORTER_OTLP_ENDPOINT", "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "OTEL_EXPORTER_OTLP_HEADERS"};
    std::erase_if(result.process.environment_names, [&](const auto& name) {
        return std::any_of(result.process.environment.begin(), result.process.environment.end(),
            [&](const auto& entry) { return environment_name_equal(name, entry.first); });
    });
    result.inspection.incremental    = true;
    result.inspection.protocol_state = initial_implementation_state();
    auto payload = addon::encode_delegated_prepare_result(result);
    if (!addon::validate_session_exchange_payload(payload, &failure)) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(std::move(payload));
}

} // namespace

std::string toml_string(std::string_view value)
{
    std::string result = "\"";
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\r': result += "\\r";  break;
        case '\n': result += "\\n";  break;
        case '\t': result += "\\t";  break;
        default:
            if (character < 0x20 || character == 0x7f) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[character >> 4];
                result += hex[character & 0x0f];
            }
            else {
                result += static_cast<char>(character);
            }
        }
    }
    return result + '"';
}

bool hooks_configuration_conflicts(const std::vector<std::string>& argv, std::size_t prefix_size)
{
    for (std::size_t index = prefix_size; index < argv.size(); ++index) {
        const auto& argument = argv[index];
        if (argument == "--") {
            break;
        }
        std::string configuration;
        if (argument == "-c" || argument == "--config") {
            if (++index == argv.size()) {
                break;
            }
            configuration = argv[index];
        }
        else
        if (argument.starts_with("--config=")) {
            configuration = argument.substr(9);
        }
        else
        if (argument.starts_with("-c") && !argument.starts_with("--") && argument.size() > 2) {
            configuration = argument.substr(2);
        }
        else {
            continue;
        }
        // Decode the first TOML dotted-key component, including quoted spellings.
        const auto first = configuration.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        std::string component;
        std::size_t cursor = first;
        if (configuration[cursor] == '\'' || configuration[cursor] == '"') {
            const char quote = configuration[cursor++];
            const auto begin = cursor;
            bool escape = false;
            while (cursor < configuration.size()) {
                const char character = configuration[cursor];
                if (!escape && character == quote) {
                    break;
                }
                if (quote == '"' && !escape && character == '\\') {
                    escape = true;
                }
                else {
                    escape = false;
                }
                ++cursor;
            }
            if (cursor == configuration.size()) {
                continue;
            }
            if (quote == '\'') {
                component = configuration.substr(begin, cursor - begin);
            }
            else {
                std::string quoted = configuration.substr(begin - 1, cursor - begin + 2);
                for (std::size_t at = 1; at + 1 < quoted.size(); ++at) {
                    if (quoted[at] != '\\') continue;
                    if (quoted[at + 1] != 'U') { ++at; continue; }
                    if (at + 10 > quoted.size() - 1) { quoted.clear(); break; }
                    std::uint32_t scalar = 0;
                    const auto parsed = std::from_chars(quoted.data() + at + 2, quoted.data() + at + 10, scalar, 16);
                    if (parsed.ec != std::errc{} || parsed.ptr != quoted.data() + at + 10 || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) { quoted.clear(); break; }
                    std::string utf8;
                    if (scalar < 0x80) utf8 += static_cast<char>(scalar);
                    else if (scalar < 0x800) { utf8 += char(0xc0 | (scalar >> 6)); utf8 += char(0x80 | (scalar & 0x3f)); }
                    else if (scalar < 0x10000) { utf8 += char(0xe0 | (scalar >> 12)); utf8 += char(0x80 | ((scalar >> 6) & 0x3f)); utf8 += char(0x80 | (scalar & 0x3f)); }
                    else { utf8 += char(0xf0 | (scalar >> 18)); utf8 += char(0x80 | ((scalar >> 12) & 0x3f)); utf8 += char(0x80 | ((scalar >> 6) & 0x3f)); utf8 += char(0x80 | (scalar & 0x3f)); }
                    const std::string encoded = addon::serialize_json(addon::Json_value(utf8));
                    quoted.replace(at, 10, encoded.substr(1, encoded.size() - 2));
                    at += encoded.size() - 3;
                }
                const auto decoded = addon::parse_json(quoted, {}, nullptr);
                if (decoded && decoded->is_string()) {
                    component = decoded->as_string();
                }
            }
            ++cursor;
        }
        else {
            const auto end = configuration.find_first_of(". =\t", cursor);
            component = configuration.substr(cursor, end - cursor);
            cursor = end;
        }
        if (component == "hooks" && cursor != std::string::npos) {
            const auto next = configuration.find_first_not_of(" \t", cursor);
            if (next != std::string::npos && (configuration[next] == '.' || configuration[next] == '=')) {
                return true;
            }
        }
    }
    return false;
}

void handle_preparation(addon::Invocation_context& context)
{
    if (context.request().capability.capability_id == "interactive-resume" &&
        context.request().operation == addon::k_inspect_resume_operation)
    {
        addon::Contract_failure failure;
        const auto request = addon::decode_resume_inspect_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto result = inspect_resume(*request, [&] { return context.cancel_requested(); });
        if (context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        context.succeed(addon::encode_resume_inspect_result(result, context.request().capability.version));
        return;
    }
    if (context.request().capability.capability_id == "interactive-resume" && context.request().operation == addon::k_prepare_resume_operation) {
        prepare_resume(context);
        return;
    }
    if (context.request().operation != addon::k_prepare_operation) {
        context.reject_unknown_operation();
        return;
    }
    if (context.request().capability.capability_id == "delegated-session") {
        prepare_delegated(context);
        return;
    }
    if (context.request().capability.capability_id == "launch-preparation") {
        prepare_interactive(context);
        return;
    }
    context.fail(addon::Result_status::UNAVAILABLE, "delegated_control_required");
}

} // namespace codex
