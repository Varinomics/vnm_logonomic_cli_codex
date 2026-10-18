#include "codex_usage.h"
#include "codex_accounts.h"
#include "codex_jsonl_projector.h"
#include "codex_usage_records.h"
#include "codex_native_usage.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_repository_attribution.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::size_t k_line_limit = 4 * 1024 * 1024;
constexpr int k_parser_revision = 1;
// A usage-scan@3 candidate keeps one metadata line per rollout in `rollouts`
// beside the snapshot's sorted `records`, so it holds four files however many
// rollouts a home has. @1 and @2 kept a metadata and a records file per
// rollout, which every scan had to copy, flush and hash again.
constexpr std::string_view k_cache_format = "codex-rollout-cache@3";
constexpr std::string_view k_piece_cache_format = "codex-rollout-cache@4";
constexpr std::chrono::milliseconds k_progress_interval{1000};
std::timed_mutex scan_mutex;

struct Scan_failure { std::string reason; };

void require(bool condition, std::string reason)
{
    if (!condition) throw Scan_failure{std::move(reason)};
}

void check_cancel(const std::function<bool()>& cancelled)
{
    require(!cancelled(), "cancelled");
}

// A retired generation may still be unwinding a scan while its replacement
// starts. The storage lock therefore covers process generations as well as
// concurrent calls in this process; no cache mutation relies on RPC ordering.
class Storage_lock final
{
public:
    Storage_lock(const fs::path& path, const std::function<bool()>& cancelled)
    {
        require(!fs::is_symlink(fs::symlink_status(path)), "usage_storage_link");
#if defined(_WIN32)
        while (m_handle == INVALID_HANDLE_VALUE) {
            check_cancel(cancelled);
            m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle != INVALID_HANDLE_VALUE) break;
            require(GetLastError() == ERROR_SHARING_VIOLATION, "usage_storage_lock_failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#else
        m_descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(m_descriptor >= 0, "usage_storage_lock_failed");
        while (flock(m_descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int failure = errno;
            if (cancelled() || (failure != EWOULDBLOCK && failure != EAGAIN && failure != EINTR)) {
                close(m_descriptor); m_descriptor = -1;
                throw Scan_failure{cancelled() ? "cancelled" : "usage_storage_lock_failed"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
    }
    ~Storage_lock()
    {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#else
        if (m_descriptor >= 0) close(m_descriptor);
#endif
    }
    Storage_lock(const Storage_lock&) = delete;
    Storage_lock& operator=(const Storage_lock&) = delete;
private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
};

std::string path_text(const fs::path& path)
{
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::string digest(std::string_view value)
{
    return addon::to_hex(addon::Sha256::digest(value));
}

std::string cache_key(std::string_view source)
{
    // This is a new package cache identity. Native history IDs are carried
    // through explicit import unchanged; no native Qt-fold hash is recreated.
    return digest(profile_account_key(source));
}

std::string nonce()
{
    static std::atomic<std::uint64_t> serial{0};
    std::random_device random;
    return digest(std::to_string(random()) + ":" + std::to_string(random()) + ":" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" + std::to_string(serial++));
}

bool digest_name(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

bool hidden_entry(const fs::directory_entry& entry)
{
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(entry.path().c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES, "usage_source_unavailable");
    return (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
    return entry.path().filename().string().starts_with('.');
#endif
}

void visit_rollouts(const fs::path& profile, const std::function<bool()>& cancelled,
    const std::function<void(const fs::path&)>& consume)
{
    require(fs::is_directory(profile), "usage_sessions_unavailable");
    for (const auto* name : {"sessions", "archived_sessions"}) {
        check_cancel(cancelled);
        const auto root = profile / name;
        if (!fs::exists(root)) {
            continue;
        }
        require(fs::is_directory(root), "usage_sessions_not_directory");
        for (fs::recursive_directory_iterator entries(root); entries != fs::recursive_directory_iterator{}; ++entries) {
            check_cancel(cancelled);
            const auto& entry = *entries;
            if (hidden_entry(entry)) {
                entries.disable_recursion_pending();
                continue;
            }
            const auto filename = addon::ascii_lowercase(entry.path().filename().string());
            if (entry.is_regular_file() && filename.starts_with("rollout-") && filename.ends_with(".jsonl")) {
                consume(entry.path());
            }
        }
    }
}

fs::path directory(const fs::path& parent, const std::string& name)
{
    const auto path = parent / name;
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    require(!fs::is_symlink(status), "usage_storage_link");
    if (!fs::exists(status)) {
        error.clear();
        fs::create_directory(path, error);
        require(!error, "usage_storage_create_failed");
    }
    require(fs::is_directory(fs::symlink_status(path, error)) && !error, "usage_storage_invalid");
    return path;
}

void remove_snapshot(const fs::path& path, const std::function<bool()>& cancelled)
{
    require(digest_name(path.filename().string()), "usage_snapshot_invalid");
    require(fs::is_directory(fs::symlink_status(path)), "usage_storage_invalid");
    // All entries written here are files. Refuse foreign directories instead
    // of following or recursively removing anything in an unexpected shape.
    for (const auto& entry : fs::directory_iterator(path)) {
        check_cancel(cancelled);
        require(!fs::is_directory(entry.symlink_status()), "usage_storage_invalid");
        fs::remove(entry.path());
    }
    fs::remove(path);
}

class Pending_snapshot final
{
public:
    explicit Pending_snapshot(fs::path path) : m_path(std::move(path)) {}
    ~Pending_snapshot()
    {
        if (!m_committed) {
            // Cancellation must not stop this cleanup.
            try { remove_snapshot(m_path, [] { return false; }); }
            catch (...) { /* The next scan retries owned scratch cleanup. */ }
        }
    }
    void commit() { m_committed = true; }
private:
    fs::path m_path;
    bool m_committed = false;
};

void prune_snapshots(const fs::path& snapshots, std::string_view current, const std::function<bool()>& cancelled)
{
    for (const auto& entry : fs::directory_iterator(snapshots)) {
        check_cancel(cancelled);
        const auto name = entry.path().filename().string();
        if (digest_name(name) && name != current) {
            remove_snapshot(entry.path(), cancelled);
        }
    }
}

bool read_line(std::istream& input, std::string* line)
{
    line->clear();
    char character = 0;
    while (input.get(character)) {
        if (character == '\n') return true;
        require(line->size() < k_line_limit, "usage_cache_record_too_large");
        line->push_back(character);
    }
    require(!input.bad(), "usage_storage_read_failed");
    require(line->empty(), "usage_cache_record_incomplete");
    return false;
}

void write_line(std::ostream& output, std::string_view line)
{
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    require(static_cast<bool>(output), "usage_storage_write_failed");
}

void sync_path(const fs::path& path, bool is_directory)
{
#if defined(_WIN32)
    // Windows publishes the pointer with a write-through move. File contents
    // must already be on stable storage when that move makes them reachable.
    if (is_directory) {
        return;
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "usage_storage_sync_failed");
    const bool synced = FlushFileBuffers(handle) != 0;
    const bool closed = CloseHandle(handle) != 0;
#else
    const int descriptor = open(path.c_str(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (is_directory ? O_DIRECTORY : 0));
    require(descriptor >= 0, "usage_storage_sync_failed");
    const bool synced = fsync(descriptor) == 0;
    const bool closed = close(descriptor) == 0;
#endif
    require(synced && closed, "usage_storage_sync_failed");
}

void replace_file(const fs::path& source, const fs::path& destination)
{
    std::error_code error;
    require(!fs::is_symlink(fs::symlink_status(destination, error)), "usage_storage_link");
#if defined(_WIN32)
    require(MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
        "usage_storage_replace_failed");
#else
    error.clear();
    fs::rename(source, destination, error);
    require(!error, "usage_storage_replace_failed");
#endif
}

void write_json(const fs::path& path, const addon::Json_value& value, Pending_snapshot* generation = nullptr)
{
    const auto encoded = addon::serialize_json(value);
    require(encoded.size() <= k_line_limit, "usage_cache_record_too_large");
    const auto temporary = path.parent_path() / (path.filename().string() + ".pending");
    require(!fs::is_symlink(fs::symlink_status(temporary)), "usage_storage_link");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    write_line(output, encoded);
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    sync_path(temporary, false);
    if (generation) {
        generation->commit();
    }
    replace_file(temporary, path);
    sync_path(path.parent_path(), true);
}

std::optional<addon::Json_value> read_json(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (!fs::exists(status)) return std::nullopt;
    require(fs::is_regular_file(status) && !fs::is_symlink(status), "usage_storage_invalid");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    std::string line;
    require(read_line(input, &line), "usage_cache_invalid");
    auto value = addon::parse_json(line, {}, nullptr);
    require(value.has_value() && value->is_object(), "usage_cache_invalid");
    require(!read_line(input, &line), "usage_cache_invalid");
    return value;
}

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value missing;
    const auto* value = object.find(name);
    return value ? *value : missing;
}

std::string string_member(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    require(value.is_string(), "usage_cache_invalid");
    return value.as_string();
}

// Persist through the public codec so scanner caches cannot acquire a second
// interpretation of category coverage or repository allocation records.
addon::Json_value record_accounting(const Rollout_usage_record& record, std::uint16_t version)
{
    auto envelope = addon::Json_value::object();
    auto usage = record.usage;
    if (version == 3) {
        usage.aggregate_evidence = "partial";
    }
    envelope.set("usage", addon::encode_usage_record(usage, version));
    envelope.set("recorded_cwd", addon::Json_value(record.recorded_cwd));
    return envelope;
}

std::string encode_record(const Rollout_usage_record& record, std::uint16_t version = 2,
    bool include_temporal = true, bool include_turn_context = false)
{
    auto envelope = record_accounting(record, version);
    // This flag travels only through a fresh parse's temporary spool. Accepted
    // caches never supply a witness for a later correction.
    if (version == 3 && include_turn_context && record.turn_cwd_witnessed) {
        envelope.set("turn_cwd_witnessed", addon::Json_value(true));
    }
    if (version == 3 && include_temporal && (record.temporal.instant || record.temporal.opaque ||
        record.temporal.reason != "missing-evidence"))
    {
        envelope.set("temporal_observation", encode_temporal_observation(record.temporal,
            record_accounting(record, version)));
        const auto encoded = addon::serialize_json(envelope);
        if (encoded.size() <= k_line_limit) return encoded;
        envelope.erase("temporal_observation");
    }
    return addon::serialize_json(envelope);
}

Rollout_usage_record decode_record(std::string_view line, std::uint16_t version = 2)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object(), "usage_cache_invalid");
    addon::Contract_failure failure;
    auto record = addon::decode_usage_record(member(*value, "usage"), &failure, version);
    require(record.has_value(), "usage_cache_invalid");
    Rollout_usage_record result{std::move(*record), string_member(*value, "recorded_cwd")};
    if (version == 3) {
        result.turn_cwd_witnessed = member(*value, "turn_cwd_witnessed") == addon::Json_value(true);
        result.temporal = decode_temporal_observation(value->find("temporal_observation"),
            record_accounting(result, version));
        const auto& proof = result.usage.cross_source_event;
        if (result.temporal.instant && (!proof ||
            proof->authority_namespace != "openai.codex.completed-turn" ||
            result.usage.record_id != "codex:" + proof->event_id))
        {
            result.temporal.instant.reset();
            result.temporal.reason = "unsupported";
            result.temporal.opaque = *value->find("temporal_observation");
        }
    }
    return result;
}

struct Fingerprint
{
    std::uintmax_t size = 0;
    fs::file_time_type modified;
    bool operator==(const Fingerprint&) const = default;
};

std::optional<Fingerprint> fingerprint(const fs::path& path)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) return std::nullopt;
    const auto modified = fs::last_write_time(path, error);
    if (error) return std::nullopt;
    return Fingerprint{size, modified};
}

bool fingerprint_matches(const addon::Json_value& metadata, const Fingerprint& value)
{
    if (member(metadata, "size") != addon::Json_value(std::to_string(value.size))) return false;
    if (member(metadata, "modified") == addon::Json_value(addon::file_time_ticks_text(value.modified))) return true;
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto modified = std::chrono::clock_cast<std::chrono::system_clock>(value.modified);
#else
    const auto modified = fs::file_time_type::clock::to_sys(value.modified);
#endif
    const auto native_msecs = std::chrono::duration_cast<std::chrono::milliseconds>(
        modified.time_since_epoch()).count();
    return member(metadata, "modified_native_msecs") == addon::Json_value(std::to_string(native_msecs));
}

// A line to sort and the key it sorts by.
struct Sort_record
{
    std::string key;
    std::string line;
};

// Recovers the sort key of a line read back from a sorted run.
using Sort_key = std::function<std::string(const std::string& line)>;

struct Rollout_refresh
{
    bool                             incomplete = false;
    // Present when the rollout was parsed again; the records in its spool
    // then replace the cached ones.
    std::optional<addon::Json_value> metadata;
};

// Parses a rollout unless its cached metadata still describes it, writing each
// record to `spool` as it is read, so a rollout of any size holds no records in
// memory. The spool is left in place only when the result carries new metadata.
// A rollout that cannot be read at rest keeps its cached evidence and marks the
// history incomplete.
Rollout_refresh refresh_rollout(const fs::path& path, const std::string& source,
    const std::optional<addon::Json_value>& metadata, bool force, const std::function<bool()>& cancelled,
    std::uint16_t version, const fs::path& spool, bool refresh_missing_temporal = true)
{
    check_cancel(cancelled);
    Rollout_refresh result;
    const auto before = fingerprint(path);
    // A writer may have created or truncated the file before populating it.
    // Preserve prior evidence and do not manufacture an empty cache entry.
    if (!before || before->size == 0) {
        result.incomplete = true;
        return result;
    }
    // Accounting freshness does not show whether optional temporal evidence
    // was examined. A present marker, including an unknown version, stays intact.
    const auto* temporal_parser = metadata ? metadata->find("temporal_parser") : nullptr;
    const bool current = metadata && member(*metadata, "revision") == addon::Json_value(k_parser_revision);
    // Cache adoption retains absent optional evidence. A changed source or
    // explicit full scan can supply it without making migration a recount.
    const bool refresh_temporal = version == 3 && !temporal_parser && refresh_missing_temporal;
    if (!force && current && !refresh_temporal && fingerprint_matches(*metadata, *before)) {
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.incomplete = true;
        return result;
    }
    std::ofstream output(spool, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    std::string error;
    std::optional<std::string> session;
    const bool parsed = read_rollout_usage(input, addon::path_from_utf8(source), [&](const Rollout_usage_record& record) {
        session = record.usage.session_key;
        write_line(output, encode_record(record, version, true, true));
        return true;
    }, cancelled, &error);
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    const auto after = fingerprint(path);
    if (!after || *after != *before || error == "rollout_read_failed" ||
        (!parsed && metadata && (!current || (!force && refresh_temporal)) &&
            fingerprint_matches(*metadata, *before) && error != "cancelled"))
    {
        fs::remove(spool);
        result.incomplete = true;
        return result;
    }
    require(parsed, error.empty() ? "rollout_invalid" : error);
    check_cancel(cancelled);
    auto next = addon::Json_value::object();
    next.set("revision", addon::Json_value(k_parser_revision));
    next.set("source", addon::Json_value(source));
    next.set("size", addon::Json_value(std::to_string(before->size)));
    next.set("modified", addon::Json_value(addon::file_time_ticks_text(before->modified)));
    if (version == 3) {
        next.set("native_session", session ? addon::Json_value(*session) : addon::Json_value{});
        auto marker = addon::Json_value::object();
        marker.set("version", addon::Json_value(1));
        next.set("temporal_parser", temporal_parser ? *temporal_parser : std::move(marker));
    }
    result.metadata = std::move(next);
    return result;
}

void write_rollout_records(const fs::path& path, const std::vector<Sort_record>& records)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    for (const auto& record : records) {
        write_line(output, record.line);
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
}

// The usage-scan@1/2 layout keeps a metadata and a records file per rollout.
void refresh_file(const fs::path& path, const fs::path& work,
    bool force, bool* incomplete, const std::function<bool()>& cancelled, std::uint16_t version = 2)
{
    const auto source = path_text(fs::absolute(path).lexically_normal());
    const auto key = cache_key(source);
    const auto metadata_path = work / (key + ".meta");
    const auto pending = work / (key + ".pending-records");
    auto metadata = read_json(metadata_path);
    auto refreshed = refresh_rollout(path, source, metadata, force, cancelled, version, pending);
    if (refreshed.incomplete) *incomplete = true;
    if (refreshed.metadata) {
        replace_file(pending, work / (key + ".records"));
        metadata = std::move(refreshed.metadata);
    }
    if (metadata && !refreshed.incomplete) {
        // A present unchanged copy participates in duplicate checks too;
        // rows retained from a former archive location are historical evidence.
        metadata->set("live_snapshot", addon::Json_value(work.filename().string()));
        write_json(metadata_path, *metadata);
    }
}

addon::Json_value account_value(const std::optional<addon::Account_context>& account)
{
    return account ? addon::Json_value(account->adapter_account_key) : addon::Json_value{};
}

bool cache_file_name(std::string_view name)
{
    const auto dot = name.find('.');
    return dot == 64 && digest_name(name.substr(0, dot)) &&
        (name.substr(dot) == ".meta" || name.substr(dot) == ".records");
}

std::string hash_file(const fs::path& path, const std::function<bool()>& cancelled)
{
    const auto status = fs::symlink_status(path);
    require(fs::is_regular_file(status) && !fs::is_symlink(status), "required_usage_cache_unavailable");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "required_usage_cache_unavailable");
    addon::Sha256 hash;
    std::array<char, 65536> bytes{};
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        hash.update(bytes.data(), (std::size_t)input.gcount());
    }
    require(!input.bad(), "required_usage_cache_unavailable");
    return addon::to_hex(hash.finish());
}

void copy_accepted_cache(const addon::Json_value& accepted, const fs::path& snapshots,
    const fs::path& work, const std::function<bool()>& cancelled)
{
    const auto identifier = string_member(accepted, "snapshot");
    require(digest_name(identifier), "required_usage_cache_unavailable");
    const auto source_root = snapshots / identifier;
    // The generation is read in place, so it must be a directory of the
    // package's storage rather than a link that leads elsewhere.
    require(fs::is_directory(fs::symlink_status(source_root)), "required_usage_cache_unavailable");
    require(hash_file(source_root / "inventory", cancelled) == string_member(accepted, "inventory_sha256"),
        "required_usage_cache_unavailable");
    std::ifstream inventory(source_root / "inventory", std::ios::binary);
    require(static_cast<bool>(inventory), "required_usage_cache_unavailable");
    std::string line;
    bool have_snapshot = false;
    bool have_records  = false;
    while (read_line(inventory, &line)) {
        check_cancel(cancelled);
        const auto entry = addon::parse_json(line, {}, nullptr);
        require(entry && entry->is_object(), "required_usage_cache_unavailable");
        const auto name = string_member(*entry, "name");
        require(cache_file_name(name) || name == "snapshot" || name == "records",
            "required_usage_cache_unavailable");
        const auto source = source_root / name;
        require(hash_file(source, cancelled) == string_member(*entry, "sha256"),
            "required_usage_cache_unavailable");
        if (name == "snapshot") { have_snapshot = true; continue; }
        if (name == "records")  { have_records  = true; continue; }
        require(!fs::exists(fs::symlink_status(work / name)), "required_usage_cache_unavailable");
        std::ifstream input(source, std::ios::binary);
        std::ofstream output(work / name, std::ios::binary | std::ios::trunc);
        require(input && output, "usage_storage_open_failed");
        std::string record;
        while (read_line(input, &record)) {
            check_cancel(cancelled);
            write_line(output, record);
        }
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
    }
    require(have_snapshot && have_records, "required_usage_cache_unavailable");
    for (const auto& entry : fs::directory_iterator(work)) {
        const auto key = entry.path().filename().string().substr(0, 64);
        require(fs::is_regular_file(fs::symlink_status(work / (key + ".meta"))) &&
            fs::is_regular_file(fs::symlink_status(work / (key + ".records"))),
            "required_usage_cache_unavailable");
    }
}

addon::Json_value accepted_generation(const fs::path& work, const addon::Json_value& seeds,
    const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled)
{
    std::ofstream inventory(work / "inventory", std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(inventory), "usage_storage_write_failed");
    for (const auto& entry : fs::directory_iterator(work)) {
        check_cancel(cancelled);
        const auto name = entry.path().filename().string();
        if (cache_file_name(name) || name == "snapshot" || name == "records") {
            auto file = addon::Json_value::object();
            file.set("name", addon::Json_value(name));
            file.set("sha256", addon::Json_value(hash_file(entry.path(), cancelled)));
            write_line(inventory, addon::serialize_json(file));
            sync_path(entry.path(), false);
        }
    }
    inventory.close();
    require(static_cast<bool>(inventory), "usage_storage_write_failed");
    sync_path(work / "inventory", false);
    sync_path(work, true);
    sync_path(work.parent_path(), true);
    auto accepted = addon::Json_value::object();
    accepted.set("snapshot", addon::Json_value(work.filename().string()));
    accepted.set("account", account_value(account));
    accepted.set("native_seeds", seeds);
    accepted.set("inventory_sha256", addon::Json_value(hash_file(work / "inventory", cancelled)));
    return accepted;
}

// Converts a host-staged native cache once per seed digest. `adopt` stores a
// converted rollout in the caller's cache layout, whose metadata names the
// seed; a layout refuses a rollout one seed names twice with
// native_usage_cache_duplicate_path.
void seed_native_cache(const addon::Usage_scan_request& request, const fs::path& storage,
    bool have_accepted, addon::Json_value* seeds,
    const std::function<void(const std::string& key, addon::Json_value metadata, std::vector<Sort_record> records)>& adopt,
    const std::function<bool()>& cancelled, std::uint16_t version = 2)
{
    if (!request.native_file_cache) return;
    const auto& descriptor = *request.native_file_cache;
    require(request.context.account_context && descriptor.source_account_key == request.context.account_context->adapter_account_key,
        "native_usage_cache_account_mismatch");
    require(digest_name(descriptor.sha256_hex), "native_usage_cache_invalid");
    const auto path = addon::path_from_utf8(descriptor.path);
    require(path.is_absolute() && profile_account_key(path_text(path.parent_path())) == profile_account_key(path_text(storage)) &&
        fs::is_regular_file(fs::symlink_status(path)), "native_usage_cache_path_invalid");
    for (const auto& seed : seeds->as_array()) {
        require(seed.is_string() && digest_name(seed.as_string()), "required_usage_cache_unavailable");
        if (seed.as_string() == descriptor.sha256_hex) {
            return;
        }
    }
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "native_usage_cache_unavailable");
    addon::Sha256 hash;
    std::array<char, 65536> bytes{};
    std::uint64_t length = 0;
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        const auto size = static_cast<std::size_t>(input.gcount());
        hash.update(bytes.data(), size);
        length += size;
    }
    require(!input.bad() && addon::to_hex(hash.finish()) == descriptor.sha256_hex, "native_usage_cache_digest_mismatch");
    if (length == 0) {
        require(!descriptor.cache_required || have_accepted, "required_usage_cache_unavailable");
        seeds->push_back(addon::Json_value(descriptor.sha256_hex));
        return; // An empty host cache never clears durable package evidence.
    }
    input.clear(); input.seekg(0);
    addon::Sha256 checked_hash;
    std::string line;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        checked_hash.update(line); checked_hash.update("\n");
        const auto wrapper = addon::parse_json(line, {}, nullptr);
        require(wrapper && wrapper->is_object(), "native_usage_cache_invalid");
        const auto key = cache_key(string_member(*wrapper, "path"));
        std::vector<Sort_record> records;
        std::string error;
        const auto file = convert_native_cache_file(*wrapper, [&](const Rollout_usage_record& record) {
            check_cancel(cancelled);
            records.push_back({record.usage.record_id, encode_record(record, version)});
            return true;
        }, &error, version);
        require(file.has_value(), error);
        auto metadata = addon::Json_value::object();
        metadata.set("native_seed", addon::Json_value(descriptor.sha256_hex));
        metadata.set("revision", addon::Json_value(file->parser_current ? k_parser_revision : 0));
        metadata.set("source", addon::Json_value(file->path));
        metadata.set("size", addon::Json_value(std::to_string(file->size)));
        metadata.set("modified_native_msecs", addon::Json_value(std::to_string(file->modified_msecs)));
        adopt(key, std::move(metadata), std::move(records));
    }
    require(addon::to_hex(checked_hash.finish()) == descriptor.sha256_hex, "native_usage_cache_digest_mismatch");
    check_cancel(cancelled);
    seeds->push_back(addon::Json_value(descriptor.sha256_hex));
}

bool id_less(std::string_view left, std::string_view right)
{
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](unsigned char a, unsigned char b) { return a < b; });
}

