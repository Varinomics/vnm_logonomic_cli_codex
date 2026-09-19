#include "codex_lifecycle.h"

#include <iostream>

int main()
{
    namespace addon = logonomic::cli_addon;
    addon::Classify_event_request request;
    request.event.name = "SessionStart";
    request.event.body.set("hook_event_name", addon::Json_value("SessionStart"));
    request.event.body.set("session_id", addon::Json_value("session-a"));
    auto result = codex::classify_hook(request);
    if (!result || result->classification != addon::Classification::TRANSITION ||
        result->observations.front().provider_session_id != "session-a")
    {
        return 1;
    }
    request.event.name = "UserPromptSubmit";
    request.event.body.set("hook_event_name", addon::Json_value(request.event.name));
    if (codex::classify_hook(request)) {
        std::cerr << "Missing turn identity accepted\n";
        return 1;
    }
    request.event.body.set("turn_id", addon::Json_value("turn-a"));
    result = codex::classify_hook(request);
    if (!result || result->observations.front().phase_evidence != addon::Evidence::INFERRED ||
        result->observations.front().phase_transition != addon::Phase_transition::WORKING)
    {
        return 1;
    }
    request.event.name = "SubagentStart";
    request.event.body.set("hook_event_name", addon::Json_value(request.event.name));
    request.event.body.set("agent_id", addon::Json_value("child-a"));
    request.event.body.set("agent_type", addon::Json_value("worker"));
    result = codex::classify_hook(request);
    if (!result || result->observations.front().subagent_transition != addon::Subagent_transition::STARTED ||
        result->observations.front().agent_label != "worker")
    {
        return 1;
    }
    return 0;
}
