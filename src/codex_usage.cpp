#include "codex_usage.h"
#include "codex_accounts.h"
#include "codex_usage_records.h"
#include "codex_native_usage.h"

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
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::size_t k_line_limit = 4 * 1024 * 1024;
constexpr std::size_t k_sort_bytes = 4 * 1024 * 1024;
constexpr std::size_t k_sort_records = 128;
constexpr int k_parser_revision = 1;
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

struct Disk_usage
{
    std::uint64_t bytes = 0;
    std::uint64_t entries = 0;
    std::uint64_t lines = 0;
};

Disk_usage path_usage(const fs::path& path, const std::function<bool()>& cancelled, const Usage_scan_limits* limits = nullptr)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (!fs::exists(status)) return {};
    require(!error && !fs::is_symlink(status), "usage_storage_invalid");
    Disk_usage usage{0, 1, 0};
    if (fs::is_directory(status)) return usage;
    require(fs::is_regular_file(status), "usage_storage_invalid");
    usage.bytes = fs::file_size(path);
    if (limits) require(usage.bytes <= limits->disk_bytes, "usage_disk_budget_exceeded");
    if (usage.bytes == 0) return usage; // Includes the exclusively held storage lock.
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    std::array<char, 65536> bytes{};
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        usage.lines += static_cast<std::uint64_t>(std::count(bytes.begin(), bytes.begin() + input.gcount(), '\n'));
        if (limits) require(usage.lines <= limits->persisted_lines, "usage_disk_budget_exceeded");
    }
    require(!input.bad(), "usage_storage_read_failed");
    return usage;
}

class Disk_budget final
{
public:
    Disk_budget(const fs::path& root, const Usage_scan_limits& limits, const std::function<bool()>& cancelled)
    : m_limits(limits), m_cancelled(cancelled)
    {
        reserve(path_usage(root, cancelled, &limits));
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            check_cancel(cancelled);
            reserve(path_usage(entry.path(), cancelled, &limits));
        }
    }
    void reserve(const Disk_usage& usage)
    {
        require(usage.bytes <= m_limits.disk_bytes - std::min(m_usage.bytes, m_limits.disk_bytes) &&
            usage.entries <= m_limits.filesystem_entries - std::min(m_usage.entries, m_limits.filesystem_entries) &&
            usage.lines <= m_limits.persisted_lines - std::min(m_usage.lines, m_limits.persisted_lines), "usage_disk_budget_exceeded");
        m_usage.bytes += usage.bytes; m_usage.entries += usage.entries; m_usage.lines += usage.lines;
    }
    void release(const Disk_usage& usage)
    {
        require(usage.bytes <= m_usage.bytes && usage.entries <= m_usage.entries && usage.lines <= m_usage.lines, "usage_budget_accounting_invalid");
        m_usage.bytes -= usage.bytes; m_usage.entries -= usage.entries; m_usage.lines -= usage.lines;
    }
    Disk_usage inspect(const fs::path& path) const { return path_usage(path, m_cancelled); }
    void prepare_file(const fs::path& path)
    {
        const auto old = inspect(path);
        if (old.entries == 0) reserve({0, 1, 0});
        else release({old.bytes, 0, old.lines});
    }
    const Usage_scan_limits& limits() const { return m_limits; }
private:
    const Usage_scan_limits& m_limits;
    const std::function<bool()>& m_cancelled;
    Disk_usage m_usage;
};

thread_local Disk_budget* active_budget = nullptr;

class Budget_scope final
{
public:
    explicit Budget_scope(Disk_budget& budget) : m_previous(active_budget) { active_budget = &budget; }
    ~Budget_scope() { active_budget = m_previous; }
private:
    Disk_budget* m_previous;
};

class Budget_output final : public std::ofstream
{
public:
    Budget_output(const fs::path& path, std::ios::openmode mode)
    {
        if (active_budget) active_budget->prepare_file(path);
        open(path, mode);
    }
};

