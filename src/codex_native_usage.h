#pragma once

#include "codex_usage_records.h"

namespace codex {

struct Native_cache_file
{
    std::string path;
    std::uint64_t size = 0;
    std::int64_t modified_msecs = 0;
    bool parser_current = false;
};

std::optional<Native_cache_file> convert_native_cache_file(
    const logonomic::cli_addon::Json_value& wrapper,
    const std::function<bool(const Rollout_usage_record&)>& emit,
    std::string* error);

} // namespace codex
