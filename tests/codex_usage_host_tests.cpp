// The Logonomic host's usage exchanges with this package, driven through the
// SDK host simulator: an explicit import of retained native history, replay
// of that history from the host-selected cache, and the answers the host
// needs to reject a partial reduction and to recover by reset.
//
// Each step plays the host's side of usage-scan@3 and legacy-state-import@1:
// it allocates the candidate, chooses the accepted selection the way the host
// does after accepting or rejecting a page, and writes the native seed in the
// host's FILE_CACHE export form. What is checked is only what this package
// answers; the host's own acceptance rules are tested in Logonomic against
// fixture packages.

#include "codex_accounts.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

constexpr const char* k_package_id = "logonomic.cli.codex";

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(static_cast<bool>(output), "write fixture file " + addon::path_to_utf8(path));
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::temp_directory_path() / ("codex-usage-host-" + addon::random_nonce_hex()))
    {
        check(fs::create_directory(m_path), "create isolated fixture");
        m_path = fs::canonical(m_path);
    }

    ~Temporary_directory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

// One completed turn of rollout telemetry whose cumulative counter ends at
// `tokens`.
std::string rollout_turn(std::string_view rollout, int tokens)
{
    return
        "{\"type\":\"session_meta\",\"payload\":{\"id\":\"" + std::string(rollout) +
            "\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"cwd\":\"\"}}\n"
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-one\","
            "\"started_at\":1767225601}}\n"
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":"
            "{\"total_tokens\":" + std::to_string(tokens) + "}}}}\n"
        "{\"type\":\"event_msg\",\"timestamp\":\"2026-01-02T12:00:00Z\",\"payload\":{\"type\":\"task_complete\","
            "\"turn_id\":\"turn-one\"}}\n";
}

addon::Json_value breakdown_field(std::optional<int> tokens, bool supported)
{
    auto field = addon::Json_value::object();
    field.set("support", addon::Json_value(supported ? "supported" : "unsupported"));
    field.set("covered_records", addon::Json_value(tokens ? 1 : 0));
    field.set("total_records", addon::Json_value(1));
    if (tokens) {
        field.set("known_tokens", addon::Json_value(*tokens));
    }
    return field;
}

// One line of the host's native FILE_CACHE export for a retained native turn,
// after the host converted its identity to this package: the stored record
// keeps its native fields, its session_id becomes the hex of the converted
// record ID, and usage-record-identity carries both identities.
std::string retained_turn_seed_line(const fs::path& source, std::string_view rollout, int input, int output)
{
    const std::string record_id = "codex:" + std::string(rollout) + ":turn-one";
    auto breakdown = addon::Json_value::object();
    breakdown.set("version", addon::Json_value(2));
    breakdown.set("coverage_unit", addon::Json_value("sessions"));
    breakdown.set("input_tokens", breakdown_field(input, true));
    breakdown.set("output_tokens", breakdown_field(output, true));
    breakdown.set("cache_read_input_tokens", breakdown_field(0, true));
    breakdown.set("cache_creation_input_tokens", breakdown_field(std::nullopt, false));
    auto identity = addon::Json_value::object();
    identity.set("version", addon::Json_value(1));
    identity.set("record_id", addon::Json_value(record_id));
    identity.set("session_key", addon::Json_value(rollout));
    identity.set("legacy_record_id", addon::Json_value(record_id));
    identity.set("legacy_session_key", addon::Json_value(rollout));
    auto capabilities = addon::Json_value::object();
    capabilities.set("token_breakdown", std::move(breakdown));
    capabilities.set("usage-record-identity", std::move(identity));

    auto record = addon::Json_value::object();
    record.set("session_id", addon::Json_value(addon::to_hex(
        reinterpret_cast<const std::uint8_t*>(record_id.data()), record_id.size())));
    record.set("rollout_id", addon::Json_value(rollout));
    record.set("turn_id", addon::Json_value("turn-one"));
    record.set("completed_date", addon::Json_value("2026-01-02"));
    record.set("total_tokens", addon::Json_value(input + output));
    record.set("source_path", addon::Json_value(addon::path_to_utf8(source)));
    record.set("recorded_cwd", addon::Json_value(""));
    record.set("model", addon::Json_value("native-model"));
    record.set("counter_field", addon::Json_value("total_token_usage.total_tokens"));
    record.set("counter_semantics", addon::Json_value("cumulative_turn_delta"));
    record.set("capabilities", std::move(capabilities));

    auto markers = addon::Json_value::object();
    auto marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(2));
    marker.set("parser_revision", addon::Json_value(2));
    markers.set("token_breakdown_cache", std::move(marker));
    marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(1));
    marker.set("parser_revision", addon::Json_value(1));
    markers.set("repository_usage_cache", std::move(marker));
    auto records = addon::Json_value::array();
    records.push_back(std::move(record));
    auto file = addon::Json_value::object();
    file.set("path", addon::Json_value(addon::path_to_utf8(source)));
    file.set("size", addon::Json_value(0));
    file.set("modified_msecs", addon::Json_value(0));
    file.set("records", std::move(records));
    file.set("capabilities", std::move(markers));
    // The host serializes stored records with sorted members.
    return addon::serialize_json_canonical(file) + '\n';
}

