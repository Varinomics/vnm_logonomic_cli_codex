#include "codex_usage.h"
#include "codex_accounts.h"
#include "codex_usage_records.h"
#include "codex_native_usage.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

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
constexpr std::chrono::milliseconds k_first_progress_interval{1000};
// The interval between progress frames doubles each time this many are sent.
constexpr std::uint64_t k_progress_frames_per_doubling = addon::limits::k_max_progress_frames_per_request / 16;
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
std::string encode_record(const Rollout_usage_record& record, std::uint16_t version = 2)
{
    auto envelope = addon::Json_value::object();
    auto usage = record.usage;
    if (version == 3) {
        usage.aggregate_evidence = "partial";
    }
    envelope.set("usage", addon::encode_usage_record(usage, version));
    envelope.set("recorded_cwd", addon::Json_value(record.recorded_cwd));
    return addon::serialize_json(envelope);
}

Rollout_usage_record decode_record(std::string_view line, std::uint16_t version = 2)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object(), "usage_cache_invalid");
    addon::Contract_failure failure;
    auto record = addon::decode_usage_record(member(*value, "usage"), &failure, version);
    require(record.has_value(), "usage_cache_invalid");
    return {std::move(*record), string_member(*value, "recorded_cwd")};
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
    if (member(metadata, "modified") == addon::Json_value(std::to_string(value.modified.time_since_epoch().count()))) return true;
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto modified = std::chrono::clock_cast<std::chrono::system_clock>(value.modified);
#else
    const auto modified = fs::file_time_type::clock::to_sys(value.modified);
#endif
    const auto native_msecs = std::chrono::duration_cast<std::chrono::milliseconds>(
        modified.time_since_epoch()).count();
    return member(metadata, "modified_native_msecs") == addon::Json_value(std::to_string(native_msecs));
}

