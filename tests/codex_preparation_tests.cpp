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

const std::vector<std::vector<std::string>> k_permission_arguments{
    {"--sandbox", "workspace-write", "--ask-for-approval", "on-request"},
    {"--sandbox=workspace-write", "--ask-for-approval=on-request"},
    {"-s", "workspace-write", "-a", "on-request"},
    {"-s=workspace-write", "-a=on-request"},
    {"-sworkspace-write", "-aon-request"},
};

class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool succeeded = false;
    bool done = false;
    std::string failure_detail;
    std::optional<std::chrono::steady_clock::time_point> cancel_at;
    fs::path cancel_when_created;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override {
        return (cancel_at && std::chrono::steady_clock::now() >= *cancel_at) ||
            (!cancel_when_created.empty() && fs::exists(cancel_when_created));
    }
    bool wait_for_cancel(int timeout) override { std::this_thread::sleep_for(std::chrono::milliseconds(timeout)); return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string detail) override { done = true; failure_detail = std::move(detail); output = addon::Json_value(std::move(reason)); }
    bool finished() const override { return done; }
};

void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
std::string utf8(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }

void require_root_configuration(const std::vector<std::string>& arguments)
{
    const auto exec = std::find(arguments.begin(), arguments.end(), "exec");
    require(exec != arguments.end() && std::none_of(exec, arguments.end(), [](const auto& argument) {
        return argument == "--config" || argument.starts_with("--config=") ||
            (argument.starts_with("-c") && !argument.starts_with("--"));
    }), "Every native configuration option must precede exec so it cannot replace the root configuration");
}

// One launch-preparation@3 prepare through the production dispatch.
void prepare_launch(Context& context, const addon::Prepare_request& request,
    int deadline_ms = addon::limits::k_launch_prepare_deadline_ms)
{
    context.input = {"interactive", {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3), deadline_ms};
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

void title_configuration_launches(const addon::Prepare_request& direct, const fs::path& root)
{
    // Native config/read returns resolved tui fields. In Codex 0.160, selecting
    // activity (also named spinner) enables Action Required title evidence.
    // An absent list uses the native default; an explicit empty list disables it.
    struct Title_case
    {
        const char* tui;
        const char* overlay;
        bool        disabled = false;
    };
    const Title_case cases[] = {
        {nullptr, nullptr},
        {"{}", nullptr},
        {R"({"terminal_title":null})", nullptr},
        {R"({"terminal_title":[]})", R"(tui.terminal_title=["spinner"])"},
        {R"({"terminal_title":["project-name","run-state"]})",
            R"(tui.terminal_title=["spinner","project-name","run-state"])"},
        {R"({"terminal_title":["project-name","activity","thread-name"]})", nullptr},
        {R"({"terminal_title":["project-name","spinner"]})", nullptr},
        {R"({"terminal_title":["project-name","future\"\\\u0001\u007f"]})",
            R"(tui.terminal_title=["spinner","project-name","future\"\\\u0001\u007f"])"},
        {R"({"terminal_title":[],"animations":false,"effects":{"progress":false},"notifications":false})",
            R"(tui.terminal_title=["spinner"])", true},
    };
    const auto log = root / "title-configuration.jsonl";
    addon::Contract_failure failure;
    for (const auto& example : cases) {
        fs::remove(log);
        auto request = direct;
        auto& environment = request.launch_environment->effective_environment;
        environment.set("CODEX_FIXTURE_WAKE_LOG", addon::Json_value(utf8(log)));
        if (example.tui) {
            environment.set("CODEX_FIXTURE_TUI", addon::Json_value(example.tui));
        }
        if (example.disabled) {
            const std::string settings = "tui={terminal_title=[],animations=false,effects={progress=false},notifications=false}";
            request.launcher_argv.insert(request.launcher_argv.end(), {"-c", settings});
            request.launcher.script_body += " -c " + settings;
        }
        Context context;
        prepare_launch(context, request);
        const auto patch = addon::decode_launch_patch(context.output, &failure, 3);
        require(context.succeeded && patch && patch->argv,
            "Native title configuration must leave ordinary preparation available");
        const auto& arguments = *patch->argv;
        require(std::search(arguments.begin(), arguments.end(),
            request.launcher_argv.begin() + 1, request.launcher_argv.end()) != arguments.end(),
            "Title preparation must preserve every original launcher argument in order");
        std::vector<std::string> title_overlays;
        for (const auto& argument : arguments) {
            if (argument.starts_with("tui.terminal_title=")) {
                title_overlays.push_back(argument);
            }
            require(!argument.starts_with("tui.") || argument.starts_with("tui.terminal_title="),
                "Title preparation must not enable disabled animations or progress effects");
        }
        const auto expected = example.overlay
            ? std::vector<std::string>{example.overlay} : std::vector<std::string>{};
        require(title_overlays == expected,
            "Resolved title fields must retain their order and gain activity only when absent");
        std::ifstream observed(log);
        std::vector<std::string> methods;
        for (std::string line; std::getline(observed, line);) {
            const auto record = addon::parse_json(line, {}, nullptr);
            require(record && record->find("method") && record->find("method")->is_string(),
                "Native title configuration probe did not record its method");
            methods.push_back(record->find("method")->as_string());
        }
        require(methods == std::vector<std::string>{"initialize", "config/read"},
            "Title preparation must reuse the one existing native configuration probe");
    }
    auto prompted = direct;
    prompted.launch_environment->effective_environment.set("CODEX_FIXTURE_TUI",
        addon::Json_value(R"({"terminal_title":[]})"));
    prompted.initial_prompt = addon::Initial_prompt_request{
        "-- preserve this first message", utf8(root / "brief.md")};
    Context prompt_context;
    prepare_launch(prompt_context, prompted);
    const auto prompt_patch = addon::decode_launch_patch(prompt_context.output, &failure, 3);
    require(prompt_context.succeeded && prompt_patch && prompt_patch->argv &&
        addon::validate_initial_prompt_acknowledgment(prompted.initial_prompt, *prompt_patch, &failure) &&
        prompt_patch->argv->at(prompt_patch->argv->size() - 2) == "--" &&
        prompt_patch->argv->back() == prompted.initial_prompt->text &&
        std::count(prompt_patch->argv->begin(), prompt_patch->argv->end(),
            R"(tui.terminal_title=["spinner"])") == 1,
        "Title configuration must precede and preserve the acknowledged positional prompt");
    for (const bool named_profile : {true, false}) {
        fs::remove(log);
        auto request = named_profile ? with_arguments(direct, {"--profile", "coding"}) : direct;
        auto& environment = request.launch_environment->effective_environment;
        environment.set("CODEX_FIXTURE_TUI", addon::Json_value(R"({"terminal_title":[]})"));
        environment.set("CODEX_FIXTURE_WAKE_LOG", addon::Json_value(utf8(log)));
        if (!named_profile) {
            environment.set("CODEX_FIXTURE_WAKE", addon::Json_value("init-failure"));
        }
        Context context;
        prepare_launch(context, request);
        const auto patch = addon::decode_launch_patch(context.output, &failure, 3);
        require(context.succeeded && patch && patch->argv &&
            std::none_of(patch->argv->begin(), patch->argv->end(), [](const auto& argument) {
                return argument.starts_with("tui.terminal_title=");
            }),
            "Unavailable native configuration must not invent a resolved title list");
        if (named_profile) {
            require(std::count(patch->argv->begin(), patch->argv->end(), "--profile") == 1 &&
                std::count(patch->argv->begin(), patch->argv->end(), "coding") == 1 && !fs::exists(log),
                "An unsupported profile probe must preserve the named native profile without reading another one");
        }
    }
}

