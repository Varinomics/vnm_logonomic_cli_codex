#include "codex_jsonl_projector.h"
#include "codex_usage.h"
#include "codex_usage_records.h"
#include "codex_native_usage.h"
#include "codex_accounts.h"
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Scans whose progress a test does not examine report into this.
codex::Usage_scan_progress quiet_progress([](std::uint64_t, const std::string&) {}, std::chrono::minutes(15));

// The notes of consecutive frames, each stage named once.
std::vector<std::string> stage_notes(const std::vector<std::string>& notes)
{
    std::vector<std::string> stages;
    for (const auto& note : notes) {
        if (stages.empty() || stages.back() != note) stages.push_back(note);
    }
    return stages;
}

struct Temporary_directory
{
    fs::path path = fs::temp_directory_path() / ("codex-usage-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary_directory() { check(fs::create_directory(path), "create isolated fixture"); }
    ~Temporary_directory() { std::error_code error; fs::remove_all(path, error); }
};

std::string metadata(std::string_view id = "rollout", bool forked = false)
{
    return "{\"type\":\"session_meta\",\"payload\":{\"id\":\"" + std::string(id) +
        "\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"cwd\":\"\"" +
        (forked ? ",\"forked_from_id\":\"parent\"" : "") + "}}\n";
}

std::string metadata_with_cwd(std::string_view id, const fs::path& cwd)
{
    auto payload = addon::Json_value::object();
    payload.set("id", addon::Json_value(std::string(id)));
    payload.set("timestamp", addon::Json_value("2026-01-01T00:00:00Z"));
    payload.set("cwd", addon::Json_value(addon::path_to_utf8(cwd)));
    auto event = addon::Json_value::object();
    event.set("type", addon::Json_value("session_meta"));
    event.set("payload", std::move(payload));
    return addon::serialize_json(event) + '\n';
}

std::string start(std::string_view turn, int seconds = 1767225601)
{
    return "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"" +
        std::string(turn) + "\",\"started_at\":" + std::to_string(seconds) + "}}\n";
}

std::string tokens(int total, std::string_view categories = {})
{
    return "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"total_tokens\":" +
        std::to_string(total) + std::string(categories) + "}}}}\n";
}

std::string complete(std::string_view turn)
{
    return "{\"type\":\"event_msg\",\"timestamp\":\"2026-01-02T12:00:00Z\",\"payload\":{"
        "\"type\":\"task_complete\",\"turn_id\":\"" + std::string(turn) + "\"}}\n";
}

std::vector<codex::Rollout_usage_record> reduce(const std::string& bytes, const fs::path& path)
{
    std::istringstream input(bytes);
    std::vector<codex::Rollout_usage_record> result;
    std::string error;
    check(codex::read_rollout_usage(input, path, [&](const auto& record) { result.push_back(record); return true; },
        [] { return false; }, &error), error.c_str());
    return result;
}

void projector_does_not_retain_ignored_large_payloads()
{
    unsigned records = 0;
    std::string error;
    codex::Codex_jsonl_projector projector([&](const auto&) { ++records; return true; });
    check(projector.feed("{\"type\":\"response_item\",\"payload\":{\"content\":\"", &error), "start ignored content");
    const std::string chunk(65536, 'x');
    for (unsigned index = 0; index < 192; ++index) {
        check(projector.feed(chunk, &error), "stream ignored content");
        check(projector.retained_bytes_for_tests() < 65536, "ignored payload retention must stay bounded");
    }
    check(projector.feed("\"}}\n", &error), "complete large JSON line");
    check(records == 1, "one complete ignored record");
    check(projector.feed("{\"type\":\"unfinished", &error), "partial final line accepted for later append");
    projector.finish_file();
    check(records == 1, "partial line cannot emit a record");
    codex::Codex_jsonl_projector invalid([](const auto&) { return true; });
    check(!invalid.feed("{\"ignored\":[1,]}\n", &error), "ignored structure still validates malformed JSON");
    codex::Codex_jsonl_projector oversized([](const auto&) { return true; });
    check(!oversized.feed("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"total_tokens\":" +
        std::string(600, '9') + "}}}}\n", &error), "oversized selected number fails rather than being silently omitted as an oversized key");
}

void reducer_keeps_turn_deltas_resets_and_fork_baselines(const fs::path& path)
{
    auto records = reduce(metadata() + start("first") + tokens(10, ",\"input_tokens\":6,\"output_tokens\":4,\"cached_input_tokens\":0") +
        complete("first") + start("second") + tokens(15, ",\"input_tokens\":9,\"output_tokens\":6,\"cached_input_tokens\":0") +
        tokens(3, ",\"input_tokens\":2,\"output_tokens\":1,\"cached_input_tokens\":0") + complete("second"), path);
    check(records.size() == 2 && records[0].usage.total_tokens == 10 && records[1].usage.total_tokens == 8,
        "cumulative baseline and counter reset preserve exact per-turn totals");
    check(records[1].usage.input.value == 5 && records[1].usage.output.value == 3, "category deltas span reset");
    check(records[0].usage.record_id == "codex:rollout:first" && records[0].usage.session_key == "rollout", "native stable turn identity");
    for (const auto& record : records) {
        addon::Contract_failure failure;
        const auto decoded = addon::decode_usage_record(addon::encode_usage_record(record.usage, 2), &failure, 2);
        check(decoded && decoded->record_id == record.usage.record_id &&
            decoded->total_tokens == record.usage.total_tokens, "native turn deltas satisfy the public usage@2 codec");
    }
    records = reduce(metadata("fork", true) + start("inherited", 1767225599) + tokens(100) + complete("inherited") +
        start("local") + tokens(107) + complete("local"), path);
    check(records.size() == 1 && records[0].usage.total_tokens == 7 && records[0].usage.record_id == "codex:fork:local",
        "inherited replay is baseline only");
    records = reduce(metadata() + start("first") + tokens(5, ",\"input_tokens\":3") + tokens(8) +
        tokens(10, ",\"input_tokens\":6") + complete("first"), path);
    check(records.size() == 1 && records[0].usage.input.applicable && records[0].usage.input.value == 0 &&
        records[0].usage.input.coverage->covered_records == 0 && records[0].usage.input.coverage->total_records == 1 &&
        records[0].usage.input.coverage->support == addon::Usage_support::UNKNOWN,
        "missing intermediate category remains unknown coverage rather than unsupported or a fabricated count");
    check(records[0].usage.cache_creation.coverage->support == addon::Usage_support::UNSUPPORTED,
        "cache creation retains the native unsupported category");
}

bool proves(const codex::Rollout_usage_record& record, std::string_view event_id)
{
    const auto& proof = record.usage.cross_source_event;
    return proof && proof->authority_namespace == "openai.codex.completed-turn" && proof->event_id == event_id;
}

void reducer_proves_only_complete_turns(const fs::path& path)
{
    // A turn's total is proved to be one complete event only when its
    // cumulative counter had a baseline before the turn, the turn observed
    // samples, and no counter reset or tokens left pending by a turn that
    // never completed entered the total.
    auto records = reduce(metadata() + start("first") + tokens(10) + complete("first") +
        start("second") + tokens(25) + complete("second"), path);
    check(records.size() == 2 && !records[0].usage.cross_source_event,
        "a rollout's first turn has no baseline and no proof");
    check(records[1].usage.total_tokens == 15 && proves(records[1], "rollout:second"),
        "a turn with a known baseline is proved by its rollout and turn IDs");

    records = reduce(metadata() + start("first") + tokens(10) + complete("first") +
        start("reset") + tokens(15) + tokens(3) + complete("reset") +
        start("after") + tokens(9) + complete("after"), path);
    check(records.size() == 3 && records[1].usage.total_tokens == 8 && !records[1].usage.cross_source_event,
        "a counter reset keeps the exact total but withholds the proof");
    check(records[2].usage.total_tokens == 6 && proves(records[2], "rollout:after"),
        "the sample after a reset is the next turn's baseline");

    records = reduce(metadata() + start("first") + tokens(10) + complete("first") +
        start("abandoned") + tokens(14) + start("next") + tokens(20) + complete("next") +
        start("quiet") + complete("quiet"), path);
    check(records.size() == 3 && records[1].usage.record_id == "codex:rollout:next" &&
        records[1].usage.total_tokens == 10 && !records[1].usage.cross_source_event,
        "tokens pending from a turn that never completed withhold the next turn's proof");
    check(records[2].usage.record_id == "codex:rollout:quiet" && records[2].usage.total_tokens == 0 &&
        !records[2].usage.cross_source_event, "a turn without samples has no proof");

    records = reduce(metadata("fork", true) + start("inherited", 1767225599) + tokens(100) + complete("inherited") +
        start("local") + tokens(107) + complete("local"), path);
    check(records.size() == 1 && proves(records[0], "fork:local"),
        "a fork's replayed history is the baseline of its first local turn");
}

void write(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    check(static_cast<bool>(output), "write fixture history");
}

fs::path accepted_snapshot(const fs::path& storage)
{
    const auto root = storage / "codex-usage-v1";
    std::ifstream input(root / "accepted", std::ios::binary);
    check(static_cast<bool>(input), "accepted generation pointer exists");
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto pointer = addon::parse_json(bytes, {}, nullptr);
    check(pointer && pointer->find("snapshot") && pointer->find("snapshot")->is_string(),
        "accepted generation has its snapshot identity");
    return root / "snapshots" / pointer->find("snapshot")->as_string();
}

void scanner_sorts_pages_and_preserves_cache_on_retry(const fs::path& fixture)
{
    const auto profile = fixture / "profile";
    const auto storage = fixture / "storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto path = profile / "sessions" / "rollout-history.jsonl";
    std::string bytes = metadata();
    for (int index = 0; index < 300; ++index) {
        const auto turn = "turn-" + std::to_string(299 - index);
        bytes += start(turn) + tokens(index + 1) + complete(turn);
    }
    write(path, bytes);
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    std::string error;
    auto first = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(first && !first->complete && first->records.size() == 256, error.empty() ? "bounded first page" : error.c_str());
    for (std::size_t index = 1; index < first->records.size(); ++index) {
        check(first->records[index - 1].record_id < first->records[index].record_id, "external sort order");
    }
    request.snapshot_id = first->snapshot_id;
    request.cursor = first->next_cursor;
    const auto final = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(final && final->complete && final->records.size() == 44, "terminal page");
    check(first->records.back().record_id < final->records.front().record_id, "ordering across pages");
    const auto retry = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(retry && addon::encode_usage_scan_result(*retry, 2) == addon::encode_usage_scan_result(*final, 2), "continuation retry is stable");
    request.snapshot_id.reset(); request.cursor.reset();
    write(path, bytes + "{broken json}\n");
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error), "malformed changed history fails snapshot");
    fs::remove(path);
    const auto retained = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(retained && retained->records.size() == 256, "disappeared source retains last valid history");
    unsigned polls = 0;
    check(!codex::scan_usage(request, profile, storage, [&] { return ++polls > 8; }, quiet_progress, &error) && error == "cancelled",
        "cancellation discards pending snapshot");
    unsigned snapshots = 0;
    for (const auto& entry : fs::directory_iterator(storage / "codex-usage-v1" / "snapshots")) {
        if (entry.is_directory()) ++snapshots;
    }
    check(snapshots <= 2, "snapshots and cancelled scratch have bounded retention");
}