void remove_owned_path(const fs::path& path)
{
    const auto usage = active_budget ? active_budget->inspect(path) : Disk_usage{};
    if (fs::remove(path) && active_budget) active_budget->release(usage);
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
        if (active_budget) active_budget->reserve({0, 1, 0});
        fs::create_directory(path, error);
        require(!error, "usage_storage_create_failed");
    }
    require(fs::is_directory(fs::symlink_status(path, error)) && !error, "usage_storage_invalid");
    return path;
}

void remove_snapshot(const fs::path& path)
{
    require(digest_name(path.filename().string()), "usage_snapshot_invalid");
    require(fs::is_directory(fs::symlink_status(path)), "usage_storage_invalid");
    // All entries written here are files. Refuse foreign directories instead
    // of following or recursively removing anything in an unexpected shape.
    for (const auto& entry : fs::directory_iterator(path)) {
        require(!fs::is_directory(entry.symlink_status()), "usage_storage_invalid");
        remove_owned_path(entry.path());
    }
    remove_owned_path(path);
}

class Pending_snapshot final
{
public:
    explicit Pending_snapshot(fs::path path) : m_path(std::move(path)) {}
    ~Pending_snapshot()
    {
        if (!m_committed) {
            const auto budget = active_budget;
            active_budget = nullptr; // Cleanup only removes files; cancellation must not inhibit it.
            try { remove_snapshot(m_path); } catch (...) { /* The next scan retries owned scratch cleanup. */ }
            active_budget = budget;
        }
    }
    void commit() { m_committed = true; }
private:
    fs::path m_path;
    bool m_committed = false;
};

