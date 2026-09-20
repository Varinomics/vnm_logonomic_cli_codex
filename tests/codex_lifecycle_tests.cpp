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

bool title_corroboration(const std::string& request_id)
{
    // Oracle: root-approved host envelope and Codex TUI action-required title
    // formatting. Real PTY visibility/answer and host freshness need their own gates.
    addon::Attention_classification_request request;
    request.base.event.name = "NativeSignal";
    request.base.ledger.current_epoch = 1;
    request.base.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "session-a", std::string(64, 'a')};
    request.base.event.body = *addon::parse_json(R"({
        "origin":"host_terminal",
        "terminal_event":{
            "terminal_id":"17","generation":"2","source":"logonomic_terminal_worker",
            "stream":"terminal","stream_index":0,"timestamp":1234.5,
            "payload":{"kind":"terminal_title_changed","title_sequence":"8","title":null}
        },
        "candidate":{"session_id":"session-a","turn_id":"turn-a","tool_use_id":"call-a"}
    })", {}, nullptr);
    const auto original = request.base.event.body;
    bool ok = true;
    for (const auto title : {"[ ! ] Action Required", "[ . ] Action Required",
        "[ ! ] Action Required | project", "[ . ] Action Required | project | thread",
        "\xe2\x97\x8f [ ! ] Action Required | project"})
    {
        request.base.event.body.find("terminal_event")->find("payload")->set("title", addon::Json_value(title));
        const auto raw = addon::serialize_json(request.base.event.body);
        const auto result = codex::classify_attention(request);
        if (!check(result && result->base.observations.size() == 1 && result->attention_kind &&
            result->attention_kind->kind == "interactive_question", "Matching title did not corroborate candidate"))
        {
            return false;
        }
        const auto& observation = result->base.observations.front();
        ok &= check(observation.attention_request_id == request_id &&
            observation.attention_transition == addon::Attention_transition::REQUESTED &&
            observation.attention_evidence == addon::Evidence::INFERRED &&
            observation.provider_event_name == "NativeSignal" &&
            observation.provider_session_id == "session-a" && observation.provider_turn_id == "turn-a",
            "Title promotion lost native identity, host origin or inferred evidence");
        ok &= check(addon::serialize_json(request.base.event.body) == raw, "Classifier altered host provenance");
        ok &= check(!codex::classify_hook(request.base), "Host signal accepted as a native hook");
    }
    for (const auto title : {addon::Json_value(), addon::Json_value(""), addon::Json_value("Working"),
        addon::Json_value("Action Required"), addon::Json_value("[ ! ] Action Requiredness"),
        addon::Json_value("project | [ ! ] Action Required")})
    {
        request.base.event.body.find("terminal_event")->find("payload")->set("title", title);
        const auto result = codex::classify_attention(request);
        ok &= check(result && result->base.classification == addon::Classification::IGNORED &&
            result->base.observations.empty() && !result->attention_kind,
            "Unavailable/nonmatching title became a question, resolution or readiness observation");
    }
    // Every envelope field is required, and a hook identity is never a host origin.
    for (const auto section : {"", "terminal_event", "candidate"}) {
        const auto& object = *section ? *original.find(section) : original;
        for (const auto& member : object.as_object()) {
            request.base.event.body = original;
            auto& target = *section ? *request.base.event.body.find(section) : request.base.event.body;
            target.erase(member.name);
            ok &= check(!codex::classify_attention(request), "Incomplete host envelope accepted");
        }
    }
    for (const auto field : {"terminal_id", "generation", "title_sequence"}) {
        for (const auto value : {"0", "01", "-1", "1x", "18446744073709551616"}) {
            request.base.event.body = original;
            auto* target = request.base.event.body.find("terminal_event");
            if (std::string(field) == "title_sequence") target = target->find("payload");
            target->set(field, addon::Json_value(value));
            ok &= check(!codex::classify_attention(request), "Invalid host runtime/sequence identity accepted");
        }
    }
    for (const auto section : {"", "terminal_event", "candidate", "payload"}) {
        request.base.event.body = original;
        auto* target = &request.base.event.body;
        if (std::string(section) == "payload") target = target->find("terminal_event")->find("payload");
        else if (*section) target = target->find(section);
        target->set("hook_event_name", addon::Json_value("PreToolUse"));
        ok &= check(!codex::classify_attention(request), "Hook identity or unknown field accepted in host envelope");
    }
    for (const auto field : {"source", "stream", "stream_index", "timestamp"}) {
        request.base.event.body = original;
        request.base.event.body.find("terminal_event")->set(field, addon::Json_value("wrong"));
        ok &= check(!codex::classify_attention(request), "Invalid host source/stream/timestamp accepted");
    }
    for (const auto field : {"kind", "title_sequence", "title"}) {
        request.base.event.body = original;
        request.base.event.body.find("terminal_event")->find("payload")->erase(field);
        ok &= check(!codex::classify_attention(request), "Incomplete title payload accepted");
        request.base.event.body = original;
        request.base.event.body.find("terminal_event")->find("payload")->set(field, addon::Json_value(1));
        ok &= check(!codex::classify_attention(request), "Mistyped title payload accepted");
    }
    for (const auto field : {"session_id", "turn_id", "tool_use_id"}) {
        request.base.event.body = original;
        request.base.event.body.find("candidate")->set(field, addon::Json_value(""));
        ok &= check(!codex::classify_attention(request), "Incomplete native candidate accepted");
    }
    for (const auto field : {"turn_id", "tool_use_id"}) {
        request.base.event.body = original;
        request.base.event.body.find("candidate")->set(field, addon::Json_value("different"));
        request.base.event.body.find("terminal_event")->find("payload")->set(
            "title", addon::Json_value("[ ! ] Action Required"));
        const auto result = codex::classify_attention(request);
        ok &= check(result && result->base.observations.size() == 1 &&
            result->base.observations.front().attention_request_id != request_id,
            "Different native turn/call reused a title candidate identity");
    }
    request.base.event.body = original;
    request.base.event.body.set("origin", addon::Json_value("cli_hook"));
    ok &= check(!codex::classify_attention(request), "Untrusted origin accepted");
    request.base.event.body = original;
    request.base.ledger.current_transition->provider_session_id = "another-session";
    ok &= check(!codex::classify_attention(request), "Title crossed provider session ledger");
    request.base.ledger.current_transition.reset();
    ok &= check(!codex::classify_attention(request), "Title accepted without a live provider session");
    return ok;
}

