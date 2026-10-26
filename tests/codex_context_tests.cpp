#include "codex_hook_metadata.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

addon::Json_value json(std::string_view bytes)
{
    auto value = addon::parse_json(bytes, {}, nullptr);
    check(value.has_value(), "fixture JSON parses");
    return *value;
}

void write(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(static_cast<bool>(output), "write isolated native rollout");
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::temp_directory_path() / ("codex-context-" + addon::random_nonce_hex()))
    {
        check(fs::create_directory(m_path), "create fixture directory");
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

// Native oracle: openai/codex rust-v0.160.0, rollout recorder, thread-store
// revert/rollback and core session compaction installation. Legacy appends all
// installed Compacted markers. Paginated/reverted history may omit prior ones.
const std::string k_legacy = R"({"timestamp":"2026-10-26T10:00:00Z","type":"session_meta","payload":{"id":"session-a","history_mode":"legacy","history_base":null}})" "\n";
const std::string k_compacted = R"({"timestamp":"2026-10-26T10:00:01Z","type":"compacted","payload":{"message":"summary","replacement_history":[]}})" "\n";
const std::string k_usage = R"({"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"total_tokens":999999},"last_token_usage":{"total_tokens":2400},"model_context_window":10000}}})" "\n";

class Packaged_host
{
public:
    Packaged_host(const fs::path& package, const fs::path& fixture)
    :
        m_host(options(package, fixture), nullptr)
    {
        addon::Contract_failure failure;
        check(m_host.start(&failure), "packaged adapter starts: " + failure.detail);
        const auto live = m_host.live_capabilities();
        check(std::find(live.begin(), live.end(), addon::Capability_pair{"context-state", 2}) != live.end(),
            "context-state@2 is declared and negotiated by the actual package");
        check(std::find(live.begin(), live.end(), addon::Capability_pair{"context-state", 1}) == live.end(),
            "the package does not promise the superseded context contract");
    }

    addon::Classify_event_request request(const fs::path& transcript, std::string_view hook = "Stop")
    {
        auto native = json(R"({"hook_event_name":"Stop","session_id":"session-a","turn_id":"turn-a","cwd":"private","prompt":"private","transcript_path":null})");
        native.set("hook_event_name", addon::Json_value(hook));
        native.set("transcript_path", addon::Json_value(addon::path_to_utf8(transcript)));
        addon::Classify_event_request result;
        result.event.name = hook;
        result.event.body = codex::hook_metadata(native);
        check(!result.event.body.find("prompt") && !result.event.body.find("cwd") &&
            addon::json::text(result.event.body, "transcript_path") == addon::path_to_utf8(transcript),
            "the helper's bounded metadata preserves the exact transcript without prompt or cwd");
        result.ledger.current_epoch = 1;
        result.ledger.current_transition = addon::Ledger_transition{
            addon::Transition_kind::START, "session-a", std::string(32, 'a')};
        result.transition_nonce = std::string(32, 'b');
        result.context_state_version = 2;
        result.context_snapshot_revision = ++m_revision;
        return result;
    }

    addon::Classify_event_result classify(const addon::Classify_event_request& request)
    {
        return classify(addon::encode_classify_event_request(request));
    }

    addon::Classify_event_result classify(const addon::Json_value& payload)
    {
        const auto id = m_host.invoke({"helper-ingress", 2}, "classify_event", payload, 5000);
        const auto outcome = m_host.wait_outcome(id, 6000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            "packaged hook classification succeeds" + (outcome ? ": " + outcome->message : ""));
        addon::Contract_failure failure;
        const auto result = addon::decode_classify_event_result(outcome->payload, &failure, 2);
        check(result.has_value(), "packaged result decodes: " + failure.detail);
        return *result;
    }

    addon::Context_update snapshot(const fs::path& path)
    {
        const auto input = request(path);
        const auto result = classify(input);
        check(result.classification == addon::Classification::EVENT && result.observations.size() == 1 &&
            result.observations.front().phase_transition == addon::Phase_transition::AWAITING_INPUT,
            "optional telemetry preserves native Stop lifecycle");
        check(result.context_updates.size() == 1 && !result.context_failure,
            "one native snapshot crosses the package codec");
        const auto update = result.context_updates.front();
        check(update.provider_session_id == "session-a" && update.compaction_count_revision == input.context_snapshot_revision &&
            !update.compacted, "only the actual fresh-read request orders an absolute count, without a second event count");
        return update;
    }

