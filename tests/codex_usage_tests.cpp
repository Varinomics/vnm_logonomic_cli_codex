#include "codex_jsonl_projector.h"
#include "codex_usage.h"
#include "codex_usage_records.h"
#include "codex_accounts.h"
#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_repository_attribution.h>

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
    fs::path path = fs::canonical(fs::temp_directory_path()) / ("codex-usage-test-" +
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

std::string turn_context(std::string_view turn, const fs::path& cwd)
{
    auto payload = addon::Json_value::object();
    if (!turn.empty()) payload.set("turn_id", addon::Json_value(std::string(turn)));
    payload.set("cwd", addon::Json_value(addon::path_to_utf8(cwd)));
    auto event = addon::Json_value::object();
    event.set("type", addon::Json_value("turn_context"));
    event.set("payload", std::move(payload));
    return addon::serialize_json(event) + '\n';
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

void reducer_preserves_fork_turn_precision(const fs::path& path)
{
    const std::string fork_metadata =
        "{\"type\":\"session_meta\",\"payload\":{\"id\":\"fork\","
        "\"timestamp\":\"2026-01-01T00:00:00.500Z\",\"cwd\":\"\",\"forked_from_id\":\"parent\"}}\n";
    // These native UUIDv7 IDs were generated at .250, .750 and 01.100.
    const std::string inherited = "019b76da-a8fa-7000-8000-000000000001";
    const std::string first     = "019b76da-aaee-7000-8000-000000000001";
    const std::string second    = "019b76da-ac4c-7000-8000-000000000001";
    const std::string history =
        start(inherited, 1767225600) + tokens(100) + complete(inherited);
    const std::string later =
        start(second, 1767225601) + tokens(130) + complete(second);
    const auto records = reduce(
        fork_metadata + history +
        start(first, 1767225600) + tokens(120) + complete(first) + later, path);
    check(records.size() == 2 && records[0].usage.record_id == "codex:fork:" + first &&
        records[0].usage.total_tokens == 20 && records[1].usage.total_tokens == 10,
        "native seconds must not discard a local turn in the fork's creation second");

    const std::string without_seconds =
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"" + first + "\"}}\n";
    const auto precise_only = reduce(
        fork_metadata + history + without_seconds + tokens(120) + complete(first) + later, path);
    check(precise_only.size() == 2 && precise_only[0].usage.total_tokens == 20 &&
        precise_only[1].usage.total_tokens == 10,
        "UUIDv7 precision is also available when native seconds are absent");

    const auto seconds_only = reduce(
        fork_metadata + start("inherited", 1767225599) + tokens(100) + complete("inherited") +
        start("local", 1767225601) + tokens(130) + complete("local"), path);
    check(seconds_only.size() == 1 && seconds_only[0].usage.total_tokens == 30,
        "turn identities without UUIDv7 retain the native seconds fallback");
}

void reducer_keeps_categories_across_interrupted_turns(const fs::path& path)
{
    const std::string baseline = metadata() + start("first") +
        tokens(10, ",\"input_tokens\":6,\"output_tokens\":4,\"cached_input_tokens\":0") +
        complete("first");
    const std::string interrupted = start("interrupted") +
        tokens(110, ",\"input_tokens\":66,\"output_tokens\":44,\"cached_input_tokens\":20");
    const std::string next = start("next") +
        tokens(160, ",\"input_tokens\":96,\"output_tokens\":64,\"cached_input_tokens\":25") +
        complete("next");
    const auto records = reduce(baseline + interrupted + next, path);
    check(records.size() == 2, "an interrupted turn remains pending until the next completion");
    const auto& usage = records[1].usage;
    check(usage.total_tokens == 150 && usage.input.value == 90 &&
        usage.output.value == 60 && usage.cache_read.value == 25,
        "pending totals and categories span the same interrupted-turn boundary");
    check(usage.input.coverage->covered_records == 1 &&
        usage.output.coverage->covered_records == 1 &&
        usage.cache_read.coverage->covered_records == 1 && !usage.cross_source_event,
        "known pending categories stay covered without claiming one complete native turn");
    auto encoded_usage = usage;
    encoded_usage.aggregate_evidence = "partial";
    addon::Contract_failure failure;
    const auto decoded = addon::decode_usage_record(
        addon::encode_usage_record(encoded_usage, 3), &failure, 3);
    check(decoded && decoded->total_tokens == 150 && decoded->input.value == 90 &&
        decoded->output.value == 60 && decoded->cache_read.value == 25,
        "interrupted category values survive the current usage codec");
    const auto completed = reduce(baseline + interrupted + complete("interrupted") + next, path);
    check(completed.size() == 3 &&
        completed[1].usage.total_tokens + completed[2].usage.total_tokens == usage.total_tokens &&
        *completed[1].usage.input.value + *completed[2].usage.input.value == *usage.input.value &&
        *completed[1].usage.output.value + *completed[2].usage.output.value == *usage.output.value,
        "an explicit intermediate completion changes grouping, not aggregate usage");

    const auto missing = reduce(
        baseline + start("interrupted") +
        tokens(50, ",\"input_tokens\":30,\"cached_input_tokens\":5") +
        tokens(110, ",\"input_tokens\":66,\"output_tokens\":44,\"cached_input_tokens\":20") + next, path);
    check(missing.size() == 2 && missing[1].usage.total_tokens == 150 &&
        missing[1].usage.input.value == 90 && missing[1].usage.output.value == 0 &&
        missing[1].usage.output.coverage->covered_records == 0 &&
        missing[1].usage.output.coverage->support == addon::Usage_support::UNKNOWN,
        "a missing category in pending usage stays unknown after the next turn starts");

    const auto reset = reduce(
        baseline + interrupted + start("next") +
        tokens(20, ",\"input_tokens\":12,\"output_tokens\":8,\"cached_input_tokens\":3") +
        complete("next"), path);
    check(reset.size() == 2 && reset[1].usage.total_tokens == 120 &&
        reset[1].usage.input.value == 72 && reset[1].usage.output.value == 48 &&
        reset[1].usage.cache_read.value == 23,
        "a cumulative counter reset adds its new categories to interrupted pending usage");
}

void reducer_attributes_resumed_turns(const fs::path& fixture)
{
    const auto original = fixture / "original-repository";
    const auto resumed  = fixture / "resumed-repository";
    check(fs::create_directories(original / ".git"), "create original repository");
    check(fs::create_directories(resumed / ".git"), "create resumed repository");
    const auto records = reduce(
        metadata_with_cwd("resumed", original) +
        start("first") + turn_context("first", original) + tokens(1200) + complete("first") +
        start("second") + tokens(1800) + complete("second") +
        start("third") + turn_context("third", resumed) + tokens(2100) + complete("third") +
        start("fourth") + turn_context("fourth", resumed) + tokens(2500) + complete("fourth"),
        fixture / "rollout-resumed.jsonl");
    check(records.size() == 4 && records[0].usage.total_tokens == 1200 &&
        records[1].usage.total_tokens == 600 && records[2].usage.total_tokens == 300 &&
        records[3].usage.total_tokens == 400, "resume keeps the correct account-wide token deltas");
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto expected = addon::path_to_utf8(index < 2 ? original : resumed);
        const auto& record = records[index];
        check(record.recorded_cwd == expected,
            "each completed turn uses its matching native working directory");
        check(record.usage.repository_usage && record.usage.repository_usage->allocations.size() == 1 &&
            record.usage.repository_usage->allocations[0].repository_path == expected &&
            record.usage.repository_usage->allocations[0].total_tokens == record.usage.total_tokens,
            "repeated and resumed directories retain each turn's own repository allocation");
    }

    const auto invalid_context = "{\"type\":\"turn_context\",\"payload\":{\"turn_id\":42,\"cwd\":" +
        addon::serialize_json(addon::Json_value(addon::path_to_utf8(resumed))) + "}}\n";
    const auto fallback = reduce(
        metadata_with_cwd("fallback", original) +
        start("resumed") + turn_context("", resumed) + tokens(50) + complete("resumed") +
        start("missing") + turn_context("another-turn", resumed) + tokens(70) + complete("missing") +
        start("unknown") + turn_context("unknown", {}) + tokens(90) + complete("unknown") +
        start("malformed") + invalid_context + tokens(110) + complete("malformed"),
        fixture / "rollout-fallback.jsonl");
    check(fallback.size() == 4 && fallback[0].recorded_cwd == addon::path_to_utf8(resumed),
        "native contexts without a turn ID belong to the active turn");
    check(fallback[1].recorded_cwd == addon::path_to_utf8(original),
        "missing matching context falls back to session metadata rather than the preceding turn");
    check(fallback[2].recorded_cwd.empty() &&
        fallback[2].usage.repository_usage->allocations.empty() &&
        fallback[2].usage.repository_usage->unattributed_tokens == 20,
        "a supplied unknown working directory cannot inherit the original repository");
    check(fallback[3].recorded_cwd == addon::path_to_utf8(original),
        "a malformed native turn ID cannot supply attribution for the active turn");
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

void select_cache(addon::Usage_scan_request& request, const fs::path& storage,
    const char* name, std::optional<addon::Usage_cache_reference> accepted = std::nullopt)
{
    const auto candidate = storage / name;
    check(fs::create_directory(candidate), "allocate an exclusive current cache candidate");
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    request.snapshot_id.reset();
    request.cursor.reset();
    request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
}

void selected_cache_preserves_previous_rollout_order(const fs::path& fixture)
{
    const auto profile = fixture / "rollout-order-profile";
    const auto storage = fixture / "rollout-order-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto hash = [](std::string_view value) { return addon::to_hex(addon::Sha256::digest(value)); };
    std::map<std::string, fs::path> paths;
    std::vector<std::string> stable_order;
    // Select a real prior ordering that differs from ordering by source alone.
    // The absolute temporary root is part of each digest.
    for (int index = 0; index < 100 && std::is_sorted(stable_order.begin(), stable_order.end()); ++index) {
        const auto path = profile / "sessions" / ("rollout-order-" + std::to_string(index) + ".jsonl");
        auto identity = addon::historical_account_profile_identity(path, path);
        check(identity.has_value(), "derive the preexisting rollout key");
#ifdef _WIN32
        std::replace(identity->begin(), identity->end(), '\\', '/');
#endif
        paths.emplace(hash("profile-" + hash(*identity)), path);
        stable_order.clear();
        for (const auto& entry : paths) stable_order.push_back(hash(addon::path_to_utf8(entry.second)));
    }
    check(!std::is_sorted(stable_order.begin(), stable_order.end()), "exercise a changed rollout-key order");
    int index = 0;
    for (const auto& entry : paths) {
        const auto session = "order-session-" + std::to_string(++index);
        write(entry.second, metadata(session) + start("turn-a") + tokens(13) + complete("turn-a"));
    }
    addon::Usage_scan_request request;
    request.mode = "incremental";
    request.context.account_context = addon::Account_context{1, "order-account"};
    select_cache(request, storage, "initial");
    std::string error;
    const auto scan = [&] { return codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error); };
    const auto initial = scan();
    check(initial && initial->records.size() == paths.size(), "read one native turn per rollout");

    const auto source = storage / "initial";
    const auto historical = storage / "historical";
    fs::create_directory(historical);
    for (const auto* name : {"records", "snapshot"}) fs::copy_file(source / name, historical / name);
    std::ifstream state_file(source / "cache-state", std::ios::binary);
    std::string line;
    std::getline(state_file, line);
    auto state = addon::parse_json(line, {}, nullptr);
    check(state.has_value(), "read the cache state");
    state->erase("rollout_order");
    write(historical / "cache-state", addon::serialize_json(*state) + '\n');
    std::map<fs::path, std::string> metadata_by_path;
    std::ifstream rollout_file(source / "rollouts", std::ios::binary);
    while (std::getline(rollout_file, line)) {
        const auto row = addon::parse_json(line, {}, nullptr);
        check(row && row->find("source") && row->find("source")->is_string(), "read rollout metadata");
        metadata_by_path.emplace(addon::path_from_utf8(row->find("source")->as_string()), line);
    }
    std::string old_lines;
    for (const auto& entry : paths) old_lines += metadata_by_path.at(entry.second) + '\n';
    write(historical / "rollouts", old_lines);
    addon::Contract_failure failure;
    const auto accepted = addon::seal_usage_cache_candidate(addon::path_to_utf8(historical),
        addon::path_to_utf8(storage), request.context.account_context, initial->snapshot_id,
        "codex-rollout-cache@3", [] { return false; }, &failure);
    check(accepted.has_value(), "seal the genuine prior source order");
#ifdef _WIN32
    const auto alias = fixture / "ROLLOUT-ORDER-PROFILE";
#else
    const auto alias = fixture / "profile-alias";
    fs::create_directory_symlink(profile, alias);
#endif
    std::error_code alias_error;
    if (fs::equivalent(profile, alias, alias_error)) {
        select_cache(request, storage, "aliased", accepted);
        const auto rediscovered = codex::scan_usage(request, alias, storage,
            [] { return false; }, quiet_progress, &error);
        check(rediscovered && rediscovered->records.size() == paths.size(),
            "a newly spelled account prefix does not duplicate the original native turns");
    }
    for (const auto& entry : paths) fs::remove(entry.second);
    select_cache(request, storage, "migrated", accepted);
    const auto migrated = scan();
    check(migrated && migrated->records.size() == paths.size() && migrated->cache_candidate,
        error.empty() ? "retain every turn after removal of the original rollout files" : error.c_str());
    select_cache(request, storage, "reused", migrated->cache_candidate);
    const auto reused = scan();
    check(reused && reused->records.size() == paths.size(), "reuse the migrated rollout order");
    for (std::size_t row = 0; row < initial->records.size(); ++row) {
        check(reused->records[row].record_id == initial->records[row].record_id &&
            reused->records[row].total_tokens == 13, "keep the original turn and amount through reindexing");
    }
}

void selected_cache_reparses_previous_parser_revision(const fs::path& fixture)
{
    const auto profile = fixture / "parser-revision-profile";
    const auto storage = fixture / "parser-revision-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-fork.jsonl";
    const std::string inherited = "019b76da-a8fa-7000-8000-000000000001";
    const std::string local = "019b76da-aaee-7000-8000-000000000001";
    const std::string second = "019b76da-ac4c-7000-8000-000000000001";
    write(source, "{\"type\":\"session_meta\",\"payload\":{\"id\":\"fork\","
        "\"timestamp\":\"2026-01-01T00:00:00.500Z\",\"cwd\":\"\",\"forked_from_id\":\"parent\"}}\n" +
        start(inherited, 1767225600) + tokens(100) + complete(inherited) +
        start(local, 1767225600) + tokens(120) + complete(local) +
        start(second, 1767225601) + tokens(130) + complete(second));
    addon::Usage_scan_request request;
    request.mode = "incremental";
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    request.context.account_context = addon::Account_context{1, "parser-revision-account"};
    const auto initial = storage / "initial";
    fs::create_directory(initial);
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(initial)};
    std::string error;
    const auto first = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(first && first->complete && first->records.size() == 2 && first->cache_candidate,
        error.empty() ? "establish precise fork accounting" : error.c_str());

    // Reconstruct revision 1's omitted same-second local turn without changing
    // the native file or its cached fingerprint and temporal parser marker.
    const auto historical = storage / "historical";
    fs::create_directory(historical);
    for (const auto* name : {"snapshot", "cache-state"}) {
        check(fs::copy_file(initial / name, historical / name), "copy prior parser cache metadata");
    }
    for (const auto* name : {"rollouts", "records"}) {
        std::ifstream input(initial / name, std::ios::binary);
        check(static_cast<bool>(input), "read prior parser fixture");
        std::string replacement;
        for (std::string line; std::getline(input, line);) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "decode prior parser fixture");
            if (std::string_view(name) == "rollouts") {
                row->set("revision", addon::Json_value(1));
            }
            else {
                const auto record = addon::decode_usage_record(*row->find("usage"), nullptr, 3);
                check(record.has_value(), "decode prior fork record");
                if (record->record_id == "codex:fork:" + local) continue;
            }
            replacement += addon::serialize_json(*row) + '\n';
        }
        write(historical / name, replacement);
    }
    addon::Contract_failure failure;
    const auto accepted = addon::seal_usage_cache_candidate(addon::path_to_utf8(historical),
        addon::path_to_utf8(storage), request.context.account_context, first->snapshot_id,
        "codex-rollout-cache@3", [] { return false; }, &failure);
    check(accepted.has_value(), "seal the prior parser's incomplete accounting");
    const auto refreshed = storage / "refreshed";
    fs::create_directory(refreshed);
    request.cache_selection = addon::Usage_cache_selection{accepted, addon::path_to_utf8(refreshed)};
    const auto update = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(update && update->changes && update->changes->complete && update->changes->cache_candidate &&
        update->changes->records.size() == 1 &&
        update->changes->records.front().record_id == "codex:fork:" + local &&
        update->changes->records.front().total_tokens == 20,
        error.empty() ? "an unchanged revision 1 rollout recovers its missing local turn" : error.c_str());
    const auto reused = storage / "reused";
    fs::create_directory(reused);
    request.cache_selection = addon::Usage_cache_selection{
        update->changes->cache_candidate, addon::path_to_utf8(reused)};
    const auto unchanged = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(unchanged && !unchanged->changes, "the refreshed parser cache is reused without another change");
}

