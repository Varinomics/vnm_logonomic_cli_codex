#include "codex_accounts.h"
#include "codex_conversation.h"
#include "codex_hook_metadata.h"
#include "codex_lifecycle.h"
#include "codex_preparation.h"
#include "codex_resume_record.h"
#include "codex_session_wake.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::string_view k_session_a = "019933a1-6e43-7102-a520-b3c8294cc763";
constexpr std::string_view k_session_b = "019933a1-6e43-7102-a520-b3c8294cc764";

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(static_cast<bool>(output), "write isolated native fixture");
}

void select_rollout(const fs::path& store, const fs::path& path, std::string_view mode = "legacy")
{
    fs::create_directories(store);
    sqlite3* database = nullptr;
    check(sqlite3_open(addon::path_to_utf8(store / "state_5.sqlite").c_str(), &database) == SQLITE_OK,
        "open native state fixture");
    check(sqlite3_exec(database,
        "CREATE TABLE IF NOT EXISTS threads (id TEXT PRIMARY KEY, rollout_path TEXT, history_mode TEXT)",
        nullptr, nullptr, nullptr) == SQLITE_OK, "create native state table");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO threads VALUES (?1, ?2, ?3)",
        -1, &statement, nullptr) == SQLITE_OK, "prepare exact native selection");
    const auto filename = addon::path_to_utf8(path);
    check(sqlite3_bind_text(statement, 1, k_session_a.data(), static_cast<int>(k_session_a.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, filename.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 3, mode.data(), static_cast<int>(mode.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE, "select exact native rollout");
    sqlite3_finalize(statement);
    sqlite3_close(database);
}

std::string metadata(bool paginated = false)
{
    return std::string(paginated ? "{\"ordinal\":0," : "{") +
        "\"type\":\"session_meta\",\"payload\":{\"id\":\"" + std::string(k_session_a) +
        "\",\"history_mode\":\"" + (paginated ? "paginated" : "legacy") + "\"}}\n";
}

void native_activation_facts(const fs::path& fixture)
{
    addon::Classify_event_request request;
    request.event.name = "SessionStart";
    request.execution_context_version = 1;
    const auto directory = addon::path_to_utf8(fixture / "non-git folder" / "nested");
    const auto profile = addon::path_to_utf8(fixture / "profile");
    const auto store = addon::path_to_utf8(fixture / "store");
    addon::Execution_context_update first_activation;
    for (int index = 0; index < 3; ++index) {
        const auto id = index == 1 ? k_session_b : k_session_a;
        const std::string source = index == 0 ? "startup" : index == 1 ? "clear" : "resume";
        request.transition_nonce = std::string(32, static_cast<char>('a' + index));
        auto native = Json::object();
        native.set("hook_event_name", Json("SessionStart"));
        native.set("session_id", Json(std::string(id)));
        native.set("source", Json(source));
        native.set("cwd", Json(directory));
        // A caller-controlled extra field must not survive the native projection.
        native.set("resume_record", Json("not preparer evidence"));
        request.event.body = codex::hook_metadata(native);
        check(!request.event.body.find("resume_record"), "hook JSON cannot nominate a native store");
        const auto record = codex::bound_resume_record(id, profile, store);
        check(record.has_value(), "create exact native scope");
        request.event.body.set("resume_record", *record);
        const auto result = codex::classify_hook(request);
        check(result && result->classification == addon::Classification::TRANSITION &&
            result->transition == (index == 2 ? addon::Transition_kind::RESUME : addon::Transition_kind::START) &&
            result->observations.size() == 1, "native A to B to A retains activation kind");
        const auto& observation = result->observations.front();
        check(observation.provider_session_id == id && observation.resume_record == record &&
            observation.session_source == source &&
            observation.session_transition == addon::Session_transition::STARTED,
            "native start carries exactly one identity and original resume target");
        check(result->execution_updates.size() == 1, "one joint native cwd observation");
        const auto& update = result->execution_updates.front();
        check(addon::is_valid_execution_context_update(update) && update.cwd == directory &&
            update.transition_nonce == request.transition_nonce && update.sequence == 0 &&
            update.status == addon::Execution_context_status::CONFIRMED && update.environment == "local",
            "exact native cwd is attributed to the activation, without Git inference");
        addon::Execution_context_state state;
        state.provider_session_id = std::string(id);
        state.transition_nonce = request.transition_nonce;
        check(addon::apply_execution_context_update(state, update), "native cwd applies to its activation");
        check(!addon::apply_execution_context_update(state, update), "duplicate cwd does not apply twice");
        if (index == 0) {
            first_activation = update;
        }
        else {
            check(!addon::apply_execution_context_update(state, first_activation),
                "old A cwd cannot move B or the new activation of A");
        }
    }
    for (const auto source : {"compact", "resume", "clear", "fork"}) {
        request.event.body.set("source", Json(source));
        const auto result = codex::classify_hook(request);
        check(result.has_value(), "recognized native source");
        if (std::string_view(source) == "compact") {
            check(result->classification == addon::Classification::IGNORED && result->observations.empty() &&
                result->execution_updates.empty(), "same-conversation compaction cannot invent a departure");
        }
        else {
            check(result->observations.size() == 1, "native new/resume/fork preserves identity");
        }
    }
    for (const auto& cwd : {Json(), Json("relative"), Json("/invalid\npath")}) {
        request.event.body.set("cwd", cwd);
        const auto result = codex::classify_hook(request);
        check(result && result->observations.size() == 1 && result->execution_updates.size() == 1 &&
            result->execution_updates.front().status == addon::Execution_context_status::UNKNOWN &&
            result->execution_updates.front().cwd.empty(), "invalid cwd does not hide native identity or borrow launch cwd");
    }
    request.execution_context_version = 0;
    check(codex::classify_hook(request)->execution_updates.empty(), "older host receives no cwd envelope");
    request.execution_context_version = 1;
    request.execution_failure = addon::Contract_failure{"unsupported", "optional capability"};
    check(codex::classify_hook(request)->execution_updates.empty(), "optional failure does not attach cwd");
    request.execution_failure.reset();
    request.event.name = "UserPromptSubmit";
    request.event.body.set("hook_event_name", Json(request.event.name));
    request.event.body.set("turn_id", Json("turn-one"));
    request.event.body.set("cwd", Json(directory));
    request.event.body.set("prompt", Json("/resume another-conversation"));
    const auto turn = codex::classify_hook(request);
    check(turn && turn->classification == addon::Classification::EVENT && turn->execution_updates.empty(),
        "command text and per-turn cwd cannot manufacture selection or default-directory changes");
    request.event.name = "SessionEnd";
    request.event.body.set("hook_event_name", Json(request.event.name));
    request.event.body.set("reason", Json("clear"));
    const auto ended = codex::classify_hook(request);
    check(ended && ended->classification == addon::Classification::EVENT &&
        ended->observations.front().provider_session_id == k_session_a &&
        ended->observations.front().session_end_reason == "clear", "departed end retains A for host epoch fencing");
}

addon::Resume_inspect_request inspection_request(const fs::path& fixture)
{
    const auto profile = addon::path_to_utf8(fixture / "profile");
    // Native preparation publishes its verified profile and store with generic
    // separators; account discovery retains the Windows-native profile spelling.
    const auto prepared_profile = (fixture / "profile").generic_u8string();
    const auto store = (fixture / "store").generic_u8string();
    addon::Resume_inspect_request request;
    request.context.launcher = {"codex", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"codex"};
    request.context.working_directory = addon::path_to_utf8(fixture);
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), Json::object(), std::nullopt};
    request.context.launch_environment->effective_environment.set("CODEX_HOME", Json(profile));
    std::string error;
    const auto selected = codex::profile_selection(*request.context.launch_environment,
        request.context.launcher, {}, request.context.working_directory, &error);
    check(selected.has_value(), "production account selection: " + error);
    request.context.account_context = addon::Account_context{
        2, selected->adapter_account_key, selected->profile_path};
    request.context.launch_environment->effective_environment.set("CODEX_HOME", Json("/unrelated/current-profile"));
    request.resume_record = *codex::bound_resume_record(k_session_a,
        std::string(prepared_profile.begin(), prepared_profile.end()),
        std::string(store.begin(), store.end()));
    return request;
}