void scanner_accepts_new_profiles_and_retains_transient_empty_history(const fs::path& fixture)
{
    const auto profile = fixture / "new-profile";
    const auto storage = fixture / "new-profile-storage";
    fs::create_directory(profile);
    fs::create_directory(storage);
    addon::Usage_scan_request request;
    std::string error;
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->complete && page->records.empty(), "existing new account without sessions is an empty successful scan");
    const auto sessions = profile / "sessions";
    write(sessions, "not a directory");
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "usage_sessions_not_directory",
        "an existing non-directory session path remains invalid");
    fs::remove(sessions);
    fs::create_directory(sessions);
    const auto rollout = sessions / "rollout-new.jsonl";
    write(rollout, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.empty() && page->incomplete_historical_coverage, "first empty rollout is transient, not invalid history");
    for (const auto& entry : fs::directory_iterator(accepted_snapshot(storage))) {
        check(entry.path().extension() != ".meta", "empty source does not manufacture a cache entry");
    }
    write(rollout, metadata() + start("turn") + tokens(17) + complete("turn"));
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17, "populated rollout establishes accepted history");
    write(rollout, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17 && page->incomplete_historical_coverage,
        "temporarily truncated rollout preserves accepted usage");
    fs::remove(rollout);
    fs::remove(sessions);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "missing sessions collection retains accepted file cache");
    fs::remove(profile);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "usage_sessions_unavailable",
        "missing profile itself remains invalid");
}

std::string utf8(const fs::path& path)
{
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

addon::Json_value native_wrapper(const fs::path& source, std::string_view rollout, int total)
{
    auto record = addon::Json_value::object();
    record.set("session_id", addon::Json_value("codex:" + std::string(rollout) + ":turn"));
    record.set("rollout_id", addon::Json_value(rollout));
    record.set("turn_id", addon::Json_value("turn"));
    record.set("completed_date", addon::Json_value("2026-01-02"));
    record.set("source_path", addon::Json_value(utf8(source)));
    record.set("total_tokens", addon::Json_value(total));
    record.set("recorded_cwd", addon::Json_value(""));
    record.set("model", addon::Json_value("native-model"));
    record.set("counter_field", addon::Json_value("total_token_usage.total_tokens"));
    record.set("counter_semantics", addon::Json_value("cumulative_turn_delta"));
    auto breakdown = addon::Json_value::object();
    breakdown.set("version", addon::Json_value(2));
    breakdown.set("coverage_unit", addon::Json_value("sessions"));
    for (const auto& entry : {std::pair{"input_tokens", total - 5}, std::pair{"output_tokens", 5},
            std::pair{"cache_read_input_tokens", 0}, std::pair{"cache_creation_input_tokens", -1}}) {
        auto field = addon::Json_value::object();
        field.set("support", addon::Json_value(entry.second < 0 ? "unsupported" : "supported"));
        field.set("covered_records", addon::Json_value(entry.second < 0 ? 0 : 1));
        field.set("total_records", addon::Json_value(1));
        if (entry.second >= 0) field.set("known_tokens", addon::Json_value(entry.second));
        breakdown.set(entry.first, std::move(field));
    }
    auto capabilities = addon::Json_value::object();
    capabilities.set("token_breakdown", std::move(breakdown));
    auto identity = addon::Json_value::object();
    const auto public_id = "codex:" + std::string(rollout) + ":turn";
    identity.set("version", addon::Json_value(1));
    identity.set("record_id", addon::Json_value(public_id));
    identity.set("session_key", addon::Json_value(rollout));
    identity.set("legacy_record_id", addon::Json_value(public_id));
    identity.set("legacy_session_key", addon::Json_value(rollout));
    capabilities.set("usage-record-identity", std::move(identity));
    record.set("session_id", addon::Json_value(addon::to_hex(reinterpret_cast<const std::uint8_t*>(public_id.data()), public_id.size())));
    record.set("capabilities", std::move(capabilities));
    auto wrapper = addon::Json_value::object();
    wrapper.set("path", addon::Json_value(utf8(source)));
    wrapper.set("size", addon::Json_value(0));
    wrapper.set("modified_msecs", addon::Json_value(0));
    auto records = addon::Json_value::array();
    records.push_back(std::move(record));
    wrapper.set("records", std::move(records));
    capabilities = addon::Json_value::object();
    auto marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(2));
    marker.set("parser_revision", addon::Json_value(2));
    capabilities.set("token_breakdown_cache", std::move(marker));
    marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(1));
    marker.set("parser_revision", addon::Json_value(1));
    capabilities.set("repository_usage_cache", std::move(marker));
    wrapper.set("capabilities", std::move(capabilities));
    return wrapper;
}

void set_seed(addon::Usage_scan_request* request, const fs::path& artifact, std::string_view bytes)
{
    write(artifact, bytes);
    request->native_file_cache = addon::Usage_native_file_cache{
        utf8(artifact), addon::to_hex(addon::Sha256::digest(bytes)), "account", true};
}

void native_seed_replay_and_required_cache_loss(const fs::path& fixture)
{
    const auto profile = fixture / "seed-profile";
    const auto storage = fixture / "seed-storage";
    fs::create_directories(profile / "sessions"); fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-retained.jsonl";
    const auto artifact = storage / "native-file-cache.jsonl";
    const auto bytes = addon::serialize_json(native_wrapper(source, "legacy", 17)) + '\n';
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    set_seed(&request, artifact, bytes);
    std::string error;
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "missing native rollout retained through typed cache seed");
    check(!page->records[0].source_counter_semantics, "usage2 native conversion remains unchanged");
    check(page->records[0].input.value == 12 && page->records[0].input.coverage->covered_records == 1 &&
        page->records[0].cache_creation.coverage->support == addon::Usage_support::UNSUPPORTED,
        "native cached category evidence preserved exactly");
    write(source, metadata("legacy") + start("turn") + tokens(23) + complete("turn"));
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records[0].total_tokens == 23, "live file advances seeded stable turn");
    fs::remove(source);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records[0].total_tokens == 23, "same consumed seed cannot overwrite newer retained counts");
    set_seed(&request, artifact, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records[0].total_tokens == 23, "empty host FILE_CACHE preserves durable package history");
    const auto cache = accepted_snapshot(storage);
    for (const auto& entry : fs::directory_iterator(cache)) {
        if (entry.path().extension() == ".records") fs::remove(entry.path());
    }
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "required_usage_cache_unavailable",
        "empty required seed plus lost retained cache fails before replacement");
    set_seed(&request, artifact, bytes);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "required_usage_cache_unavailable",
        "consumed stale seed cannot resurrect older history after cache loss");
}

