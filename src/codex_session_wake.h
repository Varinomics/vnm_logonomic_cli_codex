#pragma once

#include "codex_launcher.h"

#include <logonomic_cli_addon/session_wake.h>
#include <logonomic_cli_addon/terminal_identity.h>

#include <optional>

namespace codex {

struct Native_launch_preparation
{
    logonomic::cli_addon::Session_wake_preparation  session_wake;
    std::optional<std::vector<std::string>>        terminal_title;
    std::optional<logonomic::cli_addon::Terminal_identity_preparation> terminal_identity;
};

// Resolves configuration using the selected executable, account and directory.
// Runtime profile selectors unsupported by app-server remain unavailable.
std::optional<logonomic::cli_addon::Json_value> read_native_configuration(
    const Verified_launch& launch,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::string& working_directory,
    logonomic::cli_addon::Invocation_context& context);

// Uses one native configuration read to retain title activity and pin the
// state store for wake and conversation export. Preparation also runs when
// wake was not requested. An unavailable offer never prevents ordinary launch.
Native_launch_preparation prepare_native_launch(
    Verified_launch& launch,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::string& working_directory,
    logonomic::cli_addon::Invocation_context& context,
    bool offer_terminal_identity = false);

void handle_session_wake(logonomic::cli_addon::Invocation_context& context);

} // namespace codex
