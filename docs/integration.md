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

### Native conversation activation and default directory

Root `SessionStart` observations retain the exact native session ID. Native
`startup`, `clear` and `fork` sources establish a new conversation; `resume`
establishes a resumed activation, including a return to a previously observed
ID. The provider sends one STARTED observation with the native source. The host
owns the epoch, stable conversation/Notes association, departure retention and
late-event rejection. `SessionEnd` retains the ending ID and native reason;
it does not retarget the current conversation. Native `compact` starts and
`PostCompact` update context without creating a departed conversation.

When preparation resolves and pins the local native store, the helper captures
that `CODEX_HOME` and `sqlite_home` with the ID in an opaque `native_store`
version 1 resume record. These values come from the prepared process environment,
not hook-supplied extra fields. A delivery retry uses the same helper key and
transition nonce. A separate native start gets a fresh nonce; it is not deduplicated
merely because the conversation ID appeared earlier.

With `execution-context@1` negotiated, the same accepted start carries the exact
native `cwd`, environment `local`, its activation nonce and sequence zero.
Absolute nested and non-Git paths are valid, and no path normalization or Git-root
replacement is performed. Missing or invalid cwd yields an attributed UNKNOWN
observation while identity remains usable. The provider does not infer the
default directory from a transcript, launch directory, tool-specific `workdir`,
child shell, title or typed command. It does not emit later cwd-only observations:
those require an authoritative default-directory signal captured with the
activation identity at its native source. Host-initiated resume continues to pass
`resume -C <host-selected-directory> <exact-id>`; this argument alone is not
confirmation that Codex accepted that directory.

