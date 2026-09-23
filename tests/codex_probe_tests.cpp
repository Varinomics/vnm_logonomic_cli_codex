#include <logonomic_cli_addon/package_source.h>
#include "codex_accounts.h"
#include "codex_probes.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/presentation_record.h>

#include "codex_launcher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

namespace addon = logonomic::cli_addon;

class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool succeeded = false;
    bool done = false;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int timeout) override { std::this_thread::sleep_for(std::chrono::milliseconds(timeout)); return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override { done = true; output = addon::Json_value(std::move(reason)); }
    bool finished() const override { return done; }
};

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void auth_output_is_exact_and_never_echoes_credentials()
{
    addon::Probe_inspect_request request;
    request.inspection.final = true;
    request.inspection.exit_code = 0;
    request.inspection.stdout_bytes = "Logged in using ChatGPT\n";
    request.account_context = addon::Account_context{1, "selected-account"};
    auto result = codex::inspect_auth(request);
    check(result.complete && result.observation->state == addon::Auth_status::AUTHENTICATED, "successful login status");
    check(result.observation->scope.source_account_key == "selected-account", "auth attribution");
    request.inspection.exit_code = 1;
    request.inspection.stdout_bytes.clear();
    request.inspection.stderr_bytes = " \r\nNot logged in\r\n";
    check(codex::inspect_auth(request).observation->state == addon::Auth_status::NOT_LOGGED_IN, "exact logout status");
    request.inspection.stderr_bytes = "Not logged in: secret-token-canary";
    result = codex::inspect_auth(request);
    check(result.observation->state == addon::Auth_status::UNKNOWN, "unrecognized auth output stays unknown");
    check(addon::serialize_json(addon::encode_auth_inspect_result(result)).find("secret-token-canary") == std::string::npos,
        "auth output must not leak into normalized observation");
    request.inspection.timed_out = true;
    request.inspection.exit_code = 0;
    check(codex::inspect_auth(request).observation->state != addon::Auth_status::AUTHENTICATED, "timeout cannot authenticate");
}

void quota_protocol_and_optional_windows()
{
    addon::Probe_inspect_request request;
    request.inspection.protocol_state = "await_initialize";
    request.inspection.stdout_bytes = "{\"id\":\"1\",\"result\":{}}\n";
    std::string failure;
    auto result = codex::inspect_quota(request, &failure);
    check(!result.complete && result.protocol_state == "await_quota" && result.stdin_bytes, "initialization advances through stdin");
    check(result.stdin_bytes->find("account/rateLimits/read") != std::string::npos, "native quota method");
    request.inspection.protocol_state = result.protocol_state;
    request.inspection.stdout_bytes = "{\"id\":2,\"result\":{\"rateLimits\":{\"primary\":null,"
        "\"secondary\":{\"usedPercent\":12.5,\"windowDurationMins\":10080,\"resetsAt\":1800000000},"
        "\"planType\":\"plus\",\"credits\":{\"balance\":\"12.3400\"}},"
        "\"rateLimitResetCredits\":{\"availableCount\":2}}}\n";
    result = codex::inspect_quota(request, &failure);
    check(result.complete && result.observation->state == addon::Quota_status::READY, "quota completion");
    check(!result.observation->primary && result.observation->secondary->used_percent == 12.5, "secondary independent of primary");
    check(result.observation->secondary->resets_at_ms == 1800000000000ull, "reset instant preserved");
    check(result.observation->credits_balance == "12.3400" && result.observation->reset_credits == 2, "credits not rounded");
    addon::Contract_failure codec_failure;
    check(addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(result, 2), false, &codec_failure, 2).has_value(),
        "quota result public codec");
    request.inspection.stdout_bytes = "{\"id\":2,\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":1,\"windowDurationMins\":0}}}}\n";
    result = codex::inspect_quota(request, &failure);
    check(result.observation->state == addon::Quota_status::READY && !result.observation->primary->window_minutes,
        "native zero duration has no positive window metadata");
}

