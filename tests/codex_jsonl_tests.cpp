#include "codex_jsonl.h"

#include <iostream>
#include <string>

int main()
{
    namespace addon = logonomic::cli_addon;
    // Oracle: committed native Codex exec JSONL reducer requires one root,
    // one turn, distinct completed items and clean process exit.
    const std::string transcript =
        "{\"type\":\"thread.started\",\"thread_id\":\"thread-a\"}\n"
        "{\"type\":\"turn.started\"}\n"
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item-a\",\"type\":\"agent_message\",\"text\":\"Completed change\"}}\n"
        "{\"type\":\"turn.completed\"}\n";
    for (std::size_t split = 0; split <= transcript.size(); ++split) {
        addon::Structured_inspection first;
        first.protocol_state = codex::initial_implementation_state();
        first.stdout_bytes   = transcript.substr(0, split);
        const auto partial = codex::inspect_implementation(first);
        if (partial.complete) {
            std::cerr << "Premature completion\n";
            return 1;
        }
        addon::Structured_inspection last;
        last.protocol_state = partial.protocol_state;
        last.stdout_bytes   = transcript.substr(split);
        last.final          = true;
        const auto result = codex::inspect_implementation(last);
        if (!result.complete || result.result_bytes != "Completed change" || result.failure_reason) {
            std::cerr << "Chunked authenticated result lost\n";
            return 1;
        }
    }
    for (const std::string suffix : {
             "{\"type\":\"turn.started\"}\n",
             "{\"type\":\"thread.started\",\"thread_id\":\"thread-b\"}\n",
             "trailing"})
    {
        addon::Structured_inspection request;
        request.protocol_state = codex::initial_implementation_state();
        request.stdout_bytes   = transcript + suffix;
        request.final          = true;
        const auto result = codex::inspect_implementation(request);
        if (!result.complete || !result.failure_reason || result.result_bytes) {
            std::cerr << "Invalid completion accepted\n";
            return 1;
        }
    }
    return 0;
}
