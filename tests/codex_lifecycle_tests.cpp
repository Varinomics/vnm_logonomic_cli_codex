#include "codex_lifecycle.h"
#include "codex_hook_metadata.h"

#include <iostream>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool question_hooks()
{
    // Oracle: Codex native Pre/PostToolUse fields and the adopted inferred-candidate contract.
    // These checks do not establish PTY presentation or real model acceptance.
    addon::Attention_classification_request request;
    request.base.event.name = "PreToolUse";
    auto& body = request.base.event.body;
    body.set("hook_event_name", addon::Json_value("PreToolUse"));
    body.set("session_id", addon::Json_value("session-a"));
    body.set("turn_id", addon::Json_value("turn-a"));
    body.set("tool_use_id", addon::Json_value("call-a"));
    body.set("tool_name", addon::Json_value("request_user_input"));
    body.set("tool_input", *addon::parse_json(
        R"({"questions":[{"id":"scope","header":"Scope","question":"Which directory?","options":[{"label":"src","description":"Source files"},{"label":"tests","description":"Tests"}]}]})",
        {}, nullptr));
    const auto raw = addon::serialize_json(body);
    auto result = codex::classify_attention(request);
    if (!check(result && result->base.observations.size() == 1 && result->attention_kind &&
        result->attention_kind->kind == "interactive_question", "Question candidate missing"))
    {
        return false;
    }
    const auto candidate = result->base.observations.front();
    bool ok = check(candidate.attention_evidence == addon::Evidence::INFERRED &&
        candidate.attention_transition == addon::Attention_transition::REQUESTED,
        "Pre-use must remain inferred") &&
        check(addon::serialize_json(body) == raw, "Classifier changed native evidence");
    const auto forwarded = codex::hook_metadata(body);
    ok &= check(addon::serialize_json_canonical(forwarded) == addon::serialize_json_canonical(body),
        "Helper lost question provenance");
    const auto ordinary = codex::classify_hook(request.base);
    ok &= check(ordinary && ordinary->classification == addon::Classification::IGNORED,
        "Question classification requires the attention companion");
    const auto encoded = addon::encode_attention_classification_result(*result);
    ok &= check(addon::decode_attention_classification_result(encoded, nullptr, 2).has_value(),
        "Companion result violates helper-ingress@2");
    ok &= check(!addon::decode_classify_event_result(encoded, nullptr, 2),
        "Ordinary helper envelope accepted companion metadata");

    for (const auto field : {"session_id", "turn_id", "tool_use_id"}) {
        body.set(field, addon::Json_value("different"));
        result = codex::classify_attention(request);
        ok &= check(result && result->base.observations.size() == 1 &&
            result->base.observations.front().attention_request_id != candidate.attention_request_id,
            "Question identity crossed session, turn or call");
        body = *addon::parse_json(raw, {}, nullptr);
    }
    body.set("agent_id", addon::Json_value("child-a"));
    result = codex::classify_attention(request);
    ok &= check(result && result->base.observations.empty(), "Child question became a root candidate");
    body.erase("agent_id");
    body.set("tool_use_id", addon::Json_value(""));
    ok &= check(!codex::classify_attention(request), "Missing native call identity accepted");
    body = *addon::parse_json(raw, {}, nullptr);

    body.set("hook_event_name", addon::Json_value("terminal_title_changed"));
    ok &= check(!codex::classify_attention(request), "Host observation became a CLI hook");
    body = *addon::parse_json(raw, {}, nullptr);
    body.set("tool_name", addon::Json_value("exec_command"));
    result = codex::classify_attention(request);
    ok &= check(result && result->base.observations.empty(), "Nonquestion action became a question");
    ok &= check(!codex::hook_metadata(body).find("tool_input"), "Unrelated tool payload forwarded");
    body = *addon::parse_json(raw, {}, nullptr);

    request.base.event.name = "PostToolUse";
    body.set("hook_event_name", addon::Json_value("PostToolUse"));
    for (const auto response : {
        R"({"answers":{"scope":{"answers":["src"]}}})",
        R"({"answers":{}})",
        R"({"answers":{"scope":{"answers":[]}}})"})
    {
        body.set("tool_response", addon::Json_value(response));
        result = codex::classify_attention(request);
        ok &= check(result && result->base.observations.size() == 1 &&
            result->base.observations.front().attention_request_id == candidate.attention_request_id &&
            result->base.observations.front().attention_transition == addon::Attention_transition::RESOLVED &&
            result->base.observations.front().attention_evidence == addon::Evidence::INFERRED,
            "Native answer or empty resolution failed to match its candidate");
    }
    body.set("tool_response", *addon::parse_json(
        R"([{"type":"input_text","text":"{\"answers\":{\"scope\":{\"answers\":[\"src\"]}}}"}])", {}, nullptr));
    result = codex::classify_attention(request);
    ok &= check(result && result->base.observations.size() == 1 &&
        result->base.observations.front().attention_transition == addon::Attention_transition::RESOLVED,
        "Native function-output content array was not decoded");
    body.set("tool_response", addon::Json_value(R"({"answers":{"unrelated":{"answers":["src"]}}})"));
    ok &= check(!codex::classify_attention(request), "Unrelated answer ID accepted");
    body.set("tool_response", addon::Json_value("request_user_input was cancelled"));
    ok &= check(!codex::classify_attention(request), "Tool error text became an answer");

    for (const auto name : {"Stop", "Interrupt", "UserPromptSubmit", "SessionEnd", "PermissionRequest"}) {
        request.base.event.name = name;
        body = addon::Json_value::object();
        body.set("hook_event_name", addon::Json_value(name));
        body.set("session_id", addon::Json_value("session-a"));
        body.set("turn_id", addon::Json_value("turn-a"));
        result = codex::classify_attention(request);
        if (!check(result && result->base.observations.size() == 1,
            "Host candidate clearing lost a native lifecycle boundary"))
        {
            return false;
        }
        const auto& boundary = result->base.observations.front();
        ok &= check(boundary.provider_session_id == "session-a", "Boundary lost native session identity");
        if (std::string(name) == "SessionEnd") {
            ok &= check(boundary.session_transition == addon::Session_transition::ENDED,
                "Session termination no longer invalidates pending candidates");
        }
        else {
            ok &= check(boundary.provider_turn_id == "turn-a", "Boundary lost native turn identity");
        }
        if (std::string(name) == "Interrupt") {
            ok &= check(boundary.outcome_transition == addon::Outcome_transition::INTERRUPTED &&
                boundary.outcome_evidence == addon::Evidence::INFERRED,
                "Interruption no longer invalidates pending candidates");
        }
        if (std::string(name) == "Stop" || std::string(name) == "UserPromptSubmit") {
            const auto expected = std::string(name) == "Stop"
                ? addon::Phase_transition::AWAITING_INPUT : addon::Phase_transition::WORKING;
            ok &= check(boundary.phase_transition == expected && boundary.phase_evidence == addon::Evidence::INFERRED,
                "Turn boundary no longer invalidates pending candidates");
        }
        if (std::string(name) == "PermissionRequest") {
            ok &= check(result && result->attention_kind && result->attention_kind->kind == "permission",
                "Permission request became a question");
        }
        else {
            ok &= check(result && !result->attention_kind, "Lifecycle boundary invented a question answer");
        }
    }
    request.base.event.name = "StopFailure";
    body.set("hook_event_name", addon::Json_value("StopFailure"));
    result = codex::classify_attention(request);
    ok &= check(result && result->base.observations.empty(), "Invented unsupported native failure hook");
    return ok;
}

} // namespace

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
    return question_hooks() ? 0 : 1;
}
