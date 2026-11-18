#pragma once

#include <logonomic_cli_addon/json.h>

#include <optional>
#include <string>

namespace codex {

inline constexpr const char* k_model_source_environment = "VNM_CODEX_MODEL_SOURCE";

std::optional<std::string> model_source_path();
void arm_model_source(const logonomic::cli_addon::Json_value& start, const std::string& nonce);
void capture_turn_model(
    const logonomic::cli_addon::Json_value& event, logonomic::cli_addon::Json_value& metadata);

} // namespace codex
