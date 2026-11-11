#include "codex_conversation.h"

#include "codex_accounts.h"
#include "codex_conversation_source.h"
#include "codex_resume_record.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;
using Json = addon::Json_value;
using Block = addon::Conversation_block;
using Kind = addon::Conversation_block_kind;
using Message = addon::Conversation_message;
using Phase = addon::Conversation_message_phase;

bool fail(addon::Contract_failure* failure, std::string_view reason, std::string_view detail)
{
    *failure = {std::string(reason), std::string(detail)};
    return false;
}

std::optional<fs::path> source_profile(
    const addon::Conversation_export_request& request, addon::Contract_failure* failure)
{
    if (request.account_context) {
        const auto& account = *request.account_context;
        if (!account.profile_path || account.adapter_account_key != profile_account_key(*account.profile_path)) {
            fail(failure, "conversation_profile_mismatch", "The selected Codex account does not name its profile.");
            return {};
        }
        return addon::path_from_utf8(*account.profile_path);
    }
    const auto profile = profile_from_environment(request.launch_environment, {}, request.working_directory,
        &failure->reason_code);
    if (!profile) {
        failure->detail = "The conversation's original Codex profile is unavailable.";
        return {};
    }
    return addon::path_from_utf8(profile->profile_path);
}

std::optional<fs::path> source_file(const fs::path& profile, std::string_view id,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    std::optional<fs::path> found;
    const std::string suffix = "-" + std::string(id) + ".jsonl";
    const std::string compressed_suffix = suffix + ".zst";
    for (const auto* directory : {"sessions", "archived_sessions"}) {
        const auto root = profile / directory;
        if (!fs::exists(root)) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (cancelled && cancelled()) {
                fail(failure, "cancelled", "Conversation extraction was cancelled.");
                return {};
            }
            const auto name = addon::path_to_utf8(entry.path().filename());
            if (name.ends_with(compressed_suffix)) {
                fail(failure, "conversation_format_unavailable",
                    "This Codex conversation uses compressed native history.");
                return {};
            }
            if (!entry.is_regular_file() || !name.ends_with(suffix)) {
                continue;
            }
            if (found) {
                fail(failure, "conversation_source_ambiguous", "Several native files name this Codex conversation.");
                return {};
            }
            found = entry.path();
        }
    }
    if (!found) {
        fail(failure, "conversation_source_unavailable", "The native Codex conversation file is unavailable.");
    }
    return found;
}

std::string reference(std::string_view value)
{
    if (value.starts_with("data:")) {
        const auto end = value.find_first_of(";,");
        return "Embedded " + std::string(value.substr(5, end == std::string_view::npos ? end : end - 5)) +
            " attachment";
    }
    return std::string(value);
}

bool content_blocks(const Json& content, std::vector<Block>& blocks, Kind text_kind,
    std::string_view label, addon::Contract_failure* failure)
{
    if (content.is_string()) {
        blocks.push_back({text_kind, content.as_string(), std::string(label)});
        return true;
    }
    if (!content.is_array()) {
        return fail(failure, "conversation_content_invalid", "A Codex message has invalid content.");
    }
    for (const auto& part : content.as_array()) {
        const auto type = json::text(part, "type");
        if (type == "input_text" || type == "output_text" || type == "text") {
            const auto& value = json::member(part, "text");
            if (!value.is_string()) {
                return fail(failure, "conversation_content_invalid", "A Codex text block is incomplete.");
            }
            blocks.push_back({text_kind, value.as_string(), std::string(label)});
        }
        else
        if (type == "input_image" || type == "image" || type == "input_audio") {
            const auto& url = json::member(part, type == "input_audio" ? "audio_url" : "image_url");
            std::string_view value = url.is_string() ? url.as_string() : std::string_view{};
            if (value.empty()) {
                const auto& file_id = json::member(part, "file_id");
                if (file_id.is_string()) {
                    value = file_id.as_string();
                }
            }
            if (value.empty() && type == "image") {
                const auto& data = json::member(part, "data");
                if (data.is_string() && !data.as_string().empty()) {
                    value = "Embedded image attachment";
                }
            }
            if (value.empty()) {
                return fail(failure, "conversation_content_invalid", "A Codex attachment reference is missing.");
            }
            blocks.push_back({Kind::ATTACHMENT, reference(value), type});
        }
        else {
            return fail(failure, "conversation_content_unavailable",
                "A Codex content block has an unsupported format.");
        }
    }
    return true;
}

