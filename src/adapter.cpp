#include "codex_accounts.h"
#include "codex_jsonl.h"
#include "codex_launcher.h"
#include "codex_lifecycle.h"
#include "codex_preparation.h"
#include "codex_probes.h"
#include "codex_startup_observation.h"
#include "codex_usage.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/startup_observer.h>

#include <exception>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

void history(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_import_record_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (request->kind == addon::k_legacy_account_record_kind) {
        context.succeed(addon::encode_import_record_result(codex::import_account(*request)));
        return;
    }
    if (request->kind == addon::k_legacy_usage_records_kind) {
        context.succeed(addon::encode_import_record_result(codex::import_usage(*request)));
        return;
    }
    if (request->kind != addon::k_legacy_resume_record_kind) {
        context.fail(addon::Result_status::UNAVAILABLE, "unknown_record_kind");
        return;
    }
    const auto record = addon::decode_legacy_resume_record(request->record, &failure);
    if (!record) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Legacy_import_result result;
    if (record->provider_id == "codex") {
        auto value = addon::Json_value::object();
        value.set("provider_session_id", addon::Json_value(record->provider_session_id));
        result = *addon::Legacy_import_result::from_conversion(addon::encode_legacy_resume_conversion(value));
    }
    context.succeed(addon::encode_import_record_result(result));
}

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
        recognition.recognized = codex::recognize_launcher(request->launcher.script_body).has_value();
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
        if (request.capability.capability_id == "launcher-discovery") {
            discovery(context);
        }
        else
        if (request.capability.capability_id == "accounts") {
            codex::handle_accounts(context);
        }
        else
        if (request.capability.capability_id == "auth-status" || request.capability.capability_id == "quota") {
            codex::handle_probes(context);
        }
        else
        if (request.capability.capability_id == "usage-scan") {
            codex::handle_usage(context);
        }
        else
        if (request.capability.capability_id == "legacy-state-import" && request.operation == addon::k_import_record_operation) {
            history(context);
        }
        else
        if (request.capability.capability_id == "profile-storage" && request.operation == "acknowledge") {
            const auto* storage = request.payload.find("profile_storage");
            const auto decoded = storage ? addon::decode_profile_storage_context(*storage, nullptr) : std::nullopt;
            if (!request.payload.is_object() || request.payload.as_object().size() != 1 || !decoded) {
                context.fail(addon::Result_status::FAILED, "profile_storage_mismatch");
            }
            else {
                auto response = addon::Json_value::object();
                response.set("profile_storage", addon::profile_storage_member(*decoded));
                context.succeed(std::move(response));
            }
        }
        else
        if (request.capability.capability_id == "lifecycle-events" ||
            request.capability.capability_id == "helper-ingress" ||
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
            const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure, 2);
            if (!inspection) context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            else if (inspection->purpose != addon::Structured_purpose::IMPLEMENTATION) context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
            else context.succeed(addon::encode_structured_inspect_result(codex::inspect_implementation(inspection->inspection), 2));
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
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"profile-storage", 1},
        {"launch-preparation", 3}, {"structured-session", 2}, {"delegated-session", 2},
        {"interactive-resume", 2}, {"lifecycle-events", 1}, {"helper-ingress", 2},
        {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        {"accounts", 2}, {"auth-status", 2}, {"quota", 2}, {"usage-scan", 2}, {"usage-scan", 3}, {"legacy-state-import", 1},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
