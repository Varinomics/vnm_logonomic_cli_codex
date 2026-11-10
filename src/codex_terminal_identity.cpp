#include "codex_terminal_identity.h"

#include "codex_launcher.h"
#include "codex_resume_record.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <system_error>
#include <utility>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::size_t k_native_title_limit = 240;
constexpr std::size_t k_identity_prefix_size = 29;
constexpr std::string_view k_separator = " | ";

bool exact_members(const Json& object, std::initializer_list<std::string_view> names)
{
    return object.is_object() && object.as_object().size() == names.size() &&
        std::all_of(names.begin(), names.end(), [&](auto name) { return object.find(name); });
}

bool stable_title_version(std::string_view version)
{
    const auto suffix = version.find_first_of("-+");
    return (suffix == std::string_view::npos || version[suffix] != '-') &&
        version_is_at_least(version, 0, 167, 0);
}

bool positive_decimal(const Json& value)
{
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.as_string();
    if (text.empty() || text.front() < '1' || text.front() > '9') {
        return false;
    }
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::size_t character_count(std::string_view text)
{
    // The JSON/terminal boundary supplies UTF-8. The native whole-title limit
    // counts Unicode scalar values; individual title parts use graphemes.
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char byte) {
        return (byte & 0xc0) != 0x80;
    }));
}

bool canonical_prefix(std::string_view text)
{
    return text.size() == 32 && text.ends_with("...") &&
        canonical_session_id(std::string(text.substr(0, k_identity_prefix_size)) + "0000000");
}

struct Title_parts
{
    std::string prefix;
    std::string full_id;
    std::string caption;
};

std::optional<std::string> original_caption(std::string_view title, std::size_t suffix_begin)
{
    if (suffix_begin == 0) {
        return std::string{};
    }
    auto caption = title.substr(0, suffix_begin);
    if (caption.ends_with(k_separator)) {
        caption.remove_suffix(k_separator.size());
    }
    else if (caption.ends_with(' ')) {
        // Native activity uses a plain space on either side, including when
        // it is the sole visible original field before the identity suffix.
        caption.remove_suffix(1);
    }
    else {
        return {};
    }
    return std::string(caption);
}

std::optional<Title_parts> parse_title(std::string_view title)
{
    if (title.empty() || character_count(title) >= k_native_title_limit ||
        std::any_of(title.begin(), title.end(), [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }))
    {
        return {};
    }
    std::vector<Title_parts> candidates;
    const auto append = [&](std::size_t prefix_begin, std::string_view full_id) {
        const auto prefix = title.substr(prefix_begin, 32);
        const auto caption = original_caption(title, prefix_begin);
        if (!canonical_prefix(prefix) || !caption) {
            return;
        }
        if (!full_id.empty() && (!canonical_session_id(full_id) ||
            !full_id.starts_with(prefix.substr(0, k_identity_prefix_size))))
        {
            return;
        }
        candidates.push_back({std::string(prefix.substr(0, k_identity_prefix_size)),
            std::string(full_id), *caption});
    };
    // Unnamed: thread-name is omitted and thread falls back to the full UUID.
    constexpr std::size_t unnamed_size = 32 + 3 + 36;
    if (title.size() >= unnamed_size) {
        const auto begin = title.size() - unnamed_size;
        if (title.substr(begin + 32, 3) == k_separator) {
            append(begin, title.substr(title.size() - 36));
        }
    }
    // Named: thread-name and thread render the same native value, truncated
    // upstream to 48 grapheme clusters. A cluster may have many scalars or
    // bytes, including after native title sanitization. Bound the search by
    // the complete title instead of imposing a second, incompatible name cap.
    // Match the whole suffix, including names containing separators or UUIDs.
    // A UUID-shaped human name is duplicated and cannot impersonate the
    // unnamed grammar. More than one valid decomposition remains ambiguous.
    constexpr std::size_t named_frame_size = 32 + 3 + 3;
    const auto max_name_bytes = title.size() > named_frame_size ? (title.size() - named_frame_size) / 2 : 0;
    for (std::size_t bytes = 1; bytes <= max_name_bytes; ++bytes) {
        const auto name = title.substr(title.size() - bytes);
        const auto size = named_frame_size + bytes + bytes;
        const auto begin = title.size() - size;
        if (title.substr(begin + 32, 3) == k_separator &&
            title.substr(begin + 35, bytes) == name &&
            title.substr(begin + 35 + bytes, 3) == k_separator)
        {
            append(begin, {});
        }
    }
    if (candidates.size() != 1) {
        return {};
    }
    return std::move(candidates.front());
}