bool same_profile_spelling(const fs::path& first, const fs::path& second)
{
#ifdef _WIN32
    std::error_code error;
    return fs::equivalent(first, second, error) && !error;
#else
    return first == second;
#endif
}

void native_history(const fs::path& fixture)
{
    auto request = inspection_request(fixture);
    const auto source = fixture / "profile/sessions/selected.jsonl";
    const auto store = fixture / "store";
    select_rollout(store, source);
    const auto expect = [&](addon::Resume_availability availability, addon::Resume_content content) {
        const auto result = codex::inspect_resume(request, {});
        check(result.availability == availability && result.content == content, "native history availability/content");
        check(result.request_digest_hex == addon::resume_inspect_request_digest_hex(request), "exact request digest");
    };
    using Availability = addon::Resume_availability;
    using Content = addon::Resume_content;
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    write(source, metadata());
    expect(Availability::AVAILABLE, Content::EMPTY);
    const std::string prompt =
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Retained work"}]}})" "\n";
    write(source, metadata() + prompt);
    expect(Availability::AVAILABLE, Content::NONEMPTY);
    write(source, metadata() +
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"<environment_context>native context</environment_context>"}]}})" "\n");
    expect(Availability::AVAILABLE, Content::EMPTY);
    write(source, metadata() + "{\"type\":\"response_item\"");
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    write(source, metadata() + prompt);
    expect(Availability::AVAILABLE, Content::NONEMPTY);
    write(source, metadata() + "not json\n");
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    write(source, metadata() + R"({"type":"future_history","payload":{"content":"unknown"}})" "\n");
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    write(source, metadata() + R"({"type":"compacted","payload":{"message":"old context"}})" "\n");
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    write(source, metadata() + R"({"type":"event_msg","payload":{"type":"user_message","message":"pending canonical write"}})" "\n");
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    write(source, metadata() + prompt + R"({"type":"event_msg","payload":{"type":"future_replay"}})" "\n");
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    write(source, metadata() + R"({"type":"response_item","payload":{"type":"reasoning","encrypted_content":"opaque"}})" "\n");
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    auto inherited = metadata();
    inherited.insert(inherited.rfind("}}"), ",\"forked_from_id\":\"parent\"");
    write(source, inherited);
    expect(Availability::AVAILABLE, Content::UNKNOWN);
    write(source, metadata());
    fs::resize_file(source, 64 * 1024 * 1024 + 1);
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    write(source, R"({"type":"session_meta","payload":{"id":"wrong"}})" "\n");
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    write(source, metadata(true));
    select_rollout(store, source, "paginated");
    expect(Availability::AVAILABLE, Content::EMPTY);
    write(source, metadata(true) +
        R"({"ordinal":2,"type":"response_item","payload":{"type":"message","role":"user","content":"missing ordinal"}})" "\n");
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    // Native revert selects a differently named rollout. Neither old contents
    // nor filename suffixes determine the retained conversation's current data.
    write(source, metadata(true));
    const auto replacement = fixture / "profile/archived_sessions/replacement.jsonl";
    write(replacement, metadata() + prompt);
    select_rollout(store, replacement);
    expect(Availability::AVAILABLE, Content::NONEMPTY);
    const auto before = fs::last_write_time(replacement);
    const auto cancelled = codex::inspect_resume(request, [] { return true; });
    check(cancelled.availability == Availability::UNKNOWN && cancelled.content == Content::UNKNOWN &&
        fs::last_write_time(replacement) == before, "cancelled inspection is read-only");
    auto saved = request.resume_record;
    request.resume_record.erase("native_store");
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    request.resume_record = saved;
    request.context.account_context->profile_path = addon::path_to_utf8(fixture / "another-profile");
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    request = inspection_request(fixture);
    request.context.account_context->profile_path = addon::path_to_utf8(fixture / "PROFILE");
    if (same_profile_spelling(fixture / "profile", fixture / "PROFILE")) {
        expect(Availability::AVAILABLE, Content::NONEMPTY);
    }
    else {
        expect(Availability::UNKNOWN, Content::UNKNOWN);
    }
    request = inspection_request(fixture);