void scanner_keeps_per_turn_repositories(const fs::path& fixture)
{
    const auto root     = fixture / "turn-selected";
    const auto profile  = root / "profile";
    const auto storage  = root / "storage";
    const auto original = root / "original";
    const auto resumed  = root / "resumed";
    check(fs::create_directories(profile / "sessions"), "create turn profile");
    check(fs::create_directory(storage), "create turn cache storage");
    check(fs::create_directories(original / ".git"), "create original turn repository");
    check(fs::create_directories(resumed / ".git"), "create resumed turn repository");
    const auto source = profile / "sessions" / "rollout-per-turn.jsonl";
    const auto history = metadata_with_cwd("per-turn", original) +
        start("first") + turn_context("first", original) + tokens(1200) + complete("first") +
        start("second") + turn_context("second", resumed) + tokens(1800) + complete("second");
    write(source, history);
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "turn-account"};
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    std::optional<addon::Usage_cache_reference> accepted;
    for (unsigned generation = 0; generation < 3; ++generation) {
        const auto candidate = storage / ("generation-" + std::to_string(generation));
        check(fs::create_directory(candidate), "allocate turn cache candidate");
        request.cache_selection = addon::Usage_cache_selection{accepted, addon::path_to_utf8(candidate)};
        if (generation == 1) {
            write(source, history + start("third") + tokens(1875) + complete("third"));
        }
        std::string error;
        const auto page = codex::scan_usage(request, profile, storage,
            [] { return false; }, quiet_progress, &error);
        check(page && page->complete && page->records.size() == (generation == 0 ? 2 : 3),
            error.empty() ? "per-turn scanner retains each completed turn" : error.c_str());
        check(page->records[0].total_tokens == 1200 && page->records[1].total_tokens == 600,
            "cache refresh retains the account totals");
        for (const auto& record : page->records) {
            const auto& expected = record.record_id == "codex:per-turn:second" ? resumed : original;
            check(record.repository_usage && record.repository_usage->allocations.size() == 1 &&
                record.repository_usage->allocations[0].repository_path == addon::path_to_utf8(expected) &&
                record.repository_usage->allocations[0].total_tokens == record.total_tokens,
                "fresh, changed and reused caches preserve heterogeneous turn directories");
        }
        accepted = page->cache_candidate;
    }
}


void selected_cache_repairs_turn_repository(const fs::path& fixture)
{
    const auto profile  = fixture / "repair-profile";
    const auto storage  = fixture / "repair-storage";
    const auto original = fixture / "repair-original";
    const auto resumed  = fixture / "repair-resumed";
    check(fs::create_directories(profile / "sessions"), "create attribution repair profile");
    check(fs::create_directory(storage), "create attribution repair storage");
    check(fs::create_directories(original / ".git"), "create repair original repository");
    check(fs::create_directories(resumed / ".git"), "create repair resumed repository");
    const auto source = profile / "sessions" / "rollout-repair.jsonl";
    const auto transcript = metadata_with_cwd("repair", original) +
        start("first") + turn_context("first", original) + tokens(1200) + complete("first") +
        start("second") + turn_context("second", resumed) + tokens(1800) + complete("second");
    write(source, transcript);
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "repair-account"};
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    const auto initial_root = storage / "initial";
    check(fs::create_directory(initial_root), "allocate initial attribution cache");
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(initial_root)};
    std::string error;
    const auto first = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(first && first->complete && first->records.size() == 2 && first->cache_candidate,
        error.empty() ? "establish attribution cache fixture" : error.c_str());

    // Reconstruct the accepted record produced before native turn cwd was read.
    // Keep its correct accounting and its unchanged native source fingerprint.
    const auto historical_root = storage / "historical";
    check(fs::create_directory(historical_root), "allocate historical attribution cache");
    for (const auto* name : {"rollouts", "snapshot", "cache-state"}) {
        check(fs::copy_file(initial_root / name, historical_root / name), "copy historical cache metadata");
    }
    std::ifstream input(initial_root / "records", std::ios::binary);
    std::string line;
    std::string records;
    addon::Usage_record historical_turn;
    while (std::getline(input, line)) {
        auto row = addon::parse_json(line, {}, nullptr);
        check(row.has_value(), "read historical attribution record");
        auto usage = addon::decode_usage_record(*row->find("usage"), nullptr, 3);
        check(usage.has_value(), "decode historical attribution record");
        if (usage->record_id == "codex:repair:second") {
            usage->repository_usage = addon::Usage_repository{{{addon::path_to_utf8(original), 600}}, 0};
            row->set("usage", addon::encode_usage_record(*usage, 3));
            row->set("recorded_cwd", addon::Json_value(addon::path_to_utf8(original)));
            historical_turn = *usage;
            auto accounting = *row;
            accounting.erase("temporal_observation");
            row->find("temporal_observation")->set("accounting_sha256_hex", addon::Json_value(
                addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(accounting)))));
        }
        records += addon::serialize_json(*row) + '\n';
    }
    input.close();
    write(historical_root / "records", records);
    addon::Contract_failure failure;
    const auto historical = addon::seal_usage_cache_candidate(addon::path_to_utf8(historical_root),
        addon::path_to_utf8(storage), request.context.account_context, first->snapshot_id,
        "codex-rollout-cache@3", [] { return false; }, &failure);
    check(historical.has_value(), "seal historically incorrect repository allocation");
    using Authority = codex::Usage_scan_authority;
    const auto scan = [&](const char* name, const std::optional<addon::Usage_cache_reference>& accepted,
        Authority authority, const char* mode = "repair")
        {
            const auto candidate = storage / name;
            check(fs::create_directory(candidate), "allocate attribution candidate");
            request.mode = mode;
            request.snapshot_id.reset();
            request.cursor.reset();
            request.cache_selection = addon::Usage_cache_selection{accepted, addon::path_to_utf8(candidate)};
            error.clear();
            auto page = codex::scan_usage(request, profile, storage,
                [] { return false; }, quiet_progress, &error, {}, authority);
            check(page && page->complete && page->records.size() == 2 && page->cache_candidate,
                error.empty() ? "repair retains accepted turns" : error.c_str());
            return *page;
        };
    const auto tagged = [](const addon::Usage_record& record) {
        const auto* tag = record.capabilities.find(addon::k_usage_repository_attribution_capability_id);
        return tag && addon::decode_usage_repository_attribution_tag(*tag, nullptr) ==
            addon::Usage_repository_attribution_tag_status::AVAILABLE;
    };
    const auto attributed = [&](const addon::Usage_record& record, const fs::path& repository) {
        return record.repository_usage && record.repository_usage->allocations.size() == 1 &&
            record.repository_usage->allocations[0].repository_path == addon::path_to_utf8(repository) &&
            record.repository_usage->allocations[0].total_tokens == record.total_tokens &&
            record.repository_usage->unattributed_tokens == 0;
    };
    const auto ordinary = scan("ordinary", historical, Authority::STANDARD);
    check(attributed(ordinary.records[1], original) && !tagged(ordinary.records[1]),
        "ordinary repair preserves accepted attribution without correction authority");
    const auto repaired = scan("repair", historical, Authority::REPOSITORY_ATTRIBUTION);
    check(repaired.records[0].total_tokens == 1200 && repaired.records[1].total_tokens == 600,
        "attribution repair keeps accepted account totals");
    check(attributed(repaired.records[1], resumed) && tagged(repaired.records[1]),
        "repair replaces the historical repository with the matching native turn repository");
    const auto accounting = [](addon::Usage_record record) {
        record.repository_usage.reset();
        record.temporal.reset();
        record.capabilities.erase(addon::k_usage_temporal_capability_id);
        record.capabilities.erase(addon::k_usage_repository_attribution_capability_id);
        return addon::encode_usage_record(record, 3);
    };
    check(accounting(repaired.records[1]) == accounting(historical_turn),
        "repository repair preserves accepted numeric, date, category and provenance fields");
    check(repaired.records[1].temporal && repaired.records[1].temporal->portions.size() == 1 &&
        repaired.records[1].temporal->portions.front().instant == first->records[1].temporal->portions.front().instant,
        "repository repair retains the accepted temporal instant");

    request.snapshot_id = repaired.snapshot_id;
    request.cursor = "0";
    error.clear();
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) &&
        error == "usage_snapshot_mismatch", "ordinary continuation cannot export an attribution candidate");
    const auto continuation = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error, {}, Authority::REPOSITORY_ATTRIBUTION);
    check(continuation && addon::encode_usage_scan_result(*continuation, 3) ==
        addon::encode_usage_scan_result(repaired, 3), "the selected operation replays its sealed candidate");

    const auto full = scan("full-incremental", historical, Authority::REPOSITORY_ATTRIBUTION, "incremental");
    request.snapshot_id = full.snapshot_id;
    request.cursor = "0";
    error.clear();
    check(!codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error, {}, Authority::REPOSITORY_ATTRIBUTION) &&
        error == "usage_snapshot_mismatch", "incremental cannot adopt a full-scan continuation");

    request.snapshot_id.reset();
    request.cursor.reset();
    const auto unchanged_root = storage / "unchanged-incremental";
    check(fs::create_directory(unchanged_root), "allocate unchanged attribution update");
    request.cache_selection = addon::Usage_cache_selection{historical, addon::path_to_utf8(unchanged_root)};
    const auto unchanged = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error, {}, Authority::REPOSITORY_ATTRIBUTION);
    check(unchanged && !unchanged->changes,
        "selecting attribution authority does not reread unchanged historical sources");
    write(source, transcript + tokens(1800));
    const auto changed_root = storage / "changed-incremental";
    check(fs::create_directory(changed_root), "allocate changed attribution update");
    request.cache_selection->candidate_root = addon::path_to_utf8(changed_root);
    const auto changed = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error, {}, Authority::REPOSITORY_ATTRIBUTION);
    check(changed && changed->changes, error.empty() ? "changed attribution scan supplies an update" : error.c_str());
    check(changed->changes->complete && changed->changes->records.size() == 1 &&
        attributed(changed->changes->records.front(), resumed) && tagged(changed->changes->records.front()),
        "naturally changed sources emit only the witnessed repository correction");
    request.snapshot_id = changed->changes->snapshot_id;
    request.cursor = "0";
    error.clear();
    check(!codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error, {}, Authority::REPOSITORY_ATTRIBUTION) &&
        error == "usage_snapshot_mismatch", "full scan cannot export an incremental candidate");
    error.clear();
    check(!codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error) && error == "usage_snapshot_mismatch",
        "ordinary incremental cannot export an attribution candidate");

    for (const auto* mode : {"fallback", "unknown", "unresolved", "same-cwd"}) {
        std::string context;
        if (std::string_view(mode) == "unknown") context = turn_context("second", {});
        if (std::string_view(mode) == "unresolved") context = turn_context("second", fixture / "missing-checkout");
        if (std::string_view(mode) == "same-cwd") {
            check(fs::remove(original / ".git"), "remove original Git marker");
            context = turn_context("second", original);
        }
        write(source, metadata_with_cwd("repair", original) + start("first") + tokens(1200) + complete("first") +
            start("second") + context + tokens(1800) + complete("second"));
        const auto retained = scan(mode, historical, Authority::REPOSITORY_ATTRIBUTION);
        check(attributed(retained.records[1], original) && !tagged(retained.records[1]),
            "fallback, unknown, unresolved or vanished same-cwd evidence cannot retract accepted attribution");
    }
    check(fs::create_directory(original / ".git"), "restore original Git marker");

    write(source, metadata_with_cwd("repair", original) +
        start("second") + turn_context("second", resumed) + tokens(1800) + complete("second"));
    const auto unproved = scan("unproved", historical, Authority::REPOSITORY_ATTRIBUTION);
    check(unproved.records[0].total_tokens == 1200 && unproved.records[1].total_tokens == 600 &&
        attributed(unproved.records[1], resumed),
        "missing baseline cannot inflate accepted accounting during witnessed repository repair");

    write(source, metadata_with_cwd("repair", original) + start("first") + tokens(1200) + complete("first") +
        start("second") + turn_context("second", resumed) + tokens(2000) + complete("second"));
    for (const auto authority : {Authority::STANDARD, Authority::REPOSITORY_ATTRIBUTION}) {
        const auto grown = scan(authority == Authority::STANDARD ? "ordinary-growth" : "selected-growth",
            historical, authority);
        const auto& second = grown.records[1];
        check(second.total_tokens == 800 && second.repository_usage &&
            second.repository_usage->allocations.size() == 1 &&
            second.repository_usage->allocations[0].repository_path == addon::path_to_utf8(original) &&
            second.repository_usage->allocations[0].total_tokens == 600 &&
            second.repository_usage->unattributed_tokens == 200 && !tagged(second),
            "proved growth preserves prior known allocation and defers repository correction");
        const auto corrected = scan(authority == Authority::STANDARD ? "after-ordinary-growth" : "after-selected-growth",
            grown.cache_candidate, Authority::REPOSITORY_ATTRIBUTION);
        check(corrected.records[1].total_tokens == 800 && attributed(corrected.records[1], resumed) &&
            tagged(corrected.records[1]), "accepted growth can be corrected on a later matching observation");
    }
    write(source, metadata_with_cwd("repair", original) + start("first") + tokens(1200) + complete("first") +
        start("second") + tokens(2000) + complete("second"));
    for (const auto authority : {Authority::STANDARD, Authority::REPOSITORY_ATTRIBUTION}) {
        const auto grown = scan(authority == Authority::STANDARD ? "fallback-growth" : "selected-fallback-growth",
            repaired.cache_candidate, authority);
        const auto& second = grown.records[1];
        check(second.total_tokens == 800 && second.repository_usage &&
            second.repository_usage->allocations.size() == 1 &&
            second.repository_usage->allocations[0].repository_path == addon::path_to_utf8(resumed) &&
            second.repository_usage->allocations[0].total_tokens == 600 &&
            second.repository_usage->unattributed_tokens == 200 && !tagged(second),
            "proved growth after lost turn context preserves accepted attribution");
    }
    write(source, metadata_with_cwd("repair", original) + start("first") + tokens(1200) + complete("first") +
        start("second") + turn_context("second", resumed) + tokens(1700) + complete("second"));
    const auto lower = scan("lower", historical, Authority::REPOSITORY_ATTRIBUTION);
    check(lower.records[1].total_tokens == 500 && !tagged(lower.records[1]) &&
        lower.records[1].aggregate_evidence == "partial", "a lower total has no repository correction authority");

    check(fs::remove(source), "remove repair source");
    const auto missing = scan("missing", historical, Authority::REPOSITORY_ATTRIBUTION);
    check(attributed(missing.records[1], original) && !tagged(missing.records[1]),
        "missing source keeps historical accepted attribution");
    const auto replay = scan("replay", repaired.cache_candidate, Authority::REPOSITORY_ATTRIBUTION);
    check(attributed(replay.records[1], resumed) && !tagged(replay.records[1]),
        "replay retains corrected attribution without a stale correction request");
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
    select_cache(request, storage, "first");
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
    check(retry && addon::encode_usage_scan_result(*retry, 3) == addon::encode_usage_scan_result(*final, 3), "continuation retry is stable");
    select_cache(request, storage, "malformed", first->cache_candidate);
    write(path, bytes + "{broken json}\n");
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error), "malformed changed history fails snapshot");
    fs::remove(path);
    select_cache(request, storage, "retained", first->cache_candidate);
    const auto retained = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(retained && retained->records.size() == 256, "disappeared source retains last valid history");
    select_cache(request, storage, "cancelled", retained->cache_candidate);
    unsigned polls = 0;
    check(!codex::scan_usage(request, profile, storage, [&] { return ++polls > 8; }, quiet_progress, &error) && error == "cancelled",
        "cancellation discards pending snapshot");
    check(!fs::exists(storage / "cancelled" / "cache-manifest.json"),
        "cancelled candidate remains unsealed");
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(*retained->cache_candidate, addon::path_to_utf8(storage),
        request.context.account_context, [] { return false; }, &failure).has_value(),
        "cancellation leaves the selected accepted history intact");
}

