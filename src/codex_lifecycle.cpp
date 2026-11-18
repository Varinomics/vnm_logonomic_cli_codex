#include "codex_lifecycle.h"

#include "codex_context.h"
#include "codex_resume_record.h"
#include "codex_terminal_identity.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/model_state.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>

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

std::optional<std::string> metadata(
    const addon::Json_value& body,
    std::string_view key,
    std::size_t bound = addon::limits::k_max_lifecycle_identity_bytes,
    addon::Contract_failure* failure = nullptr,
    std::string_view event_name = {},
    std::string_view field = {})
{
    const auto* member = body.find(key);
    const auto reject = [&](addon::Provider_event_problem problem) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            std::string(event_name), std::string(field.empty() ? key : field), problem, member,
            bound, "nonempty_single_line_identity"));
    };
    if (!member) {
        return reject(addon::Provider_event_problem::MISSING);
    }
    if (!member->is_string()) {
        return reject(addon::Provider_event_problem::WRONG_TYPE);
    }
    const auto& value = member->as_string();
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return reject(addon::Provider_event_problem::EMPTY);
    }
    const auto result = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (result.size() > bound) {
        auto diagnostic = addon::make_provider_event_diagnostic(
            std::string(event_name), std::string(field.empty() ? key : field),
            addon::Provider_event_problem::TOO_LARGE, member, bound, "nonempty_single_line_identity");
        diagnostic.actual_size = result.size();
        return addon::reject_provider_event(failure, std::move(diagnostic));
    }
    if (result.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos) {
        return reject(addon::Provider_event_problem::CONTROL_CHARACTER);
    }
    return result;
}

std::optional<std::vector<std::string>> question_ids(
    const addon::Provider_event& event, addon::Contract_failure* failure)
{
    const auto& body = event.body;
    const auto* input = body.find("tool_input");
    const auto* questions = input ? input->find("questions") : nullptr;
    if (!questions || !questions->is_array() || questions->as_array().empty() ||
        questions->as_array().size() > 3)
    {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            event.name, "tool_input.questions",
            !questions ? addon::Provider_event_problem::MISSING :
            !questions->is_array() ? addon::Provider_event_problem::WRONG_TYPE :
            questions->as_array().empty() ? addon::Provider_event_problem::EMPTY :
                addon::Provider_event_problem::TOO_LARGE,
            questions, 3, "one_to_three_questions"));
    }
    std::vector<std::string> ids;
    for (const auto& question : questions->as_array()) {
        const std::string field = "tool_input.questions[" + std::to_string(ids.size()) + "]";
        const auto id = metadata(question, "id", addon::limits::k_max_lifecycle_identity_bytes,
            failure, event.name, field + ".id");
        const auto* header = question.find("header");
        const auto* text = question.find("question");
        if (!id) {
            return std::nullopt;
        }
        if (std::find(ids.begin(), ids.end(), *id) != ids.end()) {
            return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                event.name, field + ".id", addon::Provider_event_problem::DUPLICATE,
                question.find("id"), {}, "unique_question_identity"));
        }
        if (!header || !header->is_string()) {
            return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                event.name, field + ".header",
                header ? addon::Provider_event_problem::WRONG_TYPE : addon::Provider_event_problem::MISSING,
                header, {}, "string"));
        }
        if (!text || !text->is_string() || text->as_string().find_first_not_of(" \t\r\n") == std::string::npos) {
            return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                event.name, field + ".question",
                !text ? addon::Provider_event_problem::MISSING :
                !text->is_string() ? addon::Provider_event_problem::WRONG_TYPE : addon::Provider_event_problem::EMPTY,
                text, {}, "nonempty_question_text"));
        }
        ids.push_back(*id);
    }
    return ids;
}

