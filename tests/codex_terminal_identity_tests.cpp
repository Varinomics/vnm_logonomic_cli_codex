#include "codex_lifecycle.h"
#include "codex_terminal_identity.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::string_view k_a = "019933a1-6e43-7102-a520-b3c8294cc763";
constexpr std::string_view k_b = "019944b2-7f54-7203-b631-c4d93a5dd874";

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

class Identity_context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool succeeded = false;
    bool done = false;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override { done = true; output = addon::Json_value(std::move(reason)); }
    bool finished() const override { return done; }
};

bool immediate_idle_identity()
{
    // Native 0.167 renders the appended session-id/thread-name/thread selection
    // as a truncated ID followed by the full UUID while a thread is unnamed.
    // Fresh idle /clear has neither a native hook nor a persisted thread yet.
    constexpr std::string_view previous = "019933a1-6e43-7102-a520-b3c8294cc763";
    constexpr std::string_view selected = "019944b2-7f54-7203-b631-c4d93a5dd874";
    const auto root = std::filesystem::temp_directory_path() / ("codex-idle-title-" + addon::random_nonce_hex());
    std::filesystem::create_directories(root);
    auto payload = *addon::parse_json(R"({
        "event":{"name":"NativeSignal","body":{"origin":"host_terminal"}},
        "ledger":{"current_epoch":1,"current_transition":{"kind":"resume",
            "provider_session_id":"019933a1-6e43-7102-a520-b3c8294cc763",
            "nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}},
        "transition_nonce":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "terminal_identity":{"version":1,"context":{"version":1,"native_version":"0.167.0",
            "native_store":{"version":1,"codex_home":"/unused","sqlite_home":"/unused"}},
            "terminal_event":{"terminal_id":"17","generation":"2","source":"logonomic_terminal_worker",
                "stream":"terminal","stream_index":0,"timestamp":1234.5,
                "payload":{"kind":"terminal_title_changed","title_sequence":"8","title":""}}}
    })", {}, nullptr);
    auto* evidence = payload.find("terminal_identity");
    auto* store = evidence->find("context")->find("native_store");
    store->set("codex_home", addon::Json_value(addon::path_to_utf8(root / "profile")));
    store->set("sqlite_home", addon::Json_value(addon::path_to_utf8(root / "state")));
    evidence->find("terminal_event")->find("payload")->set("title",
        addon::Json_value("project | " + std::string(selected.substr(0, 29)) + "... | " + std::string(selected)));
    Identity_context context;
    context.input = {"interactive", {"terminal-identity", 1}, "classify_event", std::move(payload)};
    codex::handle_lifecycle(context);
    const auto* classification = context.output.find("classification");
    const auto* identity = context.output.find("terminal_identity");
    const auto* id = identity ? identity->find("provider_session_id") : nullptr;
    const auto* caption = identity ? identity->find("title") : nullptr;
    const auto* observations = context.output.find("observations");
    bool ok = check(context.succeeded && classification && *classification == addon::Json_value("transition") &&
        id && *id == addon::Json_value(std::string(selected)) &&
        caption && *caption == addon::Json_value("project") &&
        observations && observations->is_array() && observations->as_array().size() == 1,
        "An idle /clear title must select B immediately without a first-turn hook or native history");
    if (ok) {
        const auto& observation = observations->as_array().front();
        ok &= check(observation.find("provider_session_id") &&
            *observation.find("provider_session_id") == addon::Json_value(std::string(selected)) &&
            *observation.find("provider_session_id") != addon::Json_value(std::string(previous)) &&
            observation.find("resume_record"),
            "The immediate title transition must bind B's exact resume identity");
    }
    ok &= check(std::filesystem::is_empty(root), "Title recognition must not create native history or submit a prompt");
    std::filesystem::remove_all(root);
    return ok;
}

struct Fixture
{
    fs::path root = fs::temp_directory_path() / ("codex-title-identity-" + addon::random_nonce_hex());

    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }

    addon::Classify_event_request request(std::string title, std::string_view current = k_a) const
    {
        addon::Classify_event_request result;
        result.event.name = "NativeSignal";
        result.event.body.set("origin", Json("host_terminal"));
        result.ledger.current_epoch = 1;
        result.ledger.current_transition = addon::Ledger_transition{
            addon::Transition_kind::RESUME, std::string(current), std::string(32, 'a')};
        result.transition_nonce = std::string(32, 'b');
        auto context = *addon::parse_json(R"({"version":1,"native_version":"0.167.0",
            "native_store":{"version":1,"codex_home":"/unused","sqlite_home":"/unused"}})", {}, nullptr);
        context.find("native_store")->set("codex_home", Json(addon::path_to_utf8(root / "profile")));
        context.find("native_store")->set("sqlite_home", Json(addon::path_to_utf8(root / "state")));
        auto event = *addon::parse_json(R"({"terminal_id":"17","generation":"2",
            "source":"logonomic_terminal_worker","stream":"terminal","stream_index":0,"timestamp":1234.5,
            "payload":{"kind":"terminal_title_changed","title_sequence":"8","title":""}})", {}, nullptr);
        event.find("payload")->set("title", Json(std::move(title)));
        result.terminal_identity = addon::Terminal_identity_context{{std::move(context)}, std::move(event)};
        return result;
    }

    void persist(std::string_view id, std::string_view directory = "state") const
    {
        fs::create_directories(root / directory);
        sqlite3* database = nullptr;
        const auto path = addon::path_to_utf8(root / directory / "state_5.sqlite");
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
            throw std::runtime_error("Open native identity fixture");
        }
        if (sqlite3_exec(database, "CREATE TABLE IF NOT EXISTS threads (id TEXT PRIMARY KEY)", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Create native identity index");
        }
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "INSERT INTO threads VALUES (?1)", -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_bind_text(statement, 1, id.data(), static_cast<int>(id.size()), SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_DONE)
        {
            throw std::runtime_error("Persist native identity fixture");
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
    }
};

