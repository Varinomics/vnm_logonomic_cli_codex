#pragma once

#include <logonomic_cli_addon/session_contracts.h>

#include <string>

namespace codex {

std::string initial_implementation_state();
logonomic::cli_addon::Structured_inspect_result inspect_implementation(
    const logonomic::cli_addon::Structured_inspection& inspection);

} // namespace codex
