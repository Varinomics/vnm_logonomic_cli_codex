// The Logonomic host's usage exchanges with this package, driven through the
// SDK host simulator. The host selects accepted caches and rejects partial
// reductions; current source, missing-source replay and reset remain distinct.

#include "codex_accounts.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_session.h>
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

    fs::path storage_root() const { return addon::path_from_utf8(m_storage_root); }

    // A fresh, host-owned candidate allocation and the accepted cache the
    // host's durable usage state selects. Continuation state and reset mode
    // from an earlier exchange do not carry over.
    void select(std::string_view name, std::optional<addon::Usage_cache_reference> accepted)
    {
        const auto candidate = storage_root() / fs::path(std::string(name));
        check(fs::create_directory(addon::usage_cache_io_path(candidate)), "allocate candidate " + std::string(name));
        m_request.snapshot_id.reset();
        m_request.cursor.reset();
        m_request.mode = "incremental";
        m_request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
    }

    void reset_mode() { m_request.mode = "reset"; }

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
            if (!page->records.empty()) {
                std::vector<addon::Usage_session_identity> identities;
                for (const auto& record : page->records) {
                    check(!record.capabilities.find(addon::k_usage_session_capability_id),
                        "ordinary scan records remain unchanged for hosts without session-fact support");
                    identities.push_back({record.record_id, record.session_key});
                }
                const auto id = m_host.invoke({addon::k_usage_session_capability_id, addon::k_usage_session_version},
                    addon::k_usage_session_describe_operation, addon::encode_usage_session_request(identities), 5000);
                const auto described = m_host.wait_outcome(id, 6000);
                check(described && described->status == addon::Host_status::SUCCESS,
                    "packaged Codex adapter describes fresh and retained usage sessions");
                const auto descriptions = addon::decode_usage_session_result(described->payload, &failure);
                check(descriptions && descriptions->size() == identities.size(), "session facts preserve page cardinality");
                for (std::size_t index = 0; index < identities.size(); ++index) {
                    const auto& description = descriptions->at(index);
                    check(description.identity == identities[index] && description.session &&
                        description.session->provider_session_id == identities[index].session_key &&
                        description.session->excludes_turn_in_flight,
                        "completed-turn records identify their original rollout and exclude its unfinished turn");
                }
            }
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

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage host tests require the built package directory");
        const auto package = addon::path_from_utf8(argv[1]);
        Temporary_directory fixture;
        selected_cache_survives_reduction_and_reset(package, fixture.path() / "acceptance");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
