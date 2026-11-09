#include "codex_jsonl.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

    const auto inspect = [](std::string output, int exit_code = 0, bool timed_out = false,
        std::string stderr_bytes = {}) {
        addon::Structured_inspection request;
        request.protocol_state = codex::initial_implementation_state();
        request.stdout_bytes = std::move(output);
        request.stderr_bytes = std::move(stderr_bytes);
        request.final = true;
        request.exit_code = exit_code;
        request.timed_out = timed_out;
        return codex::inspect_implementation(request);
    };
    const auto failure_matches = [](const addon::Structured_inspect_result& result,
        std::string_view reason, std::string_view source, std::string_view message, bool truncated = false) {
        return result.complete && result.failure_reason == reason && !result.result_bytes &&
            result.failure_diagnostics && result.failure_diagnostics->source == source &&
            result.failure_diagnostics->message == message &&
            result.failure_diagnostics->truncated == truncated;
    };
    const auto native_error = [](const std::string& type, const std::string& message) {
        auto event = addon::Json_value::object();
        event.set("type", addon::Json_value(type));
        auto error = addon::Json_value::object();
        error.set("message", addon::Json_value(message));
        if (type == "error") {
            event.set("message", addon::Json_value(message));
        }
        else {
            event.set("error", std::move(error));
        }
        return addon::serialize_json(event) + '\n';
    };

    // Native fatal error fields are caller evidence, independently of the
    // existing reason code and of transport chunk boundaries.
    const std::string native_message =
        "Provider request failed: r\xc3\xa9seau unavailable.\nDetails: retry limit reached.";
    for (const std::string type : {"error", "turn.failed"}) {
        const auto event = native_error(type, native_message);
        const auto result = inspect(thread_started + turn_started + event);
        if (!failure_matches(result, "implementation_failed", "native_error", native_message)) {
            std::cerr << "Native error text or existing failure code was lost\n";
            return 1;
        }
        for (std::size_t split = 0; split < event.size(); ++split) {
            addon::Structured_inspection first;
            first.protocol_state = codex::initial_implementation_state();
            first.stdout_bytes = thread_started + turn_started + event.substr(0, split);
            const auto partial = codex::inspect_implementation(first);
            if (partial.complete) {
                std::cerr << "A partial native error record completed early\n";
                return 1;
            }
            addon::Structured_inspection last;
            last.protocol_state = partial.protocol_state;
            last.stdout_bytes = event.substr(split);
            last.final = true;
            if (!failure_matches(codex::inspect_implementation(last),
                    "implementation_failed", "native_error", native_message))
            {
                std::cerr << "Chunking discarded native failure evidence\n";
                return 1;
            }
        }
        const auto legacy_wire = addon::encode_structured_inspect_result(result);
        const auto opted_in_wire = addon::encode_structured_inspect_result(result, true);
        addon::Contract_failure failure;
        const auto decoded = addon::decode_structured_inspect_result(opted_in_wire, true, &failure);
        if (legacy_wire.find("failure_diagnostics") || !opted_in_wire.find("failure_diagnostics") ||
            !decoded || !failure_matches(*decoded, "implementation_failed", "native_error", native_message))
        {
            std::cerr << "Native failure evidence must be readable only on the opted-in wire\n";
            return 1;
        }
    }
    if (!failure_matches(inspect(native_error("error", native_message)),
            "startup_failed", "native_error", native_message) ||
        !failure_matches(inspect("", 1, false, "Not logged in\n"),
            "authentication_required", "native_error", "Not logged in") ||
        !failure_matches(inspect(transcript + native_error("error", native_message)),
            "implementation_failed", "native_error", native_message))
    {
        std::cerr << "Startup, authentication or late native error evidence changed its failure contract\n";
        return 1;
    }

    constexpr std::size_t message_limit = 16 * 1024;
    if (addon::limits::k_max_structured_failure_message_bytes != message_limit) {
        std::cerr << "The negotiated native failure-message contract is 16 KiB\n";
        return 1;
    }
    const std::string exact_message(message_limit, 'x');
    const std::string unicode_prefix(message_limit - 1, 'x');
    const std::string oversized_message = unicode_prefix + "\xe2\x82\xac" + " beyond the limit";
    const auto exact = inspect(thread_started + turn_started + native_error("error", exact_message));
    const auto bounded = inspect(thread_started + turn_started + native_error("turn.failed", oversized_message));
    if (!failure_matches(exact, "implementation_failed", "native_error", exact_message) ||
        !failure_matches(bounded, "implementation_failed", "native_error", unicode_prefix, true) ||
        !addon::is_valid_utf8(bounded.failure_diagnostics->message))
    {
        std::cerr << "Native failure text lost its exact bound or safe UTF-8 truncation\n";
        return 1;
    }
    const std::string nul_message("Before\0after", 12);
    if (!failure_matches(inspect(thread_started + turn_started + native_error("error", nul_message)),
            "implementation_failed", "native_error", std::string("Before") + "\xef\xbf\xbd" + "after"))
    {
        std::cerr << "An embedded NUL must not make the optional native diagnostic unreadable\n";
        return 1;
    }

    const auto malformed = inspect(thread_started + turn_started + "private-invalid-record\n");
    const auto nonzero = inspect(transcript, 7);
    const auto timeout = inspect(transcript, 0, true);
    const auto incomplete = inspect(thread_started + turn_started + answer);
    for (const auto* result : {&malformed, &nonzero, &timeout, &incomplete}) {
        if (!result->complete || result->failure_reason != "implementation_failed" || result->result_bytes) {
            std::cerr << "Diagnostic detail must preserve the existing failed-result contract\n";
            return 1;
        }
    }
    if (!malformed.failure_diagnostics || malformed.failure_diagnostics->source != "protocol_error" ||
        malformed.failure_diagnostics->message.empty() ||
        malformed.failure_diagnostics->message.find("private-invalid-record") != std::string::npos ||
        !nonzero.failure_diagnostics || nonzero.failure_diagnostics->source != "process_exit" ||
        nonzero.failure_diagnostics->message.find('7') == std::string::npos ||
        !timeout.failure_diagnostics || timeout.failure_diagnostics->source != "process_exit" ||
        timeout.failure_diagnostics->message.find("timed out") == std::string::npos ||
        !incomplete.failure_diagnostics || incomplete.failure_diagnostics->source != "protocol_error" ||
        incomplete.failure_diagnostics->message.find("turn.completed") == std::string::npos)
    {
        std::cerr << "Parser and process failures need useful evidence without copying a raw invalid record\n";
        return 1;
    }
    return 0;
}
