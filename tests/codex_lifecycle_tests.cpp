#include "codex_lifecycle.h"
#include "codex_hook_metadata.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sidecar.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool permission_title_corroboration()
{
    // Oracle: the native approval overlay remains Action Required while its
    // approval queue is nonempty, independently of allow, deny or cancellation.
    addon::Attention_classification_request request;
    request.base.event.name = "PermissionRequest";
    request.base.event.body = *addon::parse_json(R"({
        "hook_event_name":"PermissionRequest","session_id":"session-a","turn_id":"turn-a"
    })", {}, nullptr);
    auto result = codex::classify_attention(request);
    if (!check(result && result->attention_kind && result->base.observations.size() == 1 &&
        result->attention_kind->kind == "permission_candidate" &&
        !result->base.observations.front().attention_request_id.empty(),
        "Permission attention has no candidate identity for native-title withdrawal"))
    {
        return false;
    }
    const auto identity = result->base.observations.front().attention_request_id;
    const auto repeated = codex::classify_attention(request);
    bool ok = check(repeated && repeated->base.observations.front().attention_request_id == identity,
        "Concurrent approval requests did not share their native overlay candidate");
    request.base.event.body.set("turn_id", addon::Json_value("turn-b"));
    const auto later = codex::classify_attention(request);
    ok &= check(later && later->base.observations.front().attention_request_id != identity,
        "Permission candidate identity crossed a turn boundary");
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
        "candidate":{"session_id":"session-a","turn_id":"turn-a","attention_kind":"permission"}
    })", {}, nullptr);
    for (const auto title : {"[ ! ] Action Required", "[ . ] Action Required | project",
             "[ ! ] Action Required | another approval"})
    {
        request.base.event.body.find("terminal_event")->find("payload")->set("title", addon::Json_value(title));
        result = codex::classify_attention(request);
        if (!check(result && result->attention_kind && result->base.observations.size() == 1 &&
            result->attention_kind->kind == "permission",
            "Native pending approval queue did not corroborate permission attention"))
        {
            return false;
        }
        const auto& observation = result->base.observations.front();
        ok &= check(observation.attention_request_id == identity &&
            observation.attention_transition == addon::Attention_transition::REQUESTED &&
            observation.attention_evidence == addon::Evidence::INFERRED &&
            observation.phase_transition == addon::Phase_transition::NONE &&
            observation.outcome_transition == addon::Outcome_transition::NONE,
            "Approval presentation changed identity, phase or outcome");
    }
    for (const auto& title : {addon::Json_value(), addon::Json_value(""), addon::Json_value("Working"),
             addon::Json_value("Thinking | project"), addon::Json_value("Ready | project"),
             addon::Json_value("project⠋"), addon::Json_value("project ⠋suffix")})
    {
        request.base.event.body.find("terminal_event")->find("payload")->set("title", title);
        result = codex::classify_attention(request);
        ok &= check(result && result->base.observations.empty() && !result->attention_kind,
            "Unavailable or customized title settled a pending approval");
    }
    // Native title_setup.rs joins activity to either neighbor with a space,
    // including when users place project or thread fields before activity.
    for (const auto title : {"⠋", "⠙ thread | project", "⠹ Working | project", "● ⠸ thread",
             "project ⠋", "project ● ⠙ Working | thread", "project | thread ⠹ Working"})
    {
        request.base.event.body.find("terminal_event")->find("payload")->set("title", addon::Json_value(title));
        result = codex::classify_attention(request);
        if (!check(result && result->attention_kind && result->base.observations.size() == 1,
            "Native progress did not distinguish permission resumption from unavailable status"))
        {
            return false;
        }
        const auto& observation = result->base.observations.front();
        ok &= check(result->attention_kind->kind == "permission" &&
            observation.attention_request_id == identity &&
            observation.attention_transition == addon::Attention_transition::RESOLVED &&
            observation.attention_evidence == addon::Evidence::INFERRED &&
            observation.phase_transition == addon::Phase_transition::NONE &&
            observation.outcome_transition == addon::Outcome_transition::NONE,
            "Native permission resumption invented phase, outcome or answer evidence");
    }
    return ok;
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
    for (const auto& title : {addon::Json_value(), addon::Json_value(""), addon::Json_value("Working"),
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
    // Oracle: native StopCommandInput and the adopted final-prose evidence contract.
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

    std::string long_prose;
    for (int index = 0; index < 120; ++index) {
        long_prose += "The implementation preserves the selected configuration and its launch arguments. ";
    }
    std::string unicode_prose = "Plan ";
    for (int index = 0; index < 500; ++index) {
        unicode_prose += "界🧭";
    }
    for (const auto& reply : {
            long_prose + "PRIVATE_REPLY_MIDDLE_CANARY.\n\nWhich directory?",
            long_prose + "PRIVATE_REPLY_MIDDLE_CANARY. " + long_prose + "Which directory?",
            "Please tell me " + long_prose + "the destination directory.",
            "Please provide " + long_prose + "the destination directory.",
            "Let me know which " + long_prose + "destination you prefer.",
            unicode_prose + "Which directory？",
            "```text\n" + long_prose + "\n```\n\nWhich directory?"})
    {
        native.set("last_assistant_message", addon::Json_value(reply));
        request.base.event.body = codex::hook_metadata(native);
        result = codex::classify_attention(request);
        ok &= check(result && result->attention_kind && result->attention_kind->kind == "conversational_hint" &&
            result->base.observations.size() == 1 &&
            result->base.observations.front().attention_transition == addon::Attention_transition::REQUESTED &&
            result->base.observations.front().attention_evidence == addon::Evidence::INFERRED,
            "Long native prose reply lost its inferred question hint through helper metadata");
        const auto* evidence = request.base.event.body.find("assistant_reply_evidence");
        const auto* prefix   = evidence ? evidence->find("prefix") : nullptr;
        const auto* suffix   = evidence ? evidence->find("suffix") : nullptr;
        ok &= check(evidence && prefix && suffix && prefix->is_string() && suffix->is_string() &&
            prefix->as_string().size() <= addon::k_assistant_reply_edge_bytes &&
            suffix->as_string().size() <= addon::k_assistant_reply_edge_bytes &&
            addon::is_valid_utf8(prefix->as_string()) && addon::is_valid_utf8(suffix->as_string()),
            "Long native reply did not produce bounded UTF-8 paragraph edges");
        const auto metadata = addon::serialize_json(request.base.event.body);
        ok &= check(!request.base.event.body.find("last_assistant_message") &&
            metadata.find("PRIVATE_REPLY_MIDDLE_CANARY") == std::string::npos && metadata.size() < 1024,
            "Helper metadata exposed the full reply or its omitted middle");
        if (result) {
            ok &= check(addon::serialize_json(addon::encode_attention_classification_result(*result)).find(
                "assistant_reply_evidence") == std::string::npos,
                "Paragraph evidence escaped into lifecycle classification output");
        }
    }
    // Codex renders replies as Markdown, so the closing question may be emphasised.
    native.set("last_assistant_message",
        addon::Json_value("The build is configured.\n\n**Which target should I build?**"));
    request.base.event.body = codex::hook_metadata(native);
    result = codex::classify_attention(request);
    ok &= check(result && result->attention_kind && result->attention_kind->kind == "conversational_hint",
        "A reply ending in an emphasised question lost its hint");
    for (const auto& reply : {std::string("The changes are complete."), std::string{}, long_prose,
            "```text\n" + long_prose + "\n\nWhich directory?",
            "> " + long_prose + "\nWhich directory?",
            "    " + long_prose + "\n\n    Which directory?"})
    {
        native.set("last_assistant_message", addon::Json_value(reply));
        request.base.event.body = codex::hook_metadata(native);
        result = codex::classify_attention(request);
        ok &= check(result && !result->attention_kind && result->base.observations.size() == 1,
            "Missing, ordinary, code or quoted reply became a hint");
        ok &= check(!request.base.event.body.find("last_assistant_message"), "Raw native reply was forwarded");
    }
    native.set("last_assistant_message", addon::Json_value());
    request.base.event.body = codex::hook_metadata(native);
    result = codex::classify_attention(request);
    ok &= check(result && !result->attention_kind, "Nullable native Stop reply became a hint");
    native.set("last_assistant_message", addon::Json_value("Which directory?"));
    request.base.event.body = native;
    result = codex::classify_attention(request);
    ok &= check(result && !result->attention_kind, "Raw reply bypassed the bounded evidence contract");
    request.base.event.body.set("assistant_reply_evidence", addon::Json_value("Which directory?"));
    result = codex::classify_attention(request);
    ok &= check(result && !result->attention_kind, "Malformed paragraph evidence became a hint");
    native.set("agent_id", addon::Json_value("child-a"));
    request.base.event.body = codex::hook_metadata(native);
    result = codex::classify_attention(request);
    ok &= check(!request.base.event.body.find("assistant_reply_evidence") && result &&
        result->base.observations.empty(), "Child reply became a root hint");
    native.erase("agent_id");
    native.set("hook_event_name", addon::Json_value("UserPromptSubmit"));
    request.base.event.name = "UserPromptSubmit";
    request.base.event.body = codex::hook_metadata(native);
    request.pending_conversational_request_id = hint.attention_request_id;
    result = codex::classify_attention(request);
    ok &= check(!request.base.event.body.find("assistant_reply_evidence") && result && result->attention_kind &&
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
    ok &= check(result && result->attention_kind && result->attention_kind->kind == "permission_candidate" &&
        result->base.observations.front().attention_transition == addon::Attention_transition::REQUESTED &&
        !result->base.observations.front().attention_request_id.empty() &&
        result->base.observations.front().attention_request_id != hint.attention_request_id,
        "Permission borrowed the pending conversational identity");
    return ok;
}

bool async_question_hooks()
{
    // Captured producer contract: title/options input, text {accepted:true}
    // output, and per-question JSON identities in the native reply envelope.
    auto native = *addon::parse_json(R"({"hook_event_name":"PreToolUse",
        "session_id":"session-a","turn_id":"turn-a","tool_use_id":"call-a",
        "tool_name":"request_user_input_async","tool_input":{"questions":[
            {"title":"First?","options":["Yes","No"]},{"title":"Second?"}]}})", {}, nullptr);
    addon::Attention_classification_request request;
    const auto classify = [&]() {
        request.base.event.name = native.find("hook_event_name")->as_string();
        request.base.event.body = codex::hook_metadata(native);
        return codex::classify_async_attention(request);
    };
    const auto started = classify();
    if (!check(started && started->async_questions && started->async_questions->question_ids.size() == 2 &&
        started->base.base.observations.size() == 1 &&
        started->async_questions->kind == addon::Async_question_update_kind::STARTED,
        "Native async call admission was lost before UI presentation"))
    {
        return false;
    }
    const auto ids = started->async_questions->question_ids;
    const auto identity = started->base.base.observations.front().attention_request_id;
    bool ok = check(ids.front() == R"(["request_user_input_async","call-a",0])" &&
        ids.back() == R"(["request_user_input_async","call-a",1])",
        "Async question identities differ from the native TUI's call/index identities");
    const auto old_peer = codex::classify_attention(request);
    ok &= check(old_peer && old_peer->base.observations.empty(),
        "The existing optional capability adopted unsupported async semantics");
    ok &= check(!request.base.event.body.find("tool_input") && !request.base.event.body.find("tool_response"),
        "Async correlation forwarded raw question or response bodies");
    native.set("hook_event_name", addon::Json_value("PostToolUse"));
    for (const auto& response : {addon::Json_value(R"({"accepted":true})"),
             *addon::parse_json(R"([{"type":"input_text","text":"{\"accepted\":true}"}])", {}, nullptr)})
    {
        native.set("tool_response", response);
        const auto accepted = classify();
        ok &= check(accepted && accepted->async_questions &&
            accepted->base.base.observations.size() == 1 &&
            accepted->async_questions->kind == addon::Async_question_update_kind::ACCEPTED &&
            accepted->async_questions->question_ids == ids &&
            accepted->base.base.observations.front().attention_request_id == identity &&
            accepted->base.base.observations.front().attention_transition == addon::Attention_transition::REQUESTED,
            "Successful async Post resolved the question or changed its admission identity");
    }
    for (const auto& response : {addon::Json_value(R"({"accepted":false})"), addon::Json_value("tool failed"), addon::Json_value()}) {
        native.set("tool_response", response);
        const auto failed = classify();
        ok &= check(failed && !failed->async_questions && failed->base.base.observations.empty(),
            "Failed or absent native acceptance promoted an async question");
    }
    const std::string reply = R"(<send_user_message_question_reply>
        [{"answer":"Yes","question":"First?","questionItemId":"[\"request_user_input_async\",\"call-a\",0]"}]
        </send_user_message_question_reply>)";
    native = *addon::parse_json(R"({"hook_event_name":"UserPromptSubmit","session_id":"session-a","turn_id":"turn-a"})", {}, nullptr);
    for (const auto& text : {reply, "# Context from my IDE setup:\ncontext\n## My request for Codex:\n" + reply}) {
        native.set("prompt", addon::Json_value(text));
        const auto answered = classify();
        ok &= check(answered && answered->async_questions &&
            answered->base.base.observations.size() == 1 &&
            answered->async_questions->kind == addon::Async_question_update_kind::ANSWERED &&
            answered->async_questions->question_ids == std::vector<std::string>{ids.front()} &&
            answered->base.base.observations.front().phase_transition == addon::Phase_transition::WORKING &&
            !request.base.event.body.find("prompt"),
            "A partial native reply lost its exact identity, changed Working, or forwarded answer text");
    }
    native.set("prompt", addon::Json_value("Quoted: " + reply));
    const auto unrelated = classify();
    ok &= check(unrelated && !unrelated->async_questions,
        "Quoted reply text became an async answer");
    for (const std::string event : {"Stop", "Interrupt", "SessionEnd"}) {
        native.set("hook_event_name", addon::Json_value(event));
        const auto boundary = classify();
        ok &= check(boundary && (event == "Stop" ? !boundary->async_questions :
            boundary->async_questions && boundary->async_questions->kind == addon::Async_question_update_kind::RESET),
            "An attempted Stop became a reset or an actual native reset was lost");
    }
    request.base.event.name = "NativeSignal";
    request.base.ledger.current_epoch = 1;
    request.base.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "session-a", std::string(64, 'a')};
    request.base.event.body = *addon::parse_json(R"({"origin":"host_terminal","terminal_event":{
        "terminal_id":"17","generation":"2","source":"logonomic_terminal_worker","stream":"terminal",
        "stream_index":0,"timestamp":1234.5,"payload":{"kind":"terminal_title_changed","title_sequence":"8","title":null}},
        "candidate":{"session_id":"session-a","turn_id":"turn-a","tool_use_id":"call-a","attention_kind":"async_question"}})", {}, nullptr);
    for (const auto title : {"[ ! ] Action Required", "⠋ thread", "project ⠙ Working", "customized title"}) {
        request.base.event.body.find("terminal_event")->find("payload")->set("title", addon::Json_value(title));
        const auto presentation = codex::classify_async_attention(request);
        if (!check(presentation.has_value(), "Valid scoped async native presentation was rejected")) {
            return false;
        }
        const auto& observations = presentation->base.base.observations;
        if (std::string(title) == "customized title") {
            ok &= check(observations.empty(), "An unavailable activity marker settled an async question");
        }
        else {
            ok &= check(observations.size() == 1 && observations.front().attention_request_id == identity &&
                observations.front().attention_transition == (std::string(title).starts_with("[")
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED) &&
                observations.front().phase_transition == addon::Phase_transition::NONE,
                "Native aggregate presentation lost call correlation or invented a blocked phase");
        }
    }
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
                boundary.outcome_evidence == addon::Evidence::CONFIRMED,
                "Interruption no longer invalidates pending candidates");
        }
        if (std::string(name) == "Stop" || std::string(name) == "UserPromptSubmit") {
            const auto expected = std::string(name) == "Stop"
                ? addon::Phase_transition::AWAITING_INPUT : addon::Phase_transition::WORKING;
            ok &= check(boundary.phase_transition == expected && boundary.phase_evidence == addon::Evidence::CONFIRMED,
                "Turn boundary no longer invalidates pending candidates");
        }
        if (std::string(name) == "PermissionRequest") {
            ok &= check(result && result->attention_kind && result->attention_kind->kind == "permission_candidate",
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

std::optional<addon::Lifecycle_declaration> package_lifecycle_declaration(const std::filesystem::path& package)
{
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(
        addon::sidecar_relative_path(addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version),
        addon::limits::k_max_sidecar_bytes,
        nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    return sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
}

bool declared_profile_admits_every_shape(const std::filesystem::path& package)
{
    // Oracle: the host's feature gate. It rejects an observation carrying a
    // fact the package's own declaration leaves unsupported, phase and all.
    const auto declaration = package_lifecycle_declaration(package);
    if (!check(declaration.has_value(), "Package lifecycle declaration is unreadable")) {
        return false;
    }
    addon::Attention_classification_request request;
    request.base.ledger.current_epoch = 1;
    request.base.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "session-a", std::string(64, 'a')};
    bool ok = true;
    const auto classify = [
            &request,
            &declaration,
            &ok
        ](
            const std::string& name,
            const std::string& body,
            const char*        shape)
        {
            request.base.event.name = name;
            request.base.event.body = *addon::parse_json(body, {}, nullptr);
            if (name != "NativeSignal") {
                request.base.event.body = codex::hook_metadata(request.base.event.body);
            }
            const auto result = codex::classify_attention(request);
            ok &= check(result && !result->base.observations.empty(), shape);
            if (result) {
                for (const auto& observation : result->base.observations) {
                    ok &= check(addon::lifecycle_profile_admits(declaration->features, observation), shape);
                }
            }
            return result;
        };
    const auto hook = [](const std::string& name, const std::string& members) {
        return R"({"hook_event_name":")" + name + R"(","session_id":"session-a")" + members + "}";
    };
    classify("SessionStart", hook("SessionStart", R"(,"source":"startup")"),
        "The declaration refuses a session start");
    classify("SessionStart", hook("SessionStart", R"(,"source":"resume")"),
        "The declaration refuses a session resume");
    classify("UserPromptSubmit", hook("UserPromptSubmit", R"(,"turn_id":"turn-a")"),
        "The declaration refuses a turn start");
    classify("PermissionRequest", hook("PermissionRequest", R"(,"turn_id":"turn-a")"),
        "The declaration refuses an approval request");
    classify("Stop", hook("Stop", R"(,"turn_id":"turn-a")"), "The declaration refuses a turn completion");
    const auto hint = classify(
        "Stop",
        hook("Stop", R"(,"turn_id":"turn-a","last_assistant_message":"Which directory?")"),
        "The declaration refuses a completion that ends in a question");
    ok &= check(hint && hint->attention_kind && hint->attention_kind->kind == "conversational_hint",
        "The declaration test did not exercise a real helper-produced conversational hint");
    if (hint && hint->attention_kind) {
        request.pending_conversational_request_id = hint->base.observations.front().attention_request_id;
    }
    classify("UserPromptSubmit", hook("UserPromptSubmit", R"(,"turn_id":"turn-b")"),
        "The declaration refuses the reply that resolves a conversational hint");
    request.pending_conversational_request_id.reset();
    classify("Interrupt", hook("Interrupt", R"(,"turn_id":"turn-b")"), "The declaration refuses an interruption");
    for (const std::string name : {"SubagentStart", "SubagentStop"}) {
        classify(name, hook(name, R"(,"turn_id":"turn-b","agent_id":"child-a","agent_type":"worker")"),
            "The declaration refuses a subagent transition");
    }
    for (const std::string name : {"PreToolUse", "PostToolUse"}) {
        classify(name, hook(name, R"(,"turn_id":"turn-b","tool_use_id":"call-a","tool_name":"request_user_input",
            "tool_input":{"questions":[{"id":"scope","header":"Scope","question":"Which directory?"}]},
            "tool_response":"{\"answers\":{\"scope\":{\"answers\":[\"src\"]}}}")"),
            "The declaration refuses a structured question or its answer");
    }
    classify("NativeSignal", R"({"origin":"host_terminal",
        "terminal_event":{
            "terminal_id":"17","generation":"2","source":"logonomic_terminal_worker",
            "stream":"terminal","stream_index":0,"timestamp":1234.5,
            "payload":{"kind":"terminal_title_changed","title_sequence":"8","title":"[ ! ] Action Required"}
        },
        "candidate":{"session_id":"session-a","turn_id":"turn-b","tool_use_id":"call-a"}})",
        "The declaration refuses a corroborated structured question");
    classify("SessionEnd", hook("SessionEnd", ""), "The declaration refuses a session end");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    if (argc != 2) {
        std::cerr << "usage: codex_lifecycle_tests <package directory>\n";
        return 2;
    }
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
    if (!result || result->observations.front().phase_evidence != addon::Evidence::CONFIRMED ||
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
    const bool gated = declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
    return permission_title_corroboration() && question_hooks() && async_question_hooks() && conversational_stop() && gated ? 0 : 1;
}