bool contextual_user_fragment(const Json& part)
{
    if (json::text(part, "type") != "input_text") {
        return false;
    }
    // Native context recognition uses paired markers even in histories without
    // per-part attribution. Ordinary mentions of these tags remain discussion.
    const auto text = addon::lower(addon::trim(json::text(part, "text")));
    return
        (text.starts_with("# agents.md instructions") && text.ends_with("</instructions>")) ||
        (text.starts_with("<environment_context>") && text.ends_with("</environment_context>"));
}

bool response_message(const Json& item, Message& message, addon::Contract_failure* failure)
{
    const auto type = json::text(item, "type");
    if (type == "agent_message") {
        message.role = "agent";
        const auto& content = json::member(item, "content");
        if (!content.is_array()) {
            return fail(failure, "conversation_content_invalid", "A Codex message has invalid content.");
        }
        bool encrypted = false;
        for (const auto& part : content.as_array()) {
            const auto part_type = json::text(part, "type");
            if (part_type == "input_text") {
                const auto& text = json::member(part, "text");
                if (!text.is_string()) {
                    return fail(failure, "conversation_content_invalid", "A Codex text block is incomplete.");
                }
                message.content.push_back({Kind::TEXT, text.as_string(), {}});
            }
            else
            if (part_type == "encrypted_content") {
                if (!json::member(part, "encrypted_content").is_string()) {
                    return fail(failure, "conversation_content_invalid", "A Codex encrypted message is incomplete.");
                }
                encrypted = true;
            }
            else {
                return fail(failure, "conversation_content_unavailable",
                    "A Codex content block has an unsupported format.");
            }
        }
        if (encrypted) {
            // Native plaintext beside encrypted agent content is a routing envelope.
            message.content = {{Kind::TEXT, "[Encrypted message]", {}}};
        }
        for (auto& block : message.content) {
            block.compact_visible = encrypted;
        }
        return true;
    }
    if (type == "message") {
        message.role = json::text(item, "role");
        if (message.role.empty()) {
            return fail(failure, "conversation_content_invalid", "A Codex message has no role.");
        }
        const auto& content = json::member(item, "content");
        if (!content_blocks(content, message.content, Kind::TEXT, {}, failure)) {
            return false;
        }
        if (message.role == "assistant") {
            const auto phase = json::text(item, "phase");
            message.phase = phase == "final_answer" ? Phase::FINAL
                : phase == "commentary" ? Phase::COMMENTARY : Phase::UNKNOWN;
        }
        for (std::size_t index = 0; index < message.content.size(); ++index) {
            const bool user_context = message.role == "user" && content.is_array() &&
                contextual_user_fragment(content.as_array()[index]);
            message.content[index].compact_visible = (message.role == "user" && !user_context) ||
                (message.role == "assistant" && message.phase != Phase::COMMENTARY);
        }
        return true;
    }
    message.role = "assistant";
    if (type == "function_call" || type == "custom_tool_call" || type == "tool_search_call") {
        const auto& input = json::member(item, type == "custom_tool_call" ? "input" : "arguments");
        if (input.is_null()) {
            return fail(failure, "conversation_content_invalid", "A Codex tool call has no arguments.");
        }
        auto name = json::text(item, "name");
        if (name.empty()) {
            name = type;
        }
        const auto call = json::text(item, "call_id");
        message.content.push_back({Kind::TOOL_CALL,
            input.is_string() ? input.as_string() : addon::serialize_json(input), name + " " + call});
    }
    else
    if (type == "function_call_output" || type == "custom_tool_call_output") {
        message.role = "tool";
        if (!content_blocks(json::member(item, "output"), message.content, Kind::TOOL_RESULT,
                json::text(item, "call_id"), failure))
        {
            return false;
        }
    }
    else
    if (type == "local_shell_call" || type == "web_search_call") {
        const auto& action = json::member(item, "action");
        message.content.push_back({Kind::TOOL_CALL, addon::serialize_json(action), type});
    }
    else
    if (type == "tool_search_output") {
        message.role = "tool";
        message.content.push_back({Kind::TOOL_RESULT, addon::serialize_json(json::member(item, "tools")), type});
    }
    else
    if (type == "reasoning") {
        for (const auto* field : {"summary", "content"}) {
            const auto& parts = json::member(item, field);
            if (!parts.is_array()) {
                continue;
            }
            for (const auto& part : parts.as_array()) {
                if (const auto& value = json::member(part, "text"); value.is_string()) {
                    message.content.push_back({Kind::TEXT, value.as_string(), "Reasoning"});
                }
            }
        }
    }
    else
    if (type == "image_generation_call") {
        const auto prompt = json::text(item, "revised_prompt");
        if (!prompt.empty()) {
            message.content.push_back({Kind::TEXT, prompt, "Image prompt"});
        }
        message.content.push_back({Kind::ATTACHMENT, "Embedded generated image", json::text(item, "id")});
    }
    else
    if (type != "compaction" && type != "compaction_summary" && type != "context_compaction" &&
        type != "configuration_update" && type != "additional_tools" && type != "ghost_snapshot")
    {
        return fail(failure, "conversation_content_unavailable", "A Codex response item has an unsupported format.");
    }
    for (auto& block : message.content) {
        block.compact_visible = false;
    }
    return true;
}