bool question_response(
    const addon::Provider_event& event, const std::vector<std::string>& ids, addon::Contract_failure* failure)
{
    const auto& body = event.body;
    const auto reject = [
            &event,
            failure
        ](
            std::string field,
            addon::Provider_event_problem problem,
            const addon::Json_value* actual,
            std::string rule)
        {
            addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                event.name, std::move(field), problem, actual, {}, std::move(rule)));
            return false;
        };
    const auto* response = body.find("tool_response");
    if (!response) {
        return reject("tool_response", addon::Provider_event_problem::MISSING, response, "question_response");
    }
    std::string response_field = "tool_response";
    // FunctionToolOutput exposes its serialized model-facing content to PostToolUse.
    if (response->is_array()) {
        if (response->as_array().size() != 1) {
            return reject("tool_response", addon::Provider_event_problem::INVALID_SHAPE,
                response, "single_input_text_content");
        }
        const auto& content = response->as_array().front();
        if (metadata(content, "type") != "input_text") {
            return reject("tool_response[0].type", addon::Provider_event_problem::MISMATCH,
                content.find("type"), "input_text_content");
        }
        response = content.find("text");
        response_field = "tool_response[0].text";
    }
    if (!response || !response->is_string()) {
        return reject(response_field, response ? addon::Provider_event_problem::WRONG_TYPE :
            addon::Provider_event_problem::MISSING, response, "serialized_question_answers");
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = 4 * 1024 * 1024;
    limits.max_string_bytes = limits.max_bytes;
    const auto decoded = addon::parse_json(response->as_string(), limits, nullptr);
    const auto* answers = decoded ? decoded->find("answers") : nullptr;
    if (!decoded) {
        return reject(response_field, addon::Provider_event_problem::INVALID_VALUE,
            response, "json_question_answers");
    }
    const std::string answers_field = response_field + ".answers";
    if (!answers || !answers->is_object()) {
        return reject(answers_field, answers ? addon::Provider_event_problem::WRONG_TYPE :
            addon::Provider_event_problem::MISSING, answers, "question_answer_map");
    }
    std::size_t entry_index = 0;
    for (const auto& [id, answer] : answers->as_object()) {
        const std::string entry_field = answers_field + "[" + std::to_string(entry_index) + "]";
        const auto* values = answer.find("answers");
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            return reject(entry_field, addon::Provider_event_problem::MISMATCH,
                &answer, "known_question_identities");
        }
        if (!answer.is_object()) {
            return reject(entry_field, addon::Provider_event_problem::WRONG_TYPE, &answer, "question_answer_entry");
        }
        const std::string values_field = entry_field + ".answers";
        if (!values || !values->is_array()) {
            return reject(values_field, values ? addon::Provider_event_problem::WRONG_TYPE :
                addon::Provider_event_problem::MISSING, values, "answer_array");
        }
        for (std::size_t value_index = 0; value_index < values->as_array().size(); ++value_index) {
            const auto& value = values->as_array()[value_index];
            if (!value.is_string()) {
                return reject(values_field + "[" + std::to_string(value_index) + "]",
                    addon::Provider_event_problem::WRONG_TYPE,
                    &value, "string_answers");
            }
        }
        ++entry_index;
    }
    // Empty answers also resolve the native request; they do not establish a user answer.
    return true;
}

std::string question_request_id(const std::string& session, const std::string& turn, const std::string& call,
    bool asynchronous = false)
{
    addon::Provider_event identity;
    identity.name = asynchronous ? "request_user_input_async" : "request_user_input";
    identity.body.set("session_id", addon::Json_value(session));
    identity.body.set("turn_id", addon::Json_value(turn));
    identity.body.set("tool_use_id", addon::Json_value(call));
    return (asynchronous ? "codex-async-question-" : "codex-question-") +
        addon::provider_event_evidence_digest_hex(identity);
}

