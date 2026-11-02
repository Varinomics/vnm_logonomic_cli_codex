# P18: Codex discovery, hooks and context

## Outcome and continuation

This investigation rechecks findings 43, 44, 65 and 134 against provider source
`e6abe97d55af462b0fcff81e8d7878393c00126b` and Logonomic SDK/host source
`b9e44f6fe131624d48b0338c23295dae67d526bd`. These identify inspected inputs;
owned dependency declarations continue to track `master`.

The native approval-parser investigation is complete for the two available
executables. It does not support the proposed four-value widening. The macOS
executable-recognition gap has an isolated fail-first source-branch reproduction.
Hook process costs and the
rollout reader remain under investigation; no performance change is justified
by the historical estimates alone.

Work continues on branch `work/linux-review-20261102-p18-provider-codex` in
`Varinomics/vnm_logonomic_cli_codex`. This is the first investigation checkpoint;
publication and the focused C++ gates are pending. The next actions are to
complete the Mach-O recognition correction and portable fixtures, build the
focused standalone provider targets, and measure bounded rollout and helper
workloads. No native macOS or Windows result is claimed.

## Finding dispositions

| Finding | Current evidence | Disposition | Remaining evidence |
| --- | --- | --- | --- |
| 43 | Discovery prefers a resolvable npm `codex-pet`, but executable-chain verification implements PE and ELF only. An isolated compilation of the original macOS branch returns unavailable for an ARM64 Mach-O header. | Source-confirmed fallthrough and isolated fail-first reproduction; scoped correction and fixtures in progress. | Actual macOS ARM64 npm link discovery/preparation and native payload execution. |
| 44 | Matcher-free ordinary root `PreToolUse` synchronously requests host context. The complete process chain includes an additional private control core omitted from the historical sketch. | Source-confirmed work per emitted hook; no measured Windows latency or production optimization yet. | Complete root/subagent, empty/pending-context host path and stage timings. |
| 65 | Each classified context snapshot freshly reads, projects and hashes the whole observed rollout with a one-second loop budget. | Linear per-read cost is source-confirmed; concrete size/latency threshold is not yet measured. | Bounded rollout sizes, resume/compaction, concurrent append and fresh-read checks. |
| 134 | Fifty no-model probes against native 0.164.0 and 0.166.0-alpha.1 accept `on-request`/`never` and reject `untrusted`/`on-failure`. | Do not widen all four values or narrow the separate launcher-text transformation rules. Older `untrusted` compatibility remains conditional. | An older accepting native executable and an exact affected preparation reproduction. |

## 43: macOS npm executable chain

