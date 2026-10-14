#include "codex_accounts.h"
#include "codex_preparation.h"
#include "codex_session_wake.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    Json output;
    bool succeeded = false;
    bool done = false;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int timeout) override { std::this_thread::sleep_for(std::chrono::milliseconds(timeout)); return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override { done = true; output = Json(std::move(reason)); }
    bool finished() const override { return done; }
};

void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
std::string utf8(const fs::path& path) { const auto value = path.generic_u8string(); return {value.begin(), value.end()}; }
std::string text(const Json& value, const char* name) { const auto* field = value.find(name); return field && field->is_string() ? field->as_string() : ""; }

struct Temporary_directory
{
    fs::path path = fs::temp_directory_path() / ("codex-wake-test-" + addon::random_nonce_hex());
    Temporary_directory() { fs::create_directory(path); }
    ~Temporary_directory() { std::error_code error; fs::remove_all(path, error); }
};

addon::Launch_patch prepare(const addon::Prepare_request& request)
{
    Context context;
    context.input = {"prepare", {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3)};
    codex::handle_preparation(context);
    const auto patch = addon::decode_launch_patch(context.output, nullptr, 3);
    require(context.succeeded && patch.has_value(), "Wake negotiation prevented an ordinary prepared launch");
    return *patch;
}

std::vector<Json> records(const fs::path& path, const std::string& method)
{
    std::ifstream input(path, std::ios::binary);
    std::vector<Json> result;
    for (std::string line; std::getline(input, line);) {
        const auto value = addon::parse_json(line, {}, nullptr);
        require(value.has_value(), "Invalid fixture request record");
        if (text(*value, "method") == method) result.push_back(*value);
    }
    return result;
}

addon::Session_wake_readiness inspect(const addon::Session_wake_target& target)
{
    Context context;
    context.input = {"inspect", {"session-wake", 1}, "inspect", addon::encode_session_wake_inspect_request({target})};
    codex::handle_session_wake(context);
    const auto result = addon::decode_session_wake_readiness(context.output, nullptr);
    require(context.succeeded && result.has_value(), "Invalid readiness reply");
    return *result;
}

addon::Session_wake_notify_result notify(const addon::Session_wake_target& target)
{
    Context context;
    context.input = {"notify", {"session-wake", 1}, "notify", addon::encode_session_wake_notify_request(
        {target, "wake-notification-1", "Read mailbox: quotes \" \\ and Unicode Å 概要; $HOME %PATH%"})};
    codex::handle_session_wake(context);
    const auto result = addon::decode_session_wake_notify_result(context.output, nullptr);
    require(context.succeeded && result.has_value(), "Invalid notify reply");
    return *result;
}

// Opt-in native gate driver. The Python harness supplies only its newly created
// disposable profile and thread; ordinary CTest always uses the fake producer.
int native_probe(char** argv)
{
    addon::Prepare_request request;
    request.launcher = {"codex", addon::k_runner_direct, ""};
    request.launcher_argv = {argv[2]};
    request.launcher_argv.insert(request.launcher_argv.end(), {"-c", "sqlite_home=" + codex::toml_string(argv[6])});
    request.working_directory = argv[4];
    request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    request.launch_environment->platform_id = "windows-x86_64";
#else
    request.launch_environment->platform_id = "linux-x86_64";
#endif
    for (const char* name : {"PATH", "SystemRoot", "WINDIR", "TEMP", "TMP"}) {
        if (const char* value = std::getenv(name)) request.launch_environment->effective_environment.set(name, Json(value));
    }
    request.launch_environment->effective_environment.set("CODEX_HOME", Json(argv[3]));
    request.account_context = addon::Account_context{2, codex::profile_account_key(argv[3]), std::string(argv[3])};
    request.session_wake = addon::Session_wake_offer{};
    const auto patch = prepare(request);
    if (!patch.session_wake || !patch.session_wake->available) {
        throw std::runtime_error("Native preparation: " + (patch.session_wake ? patch.session_wake->reason : "missing"));
    }
    addon::Session_wake_target target;
    target.provider_session_id = argv[5];
    target.working_directory = request.working_directory;
    target.launcher_argv = *patch.argv;
    target.launch_environment = *request.launch_environment;
    target.correlation.transition_nonce = std::string(32, 'b');
    target.binding = patch.session_wake->binding;
    const auto ready = inspect(target);
    if (!ready.available) throw std::runtime_error("Native inspect: " + ready.reason);
    const auto result = notify(target);
    std::cout << addon::serialize_json(addon::encode_session_wake_notify_result(result)) << '\n';
    require(result.disposition == addon::Session_wake_disposition::ACCEPTED, "Native notification was not accepted");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 7 && std::string_view(argv[1]) == "--native") return native_probe(argv);
        require(argc == 2, "Expected fake native CLI");
        Temporary_directory temporary;
        const auto root = temporary.path;
        const auto log = root / "native-requests.jsonl";
        addon::Prepare_request request;
        request.launcher = {"codex", addon::k_runner_direct, ""};
        request.launcher_argv = {argv[1]};
        request.working_directory = utf8(root);
        request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
        request.launch_environment->platform_id = "windows-x86_64";