// Consecutive turns in one directory resolve its repository once. The memo
// holds one directory however many a history has.
class Repository_memo final
{
public:
    void refresh(Rollout_usage_record* record)
    {
        if (!m_resolved || m_cwd != record->recorded_cwd) {
            m_cwd      = record->recorded_cwd;
            m_root     = repository_for_cwd(record->recorded_cwd);
            m_resolved = true;
        }
        refresh_repository(record, m_root);
    }

private:
    std::string                m_cwd;
    std::optional<std::string> m_root;
    bool                       m_resolved = false;
};

fs::path run_path(const fs::path& work, std::string_view name, std::uint64_t level, std::uint64_t index)
{
    return work / (std::string(name) + "-" + std::to_string(level) + "-" + std::to_string(index));
}

void merge_runs(const fs::path& left_path, const fs::path& right_path, const fs::path& destination,
    const std::function<bool()>& cancelled, const Sort_key& key_of, bool unique_ids)
{
    std::ifstream left(left_path, std::ios::binary), right(right_path, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    require(left && right && output, "usage_storage_open_failed");
    Sort_record a, b;
    const auto next = [&key_of](std::istream& input, Sort_record* record) {
        if (!read_line(input, &record->line)) return false;
        record->key = key_of(record->line);
        return true;
    };
    bool have_a = next(left, &a), have_b = next(right, &b);
    while (have_a || have_b) {
        check_cancel(cancelled);
        require(!unique_ids || !have_a || !have_b || a.key != b.key, "duplicate_usage_record");
        if (have_a && (!have_b || id_less(a.key, b.key))) { write_line(output, a.line); have_a = next(left, &a); }
        else { write_line(output, b.line); have_b = next(right, &b); }
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    left.close(); right.close();
    fs::remove(left_path); fs::remove(right_path);
}

// Sorts lines by key while holding at most one run of them in memory: each
// full run is written sorted, and runs merge pairwise. Lines sharing a key
// are refused as duplicate records when `unique_ids` is set and kept adjacent
// otherwise.
class Record_sorter final
{
public:
    Record_sorter(fs::path work, std::string name, std::uint64_t run_bytes,
        const std::function<bool()>& cancelled, Sort_key key_of, bool unique_ids)
    :
        m_work(std::move(work)),
        m_name(std::move(name)),
        m_run_bytes(run_bytes),
        m_cancelled(cancelled),
        m_key_of(std::move(key_of)),
        m_unique_ids(unique_ids)
    {}

    void add(Sort_record record)
    {
        if (!m_chunk.empty() && m_bytes + record.line.size() > m_run_bytes) flush();
        m_bytes += record.line.size();
        m_chunk.push_back(std::move(record));
    }

    // Moves every added line, sorted by key, to `destination`.
    void finish(const fs::path& destination)
    {
        flush();
        std::uint64_t level = 0;
        while (m_runs > 1) {
            const auto next_runs = (m_runs + 1) / 2;
            for (std::uint64_t index = 0; index < next_runs; ++index) {
                check_cancel(m_cancelled);
                const auto merged = run_path(m_work, m_name, level + 1, index);
                if (index * 2 + 1 == m_runs) fs::rename(run_path(m_work, m_name, level, index * 2), merged);
                else merge_runs(run_path(m_work, m_name, level, index * 2), run_path(m_work, m_name, level, index * 2 + 1),
                    merged, m_cancelled, m_key_of, m_unique_ids);
            }
            ++level; m_runs = next_runs;
        }
        if (m_runs == 1) fs::rename(run_path(m_work, m_name, level, 0), destination);
        else { std::ofstream empty(destination, std::ios::binary); require(static_cast<bool>(empty), "usage_storage_write_failed"); }
    }

private:
    void flush()
    {
        if (m_chunk.empty()) return;
        std::sort(m_chunk.begin(), m_chunk.end(), [](const auto& a, const auto& b) { return id_less(a.key, b.key); });
        std::ofstream output(run_path(m_work, m_name, 0, m_runs++), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_write_failed");
        std::string previous;
        for (const auto& record : m_chunk) {
            require(!m_unique_ids || previous.empty() || previous != record.key, "duplicate_usage_record");
            write_line(output, record.line);
            previous = record.key;
        }
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
        m_chunk.clear();
        m_bytes = 0;
    }

    fs::path                        m_work;
    std::string                     m_name;
    std::uint64_t                   m_run_bytes;
    const std::function<bool()>&    m_cancelled;
    Sort_key                        m_key_of;
    bool                            m_unique_ids;
    std::vector<Sort_record>        m_chunk;
    std::size_t                     m_bytes = 0;
    std::uint64_t                   m_runs  = 0;
};

// The sort key of an encoded usage record: its record ID.
Sort_key record_id_key(std::uint16_t version)
{
    return [version](const std::string& line) { return decode_record(line, version).usage.record_id; };
}

// A rollout spool line holds a rollout's cache key, a tab, a 20-digit ordinal,
// a tab and a JSON object. Sorting by the first two fields orders rollouts by
// key and keeps those of one key in the order they were spooled.
constexpr std::size_t k_spool_key_size = 64 + 1 + 20;

Sort_record spool_record(const std::string& key, std::uint64_t ordinal, const addon::Json_value& payload)
{
    const auto number = std::to_string(ordinal);
    auto line = key + '\t' + std::string(20 - number.size(), '0') + number + '\t' + addon::serialize_json(payload);
    return {line.substr(0, k_spool_key_size), std::move(line)};
}

Sort_key spool_key()
{
    return [](const std::string& line) { return line.substr(0, k_spool_key_size); };
}

// A sorted rollout spool read one line at a time.
class Spool_stream final
{
public:
    explicit Spool_stream(const fs::path& path)
    :
        m_input(path, std::ios::binary)
    {
        require(static_cast<bool>(m_input), "usage_storage_read_failed");
        advance();
    }

    // The cache key of the next line.
    const std::optional<std::string>& key() const { return m_key; }

    addon::Json_value take()
    {
        auto payload = addon::parse_json(std::string_view(m_line).substr(k_spool_key_size + 1), {}, nullptr);
        require(payload && payload->is_object(), "usage_storage_read_failed");
        advance();
        return std::move(*payload);
    }

private:
    void advance()
    {
        if (read_line(m_input, &m_line)) {
            require(m_line.size() > k_spool_key_size, "usage_storage_read_failed");
            m_key = m_line.substr(0, 64);
        }
        else {
            m_key.reset();
        }
    }

    std::ifstream              m_input;
    std::string                m_line;
    std::optional<std::string> m_key;
};

bool same_observation(Rollout_usage_record left, Rollout_usage_record right);
void defer_repository_growth(const Rollout_usage_record& accepted, Rollout_usage_record& observed);
void reduce_legacy_repository(const Rollout_usage_record& accepted, Rollout_usage_record& observed);

enum class Snapshot_evidence : char
{
    RETAINED = '0',
    SEEDED   = '1',
    LIVE     = '2'
};

// Reconcile native IDs across per-path files without treating a retained row
// from before an archive move as a second current observation.
void write_unique_snapshot_records(const fs::path& sorted, const std::optional<fs::path>& accepted,
    const fs::path& destination, const std::function<bool()>& cancelled, std::uint16_t version)
{
    std::ifstream input(sorted, std::ios::binary);
    std::ifstream previous;
    if (accepted) {
        previous.open(*accepted, std::ios::binary);
        require(static_cast<bool>(previous), "required_usage_cache_unavailable");
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    require(input && output, "usage_storage_open_failed");
    std::optional<Rollout_usage_record> retained;
    const auto advance = [&previous, &retained, version]() {
        std::string line;
        retained = previous.is_open() && read_line(previous, &line)
            ? std::optional(decode_record(line, version)) : std::nullopt;
    };
    advance();
    Repository_memo repositories;
    const auto publish = [
            &retained, &advance, &repositories, &output, &cancelled, version
        ](Rollout_usage_record record, Snapshot_evidence evidence, bool conflict)
        {
            require(!conflict, "duplicate_usage_record");
            while (retained && id_less(retained->usage.record_id, record.usage.record_id)) {
                check_cancel(cancelled);
                advance();
            }
            if (retained && retained->usage.record_id == record.usage.record_id) {
                if (evidence == Snapshot_evidence::RETAINED) {
                    const bool deferred_attribution = record.recorded_cwd != retained->recorded_cwd;
                    record = *retained;
                    if (!deferred_attribution) repositories.refresh(&record);
                }
                else
                if (evidence == Snapshot_evidence::LIVE && record.recorded_cwd != retained->recorded_cwd) {
                    if (record.usage.total_tokens == retained->usage.total_tokens) {
                        record.usage.repository_path = retained->usage.repository_path;
                        record.usage.repository_usage = retained->usage.repository_usage;
                        record.recorded_cwd = retained->recorded_cwd;
                    }
                    else
                    if (record.usage.total_tokens < retained->usage.total_tokens) {
                        reduce_legacy_repository(*retained, record);
                    }
                    else defer_repository_growth(*retained, record);
                }
                record.usage.source_path = retained->usage.source_path;
            }
            write_line(output, encode_record(record, version));
        };
    std::optional<Rollout_usage_record> chosen;
    auto chosen_evidence = Snapshot_evidence::RETAINED;
    bool conflict = false;
    std::string line;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        require(line.size() > 2 && line[1] == '\t' && line[0] >= '0' && line[0] <= '2',
            "usage_storage_read_failed");
        const auto evidence = (Snapshot_evidence)line[0];
        auto record = decode_record(std::string_view(line).substr(2), version);
        if (chosen && chosen->usage.record_id == record.usage.record_id) {
            if (evidence > chosen_evidence) {
                chosen = std::move(record);
                chosen_evidence = evidence;
                conflict = false;
            }
            else
            if (evidence == chosen_evidence) {
                if (evidence != Snapshot_evidence::RETAINED && !same_observation(*chosen, record)) {
                    conflict = true;
                }
                if (record.usage.source_path.value_or("") < chosen->usage.source_path.value_or("")) {
                    chosen = std::move(record);
                }
            }
            continue;
        }
        if (chosen) {
            publish(std::move(*chosen), chosen_evidence, conflict);
        }
        chosen = std::move(record);
        chosen_evidence = evidence;
        conflict = false;
    }
    if (chosen) {
        publish(std::move(*chosen), chosen_evidence, conflict);
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
}

// The usage-scan@1/2 layout: sorts the records of every per-rollout file.
void sorted_snapshot(const fs::path& work, std::uint64_t sort_run_bytes, const std::function<bool()>& cancelled,
    const std::optional<fs::path>& accepted, std::uint16_t version = 2)
{
    const auto key_of = [version](const std::string& line) {
        require(line.size() > 2 && line[1] == '\t', "usage_storage_read_failed");
        return decode_record(std::string_view(line).substr(2), version).usage.record_id;
    };
    Record_sorter sorter(work, "run", sort_run_bytes, cancelled, key_of, false);
    Repository_memo repositories;
    const addon::Json_value identifier(work.filename().string());
    for (const auto& entry : fs::directory_iterator(work)) {
        check_cancel(cancelled);
        const auto filename = entry.path().filename().string();
        if (!filename.ends_with(".meta") || !digest_name(std::string_view(filename).substr(0, filename.size() - 5))) continue;
        const auto metadata = read_json(entry.path());
        require(metadata.has_value(), "usage_cache_invalid");
        const auto source = string_member(*metadata, "source");
        const auto evidence = member(*metadata, "live_snapshot") == identifier ? Snapshot_evidence::LIVE :
            member(*metadata, "seed_snapshot") == identifier ? Snapshot_evidence::SEEDED : Snapshot_evidence::RETAINED;
        require(cache_key(source) == filename.substr(0, 64), "usage_cache_identity_mismatch");
        const auto records = work / (filename.substr(0, 64) + ".records");
        require(fs::is_regular_file(fs::symlink_status(records)), "usage_cache_invalid");
        std::ifstream input(records, std::ios::binary);
        require(static_cast<bool>(input), "usage_storage_read_failed");
        std::string line;
        while (read_line(input, &line)) {
            check_cancel(cancelled);
            auto record = decode_record(line, version);
            require(record.usage.source_path && cache_key(*record.usage.source_path) == cache_key(source), "usage_cache_identity_mismatch");
            repositories.refresh(&record);
            sorter.add({record.usage.record_id,
                std::string(1, (char)evidence) + '\t' + encode_record(record, version)});
        }
    }
    const auto sorted = work / "sorted-records";
    sorter.finish(sorted);
    write_unique_snapshot_records(sorted, accepted, work / "records", cancelled, version);
    fs::remove(sorted);
}

std::uint64_t decimal(std::string_view text)
{
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        std::to_string(value) == text, "usage_cursor_invalid");
    return value;
}

addon::Usage_scan_page page_at(const fs::path& snapshot, const std::string& identifier,
    std::uint64_t offset, const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled,
    std::uint16_t version = 2, const addon::Usage_scan_request* request = nullptr, bool incremental = false)
{
    require(fs::is_directory(fs::symlink_status(snapshot)), "usage_snapshot_expired");
    const auto metadata = read_json(snapshot / "snapshot");
    require(metadata.has_value(), "usage_snapshot_expired");
    addon::Contract_failure failure;
    const auto encoded_scope = addon::decode_observation_scope(member(*metadata, "scope"), &failure);
    require(encoded_scope.has_value(), "usage_snapshot_invalid");
    require(encoded_scope->source_account_key == (account ? std::optional(account->adapter_account_key) : std::nullopt),
        "usage_snapshot_account_mismatch");
    addon::Usage_scan_page page;
    page.scope = *encoded_scope;
    page.snapshot_id = identifier;
    if (version == 3 && request) {
        page.account_context = request->context.account_context;
        page.profile_storage = request->context.profile_storage;
        // Sealing replaces only this fixed-width digest. Measure the actual
        // candidate identity and context before admitting any page record.
        page.cache_candidate = addon::Usage_cache_reference{
            request->cache_selection->candidate_root, identifier, std::string(64, '0')};
    }
    const auto& incomplete = member(*metadata, "incomplete");
    require(incomplete.is_boolean(), "usage_snapshot_invalid");
    page.incomplete_historical_coverage = incomplete.as_boolean();
    std::size_t envelope_bytes = 0;
    if (incremental) {
        addon::Usage_incremental_result envelope{*request->cache_selection->accepted, page,
            request->context.account_context, request->context.profile_storage};
        envelope_bytes = addon::serialize_json(addon::encode_usage_incremental_result(envelope)).size() -
            addon::serialize_json(addon::encode_usage_scan_result(page, 3)).size();
    }
    const auto path = snapshot / (incremental ? "changes" : "records");
    require(fs::is_regular_file(fs::symlink_status(path)), "usage_snapshot_invalid");
    const auto size = fs::file_size(path);
    require(offset <= size, "usage_cursor_invalid");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    if (offset > 0) {
        input.seekg(static_cast<std::streamoff>(offset - 1));
        require(input.get() == '\n', "usage_cursor_invalid");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    std::string line;
    std::size_t bytes = 0;
    std::size_t encoded_records_bytes = 0;
    while (page.records.size() < addon::k_max_usage_page_records && read_line(input, &line)) {
        check_cancel(cancelled);
        if (!page.records.empty() && bytes + line.size() > k_line_limit / 2) break;
        auto observation = decode_record(line, version);
        if (version == 3) {
            observation.usage.aggregate_evidence = "partial";
            apply_temporal_observation(observation.usage, observation.temporal,
                identifier, page.scope.source_account_key);
            const auto next = offset + line.size() + 1;
            page.complete = next == size;
            page.next_cursor = page.complete ? std::nullopt : std::optional(std::to_string(next));
            if (!addon::append_usage_page_record(page, observation.usage, encoded_records_bytes, version, envelope_bytes)) {
                if (!page.records.empty()) break;
                if (observation.usage.temporal) {
                    observation.usage.temporal->portions.clear();
                    observation.usage.temporal->unavailable_tokens = observation.usage.total_tokens;
                    observation.usage.temporal->coverage_reason = "capacity";
                    if (!valid_temporal_record(observation.usage)) observation.usage.temporal.reset();
                }
                if (!addon::append_usage_page_record(page, observation.usage, encoded_records_bytes, version, envelope_bytes)) {
                    observation.usage.temporal.reset();
                    require(addon::append_usage_page_record(page, observation.usage, encoded_records_bytes, version, envelope_bytes),
                        "usage_record_frame_capacity");
                }
            }
        }
        else page.records.push_back(std::move(observation.usage));
        bytes += line.size();
        offset += line.size() + 1;
    }
    page.complete = offset == size;
    if (!page.complete) page.next_cursor = std::to_string(offset);
    else page.next_cursor.reset();
    return page;
}

// A selected cache's rollout metadata in cache-key order: the `rollouts`
// lines of the current format, which it writes in that order, or the `.meta`
// files of @1 and @2, which are named by their keys and listed, with the
// manifest's other files, in the manifest the SDK bounds.
class Accepted_rollouts final
{
public:
    Accepted_rollouts(const addon::Usage_cache_manifest& accepted, fs::path source)
    :
        m_source(std::move(source))
    {
        if (accepted.format == k_cache_format || accepted.format == k_piece_cache_format) {
            m_lines.emplace(m_source / "rollouts", std::ios::binary);
            require(static_cast<bool>(*m_lines), "usage_cache_unavailable");
        }
        else {
            for (const auto& file : accepted.files) {
                if (file.path.ends_with(".meta") && cache_file_name(file.path)) m_files.push_back(file.path);
            }
            std::sort(m_files.begin(), m_files.end(), id_less);
        }
        advance();
    }

    const std::optional<std::string>& key() const { return m_key; }

    addon::Json_value take()
    {
        auto metadata = std::move(*m_metadata);
        advance();
        return metadata;
    }

private:
    void advance()
    {
        std::optional<addon::Json_value> metadata;
        std::string key;
        std::string line;
        if (m_lines && read_line(*m_lines, &line)) {
            metadata = addon::parse_json(line, {}, nullptr);
            require(metadata && metadata->is_object(), "usage_cache_invalid");
            key = cache_key(string_member(*metadata, "source"));
        }
        else
        if (!m_lines && m_next_file < m_files.size()) {
            const auto& name = m_files[m_next_file++];
            metadata = read_json(m_source / name);
            require(metadata.has_value(), "usage_cache_unavailable");
            key = name.substr(0, 64);
            require(cache_key(string_member(*metadata, "source")) == key, "usage_cache_identity_mismatch");
        }
        if (!metadata) {
            m_key.reset();
            m_metadata.reset();
            return;
        }
        require(!m_key || id_less(*m_key, key), "usage_cache_invalid");
        m_key      = std::move(key);
        m_metadata = std::move(metadata);
    }

    fs::path                         m_source;
    std::optional<std::ifstream>     m_lines;
    std::vector<std::string>         m_files;
    std::size_t                      m_next_file = 0;
    std::optional<std::string>       m_key;
    std::optional<addon::Json_value> m_metadata;
};

// A sorted record file read one record at a time.
class Record_stream final
{
public:
    Record_stream(const fs::path& path, std::uint16_t version, const char* unavailable)
    :
        m_input(path, std::ios::binary),
        m_version(version)
    {
        require(static_cast<bool>(m_input), unavailable);
        advance();
    }

    const std::optional<Rollout_usage_record>& head() const { return m_head; }

    Rollout_usage_record take()
    {
        auto record = std::move(*m_head);
        advance();
        return record;
    }

private:
    void advance()
    {
        if (read_line(m_input, &m_line)) m_head = decode_record(m_line, m_version);
        else m_head.reset();
    }

    std::ifstream                       m_input;
    std::uint16_t                       m_version;
    std::string                         m_line;
    std::optional<Rollout_usage_record> m_head;
};

// Two observations of one turn that differ at most in where its rollout lives.
bool same_observation(Rollout_usage_record left, Rollout_usage_record right)
{
    for (auto* record : {&left, &right}) {
        record->usage.source_path.reset();
        record->usage.repository_path.reset();
        record->usage.repository_usage.reset();
    }
    return encode_record(left, 3, false) == encode_record(right, 3, false);
}

// Whether an observation of an accepted turn confirms the accepted record
// rather than changing it. A proved observation's total is exactly the turn's
// tokens, so only an equal total confirms it. An unproved total can include
// tokens that a missing earlier sample would have accounted for, as when a
// rollout lost the events before the turn, so it cannot raise an accepted
// total; a lower one still contradicts it.
bool confirms(const Rollout_usage_record& accepted, const Rollout_usage_record& observed)
{
    return observed.usage.cross_source_event
        ? observed.usage.total_tokens == accepted.usage.total_tokens
        : observed.usage.total_tokens >= accepted.usage.total_tokens;
}

void clear_repository_correction(addon::Usage_record& record)
{
    const auto* tag = record.capabilities.find(addon::k_usage_repository_attribution_capability_id);
    if (tag && addon::decode_usage_repository_attribution_tag(*tag, nullptr) ==
        addon::Usage_repository_attribution_tag_status::AVAILABLE)
    {
        record.capabilities.erase(addon::k_usage_repository_attribution_capability_id);
    }
}

void correct_repository(Rollout_usage_record& accepted, const Rollout_usage_record& observed,
    Usage_scan_authority authority)
{
    if (authority != Usage_scan_authority::REPOSITORY_ATTRIBUTION || !observed.turn_cwd_witnessed ||
        observed.recorded_cwd == accepted.recorded_cwd || !observed.usage.repository_usage ||
        observed.usage.repository_usage->allocations.size() != 1 ||
        accepted.usage.capabilities.find(addon::k_usage_repository_attribution_capability_id)) return;
    accepted.recorded_cwd = observed.recorded_cwd;
    refresh_repository(&accepted, observed.usage.repository_usage->allocations.front().repository_path);
    accepted.usage.capabilities.set(addon::k_usage_repository_attribution_capability_id,
        addon::encode_usage_repository_attribution_tag());
}

void defer_repository_growth(const Rollout_usage_record& accepted, Rollout_usage_record& observed)
{
    if (observed.recorded_cwd == accepted.recorded_cwd ||
        observed.usage.total_tokens <= accepted.usage.total_tokens) return;
    // Numeric growth and reattribution have separate authority. Preserve known
    // allocations until a later parse confirms the newly accepted total.
    auto allocation = accepted.usage.repository_usage.value_or(
        addon::Usage_repository{{}, accepted.usage.total_tokens});
    allocation.unattributed_tokens += observed.usage.total_tokens - accepted.usage.total_tokens;
    observed.usage.repository_path.reset();
    observed.usage.repository_usage = std::move(allocation);
    observed.recorded_cwd = accepted.recorded_cwd;
}

void reduce_legacy_repository(const Rollout_usage_record& accepted, Rollout_usage_record& observed)
{
    auto allocation = accepted.usage.repository_usage.value_or(
        addon::Usage_repository{{}, accepted.usage.total_tokens});
    auto known = accepted.usage.total_tokens - allocation.unattributed_tokens;
    // Native Codex observations have at most one known repository. Legacy
    // numeric reductions consume deferred unknown tokens first, then reduce
    // that sole component without granting authority to move it elsewhere.
    if (allocation.allocations.size() == 1) {
        known = std::min(known, observed.usage.total_tokens);
        allocation.allocations.front().total_tokens = known;
    }
    // A mixed external cache has no evidence identifying which known
    // component a reduction below their combined total should consume.
    require(known <= observed.usage.total_tokens, "usage_repository_attribution_ambiguous");
    allocation.unattributed_tokens = observed.usage.total_tokens - known;
    observed.usage.repository_path.reset();
    observed.usage.repository_usage = std::move(allocation);
    observed.recorded_cwd = accepted.recorded_cwd;
}

// Writes the snapshot's `records`, reconciling every turn by its stable record
// ID. A turn parsed in this scan wins over one converted from a native seed,
// which wins over the accepted record, and a turn missing from a rollout parsed
// again keeps its accepted record. An observation that confirms an accepted
// turn leaves the accepted accounting whole: where its rollout lived, its
// completion date, provenance and category detail are
// facts of the accepted observation, which the host compares a partial
// observation against and rejects when they change. So a moved rollout keeps
// one observation under its accepted identity, a checkout removed or moved
// since acceptance cannot retract the accepted attribution, and a turn
// imported from the native cache keeps its native provenance when its rollout
// is parsed. A confirmed record without an event proof takes the
// observation's, so a turn accepted before its proof could be established
// gains it when its rollout is parsed again; the host adds a proof to an
// accepted record and keeps one an observation lacks. An observation that
// contradicts an accepted turn is presented for the host to accept or reject.
// A separately selected attribution operation may correct a confirmed turn's
// repository from matching live context. Numeric growth defers that correction
// and retains the old allocation, with its growth unknown. A host-selected
// native seed supplies the accepted observation on first import. Differing
// observations of one turn within this scan fail it.
bool merge_snapshot_records(const fs::path& output_path, const std::optional<fs::path>& accepted,
    std::uint16_t accepted_version, Record_sorter& accepted_rollout_keys,
    const fs::path& seeded_path, const fs::path& parsed_path, const std::function<bool()>& cancelled,
    Record_sorter* changes = nullptr, Usage_scan_authority authority = Usage_scan_authority::STANDARD)
{
    std::optional<Record_stream> retained;
    if (accepted) retained.emplace(*accepted, accepted_version, "usage_cache_unavailable");
    Record_stream seeded(seeded_path, 3, "usage_storage_read_failed");
    Record_stream parsed(parsed_path, 3, "usage_storage_read_failed");
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    // Reads one turn's observations from a stream, keeping the one whose
    // rollout path sorts first; differing observations fail the scan. A stream
    // outranked by another one for this turn is drained unexamined.
    const auto choose = [&cancelled](Record_stream& stream, const std::string& id, bool examine) {
        std::optional<Rollout_usage_record> chosen;
        while (stream.head() && stream.head()->usage.record_id == id) {
            check_cancel(cancelled);
            auto observation = stream.take();
            if (!examine) continue;
            if (!chosen) {
                chosen = std::move(observation);
                continue;
            }
            require(same_observation(observation, *chosen), "duplicate_usage_record");
            const bool witnessed = chosen->turn_cwd_witnessed || observation.turn_cwd_witnessed;
            if (observation.usage.source_path.value_or("") < chosen->usage.source_path.value_or("")) {
                reconcile_temporal_observations(observation.temporal, chosen->temporal);
                chosen = std::move(observation);
            }
            else reconcile_temporal_observations(chosen->temporal, observation.temporal);
            chosen->turn_cwd_witnessed = witnessed;
        }
        return chosen;
    };
    std::string previous_accepted;
    bool changed = false;
    for (;;) {
        check_cancel(cancelled);
        std::optional<std::string> id;
        for (const auto* stream : {retained ? &*retained : nullptr, &seeded, &parsed}) {
            if (stream && stream->head() && (!id || id_less(stream->head()->usage.record_id, *id))) {
                id = stream->head()->usage.record_id;
            }
        }
        if (!id) break;
        std::optional<Rollout_usage_record> accepted_record;
        std::optional<std::string> accepted_encoding;
        if (retained && retained->head() && retained->head()->usage.record_id == *id) {
            accepted_record = retained->take();
            require(accepted_record->usage.source_path.has_value(), "usage_cache_identity_mismatch");
            auto key = cache_key(*accepted_record->usage.source_path);
            accepted_rollout_keys.add({key, key});
            require(previous_accepted.empty() || id_less(previous_accepted, *id), "usage_cache_invalid");
            previous_accepted = *id;
            if (changes) accepted_encoding = encode_record(*accepted_record, 3);
        }
        const bool parsed_again = parsed.head() && parsed.head()->usage.record_id == *id;
        auto seeded_choice = choose(seeded, *id, !accepted_record);
        auto parsed_choice = choose(parsed, *id, true);
        if (!accepted_record && seeded_choice) accepted_record = *seeded_choice;
        auto& chosen = parsed_again ? parsed_choice : seeded_choice;
        Rollout_usage_record* record = accepted_record ? &*accepted_record : nullptr;
        if (record) clear_repository_correction(record->usage);
        if (chosen) {
            if (!accepted_record || !confirms(*accepted_record, *chosen)) {
                if (accepted_record) defer_repository_growth(*accepted_record, *chosen);
                record = &*chosen;
            }
            else {
                if (!accepted_record->usage.cross_source_event) {
                    accepted_record->usage.cross_source_event = chosen->usage.cross_source_event;
                }
                reconcile_temporal_observations(accepted_record->temporal, chosen->temporal);
                if (parsed_again) correct_repository(*accepted_record, *chosen, authority);
            }
        }
        auto encoded = encode_record(*record, 3);
        write_line(output, encoded);
        if (changes && (!accepted_encoding || *accepted_encoding != encoded)) {
            changed = true;
            changes->add({record->usage.record_id, std::move(encoded)});
        }
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    return changed;
}

// Requires that every accepted record's rollout, keyed in `keys_path`, is one
// the candidate's `rollouts` lists, whether from the accepted cache, the
// native seed or a live rollout. Both files are sorted by cache key, so one
// pass joins them.
void require_listed_rollouts(const fs::path& keys_path, const fs::path& rollouts_path,
    const std::function<bool()>& cancelled)
{
    std::ifstream keys(keys_path, std::ios::binary);
    std::ifstream rollouts(rollouts_path, std::ios::binary);
    require(keys && rollouts, "usage_storage_read_failed");
    std::string key;
    std::string line;
    std::optional<std::string> listed;
    while (read_line(keys, &key)) {
        check_cancel(cancelled);
        while ((!listed || id_less(*listed, key)) && read_line(rollouts, &line)) {
            check_cancel(cancelled);
            const auto metadata = addon::parse_json(line, {}, nullptr);
            require(metadata && metadata->is_object(), "usage_cache_invalid");
            listed = cache_key(string_member(*metadata, "source"));
        }
        require(listed == key, "usage_cache_identity_mismatch");
    }
}

std::string record_piece_name(std::string_view id)
{
    // Turns in one session share a piece, so appending a long rollout does
    // not scatter its earlier observations across the account's whole cache.
    const auto separator = id.rfind(':');
    const auto identity = id.starts_with("codex:") && separator > 6 && separator != std::string_view::npos
        ? id.substr(0, separator) : id;
    return "records-" + digest(identity).substr(0, 2);
}

bool record_piece_file(std::string_view name)
{
    return name.size() == 10 && name.starts_with("records-") &&
        std::all_of(name.begin() + 8, name.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        });
}

std::set<std::string> partition_records(const fs::path& path, const fs::path& work, std::string_view prefix,
    std::uint16_t version, const std::function<bool()>& cancelled, Record_sorter* accepted_keys = nullptr)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_cache_unavailable");
    std::map<std::string, std::ofstream> outputs;
    std::set<std::string> pieces;
    std::string line;
    std::string previous;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        const auto record = decode_record(line, version);
        if (accepted_keys) {
            require(previous.empty() || id_less(previous, record.usage.record_id), "usage_cache_invalid");
            require(record.usage.source_path.has_value(), "usage_cache_identity_mismatch");
            const auto key = cache_key(*record.usage.source_path);
            accepted_keys->add({key, key});
            previous = record.usage.record_id;
        }
        const auto name = record_piece_name(record.usage.record_id);
        auto [output, inserted] = outputs.try_emplace(name);
        if (inserted) {
            output->second.open(work / (std::string(prefix) + name), std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(output->second), "usage_storage_write_failed");
            pieces.insert(name);
        }
        write_line(output->second, version == 3 ? line : encode_record(record, 3));
    }
    for (auto& [name, output] : outputs) {
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
    }
    return pieces;
}

void flatten_record_pieces(const addon::Usage_cache_manifest& manifest, const fs::path& source,
    const fs::path& work, const Usage_scan_limits& limits, const std::function<bool()>& cancelled)
{
    Record_sorter sorted(work, "accepted-run", limits.sort_run_bytes, cancelled, record_id_key(3), true);
    for (const auto& file : manifest.files) {
        if (!record_piece_file(file.path)) continue;
        std::ifstream input(source / file.path, std::ios::binary);
        require(static_cast<bool>(input), "usage_cache_unavailable");
        std::string line;
        while (read_line(input, &line)) {
            check_cancel(cancelled);
            auto id = decode_record(line, 3).usage.record_id;
            require(record_piece_name(id) == file.path, "usage_cache_identity_mismatch");
            sorted.add({std::move(id), std::move(line)});
        }
    }
    sorted.finish(work / "accepted-records");
}

void merge_incremental_records(const addon::Usage_scan_request& request,
    const addon::Usage_cache_manifest& accepted, const fs::path& work, std::uint16_t accepted_version,
    Record_sorter& accepted_keys, const fs::path& seeded_path, const fs::path& parsed_path,
    const Usage_scan_limits& limits, const std::function<bool()>& cancelled, Usage_scan_authority authority)
{
    const auto& baseline = *request.cache_selection->accepted;
    const auto source = addon::usage_cache_io_path(addon::path_from_utf8(baseline.root));
    const auto seeded = partition_records(seeded_path, work, "seeded-", 3, cancelled);
    const auto parsed = partition_records(parsed_path, work, "parsed-", 3, cancelled);
    std::set<std::string> touched = seeded;
    touched.insert(parsed.begin(), parsed.end());
    std::set<std::string> retained;
    const bool adopting = accepted.format != k_piece_cache_format;
    if (adopting) {
        retained = partition_records(source / "records", work, "accepted-", accepted_version,
            cancelled, &accepted_keys);
    }
    else {
        for (const auto& file : accepted.files) {
            if (record_piece_file(file.path)) retained.insert(file.path);
        }
    }
    auto pieces = retained;
    pieces.insert(touched.begin(), touched.end());
    const auto empty_path = work / "empty-records";
    write_rollout_records(empty_path, {});
    Record_sorter changes(work, "changed-run", limits.sort_run_bytes, cancelled, record_id_key(3), true);
    addon::Contract_failure failure;
    for (const auto& piece : pieces) {
        check_cancel(cancelled);
        const bool have_retained = retained.contains(piece);
        const auto accepted_path = adopting ? work / ("accepted-" + piece) : source / piece;
        bool changed = false;
        if (touched.contains(piece)) {
            if (have_retained && !adopting &&
                !addon::validate_usage_cache_file(baseline, accepted, piece, cancelled, &failure))
            {
                throw Scan_failure{failure.reason_code};
            }
            changed = merge_snapshot_records(work / piece,
                have_retained ? std::optional(accepted_path) : std::nullopt, 3, accepted_keys,
                seeded.contains(piece) ? work / ("seeded-" + piece) : empty_path,
                parsed.contains(piece) ? work / ("parsed-" + piece) : empty_path,
                cancelled, &changes, authority);
            if (!changed) fs::remove(work / piece);
        }
        if (!changed && have_retained) {
            if (adopting) fs::rename(accepted_path, work / piece);
            else
            if (!addon::inherit_usage_cache_file(baseline, accepted, piece,
                    request.cache_selection->candidate_root, request.context.profile_storage->root,
                    cancelled, &failure))
            {
                throw Scan_failure{failure.reason_code};
            }
        }
        if (adopting && changed && have_retained) fs::remove(accepted_path);
        if (seeded.contains(piece)) fs::remove(work / ("seeded-" + piece));
        if (parsed.contains(piece)) fs::remove(work / ("parsed-" + piece));
    }
    changes.finish(work / "changes");
    fs::remove(empty_path);
}

void append_rollout_records(const fs::path& spool, Record_sorter& parsed,
    const std::function<bool()>& cancelled)
{
    std::ifstream spooled(spool, std::ios::binary);
    require(static_cast<bool>(spooled), "usage_storage_read_failed");
    std::string line;
    while (read_line(spooled, &line)) {
        check_cancel(cancelled);
        auto id = decode_record(line, 3).usage.record_id;
        parsed.add({std::move(id), std::move(line)});
    }
    spooled.close();
    fs::remove(spool);
}

// Writes the snapshot's `rollouts` in cache-key order, reconciling by key the
// accepted metadata, the seed's and the rollouts found in native history
// directories, and parses each found rollout whose metadata does not describe
// it, passing the records of every parse that holds to `parsed`. A seed's
// metadata replaces the accepted metadata of its rollout. Rollouts found under
// one key are refreshed in the order they were found, each against the
// metadata the one before left. A key no source supplies metadata for gets no
// line. Returns whether a rollout could not be read at rest.
bool reconcile_rollouts(const fs::path& work, std::optional<Accepted_rollouts>& accepted,
    const fs::path& seed_path, const fs::path& found_path, bool force, Record_sorter& parsed, Record_sorter& peers,
    Record_sorter& unindexed, Usage_scan_progress& progress, const std::function<bool()>& cancelled, bool* changed,
    bool* reparsed,
    bool refresh_missing_temporal)
{
    Spool_stream seed(seed_path);
    Spool_stream found(found_path);
    // A parsed rollout's records wait here until its parse is known to hold.
    const auto spool = work / "rollout-records";
    std::ofstream output(work / "rollouts", std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    const std::optional<std::string> none;
    bool incomplete = false;
    for (;;) {
        check_cancel(cancelled);
        std::optional<std::string> key;
        for (const auto* head : {accepted ? &accepted->key() : &none, &seed.key(), &found.key()}) {
            if (*head && (!key || id_less(**head, *key))) key = *head;
        }
        if (!key) break;
        std::optional<addon::Json_value> metadata;
        if (accepted && accepted->key() == key) metadata = accepted->take();
        if (seed.key() == key) {
            *changed = true;
            auto seeded = seed.take();
            require(seed.key() != key && (!metadata || member(*metadata, "native_seed") != member(seeded, "native_seed")),
                "native_usage_cache_duplicate_path");
            metadata = std::move(seeded);
        }
        while (found.key() == key) {
            auto rollout = found.take();
            auto refreshed = refresh_rollout(addon::path_from_utf8(string_member(rollout, "path")),
                string_member(rollout, "source"), metadata, force, cancelled, 3, spool, refresh_missing_temporal);
            progress.advance();
            if (refreshed.incomplete) incomplete = true;
            const bool parsed_again = refreshed.metadata.has_value();
            if (parsed_again) {
                *reparsed = true;
                *changed = true;
                metadata = std::move(refreshed.metadata);
                append_rollout_records(spool, parsed, cancelled);
            }
            if (!refreshed.incomplete && metadata) {
                // Parsed copies precede unchanged peers of the same session,
                // allowing a bounded second pass over only the affected peers.
                rollout.set("metadata", *metadata);
                rollout.set("parsed", addon::Json_value(parsed_again));
                const auto& session = member(*metadata, "native_session");
                if (session.is_string()) {
                    peers.add(spool_record(digest(session.as_string()), parsed_again ? 0 : 1, rollout));
                }
                else
                if (!metadata->find("native_session") || !session.is_null()) {
                    unindexed.add(spool_record(*key, 0, rollout));
                }
            }
        }
        if (metadata) write_line(output, addon::serialize_json(*metadata));
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    return incomplete;
}

std::optional<std::string> read_native_session(const fs::path& path, const std::function<bool()>& cancelled)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::optional<std::string> session;
    Codex_jsonl_projector projector([&session](const addon::Json_value& event) {
        if (member(event, "type") != addon::Json_value("session_meta")) {
            return true;
        }
        const auto& id = member(member(event, "payload"), "id");
        if (id.is_string() && !id.as_string().empty()) {
            session = id.as_string();
        }
        return false;
    });
    std::array<char, 4096> bytes{};
    std::string error;
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        if (!projector.feed(std::string_view(bytes.data(), (std::size_t)input.gcount()), &error)) {
            break;
        }
    }
    return input.bad() ? std::nullopt : session;
}

// An unchanged append-only rollout can inherit its native identity from saved
// observations of that source. Copies absent from the aggregate need only their
// session metadata read; identifying a peer does not recount its token events.
bool index_rollout_copies(const addon::Usage_scan_request& request,
    const std::optional<addon::Usage_cache_manifest>& accepted, std::uint16_t accepted_version,
    const fs::path& work, const fs::path& unindexed_path,
    Record_sorter& peers, const Usage_scan_limits& limits, const std::function<bool()>& cancelled)
{
    Record_sorter identities(work, "native-index-run", limits.sort_run_bytes, cancelled, spool_key(), false);
    addon::Contract_failure failure;
    if (accepted) {
        const auto& baseline = *request.cache_selection->accepted;
        const auto source = addon::usage_cache_io_path(addon::path_from_utf8(baseline.root));
        for (const auto& file : accepted->files) {
            if (file.path != "records" && !record_piece_file(file.path)) {
                continue;
            }
            if (!addon::validate_usage_cache_file(baseline, *accepted, file.path, cancelled, &failure)) {
                throw Scan_failure{failure.reason_code};
            }
            std::ifstream input(source / file.path, std::ios::binary);
            require(static_cast<bool>(input), "usage_cache_unavailable");
            std::string line;
            while (read_line(input, &line)) {
                check_cancel(cancelled);
                const auto record = decode_record(line, accepted_version);
                if (record.usage.source_path && record.usage.session_key) {
                    auto identity = addon::Json_value::object();
                    identity.set("session", addon::Json_value(*record.usage.session_key));
                    identities.add(spool_record(cache_key(*record.usage.source_path), 0, identity));
                }
            }
        }
    }
    const auto index_path = work / "native-index";
    identities.finish(index_path);
    bool incomplete = false;
    {
        Spool_stream indexed(index_path);
        Spool_stream unindexed(unindexed_path);
        while (unindexed.key()) {
            check_cancel(cancelled);
            const auto key = *unindexed.key();
            auto rollout = unindexed.take();
            while (indexed.key() && id_less(*indexed.key(), key)) {
                indexed.take();
            }
            std::optional<std::string> session;
            bool ambiguous = false;
            while (indexed.key() == key) {
                const auto id = string_member(indexed.take(), "session");
                if (session && *session != id) {
                    ambiguous = true;
                }
                session = id;
            }
            if (!session || ambiguous) {
                const auto path = addon::path_from_utf8(string_member(rollout, "path"));
                const auto& metadata = member(rollout, "metadata");
                const auto before = fingerprint(path);
                if (!before || !fingerprint_matches(metadata, *before)) {
                    incomplete = true;
                    continue;
                }
                session = read_native_session(path, cancelled);
                if (!session || fingerprint(path) != before) {
                    incomplete = true;
                    continue;
                }
            }
            peers.add(spool_record(digest(*session), 1, rollout));
        }
    }
    fs::remove(index_path);
    return incomplete;
}

// Accepted aggregates can retain an earlier observation of a current copy.
// Compare changed sources with the unchanged copies themselves instead.
bool recheck_rollout_copies(const fs::path& work, const fs::path& peers_path, Record_sorter& parsed,
    Usage_scan_progress& progress, const std::function<bool()>& cancelled)
{
    Spool_stream peers(peers_path);
    const auto spool = work / "rollout-records";
    std::optional<std::string> session;
    bool changed = false;
    bool incomplete = false;
    while (peers.key()) {
        check_cancel(cancelled);
        if (session != peers.key()) {
            session = peers.key();
            changed = false;
        }
        const auto rollout = peers.take();
        if (member(rollout, "parsed") == addon::Json_value(true)) {
            changed = true;
            continue;
        }
        if (!changed) {
            continue;
        }
        const auto path = addon::path_from_utf8(string_member(rollout, "path"));
        const auto& metadata = member(rollout, "metadata");
        const auto before = fingerprint(path);
        if (!before || !fingerprint_matches(metadata, *before)) {
            incomplete = true;
            continue;
        }
        const auto refreshed = refresh_rollout(path, string_member(rollout, "source"), metadata,
            true, cancelled, 3, spool);
        progress.advance();
        if (!refreshed.metadata) {
            incomplete = true;
            continue;
        }
        // A copy changing between passes cannot be represented under the
        // fingerprint already recorded in the candidate's rollout metadata.
        if (!fingerprint_matches(*refreshed.metadata, *before)) {
            fs::remove(spool);
            incomplete = true;
            continue;
        }
        append_rollout_records(spool, parsed, cancelled);
    }
    return incomplete;
}

std::string selection_digest(addon::Usage_scan_request request, bool incremental, Usage_scan_authority authority)
{
    request.snapshot_id.reset();
    request.cursor.reset();
    auto identity = addon::Json_value::object();
    const bool attribution = authority == Usage_scan_authority::REPOSITORY_ATTRIBUTION;
    identity.set("capability", addon::Json_value(attribution ? addon::k_usage_repository_attribution_capability_id :
        incremental ? addon::k_usage_incremental_capability_id : "usage-scan"));
    identity.set("version", addon::Json_value(attribution ? addon::k_usage_repository_attribution_version :
        incremental ? addon::k_usage_incremental_version : 3));
    identity.set("operation", addon::Json_value(attribution && incremental ?
        addon::k_usage_repository_attribution_incremental_operation : addon::k_usage_scan_operation));
    identity.set("request", addon::encode_usage_scan_request(request, 3));
    return digest(addon::serialize_json_canonical(identity));
}

std::optional<addon::Usage_scan_page> scan_selected_cache(
    const addon::Usage_scan_request& request,
    const fs::path& vendor_profile,
    const fs::path& storage,
    const std::function<bool()>& cancelled,
    Usage_scan_progress& progress,
    const Usage_scan_limits& limits,
    bool incremental = false, Usage_scan_authority authority = Usage_scan_authority::STANDARD)
{
    addon::Contract_failure failure;
    if (!addon::validate_usage_cache_selection(request, &failure)) {
        throw Scan_failure{failure.reason_code};
    }
    const auto& selection = *request.cache_selection;
    const auto work = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
    const auto profile_root = path_text(storage);
    const auto mutable_root = profile_account_key(path_text(storage / "codex-usage-v1"));
    const auto outside_mutable_cache = [&](const std::string& root) {
        for (auto path = addon::path_from_utf8(root); path.has_relative_path(); path = path.parent_path()) {
            if (profile_account_key(path_text(path)) == mutable_root) {
                return false;
            }
        }
        return true;
    };
    require(outside_mutable_cache(selection.candidate_root) &&
        (!selection.accepted || outside_mutable_cache(selection.accepted->root)), "usage_cache_namespace_conflict");
    if (!addon::validate_usage_cache_root(selection.candidate_root, profile_root, &failure)) {
        throw Scan_failure{failure.reason_code};
    }
    const auto request_hash = selection_digest(request, incremental, authority);
    if (request.snapshot_id) {
        require(request.cursor.has_value(), "usage_cursor_invalid");
        const auto state = read_json(work / "cache-state");
        require(state && string_member(*state, "request_sha256") == request_hash &&
            string_member(*state, "snapshot_id") == *request.snapshot_id, "usage_snapshot_mismatch");
        auto page = page_at(work, *request.snapshot_id, decimal(*request.cursor), request.context.account_context,
            cancelled, 3, &request, incremental);
        page.account_context = request.context.account_context;
        page.profile_storage = request.context.profile_storage;
        page.cache_candidate = addon::Usage_cache_reference{
            selection.candidate_root, page.snapshot_id, hash_file(work / "cache-manifest.json", cancelled)};
        for (auto& record : page.records) {
            record.aggregate_evidence = "partial";
        }
        return page;
    }
    require(!request.cursor, "usage_cursor_invalid");
    if (!addon::validate_usage_cache_allocation(selection.candidate_root, profile_root, &failure)) {
        throw Scan_failure{failure.reason_code};
    }
    // Every long step polls cancellation, the SDK's cache validation and
    // sealing included, so the poll is where the working scan reports.
    const std::function<bool()> working = [&] { progress.poll(); return cancelled(); };
    progress.begin("Preparing the usage cache");
    auto seeds = addon::Json_value::array();
    // The accepted rollout metadata and records stay on disk and are streamed
    // once into the new snapshot.
    std::optional<Accepted_rollouts> accepted_rollouts;
    std::optional<fs::path> accepted_records;
    std::optional<addon::Usage_cache_manifest> accepted_manifest;
    std::uint16_t accepted_version = 3;
    bool metadata_only = false;
    bool accepted_incomplete = false;
    if (selection.accepted) {
        accepted_manifest = incremental
            ? addon::validate_usage_cache_manifest(
                *selection.accepted, profile_root, request.context.account_context, working, &failure)
            : addon::validate_usage_cache_candidate(
                *selection.accepted, profile_root, request.context.account_context, working, &failure);
        check_cancel(working);
        const auto& accepted = accepted_manifest;
        require(accepted && (accepted->format == "codex-rollout-cache@1" ||
            accepted->format == "codex-rollout-cache@2" || accepted->format == k_cache_format ||
            accepted->format == k_piece_cache_format), "usage_cache_unavailable");
        const auto source = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root));
        metadata_only = incremental && (accepted->format == k_cache_format || accepted->format == k_piece_cache_format);
        if (metadata_only) {
            for (const auto* name : {"cache-state", "snapshot", "rollouts"}) {
                if (!addon::validate_usage_cache_file(*selection.accepted, *accepted, name, working, &failure)) {
                    throw Scan_failure{failure.reason_code};
                }
            }
        }
        else
        if (incremental && !addon::validate_usage_cache_candidate(
                *selection.accepted, profile_root, request.context.account_context, working, &failure))
        {
            throw Scan_failure{failure.reason_code};
        }
        const auto snapshot = read_json(source / "snapshot");
        require(snapshot && member(*snapshot, "incomplete").is_boolean(), "usage_cache_unavailable");
        accepted_incomplete = member(*snapshot, "incomplete").as_boolean();
        const auto state = read_json(source / "cache-state");
        require(state.has_value(), "usage_cache_unavailable");
        seeds = member(*state, "native_seeds");
        require(seeds.is_array(), "usage_cache_unavailable");
        accepted_rollouts.emplace(*accepted, source);
        if (!incremental && accepted->format == k_piece_cache_format) {
            flatten_record_pieces(*accepted, source, work, limits, working);
            accepted_records = work / "accepted-records";
        }
        else {
            accepted_records = source / "records";
        }
        accepted_version = accepted->format == "codex-rollout-cache@1" ? 2 : 3;
    }
    // Turns converted from a native seed and turns parsed in this scan, each
    // sorted by ID for reconciliation with the accepted records.
    Record_sorter seeded(work, "seeded-run", limits.sort_run_bytes, working, record_id_key(3), false);
    Record_sorter parsed(work, "parsed-run", limits.sort_run_bytes, working, record_id_key(3), false);
    // The seed's rollout metadata and the rollouts found in native history
    // directories, each sorted by cache key for reconciliation with the
    // accepted metadata.
    Record_sorter seed_rollouts(work, "seed-rollout-run", limits.sort_run_bytes, working, spool_key(), false);
    Record_sorter found_rollouts(work, "found-rollout-run", limits.sort_run_bytes, working, spool_key(), false);
    Record_sorter rollout_peers(work, "rollout-peer-run", limits.sort_run_bytes, working, spool_key(), false);
    Record_sorter unindexed_rollouts(work, "unindexed-run", limits.sort_run_bytes, working, spool_key(), false);
    std::uint64_t ordinal = 0;
    if (request.native_file_cache) progress.begin("Importing earlier usage history");
    const auto accepted_seeds = seeds;
    seed_native_cache(request, storage, selection.accepted.has_value(), &seeds,
        [&](const std::string& key, addon::Json_value metadata, std::vector<Sort_record> records) {
            seed_rollouts.add(spool_record(key, ordinal++, metadata));
            for (auto& record : records) {
                seeded.add(std::move(record));
            }
            progress.advance();
        }, working, 3);
    progress.begin("Reading Codex sessions");
    visit_rollouts(vendor_profile, working, [&](const fs::path& path) {
        const auto source = path_text(fs::absolute(path).lexically_normal());
        auto rollout = addon::Json_value::object();
        rollout.set("source", addon::Json_value(source));
        rollout.set("path", addon::Json_value(addon::path_to_utf8(path)));
        found_rollouts.add(spool_record(cache_key(source), ordinal++, rollout));
    });
    const auto seed_rollouts_path  = work / "seed-rollouts";
    const auto found_rollouts_path = work / "found-rollouts";
    seed_rollouts.finish(seed_rollouts_path);
    found_rollouts.finish(found_rollouts_path);
    bool metadata_changed = seeds != accepted_seeds;
    bool reparsed = false;
    bool incomplete = reconcile_rollouts(work, accepted_rollouts, seed_rollouts_path, found_rollouts_path,
        request.mode != "incremental", parsed, rollout_peers, unindexed_rollouts,
        progress, working, &metadata_changed, &reparsed, !incremental);
    fs::remove(seed_rollouts_path);
    fs::remove(found_rollouts_path);
    const auto unindexed_path = work / "unindexed-rollouts";
    unindexed_rollouts.finish(unindexed_path);
    if (reparsed && fs::file_size(unindexed_path) != 0 &&
        index_rollout_copies(request, accepted_manifest, accepted_version, work,
            unindexed_path, rollout_peers, limits, working))
    {
        incomplete = true;
    }
    fs::remove(unindexed_path);
    const auto peers_path = work / "rollout-peers";
    rollout_peers.finish(peers_path);
    if (recheck_rollout_copies(work, peers_path, parsed, progress, working)) {
        incomplete = true;
    }
    fs::remove(peers_path);
    // Incremental source observations do not prove that earlier coverage gaps
    // were resolved. A repair or reset verifies current sources in full.
    if (request.mode == "incremental" && accepted_incomplete) {
        incomplete = true;
    }
    if (metadata_only && !metadata_changed && incomplete == accepted_incomplete) {
        // The host already owns this accounting generation. Examining source
        // metadata does not require replaying or republishing its records.
        fs::remove(work / "rollouts");
        check_cancel(working);
        return std::nullopt;
    }
    if (metadata_only && accepted_manifest->format != k_piece_cache_format && !addon::validate_usage_cache_candidate(
            *selection.accepted, profile_root, request.context.account_context, working, &failure))
    {
        throw Scan_failure{failure.reason_code};
    }
    const auto seeded_path = work / "seeded-records";
    const auto parsed_path = work / "parsed-records";
    const auto accepted_keys_path = work / "accepted-rollout-keys";
    progress.begin("Sorting usage records");
    seeded.finish(seeded_path);
    parsed.finish(parsed_path);
    progress.begin("Merging usage history");
    Record_sorter accepted_keys(work, "key-run", limits.sort_run_bytes, working,
        [](const std::string& line) { return line; }, false);
    if (incremental) {
        merge_incremental_records(request, *accepted_manifest, work, accepted_version, accepted_keys,
            seeded_path, parsed_path, limits, working, authority);
    }
    else {
        merge_snapshot_records(work / "records", accepted_records, accepted_version,
            accepted_keys, seeded_path, parsed_path, working, nullptr, authority);
        if (accepted_manifest && accepted_manifest->format == k_piece_cache_format) fs::remove(*accepted_records);
    }
    accepted_keys.finish(accepted_keys_path);
    fs::remove(seeded_path);
    fs::remove(parsed_path);
    progress.begin("Saving the usage cache");
    require_listed_rollouts(accepted_keys_path, work / "rollouts", working);
    fs::remove(accepted_keys_path);
    const auto identifier = nonce();
    addon::Observation_scope scope;
    if (request.context.account_context) {
        scope.source_account_key = request.context.account_context->adapter_account_key;
    }
    scope.observed_at_ms = (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto metadata = addon::Json_value::object();
    metadata.set("scope", addon::encode_observation_scope(scope));
    metadata.set("incomplete", addon::Json_value(incomplete));
    write_json(work / "snapshot", metadata);
    auto state = addon::Json_value::object();
    state.set("request_sha256", addon::Json_value(request_hash));
    state.set("snapshot_id", addon::Json_value(identifier));
    state.set("native_seeds", std::move(seeds));
    write_json(work / "cache-state", state);
    auto page = page_at(work, identifier, 0, request.context.account_context, working, 3, &request, incremental);
    page.account_context = request.context.account_context;
    page.profile_storage = request.context.profile_storage;
    // A parsed rollout need not contain all historical contributing events.
    // This scanner cannot certify lower aggregate corrections from parsing alone.
    for (auto& record : page.records) {
        record.aggregate_evidence = "partial";
    }
    page.cache_candidate = incremental
        ? addon::seal_usage_incremental_cache_candidate(
            selection.candidate_root, profile_root, request.context.account_context,
            identifier, std::string(k_piece_cache_format), *selection.accepted, working, &failure)
        : addon::seal_usage_cache_candidate(
            selection.candidate_root, profile_root, request.context.account_context,
            identifier, std::string(k_cache_format), working, &failure);
    if (!page.cache_candidate) {
        throw Scan_failure{failure.reason_code};
    }
    require(addon::decode_usage_scan_result(addon::encode_usage_scan_result(page, 3), &failure, 3).has_value(),
        "usage_snapshot_invalid");
    return page;
}

} // namespace