[`discover_launchers`](https://github.com/Varinomics/vnm_logonomic_cli_codex/blob/e6abe97d55af462b0fcff81e8d7878393c00126b/src/codex_launcher.cpp)
prefers a resolvable `codex-pet`. POSIX program resolution canonicalizes the npm
link to `codex.js`, which selects npm verification. That verification requires
both the resolved Node executable and the bundled Codex payload to pass
`native_target`. The implementation has Windows PE and Linux ELF branches and
otherwise returns no target.

The affected population is auto-discovered profiles while an npm pet launcher
is preferred. The evidence does not establish failure of every macOS Codex
installation or a manually selected native Codex executable. The correction
must recognize supported Mach-O architectures, including ordinary universal
Node distributions, without weakening existing chain checks. Portable header
fixtures can establish parser behavior; they cannot establish native loading,
Rosetta selection, signing, or actual macOS npm execution.

## 44: synchronous hook work

The provider's
[`codex_preparation.cpp`](https://github.com/Varinomics/vnm_logonomic_cli_codex/blob/e6abe97d55af462b0fcff81e8d7878393c00126b/src/codex_preparation.cpp)
installs `PreToolUse` without a matcher, through an explicit PowerShell wrapper
on Windows or `sh -c` on POSIX. The native hook runner invokes its configured
outer shell as well; shell exec behavior determines actual process occurrence.
The root helper then calls the public `logonomic_vnmctl` wrapper, which starts
`logonomic_vnmctl_core` before the host control round trip. The historical
three-process description is therefore incomplete.

For an ordinary non-question root tool,
[`codex_helper.cpp`](https://github.com/Varinomics/vnm_logonomic_cli_codex/blob/e6abe97d55af462b0fcff81e8d7878393c00126b/src/codex_helper.cpp)
requests title/model context and returns without provider-event classification
or rollout scanning. A present `agent_id` suppresses that request only after
shell/helper startup and payload parsing. This cost is separate from finding
65. The provider matches every emitted `PreToolUse`; upstream tools do not all
emit it. For example, the current native `write_stdin` continuation omits a
second pre-tool hook.

The actual host renders current pending context and commits its claims during
the request. A remembered empty response cannot prove that the next checkpoint
is empty. Restoring a question-only matcher, throttling requests, or adding a
provider-only pending marker would change the adopted delivery semantics. No
such shortcut has been selected. Measurements against a fake wrapper will be
labelled component evidence and cannot establish Windows shipping latency or
justify an optimization of the complete host path.

## 65: rollout context processing

[`read_snapshot`](https://github.com/Varinomics/vnm_logonomic_cli_codex/blob/e6abe97d55af462b0fcff81e8d7878393c00126b/src/codex_context.cpp)
opens the canonical rollout, records its observed size and modification time,
then feeds each 64 KiB block through a fresh context projector and SHA-256.
It rejects changed size, modification time or canonical path after the read.
The one-second guard is checked between blocks, so it is not an exact upper
bound on total classification or hook completion.

The
[`context-state@2` contract](https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/sdk/logonomic_cli_addon_sdk/docs/context_state_2.md)
requires a current read after the host's sampling request. Cached historical
payloads cannot be relabelled with a new revision; file size, timestamp or a
previous digest does not prove freshness. A proposed incremental shortcut must
therefore account for replacement and same-size/same-time changes, incomplete
history and concurrent writing, not only append-only examples.

On read loss the producer reports unavailable occupancy and an incomplete zero
lower bound. The host retains its known compaction lower bound while hiding
unproved completeness. A later successful current read can restore telemetry;
the historical claim that the meter is permanently lost is unsupported.
No parser or production-path change has been made for this finding at this
checkpoint.

## 134: native approval arguments

The no-model command was `codex <approval option> completion bash`, which forces
argument validation without starting a model, session, login or UI. For each
value, five spellings were exercised: `-a VALUE`, `--ask-for-approval VALUE`,
`--ask-for-approval=VALUE`, `-aVALUE`, and `-a=VALUE`.

| Value | Native 0.164.0 | Native 0.166.0-alpha.1 |
| --- | --- | --- |
| `on-request` | Exit 0; completion output | Exit 0; completion output |
| `never` | Exit 0; completion output | Exit 0; completion output |
| `untrusted` | Exit 2; invalid value | Exit 2; invalid value |
| `on-failure` | Exit 2; invalid value | Exit 2; invalid value |
| Invalid-policy control | Exit 2 | Exit 2 |

All 50 cases matched this matrix. Both CLIs list only `on-request` and `never`
as accepted values. Their environment also emits a PATH-alias warning from the
read-only host; successful and rejecting cases both reach the parser, and that
warning does not explain the policy results.

The
[current upstream CLI enum](https://github.com/openai/codex/blob/a09812981de703f18f38015cd5d500ab373f616c/codex-rs/utils/cli/src/approval_mode_cli_arg.rs)
and
[current pet enum](https://github.com/imakris/codex/blob/689c8fffadebd1b2a9283cb9c5be988060e96649/codex-rs/utils/cli/src/approval_mode_cli_arg.rs)
have the same two values. Upstream
[0.146.0](https://github.com/openai/codex/blob/rust-v0.146.0/codex-rs/utils/cli/src/approval_mode_cli_arg.rs)
included `untrusted`, but not `on-failure`; sampled
[0.150.1](https://github.com/openai/codex/blob/rust-v0.150.1/codex-rs/utils/cli/src/approval_mode_cli_arg.rs)
already has only the two current values. These are sampled source facts, not a
version allowlist or an exhaustive compatibility matrix. The provider's
lifecycle minimum is 0.146.0, while wake has its own 0.149.0 minimum, so the older
source evidence cannot prove support for every feature named in the original
finding.

`launcher_approval_rules` serves a different capability: Yolofy/Unyolofy editing
of launcher text, including replacement of existing policies while preserving
unrelated configuration. Recognizing historical text for transformation is not
proof of native CLI validity. Narrowing that table merely to match today's CLI
would change a separate operation without a demonstrated reason. No production
or test code was changed for finding 134.

## Checks and limits at this checkpoint

| Check | Inputs | Actual result |
| --- | --- | --- |
| Archive inventory | Supplied P18 handoff | All 11 manifest entries verified. |
| Provider source identity | Current provider snapshot | All 103 Git blobs, root tree and original commit verified before task worktree creation. |
| Native approval parser | 0.164.0 and 0.166.0-alpha.1; 50 no-model probes | Matrix above passed; older accepting executable unavailable. |
| Linux SDK archive | `logonomic-linux-development-sdk-2026.10.23.1` | SHA-256 matches `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2`. |
| SDK preflight | Ubuntu 24.04 x86-64, GCC 13.3 | Passed after restoring the extracted kit's missing CMake/Ninja tool links to its own binaries. |
| Original macOS source branch | Original `native_target` branch, synthetic thin ARM64 header | Compiled under one queued slot; returned unavailable, exit 1 as expected. This is not native macOS execution. |
| Focused C++ provider gates | Standalone baseline and candidate | Pending; no C++ test pass claimed. |

The standalone provider build will use the current owned SDK source outside the
external binary kit, persistent ccache, queued one-slot configuration and at
most four compiler slots. Focused targets are `codex_context_tests`,
`codex_lifecycle_tests`, `codex_preparation_tests` and
`codex_session_wake_tests`, plus directly relevant fixtures.

## Native follow-up

- macOS ARM64: run real npm pet-link discovery, interactive preparation, exact
  resume and delegated preparation using thin and universal Node as applicable;
  verify the selected payload architecture. Linux parser fixtures cannot cover
  native execution.
- Windows: run PE chain regressions and measure the complete emitted hook from
  outer shell through PowerShell, helper, public/private control processes, IPC,
  service and consumed response. Include root/subagent and empty/newly pending
  context; preserve one-shot consumption and current-generation admission.
- Older native CLI: obtain an executable that accepts `untrusted`, rerun the
  no-model probe, and reproduce each actually supported affected preparation
  path before choosing a semantic remedy.

No private hosted workflow or live-user UI has been invoked. Remaining native
evidence limits keep the branch a draft; they do not turn unexecuted checks into
passes.