void native_seed_fingerprint_uses_system_clock(const fs::path& fixture)
{
    const auto profile = fixture / "fingerprint-profile";
    const auto storage = fixture / "fingerprint-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-fingerprint.jsonl";
    const auto artifact = storage / "native-file-cache.jsonl";
    write(source, metadata("fingerprint") + start("turn") + tokens(23) + complete("turn"));

    const auto modified = std::chrono::sys_days{std::chrono::year{2026} / 1 / 2} +
        std::chrono::hours{12} + std::chrono::milliseconds{123};
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto file_modified = std::chrono::clock_cast<fs::file_time_type::clock>(modified);
#else
    const auto file_modified = fs::file_time_type::clock::from_sys(modified);
#endif
    fs::last_write_time(source, file_modified);
    check(fs::last_write_time(source) == file_modified, "fixture preserves exact millisecond file timestamp");

    // Distinct counts reveal whether the trusted fingerprint reuses the native
    // evidence or reparses the source. No wall-clock sampling defines identity.
    auto wrapper = native_wrapper(source, "fingerprint", 17);
    wrapper.set("size", addon::Json_value(static_cast<std::int64_t>(fs::file_size(source))));
    wrapper.set("modified_msecs", addon::Json_value(static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(modified.time_since_epoch()).count())));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    set_seed(&request, artifact, addon::serialize_json(wrapper) + '\n');
    std::string error;
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "matching system-clock milliseconds reuse native cache evidence");

    request.native_file_cache.reset();
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "native timestamp identity survives accepted-cache reload");
    fs::last_write_time(source, file_modified + std::chrono::seconds{2});
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 23,
        "changed timestamp with unchanged size refreshes native cache evidence");
}

void native_seed_cancellation_preserves_accepted_history(const fs::path& fixture)
{
    const auto profile = fixture / "seed-cancel-profile";
    const auto storage = fixture / "seed-cancel-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto artifact = storage / "native-file-cache.jsonl";
    const auto bytes = addon::serialize_json(native_wrapper(profile / "sessions" / "rollout-a.jsonl", "a", 17)) + '\n' +
        addon::serialize_json(native_wrapper(profile / "sessions" / "rollout-b.jsonl", "b", 18)) + '\n';
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    std::string error;
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(baseline && baseline->records.empty(), "establish accepted empty history");
    const auto accepted = accepted_snapshot(storage);
    set_seed(&request, artifact, bytes);
    const auto snapshots = storage / "codex-usage-v1" / "snapshots";
    const auto cancel_after_first_file = [&] {
        if (!fs::exists(snapshots)) return false;
        for (const auto& generation : fs::directory_iterator(snapshots)) {
            if (generation.path() == accepted) continue;
            for (const auto& entry : fs::directory_iterator(generation.path())) {
                if (entry.path().extension() == ".meta") return true;
            }
        }
        return false;
    };
    check(!codex::scan_usage(request, profile, storage, cancel_after_first_file, quiet_progress, &error) && error == "cancelled",
        "cancel through production predicate after one native candidate file");
    check(accepted_snapshot(storage) == accepted, "cancelled native import cannot replace accepted evidence");
    const auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 2 && page->records[0].total_tokens + page->records[1].total_tokens == 35,
        "retry imports every native file without acknowledging the cancelled partial import");
    request.native_file_cache->sha256_hex = std::string(64, '0');
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "native_usage_cache_digest_mismatch",
        "digest mismatch cannot publish imported evidence");
}

void rejected_scan_cannot_publish_refreshed_history_after_source_deletion(const fs::path& fixture)
{
    const auto profile = fixture / "rejected-profile";
    const auto storage = fixture / "rejected-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-history.jsonl";
    const auto duplicate = profile / "sessions" / "rollout-duplicate.jsonl";
    write(source, metadata("retained") + start("turn") + tokens(17) + complete("turn"));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    std::string error;
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(baseline && baseline->records.size() == 1 && baseline->records[0].total_tokens == 17,
        "establish accepted history before whole-scan rejection");
    const auto accepted = accepted_snapshot(storage);
    write(source, metadata("retained") + start("turn") + tokens(310) + complete("turn"));
    write(duplicate, metadata("retained") + start("turn") + tokens(19) + complete("turn"));
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "duplicate_usage_record",
        "both refreshed files are rejected together when snapshot identities collide");
    check(accepted_snapshot(storage) == accepted, "failed complete candidate leaves accepted authority intact");
    fs::remove(source);
    fs::remove(duplicate);
    const auto retained = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(retained && retained->records.size() == 1 && retained->records[0].total_tokens == 17,
        "source deletion after rejection retains only previously accepted evidence");
}

void transient_refresh_discards_partial_records_before_acceptance(const fs::path& fixture)
{
    const auto profile = fixture / "transient-profile";
    const auto storage = fixture / "transient-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-history.jsonl";
    write(source, metadata("transient") + start("turn") + tokens(17) + complete("turn"));
    addon::Usage_scan_request request;
    std::string error;
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(baseline && baseline->records.size() == 1, "establish history before a concurrent source change");

    constexpr unsigned k_refresh_records = 500;
    std::string bytes = metadata("transient");
    for (unsigned index = 0; index < k_refresh_records; ++index) {
        const auto turn = "turn-" + std::to_string(index);
        bytes += start(turn) + tokens(index + 1) + complete(turn);
    }
    check(bytes.size() > 65536, "fixture spans multiple rollout reader chunks");
    write(source, bytes);
    const auto original_modified = fs::last_write_time(source);
    const auto snapshots = storage / "codex-usage-v1" / "snapshots";
    bool changed_after_output = false;
    std::size_t records_at_mutation = 0;
    const auto change_during_read = [&] {
        if (!changed_after_output) {
            for (const auto& generation : fs::directory_iterator(snapshots)) {
                for (const auto& entry : fs::directory_iterator(generation.path())) {
                    if (entry.path().extension() != ".pending-records" || fs::file_size(entry.path()) == 0) continue;
                    // Enumeration metadata can lag an open writer on Windows.
                    // Read the actual output and require a partial parse: the
                    // callback also runs after close/fingerprint validation,
                    // when this fixture has all k_refresh_records lines.
                    std::ifstream pending(entry.path(), std::ios::binary);
                    check(static_cast<bool>(pending), "open pending records for the mutation phase check");
                    const auto complete_records = std::count(std::istreambuf_iterator<char>(pending),
                        std::istreambuf_iterator<char>{}, '\n');
                    check(!pending.bad(), "read pending records for the mutation phase check");
                    if (complete_records == 0 || complete_records >= k_refresh_records) continue;
                    check(fs::last_write_time(source) == original_modified, "source fingerprint is unchanged before mutation");
                    const auto modified = original_modified + std::chrono::seconds(5);
                    fs::last_write_time(source, modified);
                    check(fs::last_write_time(source) == modified, "source fingerprint changes during the partial parse");
                    records_at_mutation = static_cast<std::size_t>(complete_records);
                    changed_after_output = true;
                    return false;
                }
            }
        }
        return false;
    };
    const auto retained = codex::scan_usage(request, profile, storage, change_during_read, quiet_progress, &error);
    check(changed_after_output && records_at_mutation > 0 && records_at_mutation < k_refresh_records,
        "source must change during partial parsing, not at the post-fingerprint callback");
    const auto diagnostic = "transient source change preserves the accepted file evidence: partial_records=" +
        std::to_string(records_at_mutation) + " error=" + error +
        (retained ? " incomplete=" + std::to_string(retained->incomplete_historical_coverage) +
            " records=" + std::to_string(retained->records.size()) +
            (retained->records.empty() ? "" : " first_tokens=" + std::to_string(retained->records[0].total_tokens))
            : " no_page");
    check(retained && retained->incomplete_historical_coverage && retained->records.size() == 1 &&
        retained->records[0].total_tokens == 17, diagnostic.c_str());
    for (const auto& entry : fs::directory_iterator(accepted_snapshot(storage))) {
        check(entry.path().extension() != ".pending-records",
            "accepted generation retains no discarded partial refresh output");
    }
}