void scanner_accepts_new_profiles_and_retains_transient_empty_history(const fs::path& fixture)
{
    const auto profile = fixture / "new-profile";
    const auto storage = fixture / "new-profile-storage";
    fs::create_directory(profile);
    fs::create_directory(storage);
    addon::Usage_scan_request request;
    std::string error;
    select_cache(request, storage, "empty");
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->complete && page->records.empty(), "existing new account without sessions is an empty successful scan");
    const auto sessions = profile / "sessions";
    write(sessions, "not a directory");
    select_cache(request, storage, "invalid-sessions", page->cache_candidate);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "usage_sessions_not_directory",
        "an existing non-directory session path remains invalid");
    fs::remove(sessions);
    fs::create_directory(sessions);
    const auto rollout = sessions / "rollout-new.jsonl";
    write(rollout, "");
    select_cache(request, storage, "truncated", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.empty() && page->incomplete_historical_coverage, "first empty rollout is transient, not invalid history");
    check(fs::file_size(addon::path_from_utf8(page->cache_candidate->root) / "rollouts") == 0,
        "empty source does not manufacture a cache entry");
    write(rollout, metadata() + start("turn") + tokens(17) + complete("turn"));
    select_cache(request, storage, "populated", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17, "populated rollout establishes accepted history");
    write(rollout, "");
    select_cache(request, storage, "truncated-again", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17 && page->incomplete_historical_coverage,
        "temporarily truncated rollout preserves accepted usage");
    fs::remove(rollout);
    fs::remove(sessions);
    select_cache(request, storage, "source-absent", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17,
        "missing sessions collection retains accepted file cache");
    fs::remove(profile);
    select_cache(request, storage, "profile-absent", page->cache_candidate);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "usage_sessions_unavailable",
        "missing profile itself remains invalid");
}

std::string utf8(const fs::path& path)
{
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

void scanner_reads_archived_rollouts(const fs::path& fixture)
{
    const auto root    = fixture / "as";
    const auto profile = root / "profile";
    const auto storage = root / "storage";
    const auto archive = profile / "archived_sessions";
    check(fs::create_directories(archive / "2026"), "create archived-only native history");
    check(fs::create_directory(storage), "create archive scan storage");
    const auto historical = archive / "2026" / "rollout-history.jsonl";
    write(historical, metadata("history") + start("first") + tokens(1200) + complete("first"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "archive-account"};
    std::optional<addon::Usage_cache_reference> accepted;
    std::string error;
    unsigned generation = 0;
    const auto scan = [
            &request, &profile, &storage, &accepted, &error, &generation
        ](const char* mode)
        {
            request.mode = mode;
            const auto candidate = storage / ("candidate-" + std::to_string(generation++));
            check(fs::create_directory(candidate), "allocate archive scan candidate");
            request.cache_selection = addon::Usage_cache_selection{
                std::string_view(mode) == "reset" ? std::nullopt : accepted, utf8(candidate)};
            const auto page = codex::scan_usage(request, profile, storage,
                [] { return false; }, quiet_progress, &error);
            check(page && page->complete && !page->incomplete_historical_coverage,
                error.empty() ? "archive scan completes" : error.c_str());
            accepted = page->cache_candidate;
            return *page;
        };
    const auto totals = [](const addon::Usage_scan_page& page) {
        std::map<std::string, std::uint64_t> result;
        for (const auto& record : page.records) {
            check(result.emplace(record.record_id, record.total_tokens).second,
                "each native turn appears exactly once");
        }
        return result;
    };
    const std::map<std::string, std::uint64_t> history{{"codex:history:first", 1200}};
    check(totals(scan("incremental")) == history,
        "fresh import discovers archived usage without a sessions directory");
    check(totals(scan("reset")) == history, "reset discovers archived native history");

    const auto active = profile / "sessions" / "rollout-active.jsonl";
    check(fs::create_directory(active.parent_path()), "create active native history");
    const auto active_bytes = metadata("active") + start("second") + tokens(600) + complete("second");
    write(active, active_bytes);
    std::map<std::string, std::uint64_t> expected = history;
    expected["codex:active:second"] = 600;
    check(totals(scan("incremental")) == expected, "active and archived roots contribute to one scan");

    const auto moved = archive / active.filename();
    fs::rename(active, moved);
    const auto archived = scan("incremental");
    check(totals(archived) == expected, "an archive move keeps stable turn IDs and tokens");
    const auto accepted_turn = std::find_if(archived.records.begin(), archived.records.end(), [](const auto& record) {
        return record.record_id == "codex:active:second";
    });
    check(accepted_turn != archived.records.end() && accepted_turn->source_path == utf8(fs::absolute(active)),
        "archiving retains the accepted observation's source identity");

    write(active, active_bytes);
    check(totals(scan("repair")) == expected, "overlapping active and archive copies count once");
    std::optional<addon::Usage_cache_reference> unindexed;
    const auto earlier = storage / "unindexed";
    check(fs::create_directory(earlier), "allocate cache without a native-session index");
    const auto current = addon::path_from_utf8(accepted->root);
    for (const auto* name : {"snapshot", "cache-state", "records"}) {
        check(fs::copy_file(current / name, earlier / name), "copy accepted accounting evidence");
    }
    {
        std::ifstream input(current / "rollouts", std::ios::binary);
        std::ofstream output(earlier / "rollouts", std::ios::binary);
        check(input && output, "open unindexed rollout metadata");
        std::string line;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "decode accepted rollout metadata");
            row->erase("native_session");
            output << addon::serialize_json(*row) << '\n';
        }
        check(input.eof() && output, "write unindexed rollout metadata");
    }
    addon::Contract_failure failure;
    unindexed = addon::seal_usage_cache_candidate(utf8(earlier), utf8(storage), request.context.account_context,
        accepted->snapshot_id, "codex-rollout-cache@3", [] { return false; }, &failure);
    check(unindexed.has_value(), "seal saved accounting without a native-session index");
    accepted = unindexed;
    check(totals(scan("incremental")) == expected,
        "a cache without the native-session index retains accepted accounting");
    check(addon::validate_usage_cache_candidate(*unindexed, utf8(storage), request.context.account_context,
        [] { return false; }, &failure).has_value(), "an unindexed selected input stays intact");
    write(active, metadata("active") + start("second") + tokens(700) + complete("second"));
    auto conflicting = request;
    conflicting.mode = "incremental";
    const auto conflicting_candidate = storage / "conflicting";
    check(fs::create_directory(conflicting_candidate), "allocate conflicting archive candidate");
    conflicting.cache_selection = addon::Usage_cache_selection{accepted, utf8(conflicting_candidate)};
    const auto collision = codex::scan_usage(conflicting, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(!collision, "incremental scan accepted conflicting current active and archive copies");
    check(error == "duplicate_usage_record", "conflicting copies report duplicate usage");
    const auto unindexed_candidate = storage / "unindexed-conflict";
    check(fs::create_directory(unindexed_candidate), "allocate unindexed conflict candidate");
    conflicting.cache_selection = addon::Usage_cache_selection{unindexed, utf8(unindexed_candidate)};
    check(!codex::scan_usage(conflicting, profile, storage,
        [] { return false; }, quiet_progress, &error) && error == "duplicate_usage_record",
        "an unindexed cache compares conflicting current copies against each other");
    write(active, active_bytes);
    check(totals(scan("incremental")) == expected, "a rejected copy leaves accepted history usable");
    check(totals(scan("reset")) == expected, "fresh reconstruction deduplicates both native roots");
    check(fs::remove(active), "remove the overlapping active copy");
    write(moved, active_bytes + start("third") + tokens(900) + complete("third"));
    expected["codex:active:third"] = 300;
    check(totals(scan("incremental")) == expected, "an archived rollout's new turn contributes its delta");

    write(moved, active_bytes + start("third") + tokens(900) + complete("third") +
        "{\"type\":\"response_item\",\"payload\":{\"content\":[]}}\n");
    const auto piece_candidate = storage / "piece-candidate";
    check(fs::create_directory(piece_candidate), "allocate archive piece-cache candidate");
    request.cache_selection = addon::Usage_cache_selection{accepted, utf8(piece_candidate)};
    const auto update = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(update && update->changes && update->changes->records.empty(),
        "adopting archived history into record pieces preserves accounting");
    accepted = update->changes->cache_candidate;
    const auto four_turns = active_bytes + start("third") + tokens(900) + complete("third") +
        start("fourth") + tokens(1400) + complete("fourth");
    write(moved, four_turns);
    const auto next = storage / "piece-next";
    check(fs::create_directory(next), "allocate changed archive piece-cache candidate");
    request.cache_selection = addon::Usage_cache_selection{accepted, utf8(next)};
    const auto changed = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(changed && changed->changes && changed->changes->records.size() == 1 &&
        changed->changes->records.front().record_id == "codex:active:fourth" &&
        changed->changes->records.front().total_tokens == 500,
        "piece-cache incremental discovery reads appended archived turns once");
    accepted = changed->changes->cache_candidate;
    expected["codex:active:fourth"] = 500;
    write(active, four_turns);
    const auto overlap = storage / "piece-overlap";
    check(fs::create_directory(overlap), "allocate overlapping piece-cache candidate");
    request.cache_selection = addon::Usage_cache_selection{accepted, utf8(overlap)};
    const auto copies = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(copies && copies->changes && copies->changes->records.empty(),
        "piece-cache incremental overlap preserves each accepted turn once");
    accepted = copies->changes->cache_candidate;
    write(moved, active_bytes + start("third") + tokens(900) + complete("third") +
        start("fourth") + tokens(1500) + complete("fourth"));
    const auto conflict = storage / "piece-conflict";
    check(fs::create_directory(conflict), "allocate conflicting piece-cache candidate");
    request.cache_selection = addon::Usage_cache_selection{accepted, utf8(conflict)};
    check(!codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error) && error == "duplicate_usage_record",
        "piece-cache incremental scans compare changed archives with unchanged active copies");
    write(moved, four_turns);
    check(fs::remove(active), "remove the overlapping piece-cache source");
    check(fs::remove(moved) && fs::remove(historical), "remove native histories after acceptance");
    check(totals(scan("incremental")) == expected, "absent native sources retain accepted history");
}

