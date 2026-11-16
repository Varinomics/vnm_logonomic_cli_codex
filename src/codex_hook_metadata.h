#pragma once

#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace codex {

inline std::optional<logonomic::cli_addon::Json_value> async_question_ids(
    const logonomic::cli_addon::Json_value& event, bool accepted)
{
    namespace addon = logonomic::cli_addon;
    const auto* call = event.find("tool_use_id");
    const auto* input = event.find("tool_input");
    const auto* questions = input ? input->find("questions") : nullptr;
    const auto* response = event.find("tool_response");
    if (response && response->is_array() && response->as_array().size() == 1) {
        const auto& content = response->as_array().front();
        response = addon::json::text(content, "type") == "input_text" ? content.find("text") : nullptr;
    }
    if (!call || !addon::json::bounded_string(*call, addon::limits::k_max_lifecycle_identity_bytes) ||
        !questions || !questions->is_array() || questions->as_array().empty() ||
        questions->as_array().size() > addon::k_max_async_question_ids ||
        (accepted && (!response || !response->is_string())))
    {
        return std::nullopt;
    }
    if (accepted) {
        const auto decoded = addon::json::parse(response->as_string());
        const auto* acceptance = decoded ? decoded->find("accepted") : nullptr;
        if (!acceptance || *acceptance != addon::Json_value(true)) {
            return std::nullopt;
        }
    }
    auto ids = addon::Json_value::array();
    for (std::size_t index = 0; index < questions->as_array().size(); ++index) {
        const auto* title = questions->as_array()[index].find("title");
        if (!title || !title->is_string() || title->as_string().find_first_not_of(" \t\r\n") == std::string::npos) {
            return std::nullopt;
        }
        auto identity = addon::Json_value::array();
        identity.push_back(addon::Json_value("request_user_input_async"));
        identity.push_back(*call);
        identity.push_back(addon::Json_value(static_cast<std::int64_t>(index)));
        auto id = addon::Json_value(addon::serialize_json(identity));
        if (!addon::json::bounded_string(id, addon::k_max_async_question_id_bytes)) {
            return std::nullopt;
        }
        ids.push_back(std::move(id));
    }
    return ids;
}

inline std::optional<logonomic::cli_addon::Json_value> async_answer_ids(
    const logonomic::cli_addon::Json_value& event)
{
    namespace addon = logonomic::cli_addon;
    const auto* prompt = event.find("prompt");
    if (!prompt || !prompt->is_string()) {
        return std::nullopt;
    }
    std::string_view text = prompt->as_string();
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        return first == std::string_view::npos ? std::string_view{}
            : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    text = trim(text);
    // Match the native async_question_reply.rs envelope, including its IDE prefix.
    if (text.starts_with("# Context from my IDE setup:\n")) {
        constexpr std::string_view delimiter = "\n## My request for Codex:\n";
        const auto position = text.rfind(delimiter);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        text = trim(text.substr(position + delimiter.size()));
    }
    constexpr std::string_view prefix = "<send_user_message_question_reply>";
    constexpr std::string_view suffix = "</send_user_message_question_reply>";
    if (!text.starts_with(prefix) || !text.ends_with(suffix)) {
        return std::nullopt;
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = 4 * 1024 * 1024;
    limits.max_string_bytes = limits.max_bytes;
    const auto decoded = addon::parse_json(text.substr(prefix.size(), text.size() - prefix.size() - suffix.size()), limits, nullptr);
    if (!decoded || (!decoded->is_object() && !decoded->is_array())) {
        return std::nullopt;
    }
    const auto replies = decoded->is_array() ? *decoded : addon::Json_value(addon::Json_value::Array{*decoded});
    if (replies.as_array().empty() || replies.as_array().size() > addon::k_max_async_question_ids) {
        return std::nullopt;
    }
    auto ids = addon::Json_value::array();
    for (const auto& reply : replies.as_array()) {
        const auto* id = reply.find("questionItemId");
        const auto* question = reply.find("question");
        const auto* answer = reply.find("answer");
        if (!id || !addon::json::bounded_string(*id, addon::k_max_async_question_id_bytes) ||
            !question || !question->is_string() || !answer || !answer->is_string())
        {
            return std::nullopt;
        }
        if (std::find(ids.as_array().begin(), ids.as_array().end(), *id) == ids.as_array().end()) {
            ids.push_back(*id);
        }
    }
    return ids;
}

inline logonomic::cli_addon::Json_value hook_metadata(const logonomic::cli_addon::Json_value& event)
{
    namespace addon = logonomic::cli_addon;
    auto result = addon::Json_value::object();
    for (const auto field : {"hook_event_name", "session_id", "turn_id", "agent_id", "agent_type", "source", "reason"}) {
        if (const auto* value = event.find(field)) {
            result.set(field, *value);
        }
    }
    const auto* name = event.find("hook_event_name");
    if (name && *name == addon::Json_value("SessionStart") && !event.find("agent_id")) {
        if (const auto* model = event.find("model")) {
            result.set("model", *model);
        }
    }
    if (const auto* cwd = event.find("cwd"); name && *name == addon::Json_value("SessionStart") &&
        !event.find("agent_id") && cwd && cwd->is_string() &&
        cwd->as_string().size() <= addon::limits::k_max_start_absolute_path_bytes)
    {
        // Preserve native bytes. Validation and optional-capability handling
        // belong to classification; neither launch cwd nor transcript path is a substitute.
        result.set("cwd", *cwd);
    }
    if (const auto* transcript = event.find("transcript_path"); transcript &&
        logonomic::cli_addon::json::bounded_string(*transcript, addon::limits::k_max_start_absolute_path_bytes))
    {
        result.set("transcript_path", *transcript);
    }
    if (name && name->is_string() && name->as_string() == "Stop" && !event.find("agent_id")) {
        // Infer from bounded final-paragraph evidence after scanning the complete reply.
        const auto* reply = event.find("last_assistant_message");
        if (reply && reply->is_string()) {
            auto evidence = addon::assistant_reply_evidence(reply->as_string());
            if (!evidence.is_null()) {
                result.set("assistant_reply_evidence", std::move(evidence));
            }
        }
    }
    const auto* tool = event.find("tool_name");
    if (name && *name == addon::Json_value("UserPromptSubmit") && !event.find("agent_id")) {
        if (auto ids = async_answer_ids(event)) {
            result.set("async_answer_ids", std::move(*ids));
        }
    }
    if (tool && *tool == addon::Json_value("request_user_input_async") && name &&
        (*name == addon::Json_value("PreToolUse") || *name == addon::Json_value("PostToolUse")) && !event.find("agent_id"))
    {
        // Successful native acceptance is pending, not an answer. Forward only IDs.
        if (auto ids = async_question_ids(event, *name == addon::Json_value("PostToolUse"))) {
            result.set("tool_name", *tool);
            result.set("tool_use_id", *event.find("tool_use_id"));
            result.set("async_question_ids", std::move(*ids));
        }
    }
    if (tool && tool->is_string() && tool->as_string() == "request_user_input") {
        // Preserve native question evidence verbatim; no host presentation fact is a CLI hook.
        for (const auto field : {"tool_name", "tool_use_id", "tool_input", "tool_response", "permission_mode"}) {
            if (const auto* value = event.find(field)) {
                result.set(field, *value);
            }
        }
    }
    return result;
}

} // namespace codex