void identity_title_launches(const addon::Prepare_request& direct, const fs::path& root)
{
    struct Title_case
    {
        const char* version;
        const char* tui;
        const char* overlay;
        bool identity;
    };
    const Title_case cases[] = {
        {"0.167.0", nullptr,
            R"(tui.terminal_title=["activity","thread-name","project-name","session-id","thread-name","thread"])", true},
        {"0.167.0", R"({"terminal_title":null})",
            R"(tui.terminal_title=["activity","thread-name","project-name","session-id","thread-name","thread"])", true},
        {"0.167.0", R"({"terminal_title":[]})",
            R"(tui.terminal_title=["spinner","session-id","thread-name","thread"])", true},
        {"0.167.0", R"({"terminal_title":["project-name","run-state"]})",
            R"(tui.terminal_title=["spinner","project-name","run-state","session-id","thread-name","thread"])", true},
        {"0.167.0", R"({"terminal_title":["spinner","thread-title","project-name"],"animations":false})",
            R"(tui.terminal_title=["spinner","thread-title","project-name","session-id","thread-name","thread"])", true},
        {"0.167.0", R"({"terminal_title":["project-name","activity","thread-name"]})", nullptr, false},
        {"0.167.0", R"({"terminal_title":["spinner","project-name","activity"]})", nullptr, false},
        {"0.166.0", nullptr, nullptr, false},
        {"0.166.0", R"({"terminal_title":[]})", R"(tui.terminal_title=["spinner"])", false},
        {"0.167.0-alpha.4", nullptr, nullptr, false},
        {"0.167.1-alpha.1", nullptr, nullptr, false},
        {"0.167.0+qualified", nullptr,
            R"(tui.terminal_title=["activity","thread-name","project-name","session-id","thread-name","thread"])", true},
        {"0.168.0", nullptr,
            R"(tui.terminal_title=["activity","thread-name","project-name","session-id","thread-name","thread"])", true},
        {"0.168.0", R"({"terminal_title":["activity","project-name"]})",
            R"(tui.terminal_title=["activity","project-name","session-id","thread-name","thread"])", true},
    };
    const auto store_path = utf8(root / "identity-state");
    for (const auto& example : cases) {
        auto request = direct;
        request.terminal_identity = addon::Terminal_identity_offer{};
        auto& environment = request.launch_environment->effective_environment;
        environment.set("CODEX_FIXTURE_VERSION", addon::Json_value(example.version));
        environment.set("CODEX_FIXTURE_SQLITE_HOME", addon::Json_value(store_path));
        if (example.tui) {
            environment.set("CODEX_FIXTURE_TUI", addon::Json_value(example.tui));
        }
        Context context;
        prepare_launch(context, request);
        addon::Contract_failure failure;
        const auto patch = addon::decode_launch_patch(context.output, &failure, 3);
        require(context.succeeded && patch && patch->argv,
            "Unavailable title identity must preserve ordinary launch preparation");
        require(patch->terminal_identity.has_value() == example.identity,
            "Only qualified stable native title layouts may opt into immediate identity");
        std::vector<std::string> overlays;
        for (const auto& argument : *patch->argv) {
            if (argument.starts_with("tui.terminal_title=")) {
                overlays.push_back(argument);
            }
            require(!argument.starts_with("tui.") || argument.starts_with("tui.terminal_title="),
                "Identity preparation must preserve disabled animations and unrelated TUI settings");
        }
        require(overlays == (example.overlay ? std::vector<std::string>{example.overlay} : std::vector<std::string>{}),
            "Identity suffix must retain explicit original fields, verified defaults and field ordering");
        if (patch->terminal_identity) {
            const auto& prepared = patch->terminal_identity->context;
            const auto* native_store = prepared.find("native_store");
            require(native_store && addon::path_from_utf8(native_store->find("codex_home")->as_string()) == root &&
                addon::path_from_utf8(native_store->find("sqlite_home")->as_string()) == root / "identity-state" &&
                *prepared.find("native_version") == addon::Json_value(example.version),
                "Immediate identity must capture the prepared native namespace before the first hook");
        }
    }
    auto failed = direct;
    failed.terminal_identity = addon::Terminal_identity_offer{};
    failed.launch_environment->effective_environment.set("CODEX_FIXTURE_VERSION", addon::Json_value("0.167.0"));
    failed.launch_environment->effective_environment.set("CODEX_FIXTURE_WAKE", addon::Json_value("init-failure"));
    Context unavailable;
    prepare_launch(unavailable, failed);
    addon::Contract_failure failure;
    const auto patch = addon::decode_launch_patch(unavailable.output, &failure, 3);
    require(unavailable.succeeded && patch && !patch->terminal_identity,
        "A failed native configuration probe must not fabricate title or store qualification");

    auto prompted = direct;
    prompted.terminal_identity = addon::Terminal_identity_offer{};
    prompted.launch_environment->effective_environment.set("CODEX_FIXTURE_VERSION", addon::Json_value("0.167.0"));
    prompted.initial_prompt = addon::Initial_prompt_request{"-- exact initial text", utf8(root / "brief.md")};
    Context with_prompt;
    prepare_launch(with_prompt, prompted);
    const auto prompt_patch = addon::decode_launch_patch(with_prompt.output, &failure, 3);
    require(with_prompt.succeeded && prompt_patch && prompt_patch->terminal_identity && prompt_patch->argv &&
        addon::validate_initial_prompt_acknowledgment(prompted.initial_prompt, *prompt_patch, &failure) &&
        prompt_patch->argv->at(prompt_patch->argv->size() - 2) == "--" &&
        prompt_patch->argv->back() == prompted.initial_prompt->text,
        "The identity override must precede and preserve the actual initial prompt");
}