Usage_scan_progress::Usage_scan_progress(
    Sink                      sink,
    std::chrono::milliseconds tolerated_silence,
    Clock                     clock)
:
    m_sink(std::move(sink)),
    m_clock(std::move(clock)),
    m_interval(std::min(k_progress_interval, tolerated_silence / 4))
{}

void Usage_scan_progress::begin(std::string note)
{
    m_note      = std::move(note);
    m_completed = 0;
    emit();
}

void Usage_scan_progress::poll()
{
    if (m_clock() - m_last_frame >= m_interval) {
        emit();
    }
}

void Usage_scan_progress::emit()
{
    m_last_frame = m_clock();
    m_sink(m_completed, m_note);
}

std::optional<addon::Usage_scan_page> scan_usage(const addon::Usage_scan_request& request,
    const fs::path& vendor_profile, const fs::path& storage, const std::function<bool()>& cancelled,
    Usage_scan_progress& progress, std::string* error, const Usage_scan_limits& limits, Usage_scan_authority authority)
{
    try {
        std::unique_lock lock(scan_mutex, std::defer_lock);
        // Selected continuations only read sealed, host-owned candidates.
        // Their lifetime spans the host's page loop; another account's scan
        // cannot change or prune them. Keep writers and mutable-cache pages
        // serialized without delaying delivery of an already completed scan.
        if (!request.cache_selection || !request.snapshot_id) {
            while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        }
        check_cancel(cancelled);
        if (request.cache_selection) {
            return scan_selected_cache(request, vendor_profile, storage, cancelled, progress, limits, false, authority);
        }
        require(authority == Usage_scan_authority::STANDARD, "usage_cache_selection_required");
        const auto root = directory(storage, "codex-usage-v1");
        Storage_lock storage_lock(root / "scan.lock", cancelled);
        const auto snapshots = directory(root, "snapshots");
        if (request.snapshot_id) {
            require(digest_name(*request.snapshot_id) && request.cursor.has_value(), "usage_cursor_invalid");
            return page_at(snapshots / *request.snapshot_id, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled);
        }
        require(!request.cursor, "usage_cursor_invalid");
        // As in scan_selected_cache, the working scan reports where it polls.
        const std::function<bool()> working = [&] { progress.poll(); return cancelled(); };
        progress.begin("Preparing the usage cache");
        require(fs::is_directory(vendor_profile), "usage_sessions_unavailable");
        const auto accepted = read_json(root / "accepted");
        auto seeds = addon::Json_value::array();
        std::string accepted_identifier;
        if (accepted) {
            require(member(*accepted, "account") == account_value(request.context.account_context),
                "usage_snapshot_account_mismatch");
            accepted_identifier = string_member(*accepted, "snapshot");
            require(digest_name(accepted_identifier), "required_usage_cache_unavailable");
            seeds = member(*accepted, "native_seeds");
            require(seeds.is_array(), "required_usage_cache_unavailable");
            // A previous pointer replacement may have reported a directory
            // flush failure. Settle that visible pointer before pruning the
            // generation to which it could otherwise roll back after a crash.
            sync_path(root / "accepted", false);
            sync_path(root, true);
            sync_path(storage, true);
        }
        prune_snapshots(snapshots, accepted_identifier, working);
        const auto identifier = nonce();
        const auto work = directory(snapshots, identifier);
        Pending_snapshot pending(work);
        if (accepted) {
            copy_accepted_cache(*accepted, snapshots, work, working);
        }
        if (request.native_file_cache) progress.begin("Importing earlier usage history");
        seed_native_cache(request, storage, accepted.has_value(), &seeds,
            [&work, &progress](const std::string& key, addon::Json_value metadata, std::vector<Sort_record> records) {
                const auto previous = read_json(work / (key + ".meta"));
                require(!previous || member(*previous, "native_seed") != member(metadata, "native_seed"),
                    "native_usage_cache_duplicate_path");
                metadata.set("seed_snapshot", addon::Json_value(work.filename().string()));
                write_rollout_records(work / (key + ".records"), records);
                write_json(work / (key + ".meta"), metadata);
                progress.advance();
            }, working);
        bool incomplete = false;
        progress.begin("Reading Codex sessions");
        visit_rollouts(vendor_profile, working, [&](const fs::path& path) {
            refresh_file(path, work, request.mode != "incremental", &incomplete, working);
            progress.advance();
        });
        progress.begin("Sorting usage records");
        sorted_snapshot(work, limits.sort_run_bytes, working,
            accepted ? std::optional(snapshots / accepted_identifier / "records") : std::nullopt);
        check_cancel(working);
        progress.begin("Saving the usage cache");
        addon::Observation_scope scope;
        if (request.context.account_context) scope.source_account_key = request.context.account_context->adapter_account_key;
        scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        auto metadata = addon::Json_value::object();
        metadata.set("scope", addon::encode_observation_scope(scope));
        metadata.set("incomplete", addon::Json_value(incomplete));
        write_json(work / "snapshot", metadata);
        auto page = page_at(work, identifier, 0, request.context.account_context, working);
        auto next = accepted_generation(work, seeds, request.context.account_context, working);
        // Include response construction in the candidate: encoding can still fail
        // before the one pointer exposes this generation and its seed receipts.
        const auto encoded = addon::encode_usage_scan_result(page, 2);
        addon::Contract_failure failure;
        require(addon::decode_usage_scan_result(encoded, &failure, 2).has_value(), "usage_snapshot_invalid");
        sync_path(root, true);
        sync_path(storage, true);
        check_cancel(working);
        // A failed rename/directory flush can leave the new pointer visible.
        // Preserve both generations until the next scan resolves that pointer.
        write_json(root / "accepted", next, &pending);
        return page;
    }
    catch (const Scan_failure& failure) { *error = failure.reason; }
    catch (const fs::filesystem_error&) { *error = "usage_storage_io_failed"; }
    catch (const std::exception&) { *error = "usage_scan_failed"; }
    return std::nullopt;
}

