#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>

#include <optional>

namespace codex {

std::optional<logonomic::cli_addon::Classify_event_result> classify_hook(
    const logonomic::cli_addon::Classify_event_request& request,
    logonomic::cli_addon::Contract_failure* failure = nullptr);
std::optional<logonomic::cli_addon::Attention_classification_result> classify_attention(
    const logonomic::cli_addon::Attention_classification_request& request,
    logonomic::cli_addon::Contract_failure* failure = nullptr);
std::optional<logonomic::cli_addon::Async_question_classification_result> classify_async_attention(
    const logonomic::cli_addon::Attention_classification_request& request,
    logonomic::cli_addon::Contract_failure* failure = nullptr);
void handle_lifecycle(logonomic::cli_addon::Invocation_context& context);

} // namespace codex