void title_configuration_bounds(const addon::Prepare_request& direct)
{
    auto long_title = direct;
    auto fields = addon::Json_value::array();
    for (int index = 0; index < 400; ++index) {
        fields.push_back(addon::Json_value("project-name"));
    }
    auto tui = addon::Json_value::object();
    tui.set("terminal_title", std::move(fields));
    long_title.launch_environment->effective_environment.set("CODEX_FIXTURE_TUI",
        addon::Json_value(addon::serialize_json(tui)));

    Context bare;
    prepare_launch(bare, with_arguments(direct, {}));
    addon::Contract_failure failure;
    const auto baseline = addon::decode_launch_patch(bare.output, &failure, 3);
    require(bare.succeeded && baseline && baseline->argv, "Title boundary baseline did not prepare");
    std::vector<std::string> options;
    while (baseline->argv->size() + options.size() + 2 <= addon::limits::k_max_argv_entries) {
        options.push_back("-c");
        options.push_back("p" + std::to_string(options.size() / 2) + "=1");
    }
    auto crowded = with_arguments(direct, options);
    crowded.launch_environment->effective_environment.set("CODEX_FIXTURE_TUI",
        addon::Json_value(R"({"terminal_title":[]})"));
    for (const auto& request : {long_title, crowded}) {
        Context context;
        prepare_launch(context, request);
        const auto patch = addon::decode_launch_patch(context.output, &failure, 3);
        require(context.succeeded && patch && patch->argv &&
            addon::validate_launch_patch(*patch, {}, {}, &failure),
            "Optional title activity must not invalidate an otherwise valid launch at argv bounds");
        require(std::none_of(patch->argv->begin(), patch->argv->end(), [](const auto& argument) {
            return argument.starts_with("tui.terminal_title=");
        }), "A title override that exceeds the launch contract must leave the original title intact");
        auto offered = request;
        offered.terminal_identity = addon::Terminal_identity_offer{};
        offered.launch_environment->effective_environment.set("CODEX_FIXTURE_VERSION", addon::Json_value("0.167.0"));
        Context identity;
        prepare_launch(identity, offered);
        const auto identity_patch = addon::decode_launch_patch(identity.output, &failure, 3);
        require(identity.succeeded && identity_patch && identity_patch->argv && !identity_patch->terminal_identity &&
            std::none_of(identity_patch->argv->begin(), identity_patch->argv->end(), [](const auto& argument) {
                return argument.starts_with("tui.terminal_title=");
            }), "A rejected suffix override must not publish a prepared identity capsule");
    }
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
    std::vector<addon::Prepare_request> permissions;
    for (const auto& arguments : k_permission_arguments) {
        permissions.push_back(with_arguments(direct, arguments));
    }
    std::vector<const addon::Prepare_request*> launchers{&direct, &powershell};
    if (full_argument_matrix) {
        launchers.push_back(&modelled);
        for (const auto& permission : permissions) launchers.push_back(&permission);
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
            if (launcher->launcher.runner_key == addon::k_runner_direct) {
                require(std::search(prepared.begin(), prepared.end(),
                    launcher->launcher_argv.begin() + 1, launcher->launcher_argv.end()) != prepared.end(),
                    "The launcher's explicit options must reach Codex unchanged");
            }
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

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
addon::Prepare_request npm_launcher(const addon::Prepare_request& native, const fs::path& root, bool pet = false)
{
    const auto directory = root / (pet ? "npm pet installation" : "npm installation");
    const auto package_name = pet ? "@imakris/codex-pet" : "@openai/codex";
    const auto package = directory / "node_modules" / package_name;
    const auto platform = pet ? package : directory / "node_modules" / "@openai" / "codex-win32-x64";
#ifdef _WIN32
    const auto image = platform / "vendor" / "x86_64-pc-windows-msvc" / "bin" / "codex.exe";
    const auto node = directory / "node.exe";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    const auto image = platform / "vendor" / "aarch64-apple-darwin" / "bin" / "codex";
    const auto node = directory / "node";
#elif defined(__APPLE__)
    const auto image = platform / "vendor" / "x86_64-apple-darwin" / "bin" / "codex";
    const auto node = directory / "node";
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
        for (const bool resumed : {false, true}) {
            for (const auto& permission : k_permission_arguments) {
                auto arguments = permission;
                arguments.insert(arguments.begin(), "codex");
                require(codex::supported_session_arguments(arguments, resumed),
                    "Explicit native sandbox and approval options must remain preparable");
            }
            for (const auto& invalid : std::vector<std::vector<std::string>>{
                    {"codex", "--sandbox", "invalid"}, {"codex", "-s="},
                    {"codex", "-a", "always"}, {"codex", "--ask-for-approval"},
                    {"codex", "-sread-only", "--sandbox", "workspace-write"},
                    {"codex", "-anever", "--ask-for-approval=on-request"},
                    {"codex", "--yolo", "-a", "on-request"},
                    {"codex", "-anever", "--dangerously-bypass-approvals-and-sandbox"},
                    {"codex", "--yolo", "--dangerously-bypass-approvals-and-sandbox"}}) {
                require(!codex::supported_session_arguments(invalid, resumed),
                    "Invalid, duplicate or conflicting permission options cannot prepare a session");
            }
            require(codex::supported_session_arguments({"codex", "-sworkspace-write", "--yolo"}, resumed) &&
                codex::supported_session_arguments({"codex", "--dangerously-bypass-approvals-and-sandbox", "-sread-only"}, resumed),
                "Native bypass and sandbox precedence remains native-owned");
        }
        const auto root = fs::canonical(fs::temp_directory_path()) / ("codex-preparation-" + addon::random_nonce_hex());
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
        auto interrupted = prepare;
        Context cancelled_configuration;
        cancelled_configuration.cancel_when_created = root / "cancel-config-read.jsonl";
        interrupted.launch_environment->effective_environment.set("CODEX_FIXTURE_WAKE_LOG",
            addon::Json_value(utf8(cancelled_configuration.cancel_when_created)));
        prepare_launch(cancelled_configuration, interrupted);
        require(!cancelled_configuration.succeeded && cancelled_configuration.output == addon::Json_value("cancelled"),
            "Cancellation during native configuration must withdraw the launch");
        title_configuration_launches(prepare, root);
        identity_title_launches(prepare, root);
        title_configuration_bounds(prepare);
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
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
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
        auto cold = through_powershell(npm_pet);
        cold.launch_environment->effective_environment.set("CODEX_FIXTURE_SLOW_CLI_START", addon::Json_value("1"));
        auto missing = cold;
        missing.launch_environment->effective_environment.set("PATH", addon::Json_value(utf8(root / "empty-path")));
        Context missing_call;
        prepare_launch(missing_call, missing);
        require(missing_call.output == addon::Json_value("launcher_program_not_found") &&
            missing_call.failure_detail.find("codex-pet") != std::string::npos,
            "An absent executable must identify the saved launcher in the failure message");
        Context short_call;
        prepare_launch(short_call, cold, 100);
        require(short_call.output == addon::Json_value("launcher_version_timeout") &&
            short_call.failure_detail.find("--version") != std::string::npos,
            "An expired native version check must explain the timeout");
        cold.initial_prompt = addon::Initial_prompt_request{"Read the brief.", utf8(root / "brief.md")};
        Context cold_call;
        prepare_launch(cold_call, cold);
        const auto cold_patch = addon::decode_launch_patch(cold_call.output, &failure, 3);
        require(cold_call.succeeded && cold_patch &&
            addon::validate_initial_prompt_acknowledgment(cold.initial_prompt, *cold_patch, &failure),
            "Slow native version and help checks must prepare the saved npm launcher within the host budget");
        Context short_help;
        prepare_launch(short_help, cold, 100);
        require(short_help.output == addon::Json_value(addon::reason::k_initial_prompt_unavailable) &&
            short_help.failure_detail.find("--help) timed out") != std::string::npos,
            "A timed-out help check after version verification must explain why prompt support is unknown");
        addon::Discover_request npm_discovery;
        npm_discovery.platform_id = environment.platform_id;
        npm_discovery.ambient_environment = npm_pet.launch_environment->effective_environment;
        const auto npm_candidates = codex::discover_launchers(npm_discovery);
        require(!npm_candidates.empty() && npm_candidates.front().script_body == "codex-pet",
            "The npm pet link must remain the preferred discoverable launcher");
        const auto& discovered = npm_candidates.front();
        std::vector<std::string> discovered_argv{discovered.script_body};
        if (discovered.runner_key == addon::k_runner_powershell) {
            discovered_argv = {"powershell.exe", "-NoLogo", "-Command", discovered.script_body};
        }
        Context discovered_probe;
        const auto discovered_launch = codex::verified_launch(
            {discovered.script_body, discovered.runner_key, discovered.environment_text},
            discovered_argv, npm_discovery.ambient_environment, utf8(root), false, discovered_probe);
        require(discovered_launch && discovered_launch->argv == npm_pet_prefix &&
            discovered_launch->prefix_size == npm_pet_prefix.size(),
            "A discovered npm pet launcher must verify the same Node, script and native payload chain");
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
        require_root_configuration(task_process->process.argv);
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
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
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
        for (const auto& arguments : k_permission_arguments) {
            const auto permission = with_arguments(prepare, arguments);
            auto request = plain;
            request.context.launcher = permission.launcher;
            request.context.launcher_argv = permission.launcher_argv;
            Context unsupported;
            unsupported.input = {"permissions", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(unsupported);
            require(!unsupported.succeeded &&
                unsupported.output == addon::Json_value("delegated_launcher_options_unavailable"),
                "Headless exec cannot silently replace an explicit human approval policy");
        }
        for (const auto& arguments : std::vector<std::vector<std::string>>{
                {"--sandbox", "workspace-write"},
                {"-s=read-only", "--ask-for-approval=never"},
                {"-sdanger-full-access", "-anever"},
                {"-c", "approvals_reviewer=\"auto_review\"", "-c", "approval_policy=\"on-request\"", "-anever"}}) {
            const auto permission = with_arguments(prepare, arguments);
            auto request = plain;
            request.context.launcher = permission.launcher;
            request.context.launcher_argv = permission.launcher_argv;
            Context permitted;
            permitted.input = {"permissions", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(permitted);
            const auto result = addon::decode_delegated_prepare_result(permitted.output, &failure);
            require(permitted.succeeded && result &&
                std::search(result->process.argv.begin(), result->process.argv.end(),
                    arguments.begin(), arguments.end()) != result->process.argv.end() &&
                std::find(result->process.argv.begin(), result->process.argv.end(), "--yolo") == result->process.argv.end() &&
                result->process.environment == plain_process->process.environment,
                "Delegation preserves sandbox and never-approval flags without adding a bypass");
            const auto exec = std::find(result->process.argv.begin(), result->process.argv.end(), "exec");
            const bool explicit_never = arguments.back() == "-anever" || arguments.back() == "--ask-for-approval=never";
            require(exec != result->process.argv.end() &&
                std::count(result->process.argv.begin(), exec, "approval_policy=\"never\"") == (explicit_never ? 1 : 0),
                "Explicit never retains precedence even when native automatic review restores configured policy");
            require_root_configuration(result->process.argv);
        }
        // Oracle: task-specific selection replaces only the requested native
        // fields; the selected saved launcher and account are not rewritten.
        for (const auto& model_arguments : std::vector<std::vector<std::string>>{
                {"--model", "gpt-6.1-sol"}, {"--model=gpt-6.1-sol"},
                {"-m", "gpt-6.1-sol"}, {"-m=gpt-6.1-sol"}})
        {
            const auto launcher = with_arguments(prepare, model_arguments);
            auto request = plain;
            request.context.launcher      = launcher.launcher;
            request.context.launcher_argv = launcher.launcher_argv;
            request.model_selection = addon::Delegated_model_selection{"gpt-6-astra", "xhigh"};
            const auto original = addon::encode_delegated_prepare_request(request);
            Context selected;
            selected.input = {"selection", {"delegated-session", 3}, "prepare", original};
            codex::handle_preparation(selected);
            const auto result = addon::decode_delegated_prepare_result(selected.output, &failure);
            require(selected.succeeded && result && result->model_selection == request.model_selection,
                "Delegation must acknowledge the exact requested model and reasoning effort");
            const auto& arguments = result->process.argv;
            const auto exec = std::find(arguments.begin(), arguments.end(), "exec");
            require(exec != arguments.end() && std::count(exec, arguments.end(), "--model") == 1 &&
                std::count(exec, arguments.end(), "gpt-6-astra") == 1 &&
                std::count(arguments.begin(), arguments.end(), "model_reasoning_effort=\"xhigh\"") == 1,
                "Requested model and effort must be encoded in native exec arguments");
            require(std::none_of(arguments.begin(), arguments.end(), [](const auto& argument) {
                    return argument.find("gpt-6.1-sol") != std::string::npos;
                }) && result->account_context && task_process->account_context &&
                addon::encode_account_context(*result->account_context) ==
                    addon::encode_account_context(*task_process->account_context) &&
                result->process.environment == plain_process->process.environment &&
                addon::encode_delegated_prepare_request(request) == original,
                "Selection must replace the conflicting model argument and preserve its source account and request");
        }
        for (const auto& selection : std::vector<addon::Delegated_model_selection>{
                {"provider/future-model", std::nullopt}, {std::nullopt, "future-native-effort"}})
        {
            auto request = plain;
            request.model_selection = selection;
            Context selected;
            selected.input = {"selection", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(selected);
            const auto result = addon::decode_delegated_prepare_result(selected.output, &failure);
            require(selected.succeeded && result && result->model_selection == request.model_selection &&
                std::count(result->process.argv.begin(), result->process.argv.end(), "--model") ==
                    (selection.model ? 1 : 0) &&
                std::count(result->process.argv.begin(), result->process.argv.end(),
                    "model_reasoning_effort=\"future-native-effort\"") == (selection.reasoning_effort ? 1 : 0),
                "Each optional native selection remains independent and has no frozen allowlist");
        }
        auto review = plain;
        review.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
        const auto review_launcher = with_arguments(prepare,
            {"--yolo", "-sworkspace-write", "--add-dir", utf8(root), "--enable", "apps", "--enable=hooks"});
        review.context.launcher      = review_launcher.launcher;
        review.context.launcher_argv = review_launcher.launcher_argv;
        review.context.launch_environment->effective_environment.set("CODEX_FIXTURE_CONFIGURATION",
            addon::Json_value(R"({"mcp_servers":{"external-server":{"enabled":true,"token":"private-canary"}}})"));
        Context reviewed;
        reviewed.input = {"review", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(review)};
        codex::handle_preparation(reviewed);
        const auto review_result = addon::decode_delegated_prepare_result(reviewed.output, &failure);
        require(reviewed.succeeded && review_result && !review_result->model_selection,
            "Read-only review must prepare with the saved account and default model");
        require_root_configuration(review_result->process.argv);
        for (const char* argument : {"read-only", "--ignore-rules", "approval_policy=\"never\"",
                "approvals_reviewer=\"user\"",
                "mcp_servers.external-server.enabled=false"})
        {
            require(std::count(review_result->process.argv.begin(), review_result->process.argv.end(), argument) == 1,
                "Read-only review must enforce its native command and external-tool ceiling");
        }
        require(std::count_if(review_result->process.argv.begin(), review_result->process.argv.end(),
            [](const auto& argument) {
                return argument.starts_with("features={") && argument.ends_with('}') &&
                    argument.find("apps=false") != std::string::npos &&
                    argument.find("plugins=false") != std::string::npos;
            }) == 1, "Review feature controls must use one native inline table");
        // Oracle: codex-cli 0.160.0 drops root controls when exec also receives
        // -c options. The complete host configuration, including both Task
        // servers, must precede exec and be the one inspected by config/read.
        auto complete_review = review;
#ifdef _WIN32
        const auto complete_launcher = with_arguments(npm, {"-c", "approval_policy=\"on-request\"", "-anever"});
#else
        const auto complete_launcher = with_arguments(prepare, {"-c", "approval_policy=\"on-request\"", "-anever"});
#endif
        complete_review.context.launcher      = complete_launcher.launcher;
        complete_review.context.launcher_argv = complete_launcher.launcher_argv;
        complete_review.model_selection = addon::Delegated_model_selection{"gpt-6-astra", "xhigh"};
        auto coordination = server;
        coordination.server_id = "coordination";
        coordination.actions   = {"status", "claim", "list", "release", "send", "receive", "notice_status", "fetch_notices"};
        auto delegation = server;
        delegation.server_id = "delegation";
        delegation.actions   = {"targets", "submit", "status", "result", "cancel"};
        complete_review.control.servers = {coordination, delegation};
        const auto receipt = root / "delegated-configuration.json";
        complete_review.context.launch_environment->effective_environment.set(
            "CODEX_FIXTURE_RECEIPT", addon::Json_value(utf8(receipt)));
        for (const auto purpose : {addon::Structured_purpose::IMPLEMENTATION, addon::Structured_purpose::READ_ONLY_REVIEW}) {
            auto request = complete_review;
            request.purpose = purpose;
            Context prepared;
            prepared.input = {"complete-delegation", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(prepared);
            const auto result = addon::decode_delegated_prepare_result(prepared.output, &failure);
            require(prepared.succeeded && result && result->process.argv.size() <= addon::limits::k_max_argv_entries &&
                addon::validate_structured_process_proposal(result->process, &failure) &&
                result->model_selection == request.model_selection &&
                result->acknowledgment.server_ids == std::vector<std::string>{"coordination", "delegation"},
                "The complete host invocation must fit the unchanged process limits");
            require_root_configuration(result->process.argv);
            addon::Json_value expected = addon::Json_value::array();
            for (const auto& argument : result->process.argv) {
                if (argument == "exec") break;
                // App-server receives configuration, not the interactive -a flag.
                if (argument != "-anever") expected.push_back(addon::Json_value(argument));
            }
            expected.push_back(addon::Json_value("app-server"));
            std::ifstream observed(receipt);
            std::string line;
            std::getline(observed, line);
            const auto record = addon::parse_json(line, {}, nullptr);
            require(record && record->find("argv") && *record->find("argv") == expected &&
                record->find("CODEX_HOME") && *record->find("CODEX_HOME") == addon::Json_value(utf8(root)),
                "Final native verification must inspect the complete launch configuration with the selected account");
        }
        // Explicitly disabled saved servers need no argv entries. Forty
        // redundant pairs alone would exceed the unchanged 64-entry cap.
        auto disabled_inventory = addon::Json_value::object();
        auto saved_servers = addon::Json_value::object();
        for (int index = 0; index < 40; ++index) {
            auto saved = addon::Json_value::object();
            saved.set("enabled", addon::Json_value(false));
            saved_servers.set("disabled-" + std::to_string(index), std::move(saved));
        }
        auto active_server = addon::Json_value::object();
        active_server.set("enabled", addon::Json_value(true));
        saved_servers.set("external-server", std::move(active_server));
        disabled_inventory.set("mcp_servers", std::move(saved_servers));
        auto many_disabled = complete_review;
        many_disabled.context.launch_environment->effective_environment.set(
            "CODEX_FIXTURE_CONFIGURATION", addon::Json_value(addon::serialize_json(disabled_inventory)));
        Context compact_inventory;
        compact_inventory.input = {"disabled-inventory", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(many_disabled)};
        codex::handle_preparation(compact_inventory);
        const auto compact_result = addon::decode_delegated_prepare_result(compact_inventory.output, &failure);
        require(compact_inventory.succeeded && compact_result &&
            compact_result->process.argv.size() <= addon::limits::k_max_argv_entries &&
            addon::validate_structured_process_proposal(compact_result->process, &failure) &&
            std::none_of(compact_result->process.argv.begin(), compact_result->process.argv.end(), [](const auto& argument) {
                return argument.starts_with("mcp_servers.disabled-");
            }) && std::count(compact_result->process.argv.begin(), compact_result->process.argv.end(),
                "mcp_servers.external-server.enabled=false") == 1,
            "Already-disabled servers must preserve argv headroom while active saved servers remain disabled");
        require_root_configuration(compact_result->process.argv);
        require(std::none_of(review_result->process.argv.begin(), review_result->process.argv.end(),
            [](const auto& argument) {
                return argument == "--yolo" || argument == "-sworkspace-write" || argument == "--add-dir";
            }) && review_result->account_context && task_process->account_context &&
            addon::encode_account_context(*review_result->account_context) ==
                addon::encode_account_context(*task_process->account_context) &&
            addon::serialize_json(reviewed.output).find("private-canary") == std::string::npos,
            "Read-only review must remove invocation-only write grants without exposing inventory credentials");
        for (const auto& inventory : {"not-json", R"({"mcp_servers":[]})",
                R"({"mcp_servers":{"unknown.name":{}}})", R"({"profile":"unsafe-profile"})",
                R"({"features":{"apps":true}})", R"({"sandbox_mode":"danger-full-access"})",
                R"({"mcp_servers":{"external-server":{"enabled":true}}})"})
        {
            auto request = review;
            request.context.launch_environment->effective_environment.set(
                "CODEX_FIXTURE_MANAGED_CONFIGURATION", addon::Json_value(inventory));
            Context unavailable;
            unavailable.input = {"review", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(unavailable);
            require(!unavailable.succeeded && unavailable.output == addon::Json_value("read_only_review_unavailable"),
                "An unverified native permission ceiling must refuse read-only review");
        }
        auto approval_review = plain;
        approval_review.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
        const auto approval_launcher = with_arguments(prepare, {"-sworkspace-write", "-aon-request"});
        approval_review.context.launcher      = approval_launcher.launcher;
        approval_review.context.launcher_argv = approval_launcher.launcher_argv;
        Context constrained_review;
        constrained_review.input = {"review-approval", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(approval_review)};
        codex::handle_preparation(constrained_review);
        const auto constrained_result = addon::decode_delegated_prepare_result(constrained_review.output, &failure);
        require(constrained_review.succeeded && constrained_result &&
            std::count(constrained_result->process.argv.begin(), constrained_result->process.argv.end(),
                "approval_policy=\"never\"") == 1 &&
            std::count(constrained_result->process.argv.begin(), constrained_result->process.argv.end(),
                "-aon-request") == 0,
            "Read-only review must constrain a saved human-approval launcher to noninteractive never approval");
        for (const auto purpose : {addon::Structured_purpose::IMPLEMENTATION, addon::Structured_purpose::READ_ONLY_REVIEW})
        {
            for (const auto& configuration : {R"({"model_reasoning_effort":"low"})",
                    R"({"model_reasoning_effort":null})", R"({"profile":"saved-profile"})", "not-json"})
            {
                auto request = plain;
                request.purpose = purpose;
                request.model_selection = addon::Delegated_model_selection{"gpt-6-astra", "xhigh"};
                request.context.launch_environment->effective_environment.set(
                    "CODEX_FIXTURE_MANAGED_CONFIGURATION", addon::Json_value(configuration));
                Context unavailable;
                unavailable.input = {"effort", {"delegated-session", 3}, "prepare",
                    addon::encode_delegated_prepare_request(request)};
                codex::handle_preparation(unavailable);
                const bool effort_mismatch = std::string_view(configuration).find("model_reasoning_effort") !=
                    std::string_view::npos;
                const char* expected = effort_mismatch ? "reasoning_effort_unavailable" :
                    purpose == addon::Structured_purpose::READ_ONLY_REVIEW
                        ? "read_only_review_unavailable" : "model_selection_unavailable";
                require(!unavailable.succeeded && unavailable.output == addon::Json_value(expected),
                    "Confirmed effort mismatch must be distinct from an unreadable or unresolved configuration");
            }
        }
        for (const auto& profile_arguments : std::vector<std::vector<std::string>>{
                {"-p", "review-profile"}, {"--profile=review-profile"}, {"-c", "profile=\"review-profile\""}})
        {
            const auto launcher = with_arguments(prepare, profile_arguments);
            auto request = review;
            request.context.launcher      = launcher.launcher;
            request.context.launcher_argv = launcher.launcher_argv;
            Context unavailable;
            unavailable.input = {"profile-review", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(unavailable);
            require(!unavailable.succeeded && unavailable.output == addon::Json_value("read_only_review_unavailable"),
                "Review cannot drop a profile selector unsupported by the native configuration reader");

            request.purpose = addon::Structured_purpose::IMPLEMENTATION;
            request.model_selection = addon::Delegated_model_selection{std::nullopt, "xhigh"};
            Context effort;
            effort.input = {"profile-effort", {"delegated-session", 3}, "prepare",
                addon::encode_delegated_prepare_request(request)};
            codex::handle_preparation(effort);
            require(!effort.succeeded && effort.output == addon::Json_value("model_selection_unavailable"),
                "An effort override must not be acknowledged from another profile's configuration");
        }
        auto unsupported_review = plain;
        unsupported_review.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
        unsupported_review.context.launch_environment->effective_environment.set(
            "CODEX_FIXTURE_NO_READ_ONLY", addon::Json_value("1"));
        Context unavailable_review;
        unavailable_review.input = {"review", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(unsupported_review)};
        codex::handle_preparation(unavailable_review);
        require(!unavailable_review.succeeded &&
            unavailable_review.output == addon::Json_value("read_only_review_unavailable"),
            "A CLI without the required read-only controls cannot advertise successful preparation");

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
        auto title_resume = resume;
        title_resume.context.launch_environment->effective_environment.set("CODEX_FIXTURE_TUI",
            addon::Json_value(R"({"terminal_title":[],"animations":false,"effects":{"progress":false}})"));
        Context exact;
        exact.input = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(title_resume, 2), 15000};
        codex::handle_preparation(exact);
        const auto proposal = addon::decode_resume_prepare_result(exact.output, &failure, 2);
        if (!exact.succeeded) std::cerr << "Resume prepare refusal: " << addon::serialize_json(exact.output) << "; " << exact.failure_detail << '\n';
        require(exact.succeeded && proposal && proposal->startup_observer && proposal->startup_observer->authorization == ("Bearer " + std::string(64, 'd')), "Isolated startup proof or exact observer echo missing");
        require(proposal->patch.session_wake && proposal->patch.session_wake->available,
            "Exact resume must retain the native wake binding for its new process");
        require(std::count(proposal->process.argv.begin(), proposal->process.argv.end(),
            R"(tui.terminal_title=["spinner"])") == 1,
            "Exact resume must retain activity in an explicitly empty native title");
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
        auto identity_resume = title_resume;
        identity_resume.context.terminal_identity = addon::Terminal_identity_offer{};
        identity_resume.context.launch_environment->effective_environment.set("CODEX_FIXTURE_VERSION", addon::Json_value("0.167.0"));
        Context identified_resume;
        identified_resume.input = {"identity-resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(identity_resume, 2), 15000};
        codex::handle_preparation(identified_resume);
        const auto identified = addon::decode_resume_prepare_result(identified_resume.output, &failure, 2);
        require(identified_resume.succeeded && identified && identified->patch.terminal_identity &&
            identified->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763" &&
            std::count(identified->process.argv.begin(), identified->process.argv.end(),
                R"(tui.terminal_title=["spinner","session-id","thread-name","thread"])") == 1,
            "Exact resume must prepare immediate identity without replacing its native UUID or display settings");
        const auto selected_directory = root / "non-git directory" / "nested";
        fs::create_directories(selected_directory);
        auto separated_directory = utf8(selected_directory / ".." / "." / "nested") + "//";
#ifdef _WIN32
        std::replace(separated_directory.begin(), separated_directory.end(), '\\', '/');
#endif
        {
            auto request = resume;
            request.context.working_directory = separated_directory;
            const auto original = addon::encode_resume_prepare_request(request);
            Context normalized;
            normalized.input = {"directory-spelling", {"interactive-resume", 2}, "prepare_resume", original, 15000};
            codex::handle_preparation(normalized);
            const auto result = addon::decode_resume_prepare_result(normalized.output, &failure, 2);
            require(normalized.succeeded && result &&
                result->process.argv[result->process.argv.size() - 2] == utf8(selected_directory) &&
                result->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763" &&
                addon::validate_resume_acknowledgment(request.resume_record, result->acknowledgment, &failure),
                "Equivalent directory spelling resumes the exact conversation in the normalized working directory");
            const auto* wake_directory = result->patch.session_wake
                ? result->patch.session_wake->binding.find("cwd") : nullptr;
            require(result->patch.session_wake && result->patch.session_wake->available && wake_directory &&
                *wake_directory == addon::Json_value(utf8(selected_directory)),
                "Resume wake binding retains the normalized directory used by the launched session");
        }
        for (const auto& saved_directory : std::vector<std::vector<std::string>>{
                {"-C", "/saved-directory"}, {"--cd", "/saved-directory"},
                {"--cd=/saved-directory"}, {"-C=/saved-directory"}, {"-C/saved-directory"}})
        {
            const auto saved = with_arguments(prepare, saved_directory);
            auto request = resume;
            request.context.launcher = saved.launcher;
            request.context.launcher_argv = saved.launcher_argv;
            request.context.working_directory = utf8(selected_directory);
            const auto original = addon::encode_resume_prepare_request(request);
            Context selected;
            selected.input = {"directory-selection", {"interactive-resume", 2}, "prepare_resume", original, 15000};
            codex::handle_preparation(selected);
            const auto result = addon::decode_resume_prepare_result(selected.output, &failure, 2);
            require(selected.succeeded && result &&
                result->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763" &&
                result->process.argv[result->process.argv.size() - 2] == utf8(selected_directory) &&
                result->process.argv[result->process.argv.size() - 3] == "-C" &&
                std::count(result->process.argv.begin(), result->process.argv.end(), "-C") == 1,
                "Exact resume must run in the working directory, a nested non-Git one included, after replacing saved Codex options");
            require(std::none_of(result->process.argv.begin(), result->process.argv.end(), [](const auto& argument) {
                    return argument.find("/saved-directory") != std::string::npos;
                }) && result->patch.account_context && proposal->patch.account_context &&
                addon::encode_account_context(*result->patch.account_context) ==
                    addon::encode_account_context(*proposal->patch.account_context) &&
                addon::encode_resume_prepare_request(request) == original,
                "A directory override preserves original account, exact conversation and immutable source recipe");
        }
        for (const auto& invalid_directory : std::vector<std::vector<std::string>>{
                {"-C"}, {"--cd="}, {"-C="}, {"-C", "/one", "--cd", "/two"}})
        {
            const auto saved = with_arguments(prepare, invalid_directory);
            auto request = resume;
            request.context.launcher = saved.launcher;
            request.context.launcher_argv = saved.launcher_argv;
            Context invalid;
            invalid.input = {"invalid-directory-option", {"interactive-resume", 2}, "prepare_resume",
                addon::encode_resume_prepare_request(request), 15000};
            codex::handle_preparation(invalid);
            require(!invalid.succeeded && invalid.output == addon::Json_value("resume_launcher_unsupported"),
                "Malformed or ambiguous saved directory options must be refused before native preparation");
        }
        for (const auto& arguments : k_permission_arguments) {
            const auto permission = with_arguments(prepare, arguments);
            auto request = resume;
            request.context.launcher = permission.launcher;
            request.context.launcher_argv = permission.launcher_argv;
            Context resumed;
            resumed.input = {"permissions", {"interactive-resume", 2}, "prepare_resume",
                addon::encode_resume_prepare_request(request, 2), 15000};
            codex::handle_preparation(resumed);
            const auto result = addon::decode_resume_prepare_result(resumed.output, &failure, 2);
            require(resumed.succeeded && result &&
                std::search(result->process.argv.begin(), result->process.argv.end(),
                    arguments.begin(), arguments.end()) != result->process.argv.end() &&
                result->process.argv.back() == proposal->process.argv.back() &&
                result->patch.set == proposal->patch.set && result->patch.account_context &&
                addon::encode_account_context(*result->patch.account_context) == addon::encode_account_context(account),
                "Exact resume preserves explicit permissions, session identity and selected profile");
        }
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
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
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