// The host side of one account's usage exchanges with the packaged adapter.
class Packaged_host
{
public:
    Packaged_host(const fs::path& package, const fs::path& fixture, const fs::path& profile)
    :
        m_host(simulator_options(package, fixture), nullptr)
    {
        addon::Contract_failure failure;
        check(m_host.start(&failure), "packaged adapter starts: " + failure.detail);
        const auto live = m_host.live_capabilities();
        const auto is_live = [&live](const addon::Capability_pair& pair) {
            return std::find(live.begin(), live.end(), pair) != live.end();
        };
        check(is_live({"usage-scan", 3}), "the package negotiates host-selected cache acceptance");
        check(is_live({"legacy-state-import", 1}), "the package negotiates native history import");

        m_account_key = codex::profile_account_key(addon::path_to_utf8(profile));
        std::string error;
        const auto allocated = addon::allocate_profile_storage_root(
            m_host.profile_storage_root(), k_package_id, &error, m_account_key);
        check(allocated.has_value(), "allocate account profile storage: " + error);
        m_storage_root = *allocated;

        m_request.context.launcher        = {"codex", addon::k_runner_direct, {}};
        m_request.context.launcher_argv   = {"codex"};
        m_request.context.profile_storage = addon::Profile_storage_context{1, m_storage_root};
        m_request.context.account_context = addon::Account_context{2, m_account_key, addon::path_to_utf8(profile)};
        m_request.context.launch_environment = addon::Launch_environment{
            addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
        m_invocation.profile_storage = m_request.context.profile_storage;

        const auto acknowledge = m_host.invoke({"profile-storage", 1}, "acknowledge",
            addon::Json_value::object(), 5000, m_invocation);
        const auto acknowledged = m_host.wait_outcome(acknowledge, 6000);
        check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
            "packaged adapter acknowledges its account profile storage");
    }

    void finish()
    {
        check(m_host.shutdown(1000, nullptr), "packaged adapter exits by itself, without contacting a model");
    }

    const std::string& account_key() const { return m_account_key; }
    fs::path storage_root() const { return addon::path_from_utf8(m_storage_root); }

    // A fresh, host-owned candidate allocation and the accepted cache the
    // host's durable usage state selects. Continuation state, reset mode and
    // any native seed from an earlier exchange do not carry over.
    void select(std::string_view name, std::optional<addon::Usage_cache_reference> accepted)
    {
        const auto candidate = storage_root() / fs::path(std::string(name));
        check(fs::create_directory(addon::usage_cache_io_path(candidate)), "allocate candidate " + std::string(name));
        m_request.snapshot_id.reset();
        m_request.cursor.reset();
        m_request.mode = "incremental";
        m_request.native_file_cache.reset();
        m_request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
    }