void current_cache_replay_and_loss(const fs::path& fixture)
{
    const auto profile = fixture / "replay-profile";
    const auto storage = fixture / "replay-storage";
    fs::create_directories(profile / "sessions"); fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-retained.jsonl";
    // A pre-turn sample proves that a larger reread belongs to this turn.
    const auto prefix = metadata("retained") +
        tokens(0, ",\"input_tokens\":0,\"output_tokens\":0,\"cached_input_tokens\":0") + start("turn");
    write(source, prefix +
        tokens(17, ",\"input_tokens\":12,\"output_tokens\":5,\"cached_input_tokens\":0") + complete("turn"));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    std::string error;
    select_cache(request, storage, "first");
    auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17 &&
        page->records[0].input.value == 12 && page->records[0].output.value == 5,
        "current rollout accounting and category evidence are accepted");
    check(page->records[0].cross_source_event &&
        page->records[0].cross_source_event->authority_namespace == "openai.codex.completed-turn" &&
        page->records[0].cross_source_event->event_id == "retained:turn",
        "the pre-turn baseline proves the completed turn");
    fs::remove(source);
    select_cache(request, storage, "retained", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 17 &&
        page->records[0].input.value == 12 && page->records[0].output.value == 5,
        "accepted current cache retains missing-rollout accounting and categories");
    write(source, prefix + tokens(23) + complete("turn"));
    select_cache(request, storage, "updated", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 1 && page->records[0].total_tokens == 23 &&
        page->records[0].record_id == "codex:retained:turn" && page->records[0].cross_source_event &&
        page->records[0].cross_source_event->authority_namespace == "openai.codex.completed-turn" &&
        page->records[0].cross_source_event->event_id == "retained:turn",
        "proved live file advances the accepted stable turn");
    fs::remove(source);
    select_cache(request, storage, "updated-retained", page->cache_candidate);
    page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records[0].total_tokens == 23, "fresh scans retain the latest accepted observation");
    const auto accepted = page->cache_candidate;
    check(fs::remove(addon::path_from_utf8(accepted->root) / "records"), "remove accepted accounting file");
    select_cache(request, storage, "lost-input", accepted);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "usage_cache_unavailable",
        "lost accepted cache files fail before replacement");
}

void current_scan_cancellation_preserves_accepted_history(const fs::path& fixture)
{
    const auto profile = fixture / "scan-cancel-profile";
    const auto storage = fixture / "scan-cancel-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "account"};
    std::string error;
    select_cache(request, storage, "baseline");
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(baseline && baseline->records.empty(), "establish accepted empty history");
    const auto accepted = baseline->cache_candidate;
    write(profile / "sessions" / "rollout-a.jsonl", metadata("a") + start("turn") + tokens(17) + complete("turn"));
    write(profile / "sessions" / "rollout-b.jsonl", metadata("b") + start("turn") + tokens(18) + complete("turn"));
    select_cache(request, storage, "cancelled", accepted);
    const auto spool = storage / "cancelled" / "rollout-records";
    bool interrupted = false;
    const auto cancel_after_first_file = [&] {
        if (fs::exists(spool) && fs::file_size(spool) != 0) interrupted = true;
        return interrupted;
    };
    check(!codex::scan_usage(request, profile, storage, cancel_after_first_file, quiet_progress, &error) &&
        interrupted && error == "cancelled", "cancel through production predicate after current candidate output");
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(*accepted, addon::path_to_utf8(storage),
        request.context.account_context, [] { return false; }, &failure).has_value(),
        "cancelled scan cannot replace accepted evidence");
    check(!fs::exists(storage / "cancelled" / "cache-manifest.json"), "cancelled output is not sealed");
    select_cache(request, storage, "retry", accepted);
    const auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(page && page->records.size() == 2 && page->records[0].total_tokens + page->records[1].total_tokens == 35,
        "retry parses every source without publishing the cancelled partial scan");
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
    select_cache(request, storage, "baseline");
    const auto baseline = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(baseline && baseline->records.size() == 1 && baseline->records[0].total_tokens == 17,
        "establish accepted history before whole-scan rejection");
    const auto accepted = baseline->cache_candidate;
    write(source, metadata("retained") + start("turn") + tokens(310) + complete("turn"));
    write(duplicate, metadata("retained") + start("turn") + tokens(19) + complete("turn"));
    select_cache(request, storage, "rejected", accepted);
    check(!codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error) && error == "duplicate_usage_record",
        "both refreshed files are rejected together when snapshot identities collide");
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(*accepted, addon::path_to_utf8(storage),
        request.context.account_context, [] { return false; }, &failure).has_value(),
        "failed complete candidate leaves accepted authority intact");
    fs::remove(source);
    fs::remove(duplicate);
    select_cache(request, storage, "retained", accepted);
    const auto retained = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(retained && retained->records.size() == 1 && retained->records[0].total_tokens == 17,
        "source deletion after rejection retains only previously accepted evidence");
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

    const auto unindexed_root = storage / "unindexed";
    check(fs::create_directory(unindexed_root), "allocate unindexed compact baseline");
    for (const auto* name : {"snapshot", "cache-state", "records"}) {
        check(fs::copy_file(storage / "first" / name, unindexed_root / name), "copy compact accounting baseline");
    }
    {
        std::ifstream input(storage / "first" / "rollouts", std::ios::binary);
        std::ofstream output(unindexed_root / "rollouts", std::ios::binary);
        check(input && output, "open compact rollout metadata");
        std::string line;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "decode compact rollout metadata");
            row->erase("native_session");
            output << addon::serialize_json(*row) << '\n';
        }
        check(input.eof() && output, "write compact rollout metadata without a native-session index");
    }
    addon::Contract_failure failure;
    const auto unindexed = addon::seal_usage_cache_candidate(utf8(unindexed_root), utf8(storage),
        request.context.account_context, first.snapshot_id, "codex-rollout-cache@3", cancelled, &failure);
    check(unindexed.has_value(), "seal unindexed compact baseline");
    for (const auto& baseline : {*first.cache_candidate, *unindexed}) {
        const auto candidate = storage / (baseline.root == first.cache_candidate->root
            ? "unchanged-indexed" : "unchanged-unindexed");
        check(fs::create_directory(candidate), "allocate unchanged compact candidate");
        request.cache_selection = addon::Usage_cache_selection{baseline, utf8(candidate)};
        const auto unchanged = codex::scan_usage_incremental(
            request, profile, storage, cancelled, quiet_progress, &error, limits);
        check(unchanged && !unchanged->changes && unchanged->baseline == baseline && fs::is_empty(candidate),
            "unchanged indexed and unindexed histories discard every temporary run and retain the accepted generation");
    }

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

void incremental_scan_preserves_the_accepted_generation(const fs::path& fixture)
{
    const auto profile = fixture / "incremental-profile";
    const auto storage = fixture / "incremental-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-incremental.jsonl";
    const auto original = metadata() + start("first") + tokens(19) + complete("first");
    write(source, original);
    addon::Usage_scan_request request;
    request.mode                    = "incremental";
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "incremental-account"};
    const auto allocate = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate incremental candidate");
        request.snapshot_id.reset();
        request.cursor.reset();
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(path)};
    };
    const auto cancelled = [] { return false; };
    std::string error;
    allocate("accepted", std::nullopt);
    const auto first = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(first && first->cache_candidate && first->records.size() == 1, "establish accepted accounting");
    const auto baseline = *first->cache_candidate;
    const auto records_path = storage / "accepted" / "records";
    const auto records_time = fs::last_write_time(records_path);

    std::vector<std::string> notes;
    codex::Usage_scan_progress progress([&](std::uint64_t, const std::string& note) {
        notes.push_back(note);
    }, std::chrono::minutes(15));
    allocate("unchanged", baseline);
    auto result = codex::scan_usage_incremental(request, profile, storage, cancelled, progress, &error);
    check(result && !result->changes && result->baseline.root == baseline.root &&
        result->baseline.snapshot_id == baseline.snapshot_id &&
        result->baseline.manifest_sha256_hex == baseline.manifest_sha256_hex,
        "unchanged accounting returns the exact accepted baseline");
    check(fs::is_empty(storage / "unchanged") && fs::last_write_time(records_path) == records_time,
        "unchanged scan leaves accepted records intact and publishes no replacement cache");
    check(stage_notes(notes) == std::vector<std::string>{"Preparing the usage cache", "Reading Codex sessions"},
        "unchanged scan stops before sorting, merging, and saving accounting history");

    write(source, original + "{\"type\":\"response_item\",\"payload\":{\"content\":[]}}\n");
    allocate("metadata-change", baseline);
    result = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(result.has_value(), error.empty() ? "metadata-only incremental scan failed" : error.c_str());
    check(result->changes && result->changes->cache_candidate,
        "source metadata changes publish a candidate even when the total is unchanged");
    check(result->changes->records.empty(), "metadata-only source changes retain identical accounting observations");
    const auto metadata_baseline = result->changes->cache_candidate;
    allocate("metadata-accepted", metadata_baseline);
    result = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(result && !result->changes, "accepted metadata prevents repeating the same parse");

    write(source, original + start("second") + tokens(26) + complete("second"));
    allocate("appended", metadata_baseline);
    result = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(result && result->changes && result->changes->records.size() == 1 &&
        result->changes->records.back().total_tokens == 7,
        "an appended turn preserves the retained turn and adds its exact delta");
    const auto appended_baseline = result->changes->cache_candidate;
    check(fs::remove(source), "remove source after accounting acceptance");
    allocate("deleted", appended_baseline);
    result = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(result && !result->changes, "source deletion retains accepted accounting without replaying it");

    write(source, metadata() + start("first") + tokens(13) + complete("first"));
    allocate("old-edit", appended_baseline);
    result = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(result && result->changes && result->changes->records.size() == 1 &&
        result->changes->records.front().total_tokens == 13,
        "an edited old rollout offers its correction while retaining omitted accepted turns");

    const auto corrected_baseline = result->changes->cache_candidate;
    allocate("complete-replay", corrected_baseline);
    const auto complete_replay = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(complete_replay && complete_replay->complete && complete_replay->records.size() == 2 &&
        complete_replay->records.front().total_tokens == 13 && complete_replay->records.back().total_tokens == 7,
        "usage-scan@3 flattens a piece cache into a complete snapshot including omitted accepted turns");

    allocate("cancelled", appended_baseline);
    check(!codex::scan_usage_incremental(request, profile, storage, [] { return true; }, quiet_progress, &error) &&
        error == "cancelled", "incremental cancellation cannot publish a result");
    write(storage / "accepted" / "rollouts", "damaged\n");
    allocate("invalid-metadata", baseline);
    check(!codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error),
        "incremental metadata must match the accepted manifest before it can skip accounting work");
}