std::string unnamed(std::string_view id, std::string_view caption = "project")
{
    return std::string(caption) + (caption.empty() ? "" : " | ") + std::string(id.substr(0, 29)) + "... | " + std::string(id);
}

std::string named(std::string_view id, std::string_view name, std::string_view caption = "project")
{
    return std::string(caption) + (caption.empty() ? "" : " | ") + std::string(id.substr(0, 29)) + "... | " +
        std::string(name) + " | " + std::string(name);
}

bool unicode_grapheme_names()
{
    // Native 0.167 limits each part to 48 grapheme clusters, then sanitizes
    // the whole title to 240 scalars. Combining acute and regional indicators
    // survive that sanitizer. Each name here has 25 clusters and 50 scalars;
    // each complete default-style title has 201 scalars and remains intact.
    std::string combining_name;
    std::string flag_name;
    for (int index = 0; index < 25; ++index) {
        combining_name += "e\u0301";
        flag_name += "\U0001F1E6\U0001F1F9";
    }
    Fixture fixture;
    fixture.persist(k_b);
    bool ok = check(flag_name.size() == 200, "Regional-indicator fixture must exceed the former 192-byte name bound");
    for (const auto& name : {combining_name, flag_name}) {
        const auto caption = name + " | project";
        const auto result = codex::classify_title_identity(fixture.request(named(k_b, name, caption)));
        ok &= check(result && result->terminal_identity && result->terminal_identity->provider_session_id == k_b &&
            result->terminal_identity->title == caption && result->base.transition == addon::Transition_kind::RESUME &&
            result->base.observations.size() == 1,
            "Native-valid grapheme names must select B and preserve the original Unicode caption");
    }
    return ok;
}

