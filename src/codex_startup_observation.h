#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codex {

// nullopt means malformed or over-bound telemetry, not an empty valid batch.
std::optional<std::vector<std::string>> conversation_ids(std::string_view body);

} // namespace codex
