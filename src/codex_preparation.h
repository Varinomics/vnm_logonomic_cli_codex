#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

#include <string>
#include <string_view>
#include <vector>

namespace codex {

std::string toml_string(std::string_view value);
bool hooks_configuration_conflicts(const std::vector<std::string>& argv, std::size_t prefix_size);
void handle_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace codex