void incremental_scan_reuses_unaffected_pieces(const fs::path& fixture)
{
    const auto profile = fixture / "piece-profile";
    const auto storage = fixture / "piece-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = [&](int index) {
        return profile / "sessions" / ("rollout-piece-" + std::to_string(index) + ".jsonl");
    };
    for (int index = 0; index < 12; ++index) {
        write(source(index), metadata("piece-" + std::to_string(index)) + start("first") +
            tokens(10) + complete("first"));
    }
    addon::Usage_scan_request request;
    request.mode                    = "incremental";
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "piece-account"};
    const auto allocate = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate piece candidate");
        request.snapshot_id.reset();
        request.cursor.reset();
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(path)};
    };
    const auto cancelled = [] { return false; };
    std::string error;
    allocate("saved", std::nullopt);
    const auto saved = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(saved && saved->cache_candidate && saved->records.size() == 12, "establish saved accounting");
    const auto original = metadata("piece-0") + start("first") + tokens(10) + complete("first");
    write(source(0), original + "{\"type\":\"response_item\",\"payload\":{\"content\":[]}}\n");
    allocate("adopted", saved->cache_candidate);
    const auto adopted = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(adopted && adopted->changes && adopted->changes->records.empty() && adopted->changes->cache_candidate,
        "saved accounting adopts pieces without replaying unchanged observations");
    check(!fs::exists(storage / "adopted" / "records"), "incremental candidate owns pieces without a full-history mirror");
    addon::Contract_failure failure;
    const auto manifest = addon::validate_usage_cache_candidate(*adopted->changes->cache_candidate,
        utf8(storage), request.context.account_context, cancelled, &failure);
    check(manifest.has_value(), "adopted piece generation independently verifies");

    const auto appended = original + start("second") + tokens(13) + complete("second");
    write(source(0), appended);
    allocate("appended", adopted->changes->cache_candidate);
    const auto updated = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(updated && updated->changes && updated->changes->records.size() == 1 &&
        updated->changes->records.front().record_id == "codex:piece-0:second" &&
        updated->changes->records.front().total_tokens == 3,
        "one appended turn exports only its changed absolute observation");
    const auto& dated = updated->changes->records.front();
    check(dated.temporal && dated.temporal->parent.snapshot_id == updated->changes->snapshot_id &&
        dated.temporal->portions.size() == 1 && dated.temporal->portions.front().total_tokens == 3,
        "changed temporal evidence is rebound to its actual delta page");
    std::size_t inherited = 0;
    for (const auto& file : manifest->files) {
        if (!file.path.starts_with("records-")) continue;
        if (fs::equivalent(storage / "adopted" / file.path, storage / "appended" / file.path)) ++inherited;
    }
    check(inherited > 0, "unaffected accounting pieces are reused as the same immutable files");

    const auto moved = profile / "sessions" / "rollout-moved.jsonl";
    check(fs::remove(source(0)), "remove original rollout after acceptance");
    write(moved, appended);
    allocate("moved", updated->changes->cache_candidate);
    const auto relocated = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(relocated && relocated->changes && relocated->changes->records.empty(),
        "a moved rollout retains its accepted identity and contributes no duplicate observation");
    check(fs::remove_all(storage / "adopted") > 0 && fs::remove_all(storage / "appended") > 0,
        "retire earlier fixture cache generations");

    auto conflicting = appended;
    const auto completion = conflicting.rfind("2026-01-02T12:00:00Z");
    check(completion != std::string::npos, "fixture has native completion evidence");
    conflicting.replace(completion, 20, "2026-01-03T12:00:00Z");
    const auto previous_modified = fs::last_write_time(moved);
    write(moved, conflicting);
    fs::last_write_time(moved, previous_modified + std::chrono::seconds(1));
    allocate("temporal-conflict", relocated->changes->cache_candidate);
    const auto conflict = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(conflict && conflict->changes && conflict->changes->records.size() == 1 &&
        conflict->changes->records.front().total_tokens == 3 && conflict->changes->records.front().temporal &&
        conflict->changes->records.front().temporal->portions.empty(),
        "changed temporal evidence is exported even when its accepted accounting total stays equal");
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

void selected_cache_keeps_accepted_turns_when_their_rollout_is_parsed(const fs::path& fixture)
{
    // Re-reading a confirmed turn preserves the accepted category detail,
    // provenance and attribution, even when the current parse lacks them.
    const auto profile  = fixture / "accepted-profile";
    const auto storage  = fixture / "accepted-storage";
    const auto checkout = fixture / "accepted-checkout";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    fs::create_directories(checkout / ".git");
    const auto source = profile / "sessions" / "rollout-accepted.jsonl";
    write(source, metadata_with_cwd("accepted", checkout) + start("turn") +
        tokens(19, ",\"input_tokens\":14,\"output_tokens\":5,\"cached_input_tokens\":0") + complete("turn"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "accepted-account"};
    const auto cancelled = [] { return false; };
    std::string error;
    const auto scan = [&](const char* name, const char* mode, const std::optional<addon::Usage_cache_reference>& accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate accepted candidate");
        request.mode = mode;
        request.cache_selection = addon::Usage_cache_selection{accepted, utf8(path)};
        auto page = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
        check(page && page->complete && page->cache_candidate, error.empty() ? "accepted scan succeeds" : error.c_str());
        return *page;
    };
    const auto encoded = [](addon::Usage_record record) {
        record.temporal.reset();
        record.capabilities.erase("usage-temporal");
        if (!record.cross_source_event) record.capabilities.erase("usage-cross-source-event");
        return addon::serialize_json(addon::encode_usage_record(record, 3));
    };

    const auto first = scan("first", "incremental", std::nullopt);
    check(first.records.size() == 1 && first.records.front().record_id == "codex:accepted:turn" &&
        first.records.front().counter_field == "total_token_usage.total_tokens" &&
        first.records.front().repository_usage && first.records.front().repository_usage->allocations.size() == 1,
        "the accepted turn is accepted with its native provenance and attribution");

    check(fs::remove(checkout / ".git"), "remove the fixture Git marker");
    // The rollout's leading sample is a baseline, so its parse proves the turn.
    write(source, metadata("accepted") + tokens(4) + start("turn") + tokens(23) + complete("turn") +
        start("next") + tokens(30) + complete("next"));
    auto accepted = first.cache_candidate;
    for (const auto* mode : {"incremental", "repair"}) {
        // Records are sorted by ID, so the new turn comes first.
        const auto page = scan(mode, mode, accepted);
        check(page.records.size() == 2, "the accepted rollout holds two turns");
        auto confirmed = page.records.back();
        check(confirmed.cross_source_event && confirmed.cross_source_event->event_id == "accepted:turn",
            "a confirmed accepted turn gains the proof its parse established");
        confirmed.cross_source_event.reset();
        check(encoded(confirmed) == encoded(first.records.front()),
            "parsing an accepted turn's rollout keeps its accepted record");
        check(page.records.front().record_id == "codex:accepted:next" && page.records.front().total_tokens == 7,
            "the rollout's new turn adds exactly its delta");
        accepted = page.cache_candidate;
    }
}

void progress_continues_at_a_paced_rate_while_working()
{
    using namespace std::chrono;
    const milliseconds silence = minutes(15);
    steady_clock::time_point now{};
    steady_clock::time_point last{};
    std::uint64_t frames = 0;
    std::uint64_t completed = 0;
    std::string note;
    codex::Usage_scan_progress progress(
        [&](std::uint64_t count, const std::string& stage) {
            ++frames;
            last = now;
            completed = count;
            note = stage;
        }, silence, [&now] { return now; });
    progress.begin("Reading Codex sessions");
    const auto end = now + hours(72);
    while (now < end) {
        const auto before = frames;
        now += milliseconds(100);
        progress.advance();
        progress.poll();
        check(now - last <= silence / 4, "a working scan keeps reporting within the tolerated silence");
        if (frames != before) {
            progress.poll();
            check(frames == before + 1, "repeated polls without elapsed time coalesce");
        }
    }
    const auto elapsed_seconds = static_cast<std::uint64_t>(duration_cast<seconds>(end.time_since_epoch()).count());
    check(frames == elapsed_seconds + 1,
        "each elapsed reporting interval delivers progress throughout a long scan");
    check(completed > 0, "working progress reports its latest completed count");
    const auto before_stage = frames;
    progress.begin("Saving the usage cache");
    check(frames == before_stage + 1 && completed == 0 && note == "Saving the usage cache",
        "a new stage still reports immediately after a long scan");
}

void scans_report_each_stage_while_they_work(const fs::path& fixture)
{
    // A scan reports each stage as it begins and, while the stage works, how
    // many items it has handled. This clock advances a second at every
    // reading, so each of the scan's polls is due to report. The second scan
    // validates an accepted cache, parses every rollout and
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
    select_cache(request, storage, "holder");
    auto waiting_request = request;
    select_cache(waiting_request, storage, "waiting");
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
    const auto waited = codex::scan_usage(waiting_request, profile, storage, [&] { return ++polls > 4; }, progress, &error);
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

void selected_cache_spools_a_rollout_while_it_is_read(const fs::path& fixture)
{
    // A rollout parsed again writes its records to a spool in the candidate
    // as they are read, so a rollout of any size holds none of them in memory,
    // and one that changes while it is read keeps its accepted records and
    // leaves no spool in the sealed candidate.
    const auto profile = fixture / "spool-profile";
    const auto storage = fixture / "spool-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-spool.jsonl";
    write(source, metadata("spool") + start("turn") + tokens(17) + complete("turn"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "spool-account"};
    std::string error;
    const auto scan = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted,
        const std::function<bool()>& cancelled)
    {
        check(fs::create_directory(storage / name), "allocate spool candidate");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(storage / name)};
        return codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    };
    const auto baseline = scan("baseline", std::nullopt, [] { return false; });
    check(baseline && baseline->records.size() == 1 && baseline->cache_candidate, "establish accepted history");

    constexpr unsigned k_refresh_records = 500;
    std::string bytes = metadata("spool");
    for (unsigned index = 0; index < k_refresh_records; ++index) {
        const auto turn = "turn-" + std::to_string(index);
        bytes += start(turn) + tokens(index + 1) + complete(turn);
    }
    check(bytes.size() > 65536, "fixture spans multiple rollout reader chunks");
    write(source, bytes);
    const auto original_modified = fs::last_write_time(source);
    const auto spool = storage / "changed" / "rollout-records";
    bool changed = false;
    std::size_t spooled_records = 0;
    const auto change_during_read = [&] {
        if (changed || !fs::exists(spool)) return false;
        std::ifstream pending(spool, std::ios::binary);
        check(static_cast<bool>(pending), "open the spool while the rollout is read");
        const auto records = std::count(std::istreambuf_iterator<char>(pending), std::istreambuf_iterator<char>{}, '\n');
        if (records == 0 || records >= k_refresh_records) return false;
        fs::last_write_time(source, original_modified + std::chrono::seconds(5));
        spooled_records = static_cast<std::size_t>(records);
        changed = true;
        return false;
    };
    const auto retained = scan("changed", baseline->cache_candidate, change_during_read);
    check(changed && spooled_records > 0 && spooled_records < k_refresh_records,
        "a rollout's records reach the spool while the rollout is read");
    check(retained && retained->complete && retained->incomplete_historical_coverage &&
        retained->records.size() == 1 && retained->records[0].total_tokens == 17,
        error.empty() ? "a rollout changed while it is read keeps its accepted records" : error.c_str());
    check(!fs::exists(spool), "the discarded spool is removed");
    addon::Contract_failure failure;
    const auto manifest = addon::validate_usage_cache_candidate(*retained->cache_candidate, utf8(storage),
        request.context.account_context, [] { return false; }, &failure);
    check(manifest.has_value(), "the candidate seals without the spool");
    for (const auto& file : manifest->files) {
        check(file.path != "rollout-records", "the sealed candidate holds no spool");
    }
}

void selected_cache_requires_the_rollout_of_each_accepted_record(const fs::path& fixture)
{
    // Every accepted record must belong to a rollout the new candidate lists:
    // one the accepted cache lists, or a live rollout that lists it again.
    const auto profile = fixture / "listed-profile";
    const auto storage = fixture / "listed-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto unlisted = profile / "sessions" / "rollout-unlisted.jsonl";
    write(profile / "sessions" / "rollout-kept.jsonl", metadata("kept") + start("turn") + tokens(11) + complete("turn"));
    write(unlisted, metadata("unlisted") + start("turn") + tokens(13) + complete("turn"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "listed-account"};
    std::string error;
    const auto scan = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        check(fs::create_directory(storage / name), "allocate listed candidate");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(storage / name)};
        return codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    };
    const auto first = scan("first", std::nullopt);
    check(first && first->records.size() == 2 && first->cache_candidate, "establish two accepted rollouts");

    // The same candidate without the second rollout's metadata line.
    const auto tampered_root = storage / "tampered";
    check(fs::create_directory(tampered_root), "allocate tampered candidate");
    for (const auto* name : {"records", "snapshot", "cache-state"}) {
        check(fs::copy_file(storage / "first" / name, tampered_root / name), "copy an accepted candidate file");
    }
    std::ifstream rollouts(storage / "first" / "rollouts", std::ios::binary);
    std::string listed;
    std::string line;
    while (std::getline(rollouts, line)) {
        if (line.find("rollout-unlisted") == std::string::npos) listed += line + '\n';
    }
    write(tampered_root / "rollouts", listed);
    addon::Contract_failure failure;
    const auto tampered = addon::seal_usage_cache_candidate(utf8(tampered_root), utf8(storage),
        request.context.account_context, first->snapshot_id, "codex-rollout-cache@3", [] { return false; }, &failure);
    check(tampered.has_value(), "seal the tampered candidate");

    const auto relisted = scan("relisted", tampered);
    check(relisted && relisted->records.size() == 2,
        error.empty() ? "a live rollout lists an accepted record's rollout again" : error.c_str());
    check(fs::remove(unlisted), "remove the unlisted rollout");
    check(!scan("refused", tampered) && error == "usage_cache_identity_mismatch",
        "an accepted record whose rollout nothing lists fails the scan");
}

