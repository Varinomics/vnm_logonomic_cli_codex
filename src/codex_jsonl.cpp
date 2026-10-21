#include "codex_jsonl.h"

#include <logonomic_cli_addon/json.h>

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;
constexpr std::size_t k_max_bytes = 1024 * 1024;
constexpr std::size_t k_max_items = 4096;

std::string text(const Json& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value && value->is_string() ? value->as_string() : std::string{};
}

addon::Structured_inspect_result failed(std::string reason = "implementation_protocol_invalid")
{
    addon::Structured_inspect_result result;
    result.complete       = true;
    result.failure_reason = std::move(reason);
    return result;
}

bool nonempty(std::string_view value)
{
    return value.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

} // namespace

std::string initial_implementation_state()
{
    return R"({"pending_base64":"","result_base64":"","root_thread_id":"","turn_started":false,"turn_completed":false,"completed_item_ids":[]})";
}

addon::Structured_inspect_result inspect_implementation(const addon::Structured_inspection& inspection)
{
    auto parsed = addon::parse_exchange_protocol_state(inspection.protocol_state);
    if (!parsed || !parsed->is_object() || parsed->as_object().size() != 6) {
        return failed();
    }
    Json state = std::move(*parsed);
    for (const auto key : {"pending_base64", "result_base64", "root_thread_id"}) {
        if (!state.find(key) || !state.find(key)->is_string()) {
            return failed();
        }
    }
    for (const auto key : {"turn_started", "turn_completed"}) {
        if (!state.find(key) || !state.find(key)->is_boolean()) {
            return failed();
        }
    }
    const auto* ids = state.find("completed_item_ids");
    if (!ids || !ids->is_array() || ids->as_array().size() > k_max_items) {
        return failed();
    }
    auto pending = addon::decode_base64(state.find("pending_base64")->as_string());
    auto answer  = addon::decode_base64(state.find("result_base64")->as_string());
    std::string thread = text(state, "root_thread_id");
    bool started   = state.find("turn_started")->as_boolean();
    bool completed = state.find("turn_completed")->as_boolean();
    if (!pending || !answer || pending->size() > k_max_bytes || answer->size() > k_max_bytes ||
        (thread.empty() && (started || completed || !answer->empty())) || (completed && !started))
    {
        return failed();
    }
    std::set<std::string> completed_ids;
    for (const auto& id : ids->as_array()) {
        if (!id.is_string() || id.as_string().empty() || !completed_ids.insert(id.as_string()).second) {
            return failed();
        }
    }
    if (inspection.stdout_bytes.size() > k_max_bytes - pending->size()) {
        return failed();
    }
    *pending += inspection.stdout_bytes;
    const auto protocol_failure = [&] {
        if (!thread.empty()) return failed("implementation_failed");
        const auto first = inspection.stderr_bytes.find_first_not_of(" \t\r\n");
        const auto last  = inspection.stderr_bytes.find_last_not_of(" \t\r\n");
        const std::string message = first == std::string::npos ? std::string{} : inspection.stderr_bytes.substr(first, last - first + 1);
        return failed(message == "Not logged in" ? "authentication_required" : "startup_failed");
    };
    addon::Json_parse_limits limits;
    limits.max_bytes        = 4 * k_max_bytes;
    limits.max_string_bytes = 2 * k_max_bytes;
    std::string_view remaining = *pending;
    while (true) {
        const auto end = remaining.find('\n');
        if (end == std::string_view::npos) {
            break;
        }
        const auto line = remaining.substr(0, end);
        remaining.remove_prefix(end + 1);
        if (!nonempty(line)) {
            continue;
        }
        const auto event = addon::parse_json(line, limits, nullptr);
        if (completed || !event || !event->is_object() || text(*event, "type").empty()) {
            return protocol_failure();
        }
        const std::string type = text(*event, "type");
        if (type == "thread.started") {
            if (!thread.empty() || started || text(*event, "thread_id").empty()) {
                return protocol_failure();
            }
            thread = text(*event, "thread_id");
            continue;
        }
        if (thread.empty()) {
            return protocol_failure();
        }
        if (type == "turn.started") {
            if (started) {
                return protocol_failure();
            }
            started = true;
        }
        else
        if (type == "turn.failed" || type == "error") {
            return failed("implementation_failed");
        }
        else
        if (type == "turn.completed") {
            if (!started) {
                return protocol_failure();
            }
            completed = true;
        }
        else
        if (type == "item.started" || type == "item.updated" || type == "item.completed") {
            const auto* item = event->find("item");
            if (!item || !item->is_object() || text(*item, "id").empty() || text(*item, "type").empty()) {
                return protocol_failure();
            }
            // Codex exec reports a warning (configuration, deprecation, a model
            // without metadata, a rerouted model) as a completed "error" item, its
            // non-fatal notice, whenever it arises, so also before the turn starts.
            // Its fatal errors are the top-level "error" event and turn.failed.
            const bool notice = type == "item.completed" && text(*item, "type") == "error";
            if (!started && !notice) {
                return protocol_failure();
            }
            if (type != "item.completed") {
                continue;
            }
            if (completed_ids.size() >= k_max_items || !completed_ids.insert(text(*item, "id")).second) {
                return protocol_failure();
            }
            if (text(*item, "type") == "agent_message") {
                const std::string message = text(*item, "text");
                if (message.empty() || message.size() > k_max_bytes) {
                    return protocol_failure();
                }
                *answer = message;
            }
        }
        else {
            return protocol_failure();
        }
    }
    addon::Structured_inspect_result result;
    if (inspection.final) {
        if (inspection.timed_out || inspection.exit_code != 0 || nonempty(remaining) ||
            thread.empty() || !started || !completed || answer->empty())
        {
            return protocol_failure();
        }
        result.complete     = true;
        result.result_bytes = std::move(*answer);
        return result;
    }
    Json serialized_ids = Json::array();
    for (const auto& id : completed_ids) {
        serialized_ids.push_back(Json(id));
    }
    state.set("pending_base64", Json(addon::encode_base64(remaining)));
    state.set("result_base64", Json(addon::encode_base64(*answer)));
    state.set("root_thread_id", Json(thread));
    state.set("turn_started", Json(started));
    state.set("turn_completed", Json(completed));
    state.set("completed_item_ids", std::move(serialized_ids));
    result.protocol_state = addon::serialize_json(state);
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) {
        return failed();
    }
    return result;
}

} // namespace codex
