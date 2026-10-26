#include "codex_launcher_approval.h"

namespace codex {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"codex", "codex.exe", "codex.cmd", "codex.bat", "codex.ps1"},
        .canonical_argument = "--yolo",
        .bypass_arguments = {"--yolo", "--dangerously-bypass-approvals-and-sandbox"},
        .approval_modes = {
            {"-a",                 {}, {"untrusted", "on-failure", "on-request", "never"}},
            {"--ask-for-approval", {}, {"untrusted", "on-failure", "on-request", "never"}},
            {"-s",                {}, {"read-only", "workspace-write", "danger-full-access"}},
            {"--sandbox",         {}, {"read-only", "workspace-write", "danger-full-access"}},
        },
        .conflicting_arguments = {"--full-auto", "--approve-for-me", "--not-so-yolo"},
        .value_arguments = {
            "-c", "--config", "--enable", "--disable", "-m", "--model", "--local-provider",
            "-p", "--profile", "--add-dir", "-C", "--cd",
        },
        .optional_value_arguments = {},
        .flag_arguments = {
            "--oss", "--search", "--no-alt-screen", "--strict-config", "--no-daemon",
            "--worktree", "--dangerously-bypass-hook-trust", "--last", "--all",
            "--include-non-interactive",
        },
        .session_commands = {"resume", "fork"},
        .unsupported_commands = {
            "exec", "e", "review", "login", "logout", "mcp", "mcp-server", "app", "app-server",
            "completion", "sandbox", "debug", "apply", "a", "cloud", "features", "agents",
            "daemon", "help", "version", "update",
        },
        .variadic_arguments = {
            {.argument = "-i", .requires_value = true, .attached_value_terminates = true,
                .empty_values_allowed = false, .value_separator = ','},
            {.argument = "--image", .requires_value = true, .attached_value_terminates = true,
                .empty_values_allowed = false, .value_separator = ','},
        },
    };
    return rules;
}

} // namespace codex
