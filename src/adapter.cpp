#include "codex_accounts.h"
#include "codex_conversation.h"
#include "codex_jsonl.h"
#include "codex_launcher.h"
#include "codex_launcher_approval.h"
#include "codex_lifecycle.h"
#include "codex_pet.h"
#include "codex_preparation.h"
#include "codex_probes.h"
#include "codex_startup_observation.h"
#include "codex_session_wake.h"
#include "codex_usage.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/startup_observer.h>

#include <exception>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

void discovery(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_discover_operation) {
        const auto request = addon::decode_discover_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_candidates(codex::discover_launchers(*request)));
    }
    else
    if (context.request().operation == addon::k_recognize_operation) {
        const auto request = addon::decode_recognize_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        addon::Launcher_recognition recognition;
        recognition.recognized = codex::recognize_launcher(request->launcher.script_body, request->launcher.runner_key).has_value();
        if (recognition.recognized) recognition.display_name = "Codex";
        context.succeed(addon::encode_recognition(recognition));
    }
    else context.reject_unknown_operation();
}

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    try {
        if (context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, "cancelled");
        }
        else
        if (request.capability.capability_id == addon::k_launcher_approval_capability_id) {
            addon::handle_launcher_approval(context, codex::launcher_approval_rules());
        }
        else
        if (request.capability.capability_id == "launcher-discovery") {
            discovery(context);
        }
        else
        if (request.capability.capability_id == addon::k_conversation_export_capability_id) {
            addon::handle_conversation_export(context, codex::export_conversation);
        }
        else
        if (request.capability.capability_id == addon::k_session_wake_capability_id) {
            codex::handle_session_wake(context);
        }
        else
        if (request.capability.capability_id == "accounts") {
            codex::handle_accounts(context);
        }
        else
        if (request.capability.capability_id == addon::k_account_pet_capability_id) {
            codex::handle_account_pet(context);
        }
        else
        if (request.capability.capability_id == "auth-status" || request.capability.capability_id == "quota" ||
            request.capability.capability_id == addon::k_account_refresh_capability_id) {
            codex::handle_probes(context);
        }
        else
        if (request.capability.capability_id == "usage-scan") {
            codex::handle_usage(context);
        }
        else
        if (request.capability.capability_id == addon::k_usage_incremental_capability_id) {
            codex::handle_usage_incremental(context);
        }
        else
        if (request.capability.capability_id == addon::k_usage_repository_attribution_capability_id) {
            codex::handle_usage_repository_attribution(context);
        }
        else
        if (request.capability.capability_id == addon::k_usage_session_capability_id) {
            codex::handle_usage_session(context);
        }
        else
        if (request.capability.capability_id == "profile-storage" && request.operation == "acknowledge") {
            addon::acknowledge_profile_storage(context);
        }
        else
        if (request.capability.capability_id == "lifecycle-events" ||
            request.capability.capability_id == "helper-ingress" ||
            request.capability.capability_id == addon::k_terminal_identity_capability_id ||
            request.capability.capability_id == addon::k_async_question_attention_capability_id ||
            request.capability.capability_id == addon::k_attention_kind_capability_id)
        {
            codex::handle_lifecycle(context);
        }
        else
        if (request.capability.capability_id == "interactive-resume" && request.operation == addon::k_classify_startup_operation) {
            addon::Contract_failure failure;
            const auto observation = addon::decode_startup_observation_request(request.payload, &failure);
            const auto ids = observation ? codex::conversation_ids(observation->body) : std::nullopt;
            if (!ids) context.fail(addon::Result_status::FAILED, "invalid_startup_observation");
            else context.succeed(addon::encode_startup_observation_result({*ids}));
        }
        else
        if (request.capability.capability_id == "structured-session" && request.operation == addon::k_inspect_operation) {
            addon::Contract_failure failure;
            const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure);
            if (!inspection) context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            else {
                auto result = codex::inspect_implementation(inspection->inspection);
                if (inspection->purpose == addon::Structured_purpose::READ_ONLY_REVIEW && result.failure_reason &&
                    result.failure_reason->starts_with("implementation_"))
                {
                    result.failure_reason = "review_" +
                        result.failure_reason->substr(std::string_view("implementation_").size());
                }
                context.succeed(addon::encode_structured_inspect_result(result, inspection->report_failure_diagnostics));
            }
        }
        else {
            codex::handle_preparation(context);
        }
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "codex_operation_failed",
            "Codex could not preserve the supplied configuration or owned storage.");
    }
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "codex-addon";
    options.adapter_version = LOGONOMIC_CLI_ADDON_ADAPTER_VERSION;
    options.capabilities = {
        {addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
        {"presentation", 1}, {"launcher-discovery", 1}, {"profile-storage", 1},
        {addon::k_conversation_export_capability_id, addon::k_conversation_export_version},
        {"launch-preparation", 3}, {"structured-session", 3}, {"delegated-session", 3},
        {addon::k_delegated_model_selection_capability_id, addon::k_delegated_model_selection_version},
        {addon::k_structured_failure_diagnostics_capability_id, addon::k_structured_failure_diagnostics_version},
        {addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version},
        {addon::k_terminal_codex_invocation_capability_id, addon::k_terminal_codex_invocation_version},
        {addon::k_terminal_identity_capability_id, addon::k_terminal_identity_version},
        {addon::k_session_wake_capability_id, addon::k_session_wake_version},
        {addon::k_model_context_delivery_capability_id, addon::k_model_context_delivery_version},
        {"interactive-resume", 2}, {"interactive-resume", 3}, {"interactive-resume", 4},
        {"lifecycle-events", 2}, {"helper-ingress", 2},
        {addon::k_execution_context_capability_id, addon::k_execution_context_version},
        {"resume-directory", 2},
        {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        {addon::k_async_question_attention_capability_id, addon::k_async_question_attention_version},
        {addon::k_context_state_capability_id, addon::k_context_state_capability_version},
        {"accounts", 2}, {"auth-status", 2}, {"quota", 2}, {"usage-scan", 3},
        {addon::k_account_pet_capability_id, addon::k_account_pet_version},
        {"current-account", 1},
        {addon::k_quota_failure_capability_id, addon::k_quota_failure_version},
        {addon::k_account_refresh_capability_id, addon::k_account_refresh_version},
        {addon::k_usage_cross_source_event_capability_id, addon::k_usage_cross_source_event_version},
        {addon::k_usage_temporal_capability_id, addon::k_usage_temporal_version},
        {addon::k_usage_incremental_capability_id, addon::k_usage_incremental_version},
        {addon::k_usage_repository_attribution_capability_id, addon::k_usage_repository_attribution_version},
        {addon::k_usage_session_capability_id, addon::k_usage_session_version},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