struct Sort_record
{
    std::string id;
    std::string line;
};

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
    std::uint16_t version, const fs::path& spool)
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
    const bool current = metadata && member(*metadata, "revision") == addon::Json_value(k_parser_revision);
    if (!force && current && fingerprint_matches(*metadata, *before)) return result;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.incomplete = true;
        return result;
    }
    std::ofstream output(spool, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    std::string error;
    const bool parsed = read_rollout_usage(input, addon::path_from_utf8(source), [&](const Rollout_usage_record& record) {
        write_line(output, encode_record(record, version));
        return true;
    }, cancelled, &error);
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    const auto after = fingerprint(path);
    if (!after || *after != *before || error == "rollout_read_failed" ||
        (!parsed && metadata && !current && fingerprint_matches(*metadata, *before) && error != "cancelled"))
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
    next.set("modified", addon::Json_value(std::to_string(before->modified.time_since_epoch().count())));
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
    const auto refreshed = refresh_rollout(path, source, read_json(metadata_path), force, cancelled, version, pending);
    if (refreshed.incomplete) *incomplete = true;
    if (!refreshed.metadata) return;
    replace_file(pending, work / (key + ".records"));
    write_json(metadata_path, *refreshed.metadata);
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

// Converts a host-staged native cache once per seed digest. `cached` reports
// the metadata already held for a rollout key; `adopt` stores a converted
// rollout in the caller's cache layout.
void seed_native_cache(const addon::Usage_scan_request& request, const fs::path& storage,
    bool have_accepted, addon::Json_value* seeds,
    const std::function<std::optional<addon::Json_value>(const std::string& key)>& cached,
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
        const auto source = string_member(*wrapper, "path");
        const auto key = cache_key(source);
        const auto previous = cached(key);
        require(!previous || member(*previous, "native_seed") != addon::Json_value(descriptor.sha256_hex),
            "native_usage_cache_duplicate_path");
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

// A rollout's records share its working directory, so each run of records
// from one directory resolves its repository once, and the memo holds one
// directory however many a history has.
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
    const std::function<bool()>& cancelled, std::uint16_t version, bool unique_ids)
{
    std::ifstream left(left_path, std::ios::binary), right(right_path, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    require(left && right && output, "usage_storage_open_failed");
    Sort_record a, b;
    const auto next = [version](std::istream& input, Sort_record* record) {
        if (!read_line(input, &record->line)) return false;
        record->id = decode_record(record->line, version).usage.record_id;
        return true;
    };
    bool have_a = next(left, &a), have_b = next(right, &b);
    while (have_a || have_b) {
        check_cancel(cancelled);
        require(!unique_ids || !have_a || !have_b || a.id != b.id, "duplicate_usage_record");
        if (have_a && (!have_b || id_less(a.id, b.id))) { write_line(output, a.line); have_a = next(left, &a); }
        else { write_line(output, b.line); have_b = next(right, &b); }
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    left.close(); right.close();
    fs::remove(left_path); fs::remove(right_path);
}

// Sorts encoded records by ID while holding at most one run of them in
// memory: each full run is written sorted, and runs merge pairwise. Records
// sharing an ID are refused when `unique_ids` is set and kept adjacent
// otherwise.
class Record_sorter final
{
public:
    Record_sorter(fs::path work, std::string name, std::uint64_t run_bytes,
        const std::function<bool()>& cancelled, std::uint16_t version, bool unique_ids)
    :
        m_work(std::move(work)),
        m_name(std::move(name)),
        m_run_bytes(run_bytes),
        m_cancelled(cancelled),
        m_version(version),
        m_unique_ids(unique_ids)
    {}

    void add(Sort_record record)
    {
        if (!m_chunk.empty() && m_bytes + record.line.size() > m_run_bytes) flush();
        m_bytes += record.line.size();
        m_chunk.push_back(std::move(record));
    }

    // Moves every added record, sorted by ID, to `destination`.
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
                    merged, m_cancelled, m_version, m_unique_ids);
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
        std::sort(m_chunk.begin(), m_chunk.end(), [](const auto& a, const auto& b) { return id_less(a.id, b.id); });
        std::ofstream output(run_path(m_work, m_name, 0, m_runs++), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_write_failed");
        std::string previous;
        for (const auto& record : m_chunk) {
            require(!m_unique_ids || previous.empty() || previous != record.id, "duplicate_usage_record");
            write_line(output, record.line);
            previous = record.id;
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
    std::uint16_t                   m_version;
    bool                            m_unique_ids;
    std::vector<Sort_record>        m_chunk;
    std::size_t                     m_bytes = 0;
    std::uint64_t                   m_runs  = 0;
};

// The usage-scan@1/2 layout: sorts the records of every per-rollout file.
void sorted_snapshot(const fs::path& work, std::uint64_t sort_run_bytes, const std::function<bool()>& cancelled,
    std::uint16_t version = 2)
{
    Record_sorter sorter(work, "run", sort_run_bytes, cancelled, version, true);
    Repository_memo repositories;
    for (const auto& entry : fs::directory_iterator(work)) {
        check_cancel(cancelled);
        const auto filename = entry.path().filename().string();
        if (!filename.ends_with(".meta") || !digest_name(std::string_view(filename).substr(0, filename.size() - 5))) continue;
        const auto metadata = read_json(entry.path());
        require(metadata.has_value(), "usage_cache_invalid");
        const auto source = string_member(*metadata, "source");
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
            sorter.add({record.usage.record_id, encode_record(record, version)});
        }
    }
    sorter.finish(work / "records");
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
    std::uint16_t version = 2)
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
    const auto& incomplete = member(*metadata, "incomplete");
    require(incomplete.is_boolean(), "usage_snapshot_invalid");
    page.incomplete_historical_coverage = incomplete.as_boolean();
    const auto path = snapshot / "records";
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
    while (page.records.size() < addon::k_max_usage_page_records && read_line(input, &line)) {
        check_cancel(cancelled);
        if (!page.records.empty() && bytes + line.size() > k_line_limit / 2) break;
        bytes += line.size();
        page.records.push_back(decode_record(line, version).usage);
        offset += line.size() + 1;
    }
    page.complete = offset == size;
    if (!page.complete) page.next_cursor = std::to_string(offset);
    return page;
}

// Loads a selected cache's rollout metadata by cache key: the `rollouts`
// lines of the current format, or the `.meta` files of @1 and @2.
void load_accepted_rollouts(const addon::Usage_cache_manifest& accepted, const fs::path& source,
    std::map<std::string, addon::Json_value>* rollouts, const std::function<bool()>& cancelled)
{
    if (accepted.format == k_cache_format) {
        std::ifstream input(source / "rollouts", std::ios::binary);
        require(static_cast<bool>(input), "usage_cache_unavailable");
        std::string line;
        while (read_line(input, &line)) {
            check_cancel(cancelled);
            auto metadata = addon::parse_json(line, {}, nullptr);
            require(metadata && metadata->is_object(), "usage_cache_invalid");
            auto key = cache_key(string_member(*metadata, "source"));
            require(rollouts->emplace(std::move(key), std::move(*metadata)).second, "usage_cache_invalid");
        }
        return;
    }
    for (const auto& file : accepted.files) {
        check_cancel(cancelled);
        if (!file.path.ends_with(".meta") || !cache_file_name(file.path)) continue;
        auto metadata = read_json(source / file.path);
        require(metadata.has_value(), "usage_cache_unavailable");
        const auto key = file.path.substr(0, 64);
        require(cache_key(string_member(*metadata, "source")) == key, "usage_cache_identity_mismatch");
        rollouts->emplace(key, std::move(*metadata));
    }
}

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
    return encode_record(left, 3) == encode_record(right, 3);
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

// Writes the snapshot's `records`, reconciling every turn by its stable record
// ID. A turn parsed in this scan wins over one converted from a native seed,
// which wins over the accepted record, and a turn missing from a rollout parsed
// again keeps its accepted record. An observation that confirms an accepted
// turn leaves the accepted record whole: where its rollout lived, its
// repository attribution, completion date, provenance and category detail are
// facts of the accepted observation, which the host compares a partial
// observation against and rejects when they change. So a moved rollout keeps
// one observation under its accepted identity, a checkout removed or moved
// since acceptance cannot retract the accepted attribution, and a turn
// imported from the native cache keeps its native provenance when its rollout
// is parsed. A confirmed record without an event proof takes the
// observation's, so a turn accepted before its proof could be established
// gains it when its rollout is parsed again; the host adds a proof to an
// accepted record and keeps one an observation lacks. An observation that
// contradicts an accepted turn is presented for the host to accept or reject;
// it and every new turn carry the attribution their parse found. Differing
// observations of one turn within this scan fail it.
void merge_snapshot_records(const fs::path& work, const std::optional<fs::path>& accepted,
    std::uint16_t accepted_version, const std::map<std::string, addon::Json_value>& rollouts,
    const fs::path& seeded_path, const fs::path& parsed_path, const std::function<bool()>& cancelled)
{
    std::optional<Record_stream> retained;
    if (accepted) retained.emplace(*accepted, accepted_version, "usage_cache_unavailable");
    Record_stream seeded(seeded_path, 3, "usage_storage_read_failed");
    Record_stream parsed(parsed_path, 3, "usage_storage_read_failed");
    std::ofstream output(work / "records", std::ios::binary | std::ios::trunc);
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
            if (observation.usage.source_path.value_or("") < chosen->usage.source_path.value_or("")) {
                chosen = std::move(observation);
            }
        }
        return chosen;
    };
    std::string previous_accepted;
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
        if (retained && retained->head() && retained->head()->usage.record_id == *id) {
            accepted_record = retained->take();
            require(accepted_record->usage.source_path &&
                rollouts.contains(cache_key(*accepted_record->usage.source_path)), "usage_cache_identity_mismatch");
            require(previous_accepted.empty() || id_less(previous_accepted, *id), "usage_cache_invalid");
            previous_accepted = *id;
        }
        const bool parsed_again = parsed.head() && parsed.head()->usage.record_id == *id;
        auto seeded_choice = choose(seeded, *id, !parsed_again);
        auto parsed_choice = choose(parsed, *id, true);
        auto& chosen = parsed_again ? parsed_choice : seeded_choice;
        Rollout_usage_record* record = accepted_record ? &*accepted_record : nullptr;
        if (chosen) {
            if (!accepted_record || !confirms(*accepted_record, *chosen)) {
                record = &*chosen;
            }
            else
            if (!accepted_record->usage.cross_source_event) {
                accepted_record->usage.cross_source_event = chosen->usage.cross_source_event;
            }
        }
        write_line(output, encode_record(*record, 3));
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
}

