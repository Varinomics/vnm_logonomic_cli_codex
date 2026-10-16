# Provider integration

The provider recognizes installed Codex launchers, preserves the selected
`CODEX_HOME` account, injects session hooks through command-line configuration,
and prepares delegated implementation sessions with required task-control MCP
servers. Native executable and stock npm launcher chains are verified before
interactive or delegated preparation. Package runners can be recognized without
being admitted as interactive launch chains.

## Hooks and guidance

Codex hooks carry session, turn, attention and subagent facts through the SDK's
authenticated helper lane. Title and coordination instructions are requested
from the host's one-shot admission service and returned using Codex's
`hookSpecificOutput.additionalContext` format. The helper does not send prompt
content through the lifecycle ledger or fabricate instructions after admission
fails.

## Live-session wake

`session-wake@1` queues a host-authored mailbox notification for the exact live
conversation UUID. Preparation captures the original CLI's external-queue
watcher support (introduced in 0.149.0), verifies effective configuration, and
pins `CODEX_HOME` and `sqlite_home` before the recipient starts. An unsupported
wake offer leaves ordinary launching available. Changed executable-chain files,
unresolved stores and nonlocal persistence make that live binding unavailable.
The native App Server does not accept the TUI's `--profile` selector, so those
launchers remain wake-unavailable; selected `CODEX_HOME` accounts are supported.

The package uses native `thread/queue/list` and `thread/queue/add` over a
short-lived App Server stdio connection to the bound store. It never loads,
resumes, steers or starts that conversation in the control process. This avoids
`codex queue`'s implicit daemon selection: an unrelated default daemon may use
a different store, and the CLI refuses an embedded writer with configuration
overrides while that daemon runs. The writer checks effective configuration in
the same control process before enqueueing, refusing changed managed store
requirements even when they override the pinned command-line setting.
Codex's external queue watcher delivers to
the original process after ordinary idle (with a polling delay); busy sessions
wait, and native interruption remains paused. The host also suppresses wake
while an approval or interruption is pending. Native acceptance is a queue
receipt, never mailbox consumption or an agent acknowledgment. A lost or
mismatched reply is uncertain and must not be automatically retried because
`clientUserMessageId` is correlation, not native deduplication.

The opt-in `tests/codex_native_wake_tests.py --codex <native executable>
--driver <codex_session_wake_tests>` gate creates a disposable profile and a
loopback model fixture. It exercises production preparation/inspection/notify,
the original idle engine, busy completion, interruption and an unrelated store.
It never uses a user's model account. This establishes native engine behavior;
terminal presentation and the host's durable mailbox need their own gates.

## Exact interactive resume

Exact interactive resume preserves the canonical provider session UUID and
requires a private startup observer. Before opening the requested conversation,
the package inspects effective configuration, refuses an existing telemetry log
exporter, and verifies startup telemetry using an isolated saved conversation.
The probe does not request a model turn. Authentication headers remain in the
launched process environment, while vendor OTLP interpretation belongs to this
package.

## Initial prompt

An interactive-initial-prompt@1 launch starts Codex's interactive TUI with the
host's text as its positional `PROMPT`, placed after `--`, and echoes the text's
SHA-256. The replacement invocation runs the verified program directly, so no
shell layer quotes the text. The package refuses with `initial_prompt_unavailable`
when it cannot guarantee that the text becomes the first user message: a launcher
whose program it cannot verify, a Codex whose help shows no positional `PROMPT`,
launcher arguments other than known session options, repeating an option Codex
takes once, or with `--image` attachments, a text starting with `!` (which Codex
runs as a local shell command), or an invocation beyond the launch-preparation@3
argument bounds. The echo covers what this launch passes to Codex as the first
message; the user's own Codex configuration is outside it, for example a Windows
sandbox that still needs elevated setup, under which Codex may hold the message
in its composer for the user to send. It grants no access to the referenced
file; Codex reads it under the permissions of the selected configuration.

On Linux, probes inherit the generation custodian's process group and session.
Probe cleanup confirms its direct child within a bounded settlement interval;
descendants remain under generation custody until retirement. A successful probe
establishes the startup protocol evidence, not that its entire descendant tree
has exited.

## Accounts and authentication

Authentication, quota, account discovery and per-turn usage scanning preserve the
native integration's semantics. Historical account, resume and usage imports
match the exact `codex` provider attribution. Structured read-only review is not
advertised. Lifecycle support requires Codex 0.146.0 or newer; delegated
implementation requires 0.150.1 or newer. Version checks have no upper bound,
and accepted protocol records still undergo structural and identity validation.

With `current-account@1` negotiated, authentication uses the selected launcher's
native app-server in the existing authentication observation. The helper reads
`account/read` and `getAuthStatus` and derives an opaque identity from the selected
credential's principal, workspace and native backend routing. Profile account
keys still own their separate conversations and usage history. Unsupported or
incoherent identity evidence leaves profiles separate; it does not infer equality
from email, plan or a local credential file. Native stdout, stderr, tokens and
errors remain private to the helper; only validated authentication state and the
opaque identity leave it. Authentication retains its own native exit, cancellation
and deadline boundary, independently of quota. Without the opt-in, authentication
continues to use native `login status`.

With `account-refresh@1` negotiated, a full refresh uses one fresh native
app-server for quota followed by authentication. Its host-selected launcher,
profile, working directory and environment are chosen once; native configuration
reloads, credential renewal and account-change resynchronization remain native
decisions. Valid quota publishes before authentication and stays accepted if
authentication or the later native exit fails. Authentication and optional
`current-account@1` identity still require successful native completion. Codex
admits this optimization only when current-account observation is requested:
native app-server AUTH has different semantics from `login status` for custom
providers. Otherwise preparation declines before native birth and the host keeps
the separate quota and ordinary AUTH route. This is Codex's availability rule,
not a dependency of the generic account-refresh capability. The helper emits only
sanitized observations, and the host owns cancellation, each observation deadline
and final retirement. Hosts without the shared capability also keep the separate
quota and AUTH processes.

## Delegated implementation permissions

A delegated implementation runs as `codex <launcher options> exec` and keeps the
launcher's permission posture: its sandbox and approvals come from the
launcher's own `--yolo`, `-c` settings or `-p` profile and from the user's
`config.toml`, while `codex exec` itself never asks for approval. The package's
only grant is `default_tools_approval_mode = "approve"` on the task's own Task
servers, whose tools are limited to the task's actions. A task that edits files
needs a launcher or configuration that allows writes, for example
`-c sandbox_mode="workspace-write"`. Without `--yolo`, `codex exec` runs only
inside a Git repository, so preparation refuses another working directory with
`untrusted_working_directory`.
