#pragma once

#include <logonomic_cli_addon/json.h>

#include <initializer_list>

namespace codex {

inline logonomic::cli_addon::Json_value hook_metadata(const logonomic::cli_addon::Json_value& event)
{
    namespace addon = logonomic::cli_addon;
    auto result = addon::Json_value::object();
    for (const auto field : {"hook_event_name", "session_id", "turn_id", "agent_id", "agent_type", "source"}) {
        if (const auto* value = event.find(field)) {
            result.set(field, *value);
        }
    }
    const auto* name = event.find("hook_event_name");
    if (name && name->is_string() && name->as_string() == "Stop" && !event.find("agent_id")) {
        // The existing local inference feed retains only complete bounded root replies.
        const auto* reply = event.find("last_assistant_message");
        if (reply && reply->is_string() && reply->as_string().size() <= 2048) {
            result.set("last_assistant_message", *reply);
        }
    }
    const auto* tool = event.find("tool_name");
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