    void reset_mode() { m_request.mode = "reset"; }

    // The retained native seed as the host writes it: beside the account's
    // storage, with its digest and the account it was exported from.
    void seed(std::string_view name, std::string_view bytes, bool cache_required)
    {
        const auto path = storage_root() / fs::path(std::string(name));
        write(path, bytes);
        m_request.native_file_cache = addon::Usage_native_file_cache{
            addon::path_to_utf8(path), addon::to_hex(addon::Sha256::digest(bytes)), m_account_key, cache_required};
    }

    addon::Host_outcome scan_outcome()
    {
        const auto id = m_host.invoke({"usage-scan", 3}, "scan",
            addon::encode_usage_scan_request(m_request, 3), 15000, m_invocation);
        const auto outcome = m_host.wait_outcome(id, 16000);
        check(outcome.has_value(), "packaged usage scan finishes");
        return *outcome;
    }

    // Every page of one snapshot, as the host reads them before staging.
    addon::Usage_scan_page scan()
    {
        std::optional<addon::Usage_scan_page> snapshot;
        for (;;) {
            const auto outcome = scan_outcome();
            check(outcome.status == addon::Host_status::SUCCESS, "packaged usage scan succeeds: " + outcome.reason_code);
            check(!addon::account_context_acknowledgment_failure(*m_request.context.account_context, outcome.payload),
                "every page acknowledges the exact account context");
            check(!addon::profile_storage_acknowledgment_failure(*m_request.context.profile_storage, outcome.payload),
                "every page acknowledges the exact profile storage");
            addon::Contract_failure failure;
            auto page = addon::decode_usage_scan_result(outcome.payload, &failure, 3);
            check(page.has_value(), "usage page decodes: " + failure.detail);
            check(page->cache_candidate && page->cache_candidate->root == m_request.cache_selection->candidate_root,
                "every page names the host's candidate allocation");
            if (!snapshot) {
                snapshot = *page;
            }
            else {
                check(page->cache_candidate == snapshot->cache_candidate, "the candidate is identical across pages");
                snapshot->records.insert(snapshot->records.end(), page->records.begin(), page->records.end());
                snapshot->complete = page->complete;
            }
            if (page->complete) {
                break;
            }
            m_request.snapshot_id = page->snapshot_id;
            m_request.cursor      = page->next_cursor;
        }
        return *snapshot;
    }

    bool sealed(const addon::Usage_cache_reference& reference) const
    {
        addon::Contract_failure failure;
        return addon::validate_usage_cache_candidate(reference, m_storage_root, m_request.context.account_context,
            [] { return false; }, &failure).has_value();
    }

    bool candidate_sealed() const
    {
        return fs::exists(addon::usage_cache_io_path(
            addon::path_from_utf8(m_request.cache_selection->candidate_root)) / "cache-manifest.json");
    }

    std::optional<addon::Json_value> import_record(std::string_view kind, const addon::Json_value& record)
    {
        const auto id = m_host.invoke({"legacy-state-import", 1}, addon::k_import_record_operation,
            addon::encode_import_record_request(kind, record), 5000);
        const auto outcome = m_host.wait_outcome(id, 6000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS, "legacy import answers");
        addon::Contract_failure failure;
        const auto result = addon::decode_import_record_result(outcome->payload, &failure);
        check(result.has_value(), "legacy import result decodes: " + failure.detail);
        if (!result->matched()) {
            return std::nullopt;
        }
        return *result->conversion();
    }

private:
    static addon::Simulator_options simulator_options(const fs::path& package, const fs::path& fixture)
    {
        addon::Simulator_options options;
        options.package_directory    = package;
        options.profile_storage_root = fixture / "host-storage";
        return options;
    }