void accepted_inventory_controls_cache_replay(const fs::path& fixture)
{
    const auto profile = fixture / "inventory-profile";
    const auto storage = fixture / "inventory-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-history.jsonl";
    write(source, metadata("inventory") + start("turn") + tokens(17) + complete("turn"));
    addon::Usage_scan_request request;
    std::string error;
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1, "establish inventory-bound cache");
    fs::remove(source);
    auto accepted = accepted_snapshot(storage);
    write(accepted / (std::string(64, 'a') + ".meta"), "{\"source\":\"unaccepted\"}\n");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "unlisted cache files cannot enter the next accepted generation");
    accepted = accepted_snapshot(storage);
    for (const auto& entry : fs::directory_iterator(accepted)) {
        if (entry.path().extension() == ".records") write(entry.path(), "");
    }
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) &&
        error == "required_usage_cache_unavailable", "changed accepted content fails its bound inventory digest");
}

void empty_native_cache_is_accepted_evidence(const fs::path& fixture)
{
    const auto profile = fixture / "empty-seed-profile";
    const auto storage = fixture / "empty-seed-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto artifact = storage / "native-file-cache.jsonl";
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    set_seed(&request, artifact, "");
    std::string error;
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) &&
        error == "required_usage_cache_unavailable", "absent required evidence is not a validated empty cache");
    auto wrapper = native_wrapper(profile / "sessions" / "rollout-empty.jsonl", "empty", 17);
    wrapper.set("records", addon::Json_value::array());
    set_seed(&request, artifact, addon::serialize_json(wrapper) + '\n');
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->complete && page->records.empty(), "valid native file with no completed turns is accepted evidence");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.empty(), "required empty native cache remains valid on seed replay");
    set_seed(&request, artifact, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.empty(), "empty host FILE_CACHE retains a validated empty accepted generation");
}

void native_turn_identity_remains_validated(const fs::path& source)
{
    auto wrapper = native_wrapper(source, "native", 17);
    std::string error;
    unsigned emitted = 0;
    const auto converted = codex::convert_native_cache_file(wrapper, [&](const auto& record) {
        addon::Contract_failure failure;
        const auto decoded = addon::decode_usage_record(addon::encode_usage_record(record.usage, 2), &failure, 2);
        check(decoded && decoded->record_id == "codex:native:turn" && decoded->total_tokens == 17,
            "native turn identity and count survive strict public conversion");
        ++emitted;
        return true;
    }, &error);
    check(converted && emitted == 1, "native private diagnostics do not invalidate a public usage record");
    wrapper.find("records")->as_array()[0].set("turn_id", addon::Json_value("different-turn"));
    check(!codex::convert_native_cache_file(wrapper, [](const auto&) { return true; }, &error),
        "native cumulative-turn identity mismatch is still rejected");
}

struct Tree_usage
{
    std::uint64_t bytes = 0;
    std::uint64_t entries = 1;
    std::uint64_t lines = 0;
};

Tree_usage tree_usage(const fs::path& root)
{
    Tree_usage usage;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        ++usage.entries;
        if (!entry.is_regular_file()) continue;
        usage.bytes += entry.file_size();
        std::ifstream input(entry.path(), std::ios::binary);
        char byte;
        while (input.get(byte)) if (byte == '\n') ++usage.lines;
    }
    return usage;
}

void disk_limits_refuse_growth_without_publishing(const fs::path& fixture)
{
    const auto profile = fixture / "budget-profile";
    const auto storage = fixture / "budget-storage";
    fs::create_directories(profile / "sessions"); fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-budget.jsonl";
    write(source, metadata("budget") + start("first") + tokens(17) + complete("first"));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    std::string error;
    const auto accepted = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(accepted && accepted->records.size() == 1, "budget fixture establishes retained history");
    const auto root = storage / "codex-usage-v1";
    const auto initial = tree_usage(root);
    const auto record_bytes = addon::serialize_json(addon::encode_usage_record(accepted->records.front(), 2)).size();
    write(source, metadata("budget") + start("first") + tokens(310) + complete("first"));
    for (const std::string kind : {"bytes", "entries", "lines", "records"}) {
        codex::Usage_scan_limits limits;
        if (kind == "bytes") limits.disk_bytes = initial.bytes + record_bytes - 1;
        if (kind == "entries") limits.filesystem_entries = initial.entries + 1;
        if (kind == "lines") limits.persisted_lines = initial.lines;
        if (kind == "records") limits.candidate_records = 0;
        const auto result = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error, limits);
        check(!result && (error == "usage_disk_budget_exceeded" || error == "usage_candidate_budget_exceeded"),
            "resource limit must reject before publishing the changed candidate");
        const auto after = tree_usage(root);
        check(after.bytes <= limits.disk_bytes && after.entries <= limits.filesystem_entries && after.lines <= limits.persisted_lines,
            "failed attempt cannot exceed admitted disk resources");
        check(after.bytes == initial.bytes && after.entries == initial.entries,
            "repeated denied replacements leave no growing scratch artifacts");
        auto replay = request;
        replay.snapshot_id = accepted->snapshot_id;
        replay.cursor = "0";
        const auto retained = codex::scan_usage(replay, profile, storage, [] { return false; }, quiet_progress, &error);
        check(retained && retained->records.front().total_tokens == 17,
            "previous immutable snapshot survives resource-limit failure unchanged");
    }
    const auto imported_profile = fixture / "budget-import-profile";
    const auto imported_storage = fixture / "budget-import-storage";
    fs::create_directories(imported_profile / "sessions"); fs::create_directory(imported_storage);
    const auto artifact = imported_storage / "native-file-cache.jsonl";
    set_seed(&request, artifact, addon::serialize_json(native_wrapper(imported_profile / "sessions" / "rollout-gone.jsonl", "gone", 17)) + '\n');
    codex::Usage_scan_limits limits;
    limits.candidate_records = 0;
    check(!codex::scan_usage(request, imported_profile, imported_storage, [] { return false; }, quiet_progress, &error, limits) &&
        error == "usage_candidate_budget_exceeded", "native import observes the same record budget before cache writes");
    const auto imported_root = imported_storage / "codex-usage-v1";
    check(fs::is_empty(imported_root / "snapshots") && !fs::exists(imported_root / "accepted"),
        "over-budget native evidence cannot publish an accepted generation");
}

void selected_cache_preserves_host_authority(const fs::path& fixture)
{
    const auto profile = fixture / "selected-profile";
    const auto storage = fixture / "selected-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-selected.jsonl";
    write(source, metadata() + start("turn-one") + tokens(19) + complete("turn-one"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    request.context.account_context = addon::Account_context{1, "selected-account"};
    const auto allocate = [&](const char* name) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate fresh candidate");
        request.snapshot_id.reset();
        request.cursor.reset();
        request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(path)};
    };
    const auto cancelled = [] { return false; };
    std::string error;
    allocate("first");
    const auto first = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(first && first->cache_candidate && first->records.front().total_tokens == 19, "first immutable candidate");
    check(first->records.front().aggregate_evidence == "partial", "parsing alone does not certify a complete aggregate");
    check(!fs::exists(storage / "codex-usage-v1"), "usage3 never creates or promotes the usage2 namespace");
    write(source, metadata() + start("turn-one") + tokens(7) + complete("turn-one"));
    allocate("rejected");
    request.cache_selection->accepted = first->cache_candidate;
    request.mode = "repair";
    const auto rejected = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(rejected && rejected->records.front().total_tokens == 7 && rejected->cache_candidate,
        "lower partial candidate remains explicitly unaccepted");
    check(fs::remove(source), "remove source to expose selected cache identity");
    allocate("retry");
    request.cache_selection->accepted = first->cache_candidate;
    const auto retry = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(retry && retry->records.front().total_tokens == 19, "host-selected cache wins over newer rejected candidate");
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(*first->cache_candidate, addon::path_to_utf8(storage),
        request.context.account_context, cancelled, &failure).has_value(), "original accepted cache remains intact");
    allocate("reset");
    request.mode = "reset";
    const auto reset = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(reset && reset->complete && reset->records.empty(), "usage3 reset successfully drops source-absent retained history");
    check(fs::exists(storage / "first" / "cache-manifest.json") && fs::exists(storage / "rejected" / "cache-manifest.json"),
        "provider never prunes accepted or unaccepted candidates");
    allocate("cancelled");
    check(!codex::scan_usage(request, profile, storage, [] { return true; }, quiet_progress, &error) && error == "cancelled",
        "cancellation does not publish any acceptance");
    write(storage / "first" / "records", "corrupt\n");
    allocate("corrupt-input");
    request.mode = "incremental";
    request.cache_selection->accepted = first->cache_candidate;
    check(!codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error) && error == "usage_cache_unavailable",
        "corrupt host-selected cache cannot fall back to the valid newer candidate");
}