void prune_snapshots(const fs::path& snapshots, std::string_view current)
{
    for (const auto& entry : fs::directory_iterator(snapshots)) {
        const auto name = entry.path().filename().string();
        if (digest_name(name) && name != current) {
            remove_snapshot(entry.path());
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
    if (active_budget) active_budget->reserve({static_cast<std::uint64_t>(line.size()) + 1, 0, 1});
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
    const auto replaced = active_budget ? active_budget->inspect(destination) : Disk_usage{};
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
    if (active_budget) active_budget->release(replaced);
}

void write_json(const fs::path& path, const addon::Json_value& value, Pending_snapshot* generation = nullptr)
{
    const auto encoded = addon::serialize_json(value);
    require(encoded.size() <= k_line_limit, "usage_cache_record_too_large");
    const auto temporary = path.parent_path() / (path.filename().string() + ".pending");
    require(!fs::is_symlink(fs::symlink_status(temporary)), "usage_storage_link");
    Budget_output output(temporary, std::ios::binary | std::ios::trunc);
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
std::string encode_record(const Rollout_usage_record& record)
{
    auto envelope = addon::Json_value::object();
    envelope.set("usage", addon::encode_usage_record(record.usage, 2));
    envelope.set("recorded_cwd", addon::Json_value(record.recorded_cwd));
    return addon::serialize_json(envelope);
}

Rollout_usage_record decode_record(std::string_view line)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object(), "usage_cache_invalid");
    addon::Contract_failure failure;
    auto record = addon::decode_usage_record(member(*value, "usage"), &failure, 2);
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

void refresh_file(const fs::path& path, const fs::path& work,
    bool force, bool* incomplete, const std::function<bool()>& cancelled)
{
    check_cancel(cancelled);
    const auto source = path_text(fs::absolute(path).lexically_normal());
    const auto key = cache_key(source);
    const auto metadata_path = work / (key + ".meta");
    const auto before = fingerprint(path);
    const auto metadata = read_json(metadata_path);
    if (!before) { *incomplete = true; return; }
    // A writer may have created or truncated the file before populating it.
    // Preserve prior evidence and do not manufacture an empty cache entry.
    if (before->size == 0) { *incomplete = true; return; }
    const bool current = metadata && member(*metadata, "revision") == addon::Json_value(k_parser_revision);
    if (!force && current && fingerprint_matches(*metadata, *before)) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) { *incomplete = true; return; }
    const auto pending = work / (key + ".pending-records");
    Budget_output output(pending, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    std::string error;
    std::uint64_t file_records = 0;
    std::uint64_t file_bytes = 0;
    const bool parsed = read_rollout_usage(input, addon::path_from_utf8(source), [&](const Rollout_usage_record& record) {
        const auto encoded = encode_record(record);
        require(file_records < active_budget->limits().candidate_records && encoded.size() + 1 <=
            active_budget->limits().candidate_bytes - std::min(file_bytes, active_budget->limits().candidate_bytes),
            "usage_candidate_budget_exceeded");
        ++file_records; file_bytes += encoded.size() + 1;
        write_line(output, encoded);
        return true;
    }, cancelled, &error);
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    const auto after = fingerprint(path);
    if (!after || *after != *before || error == "rollout_read_failed") {
        remove_owned_path(pending);
        *incomplete = true;
        return;
    }
    if (!parsed && metadata && !current && fingerprint_matches(*metadata, *before) && error != "cancelled") {
        remove_owned_path(pending);
        *incomplete = true;
        return;
    }
    require(parsed, error.empty() ? "rollout_invalid" : error);
    check_cancel(cancelled);
    replace_file(pending, work / (key + ".records"));
    auto next = addon::Json_value::object();
    next.set("revision", addon::Json_value(k_parser_revision));
    next.set("source", addon::Json_value(source));
    next.set("size", addon::Json_value(std::to_string(before->size)));
    next.set("modified", addon::Json_value(std::to_string(before->modified.time_since_epoch().count())));
    write_json(metadata_path, next);
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
        Budget_output output(work / name, std::ios::binary | std::ios::trunc);
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
    Budget_output inventory(work / "inventory", std::ios::binary | std::ios::trunc);
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

void seed_native_cache(const addon::Usage_scan_request& request, const fs::path& storage,
    const fs::path& work, bool have_accepted, addon::Json_value* seeds,
    const std::function<bool()>& cancelled)
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
    const auto input_bytes = fs::file_size(path);
    require(active_budget && input_bytes <= active_budget->limits().candidate_bytes, "usage_disk_budget_exceeded");
    active_budget->reserve({input_bytes, 1, 0});
    struct Input_reservation {
        std::uint64_t bytes;
        ~Input_reservation() { active_budget->release({bytes, 1, 0}); }
    } input_reservation{input_bytes};
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
    std::uint64_t imported_records = 0;
    std::uint64_t imported_bytes = 0;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        checked_hash.update(line); checked_hash.update("\n");
        const auto wrapper = addon::parse_json(line, {}, nullptr);
        require(wrapper && wrapper->is_object(), "native_usage_cache_invalid");
        const auto source = string_member(*wrapper, "path");
        const auto key = cache_key(source);
        const auto previous = read_json(work / (key + ".meta"));
        require(!previous || member(*previous, "native_seed") != addon::Json_value(descriptor.sha256_hex),
            "native_usage_cache_duplicate_path");
        Budget_output output(work / (key + ".records"), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_write_failed");
        std::string error;
        const auto file = convert_native_cache_file(*wrapper, [&](const Rollout_usage_record& record) {
            check_cancel(cancelled);
            const auto encoded = encode_record(record);
            require(imported_records < active_budget->limits().candidate_records && encoded.size() + 1 <=
                active_budget->limits().candidate_bytes - std::min(imported_bytes, active_budget->limits().candidate_bytes),
                "usage_candidate_budget_exceeded");
            ++imported_records; imported_bytes += encoded.size() + 1;
            write_line(output, encoded);
            return true;
        }, &error);
        require(file.has_value(), error);
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
        auto metadata = addon::Json_value::object();
        metadata.set("native_seed", addon::Json_value(descriptor.sha256_hex));
        metadata.set("revision", addon::Json_value(file->parser_current ? k_parser_revision : 0));
        metadata.set("source", addon::Json_value(file->path));
        metadata.set("size", addon::Json_value(std::to_string(file->size)));
        metadata.set("modified_native_msecs", addon::Json_value(std::to_string(file->modified_msecs)));
        write_json(work / (key + ".meta"), metadata);
    }
    require(addon::to_hex(checked_hash.finish()) == descriptor.sha256_hex, "native_usage_cache_digest_mismatch");
    check_cancel(cancelled);
    seeds->push_back(addon::Json_value(descriptor.sha256_hex));
}

struct Sort_record
{
    std::string id;
    std::string line;
};

bool id_less(std::string_view left, std::string_view right)
{
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](unsigned char a, unsigned char b) { return a < b; });
}

