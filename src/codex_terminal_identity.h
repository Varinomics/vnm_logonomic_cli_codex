#pragma once

#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/terminal_identity.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codex {

struct Title_identity_preparation
{
    std::vector<std::string> terminal_title;
    logonomic::cli_addon::Terminal_identity_preparation preparation;
};

struct Native_terminal_identity
{
    logonomic::cli_addon::Terminal_identity_observation observation;
    logonomic::cli_addon::Json_value resume_record;
};

std::optional<Title_identity_preparation> prepare_title_identity(
    std::string_view native_version, const logonomic::cli_addon::Json_value& config,
    const std::string& codex_home, const std::string& sqlite_home);

std::optional<Native_terminal_identity> observed_terminal_identity(
    const logonomic::cli_addon::Classify_event_request& request,
    const std::function<bool()>& cancelled = {});

std::optional<logonomic::cli_addon::Terminal_identity_classification_result> classify_title_identity(
    const logonomic::cli_addon::Classify_event_request& request,
    const std::function<bool()>& cancelled = {});

} // namespace codex