    addon::Host_simulator               m_host;
    std::string                         m_account_key;
    std::string                         m_storage_root;
    addon::Usage_scan_request           m_request;
    addon::Simulator_invocation_options m_invocation;
};

addon::Usage_record only_record(const addon::Usage_scan_page& page, const std::string& step)
{
    check(page.complete && page.records.size() == 1, step + ": one completed turn");
    return page.records.front();
}

// addon_usage_cache_acceptance_packaged_codex, package half: the answers
// that let the host keep its selection when a surviving source shrinks, and
// recover by reset.
void selected_cache_survives_reduction_and_reset(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "codex-profile";
    const auto rollout = profile / "sessions" / "rollout-acceptance.jsonl";
    write(rollout, rollout_turn("host-acceptance", 19));
    Packaged_host host(package, fixture, profile);

    host.select("accepted", std::nullopt);
    const auto accepted = host.scan();
    const auto first = only_record(accepted, "initial scan");
    check(first.record_id == "codex:host-acceptance:turn-one" && first.total_tokens == 19,
        "initial scan reports the completed turn");
    check(first.aggregate_evidence == "partial", "a parsed rollout does not certify a complete aggregate");
    check(host.sealed(*accepted.cache_candidate), "initial candidate is sealed before it is returned");

    // The host rejects this page: a partial aggregate cannot lower an
    // accepted total. The package's part is to label the reduction partial.
    write(rollout, rollout_turn("host-acceptance", 7));
    host.select("reduced", accepted.cache_candidate);
    const auto reduced = host.scan();
    const auto lower = only_record(reduced, "reduced scan");
    check(lower.total_tokens == 7 && lower.aggregate_evidence == "partial",
        "a surviving reduction is reported as partial evidence");
    check(host.sealed(*accepted.cache_candidate), "a newer candidate leaves the selected cache intact");

    // With the source gone, the host still selects its accepted cache, not
    // the rejected newer candidate.
    check(fs::remove(rollout), "remove the rollout");
    host.select("replayed", accepted.cache_candidate);
    const auto replayed = host.scan();
    check(only_record(replayed, "replay").total_tokens == 19,
        "the host-selected cache is replayed, never the rejected candidate");
    check(replayed.cache_candidate != accepted.cache_candidate, "replay seals a new candidate");
    check(host.sealed(*accepted.cache_candidate) && host.sealed(*reduced.cache_candidate),
        "the package never prunes selected or unaccepted candidates");

    // Reset reconstructs from current sources only, and may reduce usage.
    write(rollout, rollout_turn("host-acceptance", 7));
    host.select("reset", std::nullopt);
    host.reset_mode();
    const auto reset = host.scan();
    check(only_record(reset, "reset").total_tokens == 7, "reset reconstructs from the current rollout");

    // A corrupt selected cache fails the scan instead of falling back, and
    // reset never reads it.
    write(addon::usage_cache_io_path(addon::path_from_utf8(reset.cache_candidate->root)) / "cache-manifest.json",
        "corrupt");
    host.select("corrupt-selection", reset.cache_candidate);
    const auto refused = host.scan_outcome();
    check(refused.status == addon::Host_status::FAILED && refused.reason_code == "usage_cache_unavailable",
        "a corrupt selected cache fails the scan: " + refused.reason_code);
    check(!host.candidate_sealed(), "a refused scan seals no candidate");
    host.select("reset-after-corruption", std::nullopt);
    host.reset_mode();
    check(only_record(host.scan(), "reset after corruption").total_tokens == 7,
        "reset recovers without consuming the corrupt selection");
    host.finish();
}

