#include "codex_context.h"

#include "codex_jsonl_projector.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;

constexpr std::uint64_t k_max_context_integer = 9007199254740991;

struct Native_context_snapshot
{
    addon::Context_update update;
    bool identity_seen = false;
    bool complete = true;
};

std::optional<std::uint64_t> count(const addon::Json_value& value, std::uint64_t minimum = 0)
{
    if (!value.is_integer() || value.as_integer() < 0) {
        return {};
    }
    const auto result = static_cast<std::uint64_t>(value.as_integer());
    return result >= minimum && result <= k_max_context_integer ? std::optional(result) : std::nullopt;
}

void clear_usage(addon::Context_update& update)
{
    update.used_tokens.reset();
    update.context_window.reset();
    update.usage_unavailable = true;
}

bool consume(Native_context_snapshot& snapshot, const addon::Json_value& record)
{
    const auto type = json::text(record, "type");
    const auto& payload = json::member(record, "payload");
    if (!snapshot.identity_seen) {
        if (type != "session_meta" || json::text(payload, "id") != snapshot.update.provider_session_id) {
            return false;
        }
        snapshot.identity_seen = true;
    }
    else
    if (type == "session_meta") {
        snapshot.complete = false;
    }
    if (record.find("ordinal")) {
        snapshot.complete = false;
    }
    if (type == "session_meta") {
        const auto* mode = payload.find("history_mode");
        if (mode && (!mode->is_string() || mode->as_string() != "legacy")) {
            snapshot.complete = false;
        }
        for (const auto* reference : {
                "history_base", "forked_from_id", "forked_from_ordinal_exclusive", "subagent_history_start_ordinal"})
        {
            if (!json::member(payload, reference).is_null()) {
                snapshot.complete = false;
            }
        }
    }
    else
    if (type == "compacted") {
        const auto& replacement = json::member(payload, "replacement_history");
        if (!json::member(payload, "message").is_string() ||
            (!replacement.is_null() && !replacement.is_array()) ||
            *snapshot.update.compaction_count == k_max_context_integer)
        {
            return false;
        }
        ++*snapshot.update.compaction_count;
        // Installation precedes this native marker. A preceding token count belongs to the old history.
        clear_usage(snapshot.update);
    }
    else
    if (type == "event_msg") {
        if (json::text(payload, "type") != "token_count") {
            return true;
        }
        clear_usage(snapshot.update);
        const auto& info = json::member(payload, "info");
        const auto used = count(json::member(json::member(info, "last_token_usage"), "total_tokens"));
        const auto& native_window = json::member(info, "model_context_window");
        const auto window = count(native_window, 1);
        if (!used || (!native_window.is_null() && !window)) {
            return true;
        }
        snapshot.update.used_tokens = used;
        snapshot.update.context_window = window;
        snapshot.update.usage_unavailable = false;
    }
    else
    if (type != "response_item" && type != "turn_context") {
        snapshot.complete = false;
    }
    return true;
}

std::optional<addon::Context_update> read_snapshot(
    const fs::path& requested_path, const std::string& session)
{
    std::error_code error;
    if (!requested_path.is_absolute()) {
        return {};
    }
    const auto path = fs::canonical(requested_path, error);
    if (error || path.extension() != ".jsonl" || !fs::is_regular_file(path, error) || error) {
        return {};
    }
    const auto modified = fs::last_write_time(path, error);
    if (error) {
        return {};
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto end = stream.tellg();
    if (!stream || end <= std::streampos(0)) {
        return {};
    }
    stream.seekg(0);
    const auto bytes = static_cast<std::uint64_t>(end);
    Native_context_snapshot snapshot;
    snapshot.update.provider_session_id = session;
    snapshot.update.compaction_count = 0;
    snapshot.update.usage_unavailable = true;
    Codex_jsonl_projector projector([&](const auto& row) { return consume(snapshot, row); }, Jsonl_projection::CONTEXT);
    addon::Sha256 digest;
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t read = 0;
    bool complete_tail = false;
    // Optional telemetry must not hold a synchronous native hook past its lifecycle delivery budget.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (read < bytes) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return {};
        }
        const auto size = std::min<std::uint64_t>(bytes - read, buffer.size());
        stream.read(buffer.data(), static_cast<std::streamsize>(size));
        if (stream.gcount() != static_cast<std::streamsize>(size) ||
            !projector.feed(std::string_view(buffer.data(), static_cast<std::size_t>(size)), nullptr))
        {
            return {};
        }
        digest.update(buffer.data(), static_cast<std::size_t>(size));
        complete_tail = buffer[static_cast<std::size_t>(size) - 1] == '\n';
        read += size;
    }
    if (!snapshot.identity_seen || fs::file_size(path, error) != bytes || error ||
        fs::last_write_time(path, error) != modified || error ||
        fs::canonical(requested_path, error) != path || error)
    {
        return {};
    }
    if (!complete_tail) {
        clear_usage(snapshot.update);
    }
    snapshot.update.event_id = "codex-context-" + addon::to_hex(digest.finish());
    snapshot.update.compaction_count_complete = snapshot.complete && complete_tail;
    return std::move(snapshot.update);
}

} // namespace

std::optional<addon::Context_update> context_snapshot(const addon::Classify_event_request& request)
{
    if (request.context_failure || request.context_state_version != addon::k_context_state_capability_version) {
        return {};
    }
    const auto& body = request.event.body;
    constexpr std::array<std::string_view, 8> hooks{
        "SessionStart", "UserPromptSubmit", "Stop", "Interrupt", "PermissionRequest", "PostCompact", "PreToolUse", "PostToolUse"};
    if (body.find("agent_id") || json::text(body, "hook_event_name") != request.event.name ||
        std::find(hooks.begin(), hooks.end(), request.event.name) == hooks.end())
    {
        return {};
    }
    const auto& session = json::member(body, "session_id");
    if (!json::bounded_string(session, addon::limits::k_max_lifecycle_identity_bytes) ||
        std::any_of(session.as_string().begin(), session.as_string().end(), [](unsigned char byte) { return byte < 0x20; }))
    {
        return {};
    }
    if (request.event.name != "SessionStart" &&
        (!request.ledger.current_epoch || !request.ledger.current_transition ||
            request.ledger.current_transition->provider_session_id != session.as_string()))
    {
        return {};
    }
    addon::Context_update unavailable;
    unavailable.provider_session_id = session.as_string();
    unavailable.event_id = "codex-context-unavailable-" + addon::provider_event_evidence_digest_hex(request.event);
    unavailable.usage_unavailable = true;
    if (request.context_snapshot_revision) {
        unavailable.compaction_count = 0;
        unavailable.compaction_count_revision = request.context_snapshot_revision;
    }
    const auto& transcript = json::member(body, "transcript_path");
    if (!json::bounded_string(transcript, addon::limits::k_max_start_absolute_path_bytes)) {
        return unavailable;
    }
    try {
        auto snapshot = read_snapshot(addon::path_from_utf8(transcript.as_string()), session.as_string());
        if (snapshot) {
            // This invocation has just read the current source. The digest identifies
            // its bytes; only the host's causal read request orders this observation.
            snapshot->compaction_count_revision = request.context_snapshot_revision;
            if (!request.context_snapshot_revision) {
                snapshot->compaction_count.reset();
                snapshot->compaction_count_complete = false;
            }
        }
        return snapshot ? snapshot : std::optional(std::move(unavailable));
    }
    catch (const std::exception&) {
        return unavailable;
    }
}

} // namespace codex
