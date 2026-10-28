#include "codex_accounts.h"
#include "codex_conversation_source.h"
#include "codex_launcher.h"
#include "codex_preparation.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool succeeded = false;
    bool done = false;
    std::string failure_detail;
    std::optional<std::chrono::steady_clock::time_point> cancel_at;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return cancel_at && std::chrono::steady_clock::now() >= *cancel_at; }
    bool wait_for_cancel(int timeout) override { std::this_thread::sleep_for(std::chrono::milliseconds(timeout)); return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string detail) override { done = true; failure_detail = std::move(detail); output = addon::Json_value(std::move(reason)); }
    bool finished() const override { return done; }
};

void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
std::string utf8(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }

// One launch-preparation@3 prepare through the production dispatch.
void prepare_launch(Context& context, const addon::Prepare_request& request)
{
    context.input = {"interactive", {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3)};
    codex::handle_preparation(context);
}

void native_home_selection(const fs::path& executable, const fs::path& root)
{
    // Oracle: Codex find_codex_home -> dirs 6.0.0. Windows uses the OS profile;
    // Unix uses nonempty HOME, then the frozen native home fallback.
    const auto fallback = root / "native-home";
    const auto environment_home = root / "environment-home";
    for (const auto& home : {fallback, environment_home}) {
        fs::create_directories(home / ".codex");
        std::ofstream(home / ".codex/auth.json") << "{}";
    }
    for (const auto* platform : {"windows-x86_64", "linux-x86_64", "macos-arm64"}) {
        for (int home_state = 0; home_state < 3; ++home_state) {
            for (int codex_state = 0; codex_state < 3; ++codex_state) {
                addon::Accounts_launcher_request request;
                request.launcher.script_body = "codex";
                request.launcher.runner_key  = addon::k_runner_direct;
                request.launch_environment.platform_id = platform;
                auto& environment = request.launch_environment.effective_environment;
                environment.set("PATH", addon::Json_value(utf8(executable.parent_path())));
                if (home_state != 0) {
                    const auto value = home_state == 1 ? std::string{} : utf8(environment_home);
                    environment.set("HOME", addon::Json_value(value));
                    environment.set("USERPROFILE", addon::Json_value(home_state == 1
                        ? std::string{} : utf8(root / "userprofile-home")));
                }
                if (codex_state != 0) {
                    environment.set("CODEX_HOME", addon::Json_value(
                        codex_state == 1 ? std::string{} : utf8(environment_home / ".codex")));
                }
                request.default_home_fallback   = utf8(fallback);
                request.normalization_directory = utf8(root);
                const bool overridden = codex_state == 2 ||
                    (std::string_view(platform) != "windows-x86_64" && home_state == 2);
                const auto expected = overridden
                    ? environment_home / ".codex" : fallback / ".codex";
                const auto frozen = addon::serialize_json(environment);
                std::string error;
                const auto selected = codex::profile_selection(request.launch_environment,
                    request.launcher, request.default_home_fallback, request.normalization_directory, &error);
                require(selected && addon::path_from_utf8(selected->profile_path) == expected,
                    ("Native home selection failed: platform=" + std::string(platform) +
                        " home_state=" + std::to_string(home_state) +
                        " codex_state=" + std::to_string(codex_state)).c_str());

                auto selected_request = request;
                selected_request.launch_environment.effective_environment.set("CODEX_HOME",
                    addon::Json_value(selected->profile_path));
                Context accounts;
                accounts.input.operation = addon::k_accounts_discover_operation;
                accounts.input.payload   = addon::encode_accounts_launcher_request(selected_request);
                codex::handle_accounts(accounts);
                addon::Contract_failure failure;
                const auto inventory = addon::decode_accounts_result(accounts.output, &failure);
                require(accounts.succeeded && inventory && inventory->size() == 1 &&
                    inventory->front().profile_path == selected->profile_path,
                    "Account discovery must retain the selected native profile");

                addon::Discover_request discovery;
                discovery.platform_id           = platform;
                discovery.ambient_environment   = selected_request.launch_environment.effective_environment;
                discovery.default_home_fallback = request.default_home_fallback;
                const auto launchers = codex::discover_launchers(discovery);
                require(launchers.size() == 1 && launchers.front().is_default &&
                    launchers.front().environment_text.starts_with("CODEX_HOME=") &&
                    addon::path_from_utf8(launchers.front().environment_text.substr(11)) == expected,
                    "Launcher discovery and account resolution must select the same native home");
                require(addon::serialize_json(environment) == frozen,
                    "Home selection must preserve absent and present empty environment evidence");

                if (std::string_view(platform) != addon::host_platform_id()) {
                    continue;
                }
                addon::Prepare_request prepare;
                prepare.launcher          = request.launcher;
                prepare.launcher_argv     = {utf8(executable)};
                prepare.working_directory = utf8(root);
                prepare.launch_environment = request.launch_environment;
                prepare.account_context = addon::Account_context{2,
                    selected->adapter_account_key, selected->profile_path};
                const auto log = root / ("prepare-" + std::to_string(home_state) +
                    '-' + std::to_string(codex_state) + ".jsonl");
                auto& prepared_environment = prepare.launch_environment->effective_environment;
                prepared_environment.set("CODEX_FIXTURE_WAKE_LOG", addon::Json_value(utf8(log)));
                prepared_environment.set("TEMP", addon::Json_value(utf8(root)));
                if (const char* system_root = std::getenv("SystemRoot")) {
                    prepared_environment.set("SystemRoot", addon::Json_value(system_root));
                }
                Context prepared;
                prepare_launch(prepared, prepare);
                const auto patch = addon::decode_launch_patch(prepared.output, &failure, 3);
                require(prepared.succeeded && patch && patch->account_context &&
                    patch->account_context->version == prepare.account_context->version &&
                    patch->account_context->adapter_account_key == selected->adapter_account_key &&
                    patch->account_context->profile_path == selected->profile_path,
                    "Preparation must freeze the resolved account context");
                require(std::count(patch->set.begin(), patch->set.end(),
                    std::pair<std::string, std::string>{"CODEX_HOME", selected->profile_path}) == 1,
                    "Preparation must pin the selected native profile exactly once");
                require(std::count_if(patch->set.begin(), patch->set.end(), [&](const auto& entry) {
                    return entry.first == codex::k_export_sqlite_home_environment &&
                        addon::path_from_utf8(entry.second) == expected;
                }) == 1,
                    "Export preparation must freeze the selected profile's native state store");
                std::ifstream observed(log);
                std::string line;
                bool read_configuration = false;
                while (std::getline(observed, line)) {
                    const auto record = addon::parse_json(line, {}, nullptr);
                    require(record && record->find("codex_home") &&
                        record->find("codex_home")->as_string() == selected->profile_path,
                        "The native configuration probe must receive the selected CODEX_HOME");
                    read_configuration |= record->find("method")->as_string() == "config/read";
                }
                require(read_configuration, "Export preparation must query native configuration");
            }
        }
    }
}