#ifndef _WIN32
    const auto inaccessible = fixture / "inaccessible-store";
    fs::create_symlink(inaccessible, inaccessible);
    request.resume_record.find("native_store")->set("sqlite_home", Json(addon::path_to_utf8(inaccessible)));
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    request = inspection_request(fixture);
#endif
    write(store / "state_6.sqlite", "unknown future state");
    expect(Availability::UNKNOWN, Content::UNKNOWN);
    fs::remove(store / "state_6.sqlite");
}

void packaged_inspection(const fs::path& package, const fs::path& fixture)
{
    const auto request = inspection_request(fixture);
    for (const std::uint16_t version : {2, 3, 4}) {
        addon::Simulator_options options;
        options.package_directory = package;
        options.adopted_capabilities = std::vector<addon::Capability_pair>{
            {"interactive-resume", version}, {"accounts", 2}, {"profile-storage", 1}};
        addon::Host_simulator host(options, nullptr);
        addon::Contract_failure failure;
        check(host.start(&failure), "packaged adapter starts under old and current host adoption");
        const auto live = host.live_capabilities();
        check(std::find(live.begin(), live.end(), addon::Capability_pair{"interactive-resume", version}) != live.end(),
            "requested resume contract negotiated");
        if (version >= 3) {
            const auto id = host.invoke({"interactive-resume", version}, addon::k_inspect_resume_operation,
                addon::encode_resume_inspect_request(request), 5000);
            const auto outcome = host.wait_outcome(id, 6000);
            check(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged read-only inspection succeeds");
            const auto result = addon::decode_resume_inspect_result(outcome->payload, &failure, version);
            check(result && addon::validate_resume_inspection(request, *result, &failure) &&
                result->availability == addon::Resume_availability::AVAILABLE,
                "packaged exact request is acknowledged");
            check((outcome->payload.find("content") != nullptr) == (version == 4), "version three never emits content");
            if (version == 4) {
                check(result->content == addon::Resume_content::NONEMPTY, "version four reports native content");
            }
        }
        check(host.shutdown(1000, nullptr), "packaged adapter exits without native/model side effects");
    }
}

class Preparation_context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    Json output;
    bool succeeded = false;
    bool done = false;

    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override
    {
        done = true;
        output = Json(std::move(reason));
    }
    bool finished() const override { return done; }
};

