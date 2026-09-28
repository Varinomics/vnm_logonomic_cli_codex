#include "codex_jsonl.h"

#include <iostream>
#include <string>

int main()
{
    namespace addon = logonomic::cli_addon;
    // Oracle: committed native Codex exec JSONL reducer requires one root,
    // one turn, distinct completed items and clean process exit. Codex exec
    // (codex-rs/exec/src/exec_events.rs) reports a warning as a completed
    // "error" item, a non-fatal notice, at any point after thread.started;
    // its fatal errors are the top-level "error" event and turn.failed.
    const auto completes_in_any_chunking = [](const std::string& transcript) {
        for (std::size_t split = 0; split <= transcript.size(); ++split) {
            addon::Structured_inspection first;
            first.protocol_state = codex::initial_implementation_state();
            first.stdout_bytes   = transcript.substr(0, split);
            const auto partial = codex::inspect_implementation(first);
            if (partial.complete) {
                std::cerr << "Premature completion\n";
                return false;
            }
            addon::Structured_inspection last;
            last.protocol_state = partial.protocol_state;
            last.stdout_bytes   = transcript.substr(split);
            last.final          = true;
            const auto result = codex::inspect_implementation(last);
            if (!result.complete || result.result_bytes != "Completed change" || result.failure_reason) {
                std::cerr << "Chunked authenticated result lost\n";
                return false;
            }
        }
        return true;
    };
    const auto refused = [](const std::string& transcript) {
        addon::Structured_inspection request;
        request.protocol_state = codex::initial_implementation_state();
        request.stdout_bytes   = transcript;
        request.final          = true;
        const auto result = codex::inspect_implementation(request);
        return result.complete && result.failure_reason && !result.result_bytes;
    };

    const std::string thread_started = "{\"type\":\"thread.started\",\"thread_id\":\"thread-a\"}\n";
    const std::string turn_started   = "{\"type\":\"turn.started\"}\n";
    const std::string turn_completed = "{\"type\":\"turn.completed\"}\n";
    const std::string answer =
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item-a\",\"type\":\"agent_message\",\"text\":\"Completed change\"}}\n";
    const std::string transcript = thread_started + turn_started + answer + turn_completed;
    if (!completes_in_any_chunking(transcript)) {
        return 1;
    }
    for (const std::string suffix : {
             "{\"type\":\"turn.started\"}\n",
             "{\"type\":\"thread.started\",\"thread_id\":\"thread-b\"}\n",
             "trailing"})
    {
        if (!refused(transcript + suffix)) {
            std::cerr << "Invalid completion accepted\n";
            return 1;
        }
    }

    // Codex 0.157 output for a configured model it has no metadata for.
    const std::string metadata_notice =
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_0\",\"type\":\"error\",\"message\":"
        "\"Model metadata for `scripted-model` not found. Defaulting to fallback metadata; "
        "this can degrade performance and cause issues.\"}}\n";
    const std::string turn_notice =
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_1\",\"type\":\"error\",\"message\":"
        "\"in-process app-server event stream lagged; dropped 3 events\"}}\n";
    if (!completes_in_any_chunking(thread_started + metadata_notice + turn_started + turn_notice + answer + turn_completed)) {
        std::cerr << "A notice before or during the turn must not fail the task\n";
        return 1;
    }

    const std::string before_turn[] = {
        "{\"type\":\"error\",\"message\":\"Unauthorized\"}\n",
        "{\"type\":\"item.started\",\"item\":{\"id\":\"item_0\",\"type\":\"error\",\"message\":\"Notice\"}}\n",
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_0\",\"type\":\"agent_message\",\"text\":\"Early\"}}\n",
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_0\",\"type\":\"error\",\"message\":\"Notice\"}}\n"
            "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_0\",\"type\":\"error\",\"message\":\"Notice\"}}\n",
        "{\"type\":\"item.completed\",\"item\":{\"type\":\"error\",\"message\":\"Notice\"}}\n",
    };
    for (const auto& events : before_turn) {
        if (!refused(thread_started + events + turn_started + answer + turn_completed)) {
            std::cerr << "An error, a started item, a non-notice item, a repeated item ID or an item "
                "without an ID before the turn must fail the task\n";
            return 1;
        }
    }
    if (!refused(metadata_notice + thread_started + turn_started + answer + turn_completed)) {
        std::cerr << "A notice before the thread starts must fail the task\n";
        return 1;
    }
    return 0;
}
