#include "codex_hook_metadata.h"
#include "codex_model_source.h"
#include "codex_conversation_source.h"
#include "codex_resume_record.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace {

bool asks_for_changes(const logonomic::cli_addon::Json_value& event)
{
    std::string prompt;
    for (const auto key : {"prompt", "user_prompt", "message"}) {
        const auto* value = event.find(key);
        if (value && value->is_string()) { prompt = value->as_string(); break; }
    }
    std::transform(prompt.begin(), prompt.end(), prompt.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); });
    bool presentation = false;
    bool changes = false;
    for (const auto word : {"show", "open", "view", "display", "bring up", "pull up"}) presentation |= prompt.find(word) != std::string::npos;
    for (const auto word : {"changed file", "modified file", "edited file", "my changes", "your changes", "changes you made", "files you changed", "files you modified", "files you edited"}) changes |= prompt.find(word) != std::string::npos;
    return presentation && changes;
}

bool write_context(std::string_view event, const std::string& instruction)
{
    namespace addon = logonomic::cli_addon;
    auto hook = addon::Json_value::object();
    hook.set("hookEventName", addon::Json_value(std::string(event)));
    hook.set("additionalContext", addon::Json_value(instruction));
    auto output = addon::Json_value::object();
    output.set("hookSpecificOutput", std::move(hook));
    const auto bytes = addon::serialize_json(output) + '\n';
    return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size() && std::fflush(stdout) == 0;
}

} // namespace

int main()
{
    namespace addon = logonomic::cli_addon;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        return addon::k_helper_exit_invalid_arguments;
    }
#endif
    const auto started = std::chrono::steady_clock::now();
    std::string input;
    for (int character; (character = std::fgetc(stdin)) != EOF;) {
        if (input.size() >= 4 * 1024 * 1024) {
            return addon::k_helper_exit_invalid_arguments;
        }
        input += static_cast<char>(character);
    }
    addon::Json_parse_limits input_limits;
    input_limits.max_bytes        = 4 * 1024 * 1024;
    input_limits.max_string_bytes = 4 * 1024 * 1024;
    const auto event = addon::parse_json(input, input_limits, nullptr);
    const auto* name = event ? event->find("hook_event_name") : nullptr;
    addon::Contract_failure failure;
    const auto environment = addon::read_helper_environment(&failure);
    if (std::ferror(stdin) || !event || !event->is_object() || !name || !name->is_string() || !environment) {
        return addon::k_helper_exit_invalid_arguments;
    }
    const bool short_hook = name->as_string() == "SessionEnd" || name->as_string() == "Interrupt";
    const auto deadline = started + std::chrono::milliseconds(short_hook ? 1800 : 8000);
    const auto cancelled = [&] { return std::chrono::steady_clock::now() >= deadline; };
    addon::Helper_client client(*environment, cancelled);
    const auto* tool = event->find("tool_name");
    const bool question = tool && tool->is_string() &&
        (tool->as_string() == "request_user_input" || tool->as_string() == "request_user_input_async");
    if (name->as_string() == "PreToolUse" && !question) {
        // Ordinary tools supply a context checkpoint, not question or lifecycle evidence.
        if (!event->find("agent_id")) {
            const auto instruction = client.request_title_instruction("codex", name->as_string(), false,
                addon::Model_context_delivery{});
            if (instruction.completed && instruction.accepted && !instruction.instruction.empty() &&
                !write_context(name->as_string(), instruction.instruction))
            {
                return addon::k_helper_exit_transport_failure;
            }
        }
        return addon::k_helper_exit_committed;
    }
    const auto key   = addon::random_nonce_hex();
    const auto nonce = addon::random_nonce_hex();
    auto metadata = codex::hook_metadata(*event);
    codex::capture_turn_model(*event, metadata);
    if (name->as_string() == "SessionStart" && !event->find("agent_id")) {
        const auto* session = event->find("session_id");
        const auto* profile = std::getenv("CODEX_HOME");
        const auto* sqlite = std::getenv(codex::k_export_sqlite_home_environment);
        // The preparer pins this native store in argv. Do not accept a store
        // supplied by hook JSON or recover it later from an unrelated profile.
        if (session && session->is_string() && profile && sqlite) {
            if (auto record = codex::bound_resume_record(session->as_string(), profile, sqlite)) {
                metadata.set("resume_record", std::move(*record));
            }
        }
    }
    auto outcome = client.deliver_provider_event(name->as_string(), metadata, key, nonce);
    if (!short_hook && !outcome.committed && !cancelled() &&
        (outcome.exit_code == addon::k_helper_exit_transport_failure || outcome.exit_code == addon::k_helper_exit_pending))
    {
        outcome = client.deliver_provider_event(name->as_string(), metadata, key, nonce);
    }
    if (!outcome.committed || !outcome.accepted) {
        std::fputs("Codex hook delivery was not accepted.\n", stderr);
    }
    if (outcome.committed && outcome.accepted && outcome.epoch == 1 && outcome.helper_response &&
        name->as_string() == "SessionStart")
    {
        codex::arm_model_source(*event, addon::json::text(*outcome.helper_response, "model_activation"));
    }
    bool title_accepted = true;
    if (!event->find("agent_id") &&
        (name->as_string() == "SessionStart" || name->as_string() == "UserPromptSubmit"))
    {
        const auto instruction = client.request_title_instruction("codex", name->as_string(),
            name->as_string() == "UserPromptSubmit" && asks_for_changes(*event), addon::Model_context_delivery{});
        title_accepted = instruction.completed && instruction.accepted;
        if (title_accepted && !instruction.instruction.empty()) {
            if (!write_context(name->as_string(), instruction.instruction)) {
                return addon::k_helper_exit_transport_failure;
            }
        }
    }
    if (outcome.committed) (void)client.acknowledge_provider_event(key);
    return outcome.committed && outcome.accepted && title_accepted ? addon::k_helper_exit_committed : addon::k_helper_exit_transport_failure;
}
