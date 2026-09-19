#include "codex_lifecycle.h"

#include <logonomic_cli_addon/limits.h>

#include <array>
#include <algorithm>
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
            observation.phase_transition = *name == "UserPromptSubmit"
                ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::INFERRED;
            if (*name == "Interrupt") {
                observation.outcome_transition = addon::Outcome_transition::INTERRUPTED;
                observation.outcome_evidence   = addon::Evidence::INFERRED;
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
    if (event.name != "PreToolUse" && event.name != "PostToolUse") {
        auto base = classify_hook(request.base);
        if (!base) {
            return std::nullopt;
        }
        addon::Attention_classification_result result{std::move(*base), std::nullopt};
        if (!result.base.observations.empty() && event.name == "PermissionRequest") {
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
    addon::Provider_event identity;
    identity.name = "request_user_input";
    identity.body.set("session_id", addon::Json_value(*session));
    identity.body.set("turn_id", addon::Json_value(*turn));
    identity.body.set("tool_use_id", addon::Json_value(*call));

    addon::Lifecycle_observation observation;
    observation.provider_event_name = event.name;
    observation.provider_session_id = *session;
    observation.provider_turn_id    = *turn;
    observation.attention_request_id = "codex-question-" + addon::provider_event_evidence_digest_hex(identity);
    // PreToolUse is an attempt, before validation and other hooks. The host must
    // corroborate this candidate with PTY presentation before showing an indicator.
    observation.attention_evidence = addon::Evidence::INFERRED;
    observation.attention_transition = event.name == "PreToolUse"
        ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
    result.base.classification = addon::Classification::EVENT;
    result.base.observations.push_back(std::move(observation));
    result.attention_kind = addon::Attention_kind{0, "interactive_question"};
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
