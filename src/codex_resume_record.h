#pragma once

#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/json.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace codex {

struct Resume_record
{
    std::string session_id;
    std::string codex_home;
    std::string sqlite_home;
};

inline bool canonical_session_id(std::string_view value)
{
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (character != '-') {
                return false;
            }
        }
        else
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline std::optional<Resume_record> decode_resume_record(const logonomic::cli_addon::Json_value& value)
{
    namespace addon = logonomic::cli_addon;
    const auto* id = value.find("provider_session_id");
    if (!value.is_object() || !id || !id->is_string() || !canonical_session_id(id->as_string())) {
        return {};
    }
    Resume_record record{id->as_string(), {}, {}};
    if (value.as_object().size() == 1) {
        return record;
    }
    const auto* store = value.find("native_store");
    const auto* version = store ? store->find("version") : nullptr;
    const auto* profile = store ? store->find("codex_home") : nullptr;
    const auto* sqlite = store ? store->find("sqlite_home") : nullptr;
    if (value.as_object().size() != 2 || !store || !store->is_object() || store->as_object().size() != 3 ||
        !version || *version != addon::Json_value(1) || !profile || !profile->is_string() ||
        !sqlite || !sqlite->is_string() || !addon::is_execution_local_path(profile->as_string()) ||
        !addon::is_execution_local_path(sqlite->as_string()))
    {
        return {};
    }
    record.codex_home  = profile->as_string();
    record.sqlite_home = sqlite->as_string();
    return record;
}

inline std::optional<logonomic::cli_addon::Json_value> bound_resume_record(
    std::string_view session_id, std::string_view codex_home, std::string_view sqlite_home)
{
    namespace addon = logonomic::cli_addon;
    if (!canonical_session_id(session_id) || !addon::is_execution_local_path(codex_home) ||
        !addon::is_execution_local_path(sqlite_home))
    {
        return {};
    }
    auto store = addon::Json_value::object();
    store.set("version", addon::Json_value(1));
    store.set("codex_home", addon::Json_value(std::string(codex_home)));
    store.set("sqlite_home", addon::Json_value(std::string(sqlite_home)));
    auto record = addon::Json_value::object();
    record.set("provider_session_id", addon::Json_value(std::string(session_id)));
    record.set("native_store", std::move(store));
    return record;
}

} // namespace codex
