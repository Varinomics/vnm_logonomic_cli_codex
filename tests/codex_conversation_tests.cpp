#include "codex_conversation.h"
#include "codex_accounts.h"
#include "codex_conversation_source.h"

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
#include <string>
#include <string_view>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

bool check(bool condition, std::string_view detail)
{
    if (!condition) {
        std::cerr << "FAIL: " << detail << '\n';
    }
    return condition;
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t occurrences(std::string_view haystack, std::string_view needle)
{
    std::size_t count = 0;
    for (auto offset = haystack.find(needle); offset != std::string_view::npos;
         offset = haystack.find(needle, offset + needle.size()))
    {
        ++count;
    }
    return count;
}

bool select_rollout(const fs::path& store, const fs::path& rollout)
{
    fs::create_directories(store);
    sqlite3* database = nullptr;
    const auto database_path = addon::path_to_utf8(store / "state_5.sqlite");
    if (sqlite3_open(database_path.c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return check(false, "create the native state-store fixture");
    }
    bool ok = sqlite3_exec(database,
        "CREATE TABLE IF NOT EXISTS threads (id TEXT PRIMARY KEY, rollout_path TEXT, history_mode TEXT)",
        nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    const auto path = addon::path_to_utf8(rollout);
    ok = ok && sqlite3_prepare_v2(database,
        "INSERT OR REPLACE INTO threads VALUES ('session-a', ?1, 'paginated')", -1, &statement, nullptr) == SQLITE_OK;
    ok = ok && sqlite3_bind_text(statement, 1, path.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
    ok = ok && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return check(ok, "select the authoritative native rollout");
}

bool encrypted_projection(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    const std::string metadata = R"({"type":"session_meta","payload":{"id":"session-a"}})" "\n";
    const std::string encrypted =
        R"({"type":"response_item","payload":{"type":"agent_message","content":[{"type":"input_text","text":"Private routing envelope"},{"type":"encrypted_content","encrypted_content":"ciphertext-one"}]}})" "\n";
    const std::string discussion =
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Visible prompt"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","phase":"commentary","content":[{"type":"output_text","text":"Intermediate progress"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"reasoning","summary":[{"text":"Reasoning detail"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"agent_message","content":[{"type":"input_text","text":"Internal agent text"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"Visible final reply"}]}})" "\n";
    bool ok = true;
    for (const bool all_encrypted : {false, true}) {
        write(source, metadata + (all_encrypted ? encrypted + encrypted : discussion + encrypted));
        for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
                 addon::Conversation_export_format::CONVERSATION})
        {
            for (const bool compact : {false, true}) {
                request.format = format;
                request.compact = compact;
                request.output_directory = addon::path_to_utf8(root / (all_encrypted ? "encrypted-only" : "mixed") /
                    (compact ? "compact" : "full") / addon::conversation_export_format_name(format));
                fs::create_directories(addon::path_from_utf8(request.output_directory));
                addon::Contract_failure failure;
                const auto result = codex::export_conversation(request, {}, &failure);
                ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
                if (!result) {
                    continue;
                }
                const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
                ok &= check(result->compact == compact && result->message_count == (all_encrypted ? 2 : compact ? 3 : 6),
                    "encrypted placeholders count as individual exported messages in both projections");
                ok &= check(occurrences(text, "[Encrypted message]") == (all_encrypted ? 2 : 1) &&
                    text.find("Private routing envelope") == std::string::npos &&
                    text.find("ciphertext-one") == std::string::npos,
                    "each encrypted message has a placeholder without revealing routing text or ciphertext");
                if (!all_encrypted) {
                    ok &= check(text.find("Visible prompt") != std::string::npos &&
                        text.find("Visible final reply") != std::string::npos, "discussion survives either projection");
                    for (const auto* hidden : {"Intermediate progress", "Reasoning detail", "Internal agent text"}) {
                        ok &= check((text.find(hidden) == std::string::npos) == compact,
                            "compact excludes known non-discussion content while full export retains it");
                    }
                }
            }
        }
    }
    return ok;
}

bool contextual_projection(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    // Codex ContextualUserFragment recognizes paired markers on input_text;
    // UserInstructions and EnvironmentsState own these two native wrappers.
    write(source,
        R"({"type":"session_meta","payload":{"id":"session-a"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"# AGENTS.md instructions for /workspace\n\n<INSTRUCTIONS>\nInjected project rules\n</INSTRUCTIONS>"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":" \n<ENVIRONMENT_CONTEXT>\n<cwd>/injected/workspace</cwd>\n</ENVIRONMENT_CONTEXT>\t"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Review the workspace diagram"},{"type":"input_text","text":"<environment_context><cwd>/injected/mixed</cwd></environment_context>"},{"type":"input_image","image_url":"https://example.invalid/workspace.png"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Explain # AGENTS.md instructions and <environment_context> tags."}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"<environment_context>Quoted in the answer</environment_context>"}]}})" "\n");
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        for (const bool compact : {false, true}) {
            request.format = format;
            request.compact = compact;
            request.output_directory = addon::path_to_utf8(root / "context" / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            addon::Contract_failure failure;
            const auto result = codex::export_conversation(request, {}, &failure);
            ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
            if (!result) {
                continue;
            }
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->message_count == (compact ? 3 : 5) && result->attachment_count == 1,
                "compact excludes contextual messages while retaining discussion and attachments");
            for (const auto* hidden : {"Injected project rules", "/injected/workspace", "/injected/mixed"}) {
                ok &= check((text.find(hidden) == std::string::npos) == compact,
                    "paired native context wrappers are hidden only in compact output");
            }
            for (const auto* retained : {"Review the workspace diagram", "Explain # AGENTS.md instructions",
                     "Quoted in the answer", "https://example.invalid/workspace.png"})
            {
                ok &= check(occurrences(text, retained) == 1,
                    "ordinary discussion and media survive beside contextual fragments");
            }
            if (!compact && format == addon::Conversation_export_format::CONVERSATION) {
                ok &= check(text.find("::: user\n# AGENTS.md instructions for /workspace") != std::string::npos,
                    "full export preserves canonical context text and its native user role");
            }
        }
    }
    return ok;
}

bool paginated_source(const fs::path& root, const fs::path& profile, const fs::path& stale,
    addon::Conversation_export_request request)
{
    const auto store = profile / "state";
    const auto current = profile / "sessions/current.jsonl";
    const std::string metadata =
        R"({"ordinal":0,"type":"session_meta","payload":{"id":"session-a","history_mode":"paginated"}})" "\n";
    const std::string message =
        R"({"ordinal":1,"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Authoritative history"}]}})" "\n";
    write(current, metadata + message);
    write(stale, R"({"type":"session_meta","payload":{"id":"session-a"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":"Stale duplicate"}})" "\n");
    if (!select_rollout(store, current)) {
        return false;
    }
    request.launch_environment.effective_environment.set(codex::k_export_sqlite_home_environment,
        addon::Json_value(addon::path_to_utf8(store)));
    request.account_context = addon::Account_context{1,
        codex::profile_account_key(addon::path_to_utf8(profile)), addon::path_to_utf8(profile)};
    request.launch_environment.effective_environment.set("CODEX_HOME", addon::Json_value(addon::path_to_utf8(root / "other")));
    request.output_directory = addon::path_to_utf8(root / "paginated");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    addon::Contract_failure failure;
    const auto result = codex::export_conversation(request, {}, &failure);
    bool ok = check(result.has_value(), failure.reason_code + " " + failure.detail);
    if (result) {
        const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
        ok &= check(result->message_count == 1 && text.find("Authoritative history") != std::string::npos &&
            text.find("Stale duplicate") == std::string::npos,
            "SQLite selects the exact account's current rollout instead of a matching stale filename");
    }
    request.output_directory = addon::path_to_utf8(root / "paginated-failures");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    write(current, R"({"ordinal":0,"type":"session_meta","payload":{"id":"another","history_mode":"paginated"}})" "\n" + message);
    ok &= check(!codex::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_identity_mismatch", "state-store identity still requires matching native metadata");
    write(current, metadata +
        R"({"ordinal":2,"type":"response_item","payload":{"type":"message","role":"user","content":"Missing prefix"}})" "\n");
    ok &= check(!codex::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_format_unavailable", "missing paginated ordinals fail instead of truncating history");
    const auto outside = root / "other/sessions/current.jsonl";
    write(outside, metadata + message);
    if (!select_rollout(store, outside)) {
        return false;
    }
    ok &= check(!codex::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_source_unavailable", "a native pointer cannot escape the selected account");
    return ok;
}

bool frozen_home_source(const fs::path& root)
{
    const auto origin = root / "frozen-origin";
    const auto source = origin / ".codex/sessions/rollout-2026-10-25-session-home.jsonl";
    write(source, R"({"type":"session_meta","payload":{"id":"session-home"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":"Frozen home prompt"}})" "\n");
    addon::Conversation_export_request request;
    request.provider_session_id = "session-home";
    request.working_directory = addon::path_to_utf8(origin);
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.effective_environment = addon::Json_value::object();
#if defined(_WIN32)
    const std::string home_name = "USERPROFILE";
#else
    const std::string home_name = "HOME";
#endif
    request.launch_environment.effective_environment.set(home_name, addon::Json_value(""));
    request.output_directory = addon::path_to_utf8(root / "frozen-home");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    addon::Contract_failure failure;
    const auto result = codex::export_conversation(request, {}, &failure);
    bool ok = check(result.has_value(), "present empty home exports from the frozen origin");
    if (result) {
        const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
        ok &= check(result->message_count == 1 && text.find("Frozen home prompt") != std::string::npos,
            "frozen-origin profile retains the exact native conversation");
    }
    request.launch_environment.effective_environment = addon::Json_value::object();
    request.output_directory = addon::path_to_utf8(root / "absent-home");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    ok &= check(!codex::export_conversation(request, {}, &failure) && failure.reason_code == "profile_unavailable",
        "absent home cannot select a profile from the frozen working directory");
    ok &= check(fs::is_empty(addon::path_from_utf8(request.output_directory)),
        "unavailable profile publishes no conversation");
    return ok;
}

} // namespace

int main()
{
    // Oracle: Codex RolloutItemWire, ResponseItem and ThreadRolledBackEvent;
    // source paths and the fork/replay rules are documented in docs/conversation_export.md.
    const auto root = fs::temp_directory_path() / ("codex-conversation-" + addon::random_nonce_hex());
    const auto profile = root / "profile";
    const auto source = profile / "sessions/2026/10/23/rollout-2026-10-23-session-a.jsonl";
    const std::string prefix =
        R"({"type":"session_meta","payload":{"id":"session-a","forked_from_id":"parent"}})" "\n"
        R"({"type":"session_meta","payload":{"id":"parent"}})" "\n"
        R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"first"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Inherited prompt Ω\n```cpp\nint value = 3;\n```"},{"type":"input_image","file_id":"file-image-a"}]}})" "\n"
        R"({"type":"event_msg","payload":{"type":"user_message","message":"Inherited prompt Ω"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Canonical answer"}]}})" "\n"
        R"({"type":"event_msg","payload":{"type":"agent_message","message":"Canonical answer"}})" "\n"
        R"({"type":"response_item","payload":{"type":"function_call","name":"read_file","call_id":"call-a","arguments":"{\"path\":\"note.txt\"}"}})" "\n";
    const std::string large(1024 * 1024 + 17, 'v');
    const std::string body = prefix +
        "{\"type\":\"response_item\",\"payload\":{\"type\":\"function_call_output\","
        "\"call_id\":\"call-a\",\"output\":\"tool-output-start\\n" + large + "\\ntool-output-end\"}}\n" +
        R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"removed"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Removed branch prompt"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Removed branch reply"}]}})" "\n"
        R"({"type":"event_msg","payload":{"type":"thread_rolled_back","num_turns":1}})" "\n"
        R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"current"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Current branch prompt"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Current branch reply"}]}})" "\n";
    write(source, body + "{\"type\":\"response_item\"");
    addon::Conversation_export_request request;
    request.provider_session_id = "session-a";
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.effective_environment = addon::Json_value::object();
    request.launch_environment.effective_environment.set("CODEX_HOME", addon::Json_value(addon::path_to_utf8(profile)));
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        request.format = format;
        request.output_directory = addon::path_to_utf8(root / addon::conversation_export_format_name(format));
        fs::create_directories(addon::path_from_utf8(request.output_directory));
        addon::Contract_failure failure;
        const auto result = codex::export_conversation(request, {}, &failure);
        ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
        if (result) {
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->message_count == 6 && result->attachment_count == 1, "messages and attachments retained");
            ok &= check(result->incomplete_tail_omitted, "active partial tail reported");
            ok &= check(occurrences(text, "Canonical answer") == 1, "event aliases do not duplicate assistant output");
            ok &= check(occurrences(text, "Inherited prompt Ω") == 1, "fork context retained once");
            ok &= check(text.find("Removed branch") == std::string::npos, "rolled back turns excluded");
            ok &= check(text.find(large) != std::string::npos && text.find("tool-output-end") != std::string::npos,
                "tool text beyond IPC string size retained");
            ok &= check(text.find("Current branch reply") > text.find("tool-output-end"), "native order retained");
        }
    }
    request.output_directory = addon::path_to_utf8(root / "failures");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    addon::Contract_failure failure;
    write(source, R"({"type":"session_meta","payload":{"id":"other"}})" "\n");
    ok &= check(!codex::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_identity_mismatch", "filename is not identity proof");
    write(source, R"({"type":"session_meta","payload":{"id":"session-a","history_mode":"paginated"}})" "\n");
    ok &= check(!codex::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_format_unavailable", "paginated history requires its authoritative store binding");
    write(source, body + "broken-complete-record\n");
    ok &= check(!codex::export_conversation(request, {}, &failure), "malformed complete record fails extraction");
    write(source, body);
    ok &= check(!codex::export_conversation(request, [] { return true; }, &failure) &&
        failure.reason_code == "cancelled", "cancelled source scan returns no export");
    ok &= encrypted_projection(root, source, request);
    ok &= contextual_projection(root, source, request);
    ok &= paginated_source(root, profile, source, request);
    ok &= frozen_home_source(root);
    write(fs::path(source.string() + ".zst"), "compressed");
    ok &= check(!codex::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_format_unavailable", "compressed source never selects stale JSONL");
    fs::remove_all(root);
    return ok ? 0 : 1;
}
