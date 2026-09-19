#include "codex_accounts.h"
#include "codex_probes.h"

#include <filesystem>
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
        check(std::filesystem::equivalent(std::filesystem::u8path(result->process.argv.front()), executable),
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
        probe_preparation_preserves_program_environment_and_custody(std::filesystem::absolute(argv[1]));
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