void quota_errors_remain_redacted()
{
    addon::Probe_inspect_request request;
    request.inspection.stdout_bytes = "{\"id\":2,\"error\":{\"message\":\"token secret-canary expired with 401\"}}\n";
    std::string failure;
    auto result = codex::inspect_quota(request, &failure);
    check(result.complete && result.observation->state == addon::Quota_status::FAILED && failure == "authentication_required",
        "native authentication-required signal");
    result.failure_reason = failure;
    const auto encoded = addon::serialize_json(addon::encode_quota_inspect_result(result, 2));
    check(encoded.find("secret-canary") == std::string::npos, "quota errors stay redacted");
    request.inspection.stdout_bytes = "{\"id\":2,\"error\":{\"message\":\"token is near expiry\"}}\n";
    failure.clear();
    codex::inspect_quota(request, &failure);
    check(failure.empty(), "credential mention alone is not rejection");
    request.inspection.stdout_bytes.clear();
    request.inspection.final = true;
    check(codex::inspect_quota(request, &failure).complete, "final missing response cannot remain pending");
}

void account_resolution_uses_frozen_origin()
{
    addon::Launch_environment environment;
#if defined(_WIN32)
    environment.platform_id = "windows-x86_64";
    const std::string origin = "C:/codex-frozen-origin";
    const std::string fallback = "C:/codex-default-home";
    const std::string home_name = "USERPROFILE";
#else
    environment.platform_id = "linux-x86_64";
    const std::string origin = "/codex-frozen-origin";
    const std::string fallback = "/codex-default-home";
    const std::string home_name = "HOME";
#endif
    environment.effective_environment = addon::Json_value::object();
    addon::Launcher_request launcher;
    launcher.script_body = "codex";
    std::string error;
    auto profile = codex::profile_selection(environment, launcher, fallback, origin, &error);
    check(profile && std::filesystem::path(profile->profile_path).filename() == ".codex", "frozen default fallback");
    environment.effective_environment.set(home_name, addon::Json_value(""));
    profile = codex::profile_selection(environment, launcher, fallback, origin, &error);
    check(profile && std::filesystem::path(profile->profile_path).parent_path() == std::filesystem::path(origin),
        "present empty home resolves relative to frozen host origin");
    environment.effective_environment.set("CODEX_HOME", addon::Json_value("relative-profile"));
    profile = codex::profile_selection(environment, launcher, fallback, origin, &error);
    check(profile && std::filesystem::path(profile->profile_path) == std::filesystem::path(origin) / "relative-profile",
        "relative CODEX_HOME never uses adapter cwd");
    check(profile->adapter_account_key == codex::profile_account_key(profile->profile_path), "stable opaque account identity");
}