fs::path run_path(const fs::path& work, std::uint64_t level, std::uint64_t index)
{
    return work / ("run-" + std::to_string(level) + "-" + std::to_string(index));
}

void merge_runs(const fs::path& left_path, const fs::path& right_path, const fs::path& destination,
    const std::function<bool()>& cancelled)
{
    std::ifstream left(left_path, std::ios::binary), right(right_path, std::ios::binary);
    Budget_output output(destination, std::ios::binary | std::ios::trunc);
    require(left && right && output, "usage_storage_open_failed");
    Sort_record a, b;
    const auto next = [](std::istream& input, Sort_record* record) {
        if (!read_line(input, &record->line)) return false;
        record->id = decode_record(record->line).usage.record_id;
        return true;
    };
    bool have_a = next(left, &a), have_b = next(right, &b);
    while (have_a || have_b) {
        check_cancel(cancelled);
        require(!have_a || !have_b || a.id != b.id, "duplicate_usage_record");
        if (have_a && (!have_b || id_less(a.id, b.id))) { write_line(output, a.line); have_a = next(left, &a); }
        else { write_line(output, b.line); have_b = next(right, &b); }
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    left.close(); right.close();
    remove_owned_path(left_path); remove_owned_path(right_path);
}

void sorted_snapshot(const fs::path& work, const std::function<bool()>& cancelled)
{
    std::vector<Sort_record> chunk;
    std::size_t bytes = 0;
    std::uint64_t runs = 0;
    std::uint64_t candidate_records = 0;
    std::uint64_t candidate_bytes = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        std::sort(chunk.begin(), chunk.end(), [](const auto& a, const auto& b) { return id_less(a.id, b.id); });
        Budget_output output(run_path(work, 0, runs++), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_write_failed");
        std::string previous;
        for (const auto& record : chunk) {
            require(previous.empty() || previous != record.id, "duplicate_usage_record");
            write_line(output, record.line);
            previous = record.id;
        }
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
        chunk.clear(); bytes = 0;
    };
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
            auto record = decode_record(line);
            require(record.usage.source_path && cache_key(*record.usage.source_path) == cache_key(source), "usage_cache_identity_mismatch");
            refresh_repository(&record);
            auto encoded = encode_record(record);
            require(candidate_records < active_budget->limits().candidate_records && encoded.size() + 1 <=
                active_budget->limits().candidate_bytes - std::min(candidate_bytes, active_budget->limits().candidate_bytes),
                "usage_candidate_budget_exceeded");
            ++candidate_records; candidate_bytes += encoded.size() + 1;
            if (!chunk.empty() && (chunk.size() >= k_sort_records || bytes + encoded.size() > k_sort_bytes)) flush();
            bytes += encoded.size();
            chunk.push_back({record.usage.record_id, std::move(encoded)});
        }
    }
    flush();
    std::uint64_t level = 0;
    while (runs > 1) {
        const auto next_runs = (runs + 1) / 2;
        for (std::uint64_t index = 0; index < next_runs; ++index) {
            check_cancel(cancelled);
            const auto destination = run_path(work, level + 1, index);
            if (index * 2 + 1 == runs) fs::rename(run_path(work, level, index * 2), destination);
            else merge_runs(run_path(work, level, index * 2), run_path(work, level, index * 2 + 1), destination, cancelled);
        }
        ++level; runs = next_runs;
    }
    if (runs == 1) fs::rename(run_path(work, level, 0), work / "records");
    else { Budget_output empty(work / "records", std::ios::binary); require(static_cast<bool>(empty), "usage_storage_write_failed"); }
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
    std::uint64_t offset, const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled)
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
        page.records.push_back(decode_record(line).usage);
        offset += line.size() + 1;
    }
    page.complete = offset == size;
    if (!page.complete) page.next_cursor = std::to_string(offset);
    return page;
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
    const Usage_scan_limits& limits)
{
    addon::Contract_failure failure;
    if (!addon::validate_usage_cache_selection(request, &failure)) {
        throw Scan_failure{failure.reason_code};
    }
    const auto& selection = *request.cache_selection;
    const auto work = addon::path_from_utf8(selection.candidate_root);
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
        auto page = page_at(work, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled);
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
    Disk_budget budget(work, limits, cancelled);
    Budget_scope budget_scope(budget);
    auto seeds = addon::Json_value::array();
    if (selection.accepted) {
        const auto accepted = addon::validate_usage_cache_candidate(
            *selection.accepted, profile_root, request.context.account_context, cancelled, &failure);
        check_cancel(cancelled);
        require(accepted && accepted->format == "codex-rollout-cache@1", "usage_cache_unavailable");
        const auto source = addon::path_from_utf8(selection.accepted->root);
        const auto state = read_json(source / "cache-state");
        require(state.has_value(), "usage_cache_unavailable");
        seeds = member(*state, "native_seeds");
        require(seeds.is_array(), "usage_cache_unavailable");
        for (const auto& file : accepted->files) {
            if (!cache_file_name(file.path)) {
                continue;
            }
            std::ifstream input(source / file.path, std::ios::binary);
            Budget_output output(work / file.path, std::ios::binary);
            require(input && output, "usage_cache_unavailable");
            std::string line;
            while (read_line(input, &line)) {
                check_cancel(cancelled);
                write_line(output, line);
            }
            output.close();
            require(output.good(), "usage_storage_write_failed");
        }
    }
    seed_native_cache(request, storage, work, selection.accepted.has_value(), &seeds, cancelled);
    require(fs::is_directory(vendor_profile), "usage_sessions_unavailable");
    const auto sessions = vendor_profile / "sessions";
    const bool have_sessions = fs::exists(sessions);
    require(!have_sessions || fs::is_directory(sessions), "usage_sessions_not_directory");
    bool incomplete = false;
    fs::recursive_directory_iterator entries;
    if (have_sessions) {
        entries = fs::recursive_directory_iterator(sessions);
    }
    for (; entries != fs::recursive_directory_iterator{}; ++entries) {
        check_cancel(cancelled);
        const auto& entry = *entries;
        if (hidden_entry(entry)) {
            entries.disable_recursion_pending();
            continue;
        }
        const auto name = addon::ascii_lowercase(entry.path().filename().string());
        if (entry.is_regular_file() && name.starts_with("rollout-") && name.ends_with(".jsonl")) {
            refresh_file(entry.path(), work, request.mode != "incremental", &incomplete, cancelled);
        }
    }
    sorted_snapshot(work, cancelled);
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
    auto page = page_at(work, identifier, 0, request.context.account_context, cancelled);
    // A parsed rollout need not contain all historical contributing events.
    // This scanner cannot certify lower aggregate corrections from parsing alone.
    for (auto& record : page.records) {
        record.aggregate_evidence = "partial";
    }
    page.cache_candidate = addon::seal_usage_cache_candidate(
        selection.candidate_root, profile_root, request.context.account_context,
        identifier, "codex-rollout-cache@1", cancelled, &failure);
    if (!page.cache_candidate) {
        throw Scan_failure{failure.reason_code};
    }
    require(addon::decode_usage_scan_result(addon::encode_usage_scan_result(page, 3), &failure, 3).has_value(),
        "usage_snapshot_invalid");
    return page;
}

} // namespace

