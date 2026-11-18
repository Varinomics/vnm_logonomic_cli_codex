#include "codex_usage.h"
#include "codex_accounts.h"
#include "codex_jsonl_projector.h"
#include "codex_usage_records.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_repository_attribution.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
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
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

using addon::json::member;

constexpr std::size_t k_line_limit = 4 * 1024 * 1024;
constexpr int k_parser_revision = 2;
// A usage-scan@3 candidate keeps one metadata line per rollout in `rollouts`
// beside the snapshot's sorted `records`, so it holds four files however many
// rollouts a home has.
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
    // A sealed rollout keeps its source spelling even after the file or its
    // parent disappears. Native turn IDs reconcile copies and path aliases.
    require(!source.empty(), "usage_cache_invalid");
    return digest(source);
}

std::string nonce()
{
    static std::atomic<std::uint64_t> serial{0};
    std::random_device random;
    return digest(std::to_string(random()) + ":" + std::to_string(random()) + ":" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" + std::to_string(serial++));
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
    // Windows publishes the file with a write-through move. File contents
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

void write_json(const fs::path& path, const addon::Json_value& value)
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

std::string string_member(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    require(value.is_string(), "usage_cache_invalid");
    return value.as_string();
}

// Persist through the public codec so scanner caches cannot acquire a second
// interpretation of category coverage or repository allocation records.
addon::Json_value record_accounting(const Rollout_usage_record& record)
{
    auto envelope = addon::Json_value::object();
    auto usage = record.usage;
    usage.aggregate_evidence = "partial";
    envelope.set("usage", addon::encode_usage_record(usage, 3));
    envelope.set("recorded_cwd", addon::Json_value(record.recorded_cwd));
    return envelope;
}

std::string encode_record(const Rollout_usage_record& record,
    bool include_temporal = true, bool include_turn_context = false)
{
    auto envelope = record_accounting(record);
    const bool with_temporal = include_temporal && (record.temporal.instant || record.temporal.opaque ||
        record.temporal.reason != "missing-evidence");
    auto temporal = with_temporal
        ? encode_temporal_observation(record.temporal, envelope) : addon::Json_value{};
    // This flag travels only through a fresh parse's temporary spool. Accepted
    // caches never supply a witness for a later correction.
    if (include_turn_context && record.turn_cwd_witnessed) {
        envelope.set("turn_cwd_witnessed", addon::Json_value(true));
    }
    if (with_temporal) {
        envelope.set("temporal_observation", std::move(temporal));
        const auto encoded = addon::serialize_json(envelope);
        if (encoded.size() <= k_line_limit) return encoded;
        envelope.erase("temporal_observation");
    }
    return addon::serialize_json(envelope);
}

Rollout_usage_record decode_record(std::string_view line)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object(), "usage_cache_invalid");
    addon::Contract_failure failure;
    auto record = addon::decode_usage_record(member(*value, "usage"), &failure, 3);
    require(record.has_value(), "usage_cache_invalid");
    Rollout_usage_record result{std::move(*record), string_member(*value, "recorded_cwd")};
    result.turn_cwd_witnessed = member(*value, "turn_cwd_witnessed") == addon::Json_value(true);
    result.temporal = decode_temporal_observation(value->find("temporal_observation"),
        record_accounting(result));
    const auto& proof = result.usage.cross_source_event;
    if (result.temporal.instant && (!proof ||
        proof->authority_namespace != "openai.codex.completed-turn" ||
        result.usage.record_id != "codex:" + proof->event_id))
    {
        result.temporal.instant.reset();
        result.temporal.reason = "unsupported";
        result.temporal.opaque = *value->find("temporal_observation");
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
    return member(metadata, "size") == addon::Json_value(std::to_string(value.size)) &&
        member(metadata, "modified") == addon::Json_value(addon::file_time_ticks_text(value.modified));
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
    const fs::path& spool, bool refresh_missing_temporal = true)
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
    const bool refresh_temporal = !temporal_parser && refresh_missing_temporal;
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
        write_line(output, encode_record(record, true, true));
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
    next.set("native_session", session ? addon::Json_value(*session) : addon::Json_value{});
    auto marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(1));
    next.set("temporal_parser", temporal_parser ? *temporal_parser : std::move(marker));
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