void native_account_fallback(const fs::path& root)
{
    // Read only the OS profile path. Default discovery would enumerate a real
    // profile, so the live boundary is exercised through accounts/resolve.
    fs::path native_home;
#ifdef _WIN32
    PWSTR path = nullptr;
    const auto status = SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path);
    if (SUCCEEDED(status)) {
        native_home = path;
    }
    CoTaskMemFree(path);
#else
    const auto* record = getpwuid(getuid());
    if (record) {
        native_home = record->pw_dir;
    }
#endif
    require(!native_home.empty(), "Native home-path oracle is unavailable");
    for (int home_state = 0; home_state < 3; ++home_state) {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "codex";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = addon::host_platform_id();
        auto& environment = request.launch_environment.effective_environment;
        if (home_state != 0) {
            const auto value = home_state == 1 ? std::string{} : utf8(root / "override");
            environment.set("HOME",        addon::Json_value(value));
            environment.set("USERPROFILE", addon::Json_value(value));
        }
        request.default_home_fallback   = utf8(root / "host-fallback");
        request.normalization_directory = utf8(root);
        const auto expected_home = std::string_view(addon::host_platform_id()).starts_with("windows-") || home_state != 2
            ? native_home : root / "override";
        Context context;
        context.input.operation = addon::k_accounts_resolve_operation;
        context.input.payload   = addon::encode_accounts_launcher_request(request);
        codex::handle_accounts(context);
        addon::Contract_failure failure;
        const auto result = addon::decode_account_resolution_result(context.output, &failure);
        require(context.succeeded && result && result->adapter_account_key ==
            codex::profile_account_key(utf8(expected_home / ".codex")),
            "Live account resolution must use the native OS fallback instead of the host's Qt fallback");
    }
}

// The literal PowerShell launcher of the same program, found on PATH.
addon::Prepare_request through_powershell(addon::Prepare_request request)
{
    const auto program = addon::path_from_utf8(request.launcher_argv.front());
    request.launcher.runner_key  = addon::k_runner_powershell;
    request.launcher.script_body = addon::base_name(utf8(program), addon::Launcher_whitespace::ASCII) + " --yolo";
    request.launcher_argv        = {"powershell", "-NoLogo", "-Command", request.launcher.script_body};
    request.launch_environment->effective_environment.set("PATH", addon::Json_value(utf8(program.parent_path())));
    return request;
}

// The same launcher record with other arguments after the program.
addon::Prepare_request with_arguments(addon::Prepare_request request, const std::vector<std::string>& arguments)
{
    request.launcher.script_body = addon::base_name(request.launcher_argv.front(), addon::Launcher_whitespace::ASCII);
    request.launcher_argv.resize(1);
    for (const auto& argument : arguments) {
        request.launcher.script_body += ' ' + argument;
        request.launcher_argv.push_back(argument);
    }
    return request;
}

void require_refusal(addon::Prepare_request request, std::string text, std::string brief, const char* detail)
{
    request.initial_prompt = addon::Initial_prompt_request{std::move(text), std::move(brief)};
    Context context;
    prepare_launch(context, request);
    require(!context.succeeded && context.output.is_string() &&
        context.output.as_string() == addon::reason::k_initial_prompt_unavailable, detail);
}

// Starts a prepared invocation the way the terminal does, one command-line
// argument per argv entry under the platform's quoting rules, and returns
// the arguments the fake CLI received.
std::vector<std::string> delivered_arguments(
    const std::vector<std::string>&                  argv,
    std::vector<std::pair<std::string, std::string>> environment,
    const fs::path&                                  root)
{
    environment.emplace_back("CODEX_FIXTURE_ARGV", "1");
    Context run;
    const auto output = codex::run_probe(argv, environment, utf8(root), 10000, run);
    const auto recorded = output ? addon::parse_json(output->output, {}, nullptr) : std::nullopt;
    require(recorded && recorded->is_array(), "The prepared invocation did not reach the fake CLI");
    std::vector<std::string> arguments;
    for (const auto& argument : recorded->as_array()) {
        arguments.push_back(argument.as_string());
    }
    return arguments;
}