void incremental_adoption_keeps_unexamined_temporal_evidence(const fs::path& fixture)
{
    const auto profile = fixture / "adoption-profile";
    const auto storage = fixture / "adoption-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-saved.jsonl";
    write(source, metadata("saved") + start("first") + tokens(10) + complete("first") +
        start("second") + tokens(25) + complete("second"));
    addon::Usage_scan_request request;
    request.mode                    = "incremental";
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "adoption-account"};
    const auto allocate = [&](const std::string& name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto path = storage / name;
        check(fs::create_directory(path), "allocate adoption candidate");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(path)};
    };
    const auto cancelled = [] { return false; };
    std::string error;
    allocate("saved", std::nullopt);
    const auto first = codex::scan_usage(request, profile, storage, cancelled, quiet_progress, &error);
    check(first && first->cache_candidate, "establish saved adoption accounting");
    for (const auto* name : {"rollouts", "records"}) {
        const auto path = storage / "saved" / name;
        std::ifstream input(path, std::ios::binary);
        std::string line;
        std::string replacement;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "read saved adoption evidence");
            row->erase(std::string_view(name) == "rollouts" ? "temporal_parser" : "temporal_observation");
            if (std::string_view(name) == "rollouts") {
                row->erase("native_session");
            }
            replacement += addon::serialize_json(*row) + '\n';
        }
        input.close();
        write(path, replacement);
    }
    check(fs::remove(storage / "saved" / "cache-manifest.json"), "replace fixture temporal inventory");
    addon::Contract_failure failure;
    const auto saved = addon::seal_usage_cache_candidate(utf8(storage / "saved"), utf8(storage),
        request.context.account_context, first->snapshot_id, "codex-rollout-cache@3", cancelled, &failure);
    check(saved.has_value(), "seal saved accounting without temporal backfill");
    const auto unchanged_size = fs::file_size(source);
    const auto unchanged_time = fs::last_write_time(source);
    write(source, std::string(unchanged_size - 1, 'x') + '\n');
    fs::last_write_time(source, unchanged_time);
    write(profile / "sessions" / "rollout-fresh.jsonl",
        metadata("fresh") + start("first") + tokens(7) + complete("first"));
    allocate("adopted", saved);
    const auto adopted = codex::scan_usage_incremental(request, profile, storage, cancelled, quiet_progress, &error);
    check(adopted && adopted->changes && !adopted->changes->incomplete_historical_coverage &&
        adopted->changes->records.size() == 1 && adopted->changes->records.front().record_id == "codex:fresh:first",
        "adoption processes the new source without rereading unchanged raw history for optional evidence");
    const auto candidate = addon::path_from_utf8(adopted->changes->cache_candidate->root);
    std::ifstream rollouts(candidate / "rollouts", std::ios::binary);
    std::string line;
    bool retained_absence = false;
    while (std::getline(rollouts, line)) {
        const auto row = addon::parse_json(line, {}, nullptr);
        check(row.has_value(), "read adopted rollout metadata");
        if (row->find("source")->as_string() == utf8(source)) {
            retained_absence = !row->find("temporal_parser");
        }
    }
    check(retained_absence, "adoption does not claim that missing temporal evidence was examined");
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

    const auto without_predecessor = metadata("predecessor") + start("b") + tokens(25) + complete("b") +
        start("c") + tokens(30) + complete("c");
    write(source, without_predecessor);
    auto accepted = first.cache_candidate;
    for (const auto* mode : {"incremental", "repair"}) {
        const auto page = scan(mode, accepted);
        check(page.records.size() == 3 && find(page, "codex:predecessor:a").total_tokens == 10 && proved_b(page),
            "a turn whose predecessor was lost keeps its accepted total and proof");
        check(find(page, "codex:predecessor:c").total_tokens == 5, "the next turn adds exactly its delta");
        accepted = page.cache_candidate;
    }
    const auto archive = profile / "archived_sessions";
    check(fs::create_directory(archive), "create the truncated-copy archive");
    write(archive / source.filename(), without_predecessor);
    const auto copied = scan("incremental", accepted);
    check(copied.records.size() == 3 && proved_b(copied) &&
        find(copied, "codex:predecessor:a").total_tokens == 10 &&
        find(copied, "codex:predecessor:c").total_tokens == 5,
        "current copies compare with each other while accepted baseline evidence survives");
}

void selected_cache_preserves_peer_validation(const fs::path& fixture)
{
    const auto root    = fixture / "peer-coverage";
    const auto profile = root / "profile";
    const auto storage = root / "storage";
    check(fs::create_directories(profile / "sessions"), "create active peer profile");
    check(fs::create_directory(profile / "archived_sessions"), "create archived peer profile");
    check(fs::create_directory(storage), "create peer-validation storage");
    const auto active = profile / "sessions" / "rollout-peer.jsonl";
    const auto archive = profile / "archived_sessions" / active.filename();
    const auto original = metadata("peer") + start("turn") +
        tokens(600, ",\"input_tokens\":595,\"output_tokens\":5,\"cached_input_tokens\":0") + complete("turn");
    write(active, original);
    addon::Usage_scan_request request;
    request.mode = "incremental";
    request.context.account_context = addon::Account_context{1, "account"};
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    unsigned generation = 0;
    const auto allocate = [&](std::optional<addon::Usage_cache_reference> accepted) {
        const auto candidate = storage / ("candidate-" + std::to_string(generation++));
        check(fs::create_directory(candidate), "allocate peer-validation candidate");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), utf8(candidate)};
    };
    std::string error;
    allocate(std::nullopt);
    const auto first = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(first && first->cache_candidate && first->records.size() == 1, "accept the active observation first");
    write(archive, original);
    allocate(first->cache_candidate);
    const auto overlap = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(overlap && overlap->cache_candidate && overlap->records.size() == 1 &&
        overlap->records.front().source_path == utf8(active), "accept the equal archive copy under the original identity");

    const auto saved = storage / "unindexed";
    check(fs::create_directory(saved), "allocate unbound-peer cache fixture");
    const auto accepted = addon::path_from_utf8(overlap->cache_candidate->root);
    for (const auto* name : {"snapshot", "cache-state", "records"}) {
        check(fs::copy_file(accepted / name, saved / name), "copy accepted peer accounting");
    }
    {
        std::ifstream input(accepted / "rollouts", std::ios::binary);
        std::ofstream output(saved / "rollouts", std::ios::binary);
        check(input && output, "open unbound-peer metadata fixture");
        std::string line;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "decode peer rollout metadata");
            if (row->find("source")->as_string() == utf8(archive)) {
                row->erase("native_session");
            }
            output << addon::serialize_json(*row) << '\n';
        }
        check(input.eof() && output, "write unbound-peer metadata fixture");
    }
    addon::Contract_failure failure;
    const auto unindexed = addon::seal_usage_cache_candidate(utf8(saved), utf8(storage), request.context.account_context,
        overlap->snapshot_id, "codex-rollout-cache@3", [] { return false; }, &failure);
    check(unindexed.has_value(), "seal unbound-peer accounting fixture");
    const auto previous_modified = fs::last_write_time(archive);
    write(archive, std::string(original.size() - 1, 'x') + '\n');
    fs::last_write_time(archive, previous_modified);
    write(profile / "sessions" / "rollout-new.jsonl", metadata("new") + start("turn") + tokens(9) + complete("turn"));
    allocate(unindexed);
    const auto incomplete = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(incomplete && incomplete->changes && incomplete->changes->cache_candidate &&
        incomplete->changes->incomplete_historical_coverage, "unreadable unbound peer retains incomplete coverage");
    allocate(incomplete->changes->cache_candidate);
    const auto unchanged = codex::scan_usage_incremental(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(unchanged.has_value(), "unchanged incomplete generation remains usable");
    check(!unchanged->changes || unchanged->changes->incomplete_historical_coverage,
        "unchanged reuse promoted unresolved peer coverage to complete");
    check(!unchanged->changes, "unchanged incomplete coverage retains the exact accepted generation");
    allocate(incomplete->changes->cache_candidate);
    const auto replay = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(replay && replay->incomplete_historical_coverage && replay->records.size() == 2,
        "an incremental complete snapshot also retains unresolved coverage and accepted accounting");
    write(archive, original);
    request.mode = "repair";
    allocate(replay->cache_candidate);
    const auto repaired = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(repaired && !repaired->incomplete_historical_coverage && repaired->records.size() == 2,
        "full verification of the restored sources resolves the peer coverage gap");
}

void selected_cache_pages_while_another_account_scans(const fs::path& fixture)
{
    const auto profile = fixture / "paging-profile";
    const auto storage = fixture / "paging-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    std::string bytes = metadata("paging");
    for (unsigned index = 0; index < 270; ++index) {
        const auto turn = "turn-" + std::to_string(index);
        bytes += start(turn) + tokens(index + 1) + complete(turn);
    }
    write(profile / "sessions" / "rollout-paging.jsonl", bytes);
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "paging-account"};
    const auto candidate = storage / "candidate";
    check(fs::create_directory(candidate), "allocate paging candidate");
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, utf8(candidate)};
    std::string error;
    const auto first = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(first && !first->complete && first->next_cursor && first->cache_candidate,
        "seal the first account before a second account starts scanning");
    request.snapshot_id = first->snapshot_id;
    request.cursor      = first->next_cursor;

    const auto other_profile = fixture / "paging-other-profile";
    const auto other_storage = fixture / "paging-other-storage";
    fs::create_directory(other_profile);
    fs::create_directory(other_storage);
    const auto other_candidate = other_storage / "candidate";
    fs::create_directory(other_candidate);
    addon::Usage_scan_request other;
    other.context.profile_storage = addon::Profile_storage_context{1, utf8(other_storage)};
    other.context.account_context = addon::Account_context{1, "paging-other-account"};
    other.cache_selection = addon::Usage_cache_selection{std::nullopt, utf8(other_candidate)};

    std::mutex mutex;
    std::condition_variable changed;
    bool scanning = false;
    bool release  = false;
    bool paged    = false;
    codex::Usage_scan_progress paused_progress([&](std::uint64_t, const std::string&) {
        std::unique_lock lock(mutex);
        scanning = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    }, std::chrono::minutes(15));
    std::string other_error;
    std::optional<addon::Usage_scan_page> other_page;
    std::jthread scanner([&] {
        other_page = codex::scan_usage(other, other_profile, other_storage,
            [] { return false; }, paused_progress, &other_error);
    });
    bool started = false;
    {
        std::unique_lock lock(mutex);
        started = changed.wait_for(lock, std::chrono::seconds(5), [&] { return scanning; });
    }
    std::optional<addon::Usage_scan_page> last;
    std::jthread reader([&] {
        last = codex::scan_usage(request, profile, storage,
            [] { return false; }, quiet_progress, &error);
        std::lock_guard lock(mutex);
        paged = true;
        changed.notify_all();
    });
    bool independent = false;
    {
        std::unique_lock lock(mutex);
        independent = changed.wait_for(lock, std::chrono::seconds(2), [&] { return paged; });
        release = true;
    }
    changed.notify_all();
    scanner.join();
    reader.join();
    check(started && other_page.has_value(), "second account finishes its bounded scan");
    check(independent, "sealed selected-cache pages must not wait for another account's active scan");
    check(last && last->complete && last->records.size() == 14 &&
        last->cache_candidate == first->cache_candidate,
        "concurrent paging retains the sealed snapshot, cursor and candidate identity");
}

void selected_cache_refreshes_missing_temporal_history(const fs::path& fixture)
{
    const auto profile = fixture / "temporal-refresh-profile";
    const auto storage = fixture / "temporal-refresh-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-refresh.jsonl";
    const auto bytes = metadata("refresh") + start("first") + tokens(10) + complete("first") +
        start("dated") + tokens(25) + complete("dated");
    write(source, bytes);
    const auto original_modified = fs::last_write_time(source);
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "temporal-refresh-account"};
    unsigned allocations = 0;
    const auto scan = [&](const std::optional<addon::Usage_cache_reference>& accepted) {
        const auto candidate = storage / ("candidate-" + std::to_string(allocations++));
        check(fs::create_directory(candidate), "allocate temporal refresh candidate");
        request.cache_selection = addon::Usage_cache_selection{accepted, utf8(candidate)};
        std::string error;
        const auto page = codex::scan_usage(request, profile, storage,
            [] { return false; }, quiet_progress, &error);
        check(page && page->complete && page->cache_candidate,
            error.empty() ? "temporal refresh scan succeeds" : error.c_str());
        check(page->records.size() == 2 && page->records[0].record_id == "codex:refresh:dated" &&
            page->records[0].total_tokens == 15 && page->records[1].total_tokens == 10,
            "temporal refresh preserves both accepted accounting turns and their exact deltas");
        return *page;
    };
    const auto first = scan(std::nullopt);
    const auto historical = [&](std::string_view mode) {
        const auto root = storage / ("historical-" + std::string(mode));
        check(fs::create_directory(root), "allocate accepted historical fixture");
        const auto current = addon::path_from_utf8(first.cache_candidate->root);
        for (const auto* name : {"snapshot", "cache-state"}) {
            check(fs::copy_file(current / name, root / name), "copy unchanged accounting custody");
        }
        for (const auto* name : {"rollouts", "records"}) {
            std::ifstream input(current / name, std::ios::binary);
            std::string line;
            std::string rows;
            while (std::getline(input, line)) {
                auto row = addon::parse_json(line, {}, nullptr);
                check(row.has_value(), "parse historical cache fixture");
                if (std::string_view(name) == "rollouts") {
                    row->erase("temporal_parser");
                }
                else
                if (mode == "absent") {
                    row->erase("temporal_observation");
                    if (auto* capabilities = row->find("usage")->find("capabilities")) {
                        capabilities->erase("usage-cross-source-event");
                    }
                }
                else
                if (auto* carrier = row->find("temporal_observation")) {
                    if (mode == "unknown") {
                        carrier->set("version", addon::Json_value(2));
                    }
                    else {
                        carrier->set("instant", addon::Json_value{});
                        carrier->set("reason", addon::Json_value("unsupported"));
                    }
                }
                rows += addon::serialize_json(*row) + '\n';
            }
            check(input.eof(), "read complete historical fixture");
            write(root / name, rows);
        }
        addon::Contract_failure failure;
        const auto reference = addon::seal_usage_cache_candidate(utf8(root), utf8(storage),
            request.context.account_context, first.snapshot_id, "codex-rollout-cache@3",
            [] { return false; }, &failure);
        check(reference.has_value(), failure.detail.c_str());
        return reference;
    };
    const auto absent = historical("absent");
    const auto refreshed = scan(absent);
    const auto& dated = refreshed.records.front();
    check(dated.cross_source_event && dated.temporal && dated.temporal->portions.size() == 1 &&
        dated.temporal->portions.front().instant == "2026-01-02T12:00:00Z" &&
        dated.temporal->portions.front().total_tokens == 15,
        "unchanged rollout refreshes absent accepted temporal evidence from its native completion");
    check(refreshed.records.back().temporal && refreshed.records.back().temporal->portions.empty() &&
        refreshed.records.back().temporal->unavailable_tokens == 10,
        "refresh never dates an unproved cumulative first turn");
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(*absent, utf8(storage), request.context.account_context,
        [] { return false; }, &failure).has_value(), "refresh leaves the selected historical cache immutable");

    for (const auto* mode : {"unknown", "unsupported"}) {
        const auto replay = scan(historical(mode));
        check(replay.records.front().temporal && replay.records.front().temporal->portions.empty() &&
            replay.records.front().temporal->unavailable_tokens == 15 &&
            replay.records.front().temporal->coverage_reason == "unsupported",
            "refresh distinguishes absent temporal evidence from unknown or unsupported evidence");
        if (std::string_view(mode) == "unknown") {
            std::ifstream input(addon::path_from_utf8(replay.cache_candidate->root) / "records");
            std::string line;
            check(static_cast<bool>(std::getline(input, line)), "read retained unknown observation");
            const auto row = addon::parse_json(line, {}, nullptr);
            const auto* carrier = row ? row->find("temporal_observation") : nullptr;
            check(carrier && carrier->find("version") && carrier->find("version")->is_integer() &&
                carrier->find("version")->as_integer() == 2,
                "refresh preserves the unknown optional observation in its candidate");
        }
    }

    // An unchanged fingerprint with unreadable content reveals whether the
    // second incremental scan actually reuses the completed temporal parse.
    write(source, std::string(bytes.size() - 1, 'x') + '\n');
    fs::last_write_time(source, original_modified);
    const auto cached = scan(refreshed.cache_candidate);
    check(!cached.incomplete_historical_coverage && cached.records.front().temporal &&
        cached.records.front().temporal->portions.size() == 1,
        "a completed temporal refresh keeps the unchanged-rollout fast path");
    const auto malformed = scan(absent);
    check(malformed.incomplete_historical_coverage && malformed.records.front().temporal &&
        malformed.records.front().temporal->portions.empty(),
        "failed optional refresh preserves accepted accounting with unavailable temporal evidence");
    const auto repair_candidate = storage / "repair-candidate";
    check(fs::create_directory(repair_candidate), "allocate strict repair candidate");
    request.mode = "repair";
    request.cache_selection = addon::Usage_cache_selection{absent, utf8(repair_candidate)};
    std::string error;
    const auto repaired = codex::scan_usage(request, profile, storage,
        [] { return false; }, quiet_progress, &error);
    check(!repaired && error == "rollout_record_malformed",
        "forced repair still rejects malformed unchanged source when only temporal evidence is absent");
    request.mode = "incremental";
    check(fs::remove(source), "remove historical original");
    const auto missing = scan(absent);
    check(missing.records.front().temporal && missing.records.front().temporal->portions.empty(),
        "missing original preserves accepted accounting without inventing an instant");
}