// addon_usage_cache_import_packaged_codex, package half: the identity and
// seed exchanges of an explicit import of retained native history whose
// rollout no longer exists, and its replay from the sealed cache.
void retained_native_turn_imports_and_replays(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-codex";
    fs::create_directories(profile / "sessions");
    const auto source = profile / "sessions" / "rollout-import.jsonl";
    Packaged_host host(package, fixture, profile);

    addon::Legacy_account_record account{"codex", "native-account", addon::path_to_utf8(profile)};
    const auto resolved = host.import_record(addon::k_legacy_account_record_kind,
        addon::encode_legacy_account_record(account));
    const auto* resolved_key = resolved ? resolved->find("adapter_account_key") : nullptr;
    check(resolved_key && resolved_key->is_string() && resolved_key->as_string() == host.account_key(),
        "the native account maps to the profile's add-on account");

    // The host converts every retained record's identity before the scan and
    // back again on rollback; a native turn keeps its identity both ways.
    const std::string record_id = "codex:import-history:turn-one";
    for (const std::string direction : {"to-add-on", "to-legacy"}) {
        addon::Legacy_usage_records records{direction, "codex", "native-account", host.account_key(), {}};
        records.records.push_back({0, record_id, std::string("import-history"), addon::path_to_utf8(source)});
        const auto converted = host.import_record(addon::k_legacy_usage_records_kind,
            addon::encode_legacy_usage_records(records));
        check(converted.has_value(), direction + " conversion matches the native turn");
        addon::Contract_failure failure;
        const auto identities = addon::decode_legacy_usage_conversion(*converted, direction, &failure);
        check(identities && identities->size() == 1 && identities->front().record_id == record_id &&
            identities->front().session_key == std::optional<std::string>("import-history"),
            direction + " conversion keeps the native turn identity");
    }

    // A required seed the host could not fill fails the import instead of
    // starting the account from nothing.
    host.select("missing-seed", std::nullopt);
    host.seed("native-file-cache-missing.jsonl", "", true);
    const auto missing = host.scan_outcome();
    check(missing.status == addon::Host_status::FAILED && missing.reason_code == "required_usage_cache_unavailable",
        "an empty required seed fails the import: " + missing.reason_code);
    check(!host.candidate_sealed(), "a failed import seals no candidate");

    host.select("import", std::nullopt);
    host.seed("native-file-cache-import.jsonl", retained_turn_seed_line(source, "import-history", 14, 5), true);
    const auto imported = host.scan();
    const auto check_turn = [&](const addon::Usage_scan_page& page, const std::string& step) {
        const auto turn = only_record(page, step);
        check(turn.record_id == record_id && turn.session_key == std::optional<std::string>("import-history"),
            step + ": the converted native identity is kept");
        check(turn.total_tokens == 19 && turn.input.value == std::optional<std::uint64_t>(14) &&
            turn.output.value == std::optional<std::uint64_t>(5),
            step + ": retained counts and categories are kept");
        check(turn.source_counter_semantics == std::optional<std::string>("cumulative_turn_delta") &&
            turn.aggregate_evidence == std::optional<std::string>("partial"),
            step + ": a retained turn delta is not a complete aggregate");
        check(turn.source_path == std::optional<std::string>(addon::path_to_utf8(source)) &&
            turn.completed_date == "2026-01-02" && turn.model == std::optional<std::string>("native-model") &&
            turn.counter_field == std::optional<std::string>("total_token_usage.total_tokens"),
            step + ": native provenance is kept for the host's merge");
    };
    check_turn(imported, "import");
    check(host.sealed(*imported.cache_candidate), "the imported history is sealed before it is returned");

    // Reopening reconciles from the host-selected cache, without a seed.
    host.select("replay", imported.cache_candidate);
    const auto replayed = host.scan();
    check_turn(replayed, "replay");
    check(replayed.cache_candidate != imported.cache_candidate, "replay seals a new candidate");
    host.finish();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage host tests require the built package directory");
        const auto package = addon::path_from_utf8(argv[1]);
        Temporary_directory fixture;
        selected_cache_survives_reduction_and_reset(package, fixture.path() / "acceptance");
        retained_native_turn_imports_and_replays(package, fixture.path() / "import");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