void initial_prompt_launches(
    const addon::Prepare_request&                           direct,
    const std::vector<std::pair<std::string, std::string>>& probe_environment,
    const fs::path&                                         root,
    bool                                                   full_argument_matrix)
{
    // A brief path with spaces and non-ASCII characters, and a text made of
    // what a shell or the command-line quoting rules could reinterpret.
    const auto brief = utf8(root / u8"continuation briefs" / u8"Zoë Ångström 概要 🧭" / "brief.md");
    std::vector<std::string> texts{
        "Logonomic continuation: read the brief at " + brief +
            " before anything else and follow it; do not modify the project until the user confirms.",
        "-- \"quoted \\\"inner\\\"\" 'single' $HOME %PATH% ^& | < > ` \\\" trailing backslashes \\\\",
    };
    const auto powershell = through_powershell(direct);
    const auto modelled   = with_arguments(direct, {"--model", "gpt-5"});
    std::vector<const addon::Prepare_request*> launchers{&direct, &powershell};
    if (full_argument_matrix) {
        launchers.push_back(&modelled);
    }
    else {
        texts.erase(texts.begin());
    }
    addon::Contract_failure failure;
    for (const auto* launcher : launchers) {
        Context unprompted;
        prepare_launch(unprompted, *launcher);
        require(unprompted.succeeded && !unprompted.output.find(addon::k_initial_prompt_member),
            "A launch without an initial prompt must not acknowledge one");
        for (const auto& text : texts) {
            auto request = *launcher;
            request.initial_prompt = addon::Initial_prompt_request{text, brief};
            Context prompted;
            prepare_launch(prompted, request);
            const auto patch = addon::decode_launch_patch(prompted.output, &failure, 3);
            require(prompted.succeeded && patch &&
                addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure) &&
                addon::validate_launch_patch(*patch, {}, {}, &failure),
                "The host must accept the initial-prompt launch and its digest echo");
            const auto& prepared = *patch->argv;
            require(prepared[prepared.size() - 2] == "--" && prepared.back() == text &&
                std::count(prepared.begin(), prepared.end(), text) == 1,
                "The text must be the one positional prompt, after --");
            auto remainder = *patch;
            remainder.argv->resize(prepared.size() - 2);
            remainder.initial_prompt.reset();
            const auto encoded = addon::serialize_json(addon::encode_launch_patch(remainder, 3));
            require(encoded == addon::serialize_json(unprompted.output),
                "The initial prompt must add only --, the text and the echo");
            const auto delivered = delivered_arguments(prepared, probe_environment, root);
            require(delivered.size() == prepared.size() &&
                std::equal(prepared.begin() + 1, prepared.end(), delivered.begin() + 1),
                "Every argument, the text included, must reach Codex byte for byte");
        }
    }

    if (!full_argument_matrix) {
        return;
    }
    const auto& text = texts.front();
    require_refusal(direct, "!" + text, brief, "Codex would run a text starting with ! as a shell command");
    require_refusal(with_arguments(direct, {"resume", "--last"}), text, brief,
        "A launcher subcommand would take the initial prompt");
    require_refusal(with_arguments(direct, {"--yolo", "summarize"}), text, brief,
        "A launcher prompt would precede the initial prompt");
    require_refusal(with_arguments(direct, {"--model", "--"}), text, brief,
        "Codex takes -- as the end of options, leaving --model without its value");
    require_refusal(with_arguments(direct, {"--image", "screen.png"}), text, brief,
        "Launcher images would join the first message");
    require_refusal(with_arguments(direct, {"--search", "--search"}), text, brief,
        "Codex rejects a repeated flag, so the session would never start");
    require_refusal(with_arguments(direct, {"-m", "gpt-5", "--model", "gpt-5"}), text, brief,
        "Codex rejects a repeated single-value option under either of its names");
    auto without_prompt = direct;
    without_prompt.launch_environment->effective_environment.set("CODEX_FIXTURE_NO_PROMPT", addon::Json_value("1"));
    require_refusal(without_prompt, text, brief, "A Codex whose help shows no PROMPT cannot carry the initial prompt");
    auto runner = direct;
    runner.launcher.script_body = "npx @openai/codex";
    runner.launcher_argv        = {"npx", "@openai/codex"};
    require_refusal(runner, text, brief, "A package runner launcher is not prepared");
    auto absent = direct;
    absent.launcher_argv = {utf8(root / "absent" / "codex")};
    require_refusal(absent, text, brief, "An unverified Codex program cannot carry the initial prompt");
    Context unverified;
    prepare_launch(unverified, absent);
    require(!unverified.succeeded && unverified.output.is_string() &&
        unverified.output.as_string() == addon::reason::k_launcher_program_unavailable,
        "An unverified launch without an initial prompt keeps its refusal");

    // Launcher options that fill the argument bounds: the launch fits without
    // the prompt and exceeds them with it. The launcher record's own bound
    // keeps its options near 4 KiB, so the entry count is the bound a fixture
    // reaches.
    Context bare;
    prepare_launch(bare, with_arguments(direct, {}));
    const auto bare_patch = addon::decode_launch_patch(bare.output, &failure, 3);
    require(bare.succeeded && bare_patch && bare_patch->argv, "Launch without launcher arguments failed");
    std::vector<std::string> options;
    while (bare_patch->argv->size() + options.size() + 2 <= addon::limits::k_max_argv_entries) {
        options.push_back("-c");
        options.push_back("p" + std::to_string(options.size() / 2) + "=1");
    }
    const auto crowded = with_arguments(direct, options);
    Context fitting;
    prepare_launch(fitting, crowded);
    const auto fitting_patch = addon::decode_launch_patch(fitting.output, &failure, 3);
    require(fitting.succeeded && fitting_patch && addon::validate_launch_patch(*fitting_patch, {}, {}, &failure),
        "The crowded launch must fit the argument bounds without a prompt");
    require_refusal(crowded, text, brief, "A launch the prompt pushes past the argument bounds was accepted");
}