void selected_cache_dates_only_certified_turns(const fs::path& fixture)
{
    const auto profile = fixture / "temporal-profile";
    const auto storage = fixture / "temporal-storage";
    fs::create_directories(profile / "sessions");
    fs::create_directory(storage);
    const auto source = profile / "sessions" / "rollout-temporal.jsonl";
    const auto ended = [](std::string_view turn, std::string_view instant, std::string_view extra = {}) {
        return "{\"type\":\"event_msg\",\"timestamp\":\"" + std::string(instant) +
            "\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"" + std::string(turn) +
            "\"" + std::string(extra) + "}}\n";
    };
    const std::string raw = "2026-01-02T23:59:59.123456789+02:00";
    write(source, metadata("temporal") + start("first") + tokens(10) + complete("first") +
        start("dated") + tokens(25) + ended("dated", raw) +
        start("epoch") + tokens(29) + ended("epoch", "2026-03-01T00:00:00Z", ",\"completed_at\":1767398400") +
        start("local") + tokens(32) + ended("local", "2026-01-03T12:00:00") +
        start("zero") + tokens(32) + complete("zero") +
        start("reset") + tokens(40) + tokens(2) + complete("reset") +
        start("quiet") + complete("quiet"));
    addon::Usage_scan_request request;
    request.context.profile_storage = addon::Profile_storage_context{1, utf8(storage)};
    request.context.account_context = addon::Account_context{1, "temporal-account"};
    unsigned allocations = 0;
    std::string error;
    const auto scan = [&](const std::optional<addon::Usage_cache_reference>& accepted) {
        request.snapshot_id.reset();
        request.cursor.reset();
        const auto candidate = storage / ("candidate-" + std::to_string(allocations++));
        check(fs::create_directory(candidate), "allocate temporal candidate");
        request.cache_selection = addon::Usage_cache_selection{accepted, utf8(candidate)};
        const auto page = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
        check(page && page->complete && page->cache_candidate, error.empty() ? "temporal scan succeeds" : error.c_str());
        addon::Contract_failure failure;
        check(addon::decode_usage_scan_result(addon::encode_usage_scan_result(*page, 3), &failure, 3).has_value(),
            failure.detail.c_str());
        return *page;
    };
    const auto find = [](const addon::Usage_scan_page& page, std::string_view turn) -> const addon::Usage_record& {
        const auto found = std::find_if(page.records.begin(), page.records.end(), [&](const auto& record) {
            return record.record_id == "codex:temporal:" + std::string(turn);
        });
        check(found != page.records.end(), "every admitted temporal turn remains present");
        return *found;
    };
    const auto first = scan(std::nullopt);
    check(first.records.size() == 7, "temporal extraction preserves every accounting turn");
    const auto& dated = find(first, "dated");
    check(dated.total_tokens == 15 && dated.temporal && dated.temporal->portions.size() == 1 &&
        dated.temporal->portions.front().instant == raw && dated.temporal->portions.front().total_tokens == 15,
        "certified turn keeps the raw native nanosecond offset instant and exact delta");
    const auto& epoch = find(first, "epoch");
    check(epoch.temporal && epoch.temporal->portions.front().instant == "2026-01-03T00:00:00Z",
        "native integer completion seconds take their original priority over the row timestamp");
    for (const auto* turn : {"first", "reset", "quiet", "local"}) {
        const auto& record = find(first, turn);
        check(record.temporal && record.temporal->portions.empty() &&
            record.temporal->unavailable_tokens == record.total_tokens,
            "first/reset/sample-free/nonstrict turns remain accounting with unavailable temporal tokens");
    }
    check(find(first, "zero").temporal && find(first, "zero").temporal->portions.empty() &&
        find(first, "zero").temporal->unavailable_tokens == 0,
        "certified zero never emits a positive portion");
    for (const auto& record : first.records) {
        check(record.temporal && record.temporal->parent.record_id == record.record_id &&
            record.temporal->parent.total_tokens == record.total_tokens &&
            record.temporal->parent.snapshot_id == first.snapshot_id &&
            record.temporal->parent.source_account_key == first.scope.source_account_key,
            "every optional envelope binds the actual accounting parent and page custody");
    }
    const auto copy = profile / "sessions" / "rollout-temporal-copy.jsonl";
    write(copy, metadata("temporal") + start("first") + tokens(10) + complete("first") +
        start("dated") + tokens(25) + ended("dated", "2026-01-02T23:59:59.123456790+02:00"));
    const auto conflict = scan(std::nullopt);
    check(find(conflict, "dated").total_tokens == 15 &&
        find(conflict, "dated").temporal->portions.empty() &&
        find(conflict, "dated").temporal->unavailable_tokens == 15,
        "equal admitted accounting with conflicting native instants withdraws only temporal proof");
    check(fs::remove(copy), "remove conflicting copy");
    check(fs::remove(source), "delete original temporal source");
    const auto replay = scan(first.cache_candidate);
    check(find(replay, "dated").temporal->portions.front().instant == raw &&
        find(replay, "dated").temporal->parent.snapshot_id == replay.snapshot_id &&
        replay.snapshot_id != first.snapshot_id,
        "deleted-original replay retains exact source proof and rebinds only current snapshot custody");

    const auto changed_cache = [&](std::string_view mode) {
        const auto root = storage / ("changed-" + std::string(mode));
        check(fs::create_directory(root), "allocate changed optional cache");
        for (const auto* name : {"rollouts", "snapshot", "cache-state"}) {
            check(fs::copy_file(addon::path_from_utf8(first.cache_candidate->root) / name, root / name),
                "copy unchanged accounting custody");
        }
        std::ifstream input(addon::path_from_utf8(first.cache_candidate->root) / "records", std::ios::binary);
        std::string line;
        std::string rows;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "parse accepted temporal fixture");
            const auto* usage = row->find("usage");
            if (usage->find("record_id")->as_string() == "codex:temporal:dated") {
                auto* carrier = row->find("temporal_observation");
                if (mode == "absent") row->erase("temporal_observation");
                else if (mode == "unknown") carrier->set("version", addon::Json_value(2));
                else if (mode == "digest") carrier->set("accounting_sha256_hex", addon::Json_value(std::string(64, '0')));
                else if (mode == "oversize") carrier->set("instant", addon::Json_value(std::string(768 * 1024 + 1, 'x')));
                else if (mode == "unproved") {
                    row->find("usage")->find("capabilities")->erase("usage-cross-source-event");
                    row->find("usage")->erase("capabilities");
                    auto accounting = *row;
                    accounting.erase("temporal_observation");
                    carrier->set("accounting_sha256_hex", addon::Json_value(addon::to_hex(addon::Sha256::digest(
                        addon::serialize_json_canonical(accounting)))));
                }
                else row->set("temporal_observation", addon::Json_value("damaged"));
            }
            rows += addon::serialize_json(*row) + '\n';
        }
        write(root / "records", rows);
        addon::Contract_failure failure;
        const auto reference = addon::seal_usage_cache_candidate(utf8(root), utf8(storage),
            request.context.account_context, first.snapshot_id, "codex-rollout-cache@3", [] { return false; }, &failure);
        check(reference.has_value(), failure.detail.c_str());
        return reference;
    };
    for (const auto* mode : {"absent", "unknown", "digest", "oversize", "malformed", "unproved"}) {
        const auto page = scan(changed_cache(mode));
        const auto& record = find(page, "dated");
        check(record.total_tokens == 15 && record.temporal && record.temporal->portions.empty() &&
            record.temporal->unavailable_tokens == 15 && page.records.size() == first.records.size(),
            "optional absence/future/digest/oversize/damage keeps all original accounting usable");
    }

    // Fill an ordinary, valid accounting capability to the unchanged SDK
    // exchange boundary. Optional evidence must not make that row unusable.
    const auto capacity_root = storage / "capacity-accepted";
    const auto capacity_candidate = storage / "capacity-candidate";
    check(fs::create_directory(capacity_root) && fs::create_directory(capacity_candidate),
        "allocate optional capacity fixture");
    for (const auto* name : {"rollouts", "snapshot", "cache-state"}) {
        check(fs::copy_file(addon::path_from_utf8(first.cache_candidate->root) / name, capacity_root / name),
            "copy unchanged capacity custody");
    }
    auto large = dated;
    large.temporal.reset();
    large.capabilities.erase("usage-temporal");
    addon::Json_value padding = addon::Json_value::array();
    for (unsigned index = 0; index < 4; ++index) padding.push_back(addon::Json_value(""));
    large.capabilities.set("ordinary-retained", padding);
    auto measured = first;
    measured.snapshot_id = std::string(64, '0');
    measured.records = {large};
    measured.complete = true;
    measured.next_cursor.reset();
    measured.cache_candidate = addon::Usage_cache_reference{
        utf8(capacity_candidate), measured.snapshot_id, std::string(64, '0')};
    const auto empty_bytes = addon::serialize_json(addon::encode_usage_scan_result(measured, 3)).size();
    auto remaining = addon::limits::k_max_exchange_payload_bytes - empty_bytes - 8;
    for (unsigned index = 0; index < 4; ++index) {
        const auto bytes = std::min<std::uint64_t>(remaining, addon::limits::k_max_json_string_bytes);
        padding.as_array()[index] = addon::Json_value(std::string(static_cast<std::size_t>(bytes), 'x'));
        remaining -= bytes;
    }
    check(remaining == 0, "capacity fixture remains within each original JSON string bound");
    large.capabilities.set("ordinary-retained", std::move(padding));
    auto accounting = addon::Json_value::object();
    accounting.set("usage", addon::encode_usage_record(large, 3));
    accounting.set("recorded_cwd", addon::Json_value(""));
    auto carrier = addon::Json_value::object();
    carrier.set("version", addon::Json_value(1));
    carrier.set("accounting_sha256_hex", addon::Json_value(addon::to_hex(addon::Sha256::digest(
        addon::serialize_json_canonical(accounting)))));
    carrier.set("instant", addon::Json_value(raw));
    carrier.set("reason", addon::Json_value("complete"));
    accounting.set("temporal_observation", std::move(carrier));
    write(capacity_root / "records", addon::serialize_json(accounting) + '\n');
    addon::Contract_failure capacity_failure;
    const auto capacity_reference = addon::seal_usage_cache_candidate(utf8(capacity_root), utf8(storage),
        request.context.account_context, first.snapshot_id, "codex-rollout-cache@3", [] { return false; }, &capacity_failure);
    check(capacity_reference.has_value(), capacity_failure.detail.c_str());
    request.cache_selection = addon::Usage_cache_selection{capacity_reference, utf8(capacity_candidate)};
    const auto capacity = codex::scan_usage(request, profile, storage, [] { return false; }, quiet_progress, &error);
    check(capacity && capacity->complete && capacity->records.size() == 1 &&
        capacity->records.front().total_tokens == 15 && !capacity->records.front().temporal &&
        capacity->records.front().capabilities.find("ordinary-retained"),
        "optional singleton overflow preserves the original valid accounting row and its unrelated capability");
    check(addon::decode_usage_scan_result(addon::encode_usage_scan_result(*capacity, 3), &capacity_failure, 3).has_value(),
        "capacity fallback remains a valid unchanged-budget exchange");
}

