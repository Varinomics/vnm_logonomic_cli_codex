#pragma once

#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/helper_ingress.h>

#include <optional>

namespace codex {

// A hook supplies the exact native transcript; no account-home or newest-file lookup is used.
std::optional<logonomic::cli_addon::Context_update> context_snapshot(
    const logonomic::cli_addon::Classify_event_request& request);

} // namespace codex