void codex_invocation_launches(
    const addon::Prepare_request&                           direct,
    const std::vector<std::string>&                         prefix,
    const std::vector<std::pair<std::string, std::string>>& probe_environment,
    const fs::path&                                         root,
    bool                                                   full_argument_matrix = true)
{
    // Oracle: the approved direct launch keeps its verified argv and obtains
    // terminal adaptation only through the host's explicit capability offer.
    Context baseline;
    prepare_launch(baseline, direct);
    addon::Contract_failure failure;
    const auto original = addon::decode_launch_patch(baseline.output, &failure, 3);
    require(baseline.succeeded && original && !original->codex_invocation,
        "A host without the Codex invocation offer must keep the established launch result");

    auto offered = direct;
    offered.codex_invocation = addon::Codex_invocation_request{};
    Context adapted;
    prepare_launch(adapted, offered);
    auto patch = addon::decode_launch_patch(adapted.output, &failure, 3);
    require(adapted.succeeded && patch && patch->argv && patch->codex_invocation &&
        patch->codex_invocation->argument_prefix_size == prefix.size() &&
        patch->argv->size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), patch->argv->begin()),
        "Terminal adaptation must identify the complete verified native or Node and JavaScript prefix");
    patch->codex_invocation.reset();
    require(addon::serialize_json(addon::encode_launch_patch(*patch, 3)) == addon::serialize_json(baseline.output),
        "The capability offer must preserve every verified argument, hook and account field");
    initial_prompt_launches(offered, probe_environment, root, full_argument_matrix);
}

void codex_invocation_resume(
    const addon::Resume_prepare_request& request,
    const std::vector<std::string>&      prefix)
{
    Context baseline;
    baseline.input = {"resume-baseline", {"interactive-resume", 2}, "prepare_resume",
        addon::encode_resume_prepare_request(request, 2), 15000};
    codex::handle_preparation(baseline);
    addon::Contract_failure failure;
    const auto original = addon::decode_resume_prepare_result(baseline.output, &failure, 2);
    require(baseline.succeeded && original && !original->patch.codex_invocation,
        "Resume without a host offer must keep its established result");

    auto offered = request;
    offered.context.codex_invocation = addon::Codex_invocation_request{};
    Context adapted;
    adapted.input = {"resume-adapted", {"interactive-resume", 2}, "prepare_resume",
        addon::encode_resume_prepare_request(offered, 2), 15000};
    codex::handle_preparation(adapted);
    auto result = addon::decode_resume_prepare_result(adapted.output, &failure, 2);
    require(adapted.succeeded && result && result->patch.codex_invocation &&
        result->patch.codex_invocation->argument_prefix_size == 1 + prefix.size() &&
        result->process.argv.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), result->process.argv.begin()),
        "Resume must count the launch bridge ahead of the complete verified Codex prefix");
    result->patch.codex_invocation.reset();
    require(addon::serialize_json(addon::encode_resume_prepare_result(*result, 2)) ==
        addon::serialize_json(baseline.output),
        "Resume adaptation must preserve the bridge, exact session, hooks, account and private observer");
}

