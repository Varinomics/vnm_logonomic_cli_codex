#include "codex_startup_observation.h"

#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/startup_observer.h"

#include <algorithm>

namespace codex {
namespace {

const logonomic::cli_addon::Json_value::Array& array_member(
    const logonomic::cli_addon::Json_value& value, const char* name)
{
    static const logonomic::cli_addon::Json_value::Array empty;
    const auto* member = value.find(name);
    return member && member->is_array() ? member->as_array() : empty;
}

std::string string_member(const logonomic::cli_addon::Json_value& value, const char* name)
{
    const auto* member = value.find(name);
    return member && member->is_string() ? member->as_string() : std::string{};
}

std::size_t utf16_units(std::string_view text)
{
    std::size_t result = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xc0) != 0x80) result += byte >= 0xf0 ? 2 : 1;
    }
    return result;
}

} // namespace

std::optional<std::vector<std::string>> conversation_ids(std::string_view body)
{
    namespace addon = logonomic::cli_addon;
    addon::Json_parse_limits limits;
    limits.max_bytes = addon::k_max_startup_observation_bytes;
    const auto document = addon::parse_json(body, limits, nullptr);
    if (!document || !document->is_object()) return std::nullopt;
    std::vector<std::string> result;
    for (const auto& resource : array_member(*document, "resourceLogs")) {
        for (const auto& scope : array_member(resource, "scopeLogs")) {
            for (const auto& record : array_member(scope, "logRecords")) {
                std::string name;
                std::string id;
                bool name_seen = false;
                bool id_seen = false;
                bool duplicate = false;
                for (const auto& attribute : array_member(record, "attributes")) {
                    const auto key = string_member(attribute, "key");
                    const auto* value = attribute.find("value");
                    if (key == "event.name") {
                        duplicate |= name_seen;
                        name_seen = true;
                        name = value ? string_member(*value, "stringValue") : std::string{};
                    }
                    if (key == "conversation.id") {
                        duplicate |= id_seen;
                        id_seen = true;
                        id = value ? string_member(*value, "stringValue") : std::string{};
                    }
                }
                if (duplicate || name != "codex.conversation_starts" || id.empty() ||
                    id.find('\0') != std::string::npos || utf16_units(id) > 256 ||
                    std::find(result.begin(), result.end(), id) != result.end())
                {
                    continue;
                }
                if (result.size() == addon::k_max_startup_observation_ids) return std::nullopt;
                result.push_back(std::move(id));
            }
        }
    }
    return result;
}

} // namespace codex