struct Conversation_history
{
    std::vector<Message> messages;
    bool incomplete_tail = false;
    bool content_understood = true;
    bool events_seen = false;
};

std::optional<Conversation_history> read_history(
    const fs::path& profile, const std::string& sqlite_home, std::string_view session_id,
    const std::function<bool()>& cancelled,
    addon::Contract_failure* failure, bool inspection = false)
{
    std::optional<Conversation_store_source> stored_source;
    std::optional<fs::path> source;
    if (!sqlite_home.empty()) {
        stored_source = conversation_store_source(profile, addon::path_from_utf8(sqlite_home),
            session_id, cancelled, failure);
        if (!stored_source) {
            return {};
        }
        source = stored_source->path;
    }
    else {
        source = source_file(profile, session_id, cancelled, failure);
    }
    if (!source) {
        return {};
    }
    // Inspection is an optional background query, not an unbounded export.
    // Check at each reader checkpoint as a native writer can grow the source.
    constexpr std::uintmax_t inspection_limit = 64 * 1024 * 1024;
    const auto stopped = [&] {
        if (cancelled && cancelled()) {
            return true;
        }
        if (inspection) {
            std::error_code error;
            const auto bytes = fs::file_size(*source, error);
            return error || bytes > inspection_limit;
        }
        return false;
    };
    Conversation_history history;
    auto& messages = history.messages;
    std::vector<std::size_t> turns;
    bool identity_seen = false;
    bool paginated = false;
    std::uint64_t next_ordinal = 0;
    const auto snapshot = addon::read_conversation_jsonl(*source,
        [&](const Json& row, addon::Contract_failure* error) {
            const auto type = json::text(row, "type");
            const auto& payload = json::member(row, "payload");
            if (!identity_seen) {
                if (type != "session_meta") {
                    return fail(error, "conversation_source_invalid", "The Codex file does not begin with session metadata.");
                }
                identity_seen = true;
                if (json::text(payload, "id") != session_id) {
                    return fail(error, "conversation_identity_mismatch", "The Codex file names another conversation.");
                }
                const auto mode = json::text(payload, "history_mode");
                paginated = mode == "paginated";
                if ((!mode.empty() && mode != "legacy" && !paginated) ||
                    !json::member(payload, "history_base").is_null())
                {
                    return fail(error, "conversation_format_unavailable",
                        "This Codex conversation uses unsupported or referenced native history.");
                }
                if (paginated && (!stored_source ||
                        !json::member(payload, "subagent_history_start_ordinal").is_null() ||
                        !json::member(payload, "forked_from_ordinal_exclusive").is_null()))
                {
                    return fail(error, "conversation_format_unavailable",
                        "This Codex history requires its original state-store binding and a self-contained rollout.");
                }
                if (stored_source && stored_source->history_mode != (paginated ? "paginated" : "legacy")) {
                    return fail(error, "conversation_source_changed", "The Codex history mode changed while being read.");
                }
                if (!json::member(payload, "forked_from_id").is_null() ||
                    !json::member(payload, "forked_from_ordinal_exclusive").is_null() ||
                    !json::member(payload, "subagent_history_start_ordinal").is_null())
                {
                    history.content_understood = false;
                }
            }
            else
            if (paginated && type == "session_meta") {
                return fail(error, "conversation_format_unavailable",
                    "This paginated Codex history contains inherited session metadata.");
            }
            else
            if (type == "session_meta") {
                // A legacy inherited metadata block does not prove an empty root.
                history.content_understood = false;
            }
            if (paginated) {
                const auto& ordinal = json::member(row, "ordinal");
                if (!ordinal.is_integer() || ordinal.as_integer() < 0 ||
                    static_cast<std::uint64_t>(ordinal.as_integer()) != next_ordinal)
                {
                    return fail(error, "conversation_format_unavailable",
                        "This paginated Codex history has missing or inconsistent record ordinals.");
                }
                ++next_ordinal;
            }
            if (type == "response_item") {
                Message message;
                if (!response_message(payload, message, error)) {
                    return false;
                }
                const auto item_type = json::text(payload, "type");
                if (item_type == "compaction" || item_type == "compaction_summary" ||
                    item_type == "context_compaction" ||
                    (item_type == "reasoning" && !json::member(payload, "encrypted_content").is_null()) ||
                    (item_type == "message" && message.role != "user" && message.role != "assistant" &&
                        message.role != "system" && message.role != "developer"))
                {
                    history.content_understood = false;
                }
                if (!message.content.empty()) {
                    messages.push_back(std::move(message));
                }
            }
            else
            if (type == "event_msg") {
                history.events_seen = true;
                const auto event = json::text(payload, "type");
                if (event == "task_started" || event == "turn_started") {
                    turns.push_back(messages.size());
                }
                else
                if (event == "thread_rolled_back") {
                    if (paginated) {
                        return fail(error, "conversation_branch_unavailable",
                            "This paginated Codex history contains an unsupported rollback marker.");
                    }
                    const auto& count = json::member(payload, "num_turns");
                    if (!count.is_integer() || count.as_integer() < 0 ||
                        static_cast<std::uint64_t>(count.as_integer()) > turns.size())
                    {
                        return fail(error, "conversation_branch_unavailable",
                            "Codex rollback boundaries are not retained in this conversation.");
                    }
                    if (count.as_integer() > 0) {
                        const auto keep = turns.size() - static_cast<std::size_t>(count.as_integer());
                        messages.resize(turns[keep]);
                        turns.resize(keep);
                    }
                }
                else
                if (event != "user_message" && event != "agent_message" && event != "token_count" &&
                    event != "task_complete" && event != "turn_complete" && event != "agent_reasoning" &&
                    event != "agent_reasoning_raw_content")
                {
                    history.content_understood = false;
                }
                // User and assistant events mirror response_item bodies. Using both
                // would duplicate each displayed message and lose canonical tool data.
            }
            else
            if (paginated && type != "session_meta" && type != "turn_context" &&
                type != "token_usage_record" && type != "world_state" && type != "compacted" &&
                type != "security_risk_score" && type != "inter_agent_communication_metadata")
            {
                return fail(error, "conversation_content_unavailable",
                    "This paginated Codex history contains an unsupported record type.");
            }
            else
            if (type != "session_meta" && type != "turn_context" && type != "token_usage_record") {
                // Export can retain understood messages beside opaque legacy rows.
                // Such rows cannot establish that no meaningful content exists.
                history.content_understood = false;
            }
            return true;
        }, stopped, failure);
    if (!snapshot) {
        return {};
    }
    if (stored_source) {
        const auto current = conversation_store_source(profile, addon::path_from_utf8(sqlite_home),
            session_id, cancelled, failure);
        if (!current) {
            return {};
        }
        if (current->path != stored_source->path || current->history_mode != stored_source->history_mode) {
            fail(failure, "conversation_source_changed", "Codex selected another history while extraction was running.");
            return {};
        }
    }
    if (!identity_seen) {
        fail(failure, "conversation_source_invalid", "The Codex file has no session metadata.");
        return {};
    }
    history.incomplete_tail = snapshot->incomplete_tail_omitted;
    return history;
}

