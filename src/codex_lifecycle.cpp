#include "codex_lifecycle.h"

#include <logonomic_cli_addon/limits.h>

#include <array>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

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

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
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
