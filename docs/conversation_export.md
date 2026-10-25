# Conversation extraction

The optional `conversation-export@1` capability extracts a retained Codex
conversation into the host's request directory as UTF-8 Markdown, plain text or
the common conversation format.
The shared SDK owns the request, snapshot reader, writer and result contract.
Codex owns native source selection and interpretation.

## Live conversation identity limitation

Live-terminal extraction uses the native conversation ID most recently confirmed
to Logonomic. That identity does not prove which conversation the Codex TUI is
currently displaying. After conversation A has been confirmed, an idle internal
`/resume` to conversation B can leave A selected for extraction until another
authoritative identity event arrives. The export can therefore contain the
previous conversation; validating its native file does not resolve this gap.

Codex `0.160.0` and `0.164.0` defer `SessionStart` hooks until turn processing.
Returning to an already loaded thread can also reuse its native session without
another `conversation_starts` initialization event. An upgrade to `0.164.0` or
an observer of initialization events alone does not establish every TUI selection.

An exact resume initiated by Logonomic supplies an explicit native target and
confirms startup through its authenticated observer, establishing that identity
before a new prompt. This guarantee applies to that resume; a later unobserved
internal switch has the limitation above. Historical exports use their explicitly
selected session identity.

The timing and loaded-thread behavior follow the native
[pending hook dispatch](https://github.com/openai/codex/blob/rust-v0.164.0/codex-rs/core/src/session/turn.rs)
and [loaded-thread resume path](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/app-server/src/request_processors/thread_processor.rs).

## Native history selection and rendering

Source selection uses the original selected account profile, or the frozen
`CODEX_HOME` / `HOME` / `USERPROFILE` environment when no account was selected.
Preparation resolves and pins Codex's native state store through its existing
configuration reader, including ordinary launches without a wake request.
The provider's `VNM_CODEX_EXPORT_SQLITE_HOME` environment entry retains that
selection in the host's frozen export context. Optional preparation failure
clears the entry and leaves ordinary launch available.

With that binding, the provider opens `state_5.sqlite` read-only and selects
`threads.rollout_path` by exact native session ID. The selected file must be
inside the original account's `sessions` or `archived_sessions` directory,
after resolving symbolic links. It checks the first `session_meta` identity
and history mode, and reopens the database after reading to reject a changed
selection. Another state-store generation is unavailable rather than a reason
to use an old database. Windows uses the system SQLite library; other platforms
link SQLite3 through CMake.

Older contexts without a captured store can still select legacy JSONL by exact
filename and metadata identity. They cannot select paginated history: native
revert can retain that original filename after selecting a replacement rollout.
Later metadata in a legacy fork can belong to its copied parent. Its copied
history is already in the child file and is exported once.

`response_item` records supply message bodies, tool calls and tool results.
The corresponding `user_message` and `agent_message` events are duplicate
representations and do not produce another message. Retained reasoning text
and attachment references are included. Embedded image/audio bytes are
identified as embedded attachments, not copied as base64 into the text file.
Provider-side truncation already present in a native tool result is preserved.
Each encrypted inter-agent message is represented by `[Encrypted message]` in
its original position, replacing its routing envelope and opaque payload.
Consecutive encrypted messages remain separate, including conversations whose
messages are all encrypted. Plaintext inter-agent messages retain their text.

Compact extraction keeps user discussion and assistant replies whose native phase
is `final_answer`, and omits injected AGENTS.md and environment context, explicit
`commentary`, reasoning, tool traffic and plaintext inter-agent messages.
The native paired context markers identify injected input fragments; full
extraction preserves their canonical text and user role. Assistant text without a recognized phase is
retained conservatively; it is not guessed to be progress from its wording or
position. Every encrypted-message placeholder remains in both modes. Unchecking
Compact extracts the full retained discussion again from the native source;
filtering never changes that source or hides content inside the exported file.

Legacy rollback markers remove the corresponding retained turns using persisted
`task_started` / `turn_started` boundaries. A rollback whose boundaries are
unavailable fails explicitly. Ordinary JSONL rollouts retain their earlier
message bodies across compaction; replacement model-context copies are not
added a second time. Encrypted-only reasoning and model configuration records
are not conversation text.

The reader freezes the file length when opening it. It exports complete records
within that prefix and reports an unfinished final record. Malformed complete
records, ambiguous files, mismatched identities and unsupported content fail
without publishing a partial output. Self-contained paginated rollouts require
contiguous record ordinals beginning at zero. Their `item_completed` records
update native presentation items and do not add duplicate copies of canonical
response bodies. Referenced history, inherited paginated segments, paginated
rollback markers, compressed rollouts and unsupported record layouts remain
unavailable; neighboring or superseded JSONL files are never substitutes.

## Native schema and test oracle

The native interpretation follows these upstream definitions:

- [Rollout payload serialization](https://github.com/openai/codex/blob/main/codex-rs/history/src/rollout_payload.rs)
- [Response item and content types](https://github.com/openai/codex/blob/main/codex-rs/protocol/src/models.rs)
- [Injected user instructions](https://github.com/openai/codex/blob/main/codex-rs/core/src/context/user_instructions.rs)
- [Context fragment markers](https://github.com/openai/codex/blob/main/codex-rs/context-fragments/src/fragment.rs)
- [First-metadata identity and fork replay](https://github.com/openai/codex/blob/main/codex-rs/rollout/src/recorder.rs)
- [Rollback semantics](https://github.com/openai/codex/blob/main/codex-rs/core/src/thread_rollout_truncation.rs)
- [Explicit turn replay and rollback](https://github.com/openai/codex/blob/main/codex-rs/app-server-protocol/src/protocol/thread_history.rs)
- [Persistence policy](https://github.com/openai/codex/blob/main/codex-rs/rollout/src/policy.rs)

These sources were inspected at `970111f6cb62cc02fe36a82ea7d71b37a2889c97`.
That revision records the test oracle's provenance; runtime support is based
on native data shape, not a CLI version restriction. `codex_conversation_tests`
uses schema-focused fixtures to check identity, copied history, duplicate event
representations, rollback, attachment references, large tool content, partial
tails and unsupported layouts. It does not claim a native CLI execution test.

Paginated source selection and ordinal semantics additionally follow the
`rust-v0.160.0` definitions for
[current rollout selection](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/thread-store/src/local/thread_rollout_resolver.rs),
[revert](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/thread-store/src/local/revert_thread.rs),
[record ordinals](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/rollout/src/ordinal.rs), and
[presentation item updates](https://github.com/openai/codex/blob/rust-v0.160.0/codex-rs/app-server-protocol/src/protocol/thread_history_projection.rs).