void selected_cache_stays_compact_across_incremental_scans(const fs::path& fixture)
{
    // A candidate holds the same few files however many rollouts a home has,
    // and an incremental scan carries the accepted records of every rollout
    // it does not parse again, including rollouts whose files are gone.
    const auto profile = fixture / "compact-profile";
    const auto storage = fixture / "compact-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    constexpr int rollouts = 40;
    const auto source = [&](int index) {
        return profile / "sessions" / ("rollout-compact-" + std::to_string(index) + ".jsonl");
    };
    for (int index = 0; index < rollouts; ++index) {
        write(source(index), metadata("compact-" + std::to_string(index)) + start("turn") + tokens(10 + index) +
            complete("turn"));
    }
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "compact-account"};
    const auto cancelled = [] { return false; };
    std::string error;
    // Runs of about one record make the scan sort through many merged runs.
    codex::Usage_scan_limits limits;
    limits.sort_run_bytes = 1024;
    const auto scan = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate compact candidate");
        request.snapshot_id.reset();
        request.cursor.reset();
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(path)};
        const auto page = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error, limits);
        check(page && page->complete && page->cache_candidate, error.empty() ? "compact scan succeeds" : error.c_str());
        return *page;
    };
    const auto total = [](const addon::Usage_scan_page& page) {
        std::uint64_t sum = 0;
        for (const auto& record : page.records) {
            sum += record.total_tokens;
        }
        return sum;
    };
    const auto files = [&](const addon::Usage_scan_page& page) {
        addon::Contract_failure failure;
        const auto manifest = addon::validate_usage_cache_candidate(
            *page.cache_candidate, utf8(storage), request.context.account_context, cancelled, &failure);
        check(manifest.has_value(), "compact candidate validates");
        return manifest->files.size();
    };
    const auto first = scan("first", std::nullopt);
    check(first.records.size() == rollouts && total(first) == 1180, "first scan records every rollout");
    check(files(first) <= 4, "a candidate's file count does not grow with its rollouts");

    // Rollout 0 gains a turn, rollout 1 disappears, and rollout 2 becomes
    // unparseable behind an unchanged size and modification time.
    write(source(0), metadata("compact-0") + start("turn") + tokens(10) + complete("turn") +
        start("second") + tokens(15) + complete("second"));
    check(fs::remove(source(1)), "remove a rollout");
    const auto unchanged_size     = fs::file_size(source(2));
    const auto unchanged_modified = fs::last_write_time(source(2));
    write(source(2), std::string(unchanged_size - 1, 'x') + "\n");
    fs::last_write_time(source(2), unchanged_modified);
    const auto next = scan("next", first.cache_candidate);
    check(next.records.size() == rollouts + 1 && total(next) == 1180 + 5,
        "an incremental scan adds the new turn once and retains every other accepted record");
    for (std::size_t index = 1; index < next.records.size(); ++index) {
        check(next.records[index - 1].record_id < next.records[index].record_id, "merged records stay sorted");
    }
    check(files(next) <= 4, "an incremental candidate stays compact");
}

void selected_cache_reconciles_turns_by_identity(const fs::path& fixture)
{
    // Turns reconcile by their stable record IDs: a rollout parsed again keeps
    // the accepted turns it no longer holds, and a moved rollout remains one
    // observation of each turn.
    const auto profile = fixture / "identity-profile";
    const auto storage = fixture / "identity-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto truncated = profile / "sessions" / "rollout-truncated.jsonl";
    const auto moved     = profile / "sessions" / "rollout-moved.jsonl";
    const auto renamed   = profile / "sessions" / "rollout-renamed.jsonl";
    const auto both_turns = metadata("truncated") + start("a") + tokens(10) + complete("a") +
        start("b") + tokens(25) + complete("b");
    write(truncated, both_turns);
    write(moved, metadata("moved") + start("c") + tokens(7) + complete("c"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "identity-account"};
    const auto cancelled = [] { return false; };
    std::string error;
    unsigned allocations = 0;
    const auto scan = [&](const char* mode, std::optional<addon::Usage_cache_reference> accepted) {
        const auto path = storage / ("candidate-" + std::to_string(allocations++));
        check(fs::create_directory(path), "allocate identity candidate");
        request.mode = mode;
        request.snapshot_id.reset();
        request.cursor.reset();
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(path)};
        return codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    };
    const auto totals = [](const addon::Usage_scan_page& page) {
        std::map<std::string, std::uint64_t> result;
        for (const auto& record : page.records) {
            result[record.record_id] = record.total_tokens;
        }
        return result;
    };
    const std::map<std::string, std::uint64_t> expected{
        {"codex:moved:c", 7}, {"codex:truncated:a", 10}, {"codex:truncated:b", 15}};
    auto accepted = scan("incremental", std::nullopt);
    check(accepted && totals(*accepted) == expected, "first scan records every turn");

    write(truncated, metadata("truncated") + start("a") + tokens(10) + complete("a"));
    for (const auto* mode : {"incremental", "repair"}) {
        const auto page = scan(mode, accepted->cache_candidate);
        check(page && totals(*page) == expected,
            error.empty() ? "a truncated rollout keeps its accepted turns" : error.c_str());
        accepted = page;
    }

    fs::rename(moved, renamed);
    const auto after_move = scan("incremental", accepted->cache_candidate);
    check(after_move && totals(*after_move) == expected,
        error.empty() ? "a moved rollout reconciles to one observation of each turn" : error.c_str());
    for (const auto& record : after_move->records) {
        if (record.record_id == "codex:moved:c") {
            check(record.source_path == utf8(fs::absolute(moved).lexically_normal()),
                "the reconciled turn keeps its accepted identity");
        }
    }
    write(renamed, metadata("moved") + start("c") + tokens(7) + complete("c") + start("d") + tokens(12) + complete("d"));
    auto grown = expected;
    grown["codex:moved:d"] = 5;
    const auto after_growth = scan("incremental", after_move->cache_candidate);
    check(after_growth && totals(*after_growth) == grown, "a moved rollout's new turn adds exactly its delta");

    // Two live copies disagreeing about one turn cannot both be accepted.
    write(profile / "sessions" / "rollout-conflict.jsonl", metadata("moved") + start("c") + tokens(9) + complete("c"));
    check(!scan("repair", after_growth->cache_candidate) && error == "duplicate_usage_record",
        "conflicting observations of one turn fail the scan");
}

void selected_cache_retains_repository_attribution(const fs::path& fixture)
{
    // The host rejects a partial observation whose repository attribution
    // differs from the accepted one, so a checkout that disappears after
    // acceptance must not change the attribution of turns already accepted.
    const auto profile  = fixture / "repository-profile";
    const auto storage  = fixture / "repository-storage";
    const auto checkout = fixture / "repository-checkout";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    fs::create_directories(checkout / ".git");
    const auto source  = profile / "sessions" / "rollout-repository.jsonl";
    const auto renamed = profile / "sessions" / "rollout-repository-renamed.jsonl";
    const auto original = metadata_with_cwd("repository", checkout) + start("first") + tokens(17) + complete("first");
    write(source, original);

    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    request.context.account_context = addon::Account_context{1, "repository-account"};
    const auto cancelled = [] { return false; };
    std::string error;
    const auto scan = [&](const char* name, const std::optional<addon::Usage_cache_reference>& accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate repository candidate");
        request.mode = "incremental";
        request.cache_selection = addon::Usage_cache_selection{accepted, addon::path_to_utf8(path)};
        auto page = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
        check(page && page->complete && page->cache_candidate, error.empty() ? "repository scan succeeds" : error.c_str());
        return *page;
    };
    const auto keeps_accepted = [](const addon::Usage_scan_page& page, const std::string& repository) {
        const auto& first = page.records.front();
        return first.record_id == "codex:repository:first" && first.repository_usage &&
            first.repository_usage->allocations.size() == 1 &&
            first.repository_usage->allocations.front().repository_path == repository &&
            first.repository_usage->allocations.front().total_tokens == 17;
    };

    const auto first = scan("first", std::nullopt);
    check(first.records.size() == 1 && first.records.front().repository_usage &&
        first.records.front().repository_usage->allocations.size() == 1,
        "first scan attributes the completed turn to its Git checkout");
    const auto repository = first.records.front().repository_usage->allocations.front().repository_path;

    check(fs::remove(checkout / ".git"), "remove the fixture Git marker");
    write(source, original + start("second") + tokens(29) + complete("second"));
    const auto second = scan("second", first.cache_candidate);
    check(second.records.size() == 2 && keeps_accepted(second, repository),
        "a turn parsed again after its checkout disappears keeps its accepted attribution");
    check(second.records.back().record_id == "codex:repository:second" &&
        second.records.back().repository_usage &&
        second.records.back().repository_usage->allocations.empty() &&
        second.records.back().repository_usage->unattributed_tokens == 12,
        "a new turn is attributed as its checkout stands now");

    const auto third = scan("third", second.cache_candidate);
    check(third.records.size() == 2 && keeps_accepted(third, repository),
        "an accepted turn whose rollout is not parsed again keeps its attribution");

    fs::rename(source, renamed);
    const auto moved = scan("moved", third.cache_candidate);
    check(moved.records.size() == 2 && keeps_accepted(moved, repository) &&
        moved.records.front().source_path == utf8(fs::absolute(source).lexically_normal()),
        "a moved rollout keeps its accepted location and attribution");
}