bool suffix_and_store_cases()
{
    Fixture fixture;
    fixture.persist(k_b);
    bool ok = true;
    const auto select = [&](const std::string& title, std::string_view expected_caption) {
        const auto result = codex::classify_title_identity(fixture.request(title));
        return check(result && result->terminal_identity && result->terminal_identity->provider_session_id == k_b &&
            result->terminal_identity->title == expected_caption &&
            result->base.transition == addon::Transition_kind::RESUME && result->base.observations.size() == 1 &&
            result->base.execution_updates.empty(),
            "Native suffix must select the exact thread and retain the original caption without inventing cwd");
    };
    for (const auto caption : {"project", "original | user | fields", "[ ! ] Action Required | project",
             "● [ . ] Action Required | project", ""})
    {
        ok &= select(unnamed(k_b, caption), caption);
    }
    ok &= select("⠋ " + unnamed(k_b, ""), "⠋");
    std::string unicode_name;
    for (int index = 0; index < 48; ++index) {
        unicode_name += "猫";
    }
    const std::vector<std::string> names{
        "Saved task", "contains | delimiters", std::string(k_a), std::string(k_b),
        "019944b2-7f54-7203-b631-c4d93a5dd875", std::string(k_b) + " | codex",
        "... | " + std::string(k_b), "text | " + std::string(k_a), std::string(12, 'x') + std::string(k_b),
        std::string(k_b.substr(0, 29)) + "... | x", unicode_name};
    const auto path = fixture.root / "state/state_5.sqlite";
    std::ifstream before_file(path, std::ios::binary);
    const std::string before{std::istreambuf_iterator<char>(before_file), {}};
    for (const auto& name : names) {
        ok &= select(named(k_b, name), "project");
    }
    std::ifstream after_file(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(after_file), {}};
    ok &= check(before == after, "Native identity lookup must be read-only");

    for (const auto& title : {unnamed(k_b, std::string(166, 'x')), unnamed(k_b, std::string(167, 'x')),
             "project | " + std::string(k_b.substr(0, 29)) + "... | Saved task | Different task",
             "project | " + std::string(k_a.substr(0, 29)) + "... | " + std::string(k_b),
             std::string(k_b), "project | " + std::string(k_b), unnamed(k_b) + " | extra"})
    {
        const auto result = codex::classify_title_identity(fixture.request(title));
        ok &= check(result && !result->terminal_identity && result->base.observations.empty(),
            "Partial, globally truncated or structurally ambiguous title must not claim identity");
    }
    ok &= select(unnamed(k_b, std::string(165, 'x')), std::string(165, 'x'));
    const auto base = fixture.request(unnamed(k_b));
    for (int invalid = 0; invalid < 7; ++invalid) {
        auto request = base;
        auto& evidence = *request.terminal_identity;
        if (invalid == 0) {
            evidence.terminal_event->set("source", Json("unrelated_stdout"));
        }
        if (invalid == 1) {
            evidence.terminal_event->set("generation", Json("0"));
        }
        if (invalid == 2) {
            evidence.terminal_event->find("payload")->set("title_sequence", Json("01"));
        }
        if (invalid == 3) {
            evidence.terminal_event->find("payload")->set("title", Json());
        }
        if (invalid == 4) {
            evidence.preparation.context.set("native_version", Json("0.167.0-alpha.4"));
        }
        if (invalid == 5) {
            evidence.preparation.context.find("native_store")->set("sqlite_home", Json("relative"));
        }
        if (invalid == 6) {
            evidence.terminal_event.reset();
        }
        const auto result = codex::classify_title_identity(request);
        ok &= check(result && !result->terminal_identity, "Unqualified or unscoped title evidence must remain unavailable");
    }
    ok &= check(!codex::observed_terminal_identity(base, [] { return true; }), "Canceled title lookup must not publish identity");

    constexpr std::string_view collision = "019944b2-7f54-7203-b631-c4d93a5dd875";
    fixture.persist(collision);
    auto request = fixture.request(named(k_b, "A name cannot disambiguate IDs"));
    auto result = codex::classify_title_identity(request);
    ok &= check(result && !result->terminal_identity, "A colliding UUID prefix must not select either native row");
    fixture.persist(collision, "other-state");
    request.terminal_identity->preparation.context.find("native_store")->set("sqlite_home",
        Json(addon::path_to_utf8(fixture.root / "other-state")));
    result = codex::classify_title_identity(request);
    ok &= check(result && result->terminal_identity && result->terminal_identity->provider_session_id == collision,
        "Lookup must use the prepared native store rather than an unrelated profile or current default");
    std::ofstream(fixture.root / "other-state/state_6.sqlite") << "future generation";
    result = codex::classify_title_identity(request);
    ok &= check(result && !result->terminal_identity, "An obsolete state_5 must not stand in for another native generation");
    request.terminal_identity->preparation.context.find("native_store")->set("sqlite_home",
        Json(addon::path_to_utf8(fixture.root / "missing-state")));
    result = codex::classify_title_identity(request);
    ok &= check(result && !result->terminal_identity && !fs::exists(fixture.root / "missing-state"),
        "An unavailable named-thread index must not be guessed or created");
    return ok;
}