bool id_less(std::string_view left, std::string_view right)
{
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](unsigned char a, unsigned char b) { return a < b; });
}

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

    // Unfinished peer runs are unnecessary when no rollout was reparsed.
    void discard()
    {
        m_chunk.clear();
        m_bytes = 0;
        for (std::uint64_t index = 0; index < m_runs; ++index) {
            check_cancel(m_cancelled);
            fs::remove(run_path(m_work, m_name, 0, index));
        }
        m_runs = 0;
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
        m_runs = 0;
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
Sort_key record_id_key()
{
    return [](const std::string& line) { return decode_record(line).usage.record_id; };
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
    const addon::Usage_scan_request& request, bool incremental = false)
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
    page.account_context = request.context.account_context;
    page.profile_storage = request.context.profile_storage;
    // Sealing replaces only this fixed-width digest. Measure the actual
    // candidate identity and context before admitting any page record.
    page.cache_candidate = addon::Usage_cache_reference{
        request.cache_selection->candidate_root, identifier, std::string(64, '0')};
    const auto& incomplete = member(*metadata, "incomplete");
    require(incomplete.is_boolean(), "usage_snapshot_invalid");
    page.incomplete_historical_coverage = incomplete.as_boolean();
    std::size_t envelope_bytes = 0;
    if (incremental) {
        addon::Usage_incremental_result envelope{*request.cache_selection->accepted, page,
            request.context.account_context, request.context.profile_storage};
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
        auto observation = decode_record(line);
        observation.usage.aggregate_evidence = "partial";
        apply_temporal_observation(observation.usage, observation.temporal,
            identifier, page.scope.source_account_key);
        const auto next = offset + line.size() + 1;
        page.complete = next == size;
        page.next_cursor = page.complete ? std::nullopt : std::optional(std::to_string(next));
        if (!addon::append_usage_page_record(page, observation.usage, encoded_records_bytes, 3, envelope_bytes)) {
            if (!page.records.empty()) break;
            if (observation.usage.temporal) {
                observation.usage.temporal->portions.clear();
                observation.usage.temporal->unavailable_tokens = observation.usage.total_tokens;
                observation.usage.temporal->coverage_reason = "capacity";
                if (!valid_temporal_record(observation.usage)) observation.usage.temporal.reset();
            }
            if (!addon::append_usage_page_record(page, observation.usage, encoded_records_bytes, 3, envelope_bytes)) {
                observation.usage.temporal.reset();
                require(addon::append_usage_page_record(page, observation.usage, encoded_records_bytes, 3, envelope_bytes),
                    "usage_record_frame_capacity");
            }
        }
        bytes += line.size();
        offset += line.size() + 1;
    }
    page.complete = offset == size;
    if (!page.complete) page.next_cursor = std::to_string(offset);
    else page.next_cursor.reset();
    return page;
}

// A selected cache's rollout metadata, streamed in cache-key order.
class Accepted_rollouts final
{
public:
    explicit Accepted_rollouts(const fs::path& file)
    :
        m_lines(file, std::ios::binary)
    {
        require(static_cast<bool>(m_lines), "usage_cache_unavailable");
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
        std::string line;
        if (!read_line(m_lines, &line)) {
            m_key.reset();
            m_metadata.reset();
            return;
        }
        auto metadata = addon::parse_json(line, {}, nullptr);
        require(metadata && metadata->is_object(), "usage_cache_invalid");
        auto key = cache_key(string_member(*metadata, "source"));
        require(!m_key || id_less(*m_key, key), "usage_cache_invalid");
        m_key      = std::move(key);
        m_metadata = std::move(metadata);
    }

    std::ifstream                   m_lines;
    std::optional<std::string>       m_key;
    std::optional<addon::Json_value> m_metadata;
};

void reorder_accepted_rollouts(const fs::path& source, const fs::path& destination,
    const Usage_scan_limits& limits, const std::function<bool()>& cancelled)
{
    const auto source_key = [](const std::string& line) {
        const auto metadata = addon::parse_json(line, {}, nullptr);
        require(metadata && metadata->is_object(), "usage_cache_invalid");
        return cache_key(string_member(*metadata, "source"));
    };
    Record_sorter sorter(destination.parent_path(), "accepted-source-run", limits.sort_run_bytes,
        cancelled, source_key, false);
    std::ifstream input(source, std::ios::binary);
    require(static_cast<bool>(input), "usage_cache_unavailable");
    std::string line;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        sorter.add({source_key(line), std::move(line)});
    }
    sorter.finish(destination);
}