std::optional<Resume_record> native_store(const Json& context)
{
    if (!exact_members(context, {"version", "native_version", "native_store"}) ||
        *context.find("version") != Json(1) || !context.find("native_version")->is_string() ||
        !stable_title_version(context.find("native_version")->as_string()))
    {
        return {};
    }
    const auto& store = *context.find("native_store");
    if (!exact_members(store, {"version", "codex_home", "sqlite_home"}) ||
        *store.find("version") != Json(1) || !store.find("codex_home")->is_string() ||
        !store.find("sqlite_home")->is_string() ||
        !addon::is_execution_local_path(store.find("codex_home")->as_string()) ||
        !addon::is_execution_local_path(store.find("sqlite_home")->as_string()))
    {
        return {};
    }
    return Resume_record{{}, store.find("codex_home")->as_string(), store.find("sqlite_home")->as_string()};
}

std::optional<std::string> native_title(const Json& event)
{
    if (!exact_members(event, {"terminal_id", "generation", "source", "stream", "stream_index", "timestamp", "payload"}) ||
        !positive_decimal(*event.find("terminal_id")) || !positive_decimal(*event.find("generation")) ||
        *event.find("source") != Json("logonomic_terminal_worker") ||
        *event.find("stream") != Json("terminal") || *event.find("stream_index") != Json(0) ||
        !event.find("timestamp")->is_number())
    {
        return {};
    }
    const auto& payload = *event.find("payload");
    if (!exact_members(payload, {"kind", "title_sequence", "title"}) ||
        *payload.find("kind") != Json("terminal_title_changed") ||
        !positive_decimal(*payload.find("title_sequence")) || !payload.find("title")->is_string())
    {
        return {};
    }
    return payload.find("title")->as_string();
}

std::optional<std::string> resolve_native_prefix(
    const std::string& sqlite_home, const std::string& prefix, const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) {
        return {};
    }
    const auto directory = addon::path_from_utf8(sqlite_home);
    std::error_code error;
    if (!fs::is_directory(directory, error) || error) {
        return {};
    }
    // The prepared native store is authoritative. An obsolete state_5 beside
    // another generation is not a fallback source for current identity.
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (cancelled && cancelled()) {
            return {};
        }
        const auto name = addon::path_to_utf8(entry.path().filename());
        if (name.starts_with("state_") && name.ends_with(".sqlite") && name != "state_5.sqlite") {
            return {};
        }
    }
    sqlite3* raw_database = nullptr;
    const auto path = addon::path_to_utf8(directory / "state_5.sqlite");
    const auto opened = sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw_database, sqlite3_close);
    if (opened != SQLITE_OK || sqlite3_db_readonly(database.get(), "main") != 1) {
        return {};
    }
    sqlite3_busy_timeout(database.get(), 1000);
    auto stop = cancelled;
    sqlite3_progress_handler(database.get(), 1000, [](void* value) {
        const auto& callback = *static_cast<const std::function<bool()>*>(value);
        return callback && callback() ? 1 : 0;
    }, &stop);
    // A binary prefix range uses the native primary-key index and reads at
    // most two candidates. Neither timestamps nor human names choose an ID.
    auto upper = prefix;
    ++upper.back();
    sqlite3_stmt* raw_statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(database.get(),
        "SELECT id FROM threads WHERE id >= ?1 COLLATE BINARY AND id < ?2 COLLATE BINARY ORDER BY id LIMIT 2",
        -1, &raw_statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement, sqlite3_finalize);
    if (prepared != SQLITE_OK ||
        sqlite3_bind_text(statement.get(), 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(statement.get(), 2, upper.data(), static_cast<int>(upper.size()), SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT)
    {
        return {};
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    if (!text) {
        return {};
    }
    const std::string id(text, static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), 0)));
    if (!canonical_session_id(id) || !id.starts_with(prefix) || sqlite3_step(statement.get()) != SQLITE_DONE) {
        return {};
    }
    return id;
}

} // namespace