void selected_cache_keeps_imported_turns_when_their_rollout_is_parsed(const fs::path& fixture)
{
    // A turn imported from the native cache carries native counter provenance,
    // model and category detail that a parse of its rollout does not
    // reproduce. The host rejects a partial observation that changes them, so
    // parsing the rollout later must confirm the accepted record, not replace it.
    const auto profile  = fixture / "imported-profile";
    const auto storage  = fixture / "imported-storage";
    const auto checkout = fixture / "imported-checkout";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    fs::create_directories(checkout / ".git");
    const auto source = profile / "sessions" / "rollout-imported.jsonl";
    auto wrapper = native_wrapper(source, "imported", 19);
    wrapper.find("records")->as_array().front().set("recorded_cwd", addon::Json_value(utf8(checkout)));
    const auto seed_path  = storage / "native-file-cache.jsonl";
    const auto seed_bytes = addon::serialize_json(wrapper) + '\n';
    write(seed_path, seed_bytes);

    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "imported-account"};
    request.native_file_cache = addon::Usage_native_file_cache{
        utf8(seed_path), addon::to_hex(addon::Sha256::digest(seed_bytes)), "imported-account", true};
    const auto cancelled = [] { return false; };
    std::string error;
    const auto scan = [&](const char* name, const char* mode, const std::optional<addon::Usage_cache_reference>& accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate imported candidate");
        request.mode = mode;
        request.cache_selection = addon::Usage_cache_selection{accepted, utf8(path)};
        auto page = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
        check(page && page->complete && page->cache_candidate, error.empty() ? "imported scan succeeds" : error.c_str());
        return *page;
    };
    const auto encoded = [](const addon::Usage_record& record) {
        return addon::serialize_json(addon::encode_usage_record(record, 3));
    };

    const auto first = scan("first", "incremental", std::nullopt);
    check(first.records.size() == 1 && first.records.front().record_id == "codex:imported:turn" &&
        first.records.front().source_counter_semantics == "cumulative_turn_delta" &&
        first.records.front().repository_usage && first.records.front().repository_usage->allocations.size() == 1,
        "the imported turn is accepted with its native provenance and attribution");

    check(fs::remove(checkout / ".git"), "remove the fixture Git marker");
    // The rollout's leading sample is a baseline, so its parse proves the turn.
    write(source, metadata("imported") + tokens(4) + start("turn") + tokens(23) + complete("turn") +
        start("next") + tokens(30) + complete("next"));
    request.native_file_cache.reset();
    auto accepted = first.cache_candidate;
    for (const auto* mode : {"incremental", "repair"}) {
        // Records are sorted by ID, so the new turn comes first.
        const auto page = scan(mode, mode, accepted);
        check(page.records.size() == 2, "the imported rollout holds two turns");
        auto confirmed = page.records.back();
        check(confirmed.cross_source_event && confirmed.cross_source_event->event_id == "imported:turn",
            "a confirmed imported turn gains the proof its parse established");
        confirmed.cross_source_event.reset();
        check(encoded(confirmed) == encoded(first.records.front()),
            "parsing an imported turn's rollout keeps its accepted record");
        check(page.records.front().record_id == "codex:imported:next" && page.records.front().total_tokens == 7,
            "the rollout's new turn adds exactly its delta");
        accepted = page.cache_candidate;
    }
}

// Rebuilds a sealed current candidate in the per-rollout layout of @1 or @2:
// a `.meta` and a `.records` file per rollout beside the sorted `records`.
// @1 records use the usage2 grammar, which carries no counter provenance.
void progress_stays_within_the_frame_bound_and_the_silence()
{
    // The core protocol allows 4,096 progress frames per request, and the
    // host ends a request whose package sends nothing for its deadline. A
    // scan that keeps working reports within a quarter of that silence for
    // three days, and never exceeds the bound, not even for a new stage.
    using namespace std::chrono;
    const milliseconds silence = minutes(15);
    steady_clock::time_point now{};
    std::vector<steady_clock::time_point> frames;
    codex::Usage_scan_progress progress(
        [&](std::uint64_t, const std::string&) { frames.push_back(now); }, silence, [&now] { return now; });
    progress.begin("Reading Codex sessions");
    const auto end = now + hours(72);
    while (now < end) {
        now += milliseconds(100);
        progress.poll();
    }
    check(frames.size() > 1 && frames.size() <= addon::limits::k_max_progress_frames_per_request,
        "a working scan reports within the frame bound");
    for (std::size_t index = 1; index < frames.size(); ++index) {
        check(frames[index] - frames[index - 1] <= silence / 4, "a working scan reports within a quarter of the silence");
    }
    check(end - frames.back() <= silence / 4, "a scan still working after three days still reports");
    while (frames.size() < addon::limits::k_max_progress_frames_per_request) {
        now += hours(1);
        progress.poll();
    }
    now += hours(1);
    progress.poll();
    progress.begin("Saving the usage cache");
    check(frames.size() == addon::limits::k_max_progress_frames_per_request, "a spent frame bound sends nothing more");
}

void scans_report_each_stage_while_they_work(const fs::path& fixture)
{
    // A scan reports each stage as it begins and, while the stage works, how
    // many items it has handled. This clock advances a second at every
    // reading, so each of the scan's polls is due to report. The second scan
    // of each layout validates an accepted cache, parses every rollout and
    // sorts through merged runs, so every stage of it has work to report.
    const auto profile = fixture / "pgp";
    fs::create_directories(profile / "sessions");
    constexpr std::uint64_t rollouts = 3;
    for (std::uint64_t index = 0; index < rollouts; ++index) {
        const auto id = "progress-" + std::to_string(index);
        write(profile / "sessions" / ("rollout-" + id + ".jsonl"),
            metadata(id) + start("turn") + tokens(5) + complete("turn"));
    }
    std::chrono::steady_clock::time_point now{};
    std::vector<std::uint64_t> counts;
    std::vector<std::string> notes;
    codex::Usage_scan_progress progress(
        [&](std::uint64_t completed, const std::string& note) { counts.push_back(completed); notes.push_back(note); },
        std::chrono::minutes(15),
        [&now] { return now += std::chrono::seconds(1); });
    const auto check_frames = [&](const std::vector<std::string>& stages, const char* message) {
        check(stage_notes(notes) == stages, message);
        std::uint64_t sessions_read = 0;
        std::size_t stage_frames = 0;
        for (std::size_t index = 0; index < notes.size(); ++index) {
            if (index == 0 || notes[index] != notes[index - 1]) {
                check(index == 0 || stage_frames > 1, "each stage reports while it works");
                check(counts[index] == 0, "a stage counts from zero");
                stage_frames = 0;
            }
            else {
                check(counts[index] >= counts[index - 1], "a stage's count never decreases");
            }
            ++stage_frames;
            if (notes[index] == "Reading Codex sessions") sessions_read = std::max(sessions_read, counts[index]);
        }
        check(stage_frames > 1, "each stage reports while it works");
        check(sessions_read > 0 && sessions_read <= rollouts, "the reading stage counts the session files it read");
        counts.clear();
        notes.clear();
    };
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "progress-account"};
    codex::Usage_scan_limits limits;
    limits.sort_run_bytes = 64;
    std::string error;

    const auto storage = fixture / "pgs";
    const auto selected_scan = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        check(fs::create_directories(storage / name), "allocate progress candidate");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(storage / name)};
        const auto page = codex::scan_usage(request, profile, storage, [] { return false; }, progress, &error, limits);
        check(page && page->complete && page->cache_candidate, error.empty() ? "selected-cache scan succeeds" : error.c_str());
        return page->cache_candidate;
    };
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    const auto first = selected_scan("first", std::nullopt);
    counts.clear();
    notes.clear();
    request.mode = "repair";
    selected_scan("repair", first);
    check_frames({"Preparing the usage cache", "Reading Codex sessions", "Sorting usage records",
        "Merging usage history", "Saving the usage cache"}, "a selected-cache scan reports each stage in order");

    const auto legacy_storage = fixture / "pgl";
    fs::create_directory(legacy_storage);
    request.context.profile_storage.reset();
    request.cache_selection.reset();
    for (const char* mode : {"incremental", "repair"}) {
        counts.clear();
        notes.clear();
        request.mode = mode;
        const auto legacy = codex::scan_usage(request, profile, legacy_storage, [] { return false; }, progress, &error, limits);
        check(legacy && legacy->complete, error.empty() ? "per-rollout scan succeeds" : error.c_str());
    }
    check_frames({"Preparing the usage cache", "Reading Codex sessions", "Sorting usage records", "Saving the usage cache"},
        "a per-rollout scan reports each stage in order");
}