#else
        request.launch_environment->platform_id = "linux-x86_64";
#endif
        auto& environment = request.launch_environment->effective_environment;
        for (const char* name : {"PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "HOME"}) {
            if (const char* value = std::getenv(name)) environment.set(name, Json(value));
        }
        environment.set("CODEX_FIXTURE_WAKE_LOG", Json(utf8(log)));
        environment.set("CODEX_HOME", Json(utf8(root / "wrong-profile")));
        environment.set("CODEX_SQLITE_HOME", Json("relative-store"));
        request.account_context = addon::Account_context{2, codex::profile_account_key(utf8(root)), utf8(root)};
        request.session_wake = addon::Session_wake_offer{};

        // Oracle: rust-v0.160.0 external queue watcher and thread/queue/add
        // protocol, not a second native thread/resume or turn/start invocation.
        const auto patch = prepare(request);
        require(patch.session_wake && patch.session_wake->available, "Native queue birth evidence missing");
        const auto& binding = patch.session_wake->binding;
        require(text(binding, "codex_home") == utf8(root) && text(binding, "sqlite_home") == utf8(root / "relative-store"),
            "Selected account or relative SQLite store was lost");
        require(patch.argv && std::count(patch.argv->begin(), patch.argv->end(),
            "sqlite_home=" + codex::toml_string(utf8(root / "relative-store"))) == 1,
            "Recipient store must be pinned before birth");

        addon::Session_wake_target target;
        target.provider_session_id = "019933a1-6e43-7102-a520-b3c8294cc763";
        target.working_directory = request.working_directory;
        target.launcher_argv = *patch.argv;
        target.launch_environment = *request.launch_environment;
        target.correlation.transition_nonce = std::string(32, 'a');
        // The real host carries opaque preparation through QJsonObject and
        // QVariantMap, which sort object keys. Match that wire round trip:
        // file evidence changes from path,size,modified to modified,path,size.
        const auto transported_binding = addon::parse_json(addon::serialize_json_canonical(binding), {}, nullptr);
        require(transported_binding && transported_binding->find("files")->as_array().front().as_object().front().name == "modified",
            "The host-style regression must actually reorder nested file evidence");
        target.binding = *transported_binding;
        const auto ready = inspect(target);
        if (!ready.available) {
            std::cerr << "Readiness: " << ready.reason << '\n';
            std::ifstream diagnostic(log);
            std::cerr << diagnostic.rdbuf();
        }
        require(ready.available, "Exact native queue could not be inspected");
        const auto receipt = notify(target);
        require(receipt.disposition == addon::Session_wake_disposition::ACCEPTED && receipt.receipt &&
            *receipt.receipt == "019933a1-6e43-7102-a520-b3c8294cc764", "Native acknowledgment must retain queue receipt");
        auto sent = records(log, "thread/queue/add");
        require(sent.size() == 1 && text(sent.back(), "codex_home") == utf8(root) &&
            text(sent.back(), "sqlite_home") == utf8(root / "relative-store"), "Queue writer used another account/store");
        require(records(log, "thread/resume").empty() && records(log, "turn/start").empty(), "Wake must never load or run another conversation");

        target.launch_environment.effective_environment.set("CODEX_SQLITE_HOME", Json(utf8(root / "changed-store")));
        require(notify(target).disposition == addon::Session_wake_disposition::ACCEPTED, "Frozen store binding should survive environment/config drift");
        sent = records(log, "thread/queue/add");
        require(text(sent.back(), "sqlite_home") == utf8(root / "relative-store"), "New disk configuration moved an existing queue");
        for (const std::string scenario : {"lost-reply", "malformed", "wrong-echo", "unsupported"}) {
            const auto before = records(log, "thread/queue/add").size();
            auto uncertain = target;
            uncertain.launch_environment.effective_environment.set("CODEX_FIXTURE_WAKE", Json(scenario));
            require(notify(uncertain).disposition == addon::Session_wake_disposition::UNCERTAIN,
                "A request without a matching acknowledgment must remain uncertain");
            require(records(log, "thread/queue/add").size() == before + 1, "An ambiguous request must never be automatically retried");
        }
        auto unavailable = target;
        unavailable.launch_environment.effective_environment.set("CODEX_FIXTURE_MANAGED_STORE", Json(utf8(root / "managed-store")));
        const auto before_managed = records(log, "thread/queue/add").size();
        require(!inspect(unavailable).available && notify(unavailable).disposition == addon::Session_wake_disposition::UNAVAILABLE &&
            records(log, "thread/queue/add").size() == before_managed,
            "Managed requirements that outrank -c must refuse before the mutating queue call");
        unavailable = target;
        unavailable.launch_environment.effective_environment.set("CODEX_FIXTURE_WAKE", Json("init-failure"));
        const auto count = records(log, "thread/queue/add").size();
        require(notify(unavailable).disposition == addon::Session_wake_disposition::UNAVAILABLE &&
            records(log, "thread/queue/add").size() == count, "Failure before initialize must not claim native acceptance");
        unavailable = target;
        unavailable.provider_session_id = "friendly-session-name";
        require(!inspect(unavailable).available && notify(unavailable).disposition == addon::Session_wake_disposition::UNAVAILABLE,
            "Session names must never reach native lookup");
        unavailable = target;
        unavailable.working_directory += "/another";
        require(!inspect(unavailable).available, "A stale target binding must fail closed");
        unavailable = target;
        unavailable.binding.set("version", Json("0.148.0"));
        require(!inspect(unavailable).available, "A new CLI on disk cannot upgrade the original process watcher");
        unavailable = target;
        auto files = *unavailable.binding.find("files");
        files.as_array().front().set("modified", Json("different"));
        unavailable.binding.set("files", std::move(files));
        require(!inspect(unavailable).available && records(log, "thread/queue/add").size() == count,
            "An executable changed since birth must not execute a queue command");

        auto old = request;
        old.launch_environment->effective_environment.set("CODEX_FIXTURE_VERSION", Json("0.148.0"));
        const auto old_patch = prepare(old);
        require(old_patch.session_wake && !old_patch.session_wake->available &&
            old_patch.session_wake->reason == "native_queue_watcher_unavailable", "Unsupported wake must leave an ordinary launch available");
        auto configured = request;
        configured.launch_environment->effective_environment.set("CODEX_FIXTURE_SQLITE_HOME", Json(utf8(root / "configured-store")));
        const auto configured_patch = prepare(configured);
        require(configured_patch.session_wake && configured_patch.session_wake->available &&
            text(configured_patch.session_wake->binding, "sqlite_home") == utf8(root / "configured-store"),
            "Native sqlite_home configuration takes precedence over CODEX_SQLITE_HOME");
        configured.launch_environment->effective_environment.set("CODEX_FIXTURE_SQLITE_HOME", Json("relative-config-root"));
        require(!prepare(configured).session_wake->available, "Unresolved configuration paths must not route a queue");
        auto profiled = request;
        profiled.launcher.script_body = "codex --profile wake";
        profiled.launcher_argv.insert(profiled.launcher_argv.end(), {"--profile", "wake"});
        require(!prepare(profiled).session_wake->available,
            "Native app-server rejects profile-v2; dropping the selector would inspect another store");
        request.session_wake.reset();
        require(!prepare(request).session_wake, "Unnegotiated wake must not modify ordinary launch behavior");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