std::optional<addon::Usage_incremental_result> scan_usage_incremental(
    const addon::Usage_scan_request& request, const fs::path& vendor_profile, const fs::path& storage,
    const std::function<bool()>& cancelled, Usage_scan_progress& progress,
    std::string* error, const Usage_scan_limits& limits, Usage_scan_authority authority)
{
    try {
        require(request.mode == "incremental" && request.cache_selection && request.cache_selection->accepted &&
            !request.native_file_cache,
            "usage_incremental_baseline_required");
        std::unique_lock lock(scan_mutex, std::defer_lock);
        if (!request.snapshot_id) {
            while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        }
        check_cancel(cancelled);
        addon::Usage_incremental_result result{*request.cache_selection->accepted,
            scan_selected_cache(request, vendor_profile, storage, cancelled, progress, limits, true, authority),
            request.context.account_context, request.context.profile_storage};
        return result;
    }
    catch (const Scan_failure& failure) { *error = failure.reason; }
    catch (const fs::filesystem_error&) { *error = "usage_storage_io_failed"; }
    catch (const std::exception&) { *error = "usage_scan_failed"; }
    return std::nullopt;
}

void handle_usage_incremental(addon::Invocation_context& context)
{
    const bool attribution = context.request().capability.capability_id == addon::k_usage_repository_attribution_capability_id;
    if (context.request().operation != (attribution ? addon::k_usage_repository_attribution_incremental_operation :
        addon::k_usage_scan_operation))
    {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_incremental_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    const auto profile = selected_profile(request->context, context);
    if (!profile) return;
    if (!request->context.profile_storage) {
        context.fail(addon::Result_status::UNAVAILABLE, "profile_storage_unavailable");
        return;
    }
    std::string error;
    Usage_scan_progress progress(
        [&context](std::uint64_t completed, const std::string& note) {
            context.report_progress(completed, std::nullopt, note);
        },
        std::chrono::milliseconds(context.request().deadline_ms));
    const auto result = scan_usage_incremental(*request, addon::path_from_utf8(profile->profile_path),
        addon::path_from_utf8(request->context.profile_storage->root),
        [&context] { return context.cancel_requested(); }, progress, &error, {}, attribution ?
            Usage_scan_authority::REPOSITORY_ATTRIBUTION : Usage_scan_authority::STANDARD);
    if (!result) {
        context.fail(error == "cancelled" ? addon::Result_status::CANCELLED : addon::Result_status::FAILED, error);
        return;
    }
    context.succeed(addon::encode_usage_incremental_result(*result));
}

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const bool attribution = context.request().capability.capability_id == addon::k_usage_repository_attribution_capability_id;
    const std::uint16_t version = attribution ? 3 : context.request().capability.version;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure, version);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    const auto profile = selected_profile(request->context, context);
    if (!profile) return;
    if (!request->context.profile_storage) { context.fail(addon::Result_status::UNAVAILABLE, "profile_storage_unavailable"); return; }
    std::string error;
    Usage_scan_progress progress(
        [&context](std::uint64_t completed, const std::string& note) {
            context.report_progress(completed, std::nullopt, note);
        },
        std::chrono::milliseconds(context.request().deadline_ms));
    const auto page = scan_usage(*request, addon::path_from_utf8(profile->profile_path), addon::path_from_utf8(request->context.profile_storage->root),
        [&context] { return context.cancel_requested(); }, progress, &error, {}, attribution ?
            Usage_scan_authority::REPOSITORY_ATTRIBUTION : Usage_scan_authority::STANDARD);
    if (!page) { context.fail(error == "cancelled" ? addon::Result_status::CANCELLED : addon::Result_status::FAILED, error); return; }
    context.succeed(addon::encode_usage_scan_result(*page, version));
}

