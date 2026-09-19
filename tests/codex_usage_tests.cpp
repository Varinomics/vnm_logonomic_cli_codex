#include "codex_jsonl_projector.h"
#include "codex_usage.h"
#include "codex_usage_records.h"
#include "codex_native_usage.h"
#include <logonomic_cli_addon/sha256.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
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
    const auto pointer = addon::parse_json(bytes);
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
    auto first = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(first && !first->complete && first->records.size() == 256, error.empty() ? "bounded first page" : error.c_str());
    for (std::size_t index = 1; index < first->records.size(); ++index) {
        check(first->records[index - 1].record_id < first->records[index].record_id, "external sort order");
    }
    request.snapshot_id = first->snapshot_id;
    request.cursor = first->next_cursor;
    const auto final = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(final && final->complete && final->records.size() == 44, "terminal page");
    check(first->records.back().record_id < final->records.front().record_id, "ordering across pages");
    const auto retry = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(retry && addon::encode_usage_scan_result(*retry, 2) == addon::encode_usage_scan_result(*final, 2), "continuation retry is stable");
    request.snapshot_id.reset(); request.cursor.reset();
    write(path, bytes + "{broken json}\n");
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error), "malformed changed history fails snapshot");
    fs::remove(path);
    const auto retained = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(retained && retained->records.size() == 256, "disappeared source retains last valid history");
    unsigned polls = 0;
    check(!codex::scan_usage(request, profile, storage, [&] { return ++polls > 8; }, &error) && error == "cancelled",
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
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->complete && page->records.empty(), "existing new account without sessions is an empty successful scan");
    const auto sessions = profile / "sessions";
    write(sessions, "not a directory");
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) && error == "usage_sessions_not_directory",
        "an existing non-directory session path remains invalid");
    fs::remove(sessions);
    fs::create_directory(sessions);
    const auto rollout = sessions / "rollout-new.jsonl";
    write(rollout, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.empty() && page->incomplete_historical_coverage, "first empty rollout is transient, not invalid history");
    for (const auto& entry : fs::directory_iterator(accepted_snapshot(storage))) {
        check(entry.path().extension() != ".meta", "empty source does not manufacture a cache entry");
    }
    write(rollout, metadata() + start("turn") + tokens(17) + complete("turn"));
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17, "populated rollout establishes accepted history");
    write(rollout, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17 && page->incomplete_historical_coverage,
        "temporarily truncated rollout preserves accepted usage");
    fs::remove(rollout);
    fs::remove(sessions);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "missing sessions collection retains accepted file cache");
    fs::remove(profile);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) && error == "usage_sessions_unavailable",
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
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "missing native rollout retained through typed cache seed");
    check(page->records[0].input.value == 12 && page->records[0].input.coverage->covered_records == 1 &&
        page->records[0].cache_creation.coverage->support == addon::Usage_support::UNSUPPORTED,
        "native cached category evidence preserved exactly");
    write(source, metadata("legacy") + start("turn") + tokens(23) + complete("turn"));
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records[0].total_tokens == 23, "live file advances seeded stable turn");
    fs::remove(source);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records[0].total_tokens == 23, "same consumed seed cannot overwrite newer retained counts");
    set_seed(&request, artifact, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records[0].total_tokens == 23, "empty host FILE_CACHE preserves durable package history");
    const auto cache = accepted_snapshot(storage);
    for (const auto& entry : fs::directory_iterator(cache)) {
        if (entry.path().extension() == ".records") fs::remove(entry.path());
    }
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) && error == "required_usage_cache_unavailable",
        "empty required seed plus lost retained cache fails before replacement");
    set_seed(&request, artifact, bytes);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) && error == "required_usage_cache_unavailable",
        "consumed stale seed cannot resurrect older history after cache loss");
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
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
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
    check(!codex::scan_usage(request, profile, storage, cancel_after_first_file, &error) && error == "cancelled",
        "cancel through production predicate after one native candidate file");
    check(accepted_snapshot(storage) == accepted, "cancelled native import cannot replace accepted evidence");
    const auto page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 2 && page->records[0].total_tokens + page->records[1].total_tokens == 35,
        "retry imports every native file without acknowledging the cancelled partial import");
    request.native_file_cache->sha256_hex = std::string(64, '0');
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) && error == "native_usage_cache_digest_mismatch",
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
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(baseline && baseline->records.size() == 1 && baseline->records[0].total_tokens == 17,
        "establish accepted history before whole-scan rejection");
    const auto accepted = accepted_snapshot(storage);
    write(source, metadata("retained") + start("turn") + tokens(310) + complete("turn"));
    write(duplicate, metadata("retained") + start("turn") + tokens(19) + complete("turn"));
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) && error == "duplicate_usage_record",
        "both refreshed files are rejected together when snapshot identities collide");
    check(accepted_snapshot(storage) == accepted, "failed complete candidate leaves accepted authority intact");
    fs::remove(source);
    fs::remove(duplicate);
    const auto retained = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
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
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(baseline && baseline->records.size() == 1, "establish history before a concurrent source change");

    std::string bytes = metadata("transient");
    for (unsigned index = 0; index < 500; ++index) {
        const auto turn = "turn-" + std::to_string(index);
        bytes += start(turn) + tokens(index + 1) + complete(turn);
    }
    check(bytes.size() > 65536, "fixture spans multiple rollout reader chunks");
    write(source, bytes);
    const auto snapshots = storage / "codex-usage-v1" / "snapshots";
    bool changed_after_output = false;
    const auto change_during_read = [&] {
        if (!changed_after_output) {
            for (const auto& generation : fs::directory_iterator(snapshots)) {
                for (const auto& entry : fs::directory_iterator(generation.path())) {
                    if (entry.path().extension() == ".pending-records" && entry.file_size() > 0) {
                        fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(5));
                        changed_after_output = true;
                        return false;
                    }
                }
            }
        }
        return false;
    };
    const auto retained = codex::scan_usage(request, profile, storage, change_during_read, &error);
    check(changed_after_output, "source changes only after streamed candidate records reach disk");
    check(retained && retained->incomplete_historical_coverage && retained->records.size() == 1 &&
        retained->records[0].total_tokens == 17, "transient source change preserves the accepted file evidence");
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
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 1, "establish inventory-bound cache");
    fs::remove(source);
    auto accepted = accepted_snapshot(storage);
    write(accepted / (std::string(64, 'a') + ".meta"), "{\"source\":\"unaccepted\"}\n");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "unlisted cache files cannot enter the next accepted generation");
    accepted = accepted_snapshot(storage);
    for (const auto& entry : fs::directory_iterator(accepted)) {
        if (entry.path().extension() == ".records") write(entry.path(), "");
    }
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) &&
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
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, &error) &&
        error == "required_usage_cache_unavailable", "absent required evidence is not a validated empty cache");
    auto wrapper = native_wrapper(profile / "sessions" / "rollout-empty.jsonl", "empty", 17);
    wrapper.set("records", addon::Json_value::array());
    set_seed(&request, artifact, addon::serialize_json(wrapper) + '\n');
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->complete && page->records.empty(), "valid native file with no completed turns is accepted evidence");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.empty(), "required empty native cache remains valid on seed replay");
    set_seed(&request, artifact, "");
    page = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
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
    const auto accepted = codex::scan_usage(request, profile, storage, [] { return false; }, &error);
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
        const auto result = codex::scan_usage(request, profile, storage, [] { return false; }, &error, limits);
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
        const auto retained = codex::scan_usage(replay, profile, storage, [] { return false; }, &error);
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
    check(!codex::scan_usage(request, imported_profile, imported_storage, [] { return false; }, &error, limits) &&
        error == "usage_candidate_budget_exceeded", "native import observes the same record budget before cache writes");
    const auto imported_root = imported_storage / "codex-usage-v1";
    check(fs::is_empty(imported_root / "snapshots") && !fs::exists(imported_root / "accepted"),
        "over-budget native evidence cannot publish an accepted generation");
}

} // namespace

int main()
{
    try {
        Temporary_directory fixture;
        projector_does_not_retain_ignored_large_payloads();
        reducer_keeps_turn_deltas_resets_and_fork_baselines(fixture.path / "rollout-fixture.jsonl");
        scanner_sorts_pages_and_preserves_cache_on_retry(fixture.path);
        scanner_accepts_new_profiles_and_retains_transient_empty_history(fixture.path);
        native_seed_replay_and_required_cache_loss(fixture.path);
        native_seed_cancellation_preserves_accepted_history(fixture.path);
        rejected_scan_cannot_publish_refreshed_history_after_source_deletion(fixture.path);
        transient_refresh_discards_partial_records_before_acceptance(fixture.path);
        accepted_inventory_controls_cache_replay(fixture.path);
        empty_native_cache_is_accepted_evidence(fixture.path);
        native_turn_identity_remains_validated(fixture.path / "rollout-native.jsonl");
        disk_limits_refuse_growth_without_publishing(fixture.path);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