bool meaningful_message(const Message& message)
{
    if (message.role == "system" || message.role == "developer") {
        return false;
    }
    return std::any_of(message.content.begin(), message.content.end(), [&](const auto& block) {
        return (message.role != "user" || block.compact_visible) &&
            (block.kind != Kind::TEXT || block.text.find_first_not_of(" \t\r\n") != std::string::npos);
    });
}

} // namespace

addon::Resume_inspect_result inspect_resume(
    const addon::Resume_inspect_request& request, const std::function<bool()>& cancelled)
{
    addon::Resume_inspect_result result;
    result.request_digest_hex = addon::resume_inspect_request_digest_hex(request);
    const auto record = decode_resume_record(request.resume_record);
    const auto& account = request.context.account_context;
    // An old ID-only record does not identify the original store. Inspection is
    // reconstructed later and cannot recover a preparer's environment patch.
    if (!record || record->sqlite_home.empty() || !account || !account->profile_path ||
        addon::path_from_utf8(*account->profile_path).generic_u8string() !=
            addon::path_from_utf8(record->codex_home).generic_u8string() ||
        account->adapter_account_key != profile_account_key(record->codex_home) ||
        (cancelled && cancelled()))
    {
        return result;
    }
    addon::Contract_failure failure;
    std::optional<Conversation_history> history;
    try {
        history = read_history(addon::path_from_utf8(record->codex_home), record->sqlite_home,
            record->session_id, cancelled, &failure, true);
    }
    catch (const fs::filesystem_error&) {
        return result;
    }
    if (!history) {
        // SQLite absence alone is not native absence: Codex can still resolve
        // legacy files or a live writer. Unsupported and changing stores remain unknown.
        return result;
    }
    result.availability = addon::Resume_availability::AVAILABLE;
    if (history->incomplete_tail || !history->content_understood) {
        return result;
    }
    if (std::any_of(history->messages.begin(), history->messages.end(), meaningful_message)) {
        result.content = addon::Resume_content::NONEMPTY;
    }
    else
    if (!history->events_seen) {
        result.content = addon::Resume_content::EMPTY;
    }
    return result;
}

std::optional<addon::Conversation_export_result> export_conversation(
    const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure)
{
    const auto profile = source_profile(request, failure);
    if (!profile) {
        return {};
    }
    const auto sqlite_home = addon::environment_value(
        addon::environment_pairs(request.launch_environment.effective_environment), k_export_sqlite_home_environment);
    const auto history = read_history(*profile, sqlite_home, request.provider_session_id, cancelled, failure);
    if (!history) {
        return {};
    }
    if (history->messages.empty()) {
        fail(failure, "conversation_content_unavailable", "No retained Codex conversation messages are available.");
        return {};
    }
    addon::Conversation_writer writer(request, cancelled);
    if (!writer.open(failure)) {
        return {};
    }
    for (const auto& message : history->messages) {
        if (!writer.append(message, failure)) {
            return {};
        }
    }
    return writer.finish(history->incomplete_tail, failure);
}

} // namespace codex