void waiting_for_another_scan_reports_nothing(const fs::path& fixture)
{
    // The host restarts every request's deadline on any frame of the package,
    // so frames sent while a scan waits for another one would hide that the
    // other scan stalled.
    const auto profile = fixture / "waiting-profile";
    const auto storage = fixture / "waiting-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    write(profile / "sessions" / "rollout-waiting.jsonl", metadata("waiting") + start("turn") + tokens(5) + complete("turn"));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "waiting-account"};
    std::mutex mutex;
    std::condition_variable changed;
    bool holding  = false;
    bool released = false;
    std::string holder_error;
    std::thread holder([&] {
        // The first poll after taking the package's scan lock holds it here.
        const auto hold = [&] {
            std::unique_lock lock(mutex);
            holding = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
            return false;
        };
        codex::scan_usage(request, profile, storage, hold, quiet_progress, &holder_error);
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return holding; });
    }
    std::vector<std::string> notes;
    codex::Usage_scan_progress progress(
        [&notes](std::uint64_t, const std::string& note) { notes.push_back(note); }, std::chrono::minutes(15));
    unsigned polls = 0;
    std::string error;
    const auto waited = codex::scan_usage(request, profile, storage, [&] { return ++polls > 4; }, progress, &error);
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    changed.notify_all();
    holder.join();
    check(!waited && error == "cancelled", "the waiting scan ends when it is cancelled");
    check(notes.empty(), "a scan waiting for another one reports nothing");
    check(holder_error.empty(), "the holding scan completes");
}

std::optional<addon::Usage_cache_reference> per_rollout_fixture(const addon::Usage_cache_reference& current,
    const fs::path& fixture_root, const std::string& profile_root,
    const std::optional<addon::Account_context>& account, int format)
{
    const auto from = addon::usage_cache_io_path(addon::path_from_utf8(current.root));
    const auto to = addon::usage_cache_io_path(fixture_root);
    check(fs::create_directory(to), "allocate per-rollout fixture");
    const auto key_of = [](const std::string& source) {
        return addon::to_hex(addon::Sha256::digest(codex::profile_account_key(source)));
    };
    addon::Contract_failure failure;
    std::map<std::string, std::string> rollout_records;
    std::string sorted_records;
    std::ifstream records(from / "records", std::ios::binary);
    std::string line;
    while (std::getline(records, line)) {
        auto envelope = addon::parse_json(line, {}, nullptr);
        check(envelope.has_value(), "parse current cache envelope");
        auto record = addon::decode_usage_record(*envelope->find("usage"), &failure, 3);
        check(record.has_value() && record->source_path.has_value(), "decode current cache record");
        if (format == 1) {
            if (record->source_counter_semantics == "cumulative_turn_delta") {
                record->source_counter_semantics.reset();
            }
            envelope->set("usage", addon::encode_usage_record(*record, 2));
        }
        const auto encoded = addon::serialize_json(*envelope) + '\n';
        sorted_records += encoded;
        rollout_records[key_of(*record->source_path)] += encoded;
    }
    check(records.eof(), "read complete cache fixture");
    write(to / "records", sorted_records);
    std::ifstream rollouts(from / "rollouts", std::ios::binary);
    while (std::getline(rollouts, line)) {
        const auto rollout = addon::parse_json(line, {}, nullptr);
        check(rollout && rollout->find("source"), "parse current rollout metadata");
        const auto key = key_of(rollout->find("source")->as_string());
        write(to / (key + ".meta"), line + '\n');
        write(to / (key + ".records"), rollout_records[key]);
    }
    for (const auto* name : {"snapshot", "cache-state"}) {
        check(fs::copy_file(from / name, to / name), "copy preceding-format metadata");
    }
    return addon::seal_usage_cache_candidate(addon::path_to_utf8(fixture_root), profile_root, account,
        current.snapshot_id, "codex-rollout-cache@" + std::to_string(format), [] { return false; }, &failure);
}