std::optional<Title_identity_preparation> prepare_title_identity(
    std::string_view native_version, const Json& config, const std::string& codex_home, const std::string& sqlite_home)
{
    if (!stable_title_version(native_version) || !addon::is_execution_local_path(codex_home) ||
        !addon::is_execution_local_path(sqlite_home))
    {
        return {};
    }
    const auto* tui = config.find("tui");
    const auto* selected = tui ? tui->find("terminal_title") : nullptr;
    std::vector<std::string> items;
    if (!selected || selected->is_null()) {
        // config/read leaves native defaults unexpanded. Preserve the native
        // default selection; observed suffix proof still owns exact identity.
        items = {"activity", "thread-name", "project-name"};
    }
    else {
        if (!selected->is_array()) {
            return {};
        }
        for (const auto& item : selected->as_array()) {
            if (!item.is_string()) {
                return {};
            }
            items.push_back(item.as_string());
        }
    }
    bool activity = false;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index] != "activity" && items[index] != "spinner") {
            continue;
        }
        // The host's generic leading-marker normalization can fence identity
        // across spinner frames. Keep other layouts unchanged and hook-based.
        if (index != 0 || activity) {
            return {};
        }
        activity = true;
    }
    if (!activity) {
        items.insert(items.begin(), "spinner");
    }
    items.insert(items.end(), {"session-id", "thread-name", "thread"});
    auto store = Json::object();
    store.set("version", Json(1));
    store.set("codex_home", Json(codex_home));
    store.set("sqlite_home", Json(sqlite_home));
    auto context = Json::object();
    context.set("version", Json(1));
    context.set("native_version", Json(std::string(native_version)));
    context.set("native_store", std::move(store));
    if (addon::serialize_json_canonical(context).size() > addon::k_max_terminal_identity_context_bytes) {
        return {};
    }
    return Title_identity_preparation{std::move(items), {std::move(context)}};
}

std::optional<Native_terminal_identity> observed_terminal_identity(
    const addon::Classify_event_request& request, const std::function<bool()>& cancelled)
{
    if (!request.terminal_identity || !request.terminal_identity->terminal_event || (cancelled && cancelled())) {
        return {};
    }
    const auto store = native_store(request.terminal_identity->preparation.context);
    const auto title = native_title(*request.terminal_identity->terminal_event);
    if (!store || !title) {
        return {};
    }
    const auto parsed = parse_title(*title);
    if (!parsed) {
        return {};
    }
    std::optional<std::string> id;
    if (!parsed->full_id.empty()) {
        id = parsed->full_id;
    }
    else {
        try {
            id = resolve_native_prefix(store->sqlite_home, parsed->prefix, cancelled);
        }
        catch (const fs::filesystem_error&) {
            return {};
        }
    }
    if (!id || (cancelled && cancelled())) {
        return {};
    }
    const auto record = bound_resume_record(*id, store->codex_home, store->sqlite_home);
    if (!record) {
        return {};
    }
    return Native_terminal_identity{{std::move(*id), parsed->caption}, *record};
}

std::optional<addon::Terminal_identity_classification_result> classify_title_identity(
    const addon::Classify_event_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure)
{
    if (request.event.name != "NativeSignal") {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "event.name", addon::Provider_event_problem::MISMATCH,
            nullptr, {}, "native_signal_event"));
    }
    if (!exact_members(request.event.body, {"origin"})) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "body", addon::Provider_event_problem::INVALID_SHAPE,
            &request.event.body, {}, "terminal_identity_envelope"));
    }
    if (*request.event.body.find("origin") != Json("host_terminal")) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "origin", addon::Provider_event_problem::MISMATCH,
            request.event.body.find("origin"), {}, "host_terminal_origin"));
    }
    if (!request.terminal_identity) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "terminal_identity", addon::Provider_event_problem::MISSING,
            nullptr, {}, "terminal_identity_context"));
    }
    addon::Terminal_identity_classification_result result;
    const auto identity = observed_terminal_identity(request, cancelled);
    if (!identity) {
        return result;
    }
    result.terminal_identity = identity->observation;
    if (request.ledger.current_transition &&
        request.ledger.current_transition->provider_session_id == identity->observation.provider_session_id)
    {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = request.event.name;
    observation.provider_session_id = identity->observation.provider_session_id;
    observation.session_transition = addon::Session_transition::STARTED;
    observation.session_source = "terminal_title";
    observation.resume_record = identity->resume_record;
    result.base.classification = addon::Classification::TRANSITION;
    // Selection may return to a loaded thread with no new hook, or select an
    // ID the host has never seen. RESUME admits both without guessing age.
    result.base.transition = addon::Transition_kind::RESUME;
    result.base.observations.push_back(std::move(observation));
    return result;
}

} // namespace codex