// A sorted record file read one record at a time.
class Record_stream final
{
public:
    Record_stream(const fs::path& path, const char* unavailable)
    :
        m_input(path, std::ios::binary)
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
        if (read_line(m_input, &m_line)) m_head = decode_record(m_line);
        else m_head.reset();
    }

    std::ifstream                       m_input;
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
    return encode_record(left, false) == encode_record(right, false);
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

// Writes the snapshot's `records`, reconciling every turn by its stable record
// ID. A turn parsed in this scan is reconciled with the accepted record, and
// a turn missing from a rollout parsed again keeps its accepted record. An
// observation that confirms an accepted turn leaves the accepted accounting
// whole: where its rollout lived, its
// completion date, provenance and category detail are
// facts of the accepted observation, which the host compares a partial
// observation against and rejects when they change. So a moved rollout keeps
// one observation under its accepted identity, a checkout removed or moved
// since acceptance cannot retract the accepted attribution. A confirmed
// record without an event proof takes the
// observation's, so a turn accepted before its proof could be established
// gains it when its rollout is parsed again; the host adds a proof to an
// accepted record and keeps one an observation lacks. An observation that
// contradicts an accepted turn is presented for the host to accept or reject.
// A separately selected attribution operation may correct a confirmed turn's
// repository from matching live context. Numeric growth defers that correction
// and retains the old allocation, with its growth unknown. Differing
// observations of one turn within this scan fail it.
bool merge_snapshot_records(const fs::path& output_path, const std::optional<fs::path>& accepted,
    Record_sorter& accepted_rollout_keys,
    const fs::path& parsed_path, const std::function<bool()>& cancelled,
    Record_sorter* changes = nullptr, Usage_scan_authority authority = Usage_scan_authority::STANDARD)
{
    std::optional<Record_stream> retained;
    if (accepted) retained.emplace(*accepted, "usage_cache_unavailable");
    Record_stream parsed(parsed_path, "usage_storage_read_failed");
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    // Reads one turn's observations from a stream, keeping the one whose
    // rollout path sorts first; differing observations fail the scan.
    const auto choose = [&cancelled](Record_stream& stream, const std::string& id) {
        std::optional<Rollout_usage_record> chosen;
        while (stream.head() && stream.head()->usage.record_id == id) {
            check_cancel(cancelled);
            auto observation = stream.take();
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
        for (const auto* stream : {retained ? &*retained : nullptr, &parsed}) {
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
            if (changes) accepted_encoding = encode_record(*accepted_record);
        }
        const bool parsed_again = parsed.head() && parsed.head()->usage.record_id == *id;
        auto chosen = choose(parsed, *id);
        Rollout_usage_record* record = accepted_record ? &*accepted_record : nullptr;
        if (record) clear_repository_correction(record->usage);
        if (chosen) {
            // Earlier scanners committed this dimension as unsupported. The
            // host retains that coverage on partial cumulative observations,
            // including when a corrected total grows.
            if (accepted_record && accepted_record->usage.cache_creation.coverage &&
                accepted_record->usage.cache_creation.coverage->support == addon::Usage_support::UNSUPPORTED)
            {
                chosen->usage.cache_creation = accepted_record->usage.cache_creation;
            }
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
        auto encoded = encode_record(*record);
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
// the candidate's `rollouts` lists, whether from the accepted cache or a
// live rollout. Both files are sorted by cache key, so one
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
    const std::function<bool()>& cancelled, Record_sorter* accepted_keys = nullptr)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_cache_unavailable");
    std::map<std::string, std::ofstream> outputs;
    std::set<std::string> pieces;
    std::string line;
    std::string previous;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        const auto record = decode_record(line);
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
        write_line(output->second, line);
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
    Record_sorter sorted(work, "accepted-run", limits.sort_run_bytes, cancelled, record_id_key(), true);
    for (const auto& file : manifest.files) {
        if (!record_piece_file(file.path)) continue;
        std::ifstream input(source / file.path, std::ios::binary);
        require(static_cast<bool>(input), "usage_cache_unavailable");
        std::string line;
        while (read_line(input, &line)) {
            check_cancel(cancelled);
            auto id = decode_record(line).usage.record_id;
            require(record_piece_name(id) == file.path, "usage_cache_identity_mismatch");
            sorted.add({std::move(id), std::move(line)});
        }
    }
    sorted.finish(work / "accepted-records");
}

void merge_incremental_records(const addon::Usage_scan_request& request,
    const addon::Usage_cache_manifest& accepted, const fs::path& work,
    Record_sorter& accepted_keys, const fs::path& parsed_path,
    const Usage_scan_limits& limits, const std::function<bool()>& cancelled, Usage_scan_authority authority)
{
    const auto& baseline = *request.cache_selection->accepted;
    const auto source = addon::usage_cache_io_path(addon::path_from_utf8(baseline.root));
    const auto parsed = partition_records(parsed_path, work, "parsed-", cancelled);
    const auto& touched = parsed;
    std::set<std::string> retained;
    const bool adopting = accepted.format != k_piece_cache_format;
    if (adopting) {
        retained = partition_records(source / "records", work, "accepted-",
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
    Record_sorter changes(work, "changed-run", limits.sort_run_bytes, cancelled, record_id_key(), true);
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
                have_retained ? std::optional(accepted_path) : std::nullopt, accepted_keys,
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
        auto id = decode_record(line).usage.record_id;
        parsed.add({std::move(id), std::move(line)});
    }
    spooled.close();
    fs::remove(spool);
}

// Writes the snapshot's `rollouts` in cache-key order, reconciling by key the
// accepted metadata and the rollouts found in native history directories,
// and parses each found rollout whose metadata does not describe it, passing
// the records of every parse that holds to `parsed`. Rollouts found under
// one key are refreshed in the order they were found, each against the
// metadata the one before left. A key no source supplies metadata for gets no
// line. Returns whether a rollout could not be read at rest.
bool reconcile_rollouts(const fs::path& work, std::optional<Accepted_rollouts>& accepted,
    const fs::path& found_path, bool force, Record_sorter& parsed, Record_sorter& peers,
    Record_sorter& unindexed, Usage_scan_progress& progress, const std::function<bool()>& cancelled, bool* changed,
    bool* reparsed,
    bool refresh_missing_temporal)
{
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
        for (const auto* head : {accepted ? &accepted->key() : &none, &found.key()}) {
            if (*head && (!key || id_less(**head, *key))) key = *head;
        }
        if (!key) break;
        std::optional<addon::Json_value> metadata;
        if (accepted && accepted->key() == key) metadata = accepted->take();
        while (found.key() == key) {
            auto rollout = found.take();
            auto refreshed = refresh_rollout(addon::path_from_utf8(string_member(rollout, "path")),
                string_member(rollout, "source"), metadata, force, cancelled, spool, refresh_missing_temporal);
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
    const std::optional<addon::Usage_cache_manifest>& accepted,
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
                const auto record = decode_record(line);
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
            true, cancelled, spool);
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
            cancelled, request, incremental);
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
    // The accepted rollout metadata and records stay on disk and are streamed
    // once into the new snapshot.
    std::optional<Accepted_rollouts> accepted_rollouts;
    std::optional<fs::path> accepted_records;
    std::optional<addon::Usage_cache_manifest> accepted_manifest;
    bool metadata_only = false;
    bool accepted_incomplete = false;
    std::optional<fs::path> reordered_rollouts;
    if (selection.accepted) {
        accepted_manifest = incremental
            ? addon::validate_usage_cache_manifest(
                *selection.accepted, profile_root, request.context.account_context, working, &failure)
            : addon::validate_usage_cache_candidate(
                *selection.accepted, profile_root, request.context.account_context, working, &failure);
        check_cancel(working);
        const auto& accepted = accepted_manifest;
        require(accepted && (accepted->format == k_cache_format ||
            accepted->format == k_piece_cache_format), "usage_cache_unavailable");
        const auto source = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root));
        metadata_only = incremental;
        if (metadata_only) {
            for (const auto* name : {"cache-state", "snapshot", "rollouts"}) {
                if (!addon::validate_usage_cache_file(*selection.accepted, *accepted, name, working, &failure)) {
                    throw Scan_failure{failure.reason_code};
                }
            }
        }
        const auto snapshot = read_json(source / "snapshot");
        require(snapshot && member(*snapshot, "incomplete").is_boolean(), "usage_cache_unavailable");
        accepted_incomplete = member(*snapshot, "incomplete").as_boolean();
        const auto state = read_json(source / "cache-state");
        require(state.has_value(), "usage_cache_unavailable");
        const auto order = state->find("rollout_order");
        require(!order || *order == addon::Json_value("source-path"), "usage_cache_unavailable");
        auto rollouts = source / "rollouts";
        if (!order) {
            // Earlier metadata was ordered by the filesystem profile key.
            // Reindex the verified lines without reading the original files.
            reordered_rollouts = work / "accepted-source-order";
            reorder_accepted_rollouts(rollouts, *reordered_rollouts, limits, working);
            rollouts = *reordered_rollouts;
        }
        accepted_rollouts.emplace(rollouts);
        if (!incremental && accepted->format == k_piece_cache_format) {
            flatten_record_pieces(*accepted, source, work, limits, working);
            accepted_records = work / "accepted-records";
        }
        else {
            accepted_records = source / "records";
        }
    }
    // Current observations are sorted for reconciliation with accepted records.
    Record_sorter parsed(work, "parsed-run", limits.sort_run_bytes, working, record_id_key(), false);
    Record_sorter found_rollouts(work, "found-rollout-run", limits.sort_run_bytes, working, spool_key(), false);
    Record_sorter rollout_peers(work, "rollout-peer-run", limits.sort_run_bytes, working, spool_key(), false);
    Record_sorter unindexed_rollouts(work, "unindexed-run", limits.sort_run_bytes, working, spool_key(), false);
    std::uint64_t ordinal = 0;
    progress.begin("Reading Codex sessions");
    visit_rollouts(vendor_profile, working, [&](const fs::path& path) {
        const auto source = path_text(fs::absolute(path).lexically_normal());
        auto rollout = addon::Json_value::object();
        rollout.set("source", addon::Json_value(source));
        rollout.set("path", addon::Json_value(addon::path_to_utf8(path)));
        found_rollouts.add(spool_record(cache_key(source), ordinal++, rollout));
    });
    const auto found_rollouts_path = work / "found-rollouts";
    found_rollouts.finish(found_rollouts_path);
    bool metadata_changed = false;
    bool reparsed = false;
    bool incomplete = reconcile_rollouts(work, accepted_rollouts, found_rollouts_path,
        request.mode != "incremental", parsed, rollout_peers, unindexed_rollouts,
        progress, working, &metadata_changed, &reparsed, !incremental);
    if (reordered_rollouts) {
        accepted_rollouts.reset();
        fs::remove(*reordered_rollouts);
    }
    fs::remove(found_rollouts_path);
    if (reparsed) {
        const auto unindexed_path = work / "unindexed-rollouts";
        unindexed_rollouts.finish(unindexed_path);
        if (fs::file_size(unindexed_path) != 0 &&
            index_rollout_copies(request, accepted_manifest, work,
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
    }
    else {
        unindexed_rollouts.discard();
        rollout_peers.discard();
    }
    // Incremental source observations do not prove that earlier coverage gaps
    // were resolved. A repair or reset verifies current sources in full.
    if (request.mode == "incremental" && accepted_incomplete) {
        incomplete = true;
    }
    if (metadata_only && !reordered_rollouts && !metadata_changed && incomplete == accepted_incomplete) {
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
    const auto parsed_path = work / "parsed-records";
    const auto accepted_keys_path = work / "accepted-rollout-keys";
    progress.begin("Sorting usage records");
    parsed.finish(parsed_path);
    progress.begin("Merging usage history");
    Record_sorter accepted_keys(work, "key-run", limits.sort_run_bytes, working,
        [](const std::string& line) { return line; }, false);
    if (incremental) {
        merge_incremental_records(request, *accepted_manifest, work, accepted_keys,
            parsed_path, limits, working, authority);
    }
    else {
        merge_snapshot_records(work / "records", accepted_records,
            accepted_keys, parsed_path, working, nullptr, authority);
        if (accepted_manifest && accepted_manifest->format == k_piece_cache_format) fs::remove(*accepted_records);
    }
    accepted_keys.finish(accepted_keys_path);
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
    state.set("rollout_order", addon::Json_value("source-path"));
    write_json(work / "cache-state", state);
    auto page = page_at(work, identifier, 0, request.context.account_context, working, request, incremental);
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
        // cannot change or prune them. Keep writers serialized without
        // delaying delivery of an already completed scan.
        if (!request.snapshot_id) {
            while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        }
        check_cancel(cancelled);
        return scan_selected_cache(request, vendor_profile, storage, cancelled, progress, limits, false, authority);
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
        require(request.mode == "incremental" && request.cache_selection && request.cache_selection->accepted,
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
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure, 3);
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
    context.succeed(addon::encode_usage_scan_result(*page, 3));
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

} // namespace codex
