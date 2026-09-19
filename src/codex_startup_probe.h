#pragma once

#include "codex_launcher.h"

#include <logonomic_cli_addon/adapter_runtime.h>

#include <string>
#include <utility>
#include <vector>

namespace codex {

bool startup_supported(
    const Verified_launch& launch,
    const std::vector<std::string>& configuration,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::string& working_directory,
    logonomic::cli_addon::Invocation_context& context,
    std::string* error);

} // namespace codex