void adapter_optional_isolation(const fs::path& package, const fs::path& fixture, bool member_boundary)
{
    const auto profile = fixture / "profile";
    const auto source = profile / "sessions" / "rollout-isolation.jsonl";
    fs::create_directories(source.parent_path());
    write(source, metadata(member_boundary ? "members" : "s\\tt") +
        start("turn") + tokens(10) + complete("turn"));
    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "cache";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail.c_str());
    const auto native_profile = utf8(profile);
    const auto account = codex::profile_account_key(native_profile);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.codex", &error, account);
    check(allocated.has_value(), error.c_str());
    addon::Usage_scan_request request;
    request.context.launcher = {"codex", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"codex"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledgment = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledgment, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "isolation adapter acknowledges its actual allocation");
    const auto scan = [&] {
        const auto id = host.invoke({"usage-scan", 3}, "scan",
            addon::encode_usage_scan_request(request, 3), 10000, invocation);
        const auto outcome = host.wait_outcome(id, 11000);
        if (!outcome || outcome->status != addon::Host_status::SUCCESS) {
            std::cerr << "isolation usage: " <<
                (outcome ? outcome->reason_code : "timeout") << '\n';
        }
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            member_boundary ? "1024 ordinary capability members remain usable through the actual adapter" :
            "accounting control-character identity remains usable through the actual adapter");
        const auto page = addon::decode_usage_scan_result(outcome->payload, &failure, 3);
        check(page && page->complete && page->records.size() == 1 && page->records[0].total_tokens == 10,
            "isolation retains the exact original accounting parent and H");
        return *page;
    };
    const std::string identity = member_boundary ? "codex:members:turn" : "codex:s\tt:turn";
    const auto candidate = addon::path_from_utf8(*allocated) / "first";
    check(fs::create_directory(candidate), "allocate isolation selected candidate");
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, utf8(candidate)};
    const auto first = scan();
    if (!member_boundary) {
        check(first.records[0].record_id == identity && !first.records[0].temporal,
            "invalid optional parent withdraws proof without changing the accounting identity");
    }
    else {
        check(fs::remove(source), "remove source for retained structural boundary");
        const auto accepted = addon::path_from_utf8(*allocated) / "accepted-members";
        check(fs::create_directory(accepted), "allocate retained structural fixture");
        for (const auto* name : {"rollouts", "snapshot", "cache-state"}) {
            check(fs::copy_file(candidate / name, accepted / name), "copy structural accounting custody");
        }
        auto record = first.records[0];
        record.temporal.reset();
        record.cross_source_event.reset();
        record.capabilities = addon::Json_value::object();
        for (std::size_t index = 0; index < addon::limits::k_max_json_members; ++index) {
            record.capabilities.set("ordinary-" + std::to_string(index), addon::Json_value(static_cast<int>(index)));
        }
        auto accounting = addon::Json_value::object();
        accounting.set("usage", addon::encode_usage_record(record, 3));
        accounting.set("recorded_cwd", addon::Json_value(""));
        const auto original = addon::serialize_json(accounting);
        check(addon::parse_json(original, {}, nullptr).has_value() &&
            addon::decode_usage_record(addon::encode_usage_record(record, 3), &failure, 3).has_value(),
            "the unchanged original accounting is within its JSON member limit");
        write(accepted / "records", original + '\n');
        const auto reference = addon::seal_usage_cache_candidate(utf8(accepted), *allocated,
            request.context.account_context, first.snapshot_id, "codex-rollout-cache@3", [] { return false; }, &failure);
        check(reference.has_value(), failure.detail.c_str());
        const auto next = addon::path_from_utf8(*allocated) / "next";
        check(fs::create_directory(next), "allocate structural successor");
        request.cache_selection = addon::Usage_cache_selection{reference, utf8(next)};
        const auto retained = scan();
        check(retained.records[0].record_id == identity && !retained.records[0].temporal &&
            retained.records[0].capabilities.as_object().size() == addon::limits::k_max_json_members,
            "optional structural overflow preserves every original opaque capability");
    }
    check(host.shutdown(1000, nullptr), "isolation adapter exits without any model request");
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
    const auto retained_source = profile / "sessions" / "rollout-retained.jsonl";
    write(retained_source, metadata("retained") + start("turn") +
        tokens(19, ",\"input_tokens\":14,\"output_tokens\":5,\"cached_input_tokens\":0") + complete("turn"));
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
    const auto check_retained = [](const auto& first_page, const auto& last_page) {
        bool found = false;
        for (const auto* page : {&first_page, &last_page}) {
            for (const auto& record : page->records) {
                if (record.record_id != "codex:retained:turn") continue;
                found = true;
                check(record.counter_field == "total_token_usage.total_tokens" &&
                    record.aggregate_evidence == "partial" && record.total_tokens == 19 &&
                    record.input.value == 14 && record.output.value == 5,
                    "actual adapter preserves retained turn-delta attribution and partial evidence");
            }
        }
        check(found, "accepted current record survives packaged scans");
    };
    const auto check_temporal = [](const auto& first_page, const auto& last_page) {
        std::uint64_t instant_tokens = 0;
        std::uint64_t unavailable_tokens = 0;
        for (const auto* page : {&first_page, &last_page}) {
            for (const auto& record : page->records) {
                check(record.temporal && record.temporal->parent.record_id == record.record_id &&
                    record.temporal->parent.total_tokens == record.total_tokens &&
                    record.temporal->parent.snapshot_id == page->snapshot_id &&
                    record.temporal->parent.source_account_key == page->scope.source_account_key,
                    "packaged producer binds optional proof to every actual page/account/parent");
                for (const auto& portion : record.temporal->portions) {
                    check(portion.basis == "instant" && portion.instant == "2026-01-02T12:00:00Z",
                        "packaged producer preserves the certified native completion instant");
                    instant_tokens += portion.total_tokens;
                }
                unavailable_tokens += record.temporal->unavailable_tokens;
            }
        }
        check(instant_tokens == 269 && unavailable_tokens == 20,
            "packaged pages conserve certified deltas separately from first-turn/retained accounting");
    };
    check_retained(first, last);
    check_temporal(first, last);
    check(addon::validate_usage_cache_candidate(*last.cache_candidate, *allocated,
        request.context.account_context, [] { return false; }, &failure).has_value(), "actual adapter cache verifies after paging");
    check(fs::remove(retained_source), "remove the accepted source to exercise retained cache evidence");
    const auto unchanged_candidate = allocation_parent / "scan-unchanged";
    check(fs::create_directory(addon::usage_cache_io_path(unchanged_candidate)), "allocate incremental adapter candidate");
    auto incremental = request;
    incremental.mode = "incremental";
    incremental.snapshot_id.reset();
    incremental.cursor.reset();
    incremental.cache_selection = addon::Usage_cache_selection{
        first.cache_candidate, addon::path_to_utf8(unchanged_candidate)};
    const auto unchanged_id = host.invoke({addon::k_usage_incremental_capability_id, 1}, "scan",
        addon::encode_usage_incremental_request(incremental), 15000, invocation);
    const auto unchanged = host.wait_outcome(unchanged_id, 16000);
    check(unchanged && unchanged->status == addon::Host_status::SUCCESS,
        unchanged ? unchanged->reason_code.c_str() : "actual packaged incremental scan timed out");
    check(!addon::account_context_acknowledgment_failure(*incremental.context.account_context, unchanged->payload) &&
        !addon::profile_storage_acknowledgment_failure(*incremental.context.profile_storage, unchanged->payload),
        "incremental responses acknowledge their account and profile storage at the invocation boundary");
    const auto unchanged_result = addon::decode_usage_incremental_result(unchanged->payload, &failure);
    check(unchanged_result && !unchanged_result->changes &&
        unchanged_result->baseline == *first.cache_candidate &&
        fs::is_empty(addon::usage_cache_io_path(unchanged_candidate)),
        "actual adapter retains its accepted baseline without exporting a replacement snapshot");
    std::string added_bytes = metadata("late");
    for (int index = 0; index < 300; ++index) {
        const auto turn = "turn-" + std::to_string(index);
        added_bytes += start(turn) + tokens(index + 1) + complete(turn);
    }
    const auto added_source = profile / "sessions" / "rollout-late.jsonl";
    write(added_source, added_bytes);
    const auto changed_candidate = allocation_parent / "scan-changed";
    check(fs::create_directory(addon::usage_cache_io_path(changed_candidate)), "allocate changed adapter candidate");
    incremental.cache_selection->candidate_root = addon::path_to_utf8(changed_candidate);
    const auto changed_page = [&] {
        const auto id = host.invoke({addon::k_usage_incremental_capability_id, 1}, "scan",
            addon::encode_usage_incremental_request(incremental), 15000, invocation);
        const auto outcome = host.wait_outcome(id, 16000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            outcome ? outcome->reason_code.c_str() : "actual packaged changed scan timed out");
        const auto result = addon::decode_usage_incremental_result(outcome->payload, &failure);
        check(result && result->changes && result->baseline == *first.cache_candidate,
            "actual adapter returns changes bound to the selected baseline");
        for (const auto& record : result->changes->records) {
            check(record.record_id.starts_with("codex:late:"), "changed pages omit retained accounting");
        }
        return *result->changes;
    };
    const auto changed_first = changed_page();
    check(!changed_first.complete && changed_first.records.size() == 256, "changed observations respect page bounds");
    incremental.snapshot_id = changed_first.snapshot_id;
    incremental.cursor      = changed_first.next_cursor;
    const auto changed_last = changed_page();
    check(changed_last.complete && changed_last.records.size() == 44 &&
        changed_last.cache_candidate == changed_first.cache_candidate,
        "changed continuation exports the remaining observations from the same sealed candidate");
    check(fs::remove(added_source), "remove late fixture after isolated changed scan");
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
    check_retained(replay, replay_last);
    check_temporal(replay, replay_last);
    check(addon::validate_usage_cache_candidate(*replay_last.cache_candidate, *allocated,
        request.context.account_context, [] { return false; }, &failure).has_value(), "replayed cache seals and reopens");
    check(host.shutdown(1000, nullptr), "actual adapter exits without any model request");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Temporary_directory fixture;
        if (argc == 3) {
            const std::string mode = argv[2];
            if (mode == "turn-cwd") {
                reducer_attributes_resumed_turns(fixture.path);
                return 0;
            }
            if (mode == "turn-cwd-cache") {
                scanner_keeps_per_turn_repositories(fixture.path);
                return 0;
            }
            if (mode == "turn-cwd-repair") {
                selected_cache_repairs_turn_repository(fixture.path);
                return 0;
            }
            if (mode == "turn-cwd-selected") {
                selected_cache_repairs_turn_repository(fixture.path);
                return 0;
            }
            if (mode == "archived-selected") {
                scanner_reads_archived_rollouts(fixture.path);
                return 0;
            }
            if (mode == "peer-coverage") {
                selected_cache_preserves_peer_validation(fixture.path);
                return 0;
            }
            if (mode == "selected-page") {
                selected_cache_pages_while_another_account_scans(fixture.path);
                return 0;
            }
            check(mode == "optional-parent" || mode == "optional-members", "known isolation fixture selection");
            adapter_optional_isolation(addon::path_from_utf8(argv[1]), fixture.path,
                mode == "optional-members");
            return 0;
        }
        projector_does_not_retain_ignored_large_payloads();
        reducer_keeps_turn_deltas_resets_and_fork_baselines(fixture.path / "rollout-fixture.jsonl");
        reducer_preserves_fork_turn_precision(fixture.path / "rollout-fixture.jsonl");
        reducer_keeps_categories_across_interrupted_turns(fixture.path / "rollout-fixture.jsonl");
        selected_cache_reparses_previous_parser_revision(fixture.path);
        selected_cache_preserves_previous_rollout_order(fixture.path);
        reducer_attributes_resumed_turns(fixture.path);
        scanner_keeps_per_turn_repositories(fixture.path);
        selected_cache_repairs_turn_repository(fixture.path);
        reducer_proves_only_complete_turns(fixture.path / "rollout-fixture.jsonl");
        scanner_sorts_pages_and_preserves_cache_on_retry(fixture.path);
        scanner_reads_archived_rollouts(fixture.path);
        scanner_accepts_new_profiles_and_retains_transient_empty_history(fixture.path);
        current_cache_replay_and_loss(fixture.path);
        selected_cache_preserves_peer_validation(fixture.path);
        current_scan_cancellation_preserves_accepted_history(fixture.path);
        rejected_scan_cannot_publish_refreshed_history_after_source_deletion(fixture.path);
        selected_cache_preserves_host_authority(fixture.path);
        selected_cache_stays_compact_across_incremental_scans(fixture.path);
        incremental_scan_preserves_the_accepted_generation(fixture.path);
        incremental_scan_reuses_unaffected_pieces(fixture.path);
        incremental_adoption_keeps_unexamined_temporal_evidence(fixture.path);
        selected_cache_reconciles_turns_by_identity(fixture.path);
        selected_cache_retains_repository_attribution(fixture.path);
        selected_cache_keeps_accepted_turns_when_their_rollout_is_parsed(fixture.path);
        selected_cache_keeps_turns_a_rescan_cannot_contradict(fixture.path);
        selected_cache_pages_while_another_account_scans(fixture.path);
        selected_cache_refreshes_missing_temporal_history(fixture.path);
        selected_cache_dates_only_certified_turns(fixture.path);
        selected_cache_spools_a_rollout_while_it_is_read(fixture.path);
        selected_cache_requires_the_rollout_of_each_accepted_record(fixture.path);
        progress_continues_at_a_paced_rate_while_working();
        scans_report_each_stage_while_they_work(fixture.path);
        waiting_for_another_scan_reports_nothing(fixture.path);
        check(argc == 2, "usage tests require the built package directory");
        adapter_optional_isolation(addon::path_from_utf8(argv[1]), fixture.path / "optional-parent", false);
        adapter_optional_isolation(addon::path_from_utf8(argv[1]), fixture.path / "optional-members", true);
        adapter_cache_selection_contract(addon::path_from_utf8(argv[1]), fixture.path / "short-allocation", false);
        adapter_cache_selection_contract(addon::path_from_utf8(argv[1]), fixture.path / "long-allocation", true);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