#if defined(_WIN32) || defined(__linux__)
addon::Prepare_request npm_launcher(const addon::Prepare_request& native, const fs::path& root, bool pet = false)
{
    const auto directory = root / (pet ? "npm pet installation" : "npm installation");
    const auto package_name = pet ? "@imakris/codex-pet" : "@openai/codex";
    const auto package = directory / "node_modules" / package_name;
    const auto platform = pet ? package : directory / "node_modules" / "@openai" / "codex-win32-x64";
#ifdef _WIN32
    const auto image = platform / "vendor" / "x86_64-pc-windows-msvc" / "bin" / "codex.exe";
    const auto node = directory / "node.exe";
#elif defined(__aarch64__)
    const auto image = platform / "vendor" / "aarch64-unknown-linux-musl" / "bin" / "codex";
    const auto node = directory / "node";
#else
    const auto image = platform / "vendor" / "x86_64-unknown-linux-musl" / "bin" / "codex";
    const auto node = directory / "node";
#endif
    fs::create_directories(package / "bin");
    fs::create_directories(image.parent_path());
    fs::copy_file(addon::path_from_utf8(native.launcher_argv.front()), node);
    fs::copy_file(addon::path_from_utf8(native.launcher_argv.front()), image);
    // The copied native fixture accepts the verified JavaScript prefix before
    // Codex arguments; the preparation still checks the real npm file layout.
    const auto write = [](const fs::path& path, std::string_view content) {
        std::ofstream stream(path, std::ios::binary);
        stream << content;
        require(stream.good(), "Could not write the Codex npm launch fixture");
    };
#ifdef _WIN32
    const std::string command_name = pet ? "codex-pet.cmd" : "codex.cmd";
    write(directory / command_name, pet
        ? "@echo off\r\n\"%~dp0node.exe\" \"%~dp0node_modules\\@imakris\\codex-pet\\bin\\codex.js\" %*\r\n"
        :
        "@echo off\r\n\"%~dp0node.exe\" \"%~dp0node_modules\\@openai\\codex\\bin\\codex.js\" %*\r\n");
#else
    const std::string command_name = "codex-pet";
    fs::create_symlink(package / "bin" / "codex.js", directory / command_name);
#endif
    write(package / "bin" / "codex.js", "#!/usr/bin/env node\n// Verified npm entry point fixture.\n");
#ifndef _WIN32
    fs::permissions(package / "bin" / "codex.js", fs::perms::owner_exec, fs::perm_options::add);
#endif
    write(package / "package.json", pet
        ? "{\"name\":\"@imakris/codex-pet\",\"version\":\"0.150.1-pet.46.1\",\"bin\":{\"codex-pet\":\"bin/codex.js\"}}"
        :
        "{\"name\":\"@openai/codex\",\"version\":\"0.150.1\",\"bin\":{\"codex\":\"bin/codex.js\"},"
            "\"optionalDependencies\":{\"@openai/codex-win32-x64\":\"npm:@openai/codex@0.150.1-win32-x64\"}}");
    if (!pet) {
        write(platform / "package.json", "{\"name\":\"@openai/codex\",\"version\":\"0.150.1-win32-x64\"}");
    }

    auto request = native;
    request.launcher_argv.front() = utf8(directory / command_name);
    request.launch_environment->effective_environment.set("PATH", addon::Json_value(utf8(directory)));
    return request;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try {
        for (const auto assignment : {"hooks={}", "'hooks'.SessionStart=[]", "\"h\\u006foks\"={}", "\"\\U00000068ooks\"={}"}) {
            require(codex::hooks_configuration_conflicts({"codex", "-c", assignment}, 1), "TOML hooks override escaped ownership");
        }
        require(!codex::hooks_configuration_conflicts({"codex", "-c", "model=\"hooks\""}, 1), "Unrelated config rejected");
        require(codex::supported_session_arguments({"codex", "--model", "gpt-5", "--yolo"}, false), "Supported delegated flags rejected");
        require(!codex::supported_session_arguments({"codex", "resume", "session"}, false), "Existing delegated subcommand accepted");
        require(!codex::supported_session_arguments({"codex", "--yolo", "--yolo"}, true) &&
            codex::supported_session_arguments({"codex", "-c", "a=1", "--config", "b=2", "--enable", "x", "--enable", "y"}, false),
            "Only configuration, feature and attachment options may repeat");
        const auto root = fs::temp_directory_path() / ("codex-preparation-" + addon::random_nonce_hex());
        fs::create_directories(root);
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
        native_account_fallback(root);
        native_home_selection(fs::absolute(argv[1]), root);
        addon::Launch_environment environment;
#ifdef _WIN32
        environment.platform_id = "windows-x86_64";
#else
        environment.platform_id = addon::host_platform_id();
#endif
        environment.effective_environment.set("CODEX_HOME", addon::Json_value(utf8(root)));
        environment.effective_environment.set("TEMP", addon::Json_value(utf8(root)));
        if (const char* system_root = std::getenv("SystemRoot")) environment.effective_environment.set("SystemRoot", addon::Json_value(system_root));
        std::vector<std::pair<std::string, std::string>> probe_environment;
        for (const auto& member : environment.effective_environment.as_object()) probe_environment.emplace_back(member.name, member.value.as_string());
        // More admissions than the custody table holds must survive both
        // failed spawns and successfully reaped children without leaking slots.
        for (int attempt = 0; attempt < 64; ++attempt) {
            Context probe;
            require(!codex::run_probe({utf8(root / "absent-cli")}, probe_environment, utf8(root), 1000, probe),
                "Missing probe executable unexpectedly spawned");
            const auto result = codex::run_probe({argv[1], "--version"}, probe_environment, utf8(root), 1000, probe);
            require(result && (result->output == "codex-cli 0.150.1\n" ||
                result->output == "codex-cli 0.150.1\r\n"),
                "Probe custody admission leaked after failed spawn or child reaping");
        }
#ifndef _WIN32
        Context inherited;
        const auto custody = codex::run_probe({argv[1], "--fixture-custody"}, probe_environment, utf8(root), 1000, inherited);
        require(custody && custody->output == std::to_string(getpgrp()) + ':' + std::to_string(getsid(0)) + '\n',
            "Probe escaped the inherited custodian process group or session");
#endif
        for (const bool cancel : {false, true}) {
            Context blocked;
            const auto started = std::chrono::steady_clock::now();
            if (cancel) blocked.cancel_at = started + std::chrono::milliseconds(50);
            addon::Probe_exchange exchange;
            exchange.initial_input = std::string(2 * 1024 * 1024, 'x');
            exchange.on_output = [](std::string_view) { return std::optional<std::string>(""); };
            const auto observed = codex::run_probe({argv[1], "--fixture-ignore-stdin"}, probe_environment,
                utf8(root), 150, blocked, &exchange);
            require(!observed, "Nonreading child escaped cancellation/deadline");
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(3), "Custody settlement exceeded its bound");
        }
        Context overflow;
        require(!codex::run_probe({argv[1], "--fixture-overflow"}, probe_environment, utf8(root), 1000, overflow),
            "Oversized version output accepted");
        addon::Prepare_request prepare;
        prepare.launcher.runner_key  = addon::k_runner_direct;
        prepare.launcher.script_body = "codex --yolo -c features.plugins=true";
        prepare.launcher_argv        = {argv[1], "--yolo", "-c", "features.plugins=true"};
        prepare.working_directory    = utf8(root);
        prepare.launch_environment   = environment;
        addon::Account_context account;
        account.version             = 2;
        account.adapter_account_key = codex::profile_account_key(utf8(root));
        account.profile_path        = utf8(root);
        prepare.account_context     = account;
        Context interactive;
        interactive.input = {"interactive", {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(prepare, 3)};
        codex::handle_preparation(interactive);
        addon::Contract_failure failure;
        const auto patch = addon::decode_launch_patch(interactive.output, &failure, 3);
        require(interactive.succeeded && patch && patch->argv && patch->argv->at(1) == "-c", "Verified hook launch missing");
        require(std::count(patch->argv->begin(), patch->argv->end(), "features.plugins=true") == 1 &&
            std::count(patch->argv->begin(), patch->argv->end(), "features.plugins=false") == 0,
            "Interactive launch keeps the user's plugin configuration");
        if (!addon::validate_launch_patch(*patch, {}, {}, &failure)) {
            throw std::runtime_error(failure.reason_code + ": " + failure.detail);
        }
        for (const std::string event : {"SessionStart", "SessionEnd", "UserPromptSubmit", "Stop",
                "Interrupt", "PermissionRequest", "SubagentStart", "SubagentStop", "PostCompact"})
        {
            const auto prefix = "hooks." + event + "=";
            require(std::count_if(patch->argv->begin(), patch->argv->end(), [&](const auto& argument) {
                return argument.starts_with(prefix);
            }) == 1, "Each required lifecycle hook must be configured exactly once");
        }
        require(std::count_if(patch->argv->begin(), patch->argv->end(), [](const auto& argument) {
            return argument.starts_with("hooks.PreToolUse=[{hooks=[") &&
                argument.find("additionalContextLimit=12000") != std::string::npos;
        }) == 1, "Every tool checkpoint must admit bounded quiet context exactly once");
        require(std::count_if(patch->argv->begin(), patch->argv->end(), [](const auto& argument) {
            return argument.starts_with("hooks.PostToolUse=[{matcher=\"^request_user_input(_async)?$\",hooks=[");
        }) == 1, "Both native question completion hooks must be narrowly matched and configured exactly once");
        const std::vector<std::string> native_prefix{utf8(fs::canonical(addon::path_from_utf8(argv[1])))};
        codex_invocation_launches(prepare, native_prefix, probe_environment, root);
        const auto pet_directory = root / "pet";
        fs::create_directories(pet_directory);
        const auto pet_program = pet_directory / (std::string("codex-pet") + fs::path(argv[1]).extension().string());
        fs::copy_file(addon::path_from_utf8(argv[1]), pet_program);
        auto pet = prepare;
        pet.launcher.script_body = "codex-pet --yolo -c features.plugins=true";
        pet.launcher_argv.front() = utf8(pet_program);
        const std::vector<std::string> pet_prefix{utf8(pet_program)};
        codex_invocation_launches(pet, pet_prefix, probe_environment, root, false);
#ifdef _WIN32
        const auto npm = npm_launcher(prepare, root);
        const std::vector<std::string> npm_prefix{
            utf8(root / "npm installation" / "node.exe"),
            utf8(root / "npm installation" / "node_modules" / "@openai" / "codex" / "bin" / "codex.js"),
        };
        codex_invocation_launches(npm, npm_prefix, probe_environment, root);
#endif
#if defined(_WIN32) || defined(__linux__)
        const auto npm_pet = npm_launcher(pet, root, true);
        const std::vector<std::string> npm_pet_prefix{
#ifdef _WIN32
            utf8(root / "npm pet installation" / "node.exe"),
#else
            utf8(root / "npm pet installation" / "node"),
#endif
            utf8(root / "npm pet installation" / "node_modules" / "@imakris" / "codex-pet" / "bin" / "codex.js"),
        };
        codex_invocation_launches(npm_pet, npm_pet_prefix, probe_environment, root, false);
#endif
        addon::Delegated_prepare_request delegated;
        delegated.context.launcher           = prepare.launcher;
        delegated.context.launcher_argv      = prepare.launcher_argv;
        delegated.context.working_directory = prepare.working_directory;
        delegated.context.launch_environment = environment;
        delegated.context.account_context = account;
        delegated.prompt = "Implement the selected repository change.";
        delegated.control.task_id = "task-a";
        delegated.control.private_artifact_root = utf8(root);
        delegated.control.connection = addon::Delegated_connection{
            utf8(root / "host relay"), utf8(root / ".logonomic/task-servers.json")};
        addon::Delegated_task_server server;
        server.server_id = "control";
        server.program = argv[1];
        server.actions = {"task_status"};
        server.environment = {
            {"LOGONOMIC_SESSION_CAPABILITY", std::string(64, 'b')},
            {"VNM_CONTROL_ENDPOINT", "tcp://127.0.0.1:32970"},
            {"VNM_CONTROL_TOKEN", std::string(64, 'c')},
            {"LOGONOMIC_VNMCTL_PATH", argv[1]},
        };
        delegated.control.servers.push_back(server);
        Context task;
        task.input = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(delegated)};
        codex::handle_preparation(task);
        const auto task_process = addon::decode_delegated_prepare_result(task.output, &failure);
        require(task.succeeded && task_process && task_process->private_artifacts.empty() &&
            task_process->acknowledgment.server_ids == std::vector<std::string>{"control"} &&
            task_process->acknowledgment.connection_version == addon::k_delegated_connection_version,
            "Delegation must acknowledge the configured relay");
        const auto has_overlay = [&](const std::string& suffix) {
            return std::any_of(task_process->process.argv.begin(), task_process->process.argv.end(),
                [&](const auto& argument) {
                    return argument.starts_with("mcp_servers.logonomic_") && argument.ends_with(suffix);
                });
        };
        require(has_overlay("command=" + codex::toml_string(delegated.control.connection->relay_program)),
            "Native MCP command is the supplied relay");
        require(has_overlay("args=[\"--descriptor\"," + codex::toml_string(delegated.control.connection->descriptor_path) +
                ",\"--server\",\"control\"]"),
            "Native MCP arguments preserve the exact descriptor and server");
        require(has_overlay("enabled_tools=[\"task_status\"]") &&
            has_overlay("default_tools_approval_mode=\"approve\""), "Native tool configuration is preserved");
        require(addon::serialize_json(task.output).find(std::string(64, 'c')) == std::string::npos,
            "Host credentials cannot enter protocol results");
        auto disconnected = delegated;
        disconnected.control.connection.reset();
        Context refused;
        refused.input = task.input;
        refused.input.payload = addon::encode_delegated_prepare_request(disconnected);
        codex::handle_preparation(refused);
        require(!refused.succeeded, "Missing connection refuses delegation");
        require(addon::validate_structured_process_proposal(task_process->process, &failure),
            "Delegated producer must satisfy the host process policy");
        require(std::find(task_process->process.environment_names.begin(), task_process->process.environment_names.end(),
            "CODEX_HOME") == task_process->process.environment_names.end(), "Selected profile must not also be inherited");
        require(std::count(task_process->process.environment.begin(), task_process->process.environment.end(),
            std::pair<std::string, std::string>{"CODEX_HOME", utf8(root)}) == 1, "Selected profile must be assigned exactly once");
        require(task_process->process.stdin_payload == delegated.prompt && task_process->process.argv.back() == "-", "Delegated prompt stdin changed");
        const auto verify_pet_delegation = [&](
                const addon::Prepare_request& launcher,
                const std::vector<std::string>& prefix)
            {
                auto request = delegated;
                request.context.launcher      = launcher.launcher;
                request.context.launcher_argv = launcher.launcher_argv;
                request.context.launch_environment = launcher.launch_environment;
                Context prepared;
                prepared.input = task.input;
                prepared.input.payload = addon::encode_delegated_prepare_request(request);
                codex::handle_preparation(prepared);
                const auto result = addon::decode_delegated_prepare_result(prepared.output, &failure);
                require(prepared.succeeded && result && result->process.argv.size() >= prefix.size() &&
                    std::equal(prefix.begin(), prefix.end(), result->process.argv.begin()) &&
                    result->process.stdin_payload == delegated.prompt && result->process.argv.back() == "-" &&
                    result->account_context && task_process->account_context &&
                    addon::encode_account_context(*result->account_context) ==
                        addon::encode_account_context(*task_process->account_context),
                    "Codex pet delegation preserves the verified launcher, prompt and account");
            };
        verify_pet_delegation(pet, pet_prefix);
#if defined(_WIN32) || defined(__linux__)
        verify_pet_delegation(npm_pet, npm_pet_prefix);
#endif
        require(std::any_of(task_process->process.argv.begin(), task_process->process.argv.end(), [](const auto& argument) {
            return argument.starts_with("mcp_servers.logonomic_") && argument.ends_with("required=true");
        }), "Task control MCP server was not required");
        // Oracle: the owner's rule that a delegated run keeps its launcher's
        // permission posture and the package grants only its own Task tools
        // (2026-09-28), and codex-cli 0.157.0 recorded against a scripted
        // endpoint: `codex exec` runs a Task tool its server entry approves
        // without --yolo, and refuses a directory with no `.git` entry in it or
        // an ancestor unless --yolo is passed.
        require(std::count(task_process->process.argv.begin(), task_process->process.argv.end(), "--yolo") == 1,
            "The launcher's own --yolo is kept, once");
        require(std::count(task_process->process.argv.begin(), task_process->process.argv.end(), "features.plugins=true") == 1 &&
            std::count(task_process->process.argv.begin(), task_process->process.argv.end(), "features.plugins=false") == 0,
            "Delegated launch keeps the user's plugin configuration");
        auto plain = delegated;
        plain.context.launcher.script_body = "codex";
        plain.context.launcher_argv        = {argv[1]};
        Context outside;
        outside.input = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(plain)};
        codex::handle_preparation(outside);
        require(!outside.succeeded && outside.output == addon::Json_value("untrusted_working_directory") &&
            outside.failure_detail.find("Git repository") != std::string::npos,
            "Without --yolo a directory outside a Git repository is refused, saying why");
        fs::create_directories(root / "repository" / ".git");
        fs::create_directories(root / "repository" / "src");
        plain.context.working_directory = utf8(root / "repository" / "src");
        Context inside;
        inside.input = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(plain)};
        codex::handle_preparation(inside);
        const auto plain_process = addon::decode_delegated_prepare_result(inside.output, &failure);
        require(inside.succeeded && plain_process &&
            std::find(plain_process->process.argv.begin(), plain_process->process.argv.end(), "--yolo") ==
                plain_process->process.argv.end() &&
            std::any_of(plain_process->process.argv.begin(), plain_process->process.argv.end(), [](const auto& argument) {
                return argument.starts_with("mcp_servers.logonomic_") &&
                    argument.ends_with("default_tools_approval_mode=\"approve\"");
            }),
            "A launcher without --yolo runs in its own sandbox; only the Task tools are approved");
        addon::Resume_prepare_request resume;
        resume.context.launcher           = prepare.launcher;
        resume.context.launcher_argv      = prepare.launcher_argv;
        resume.context.working_directory = prepare.working_directory;
        resume.context.launch_environment = environment;
        resume.context.account_context = prepare.account_context;
        resume.resume_record.set("provider_session_id", addon::Json_value("019933a1-6e43-7102-a520-b3c8294cc763"));
        resume.startup_observer = addon::Startup_observer_offer{1, "http-json-post", "http://127.0.0.1:32871/events", ("Bearer " + std::string(64, 'd')), "prepared-identity"};
        require(addon::decode_startup_observer_offer(
            addon::encode_startup_observer_offer(*resume.startup_observer), &failure).has_value(),
            "Positive startup offer must satisfy the actual public decoder");
        for (const bool bad_endpoint : {false, true}) {
            auto invalid = *resume.startup_observer;
            if (bad_endpoint) invalid.endpoint = "http://127.0.0.1:32871/v1/logs";
            else invalid.authorization = "Bearer live-private-token";
            require(!addon::decode_startup_observer_offer(
                addon::encode_startup_observer_offer(invalid), &failure) &&
                failure.reason_code == "frame_violation",
                "Malformed public startup offer must remain a frame violation");
        }
        auto wrong_endpoint = resume;
        wrong_endpoint.startup_observer->endpoint = "http://127.0.0.1:32871/v1/logs";
        Context invalid_endpoint;
        invalid_endpoint.input = {"invalid-endpoint", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(wrong_endpoint, 2), 15000};
        codex::handle_preparation(invalid_endpoint);
        require(!invalid_endpoint.succeeded && invalid_endpoint.output.is_string() &&
            invalid_endpoint.output.as_string() == "frame_violation",
            "Invalid public endpoint must fail before private startup proof");
        auto malformed_resume = resume;
        malformed_resume.startup_observer->authorization = "Bearer live-private-token";
        Context malformed_observer;
        malformed_observer.input = {"malformed-observer", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(malformed_resume, 2), 15000};
        codex::handle_preparation(malformed_observer);
        require(!malformed_observer.succeeded && malformed_observer.output.is_string() &&
            malformed_observer.output.as_string() == "frame_violation",
            "Malformed startup observer offer must remain rejected");
        resume.context.session_wake = addon::Session_wake_offer{};
        Context exact;
        exact.input = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume, 2), 15000};
        codex::handle_preparation(exact);
        const auto proposal = addon::decode_resume_prepare_result(exact.output, &failure, 2);
        if (!exact.succeeded) std::cerr << "Resume prepare refusal: " << addon::serialize_json(exact.output) << "; " << exact.failure_detail << '\n';
        require(exact.succeeded && proposal && proposal->startup_observer && proposal->startup_observer->authorization == ("Bearer " + std::string(64, 'd')), "Isolated startup proof or exact observer echo missing");
        require(proposal->patch.session_wake && proposal->patch.session_wake->available,
            "Exact resume must retain the native wake binding for its new process");
        if (!addon::validate_resume_process_proposal(proposal->process, &failure)) {
            throw std::runtime_error(failure.reason_code + ": " + failure.detail);
        }
        if (!addon::validate_launch_patch(proposal->patch, {}, {}, &failure)) {
            throw std::runtime_error(failure.reason_code + ": " + failure.detail);
        }
        require(proposal->process.environment_names.empty() && proposal->process.environment.empty() &&
            proposal->process.assets.empty(), "Resume environment must reach the terminal through its patch");
        require(std::count(proposal->patch.set.begin(), proposal->patch.set.end(),
            std::pair<std::string, std::string>{"CODEX_HOME", utf8(root)}) == 1, "Resume patch lost selected profile");
        require(std::count(proposal->patch.set.begin(), proposal->patch.set.end(),
            std::pair<std::string, std::string>{"OTEL_EXPORTER_OTLP_LOGS_HEADERS", ("Authorization=Bearer%20" + std::string(64, 'd'))}) == 1,
            "Resume patch lost private observer authorization");
        require(proposal->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763", "Exact resume identity changed");
        require(std::count(proposal->process.argv.begin(), proposal->process.argv.end(), "features.plugins=true") == 1 &&
            std::count(proposal->process.argv.begin(), proposal->process.argv.end(), "features.plugins=false") == 0,
            "Interactive resume keeps the user's plugin configuration");
        for (const auto& argument : proposal->process.argv) require(argument.find(std::string(64, 'd')) == std::string::npos, "Startup secret escaped into argv");
        codex_invocation_resume(resume, native_prefix);
        auto pet_resume = resume;
        pet_resume.context.launcher      = pet.launcher;
        pet_resume.context.launcher_argv = pet.launcher_argv;
        codex_invocation_resume(pet_resume, pet_prefix);
#ifdef _WIN32
        auto npm_resume = resume;
        npm_resume.context.launcher      = npm.launcher;
        npm_resume.context.launcher_argv = npm.launcher_argv;
        codex_invocation_resume(npm_resume, npm_prefix);
#endif
#if defined(_WIN32) || defined(__linux__)
        auto npm_pet_resume = resume;
        npm_pet_resume.context.launcher      = npm_pet.launcher;
        npm_pet_resume.context.launcher_argv = npm_pet.launcher_argv;
        npm_pet_resume.context.launch_environment = npm_pet.launch_environment;
        codex_invocation_resume(npm_pet_resume, npm_pet_prefix);
#endif
        resume.context.launcher.script_body = "codex -c fixture_exporter=true";
        resume.context.launcher_argv = {argv[1], "-c", "fixture_exporter=true"};
        Context conflict;
        conflict.input = {"conflict", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume, 2)};
        codex::handle_preparation(conflict);
        require(!conflict.succeeded, "Existing telemetry exporter overwritten");
        for (const std::string& defect : {std::string("missing-type"), std::string("surplus")}) {
            resume.context.launch_environment->effective_environment.set(
                "CODEX_FIXTURE_OBSERVER", addon::Json_value(defect));
            resume.context.launcher.script_body = "codex -c fixture_observer=" + defect;
            resume.context.launcher_argv = {argv[1], "-c", "fixture_observer=" + defect};
            Context rejected;
            rejected.input = {"invalid-http", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume, 2), 15000};
            codex::handle_preparation(rejected);
            require(!rejected.succeeded && rejected.output.is_string() &&
                rejected.output.as_string() == "startup_observer_unavailable",
                "Malformed telemetry must fail at the actual private startup-observer owner");
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