std::string permission_request_id(const std::string& session, const std::string& turn)
{
    // PermissionRequest has no native call ID. The TUI presents concurrent
    // approvals in one overlay, so retain one presentation candidate per turn.
    addon::Provider_event identity;
    identity.name = "PermissionRequest";
    identity.body.set("session_id", addon::Json_value(session));
    identity.body.set("turn_id", addon::Json_value(turn));
    return "codex-permission-" + addon::provider_event_evidence_digest_hex(identity);
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

bool progress_title(std::string_view title)
{
    // Codex suppresses these native activity frames while an approval overlay
    // requires action. Activity can sit anywhere in the configured title and
    // is separated from neighboring fields by spaces after native sanitization.
    constexpr std::array<std::string_view, 10> frames{
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    for (;;) {
        const auto separator = title.find(' ');
        const auto item      = title.substr(0, separator);
        if (std::find(frames.begin(), frames.end(), item) != frames.end()) {
            return true;
        }
        if (separator == std::string_view::npos) {
            return false;
        }
        title.remove_prefix(separator + 1);
    }
}

std::optional<addon::Attention_classification_result> classify_native_signal(
    const addon::Attention_classification_request& request, bool asynchronous,
    addon::Contract_failure* failure)
{
    // This envelope is supplied only by the host terminal lane, never helper
    // ingress. The host owns authentication, generation/epoch/sequence freshness,
    // and matching the returned request ID to its retained root candidate.
    const auto& body = request.base.event.body;
    const auto reject = [
            &request,
            failure
        ](
            std::string field,
            addon::Provider_event_problem problem,
            const addon::Json_value* actual,
            std::string rule)
        {
            return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                request.base.event.name, std::move(field), problem, actual, {}, std::move(rule)));
        };
    if (!exact_members(body, {"origin", "terminal_event", "candidate"})) {
        return reject("body", addon::Provider_event_problem::INVALID_SHAPE, &body, "native_attention_envelope");
    }
    if (*body.find("origin") != addon::Json_value("host_terminal")) {
        return reject("origin", addon::Provider_event_problem::MISMATCH,
            body.find("origin"), "host_terminal_origin");
    }
    const auto& terminal = *body.find("terminal_event");
    const auto& candidate = *body.find("candidate");
    const bool permission = exact_members(candidate, {"session_id", "turn_id", "attention_kind"}) &&
        metadata(candidate, "attention_kind") == "permission";
    const bool async_question = asynchronous &&
        exact_members(candidate, {"session_id", "turn_id", "tool_use_id", "attention_kind"}) &&
        metadata(candidate, "attention_kind") == "async_question";
    if (!exact_members(terminal, {"terminal_id", "generation", "source", "stream", "stream_index", "timestamp", "payload"})) {
        return reject("terminal_event", addon::Provider_event_problem::INVALID_SHAPE,
            &terminal, "terminal_event_members");
    }
    if (!permission && !async_question && !exact_members(candidate, {"session_id", "turn_id", "tool_use_id"})) {
        return reject("candidate", addon::Provider_event_problem::INVALID_SHAPE,
            &candidate, "native_attention_candidate");
    }
    for (const char* key : {"terminal_id", "generation"}) {
        if (!positive_decimal(*terminal.find(key))) {
            return reject(std::string("terminal_event.") + key, addon::Provider_event_problem::INVALID_VALUE,
                terminal.find(key), "positive_decimal_identity");
        }
    }
    for (const auto& [key, expected] : std::array<std::pair<const char*, addon::Json_value>, 3>{
             {{"source", addon::Json_value("logonomic_terminal_worker")},
              {"stream", addon::Json_value("terminal")}, {"stream_index", addon::Json_value(0)}}})
    {
        if (*terminal.find(key) != expected) {
            return reject(std::string("terminal_event.") + key, addon::Provider_event_problem::MISMATCH,
                terminal.find(key), "native_terminal_stream");
        }
    }
    if (!terminal.find("timestamp")->is_number()) {
        return reject("terminal_event.timestamp", addon::Provider_event_problem::WRONG_TYPE,
            terminal.find("timestamp"), "numeric_timestamp");
    }
    const auto& payload = *terminal.find("payload");
    if (!exact_members(payload, {"kind", "title_sequence", "title"})) {
        return reject("terminal_event.payload", addon::Provider_event_problem::INVALID_SHAPE,
            &payload, "terminal_title_members");
    }
    if (*payload.find("kind") != addon::Json_value("terminal_title_changed")) {
        return reject("terminal_event.payload.kind", addon::Provider_event_problem::MISMATCH,
            payload.find("kind"), "terminal_title_changed");
    }
    if (!positive_decimal(*payload.find("title_sequence"))) {
        return reject("terminal_event.payload.title_sequence", addon::Provider_event_problem::INVALID_VALUE,
            payload.find("title_sequence"), "positive_decimal_sequence");
    }
    const auto session = metadata(candidate, "session_id", addon::limits::k_max_lifecycle_identity_bytes,
        failure, request.base.event.name, "candidate.session_id");
    if (!session) {
        return std::nullopt;
    }
    const auto turn = metadata(candidate, "turn_id", addon::limits::k_max_lifecycle_identity_bytes,
        failure, request.base.event.name, "candidate.turn_id");
    if (!turn) {
        return std::nullopt;
    }
    const auto call = metadata(candidate, "tool_use_id", addon::limits::k_max_lifecycle_identity_bytes,
        permission ? nullptr : failure, request.base.event.name, "candidate.tool_use_id");
    if (!permission && !call) {
        return std::nullopt;
    }
    const auto& ledger = request.base.ledger;
    if (!ledger.current_epoch || !ledger.current_transition) {
        return reject("ledger.current_transition", addon::Provider_event_problem::MISSING,
            nullptr, "current_native_transition");
    }
    if (ledger.current_transition->provider_session_id != *session) {
        return reject("candidate.session_id", addon::Provider_event_problem::MISMATCH,
            candidate.find("session_id"), "current_native_session");
    }
    const auto& title = *payload.find("title");
    if (!title.is_null() && !title.is_string()) {
        return reject("terminal_event.payload.title", addon::Provider_event_problem::WRONG_TYPE,
            &title, "nullable_title_string");
    }
    addon::Attention_classification_result result;
    const bool required = title.is_string() && action_required_title(title.as_string());
    const bool resumed  = (permission || async_question) && title.is_string() && progress_title(title.as_string());
    // Native status can be absent from a customized title. Only an explicit
    // activity frame distinguishes resumption from missing observation.
    if (!required && !resumed) {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name   = request.base.event.name;
    observation.provider_session_id   = *session;
    observation.provider_turn_id      = *turn;
    observation.attention_request_id  = permission
        ? permission_request_id(*session, *turn)
        : question_request_id(*session, *turn, *call, async_question);
    observation.attention_transition  = required
        ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
    observation.attention_evidence    = addon::Evidence::INFERRED;
    result.base.classification = addon::Classification::EVENT;
    result.base.observations.push_back(std::move(observation));
    result.attention_kind = addon::Attention_kind{0, permission ? "permission" : "interactive_question"};
    return result;
}

} // namespace

