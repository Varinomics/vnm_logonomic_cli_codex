#pragma once

#include <logonomic_cli_addon/conversation_export.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <functional>
#include <optional>

namespace codex {

logonomic::cli_addon::Resume_inspect_result inspect_resume(
    const logonomic::cli_addon::Resume_inspect_request& request,
    const std::function<bool()>& cancelled);

std::optional<logonomic::cli_addon::Conversation_export_result> export_conversation(
    const logonomic::cli_addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

} // namespace codex