void write_rollouts(const fs::path& work, const std::map<std::string, addon::Json_value>& rollouts,
    const std::function<bool()>& cancelled)
{
    std::ofstream output(work / "rollouts", std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    for (const auto& [key, metadata] : rollouts) {
        (void)key;
        check_cancel(cancelled);
        write_line(output, addon::serialize_json(metadata));
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
}

std::string selection_digest(addon::Usage_scan_request request)
{
    request.snapshot_id.reset();
    request.cursor.reset();
    return digest(addon::serialize_json_canonical(addon::encode_usage_scan_request(request, 3)));
}

addon::Usage_scan_page scan_selected_cache(
    const addon::Usage_scan_request& request,
    const fs::path& vendor_profile,
    const fs::path& storage,
    const std::function<bool()>& cancelled,
    Usage_scan_progress& progress,
    const Usage_scan_limits& limits)
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
    const auto request_hash = selection_digest(request);
    if (request.snapshot_id) {
        require(request.cursor.has_value(), "usage_cursor_invalid");
        const auto state = read_json(work / "cache-state");
        require(state && string_member(*state, "request_sha256") == request_hash &&
            string_member(*state, "snapshot_id") == *request.snapshot_id, "usage_snapshot_mismatch");
        auto page = page_at(work, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled, 3);
        page.account_context = request.context.account_context;
        page.profile_storage = request.context.profile_storage;
        require(fs::file_size(work / "cache-manifest.json") <= addon::k_max_usage_cache_manifest_bytes,
            "usage_cache_unavailable");
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
    // Rollout metadata by cache key; the accepted records stay on disk and are
    // streamed once into the new snapshot.
    std::map<std::string, addon::Json_value> rollouts;
    std::optional<fs::path> accepted_records;
    std::uint16_t accepted_version = 3;
    if (selection.accepted) {
        const auto accepted = addon::validate_usage_cache_candidate(
            *selection.accepted, profile_root, request.context.account_context, working, &failure);
        check_cancel(working);
        require(accepted && (accepted->format == "codex-rollout-cache@1" ||
            accepted->format == "codex-rollout-cache@2" || accepted->format == k_cache_format), "usage_cache_unavailable");
        const auto source = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root));
        const auto state = read_json(source / "cache-state");
        require(state.has_value(), "usage_cache_unavailable");
        seeds = member(*state, "native_seeds");
        require(seeds.is_array(), "usage_cache_unavailable");
        load_accepted_rollouts(*accepted, source, &rollouts, working);
        accepted_records = source / "records";
        accepted_version = accepted->format == "codex-rollout-cache@1" ? 2 : 3;
    }
    // Turns converted from a native seed and turns parsed in this scan, each
    // sorted by ID for reconciliation with the accepted records.
    Record_sorter seeded(work, "seeded-run", limits.sort_run_bytes, working, 3, false);
    Record_sorter parsed(work, "parsed-run", limits.sort_run_bytes, working, 3, false);
    if (request.native_file_cache) progress.begin("Importing earlier usage history");
    seed_native_cache(request, storage, selection.accepted.has_value(), &seeds,
        [&rollouts](const std::string& key) {
            const auto found = rollouts.find(key);
            return found == rollouts.end() ? std::nullopt : std::optional(found->second);
        },
        [&rollouts, &seeded, &progress](const std::string& key, addon::Json_value metadata, std::vector<Sort_record> records) {
            rollouts[key] = std::move(metadata);
            for (auto& record : records) {
                seeded.add(std::move(record));
            }
            progress.advance();
        }, working, 3);
    require(fs::is_directory(vendor_profile), "usage_sessions_unavailable");
    const auto sessions = vendor_profile / "sessions";
    const bool have_sessions = fs::exists(sessions);
    require(!have_sessions || fs::is_directory(sessions), "usage_sessions_not_directory");
    bool incomplete = false;
    // A parsed rollout's records wait here until its parse is known to hold.
    const auto spool = work / "rollout-records";
    progress.begin("Reading Codex sessions");
    fs::recursive_directory_iterator entries;
    if (have_sessions) {
        entries = fs::recursive_directory_iterator(sessions);
    }
    for (; entries != fs::recursive_directory_iterator{}; ++entries) {
        check_cancel(working);
        const auto& entry = *entries;
        if (hidden_entry(entry)) {
            entries.disable_recursion_pending();
            continue;
        }
        const auto name = addon::ascii_lowercase(entry.path().filename().string());
        if (!entry.is_regular_file() || !name.starts_with("rollout-") || !name.ends_with(".jsonl")) continue;
        const auto source = path_text(fs::absolute(entry.path()).lexically_normal());
        const auto key = cache_key(source);
        const auto cached = rollouts.find(key);
        auto refreshed = refresh_rollout(entry.path(), source,
            cached == rollouts.end() ? std::nullopt : std::optional(cached->second),
            request.mode != "incremental", working, 3, spool);
        progress.advance();
        if (refreshed.incomplete) incomplete = true;
        if (!refreshed.metadata) continue;
        rollouts[key] = std::move(*refreshed.metadata);
        std::ifstream spooled(spool, std::ios::binary);
        require(static_cast<bool>(spooled), "usage_storage_read_failed");
        std::string line;
        while (read_line(spooled, &line)) {
            check_cancel(working);
            auto id = decode_record(line, 3).usage.record_id;
            parsed.add({std::move(id), std::move(line)});
        }
        spooled.close();
        fs::remove(spool);
    }
    const auto seeded_path = work / "seeded-records";
    const auto parsed_path = work / "parsed-records";
    progress.begin("Sorting usage records");
    seeded.finish(seeded_path);
    parsed.finish(parsed_path);
    progress.begin("Merging usage history");
    merge_snapshot_records(work, accepted_records, accepted_version, rollouts, seeded_path, parsed_path, working);
    fs::remove(seeded_path);
    fs::remove(parsed_path);
    progress.begin("Saving the usage cache");
    write_rollouts(work, rollouts, working);
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
    auto page = page_at(work, identifier, 0, request.context.account_context, working, 3);
    page.account_context = request.context.account_context;
    page.profile_storage = request.context.profile_storage;
    // A parsed rollout need not contain all historical contributing events.
    // This scanner cannot certify lower aggregate corrections from parsing alone.
    for (auto& record : page.records) {
        record.aggregate_evidence = "partial";
    }
    page.cache_candidate = addon::seal_usage_cache_candidate(
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
    m_longest_interval(tolerated_silence / 4)
{}

void Usage_scan_progress::begin(std::string note)
{
    m_note      = std::move(note);
    m_completed = 0;
    emit();
}

void Usage_scan_progress::poll()
{
    const auto doublings = static_cast<int>(m_frames / k_progress_frames_per_doubling);
    const auto interval  = std::min(k_first_progress_interval * (std::int64_t{1} << doublings), m_longest_interval);
    if (m_clock() - m_last_frame >= interval) {
        emit();
    }
}

void Usage_scan_progress::emit()
{
    if (m_frames == addon::limits::k_max_progress_frames_per_request) {
        return;
    }
    ++m_frames;
    m_last_frame = m_clock();
    m_sink(m_completed, m_note);
}

std::optional<addon::Usage_scan_page> scan_usage(const addon::Usage_scan_request& request,
    const fs::path& vendor_profile, const fs::path& storage, const std::function<bool()>& cancelled,
    Usage_scan_progress& progress, std::string* error, const Usage_scan_limits& limits)
{
    try {
        std::unique_lock lock(scan_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        check_cancel(cancelled);
        if (request.cache_selection) {
            return scan_selected_cache(request, vendor_profile, storage, cancelled, progress, limits);
        }
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
        const auto sessions = vendor_profile / "sessions";
        const bool have_sessions = fs::exists(sessions);
        require(!have_sessions || fs::is_directory(sessions), "usage_sessions_not_directory");
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
            [&work](const std::string& key) { return read_json(work / (key + ".meta")); },
            [&work, &progress](const std::string& key, addon::Json_value metadata, std::vector<Sort_record> records) {
                write_rollout_records(work / (key + ".records"), records);
                write_json(work / (key + ".meta"), metadata);
                progress.advance();
            }, working);
        bool incomplete = false;
        progress.begin("Reading Codex sessions");
        fs::recursive_directory_iterator entries;
        if (have_sessions) entries = fs::recursive_directory_iterator(sessions);
        for (; entries != fs::recursive_directory_iterator{}; ++entries) {
            check_cancel(working);
            const auto& entry = *entries;
            if (hidden_entry(entry)) { entries.disable_recursion_pending(); continue; }
            const auto name = addon::ascii_lowercase(entry.path().filename().string());
            if (entry.is_regular_file() && name.starts_with("rollout-") && name.ends_with(".jsonl")) {
                refresh_file(entry.path(), work, request.mode != "incremental", &incomplete, working);
                progress.advance();
            }
        }
        progress.begin("Sorting usage records");
        sorted_snapshot(work, limits.sort_run_bytes, working);
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

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto version = context.request().capability.version;
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
        [&context] { return context.cancel_requested(); }, progress, &error);
    if (!page) { context.fail(error == "cancelled" ? addon::Result_status::CANCELLED : addon::Result_status::FAILED, error); return; }
    context.succeed(addon::encode_usage_scan_result(*page, version));
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
