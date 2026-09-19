#include "codex_hook_metadata.h"

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
#include <string>

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
    const auto key   = addon::random_nonce_hex();
    const auto nonce = addon::random_nonce_hex();
    const auto metadata = codex::hook_metadata(*event);
    auto outcome = client.deliver_provider_event(name->as_string(), metadata, key, nonce);
    if (!short_hook && !outcome.committed && !cancelled() &&
        (outcome.exit_code == addon::k_helper_exit_transport_failure || outcome.exit_code == addon::k_helper_exit_pending))
    {
        outcome = client.deliver_provider_event(name->as_string(), metadata, key, nonce);
    }
    if (!outcome.committed || !outcome.accepted) {
        std::fputs("Codex hook delivery was not accepted.\n", stderr);
    }
    bool title_accepted = true;
    if (name->as_string() == "SessionStart" || name->as_string() == "UserPromptSubmit") {
        const auto instruction = client.request_title_instruction("codex", name->as_string(),
            name->as_string() == "UserPromptSubmit" && asks_for_changes(*event));
        title_accepted = instruction.completed && instruction.accepted;
        if (title_accepted && !instruction.instruction.empty()) {
            auto hook = addon::Json_value::object();
            hook.set("hookEventName", addon::Json_value(name->as_string()));
            hook.set("additionalContext", addon::Json_value(instruction.instruction));
            auto output = addon::Json_value::object();
            output.set("hookSpecificOutput", std::move(hook));
            const auto bytes = addon::serialize_json(output) + '\n';
            if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout) != 0) {
                return addon::k_helper_exit_transport_failure;
            }
        }
    }
    if (outcome.committed) (void)client.acknowledge_provider_event(key);
    return outcome.committed && outcome.accepted && title_accepted ? addon::k_helper_exit_committed : addon::k_helper_exit_transport_failure;
}