void bound_preparation(const fs::path& fixture, const fs::path& native_fixture, const fs::path& package)
{
    const auto inspection = inspection_request(fixture);
    addon::Resume_prepare_request request;
    request.context = inspection.context;
    request.context.launcher_argv = {addon::path_to_utf8(native_fixture)};
    request.context.launch_environment->effective_environment.set("TEMP",
        Json(addon::path_to_utf8(fs::temp_directory_path())));
    if (const char* system_root = std::getenv("SystemRoot")) {
        request.context.launch_environment->effective_environment.set("SystemRoot", Json(system_root));
    }
    request.context.launch_environment->effective_environment.set("CODEX_FIXTURE_SQLITE_HOME",
        Json(addon::path_to_utf8(fixture / "changed-default-store")));
    request.resume_record = inspection.resume_record;
    auto original_environment = request.context.launch_environment->effective_environment;
    original_environment.set("CODEX_HOME", Json(*request.context.account_context->profile_path));
    original_environment.set("CODEX_FIXTURE_SQLITE_HOME", Json(addon::path_to_utf8(fixture / "store")));
    original_environment.set("CODEX_FIXTURE_VERSION", Json("0.167.1"));
    Preparation_context preparation;
    auto launch = codex::verified_launch(request.context.launcher, request.context.launcher_argv,
        original_environment, request.context.working_directory, false, preparation);
    check(launch.has_value(), "verify native preparation fixture");
    const auto native = codex::prepare_native_launch(*launch, addon::environment_pairs(original_environment),
        request.context.working_directory, preparation, true);
    check(native.terminal_identity.has_value(), "production preparation captures native scope");
    request.resume_record.set("native_store", *native.terminal_identity->context.find("native_store"));
    check(codex::inspect_resume({request.context, request.resume_record}, {}).availability ==
        addon::Resume_availability::AVAILABLE,
        "production account spelling inspects the actual prepared native-store record");
    request.startup_observer = addon::Startup_observer_offer{1, "http-json-post", "http://127.0.0.1:32871/events",
        "Bearer " + std::string(64, 'd'), "prepared-identity"};
    Preparation_context context;
    context.input = {"bound-resume", {"interactive-resume", 2}, "prepare_resume",
        addon::encode_resume_prepare_request(request, 2), 15000};
    codex::handle_preparation(context);
    addon::Contract_failure failure;
    const auto result = addon::decode_resume_prepare_result(context.output, &failure, 2);
    check(context.succeeded && result,
        "saved native store supports explicit preparation: " + addon::serialize_json(context.output));
    const auto store_path = (fixture / "store").generic_u8string();
    const std::string store(store_path.begin(), store_path.end());
    check(result->process.argv.back() == k_session_a &&
        result->process.argv[result->process.argv.size() - 2] == request.context.working_directory &&
        result->process.argv[result->process.argv.size() - 3] == "-C" &&
        result->acknowledgment.record_digest_hex == addon::resume_record_digest_hex(request.resume_record),
        "host working directory and original identity are prepared independently");
    check(std::find(result->patch.set.begin(), result->patch.set.end(),
        std::pair<std::string, std::string>{"VNM_CODEX_EXPORT_SQLITE_HOME", store}) != result->patch.set.end(),
        "preparation pins original store instead of current launcher defaults");

    addon::Simulator_options options;
    options.package_directory = package;
    addon::Host_simulator host(options, nullptr);
    check(host.start(&failure), "packaged resume adapter starts: " + failure.detail);
    std::string storage_error;
    const auto storage = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.codex", &storage_error);
    check(storage.has_value(), "allocate resume-owned storage: " + storage_error);
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = addon::Profile_storage_context{1, *storage};
    // Native SessionStart now binds the store in remembered records. Exercise
    // that record with the same preparation offers the desktop sends on resume.
    for (const int offers : {1, 2, 3}) {
        auto offered = request;
        // The host sends its allocated profile storage inside the request.
        offered.context.profile_storage = invocation.profile_storage;
        const auto alternate = fixture / "non-git directory" / "nested";
        fs::create_directories(alternate);
        offered.context.working_directory = addon::path_to_utf8(alternate);
        offered.context.launcher_argv.insert(offered.context.launcher_argv.end(),
            {"-c", "model=\"retained-model\"", "--sandbox", "read-only", "--cd", "/old-saved-location"});
        if (offers & 1) {
            offered.context.session_wake = addon::Session_wake_offer{};
        }
        if (offers & 2) {
            offered.context.codex_invocation = addon::Codex_invocation_request{};
        }
        const auto id = host.invoke({"interactive-resume", 4}, addon::k_prepare_resume_operation,
            addon::encode_resume_prepare_request(offered, 2), 15000, invocation);
        const auto outcome = host.wait_outcome(id, 16000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            "packaged bound resume with preparation offers: " +
                (outcome ? outcome->reason_code + ": " + outcome->message : "no result"));
        const auto prepared = addon::decode_resume_prepare_result(outcome->payload, &failure, 2);
        check(prepared && prepared->process.argv.back() == k_session_a &&
            prepared->acknowledgment.record_digest_hex == addon::resume_record_digest_hex(offered.resume_record) &&
            prepared->patch.profile_storage && prepared->patch.profile_storage->root == *storage &&
            prepared->patch.account_context &&
            prepared->patch.account_context->profile_path == offered.context.account_context->profile_path,
            "bound resume preserves exact conversation, account and owned storage");
        check(prepared && prepared->process.argv[prepared->process.argv.size() - 2] == offered.context.working_directory &&
            std::find(prepared->process.argv.begin(), prepared->process.argv.end(), "--cd") == prepared->process.argv.end() &&
            std::find(prepared->process.argv.begin(), prepared->process.argv.end(), "/old-saved-location") == prepared->process.argv.end(),
            "a nested non-Git working directory replaces the saved Codex directory option before preparation");
        check(std::find(prepared->process.argv.begin(), prepared->process.argv.end(),
                "model=\"retained-model\"") != prepared->process.argv.end() &&
            std::find(prepared->process.argv.begin(), prepared->process.argv.end(),
                "read-only") != prepared->process.argv.end(),
            "bound resume preserves supplied model and permissions");
        check(prepared->patch.session_wake.has_value() == ((offers & 1) != 0) &&
            prepared->patch.codex_invocation.has_value() == ((offers & 2) != 0),
            "read-only inspection preserves preparation acknowledgments for the resumed process");
        if (prepared->patch.session_wake) {
            const auto* bound_store = prepared->patch.session_wake->binding.find("sqlite_home");
            check(prepared->patch.session_wake->available && bound_store && *bound_store == Json(store),
                "resumed wake preparation retains the remembered native store");
        }
        if (prepared->patch.codex_invocation) {
            check(prepared->patch.codex_invocation->argument_prefix_size == 2,
                "resumed invocation retains the bridge and verified Codex prefix");
        }
    }
    check(host.shutdown(1000, nullptr), "packaged resume adapter exits");
    fs::remove(fixture / "profile/archived_sessions/replacement.jsonl");
    Preparation_context missing;
    missing.input = context.input;
    codex::handle_preparation(missing);
    const auto uncertain = addon::decode_resume_prepare_result(missing.output, &failure, 2);
    check(missing.succeeded && uncertain && uncertain->process.argv.back() == k_session_a &&
        !fs::exists(fixture / "profile/archived_sessions/replacement.jsonl") &&
        codex::inspect_resume(inspection, {}).availability == addon::Resume_availability::UNKNOWN,
        "unknown inspection remains recoverable only by the exact native entry point and startup observer");
    for (const auto& profile : {fixture / "other-profile", fixture / "PROFILE"}) {
        request.resume_record.find("native_store")->set("codex_home", Json(addon::path_to_utf8(profile)));
        Preparation_context mismatch;
        mismatch.input = {"wrong-scope", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(request, 2), 15000};
        codex::handle_preparation(mismatch);
        if (same_profile_spelling(fixture / "profile", profile)) {
            check(mismatch.succeeded, "an equivalent native spelling retains the selected resume scope");
        }
        else {
            check(!mismatch.succeeded && mismatch.output == Json("resume_scope_unavailable"),
                "unknown content does not relax a different profile scope");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: codex_resume_tests <package directory> <native fixture>\n";
        return 2;
    }
    const auto fixture = fs::canonical(fs::temp_directory_path()) / ("codex-resume-" + addon::random_nonce_hex());
    try {
        fs::create_directories(fixture);
        std::vector<fs::path> scopes{fixture / "drive"};
#ifdef _WIN32
        const auto spelling = fixture.native();
        if (fixture.root_name().native().size() == 2 && spelling[1] == L':') {
            const auto share = L"\\\\localhost\\" + spelling.substr(0, 1) + L"$" + spelling.substr(2);
            std::error_code error;
            if (fs::equivalent(fixture, fs::path(share), error) && !error) {
                scopes.push_back(fs::path(share) / "unc");
            }
            else {
                std::cout << "SKIP UNC integration: local administrative share is unavailable\n";
            }
        }
#endif
        for (const auto& scope : scopes) {
            fs::create_directories(scope);
            std::cout << "Profile scope: " << addon::path_to_utf8(scope) << std::endl;
            native_activation_facts(scope);
            native_history(scope);
            packaged_inspection(addon::path_from_utf8(argv[1]), scope);
            bound_preparation(scope, addon::path_from_utf8(argv[2]), addon::path_from_utf8(argv[1]));
        }
        fs::remove_all(fixture);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(fixture, ignored);
        return 1;
    }
}
