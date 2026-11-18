#include "codex_model_source.h"

#include "codex_resume_record.h"

#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/model_state.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string_view>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace json = addon::json;
using Json = addon::Json_value;
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::string environment_path(const std::string& name)
{
#ifdef _WIN32
    // Both SQLite and the SDK take UTF-8. The narrow CRT environment uses
    // the system code page even when the host supplied a Unicode path.
    const std::wstring wide_name(name.begin(), name.end());
    const auto* path = _wgetenv(wide_name.c_str());
    return path ? addon::path_to_utf8(path) : std::string{};
#else
    const auto* path = std::getenv(name.c_str());
    return path ? path : "";
#endif
}

Database open_source(bool create)
{
    sqlite3* raw = nullptr;
    const auto path = environment_path(k_model_source_environment);
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (addon::is_execution_local_path(path) && sqlite3_open_v2(path.c_str(), &raw, flags, nullptr) == SQLITE_OK)
    {
        sqlite3_busy_timeout(raw, 150);
    }
    else {
        sqlite3_close(raw);
        raw = nullptr;
    }
    return Database(raw, sqlite3_close);
}

Statement prepare(sqlite3* db, const char* sql, std::initializer_list<std::string_view> values)
{
    sqlite3_stmt* raw = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    int parameter = 0;
    for (const auto value : values) {
        if (!statement || sqlite3_bind_text(statement.get(), ++parameter, value.data(),
                static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        {
            return Statement(nullptr, sqlite3_finalize);
        }
    }
    return statement;
}

bool own_rollout(const std::string& transcript, const std::string& actor)
{
    if (!canonical_session_id(actor) || !addon::is_execution_local_path(transcript)) {
        return false;
    }
    const auto name = addon::path_to_utf8(addon::path_from_utf8(transcript).filename());
    return name.starts_with("rollout-") && name.ends_with("-" + actor + ".jsonl");
}

} // namespace

std::optional<std::string> model_source_path()
{
    // The custodian removes this generation's private scratch even after a crash.
    const auto scratch = environment_path("LOGONOMIC_ADDON_SCRATCH_DIR");
    if (!addon::is_execution_local_path(scratch)) {
        return {};
    }
    return addon::path_to_utf8(addon::path_from_utf8(scratch) / ("codex-model-" + addon::random_nonce_hex() + ".sqlite"));
}

void arm_model_source(const Json& start, const std::string& nonce)
{
    const auto root = json::text(start, "session_id");
    const auto transcript = json::text(start, "transcript_path");
    if (json::text(start, "hook_event_name") != "SessionStart" || start.find("agent_id") ||
        json::text(start, "source") != "startup" || !own_rollout(transcript, root) ||
        !addon::is_valid_transition_nonce_hex(nonce))
    {
        return;
    }
    const auto db = open_source(true);
    if (!db || sqlite3_exec(db.get(),
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS activation(slot INTEGER PRIMARY KEY,root TEXT,path TEXT,nonce TEXT);"
            "CREATE TABLE IF NOT EXISTS actor(id TEXT PRIMARY KEY,turn TEXT,sequence INTEGER);",
            nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        return;
    }
    const auto insert = prepare(db.get(), "INSERT OR IGNORE INTO activation VALUES(1,?1,?2,?3)", {root, transcript, nonce});
    if (insert && sqlite3_step(insert.get()) == SQLITE_DONE) {
        sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);
    }
}

void capture_turn_model(const Json& event, Json& metadata)
{
    const auto name = json::text(event, "hook_event_name");
    if (name != "UserPromptSubmit" && name != "SubagentStart") {
        return;
    }
    const auto root = json::text(event, "session_id");
    const auto agent = json::text(event, "agent_id");
    const auto actor = agent.empty() ? root : agent;
    const auto turn = json::text(event, "turn_id");
    const auto transcript = json::text(event, "transcript_path");
    const auto model = json::text(event, "model");
    if (!canonical_session_id(root) || !canonical_session_id(turn) || turn[14] != '7' ||
        (event.find("agent_id") && (agent.empty() || agent == root)) ||
        (name == "SubagentStart" && agent.empty()) || !own_rollout(transcript, actor) ||
        !addon::is_valid_model_label(model))
    {
        return;
    }
    const auto db = open_source(false);
    if (!db || sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return;
    }
    auto activation = prepare(db.get(), "SELECT nonce FROM activation WHERE root=?1 AND (?2<>'' OR path=?3)",
        {root, agent, transcript});
    if (!activation || sqlite3_step(activation.get()) != SQLITE_ROW) {
        return;
    }
    const auto* raw_nonce = sqlite3_column_text(activation.get(), 0);
    const std::string nonce = raw_nonce ? reinterpret_cast<const char*>(raw_nonce) : "";
    activation.reset();
    if (!addon::is_valid_transition_nonce_hex(nonce)) {
        return;
    }
    // Codex creates each turn with its process's monotone UUIDv7 generator.
    // A repeated prompt or a late hook from an older turn cannot advance this
    // actor, even if its helper gets CPU after the newer turn's helper.
    auto insert = prepare(db.get(), "INSERT OR IGNORE INTO actor VALUES(?1,'',0)", {actor});
    auto advance = prepare(db.get(), "UPDATE actor SET turn=?2,sequence=sequence+1 "
        "WHERE id=?1 AND turn<?2 COLLATE BINARY AND sequence<9223372036854775807", {actor, turn});
    if (!insert || sqlite3_step(insert.get()) != SQLITE_DONE || !advance ||
        sqlite3_step(advance.get()) != SQLITE_DONE || sqlite3_changes(db.get()) != 1)
    {
        return;
    }
    auto read = prepare(db.get(), "SELECT sequence FROM actor WHERE id=?1", {actor});
    if (!read || sqlite3_step(read.get()) != SQLITE_ROW) {
        return;
    }
    const auto sequence = sqlite3_column_int64(read.get(), 0);
    read.reset();
    if (sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return;
    }
    addon::Model_update update;
    update.provider_session_id = root;
    update.transition_nonce    = nonce;
    update.sequence            = static_cast<std::uint64_t>(sequence);
    update.agent_id            = agent;
    update.status              = addon::Model_status::CONFIRMED;
    update.model               = model;
    update.source              = "codex." + name;
    addon::encode_model_updates(metadata, {update});
}

} // namespace codex