std::optional<addon::Classify_event_result> classify_hook(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    const auto& body = request.event.body;
    const auto name = metadata(body, "hook_event_name", addon::limits::k_max_lifecycle_identity_bytes,
        failure, request.event.name);
    if (!name) {
        return std::nullopt;
    }
    if (*name != request.event.name) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "hook_event_name", addon::Provider_event_problem::MISMATCH,
            body.find("hook_event_name"), {}, "matches_event_name"));
    }
    addon::Classify_event_result result;
    result.model_failure = request.model_failure;
    constexpr std::array<std::string_view, 9> supported{
        "SessionStart", "SessionEnd", "UserPromptSubmit", "Stop", "Interrupt", "PermissionRequest",
        "SubagentStart", "SubagentStop", "PostCompact"};
    if (std::find(supported.begin(), supported.end(), *name) == supported.end()) {
        return result;
    }
    if (request.model_state_version == addon::k_model_state_version && !request.model_failure &&
        (*name == "UserPromptSubmit" || *name == "SubagentStart") && body.find("model_state"))
    {
        addon::decode_model_updates(body, result.model_updates, result.model_failure);
        const auto& transition = request.ledger.current_transition;
        const auto actor = addon::json::text(body, "agent_id");
        if (result.model_updates.size() != 1 || !transition ||
            (*name == "SubagentStart" && actor.empty()) || (body.find("agent_id") && actor.empty()))
        {
            result.model_updates.clear();
        }
        else {
            const auto& sample = result.model_updates.front();
            if (sample.provider_session_id != addon::json::text(body, "session_id") ||
                sample.provider_session_id != transition->provider_session_id || sample.transition_nonce != transition->nonce ||
                sample.agent_id != actor || sample.sequence == 0 || sample.status != addon::Model_status::CONFIRMED ||
                sample.source != "codex." + *name || !sample.provider.empty() || sample.reasoning_effort)
            {
                result.model_updates.clear();
            }
        }
    }
    if (*name != "SubagentStart" && *name != "SubagentStop" && body.find("agent_id")) {
        return result;
    }
    const auto session = metadata(body, "session_id", addon::limits::k_max_lifecycle_identity_bytes,
        failure, request.event.name);
    if (!session) {
        return std::nullopt;
    }
    const auto source = metadata(body, "source");
    std::optional<Native_terminal_identity> selected;
    if (*name == "SessionStart" && source != "compact") {
        selected = observed_terminal_identity(request);
        if (selected && selected->observation.provider_session_id != *session) {
            // A delayed reporting hook cannot undo a newer displayed selection.
            return result;
        }
    }
    const bool already_selected = selected && request.ledger.current_transition &&
        request.ledger.current_transition->provider_session_id == *session;
    if (const auto snapshot = context_snapshot(request)) {
        result.context_updates.push_back(*snapshot);
    }
    if (request.context_failure && !body.find("agent_id")) {
        result.context_failure = addon::Context_failure{*session, *request.context_failure};
    }
    if (*name == "PostCompact" || (*name == "SessionStart" && source == "compact")) {
        // Native compaction reuses the root conversation. It is not an activation.
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = *name;
    observation.provider_session_id = *session;
    result.classification = addon::Classification::EVENT;
    if (*name == "SessionStart") {
        observation.session_transition = addon::Session_transition::STARTED;
        result.classification = addon::Classification::TRANSITION;
        result.transition = source && *source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        observation.session_source = source.value_or("");
        // Enrol only the first, newly-created native root. The committed
        // response reaches its synchronous hook before Codex samples turns.
        if (request.model_state_version == addon::k_model_state_version && !request.model_failure &&
            source == "startup" && canonical_session_id(*session) &&
            ((!request.ledger.current_transition && request.ledger.current_epoch == 0) ||
             (already_selected && request.ledger.current_epoch == 1)))
        {
            const auto& nonce = already_selected ? request.ledger.current_transition->nonce : request.transition_nonce;
            result.helper_response = addon::json::object({{"model_activation", addon::Json_value(nonce)}});
        }
        if (!already_selected && request.model_state_version == addon::k_model_state_version &&
            !request.model_failure)
        {
            if (const auto* model = body.find("model")) {
                addon::Model_update update;
                update.provider_session_id = *session;
                update.transition_nonce    = request.transition_nonce;
                update.status              = addon::Model_status::CONFIRMED;
                update.source              = "codex.SessionStart";
                if (model->is_string()) {
                    update.model = model->as_string();
                }
                // The native start supplies its own live model. No backend is
                // identified, and the joint transition owns sequence zero.
                if (addon::is_valid_model_update(update)) {
                    result.model_updates.push_back(std::move(update));
                }
                else {
                    result.model_failure = addon::Contract_failure{
                        "invalid_model_observation", "Codex SessionStart model is malformed"};
                }
            }
        }
        if (const auto* record = body.find("resume_record")) {
            const auto decoded = decode_resume_record(*record);
            if (decoded && decoded->session_id == *session && !decoded->sqlite_home.empty()) {
                observation.resume_record = *record;
            }
        }
        if (request.execution_context_version == 1 && !request.execution_failure) {
            addon::Execution_context_update update;
            update.provider_session_id = *session;
            update.transition_nonce    = already_selected
                ? request.ledger.current_transition->nonce : request.transition_nonce;
            update.source              = "codex.SessionStart";
            const auto* cwd = body.find("cwd");
            if (cwd && cwd->is_string() && addon::is_execution_local_path(cwd->as_string())) {
                update.status      = addon::Execution_context_status::CONFIRMED;
                update.environment = "local";
                update.cwd         = cwd->as_string();
            }
            // This joint native start owns sequence zero. Tool hooks may report
            // per-call workdir overrides, not the conversation's default cwd.
            result.execution_updates.push_back(std::move(update));
        }
        if (already_selected) {
            // The title already opened this activation. Keep the native cwd
            // and context evidence under that nonce without opening it again.
            result.classification = addon::Classification::IGNORED;
            result.transition.reset();
            return result;
        }
    }
    else
    if (*name == "SessionEnd") {
        observation.session_transition = addon::Session_transition::ENDED;
        observation.session_end_reason = metadata(body, "reason").value_or("");
    }
    else {
        const auto turn = metadata(body, "turn_id", addon::limits::k_max_lifecycle_identity_bytes,
            failure, request.event.name);
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
            const auto agent = metadata(body, "agent_id", addon::limits::k_max_lifecycle_identity_bytes,
                failure, request.event.name);
            if (!agent) {
                return std::nullopt;
            }
            const auto label = metadata(body, "agent_type", addon::limits::k_max_lifecycle_label_bytes,
                failure, request.event.name);
            if (!label) {
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
    const addon::Attention_classification_request& request, addon::Contract_failure* failure)
{
    const auto& event = request.base.event;
    if (event.name == "NativeSignal") {
        return classify_native_signal(request, false, failure);
    }
    if (event.name != "PreToolUse" && event.name != "PostToolUse") {
        auto base = classify_hook(request.base, failure);
        if (!base) {
            return std::nullopt;
        }
        addon::Attention_classification_result result{std::move(*base), std::nullopt};
        if (result.base.observations.empty()) {
            return result;
        }
        auto& observation = result.base.observations.front();
        if (event.name == "Stop") {
            const auto* evidence = event.body.find("assistant_reply_evidence");
            if (evidence && addon::looks_like_reply_request_evidence(*evidence)) {
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
            observation.attention_request_id = permission_request_id(
                observation.provider_session_id, observation.provider_turn_id);
            result.attention_kind = addon::Attention_kind{0, "permission_candidate"};
        }
        return result;
    }
    const auto& body = event.body;
    const auto name = metadata(body, "hook_event_name", addon::limits::k_max_lifecycle_identity_bytes,
        failure, event.name);
    if (!name) {
        return std::nullopt;
    }
    if (*name != event.name) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            event.name, "hook_event_name", addon::Provider_event_problem::MISMATCH,
            body.find("hook_event_name"), {}, "matches_event_name"));
    }
    addon::Attention_classification_result result;
    if (body.find("agent_id") || metadata(body, "tool_name") != "request_user_input") {
        return result;
    }
    const auto session = metadata(body, "session_id", addon::limits::k_max_lifecycle_identity_bytes,
        failure, event.name);
    if (!session) {
        return std::nullopt;
    }
    const auto turn = metadata(body, "turn_id", addon::limits::k_max_lifecycle_identity_bytes,
        failure, event.name);
    if (!turn) {
        return std::nullopt;
    }
    const auto call = metadata(body, "tool_use_id", addon::limits::k_max_lifecycle_identity_bytes,
        failure, event.name);
    if (!call) {
        return std::nullopt;
    }
    const auto ids = question_ids(event, failure);
    if (!ids || (event.name == "PostToolUse" && !question_response(event, *ids, failure))) {
        return std::nullopt;
    }
    if (const auto snapshot = context_snapshot(request.base)) {
        result.base.context_updates.push_back(*snapshot);
    }
    if (request.base.context_failure) {
        result.base.context_failure = addon::Context_failure{*session, *request.base.context_failure};
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

std::optional<addon::Async_question_classification_result> classify_async_attention(
    const addon::Attention_classification_request& request, addon::Contract_failure* failure)
{
    const auto& event = request.base.event;
    auto base = event.name == "NativeSignal"
        ? classify_native_signal(request, true, failure) : classify_attention(request, failure);
    if (!base) {
        return std::nullopt;
    }
    addon::Async_question_classification_result result{std::move(*base), std::nullopt};
    const auto& body = event.body;
    if (body.find("agent_id") || metadata(body, "hook_event_name") != event.name) {
        return result;
    }
    const auto session = metadata(body, "session_id");
    const auto turn = metadata(body, "turn_id");
    if (!session || !turn) {
        return result;
    }
    if (event.name == "Interrupt" || event.name == "SessionEnd") {
        if (!result.base.base.observations.empty()) {
            result.async_questions = addon::Async_question_update{0, addon::Async_question_update_kind::RESET, {}};
        }
        return result;
    }
    const bool question = (event.name == "PreToolUse" || event.name == "PostToolUse") &&
        metadata(body, "tool_name") == "request_user_input_async";
    const auto* ids = body.find(question ? "async_question_ids" : "async_answer_ids");
    if ((!question && event.name != "UserPromptSubmit") || !ids || !ids->is_array() ||
        ids->as_array().empty() || ids->as_array().size() > addon::k_max_async_question_ids)
    {
        return result;
    }
    addon::Async_question_update update;
    update.kind = question ? (event.name == "PostToolUse" ? addon::Async_question_update_kind::ACCEPTED
        : addon::Async_question_update_kind::STARTED) : addon::Async_question_update_kind::ANSWERED;
    const auto call = metadata(body, "tool_use_id");
    for (const auto& id : ids->as_array()) {
        if (!id.is_string() || id.as_string().empty() || id.as_string().size() > addon::k_max_async_question_id_bytes ||
            id.as_string().find('\0') != std::string::npos ||
            std::find(update.question_ids.begin(), update.question_ids.end(), id.as_string()) != update.question_ids.end())
        {
            return result;
        }
        if (question) {
            if (!call) {
                return result;
            }
            auto identity = addon::Json_value::array();
            identity.push_back(addon::Json_value("request_user_input_async"));
            identity.push_back(addon::Json_value(*call));
            identity.push_back(addon::Json_value(static_cast<std::int64_t>(update.question_ids.size())));
            if (addon::serialize_json(identity) != id.as_string()) {
                return result;
            }
        }
        update.question_ids.push_back(id.as_string());
    }
    if (question) {
        addon::Lifecycle_observation observation;
        observation.provider_event_name = event.name;
        observation.provider_session_id = *session;
        observation.provider_turn_id = *turn;
        observation.attention_request_id = question_request_id(*session, *turn, *call, true);
        observation.attention_transition = addon::Attention_transition::REQUESTED;
        observation.attention_evidence = addon::Evidence::INFERRED;
        result.base.base.classification = addon::Classification::EVENT;
        result.base.base.observations.push_back(std::move(observation));
        result.base.attention_kind = addon::Attention_kind{0, "interactive_question_candidate"};
        if (const auto snapshot = context_snapshot(request.base)) {
            result.base.base.context_updates.push_back(*snapshot);
        }
        if (request.base.context_failure) {
            result.base.base.context_failure = addon::Context_failure{*session, *request.base.context_failure};
        }
    }
    if (!result.base.base.observations.empty()) {
        result.async_questions = std::move(update);
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
    if (context.request().capability.capability_id == addon::k_terminal_identity_capability_id) {
        const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto result = classify_title_identity(*request, [&] { return context.cancel_requested(); }, &failure);
        if (!result) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_terminal_identity_classification_result(*result));
        return;
    }
    if (context.request().capability.capability_id == addon::k_async_question_attention_capability_id) {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto result = classify_async_attention(*request, &failure);
        if (!result) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_async_question_classification_result(*result));
        return;
    }
    if (context.request().capability.capability_id == addon::k_attention_kind_capability_id) {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto result = classify_attention(*request, &failure);
        if (!result) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
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
    const auto result = classify_hook(*request, &failure);
    if (!result) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(addon::encode_classify_event_result(*result));
}

} // namespace codex
