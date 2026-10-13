#pragma once

#include "codex_launcher.h"

#include <logonomic_cli_addon/session_wake.h>

namespace codex {

// Captures the running CLI's queue support before birth and pins its native
// state store. An unavailable wake offer never prevents an ordinary launch.
logonomic::cli_addon::Session_wake_preparation prepare_session_wake(
    Verified_launch& launch,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::string& working_directory,
    logonomic::cli_addon::Invocation_context& context);

void handle_session_wake(logonomic::cli_addon::Invocation_context& context);

} // namespace codex