void selected_cache_keeps_turns_a_rescan_cannot_contradict(const fs::path& fixture)
{
    // A rollout that lost the events before a turn leaves that turn's first
    // sample without a baseline, so its parse counts the whole cumulative
    // counter. That unproved total cannot replace the accepted one, and the
    // accepted record keeps its proof.
    const auto profile = fixture / "predecessor-profile";
    const auto storage = fixture / "predecessor-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-predecessor.jsonl";
    write(source, metadata("predecessor") + start("a") + tokens(10) + complete("a") +
        start("b") + tokens(25) + complete("b"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "predecessor-account"};
    const auto cancelled = [] { return false; };
    std::string error;
    unsigned allocations = 0;
    const auto scan = [&](const char* mode, const std::optional<addon::Usage_cache_reference>& accepted) {
        const auto path = storage / ("candidate-" + std::to_string(allocations++));
        check(fs::create_directory(path), "allocate predecessor candidate");
        request.mode = mode;
        request.cache_selection = addon::Usage_cache_selection{accepted, utf8(path)};
        auto page = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
        check(page && page->complete && page->cache_candidate, error.empty() ? "predecessor scan succeeds" : error.c_str());
        return *page;
    };
    const auto find = [](const addon::Usage_scan_page& page, std::string_view id) {
        const auto found = std::find_if(page.records.begin(), page.records.end(),
            [id](const addon::Usage_record& record) { return record.record_id == id; });
        check(found != page.records.end(), "the scan reports every accepted turn");
        return *found;
    };
    const auto proved_b = [&](const addon::Usage_scan_page& page) {
        const auto b = find(page, "codex:predecessor:b");
        return b.total_tokens == 15 && b.cross_source_event &&
            b.cross_source_event->authority_namespace == "openai.codex.completed-turn" &&
            b.cross_source_event->event_id == "predecessor:b";
    };
    const auto first = scan("incremental", std::nullopt);
    check(first.records.size() == 2 && find(first, "codex:predecessor:a").total_tokens == 10 &&
        !find(first, "codex:predecessor:a").cross_source_event && proved_b(first),
        "the scan carries the proof of the turn with a baseline");

    write(source, metadata("predecessor") + start("b") + tokens(25) + complete("b") +
        start("c") + tokens(30) + complete("c"));
    auto accepted = first.cache_candidate;
    for (const auto* mode : {"incremental", "repair"}) {
        const auto page = scan(mode, accepted);
        check(page.records.size() == 3 && find(page, "codex:predecessor:a").total_tokens == 10 && proved_b(page),
            "a turn whose predecessor was lost keeps its accepted total and proof");
        check(find(page, "codex:predecessor:c").total_tokens == 5, "the next turn adds exactly its delta");
        accepted = page.cache_candidate;
    }
}

void adapter_cache_selection_contract(const fs::path& package, const fs::path& fixture, bool long_allocation)
{
    const auto profile = fixture / "adapter-profile";
    fs::create_directories(profile / "sessions");
    const auto source = profile / "sessions" / "rollout-adapter.jsonl";
    std::string bytes = metadata();
    for (int i = 0; i < 270; ++i) {
        const auto turn = "turn-" + std::to_string(i);
        bytes += start(turn) + tokens(i + 1) + complete(turn);
    }
    write(source, bytes);
    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "adapter-cache";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail.c_str());
    const auto native_profile = addon::path_to_utf8(profile);
    const auto account_key = codex::profile_account_key(native_profile);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.codex", &error, account_key);
    check(allocated.has_value(), error.c_str());
    auto allocation_parent = addon::path_from_utf8(*allocated);
    while (long_allocation && allocation_parent.native().size() < 291) {
        allocation_parent /= "retained-account-cache-allocation-component";
    }
    const auto candidate = allocation_parent / "scan-one";
    check(fs::create_directories(addon::usage_cache_io_path(candidate)), "allocate real adapter candidate");
    addon::Usage_scan_request request;
    request.context.launcher = {"codex", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"codex"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account_key, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
    const auto imported_source = profile / "sessions" / "rollout-imported.jsonl";
    auto imported = native_wrapper(imported_source, "imported", 19);
    const auto seed_path = addon::path_from_utf8(*allocated) / "native-file-cache.jsonl";
    const auto seed_bytes = addon::serialize_json(imported) + '\n';
    write(seed_path, seed_bytes);
    request.native_file_cache = addon::Usage_native_file_cache{
        addon::path_to_utf8(seed_path), addon::to_hex(addon::Sha256::digest(seed_bytes)), account_key, true};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledge_id = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge_id, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS, "adapter acknowledges real profile allocation");
    const auto scan = [&] {
        const bool continuation = request.snapshot_id.has_value();
        const auto id = host.invoke({"usage-scan", 3}, "scan",
            addon::encode_usage_scan_request(request, 3), 15000, invocation);
        const auto outcome = host.wait_outcome(id, 16000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            outcome ? outcome->reason_code.c_str() : "actual packaged adapter scan timed out");
        // A scan reports its stages through the core protocol; a continuation
        // page, read from the finished snapshot, reports nothing.
        std::vector<std::string> notes;
        for (const auto& frame : host.progress(id)) {
            notes.push_back(frame.note);
        }
        std::vector<std::string> expected;
        if (!continuation) {
            expected.push_back("Preparing the usage cache");
            if (request.native_file_cache) expected.push_back("Importing earlier usage history");
            expected.insert(expected.end(), {
                "Reading Codex sessions", "Sorting usage records", "Merging usage history", "Saving the usage cache"});
        }
        check(stage_notes(notes) == expected, "actual adapter reports each stage of a scan and none for a continuation page");
        check(!addon::account_context_acknowledgment_failure(*request.context.account_context, outcome->payload),
            "actual adapter acknowledges exact account context on every page");
        check(!addon::profile_storage_acknowledgment_failure(*request.context.profile_storage, outcome->payload),
            "actual adapter acknowledges exact profile storage on every page");
        const auto page = addon::decode_usage_scan_result(outcome->payload, &failure, 3);
        check(page.has_value(), failure.detail.c_str());
        return *page;
    };
    const auto first = scan();
    check(!first.complete && first.records.size() == 256 && first.cache_candidate, "adapter supplies bounded sealed first page");
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    const auto last = scan();
    check(last.complete && last.records.size() == 15 && last.cache_candidate == first.cache_candidate,
        "actual adapter preserves cache descriptor across pages");
    const auto check_import = [](const auto& first_page, const auto& last_page) {
        bool found = false;
        for (const auto* page : {&first_page, &last_page}) {
            for (const auto& record : page->records) {
                if (record.record_id != "codex:imported:turn") continue;
                found = true;
                check(record.source_counter_semantics == "cumulative_turn_delta" &&
                    record.aggregate_evidence == "partial" && record.total_tokens == 19 &&
                    record.input.value == 14 && record.output.value == 5,
                    "actual adapter preserves imported turn-delta attribution and partial evidence");
            }
        }
        check(found, "native source-absent record survives packaged scan");
    };
    check_import(first, last);
    check(addon::validate_usage_cache_candidate(*last.cache_candidate, *allocated,
        request.context.account_context, [] { return false; }, &failure).has_value(), "actual adapter cache verifies after paging");
    check(fs::remove(source), "remove native history to exercise selected cache replay");
    const auto replay_candidate = allocation_parent / "scan-replay";
    check(fs::create_directory(addon::usage_cache_io_path(replay_candidate)), "allocate replay candidate");
    request.snapshot_id.reset();
    request.cursor.reset();
    request.cache_selection = addon::Usage_cache_selection{first.cache_candidate, addon::path_to_utf8(replay_candidate)};
    const auto replay = scan();
    check(replay.records.size() == 256 && replay.cache_candidate &&
        replay.cache_candidate->root == addon::path_to_utf8(replay_candidate),
        "selected cache replay writes a new candidate without changing its wire identity");
    request.snapshot_id = replay.snapshot_id;
    request.cursor = replay.next_cursor;
    const auto replay_last = scan();
    check(replay_last.complete && replay_last.records.size() == 15 && replay_last.cache_candidate == replay.cache_candidate,
        "selected cache replay preserves all records across pages");
    check_import(replay, replay_last);
    check(addon::validate_usage_cache_candidate(*replay_last.cache_candidate, *allocated,
        request.context.account_context, [] { return false; }, &failure).has_value(), "replayed cache seals and reopens");
    imported.find("records")->as_array().front().set("counter_semantics", addon::Json_value("future_counter"));
    const auto unknown_bytes = addon::serialize_json(imported) + '\n';
    write(seed_path, unknown_bytes);
    request.native_file_cache->sha256_hex = addon::to_hex(addon::Sha256::digest(unknown_bytes));
    const auto unknown_candidate = allocation_parent / "unknown-provenance";
    check(fs::create_directory(addon::usage_cache_io_path(unknown_candidate)), "allocate unknown-provenance candidate");
    request.snapshot_id.reset();
    request.cursor.reset();
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(unknown_candidate)};
    const auto unknown_id = host.invoke({"usage-scan", 3}, "scan",
        addon::encode_usage_scan_request(request, 3), 15000, invocation);
    const auto unknown = host.wait_outcome(unknown_id, 16000);
    check(unknown && unknown->status != addon::Host_status::SUCCESS && unknown->reason_code == "native_usage_cache_invalid",
        "unknown native provenance fails without producing a silently rewritten candidate");
    // The preceding provider-owned formats kept files per rollout; @1 records
    // also used the usage2 grammar, whose missing provenance cannot be guessed.
    request.native_file_cache.reset();
    for (const int format : {1, 2}) {
        const auto name = "preceding-format-" + std::to_string(format);
        const auto old_reference = per_rollout_fixture(*first.cache_candidate, allocation_parent / name, *allocated,
            request.context.account_context, format);
        check(old_reference.has_value(), "seal preceding-format fixture");
        const auto upgraded_root = allocation_parent / ("upgraded-format-" + std::to_string(format));
        check(fs::create_directory(addon::usage_cache_io_path(upgraded_root)), "allocate upgraded candidate");
        request.snapshot_id.reset();
        request.cursor.reset();
        request.cache_selection = addon::Usage_cache_selection{old_reference, addon::path_to_utf8(upgraded_root)};
        const auto upgraded = scan();
        request.snapshot_id = upgraded.snapshot_id;
        request.cursor = upgraded.next_cursor;
        const auto upgraded_last = scan();
        check(upgraded.records.size() == 256 && upgraded_last.records.size() == 15 && upgraded_last.complete,
            "preceding selected format retains all source-absent history through upgrade");
        const auto sum = [](const auto& page) {
            std::uint64_t total = 0;
            for (const auto& record : page.records) {
                total += record.total_tokens;
            }
            return total;
        };
        check(sum(upgraded) + sum(upgraded_last) == sum(first) + sum(last),
            "a preceding-format upgrade keeps the exact total");
        if (format == 1) {
            for (const auto* page : {&upgraded, &upgraded_last}) {
                for (const auto& record : page->records) {
                    check(!record.source_counter_semantics && record.aggregate_evidence == "partial",
                        "format upgrade does not invent absent provenance or completeness");
                }
            }
        }
        else {
            check_import(upgraded, upgraded_last);
        }
        check(addon::validate_usage_cache_candidate(*old_reference, *allocated,
            request.context.account_context, [] { return false; }, &failure).has_value(),
            "format upgrade leaves the selected old candidate immutable");
    }
    check(host.shutdown(1000, nullptr), "actual adapter exits without any model request");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Temporary_directory fixture;
        projector_does_not_retain_ignored_large_payloads();
        reducer_keeps_turn_deltas_resets_and_fork_baselines(fixture.path / "rollout-fixture.jsonl");
        reducer_proves_only_complete_turns(fixture.path / "rollout-fixture.jsonl");
        scanner_sorts_pages_and_preserves_cache_on_retry(fixture.path);
        scanner_accepts_new_profiles_and_retains_transient_empty_history(fixture.path);
        native_seed_replay_and_required_cache_loss(fixture.path);
        native_seed_fingerprint_uses_system_clock(fixture.path);
        native_seed_cancellation_preserves_accepted_history(fixture.path);
        rejected_scan_cannot_publish_refreshed_history_after_source_deletion(fixture.path);
        transient_refresh_discards_partial_records_before_acceptance(fixture.path);
        accepted_inventory_controls_cache_replay(fixture.path);
        empty_native_cache_is_accepted_evidence(fixture.path);
        native_turn_identity_remains_validated(fixture.path / "rollout-native.jsonl");
        disk_limits_refuse_growth_without_publishing(fixture.path);
        selected_cache_preserves_host_authority(fixture.path);
        selected_cache_stays_compact_across_incremental_scans(fixture.path);
        selected_cache_reconciles_turns_by_identity(fixture.path);
        selected_cache_retains_repository_attribution(fixture.path);
        selected_cache_keeps_imported_turns_when_their_rollout_is_parsed(fixture.path);
        selected_cache_keeps_turns_a_rescan_cannot_contradict(fixture.path);
        progress_stays_within_the_frame_bound_and_the_silence();
        scans_report_each_stage_while_they_work(fixture.path);
        waiting_for_another_scan_reports_nothing(fixture.path);
        check(argc == 2, "usage tests require the built package directory");
        adapter_cache_selection_contract(addon::path_from_utf8(argv[1]), fixture.path / "short-allocation", false);
        adapter_cache_selection_contract(addon::path_from_utf8(argv[1]), fixture.path / "long-allocation", true);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