void handle_usage_repository_attribution(addon::Invocation_context& context)
{
    if (context.request().operation == addon::k_usage_repository_attribution_incremental_operation) {
        handle_usage_incremental(context);
    }
    else handle_usage(context);
}

std::optional<addon::Usage_session_fact> describe_usage_session(const addon::Usage_session_identity& identity)
{
    if (!identity.session_key) {
        return std::nullopt;
    }
    const std::string prefix = "codex:" + *identity.session_key + ":";
    if (!identity.record_id.starts_with(prefix) || identity.record_id.size() == prefix.size()) {
        return std::nullopt;
    }
    return addon::Usage_session_fact{*identity.session_key, true};
}

void handle_usage_session(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_session_describe_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_session_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    std::vector<addon::Usage_session_description> result;
    for (const auto& identity : *request) {
        result.push_back({identity, describe_usage_session(identity)});
    }
    context.succeed(addon::encode_usage_session_result(result));
}

addon::Legacy_import_result import_usage(const addon::Import_record_request& request)
{
    addon::Contract_failure failure;
    auto legacy = addon::decode_legacy_usage_records(request.record, &failure);
    if (!legacy || legacy->provider_id != "codex") return {};
    for (auto& record : legacy->records) {
        const auto separator = record.record_id.rfind(':');
        // The native store already keys each completed turn by this exact ID.
        // Migration changes ownership, never the identity or replay semantics.
        if (legacy->direction == "to-add-on" && !record.session_key && record.record_id.starts_with("codex:") &&
            separator > 6 && separator + 1 < record.record_id.size()) record.session_key = record.record_id.substr(6, separator - 6);
    }
    const auto conversion = addon::encode_legacy_usage_conversion(legacy->records);
    auto result = addon::Legacy_import_result::from_conversion(conversion);
    return result ? std::move(*result) : addon::Legacy_import_result{};
}

} // namespace codex