    addon::Attention_classification_result attention(const addon::Classify_event_request& request)
    {
        addon::Attention_classification_request input;
        input.base = request;
        const auto id = m_host.invoke({"attention-kind", 1}, "classify_event",
            addon::encode_attention_classification_request(input), 5000);
        const auto outcome = m_host.wait_outcome(id, 6000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged attention classification succeeds");
        const auto result = addon::decode_attention_classification_result(outcome->payload, nullptr, 2);
        check(result.has_value(), "packaged attention result decodes");
        return *result;
    }

    void finish()
    {
        check(m_host.shutdown(1000, nullptr), "packaged adapter shuts down normally");
    }

private:
    static addon::Simulator_options options(const fs::path& package, const fs::path& fixture)
    {
        addon::Simulator_options result;
        result.package_directory = package;
        result.scratch_directory = fixture / "adapter";
        return result;
    }

    addon::Host_simulator m_host;
    std::uint64_t m_revision = 0;
};

void fresh_reads(Packaged_host& host, const fs::path& path)
{
    const auto original = k_legacy + k_compacted + k_usage;
    write(path, original);
    const auto first = host.snapshot(path);
    check(first.compaction_count == 1 && first.compaction_count_complete && first.used_tokens == 2400 &&
        first.context_window == 10000 && !first.usage_unavailable, "legacy count and current usage are independent of billing totals");
    addon::Context_state state;
    addon::apply_context_update(state, first);

    fs::remove(path);
    const auto lost = host.snapshot(path);
    check(lost.compaction_count == 0 && !lost.compaction_count_complete && lost.usage_unavailable,
        "read loss is scoped incomplete zero, never complete zero");
    addon::apply_context_update(state, lost);
    check(state.compaction_count == 1 && !state.compaction_count_complete && !state.used_tokens,
        "loss retains the known lower bound and withdraws completeness and usage");

    write(path, original);
    const auto restored = host.snapshot(path);
    check(restored.event_id == first.event_id && restored.compaction_count_revision > lost.compaction_count_revision,
        "identical native bytes are sampled anew after loss, without inventing a native identity");
    addon::apply_context_update(state, restored);
    check(state.compaction_count == 1 && state.compaction_count_complete && state.used_tokens == 2400,
        "a new successful current read restores completeness");
    addon::apply_context_update(state, lost);
    check(state.compaction_count == 1 && state.compaction_count_complete,
        "stale loss cannot change current count or completeness");
    auto equal = lost;
    equal.compaction_count_revision = restored.compaction_count_revision;
    addon::apply_context_update(state, equal);
    check(state.compaction_count == 1 && state.compaction_count_complete,
        "equal revision replay cannot change count or completeness");

    write(path, k_legacy + k_usage);
    addon::apply_context_update(state, host.snapshot(path));
    check(state.compaction_count == 1 && !state.compaction_count_complete,
        "a lower complete-looking readable replacement contradicts the retained count");
    write(path, original + k_compacted);
    const auto compacted = host.snapshot(path);
    check(compacted.compaction_count == 2 && compacted.usage_unavailable && !compacted.used_tokens,
        "installed compaction clears usage measured against its prior history");
    auto post = host.request(path, "PostCompact");
    const auto post_result = host.classify(post);
    check(post_result.classification == addon::Classification::IGNORED && post_result.observations.empty() &&
        post_result.context_updates.size() == 1 && post_result.context_updates.front().compaction_count == 2 &&
        !post_result.context_updates.front().compacted, "PostCompact reads installed history without adding another success");
    write(path, original + k_compacted + k_usage);
    addon::apply_context_update(state, host.snapshot(path));
    check(state.compaction_count == 2 && state.compaction_count_complete && state.used_tokens == 2400,
        "new native usage after compaction restores current occupancy");
}

void incomplete_histories(Packaged_host& host, const fs::path& path)
{
    for (const auto metadata : {
            R"({"type":"session_meta","ordinal":0,"payload":{"id":"session-a","history_mode":"paginated","history_base":null}})",
            R"({"type":"session_meta","payload":{"id":"session-a","history_mode":"paginated","history_base":{"thread_id":"session-a","rollout_id":"old","ordinal_exclusive":5}}})",
            R"({"type":"session_meta","payload":{"id":"session-a","forked_from_id":"parent"}})",
            R"({"type":"session_meta","payload":{"id":"session-a","subagent_history_start_ordinal":3}})"})
    {
        write(path, std::string(metadata) + '\n' + k_compacted + k_usage);
        const auto update = host.snapshot(path);
        check(update.compaction_count == 1 && !update.compaction_count_complete && update.used_tokens == 2400,
            "paginated/replaced/forked history preserves observed count and usage without promising full history");
    }
    for (const auto& suffix : {k_legacy, std::string("{\"type\":\"unknown_native_record\",\"payload\":{}}\n")}) {
        write(path, k_legacy + k_compacted + k_usage + suffix);
        check(!host.snapshot(path).compaction_count_complete, "repeated ownership or unknown native history remains incomplete");
    }
    write(path, k_legacy + k_compacted + k_usage + "{\"type\":\"compacted\"");
    const auto partial = host.snapshot(path);
    check(partial.compaction_count == 1 && !partial.compaction_count_complete && partial.usage_unavailable,
        "an incomplete final record withdraws usage and completeness without erasing parsed successes");
}

void optional_isolation(Packaged_host& host, const fs::path& path)
{
    for (const auto& bytes : {
            std::string("{\"type\":\"session_meta\",\"payload\":{\"id\":\"other-session\"}}\n") + k_usage,
            k_legacy + "{\"type\":\"compacted\",\"payload\":{\"message\":null}}\n",
            k_legacy + "{\"type\":\"compacted\",\"type\":\"response_item\",\"payload\":{\"message\":\"x\"}}\n",
            k_legacy + "{\"type\":\"response_item\",\"payload\":{\"ignored\":[1,]}}\n"})
    {
        write(path, bytes);
        const auto update = host.snapshot(path);
        check(update.compaction_count == 0 && !update.compaction_count_complete && update.usage_unavailable,
            "wrong ownership or malformed native JSON isolates optional telemetry from Stop");
    }
    write(path, k_legacy + k_compacted + k_usage);
    auto request = host.request(path);
    request.context_snapshot_revision.reset();
    auto result = host.classify(request);
    check(result.context_updates.size() == 1 && result.context_updates.front().used_tokens == 2400 &&
        !result.context_updates.front().compaction_count && !result.context_updates.front().compaction_count_revision &&
        !result.context_updates.front().compaction_count_complete, "without read authority only occupancy is reported");
    request.context_state_version = 0;
    result = host.classify(request);
    check(result.context_updates.empty() && !result.context_failure && result.observations.size() == 1,
        "unnegotiated optional telemetry is omitted while lifecycle remains valid");

    request = host.request(path);
    for (const auto& invalid : {addon::Json_value(0), addon::Json_value(-1), addon::Json_value(1.5),
            addon::Json_value(std::uint64_t{9007199254740992}), addon::Json_value("2")})
    {
        auto wire = addon::encode_classify_event_request(request);
        wire.set("context_snapshot_revision", invalid);
        result = host.classify(wire);
        check(result.observations.size() == 1 && result.context_updates.empty() && result.context_failure &&
            result.context_failure->provider_session_id == "session-a",
            "invalid optional revisions preserve lifecycle and report independently known native session scope");
    }
    request.context_snapshot_revision = 9007199254740991;
    result = host.classify(request);
    check(result.context_updates.size() == 1 && result.context_updates.front().compaction_count_revision == request.context_snapshot_revision,
        "the exact integer upper boundary survives the packaged codec");
    request = host.request(path);
    request.event.body.set("agent_id", addon::Json_value("child"));
    check(host.classify(request).context_updates.empty(), "a child hook cannot claim root context");
    request = host.request(path);
    request.ledger.current_transition->provider_session_id = "other-session";
    check(host.classify(request).context_updates.empty(), "a stale hook cannot read for a different active session");

    request = host.request(path, "PreToolUse");
    request.event.body.set("tool_name", addon::Json_value("request_user_input"));
    request.event.body.set("tool_use_id", addon::Json_value("call-a"));
    request.event.body.set("tool_input", json(R"({"questions":[{"id":"scope","header":"Scope","question":"Which directory?"}]})"));
    const auto attention = host.attention(request);
    check(attention.attention_kind && attention.attention_kind->kind == "interactive_question_candidate" &&
        attention.base.observations.size() == 1 && attention.base.context_updates.size() == 1 &&
        attention.base.context_updates.front().compaction_count_revision == request.context_snapshot_revision,
        "question classification and optional context survive the packaged attention codec together");

    write(path, k_legacy + k_usage + R"({"type":"event_msg","payload":{"type":"token_count","info":null}})" "\n");
    check(host.snapshot(path).usage_unavailable, "native null token info withdraws earlier occupancy");
    const std::string large_response = "{\"type\":\"response_item\",\"payload\":{\"content\":\"" +
        std::string(2 * 1024 * 1024, 'x') + "\"}}\n";
    write(path, k_legacy + large_response + k_compacted + k_usage);
    const auto large = host.snapshot(path);
    check(large.compaction_count_complete && large.compaction_count == 1 && large.used_tokens == 2400,
        "large unrelated transcript payload is streamed without losing context evidence");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        // A local transport fixture for exercising the packaged helper itself.
        // It records only the synthetic event selected by the focused gate.
        if (argc == 4 && std::string_view(argv[1]) == "provider-event") {
            const char* capture = std::getenv("CODEX_CONTEXT_TEST_CAPTURE");
            check(capture != nullptr, "helper transport fixture requires an isolated capture path");
            if (std::string_view(argv[2]) == "deliver" && std::string_view(argv[3]) == "--args-stdin") {
                const std::string bytes{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
                write(addon::path_from_utf8(capture), bytes);
                std::cout << "{\"accepted\":true,\"epoch\":1}";
                return 0;
            }
            if (std::string_view(argv[2]) == "acknowledge") {
                return 0;
            }
            return 2;
        }
        check(argc == 2, "expected packaged adapter directory");
        Temporary_directory fixture;
        Packaged_host host(addon::path_from_utf8(argv[1]), fixture.path());
        const auto rollout = fixture.path() / "rollout.jsonl";
        fresh_reads(host, rollout);
        incomplete_histories(host, rollout);
        optional_isolation(host, rollout);
        host.finish();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