bool hook_ordering()
{
    Fixture fixture;
    auto request = fixture.request(unnamed(k_b));
    const auto selected = codex::classify_title_identity(request);
    bool ok = check(selected && selected->terminal_identity && selected->base.observations.size() == 1,
        "A to idle B must be an activation before native turn hooks arrive");
    request.event.name = "SessionStart";
    request.event.body = Json::object();
    request.event.body.set("hook_event_name", Json("SessionStart"));
    request.event.body.set("session_id", Json(std::string(k_a)));
    request.event.body.set("source", Json("resume"));
    request.event.body.set("cwd", Json(addon::path_to_utf8(fixture.root / "nested")));
    request.execution_context_version = 1;
    request.context_state_version = 2;
    auto result = codex::classify_hook(request);
    ok &= check(result && result->classification == addon::Classification::IGNORED && result->observations.empty() &&
        result->context_updates.empty() && result->execution_updates.empty(),
        "A delayed SessionStart A must not roll a newer displayed B back or attach A's context");
    request.event.body.set("session_id", Json(std::string(k_b)));
    request.event.body.set("source", Json("clear"));
    request.ledger.current_transition->provider_session_id = std::string(k_b);
    request.ledger.current_transition->nonce = std::string(32, 'c');
    result = codex::classify_hook(request);
    ok &= check(result && result->classification == addon::Classification::IGNORED && result->observations.empty() &&
        !result->transition && result->execution_updates.size() == 1 &&
        result->execution_updates.front().transition_nonce == std::string(32, 'c') &&
        result->execution_updates.front().cwd == addon::path_to_utf8(fixture.root / "nested"),
        "The first matching native hook must confirm cwd under B's accepted title nonce without a duplicate activation");
    if (result) {
        addon::Contract_failure failure;
        ok &= check(addon::decode_classify_event_result(addon::encode_classify_event_result(*result), &failure).has_value(),
            "Ignored duplicate start must retain a valid context/execution result envelope");
    }
    addon::Attention_classification_request attention;
    attention.base = request;
    const auto normal = codex::classify_attention(attention);
    const auto asynchronous = codex::classify_async_attention(attention);
    ok &= check(normal && asynchronous && normal->base.execution_updates.size() == 1 &&
        asynchronous->base.base.execution_updates.size() == 1 && !normal->attention_kind && !asynchronous->async_questions,
        "Title guarding must preserve the existing attention and asynchronous-question envelopes");

    auto returned = fixture.request(unnamed(k_a), k_b);
    const auto back = codex::classify_title_identity(returned);
    ok &= check(back && back->terminal_identity && back->terminal_identity->provider_session_id == k_a &&
        back->base.transition == addon::Transition_kind::RESUME,
        "A loaded unnamed A must be selectable again without persistence or a new native hook");
    returned.ledger.current_transition->provider_session_id = std::string(k_a);
    const auto same = codex::classify_title_identity(returned);
    ok &= check(same && same->terminal_identity && same->base.classification == addon::Classification::IGNORED &&
        same->base.observations.empty(), "Repeated same-ID title must update presentation without a new epoch");
    request.terminal_identity.reset();
    result = codex::classify_hook(request);
    ok &= check(result && result->classification == addon::Classification::TRANSITION,
        "A launch without title opt-in must retain ordinary native-hook activation");
    return ok;
}

} // namespace

int main()
{
    try {
        return immediate_idle_identity() && suffix_and_store_cases() && unicode_grapheme_names() && hook_ordering() ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