void accounts_are_named_after_their_profile_folders()
{
    // Oracle: accounts@1 names an account with a filesystem profile after that
    // profile's folder, which tells it apart from the provider's other accounts.
#if defined(_WIN32)
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
    const std::string home_env = "USERPROFILE";
#else
    const std::string platform = "linux-x86_64";
    const std::string home     = "/home/jane";
    const std::string home_env = "HOME";
#endif
    const auto discovered_name = [&](const char* codex_home) {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "codex";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = platform;
        request.launch_environment.effective_environment = addon::Json_value::object();
        request.launch_environment.effective_environment.set(home_env, addon::Json_value(home));
        if (codex_home) request.launch_environment.effective_environment.set("CODEX_HOME", addon::Json_value(codex_home));
        request.default_home_fallback   = home;
        request.normalization_directory = home;
        Context context;
        context.input.operation = addon::k_accounts_discover_operation;
        context.input.payload   = addon::encode_accounts_launcher_request(request);
        codex::handle_accounts(context);
        addon::Contract_failure failure;
        const auto accounts = addon::decode_accounts_result(context.output, &failure);
        check(context.succeeded && accounts && accounts->size() == 1, "discovery returns the selected account");
        return accounts->front().display_name;
    };
    check(discovered_name(nullptr) == ".codex", "the default profile is named .codex");
    check(discovered_name((home + "/.codex-work").c_str()) == ".codex-work", "a CODEX_HOME profile is named after its folder");
    check(discovered_name((home + "/.codex-work/").c_str()) == ".codex-work", "a trailing separator does not hide the folder name");
    check(discovered_name((home + "/.codex-review").c_str()) != discovered_name((home + "/.codex-work").c_str()),
        "two profiles of one provider are named apart");
    const std::string long_home = home + "/.codex-account-for-the-varinomics-nightly-release-builds";
    check(discovered_name(long_home.c_str()) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
}

void launcher_discovery_returns_authenticated_codex_profiles(
    const std::filesystem::path& executable)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("codex discovery " + addon::random_nonce_hex());
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{root};

    const fs::path bin = root / "bin";
    const fs::path home = root / "profiles";
    const fs::path personal = home / ".codex-personal";
    const fs::path work = home / ".codex-work";
    const fs::path team = home / ".codex-team";
    const fs::path unauthenticated = home / ".codex-empty";
    const fs::path long_profile = home /
        (".codex-" + std::string(70, 'a'));
    fs::create_directories(bin);
    fs::create_directories(personal);
    fs::create_directories(work);
    fs::create_directories(team);
    fs::create_directories(unauthenticated);
#ifdef _WIN32
    const fs::path codex_program = bin / "codex.exe";
#else
    const fs::path codex_program = bin / "codex";
#endif
    fs::copy_file(executable, codex_program);
#ifndef _WIN32
    fs::permissions(
        codex_program,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
        fs::perm_options::replace);
#endif
    for (const fs::path& profile : {personal, work, team}) {
        std::ofstream auth(profile / "auth.json", std::ios::binary);
        auth << "{}";
        check(auth.good(), "create an authenticated-profile marker");
    }

    const auto path_text = [](const fs::path& path) {
        const auto bytes = path.generic_u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    const std::string assignment_prefix = "CODEX_HOME=";
    const std::string maximum_profile_path(
        addon::limits::k_max_environment_text_bytes - assignment_prefix.size(), 'x');
    const auto maximum_environment_text = codex::codex_profile_environment_text(
        maximum_profile_path);
    check(maximum_environment_text &&
        maximum_environment_text->size() == addon::limits::k_max_environment_text_bytes,
        "a CODEX_HOME assignment exactly at the SDK byte limit is retained");
    check(!codex::codex_profile_environment_text(maximum_profile_path + "x"),
        "a CODEX_HOME assignment above the SDK byte limit is rejected");
    check(!codex::codex_profile_environment_text(std::string("bad-\xFF", 5)),
        "a CODEX_HOME assignment with invalid UTF-8 is rejected");
    const auto utf8_scalar_count = [](std::string_view value) {
        return static_cast<std::size_t>(std::ranges::count_if(
            value, [](unsigned char byte) { return (byte & 0xC0) != 0x80; }));
    };
    addon::Discover_request request;
#ifdef _WIN32
    request.platform_id = "windows-x86_64";
    request.ambient_environment.set("USERPROFILE", addon::Json_value(path_text(home)));
#else
    request.platform_id = "linux-x86_64";
    request.ambient_environment.set("HOME", addon::Json_value(path_text(home)));
#endif
    request.ambient_environment.set("PATH", addon::Json_value(path_text(bin)));
    request.ambient_environment.set("CODEX_HOME", addon::Json_value(path_text(work)));
    request.default_home_fallback = path_text(home);

    const auto candidates = codex::discover_launchers(request);
    check(candidates.size() == 3, "discover each authenticated sibling Codex profile");
    addon::Contract_failure failure;
    const auto decoded = addon::decode_discover_result(
        addon::encode_candidates(candidates), request.platform_id, &failure);
    if (!decoded) {
        std::cerr << "discovery contract failure: " << failure.detail << '\n';
    }
    check(decoded && decoded->size() == candidates.size(),
        "profile candidates satisfy the public discovery contract");
    check(candidates[0].display_name == "Codex work" && candidates[0].is_default,
        "the active Codex profile is named and marked as the default");
    check(candidates[0].environment_text == "CODEX_HOME=" + path_text(work),
        "the active profile receives its own CODEX_HOME");
    check(candidates[1].display_name == "Codex personal" && !candidates[1].is_default,
        "a sibling profile is separately named");
    check(candidates[1].environment_text == "CODEX_HOME=" + path_text(personal),
        "a sibling profile receives its own CODEX_HOME");
    check(candidates[2].display_name == "Codex team" && !candidates[2].is_default,
        "all authenticated sibling profiles are returned");
    check(std::ranges::count_if(candidates, [](const auto& candidate) {
        return candidate.is_default;
    }) == 1, "exactly the active Codex profile is the discovery default");
    check(candidates[0].candidate_id != candidates[1].candidate_id &&
        candidates[1].candidate_id != candidates[2].candidate_id,
        "discovered profile candidate IDs are unique");

    fs::create_directories(long_profile);
    {
        std::ofstream auth(long_profile / "auth.json", std::ios::binary);
        auth << "{}";
        check(auth.good(), "create the long-label authenticated-profile marker");
    }
    const auto with_long_profile = codex::discover_launchers(request);
    const auto long_label = std::ranges::find_if(
        with_long_profile, [&long_profile, &path_text](const auto& candidate) {
            return candidate.environment_text.ends_with(path_text(long_profile));
        });
    addon::Contract_failure long_label_failure;
    const auto long_label_decoded = long_label == with_long_profile.end()
        ? std::optional<std::vector<addon::Launcher_candidate>>{}
        : addon::decode_discover_result(
            addon::encode_candidates({*long_label}), request.platform_id,
            &long_label_failure);
    check(long_label != with_long_profile.end() &&
        utf8_scalar_count(long_label->display_name) <= addon::limits::k_max_display_name_scalars &&
        long_label->display_name.size() <= addon::limits::k_max_display_name_bytes &&
        long_label_decoded.has_value(),
        "a long profile folder produces a discovery-contract-sized label");

#ifndef _WIN32
    const fs::path tab_profile = home / ".codex-tab\tprofile";
    fs::create_directories(tab_profile);
    {
        std::ofstream auth(tab_profile / "auth.json", std::ios::binary);
        auth << "{}";
        check(auth.good(), "create a tab-named authenticated profile marker");
    }
    const auto with_tab_profile = codex::discover_launchers(request);
    addon::Contract_failure tab_profile_failure;
    const auto decoded_tab_profiles = addon::decode_discover_result(
        addon::encode_candidates(with_tab_profile), request.platform_id,
        &tab_profile_failure);
    const auto tab_candidate = std::ranges::find_if(
        with_tab_profile, [&tab_profile, &path_text](const auto& candidate) {
            return candidate.environment_text.ends_with(path_text(tab_profile));
        });
    std::string tab_label_detail;
    const auto normalized_tab_label = tab_candidate == with_tab_profile.end()
        ? std::optional<std::string>{}
        : addon::normalized_display_name(tab_candidate->display_name, &tab_label_detail);
    check(decoded_tab_profiles && tab_candidate != with_tab_profile.end() &&
        normalized_tab_label == tab_candidate->display_name,
        "a tab in an authenticated profile folder yields a represented profile and valid control-free label");

    const std::string invalid_utf8_name = ".codex-invalid-\xFF";
    const fs::path invalid_utf8_profile = home / fs::path(invalid_utf8_name);
    fs::create_directories(invalid_utf8_profile);
    {
        std::ofstream auth(invalid_utf8_profile / "auth.json", std::ios::binary);
        auth << "{}";
        check(auth.good(), "create an invalid-UTF-8 authenticated profile marker");
    }
    const auto without_invalid_utf8 = codex::discover_launchers(request);
    const bool retained_valid_profiles = std::ranges::all_of(
        with_long_profile, [&without_invalid_utf8](const auto& expected) {
            return std::ranges::any_of(
                without_invalid_utf8, [&expected](const auto& actual) {
                    return actual.environment_text == expected.environment_text;
                });
        });
    check(retained_valid_profiles &&
        std::ranges::none_of(without_invalid_utf8, [&invalid_utf8_profile, &path_text](const auto& candidate) {
            return candidate.environment_text.ends_with(path_text(invalid_utf8_profile));
        }), "an invalid-UTF-8 profile is discarded without losing valid profiles");

    std::vector<fs::path> line_break_profiles;
    for (const std::string_view line_break : {"\r", "\n"}) {
        const fs::path profile = home / (".codex-unsafe" + std::string(line_break) + "name");
        fs::create_directories(profile);
        std::ofstream auth(profile / "auth.json", std::ios::binary);
        auth << "{}";
        line_break_profiles.push_back(profile);
    }
    const auto without_line_break_paths = codex::discover_launchers(request);
    check(std::ranges::none_of(without_line_break_paths, [](const auto& candidate) {
        return candidate.environment_text.find_first_of("\r\n") != std::string::npos;
    }), "profile paths containing CR or LF never enter launcher environment text");
    check(std::ranges::none_of(line_break_profiles, [&without_line_break_paths, &path_text](const auto& profile) {
        return std::ranges::any_of(without_line_break_paths, [&profile, &path_text](const auto& candidate) {
            return candidate.environment_text.ends_with(path_text(profile));
        });
    }), "profiles with CR or LF in their path are rejected from discovery");
#else
    std::cout << "SKIP: CR/LF profile paths cannot be created on Windows\n";
#endif

    const fs::path selected_target = root / "selected-target";
    const fs::path selected_link = home / ".codex-selected-link";
    const fs::path personal_alias = home / ".codex-personal-alias";
    fs::create_directories(selected_target);
    {
        std::ofstream auth(selected_target / "auth.json", std::ios::binary);
        auth << "{}";
    }
    std::error_code symlink_error;
    fs::create_directory_symlink(selected_target, selected_link, symlink_error);
    if (!symlink_error) {
        addon::Discover_request symlink_request = request;
        symlink_request.ambient_environment.set(
            "CODEX_HOME", addon::Json_value(path_text(selected_link)));
        const auto symlink_candidates = codex::discover_launchers(symlink_request);
        const auto selected = std::ranges::find_if(
            symlink_candidates, [](const auto& candidate) { return candidate.is_default; });
        check(selected != symlink_candidates.end() &&
            selected->environment_text.ends_with(path_text(selected_target)),
            "symlinked CODEX_HOME remains the selected canonical profile");
        check(std::ranges::any_of(symlink_candidates, [&personal, &path_text](const auto& candidate) {
            return candidate.environment_text.ends_with(path_text(personal));
        }), "sibling profiles are scanned beside the configured symlink");
        std::error_code alias_error;
        fs::create_directory_symlink(personal, personal_alias, alias_error);
        if (!alias_error) {
            const auto with_alias = codex::discover_launchers(symlink_request);
            const std::size_t personal_representations = std::ranges::count_if(
                with_alias, [&personal, &path_text](const auto& candidate) {
                    return candidate.environment_text.ends_with(path_text(personal));
                });
            check(personal_representations == 1,
                "a symlink alias does not duplicate its canonical authenticated profile");
        }
        else {
            std::cout << "SKIP: profile-alias symlink test is unsupported here: "
                      << alias_error.message() << '\n';
        }
    }
    else {
        std::cout << "SKIP: directory symlink discovery test is unsupported here: "
                  << symlink_error.message() << '\n';
    }

    for (int index = 0; index < 20; ++index) {
        const fs::path profile = home /
            (".codex-overflow-" + std::to_string(index));
        fs::create_directories(profile);
        std::ofstream auth(profile / "auth.json", std::ios::binary);
        auth << "{}";
    }
    codex::Codex_profile_discovery_stats capped_scan;
    const auto capped = codex::discover_launchers(request, &capped_scan);
    const auto retained_default = std::ranges::find_if(
        capped, [](const auto& candidate) { return candidate.is_default; });
    addon::Contract_failure capped_failure;
    const auto capped_decoded = addon::decode_discover_result(
        addon::encode_candidates(capped), request.platform_id, &capped_failure);
    check(capped.size() == addon::limits::k_max_discovery_candidates &&
        capped_scan.entries_examined <= codex::k_max_codex_profile_scan_entries &&
        capped_scan.profiles_retained == addon::limits::k_max_discovery_candidates &&
        retained_default != capped.end() &&
        retained_default->environment_text.ends_with(path_text(work)) &&
        capped_decoded.has_value(),
        "unsafe profiles are filtered before the 16-candidate limit, which retains the selected default");

    const fs::path bounded_home = root / "bounded-profile-scan";
    const fs::path bounded_selected = bounded_home / ".codex-selected";
    fs::create_directories(bounded_selected);
    {
        std::ofstream auth(bounded_selected / "auth.json", std::ios::binary);
        auth << "{}";
    }
    for (int index = 0; index < 300; ++index) {
        fs::create_directories(bounded_home / ("unrelated-" + std::to_string(index)));
    }
    addon::Discover_request bounded_request = request;
    bounded_request.ambient_environment.set(
        "CODEX_HOME", addon::Json_value(path_text(bounded_selected)));
    codex::Codex_profile_discovery_stats bounded_scan;
    const auto bounded_candidates = codex::discover_launchers(
        bounded_request, &bounded_scan);
    check(bounded_scan.entries_examined == codex::k_max_codex_profile_scan_entries &&
        bounded_scan.profiles_retained == 1 &&
        bounded_candidates.size() == 1 && bounded_candidates.front().is_default &&
        bounded_candidates.front().environment_text.ends_with(path_text(bounded_selected)),
        "profile scanning stops at its entry bound while retaining the configured default");

    addon::Discover_request empty_request = request;
    const fs::path empty_home = root / "empty home";
    empty_request.ambient_environment.set(
        "CODEX_HOME", addon::Json_value(path_text(empty_home / ".codex")));
    const auto fallback = codex::discover_launchers(empty_request);
    check(fallback.size() == 1 && fallback.front().candidate_id == "interactive" &&
        fallback.front().environment_text.empty() && fallback.front().is_default,
        "an installation without authenticated profiles retains generic discovery");
}

void probe_preparation_preserves_program_environment_and_custody(const std::filesystem::path& executable)
{
    const auto utf8 = [](const auto& path) { const auto bytes = path.u8string(); return std::string(bytes.begin(), bytes.end()); };
    addon::Probe_prepare_request request;
    request.context.launcher.runner_key = addon::k_runner_powershell;
    request.context.launcher.script_body = "codex";
    request.context.launcher_argv = {"powershell", "-NoLogo", "-Command", "codex"};
    request.context.working_directory = utf8(executable.parent_path());
    addon::Launch_environment environment;
#if defined(_WIN32)
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment.set("PATH", addon::Json_value(utf8(executable.parent_path())));
    environment.effective_environment.set("OPENAI_API_KEY", addon::Json_value("frozen-credential-canary"));
    request.context.launch_environment = environment;
    const auto profile = utf8(std::filesystem::temp_directory_path() / "codex-probe-isolated-profile");
    addon::Account_context account;
    account.version = 2;
    account.adapter_account_key = codex::profile_account_key(profile);
    account.profile_path = profile;
    request.context.account_context = account;
    for (const bool quota : {false, true}) {
        Context context;
        context.input = {"probe", {quota ? "quota" : "auth-status", 2}, "prepare", addon::encode_probe_prepare_request(request, 2)};
        codex::handle_probes(context);
        addon::Contract_failure failure;
        const auto result = addon::decode_structured_prepare_result(context.output, &failure, 2);
        check(context.succeeded && result.has_value(), "probe preparation through actual dispatch and public codec");
        check(result->process.executable.kind == addon::Process_executable::Kind::PACKAGE_PATH &&
            result->process.executable.path.starts_with("helpers/codex_launch_bridge"), "probe uses package bridge instead of frozen shell argv0");
        check(std::filesystem::equivalent(logonomic::cli_addon::path_from_utf8(result->process.argv.front()), executable),
            "recognized Codex executable preserved");
        check(result->process.argv[1] == (quota ? "app-server" : "login"), "native probe operation preserved");
        const auto contains = [&](std::string_view name, std::string_view value) {
            for (const auto& entry : result->process.environment) if (entry.first == name && entry.second == value) return true;
            return false;
        };
        check(contains("OPENAI_API_KEY", "frozen-credential-canary") && contains("CODEX_HOME", profile), "frozen account and auth environment preserved");
        check(result->account_context && result->account_context->profile_path == profile, "selected account locator echoed");
        check(result->inspection.keep_stdin_open == quota && result->inspection.incremental == quota,
            "quota alone keeps host-owned stdin open for continuation");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try {
        auth_output_is_exact_and_never_echoes_credentials();
        quota_protocol_and_optional_windows();
        quota_errors_remain_redacted();
        account_resolution_uses_frozen_origin();
        accounts_are_named_after_their_profile_folders();
        launcher_discovery_returns_authenticated_codex_profiles(
            std::filesystem::absolute(argv[1]));
        probe_preparation_preserves_program_environment_and_custody(std::filesystem::absolute(argv[1]));
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
