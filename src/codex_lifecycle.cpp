#include "codex_lifecycle.h"

#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>

#include <array>
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

std::optional<std::string> metadata(const addon::Json_value& body, std::string_view key,
    std::size_t bound = addon::limits::k_max_lifecycle_identity_bytes)
{
    const auto* member = body.find(key);
    if (!member || !member->is_string()) {
        return std::nullopt;
    }
    const auto& value = member->as_string();
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const auto result = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (result.size() > bound || result.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::vector<std::string>> question_ids(const addon::Json_value& body)
{
    const auto* input = body.find("tool_input");
    const auto* questions = input ? input->find("questions") : nullptr;
    if (!questions || !questions->is_array() || questions->as_array().empty() ||
        questions->as_array().size() > 3)
    {
        return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& question : questions->as_array()) {
        const auto id = metadata(question, "id");
        const auto* header = question.find("header");
        const auto* text = question.find("question");
        if (!id || std::find(ids.begin(), ids.end(), *id) != ids.end() ||
            !header || !header->is_string() || !text || !text->is_string() ||
            text->as_string().find_first_not_of(" \t\r\n") == std::string::npos)
        {
            return std::nullopt;
        }
        ids.push_back(*id);
    }
    return ids;
}

bool question_response(const addon::Json_value& body, const std::vector<std::string>& ids)
{
    const auto* response = body.find("tool_response");
    if (!response) {
        return false;
    }
    // FunctionToolOutput exposes its serialized model-facing content to PostToolUse.
    if (response->is_array()) {
        if (response->as_array().size() != 1) {
            return false;
        }
        const auto& content = response->as_array().front();
        if (metadata(content, "type") != "input_text") {
            return false;
        }
        response = content.find("text");
    }
    if (!response || !response->is_string()) {
        return false;
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = 4 * 1024 * 1024;
    limits.max_string_bytes = limits.max_bytes;
    const auto decoded = addon::parse_json(response->as_string(), limits, nullptr);
    const auto* answers = decoded ? decoded->find("answers") : nullptr;
    if (!answers || !answers->is_object()) {
        return false;
    }
    for (const auto& [id, answer] : answers->as_object()) {
        const auto* values = answer.find("answers");
        if (std::find(ids.begin(), ids.end(), id) == ids.end() || !values || !values->is_array()) {
            return false;
        }
        for (const auto& value : values->as_array()) {
            if (!value.is_string()) {
                return false;
            }
        }
    }
    // Empty answers also resolve the native request; they do not establish a user answer.
    return true;
}

std::string question_request_id(const std::string& session, const std::string& turn, const std::string& call)
{
    addon::Provider_event identity;
    identity.name = "request_user_input";
    identity.body.set("session_id", addon::Json_value(session));
    identity.body.set("turn_id", addon::Json_value(turn));
    identity.body.set("tool_use_id", addon::Json_value(call));
    return "codex-question-" + addon::provider_event_evidence_digest_hex(identity);
}

bool exact_members(const addon::Json_value& value, std::initializer_list<std::string_view> names)
{
    return value.is_object() && value.as_object().size() == names.size() &&
        std::all_of(names.begin(), names.end(), [&](auto name) { return value.find(name); });
}

bool positive_decimal(const addon::Json_value& value)
{
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.as_string();
    if (text.empty() || text.front() < '1' || text.front() > '9') {
        return false;
    }
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool action_required_title(std::string_view title)
{
    // Codex TUI status_surfaces.rs: both blink phases mean Action Required;
    // realtime listening adds a microphone dot and configured items use " | ".
    constexpr std::string_view microphone = "\xe2\x97\x8f ";
    if (title.starts_with(microphone)) {
        title.remove_prefix(microphone.size());
    }
    for (const std::string_view prefix : {"[ ! ] Action Required", "[ . ] Action Required"}) {
        if (title.starts_with(prefix) &&
            (title.size() == prefix.size() || title.substr(prefix.size()).starts_with(" | ")))
        {
            return true;
        }
    }
    return false;
}

std::optional<addon::Attention_classification_result> classify_native_signal(
    const addon::Attention_classification_request& request)
{
    // This envelope is supplied only by the host terminal lane, never helper
    // ingress. The host owns authentication, generation/epoch/sequence freshness,
    // and matching the returned request ID to its retained root candidate.
    const auto& body = request.base.event.body;
    if (!exact_members(body, {"origin", "terminal_event", "candidate"}) ||
        *body.find("origin") != addon::Json_value("host_terminal"))
    {
        return std::nullopt;
    }
    const auto& terminal = *body.find("terminal_event");
    const auto& candidate = *body.find("candidate");
    if (!exact_members(terminal, {"terminal_id", "generation", "source", "stream", "stream_index", "timestamp", "payload"}) ||
        !exact_members(candidate, {"session_id", "turn_id", "tool_use_id"}) ||
        !positive_decimal(*terminal.find("terminal_id")) || !positive_decimal(*terminal.find("generation")) ||
        *terminal.find("source") != addon::Json_value("logonomic_terminal_worker") ||
        *terminal.find("stream") != addon::Json_value("terminal") ||
        *terminal.find("stream_index") != addon::Json_value(0) || !terminal.find("timestamp")->is_number())
    {
        return std::nullopt;
    }
    const auto& payload = *terminal.find("payload");
    if (!exact_members(payload, {"kind", "title_sequence", "title"}) ||
        *payload.find("kind") != addon::Json_value("terminal_title_changed") ||
        !positive_decimal(*payload.find("title_sequence")))
    {
        return std::nullopt;
    }
    const auto session = metadata(candidate, "session_id");
    const auto turn    = metadata(candidate, "turn_id");
    const auto call    = metadata(candidate, "tool_use_id");
    const auto& ledger = request.base.ledger;
    if (!session || !turn || !call || !ledger.current_epoch || !ledger.current_transition ||
        ledger.current_transition->provider_session_id != *session)
    {
        return std::nullopt;
    }
    const auto& title = *payload.find("title");
    if (!title.is_null() && !title.is_string()) {
        return std::nullopt;
    }
    addon::Attention_classification_result result;
    // Empty, unavailable and nonmatching titles withdraw corroboration only.
    // They are neither an answer nor a model-readiness observation.
    if (title.is_null() || !action_required_title(title.as_string())) {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name   = request.base.event.name;
    observation.provider_session_id   = *session;
    observation.provider_turn_id      = *turn;
    observation.attention_request_id  = question_request_id(*session, *turn, *call);
    observation.attention_transition  = addon::Attention_transition::REQUESTED;
    observation.attention_evidence    = addon::Evidence::INFERRED;
    result.base.classification = addon::Classification::EVENT;
    result.base.observations.push_back(std::move(observation));
    result.attention_kind = addon::Attention_kind{0, "interactive_question"};
    return result;
}

} // namespace

std::optional<addon::Classify_event_result> classify_hook(const addon::Classify_event_request& request)
{
    const auto& body = request.event.body;
    const auto name = metadata(body, "hook_event_name");
    if (!name || *name != request.event.name) {
        return std::nullopt;
    }
    addon::Classify_event_result result;
    constexpr std::array<std::string_view, 8> supported{
        "SessionStart", "SessionEnd", "UserPromptSubmit", "Stop", "Interrupt", "PermissionRequest", "SubagentStart", "SubagentStop"};
    if (std::find(supported.begin(), supported.end(), *name) == supported.end()) {
        return result;
    }
    if (*name != "SubagentStart" && *name != "SubagentStop" && body.find("agent_id")) {
        return result;
    }
    const auto session = metadata(body, "session_id");
    if (!session) {
        return std::nullopt;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = *name;
    observation.provider_session_id = *session;
    result.classification = addon::Classification::EVENT;
    if (*name == "SessionStart") {
        observation.session_transition = addon::Session_transition::STARTED;
        result.classification = addon::Classification::TRANSITION;
        const auto source = metadata(body, "source");
        result.transition = source && *source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
    }
    else
    if (*name == "SessionEnd") {
        observation.session_transition = addon::Session_transition::ENDED;
    }
    else {
        const auto turn = metadata(body, "turn_id");
        if (!turn) {
            return std::nullopt;
        }
        observation.provider_turn_id = *turn;
        if (*name == "UserPromptSubmit" || *name == "Stop" || *name == "Interrupt") {
            // A native turn hook is Codex's own report of the turn boundary.
            observation.phase_transition = *name == "UserPromptSubmit"
                ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::CONFIRMED;
            if (*name == "Interrupt") {
                observation.outcome_transition = addon::Outcome_transition::INTERRUPTED;
                observation.outcome_evidence   = addon::Evidence::CONFIRMED;
            }
        }
        else
        if (*name == "PermissionRequest") {
            observation.attention_transition = addon::Attention_transition::REQUESTED;
            observation.attention_evidence   = addon::Evidence::INFERRED;
        }
        else {
            const auto agent = metadata(body, "agent_id");
            const auto label = metadata(body, "agent_type", addon::limits::k_max_lifecycle_label_bytes);
            if (!agent || !label) {
                return std::nullopt;
            }
            observation.agent_id    = *agent;
            observation.agent_label = *label;
            observation.subagent_transition = *name == "SubagentStart"
                ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
        }
    }
    result.observations.push_back(std::move(observation));
    return result;
}

std::optional<addon::Attention_classification_result> classify_attention(
    const addon::Attention_classification_request& request)
{
    const auto& event = request.base.event;
    if (event.name == "NativeSignal") {
        return classify_native_signal(request);
    }
    if (event.name != "PreToolUse" && event.name != "PostToolUse") {
        auto base = classify_hook(request.base);
        if (!base) {
            return std::nullopt;
        }
        addon::Attention_classification_result result{std::move(*base), std::nullopt};
        if (result.base.observations.empty()) {
            return result;
        }
        auto& observation = result.base.observations.front();
        if (event.name == "Stop") {
            const auto* message = event.body.find("last_assistant_message");
            if (message && message->is_string() && message->as_string().size() <= 2048 &&
                addon::looks_like_reply_request(message->as_string()))
            {
                observation.attention_transition = addon::Attention_transition::REQUESTED;
                observation.attention_evidence   = addon::Evidence::INFERRED;
                observation.attention_request_id = "conversation-" + std::to_string(request.base.ledger.current_epoch) + "-" +
                    (request.base.ledger.current_transition ? request.base.ledger.current_transition->nonce : std::string{}) + "-" +
                    addon::provider_event_evidence_digest_hex(event);
                result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
            }
        }
        else
        if (event.name == "UserPromptSubmit" && request.pending_conversational_request_id) {
            observation.attention_transition = addon::Attention_transition::RESOLVED;
            observation.attention_evidence   = addon::Evidence::INFERRED;
            observation.attention_request_id = *request.pending_conversational_request_id;
            result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
        }
        else
        if (event.name == "PermissionRequest") {
            result.attention_kind = addon::Attention_kind{0, "permission"};
        }
        return result;
    }
    const auto& body = event.body;
    if (metadata(body, "hook_event_name") != event.name) {
        return std::nullopt;
    }
    addon::Attention_classification_result result;
    if (body.find("agent_id") || metadata(body, "tool_name") != "request_user_input") {
        return result;
    }
    const auto session = metadata(body, "session_id");
    const auto turn    = metadata(body, "turn_id");
    const auto call    = metadata(body, "tool_use_id");
    const auto ids     = question_ids(body);
    if (!session || !turn || !call || !ids ||
        (event.name == "PostToolUse" && !question_response(body, *ids)))
    {
        return std::nullopt;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = event.name;
    observation.provider_session_id = *session;
    observation.provider_turn_id    = *turn;
    observation.attention_request_id = question_request_id(*session, *turn, *call);
    // PreToolUse is an attempt, before validation and other hooks. The host must
    // corroborate this candidate with PTY presentation before showing an indicator.
    observation.attention_evidence = addon::Evidence::INFERRED;
    observation.attention_transition = event.name == "PreToolUse"
        ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
    result.base.classification = addon::Classification::EVENT;
    result.base.observations.push_back(std::move(observation));
    if (event.name == "PreToolUse") {
        result.attention_kind = addon::Attention_kind{0, "interactive_question_candidate"};
    }
    return result;
}

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    if (context.request().capability.capability_id == addon::k_attention_kind_capability_id) {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto result = classify_attention(*request);
        if (!result) {
            context.fail(addon::Result_status::FAILED, "invalid_provider_event");
            return;
        }
        context.succeed(addon::encode_attention_classification_result(*result));
        return;
    }
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto result = classify_hook(*request);
    if (!result) {
        context.fail(addon::Result_status::FAILED, "invalid_provider_event");
        return;
    }
    context.succeed(addon::encode_classify_event_result(*result));
}

} // namespace codex