std::optional<addon::Usage_scan_page> scan_usage(const addon::Usage_scan_request& request,
    const fs::path& vendor_profile, const fs::path& storage, const std::function<bool()>& cancelled,
    std::string* error, const Usage_scan_limits& limits)
{
    try {
        std::unique_lock lock(scan_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        check_cancel(cancelled);
        if (request.cache_selection) {
            return scan_selected_cache(request, vendor_profile, storage, cancelled, limits);
        }
        // Bootstrap creates only these three zero-byte entries before inventory.
        require(limits.filesystem_entries >= 3, "usage_disk_budget_exceeded");
        const auto root = directory(storage, "codex-usage-v1");
        Storage_lock storage_lock(root / "scan.lock", cancelled);
        const auto snapshots = directory(root, "snapshots");
        if (request.snapshot_id) {
            require(digest_name(*request.snapshot_id) && request.cursor.has_value(), "usage_cursor_invalid");
            return page_at(snapshots / *request.snapshot_id, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled);
        }
        require(!request.cursor, "usage_cursor_invalid");
        Disk_budget budget(root, limits, cancelled);
        Budget_scope budget_scope(budget);
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
        prune_snapshots(snapshots, accepted_identifier);
        const auto identifier = nonce();
        const auto work = directory(snapshots, identifier);
        Pending_snapshot pending(work);
        if (accepted) {
            copy_accepted_cache(*accepted, snapshots, work, cancelled);
        }
        seed_native_cache(request, storage, work, accepted.has_value(), &seeds, cancelled);
        bool incomplete = false;
        fs::recursive_directory_iterator entries;
        if (have_sessions) entries = fs::recursive_directory_iterator(sessions);
        for (; entries != fs::recursive_directory_iterator{}; ++entries) {
            check_cancel(cancelled);
            const auto& entry = *entries;
            if (hidden_entry(entry)) { entries.disable_recursion_pending(); continue; }
            const auto name = addon::ascii_lowercase(entry.path().filename().string());
            if (entry.is_regular_file() && name.starts_with("rollout-") && name.ends_with(".jsonl")) {
                refresh_file(entry.path(), work, request.mode != "incremental", &incomplete, cancelled);
            }
        }
        sorted_snapshot(work, cancelled);
        check_cancel(cancelled);
        addon::Observation_scope scope;
        if (request.context.account_context) scope.source_account_key = request.context.account_context->adapter_account_key;
        scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        auto metadata = addon::Json_value::object();
        metadata.set("scope", addon::encode_observation_scope(scope));
        metadata.set("incomplete", addon::Json_value(incomplete));
        write_json(work / "snapshot", metadata);
        auto page = page_at(work, identifier, 0, request.context.account_context, cancelled);
        auto next = accepted_generation(work, seeds, request.context.account_context, cancelled);
        // Include response construction in the candidate: encoding can still fail
        // before the one pointer exposes this generation and its seed receipts.
        const auto encoded = addon::encode_usage_scan_result(page, 2);
        addon::Contract_failure failure;
        require(addon::decode_usage_scan_result(encoded, &failure, 2).has_value(), "usage_snapshot_invalid");
        sync_path(root, true);
        sync_path(storage, true);
        check_cancel(cancelled);
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
    const auto page = scan_usage(*request, addon::path_from_utf8(profile->profile_path), addon::path_from_utf8(request->context.profile_storage->root),
        [&context] { return context.cancel_requested(); }, &error);
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
