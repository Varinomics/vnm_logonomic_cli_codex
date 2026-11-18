#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <optional>
#include <string>
#include <string_view>

namespace codex {

struct Profile_selection
{
    std::string adapter_account_key;
    std::string profile_path;
};

std::string profile_account_key(std::string_view absolute_path);
std::optional<Profile_selection> granted_profile(const logonomic::cli_addon::Account_context& account,
    std::string_view selected_path);
// Live selection uses Codex's OS fallback; retained exports use only frozen inputs.
std::string native_home_fallback();
std::optional<Profile_selection> profile_from_environment(
    const logonomic::cli_addon::Launch_environment& environment,
    std::string_view default_home_fallback, std::string_view normalization_directory, std::string* error);
std::optional<Profile_selection> profile_selection(
    const logonomic::cli_addon::Launch_environment& environment,
    const logonomic::cli_addon::Launcher_request& launcher,
    std::string_view default_home_fallback, std::string_view normalization_directory, std::string* error);
std::optional<Profile_selection> selected_profile(
    const logonomic::cli_addon::Session_launch_context& request,
    logonomic::cli_addon::Invocation_context& context);
void handle_accounts(logonomic::cli_addon::Invocation_context& context);

} // namespace codex
