#pragma once

#include <logonomic_cli_addon/conversation_export.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace codex {

inline constexpr const char* k_export_sqlite_home_environment = "VNM_CODEX_EXPORT_SQLITE_HOME";

struct Conversation_store_source
{
    std::filesystem::path path;
    std::string history_mode;
};

// Each lookup opens a new read-only connection. A second lookup after the
// finite JSONL read must observe any intervening native rollout replacement.
std::optional<Conversation_store_source> conversation_store_source(
    const std::filesystem::path& profile, const std::filesystem::path& sqlite_home,
    std::string_view session_id, const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

} // namespace codex