These are reporting hooks, with a specific native observation boundary. Source
review at Codex `748ab55ca7de0b647d9a7a9d9a8e70e00f28666d` still shows
[`SessionStart` dispatch during turn processing](https://github.com/openai/codex/blob/748ab55ca7de0b647d9a7a9d9a8e70e00f28666d/codex-rs/core/src/session/turn.rs)
and [reuse of an already loaded thread on resume](https://github.com/openai/codex/blob/748ab55ca7de0b647d9a7a9d9a8e70e00f28666d/codex-rs/app-server/src/request_processors/thread_processor.rs).
Consequently an idle `/clear`, `/new` or cold `/resume` need not be reported until
a turn, and returning to a loaded conversation need not emit a new start at all.
The provider cannot promise immediate picker selection, idle A → B → A, or its
accepted cwd from this hook path. No command text or file-activity heuristic fills
that gap. The [native start hook](https://github.com/openai/codex/blob/748ab55ca7de0b647d9a7a9d9a8e70e00f28666d/codex-rs/hooks/src/events/session_start.rs)
can stop processing a turn, but that is not a proven veto or rollback of a TUI
selection. Rejecting a reporting hook does not prevent two native live owners.
Equivalent cross-worktree adoption and exclusive ownership remain bounded by
the shared host contract and a suitable native selection control.

Codex builds with native hook trust present their own startup hook review.
Continuing without trusting leaves the prepared hooks inactive. Logonomic does
not override that choice. An isolated Windows TUI check with Codex `0.160.0`
confirmed this review for command-line hooks and continuation without trusting.

`codex_resume_tests` checks these event shapes, nonce-attributed cwd, native-history
inspection and packaged host negotiation. A disposable Windows native App Server
check with Codex `0.160.0` and a no-auth loopback model confirmed deferred initial
starts and loaded A → B → A return without another `SessionStart`. The returned
thread's prompt and stop hooks still carried its exact ID. Those facts establish
the engine boundary; they do not establish which loaded thread the TUI displays.
The provider does not promote an unmatched prompt into an activation; the host
rejects events whose identity differs from its current epoch. A background
thread's prompt does not prove a foreground selection. Actual TUI picker
cancellation, selection order and cross-directory reassociation need their own
native acceptance; sending slash-command text is not proof of selection.

Interactive launch and resume reuse the native configuration read described
below to keep the terminal title's activity field available. If an explicit
`tui.terminal_title` list omits both `activity` and its `spinner` alias, preparation
prepends `spinner` through a process-local `-c` override. Every configured field
and its relative order is preserved; an explicit empty list becomes activity
alone. An absent or null list keeps Codex's native default. Configuration files
and unrelated TUI settings are unchanged. This preparation requires the existing
native configuration probe (Codex 0.149.0 or newer); unsupported named profiles
or a failed probe retain their original title configuration. Preparation also
keeps the original title when the override would exceed the host's launch
argument contract.

In Codex 0.160.0, native Action Required titles require the activity field even
for a static title. These titles confirm a retained permission or question
candidate; unrelated terminal text does not create attention. Resumed activity
can resolve an inferred permission or asynchronous-question wait. The provider
preserves disabled `tui.animations`, `tui.effects.progress`, and native
accessibility settings, so those settings can withhold the activity evidence
used for resumption. A later native completion or stop remains authoritative.
The [native title implementation](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/tui/src/chatwidget/status_surfaces.rs)
defines these signals. Native
[`request_permissions`](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/core/src/session/mod.rs)
and [MCP elicitation](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/core/src/session/mcp.rs)
bypass the hooks used to retain these candidates at their actual wait, so their
exact per-request attention lifetimes still require a typed native
pending/resolved integration.

Source inspection of Codex 0.165.0 also shows
[OSC 7501 program status](https://github.com/openai/codex/blob/360319d8dee62d87e2d59fc0c21805d79519cfed/codex-rs/tui/src/terminal_program_status.rs)
on terminal stdout, independent of terminal-title selections. Its
[status calculation](https://github.com/openai/codex/blob/360319d8dee62d87e2d59fc0c21805d79519cfed/codex-rs/tui/src/chatwidget/status_surfaces.rs)
represents the displayed thread. Blocked frames carry `state=blocked:app=codex`
and `kind=permission`, `question` or `auth`, providing a potential foreground
attention signal where hook candidates are unavailable. Logonomic's
[terminal parser](https://github.com/Varinomics/vnm_terminal_surface/blob/886349b663aabec9f4ab8640ae23511a2661897c/src/terminal_byte_stream_parser.cpp)
currently lacks OSC 7501 handling, so transport and displayed-thread ownership
need an explicit integration.

Frames contain no session, turn or request IDs, and `state=clear` also occurs
during [handoff to another program](https://github.com/openai/codex/blob/360319d8dee62d87e2d59fc0c21805d79519cfed/codex-rs/tui/src/tui.rs).
Clear, nonblocked state or a displayed-thread switch cannot retire exact root,
child or background requests; those lifetimes still require their owning
pending/resolved events.

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

The provider also declares optional `interactive-resume@3` and `@4`, with the
same preparation/startup declaration as `@2`. Read-only inspection checks the
exact saved native store and account before selecting history; it does not run
Codex or a model. Version 3 reports availability and the exact-request digest.
Version 4 additionally reports independent UNKNOWN, EMPTY or NONEMPTY content.
An original ID-only record has no trustworthy stored location for later
inspection and therefore remains UNKNOWN. It retains the older explicit
resume path. A new bound record is inspected afresh for explicit preparation,
then its saved store is pinned and verified through the native config reader;
changed launcher defaults cannot silently redirect that resume.
UNKNOWN inspection leaves this explicit path available: a supported native
reader can resolve history that the provider cannot inspect. The exact ID and
private startup confirmation still govern association. Invalid scope or failed
native-store verification remains a preparation failure.

The [conversation reader](conversation_export.md) supplies the inspection's
identity and replay checks. Complete understood metadata or injected context
without meaningful conversation content can be EMPTY. Canonical user,
assistant, tool or agent content is NONEMPTY when its history is understood.
Partial final writes, malformed or unsupported history, inherited/reference
ambiguity and unsupported compaction remain UNKNOWN. Every inspection reads a
fresh snapshot, so a completed later write can change the answer without
reconstructing the saved record. A missing SQLite row or file alone remains
UNKNOWN: native legacy filesystem fallback or a live writer can still resolve
the conversation. Inspection never deletes or alters native history. The optional
inspection reads at most a 64 MiB source and honors cancellation; larger or
inaccessible sources remain UNKNOWN, while explicit export keeps its existing
snapshot behavior.

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
credential's issuer and stable provider user ID, workspace and native backend
routing. Separate logins for the same provider user retain the same identity even
when their token subjects differ or are absent. Profile account keys still own
their separate conversations and usage history. Unsupported or
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

A delegated implementation runs as `codex <launcher options> exec`. Its sandbox
comes from the launcher's explicit sandbox flags, `--yolo`, configuration and
profile under native precedence. Native `exec` cannot request human approval,
so preparation refuses an explicit `--ask-for-approval on-request` instead of
silently replacing it. Explicit `never` and sandbox flags are retained; `never`
also gets an exec-local configuration override because native exec does not
inherit the TUI flag and automatic review can change its default policy.
Interactive Continue and exact resume also retain
`on-request`. The long names, short names and attached-value forms are supported.
Invalid values, repeated single-value options and approval/bypass conflicts
remain unavailable. The package's
only grant is `default_tools_approval_mode = "approve"` on the task's own Task
servers, whose tools are limited to the task's actions. A task that edits files
needs a launcher or configuration that allows writes, for example
`-c sandbox_mode="workspace-write"`. Without `--yolo`, `codex exec` runs only
inside a Git repository, so preparation refuses another working directory with
`untrusted_working_directory`.
