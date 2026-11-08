#include "codex_conversation_source.h"

#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <memory>
#include <system_error>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

std::optional<Conversation_store_source> unavailable(
    addon::Contract_failure* failure, std::string detail)
{
    *failure = {"conversation_source_unavailable", std::move(detail)};
    return {};
}

bool managed_rollout(const fs::path& profile, const fs::path& path)
{
    for (const auto* name : {"sessions", "archived_sessions"}) {
        std::error_code error;
        const auto root = fs::canonical(profile / name, error);
        if (error) {
            continue;
        }
        const auto relative = path.lexically_relative(root);
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<Conversation_store_source> conversation_store_source(
    const fs::path& profile, const fs::path& sqlite_home, std::string_view session_id,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    if (cancelled && cancelled()) {
        *failure = {"cancelled", "Conversation extraction was cancelled."};
        return {};
    }
    if (!sqlite_home.is_absolute()) {
        return unavailable(failure, "The conversation's original Codex state store is unavailable.");
    }
    // A different state-store generation can retain an obsolete state_5 file.
    // Do not guess which generation the prepared native process opened.
    if (!fs::is_directory(sqlite_home)) {
        return unavailable(failure, "The conversation's original Codex state store is unavailable.");
    }
    for (const auto& entry : fs::directory_iterator(sqlite_home)) {
        if (cancelled && cancelled()) {
            *failure = {"cancelled", "Conversation extraction was cancelled."};
            return {};
        }
        const auto name = addon::path_to_utf8(entry.path().filename());
        if (name.starts_with("state_") && name.ends_with(".sqlite") && name != "state_5.sqlite") {
            return unavailable(failure, "The Codex profile contains an unsupported state-store generation.");
        }
    }
    auto stop = cancelled;
    sqlite3* raw_database = nullptr;
    const auto database_path = addon::path_to_utf8(sqlite_home / "state_5.sqlite");
    const int opened = sqlite3_open_v2(database_path.c_str(), &raw_database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw_database, sqlite3_close);
    if (opened != SQLITE_OK || sqlite3_db_readonly(database.get(), "main") != 1) {
        return unavailable(failure, "The conversation's original Codex state store could not be read.");
    }
    sqlite3_busy_timeout(database.get(), 1000);
    sqlite3_progress_handler(database.get(), 1000, [](void* context) {
        const auto& callback = *static_cast<const std::function<bool()>*>(context);
        return callback && callback() ? 1 : 0;
    }, &stop);

    sqlite3_stmt* raw_statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database.get(),
        "SELECT rollout_path, history_mode FROM threads WHERE id = ?1", -1, &raw_statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement, sqlite3_finalize);
    if (prepared != SQLITE_OK || sqlite3_bind_text(statement.get(), 1, session_id.data(),
            static_cast<int>(session_id.size()), SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_ROW ||
        sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT)
    {
        return unavailable(failure, "Codex's current conversation source could not be resolved.");
    }
    const auto column = [&](int index) {
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), index));
        if (!value) {
            return std::string{};
        }
        return std::string(value, static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), index)));
    };
    const auto selected_path = column(0);
    const auto history_mode = column(1);
    if (sqlite3_step(statement.get()) != SQLITE_DONE || selected_path.empty() ||
        selected_path.find('\0') != std::string::npos ||
        (history_mode != "legacy" && history_mode != "paginated"))
    {
        return unavailable(failure, "Codex's current conversation source has an unsupported layout.");
    }
    const auto path = addon::path_from_utf8(selected_path);
    if (!path.is_absolute()) {
        return unavailable(failure, "Codex's current conversation source is not an absolute path.");
    }
    if (path.extension() == ".zst" || fs::exists(addon::path_from_utf8(selected_path + ".zst"))) {
        *failure = {"conversation_format_unavailable", "This Codex conversation uses compressed native history."};
        return {};
    }
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    if (error || canonical.extension() != ".jsonl" || !fs::is_regular_file(canonical) ||
        !managed_rollout(profile, canonical))
    {
        return unavailable(failure, "Codex's current conversation source is outside the selected account's history.");
    }
    return Conversation_store_source{canonical, history_mode};
}

} // namespace codex
