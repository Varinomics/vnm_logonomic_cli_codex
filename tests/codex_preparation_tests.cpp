#include "codex_accounts.h"
#include "codex_launcher.h"
#include "codex_preparation.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

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
        const auto root = fs::temp_directory_path() / ("codex-preparation-" + addon::random_nonce_hex());
        fs::create_directories(root);
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
        addon::Launch_environment environment;
#ifdef _WIN32
        environment.platform_id = "windows-x86_64";
#else
        environment.platform_id = "linux-x86_64";
#endif
        environment.effective_environment.set("CODEX_HOME", addon::Json_value(utf8(root)));
        environment.effective_environment.set("TEMP", addon::Json_value(utf8(root)));
        if (const char* system_root = std::getenv("SystemRoot")) environment.effective_environment.set("SystemRoot", addon::Json_value(system_root));
        std::vector<std::pair<std::string, std::string>> probe_environment;
        for (const auto& member : environment.effective_environment.as_object()) probe_environment.emplace_back(member.name, member.value.as_string());
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
            codex::Probe_exchange exchange;
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
        prepare.launcher.script_body = "codex --yolo";
        prepare.launcher_argv        = {argv[1], "--yolo"};
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
        if (!addon::validate_launch_patch(*patch, {}, {}, &failure)) {
            throw std::runtime_error(failure.reason_code + ": " + failure.detail);
        }
        for (const std::string event : {"SessionStart", "SessionEnd", "UserPromptSubmit", "Stop",
                "Interrupt", "PermissionRequest", "SubagentStart", "SubagentStop"})
        {
            const auto prefix = "hooks." + event + "=";
            require(std::count_if(patch->argv->begin(), patch->argv->end(), [&](const auto& argument) {
                return argument.starts_with(prefix);
            }) == 1, "Each required lifecycle hook must be configured exactly once");
        }
        addon::Delegated_prepare_request delegated;
        delegated.context.launcher           = prepare.launcher;
        delegated.context.launcher_argv      = prepare.launcher_argv;
        delegated.context.working_directory = prepare.working_directory;
        delegated.context.launch_environment = environment;
        delegated.context.account_context = account;
        delegated.prompt = "Implement the selected repository change.";
        delegated.control.task_id = "task-a";
        delegated.control.private_artifact_root = utf8(root);
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
        task.input = {"delegated", {"delegated-session", 2}, "prepare", addon::encode_delegated_prepare_request(delegated, 2)};
        codex::handle_preparation(task);
        const auto task_process = addon::decode_delegated_prepare_result(task.output, &failure, 2);
        require(task.succeeded && task_process && task_process->private_artifacts.size() == 1 &&
            task_process->acknowledgment.server_ids == std::vector<std::string>{"control"}, "Delegated control acknowledgment or private wrapper missing");
        require(task_process->private_artifacts.front().content.find(" mcp --stdio") == std::string::npos,
            "Exact task-server entrypoint must not acquire provider-invented arguments");
        // Assert the actual emitted private wrapper, not a second producer.
        const auto quote = [](std::string_view value) {
            std::string result = "'";
            for (const char character : value) {
                if (character == '\'') {
#ifdef _WIN32
                    result += "''";
#else
                    result += "'\"'\"'";
#endif
                } else result += character;
            }
            return result + '\'';
        };
        const auto& wrapper = task_process->private_artifacts.front().content;
#ifdef _WIN32
        const auto exact_execution = "& " + quote(server.program) + "\nexit $LASTEXITCODE\n";
#else
        const auto exact_execution = "exec " + quote(server.program) + "\n";
#endif
        require(wrapper.ends_with(exact_execution), "Exact supplied control entrypoint gained arguments");
        for (const auto& [name, value] : server.environment) {
#ifdef _WIN32
            const auto assignment = "$env:" + name + " = " + quote(value) + '\n';
#else
            const auto assignment = "export " + name + "=" + quote(value) + '\n';
#endif
            require(wrapper.find(assignment) != std::string::npos,
                "Control wrapper changed an exact supplied environment assignment");
        }
        require(addon::validate_structured_process_proposal(task_process->process, &failure),
            "Delegated producer must satisfy the host process policy");
        require(std::find(task_process->process.environment_names.begin(), task_process->process.environment_names.end(),
            "CODEX_HOME") == task_process->process.environment_names.end(), "Selected profile must not also be inherited");
        require(std::count(task_process->process.environment.begin(), task_process->process.environment.end(),
            std::pair<std::string, std::string>{"CODEX_HOME", utf8(root)}) == 1, "Selected profile must be assigned exactly once");
        require(task_process->process.stdin_payload == delegated.prompt && task_process->process.argv.back() == "-", "Delegated prompt stdin changed");
        require(std::any_of(task_process->process.argv.begin(), task_process->process.argv.end(), [](const auto& argument) {
            return argument.starts_with("mcp_servers.logonomic_") && argument.ends_with("required=true");
        }), "Task control MCP server was not required");
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
        Context exact;
        exact.input = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume, 2), 15000};
        codex::handle_preparation(exact);
        const auto proposal = addon::decode_resume_prepare_result(exact.output, &failure, 2);
        if (!exact.succeeded) std::cerr << "Resume prepare refusal: " << addon::serialize_json(exact.output) << "; " << exact.failure_detail << '\n';
        require(exact.succeeded && proposal && proposal->startup_observer && proposal->startup_observer->authorization == ("Bearer " + std::string(64, 'd')), "Isolated startup proof or exact observer echo missing");
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
        for (const auto& argument : proposal->process.argv) require(argument.find(std::string(64, 'd')) == std::string::npos, "Startup secret escaped into argv");
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