bool conversational_stop()
{
    // Oracle: native StopCommandInput and the adopted bounded local hint contract.
    // Exercise the same metadata producer used by codex_helper before classification.
    auto native = *addon::parse_json(R"({
        "session_id":"session-a","turn_id":"turn-a","transcript_path":null,
        "cwd":"C:/project","hook_event_name":"Stop","model":"gpt-5",
        "permission_mode":"default","stop_hook_active":false,
        "last_assistant_message":"Done.\n\nWhich directory?  \n"
    })", {}, nullptr);
    addon::Attention_classification_request request;
    request.base.event.name = "Stop";
    request.base.event.body = codex::hook_metadata(native);
    request.base.ledger.current_epoch = 1;
    request.base.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "session-a", std::string(64, 'a')};
    auto result = codex::classify_attention(request);
    if (!check(result && result->attention_kind && result->attention_kind->kind == "conversational_hint" &&
        result->base.observations.size() == 1, "Native Stop reply lost through helper to classifier"))
    {
        return false;
    }
    const auto hint = result->base.observations.front();
    bool ok = check(hint.attention_transition == addon::Attention_transition::REQUESTED &&
        hint.attention_evidence == addon::Evidence::INFERRED && !hint.attention_request_id.empty() &&
        hint.phase_transition == addon::Phase_transition::AWAITING_INPUT,
        "Conversational hint lost inferred attention or native phase");
    const auto wire = addon::encode_attention_classification_result(*result);
    const auto decoded = addon::decode_attention_classification_result(wire, nullptr, 2);
    ok &= check(decoded && decoded->attention_kind && decoded->attention_kind->kind == "conversational_hint",
        "Conversational hint failed SDK decoding");
    ok &= check(addon::serialize_json(wire).find("Which directory?") == std::string::npos,
        "Classification output exposed assistant text");
    result = codex::classify_attention(request);
    ok &= check(result && result->base.observations.front().attention_request_id == hint.attention_request_id,
        "Replayed Stop changed conversational identity");
    request.base.ledger.current_epoch = 2;
    result = codex::classify_attention(request);
    ok &= check(result && result->base.observations.front().attention_request_id != hint.attention_request_id,
        "Conversational identity crossed epochs");
    const auto base = codex::classify_hook(request.base);
    ok &= check(base && base->observations.front().attention_transition == addon::Attention_transition::NONE,
        "Unnegotiated lifecycle classification emitted a hint");

    native.set("last_assistant_message", addon::Json_value(std::string(2032, ' ') + "Which directory?"));
    request.base.event.body = codex::hook_metadata(native);
    result = codex::classify_attention(request);
    ok &= check(request.base.event.body.find("last_assistant_message") && result && result->attention_kind &&
        result->attention_kind->kind == "conversational_hint", "Complete reply at the byte bound lost its hint");
    for (const auto& reply : {std::string("The changes are complete."), std::string{}, std::string(2049, '?')}) {
        native.set("last_assistant_message", addon::Json_value(reply));
        request.base.event.body = codex::hook_metadata(native);
        result = codex::classify_attention(request);
        ok &= check(result && !result->attention_kind && result->base.observations.size() == 1,
            "Missing, ordinary or oversized reply became a hint");
        if (reply.size() > 2048) {
            ok &= check(!request.base.event.body.find("last_assistant_message"), "Oversized reply was forwarded");
            request.base.event.body = native;
            result = codex::classify_attention(request);
            ok &= check(result && !result->attention_kind, "Direct classifier input bypassed the reply bound");
        }
    }
    native.set("last_assistant_message", addon::Json_value());
    request.base.event.body = codex::hook_metadata(native);
    result = codex::classify_attention(request);
    ok &= check(result && !result->attention_kind, "Nullable native Stop reply became a hint");
    native.set("last_assistant_message", addon::Json_value("Which directory?"));
    native.set("agent_id", addon::Json_value("child-a"));
    request.base.event.body = codex::hook_metadata(native);
    result = codex::classify_attention(request);
    ok &= check(!request.base.event.body.find("last_assistant_message") && result &&
        result->base.observations.empty(), "Child reply became a root hint");
    native.erase("agent_id");
    native.set("hook_event_name", addon::Json_value("UserPromptSubmit"));
    request.base.event.name = "UserPromptSubmit";
    request.base.event.body = codex::hook_metadata(native);
    request.pending_conversational_request_id = hint.attention_request_id;
    result = codex::classify_attention(request);
    ok &= check(!request.base.event.body.find("last_assistant_message") && result && result->attention_kind &&
        result->attention_kind->kind == "conversational_hint" && result->base.observations.size() == 1 &&
        result->base.observations.front().attention_request_id == hint.attention_request_id &&
        result->base.observations.front().attention_transition == addon::Attention_transition::RESOLVED &&
        result->base.observations.front().attention_evidence == addon::Evidence::INFERRED &&
        result->base.observations.front().phase_transition == addon::Phase_transition::WORKING,
        "User submission did not resolve only the supplied conversational hint");
    request.pending_conversational_request_id.reset();
    result = codex::classify_attention(request);
    ok &= check(result && !result->attention_kind &&
        result->base.observations.front().attention_transition == addon::Attention_transition::NONE,
        "User submission invented a conversational resolution");
    request.pending_conversational_request_id = hint.attention_request_id;
    request.base.event.name = "PermissionRequest";
    request.base.event.body.set("hook_event_name", addon::Json_value("PermissionRequest"));
    result = codex::classify_attention(request);
    ok &= check(result && result->attention_kind && result->attention_kind->kind == "permission" &&
        result->base.observations.front().attention_transition == addon::Attention_transition::REQUESTED &&
        result->base.observations.front().attention_request_id.empty(),
        "Permission borrowed the pending conversational identity");
    return ok;
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
        result->attention_kind->kind == "interactive_question_candidate", "Hidden question candidate missing"))
    {
        return false;
    }
    const auto candidate = result->base.observations.front();
    bool ok = title_corroboration(candidate.attention_request_id);
    ok &= check(candidate.attention_evidence == addon::Evidence::INFERRED &&
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
    const auto decoded = addon::decode_attention_classification_result(encoded, nullptr, 2);
    ok &= check(decoded && decoded->attention_kind &&
        decoded->attention_kind->kind == "interactive_question_candidate",
        "SDK attention-kind vocabulary does not retain the hidden candidate");
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
        ok &= check(result && !result->attention_kind && result->base.observations.size() == 1 &&
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
    return question_hooks() && conversational_stop() ? 0 : 1;
}
