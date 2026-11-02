# P18: Codex discovery, hooks and context

## Outcome and continuation

This investigation rechecks findings 43, 44, 65 and 134 against provider source
`e6abe97d55af462b0fcff81e8d7878393c00126b` and Logonomic SDK/host source
`b9e44f6fe131624d48b0338c23295dae67d526bd`. These identify inspected inputs;
owned dependency declarations continue to track `master`.

Two scoped changes are implemented: supported Mach-O recognition for the macOS
npm executable chain, and faster scanning of ordinary ASCII in ignored rollout
strings. Seven focused Linux C++ gates pass. The context change is supported by
interleaved complete-provider measurements, including the actual packaged
adapter, fresh-read controls and the shared usage consumer. It preserves the
full read and hash; the one-second budget can still make larger or slower reads
unavailable.

Finding 44 remains an investigation: the source path and bounded Linux
components were measured, but the actual host path and Windows latency were not.
Finding 134 does not support widening the two current approval values to four;
older `untrusted` compatibility remains conditional.

Work is published on branch `work/linux-review-20261102-p18-provider-codex` in
[draft PR 9](https://github.com/Varinomics/vnm_logonomic_cli_codex/pull/9).
The remaining continuation is native macOS/Windows validation and, if an older
accepting executable is available, its exact approval-preparation reproduction.
No native macOS or Windows execution result is claimed.

## Finding dispositions

| Finding | Current evidence | Disposition | Remaining evidence |
| --- | --- | --- | --- |
| 43 | The original macOS source branch returns unavailable for a supported ARM64 Mach-O header; the candidate recognizes it. Portable thin/universal parser and preparation regressions pass on Linux. | Implemented bounded Mach-O recognition and enabled Apple npm-chain fixtures. | Actual macOS ARM64 npm link discovery/preparation and native payload execution. |
| 44 | Matcher-free ordinary root `PreToolUse` synchronously requests host context. Source describes five executable stages; 264 Linux synthetic-component invocations preserve root/subagent semantics. | Source-confirmed cost per emitted hook; component timings and process trace only. No production optimization selected. | Complete root/subagent, empty/newly pending-context host path and Windows stage timings. |
| 65 | Full read/project/hash is linear. In the paired 128 MiB case, both baseline context paths return unavailable in 3/3 reads; both candidate paths complete in 3/3. | Implemented bounded ignored-ASCII scanning with full read/hash, validation and freshness preserved. Dense short records show a possible small overhead. | Native-platform performance; larger/slower sources can still exhaust the unchanged budget. |
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
installation or a manually selected native Codex executable.

The correction recognizes supported ARM64/x86-64 thin Mach-O and FAT32/FAT64
universal files with bounded reads and offset checks. Universal Node selection
uses the host architecture preference; the bundled payload must support the
architecture selected for Node. Existing PE and ELF paths remain in place.
The existing npm discovery/preparation, delegated-preparation and resume
fixtures are now enabled for Apple builds, and portable parser fixtures cover
supported and malformed headers.

The isolated original Apple source branch returned unavailable, exit 1, for the
synthetic ARM64 header. The corrected branch returned `darwin-arm64`, exit 0,
and `codex_macho_tests` passed on Linux. These establish source/parser behavior;
they do not establish native loading, Rosetta selection, signing or actual
macOS npm execution.

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
such shortcut has been selected.

The bounded Linux component matrix ran three warmups and 30 measured rounds
across eight rows, interleaving the actual helper and the exact POSIX provider
command beneath `/bin/sh -lc`. All 264 invocations preserved the expected
semantics: each root made exactly one synthetic wrapper request, each subagent
made none, and pending root context reached output unchanged.

| Entry | Actor | Synthetic reply | Median ms | p95 ms |
| --- | --- | --- | ---: | ---: |
| Direct helper | Root | Empty | 10.346 | 86.983 |
| Direct helper | Root | Pending | 13.007 | 393.230 |
| Direct helper | Subagent | Empty | 5.097 | 73.500 |
| Direct helper | Subagent | Pending | 5.526 | 95.945 |
| Provider shell command | Root | Empty | 31.528 | 553.976 |
| Provider shell command | Root | Pending | 29.109 | 591.880 |
| Provider shell command | Subagent | Empty | 21.659 | 211.230 |
| Provider shell command | Subagent | Pending | 19.262 | 79.565 |

The fixture substitutes the actual public wrapper, private core, IPC and service
with synthetic executables. Empty and pending replies use different existing
fixture executables, so their timing difference is not host pending-context
cost. The window had no task-owned compiler/test work, but its host CPU steal
counter advanced 7,015 ticks; the high tails remain in the evidence. No shipping
latency or optimization benefit follows from this matrix.

A separate process trace observed six executable occurrences for root and five
for subagent in this Linux fixture: outer shell, login-profile `id` and
`locale-check`, inner shell, helper, and the root-only synthetic wrapper. Those
counts include that login profile and exclude the instrumentation parent. They
are distinct from the five real executable stages established by source, and
do not measure OS process creation in the actual host chain. Traced calls were
excluded from the timing matrix.

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

### Implemented change and retained contracts

The former projector dispatched every ignored string byte through the full
string/UTF-8 state machine. The candidate uses `std::find_if` only when a string
is uncaptured and has no pending escape, Unicode escape digits, UTF-8 sequence
or error. It skips ordinary ASCII until a quote, backslash, control byte or
non-ASCII byte; that boundary returns to the existing validator. It retains
every fresh filesystem read, SHA-256 update, ownership check, tail check,
compaction-count rule and host revision. No cache or metadata-only shortcut was
introduced, and `codex_context.cpp` is byte-identical to the baseline.

The new tests exercise valid escapes/UTF-8 and malformed ignored strings split
at each exceptional byte for both `CONTEXT` and `USAGE` projections. A packaged
test rewrites current tokens from 2400 to 3600 while preserving size and exact
modification time, then requires the fresh value, changed digest, newer host
revision and complete count. Existing tests retain incomplete/forked/paginated
history, malformed input, large ignored payloads, loss/recovery and subagent
isolation coverage.

### Complete-path measurements

The measured baseline is provider `e6abe97d55af462b0fcff81e8d7878393c00126b`.
The measured candidate is initial PR commit
`f3a83e6074e1cb7af111311e78a0c07aae19441a` plus the recorded projector/context-test
diff, SHA-256 `c71f93423f064f44aac356079513f3641b86074d68a26d93c8eab5d3f3f643c4`.
The compiled provider source manifests are
`2adc74015d9c0260e57bf314afda36d98e47ed5d720be6b59d16b817ff750087` and
`d8de5d982afc15d8a51d392d38e0d801846548297a8bfa2e38c12c6a5fc442d8`, respectively.
The shared SDK manifest is
`78c0025ad70b93b394eb39e148ec63ce4f69df31ccd3cddd2e31b21eddf8160d`.
Before/after build-and-test manifests match. Later report edits do not change
the measured implementation.

Both versions used GCC 13.3, C++20, Release `-O3 -DNDEBUG`, the same SDK and the
same compiled driver object within each pair. The platform was Linux 6.18.57
x86-64, glibc 2.39, with the Ubuntu 24.04 development kit. The synthetic files
contain legacy session metadata, ignored function-call output, installed
compaction records and final current usage of 2400/10000 tokens. Their exact
bytes and independently calculated hashes are retained with each run. Nominal
sizes below are MiB targets, not exact file lengths.

Each case ran three alternating pairs, baseline/candidate then
candidate/baseline then baseline/candidate. The driver measures the full
`context_snapshot` call and actual packaged `classify_event` invocation,
response decoding and host context-state application. Adapter startup and
negotiation precede the timed call. Generating/reading the files warms the
filesystem cache. There were no overlapping task-owned compiler/test jobs;
shared-host variability remains. Driver CPU fields exclude adapter child CPU.

These are **wall medians in milliseconds**, including unavailable exits.
Availability is shown separately. A budget exit and a completed read perform
unequal work; their timings are not a completed-work speedup.

| Workload | Snapshot ms, baseline / candidate | Snapshot available, baseline / candidate | Adapter ms, baseline / candidate | Adapter available, baseline / candidate |
| --- | ---: | --- | ---: | --- |
| ASCII 1 MiB, Stop | 8.84 / 6.42 | 3/3 / 3/3 | 10.76 / 7.47 | 3/3 / 3/3 |
| ASCII 8 MiB, Stop | 78.73 / 52.50 | 3/3 / 3/3 | 77.54 / 51.70 | 3/3 / 3/3 |
| ASCII 32 MiB, Stop | 319.99 / 213.29 | 3/3 / 3/3 | 315.26 / 208.15 | 3/3 / 3/3 |
| ASCII 64 MiB, Stop | 647.92 / 425.90 | 3/3 / 3/3 | 823.74 / 433.69 | 2/3 / 3/3 |
| ASCII 128 MiB, Stop | 1000.49 / 822.43 | 0/3 / 3/3 | 1001.09 / 862.97 | 0/3 / 3/3 |
| Escaped output 8 MiB | 80.58 / 58.16 | 3/3 / 3/3 | 80.50 / 64.52 | 3/3 / 3/3 |
| UTF-8 output 8 MiB | 104.86 / 87.24 | 3/3 / 3/3 | 95.38 / 93.84 | 3/3 / 3/3 |
| Short records 8 MiB | 182.16 / 238.85 | 3/3 / 3/3 | 243.90 / 201.68 | 3/3 / 3/3 |
| Compaction history 8 MiB, PostCompact | 86.37 / 56.11 | 3/3 / 3/3 | 127.58 / 60.65 | 3/3 / 3/3 |
| ASCII 32 MiB, resumed SessionStart | 315.34 / 228.00 | 3/3 / 3/3 | 309.83 / 245.90 | 3/3 / 3/3 |

The equal-completion ASCII 8/32 MiB cases show approximately 33–34% lower
medians for both complete context paths. The 32 MiB read-only, hash and
projection stage medians were respectively 5.63/6.18, 179.17/172.69 and
135.53/37.03 ms, supporting the ignored-string hypothesis while the full-path
results establish its practical effect. The 128 MiB result establishes an
availability improvement in this workload, not a universal size threshold.
Initial unpaired baseline measurements also observed unavailable results at
64 MiB and, during a later variable interval, 32 MiB resume reads.

The short-record row prompted one bounded five-pair follow-up. Snapshot
medians were 186.86/192.40 ms (+3.0% candidate), and adapter medians were
165.80/173.36 ms (+4.6% candidate); all reads completed. Snapshot ranges were
170.53–225.44/168.60–218.61 ms, and adapter ranges were
155.60–191.86/157.83–186.91 ms. Pair directions were mixed. The initial large
snapshot shift did not recur, but overlapping ranges do not prove equivalence:
a small per-record overhead remains a possible tradeoff. No dense-short-record
speedup is claimed.

### Shared usage consumer and freshness controls

The full cold application-cache usage scan was also compared in three
alternating pairs per case. This includes rollout reduction, sorting, merge
and sealed-cache publication, with generated files in a warm filesystem cache.

| Usage workload | Baseline median ms | Candidate median ms |
| --- | ---: | ---: |
| ASCII 8 MiB | 49.03 | 19.61 |
| ASCII 32 MiB | 228.33 | 67.66 |
| UTF-8 8 MiB | 121.62 | 67.59 |

All 18 scans returned exact 1000/2000 per-turn deltas, total 3000 / input 1800 /
output 1200 / cached 600, known category coverage and a valid sealed cache.
The first raw record-equality check correctly exposed a harness assumption:
each fresh scan receives a different `usage-temporal.parent.snapshot_id`.
The original mismatch is retained; the same six samples were rechecked after
normalizing only that identity, with every accounting and temporal-evidence
field retained. No new timings replaced those samples.

The third projector consumer, private `read_native_session`, reads until the
session metadata is found. The existing
`codex_usage_tests::scanner_reads_archived_rollouts` unindexed-copy conflict
exercises its actual fallback: an accepted cache lacks the native-session
index, an active copy changes, and the archived peer must be identified before
duplicate accounting is rejected. That path is correctness-covered, with no
separate identity-reader speed claim.

Both packaged versions passed the five-state freshness sequence: original
usage 2400; same-size/same-mtime rewrite to 3600 with a new digest; resume with
the same current digest; appended compaction clearing usage and increasing the
complete count; and new usage restoring 2400 with the increased count.
Linux `IN_OPEN`-coordinated append probes passed 6/6 trials for each version
(three direct snapshots and three adapter calls). Every changed read returned
unavailable with zero incomplete count, then the next stable read recovered
2400 and complete count 1 with the independently calculated current digest.

The first baseline append attempt is retained as inconclusive: its static
8 MiB control returned unavailable before any mutation, and the original
harness raised before saving its elapsed sample. Its diagnostic assumed a
budget overrun; the cause of that particular control is unknown. The bounded
rerun recorded every control and passed. It does not retroactively establish
the cause of the first unavailable result.

### Reproduction recipe

The compact evidence bundle retains `profile_context.cpp`, `profile_usage.cpp`,
`run_profile.py`, `compare_profiles.py`, `append_probe.py`, exact command arrays,
source/binary hashes and raw results. No benchmark is added to normal CTest.
The original comparison script is retained separately with its original hash.
Build both provider snapshots with the source selections above; compile each
driver once with `-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Werror`, then link that
same object against each provider archive and matching SDK archive. The usage
driver also uses the same SQLite link input as the selected usage test.
Configuration/serial compilation/linking used `queued-build --slots 1`;
provider builds used four declared slots and `--parallel 4`, with persistent
ccache. Full build/link arrays are in `gates/linux/commands.json`.

From the evidence layout, with `P18_ROOT` set to its containing workspace:

```sh
P18_EVIDENCE="$P18_ROOT/evidence/context-profile"
python "$P18_EVIDENCE/compare_profiles.py" \
  --baseline "$P18_EVIDENCE/profile-baseline" \
  --baseline-package "$P18_ROOT/build-baseline/package/logonomic.cli.codex" \
  --candidate "$P18_EVIDENCE/profile-candidate" \
  --candidate-package "$P18_ROOT/build-candidate/package/logonomic.cli.codex" \
  --fixtures "$P18_EVIDENCE/fixtures" --output "$P18_EVIDENCE/paired-context.json" --rounds 3
python "$P18_EVIDENCE/compare_profiles.py" --usage \
  --baseline "$P18_EVIDENCE/profile-usage-baseline" \
  --candidate "$P18_EVIDENCE/profile-usage-candidate" \
  --fixtures "$P18_EVIDENCE/fixtures" --output "$P18_EVIDENCE/paired-usage.json" --rounds 3
```

The context matrix is bounded to ASCII 1/8/32/64/128 MiB; escaped, UTF-8 and
short output at 8 MiB; 8 MiB containing 136 installed compactions; and a resumed
32 MiB session. The 32 MiB ASCII fixture is exactly 33,536,812 bytes, SHA-256
`359694b3f1e6c3bc2cbb50350e81635aa5b6ef7b3b278d3d304673ca388f3cc7`.
Each available update must have current usage 2400/10000, the exact complete
compaction count and the full independently calculated digest. Unavailable
static reads must have zero incomplete count and no usage. For the short-record
follow-up, add `--case short:8:Stop --rounds 5` to the context command. The usage
matrix is ASCII 8/32 MiB and UTF-8 8 MiB. `append_probe.py` takes
`--driver`, `--package`, `--fixtures` and `--output`, and fixes its workload to
six 8 MiB trials per version. The context driver's `freshness` mode implements
the five-state sequence above.

The measured scope excludes native hook dispatch, outer/provider shells,
public/private control wrappers, production host IPC/service, UI and Windows.
Full read/hash remains linear; the result supports this provider-local scan
change, not a claim that every large rollout now fits the budget.

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

## Checks and observed limitations

| Check | Inputs | Actual result |
| --- | --- | --- |
| Archive inventory | Supplied P18 handoff | All 11 manifest entries verified. |
| Provider source identity | Current provider snapshot | All 103 Git blobs, root tree and original commit verified before task worktree creation. |
| Native approval parser | 0.164.0 and 0.166.0-alpha.1; 50 no-model probes | Matrix above passed; older accepting executable unavailable. |
| Linux SDK archive | `logonomic-linux-development-sdk-2026.10.23.1` | SHA-256 matches `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2`. |
| SDK preflight | Ubuntu 24.04 x86-64, GCC 13.3 | Passed after restoring the extracted kit's missing CMake/Ninja tool links to its own binaries. |
| Original macOS source branch | Original `native_target` branch, synthetic thin ARM64 header | Compiled under one queued slot; returned unavailable, exit 1 as expected. This is not native macOS execution. |
| Corrected macOS source branch | Synthetic ARM64 header through isolated Apple branch | Compiled and returned `darwin-arm64`, exit 0. Native macOS execution remains pending. |
| Focused baseline provider gates | Context, lifecycle, preparation, session wake | All four passed; preparation alone required the isolated temporary-directory correction below. |
| Focused candidate provider gates | Mach-O, preparation, session wake, JSONL, lifecycle, usage, context | All 7 passed in 32.47 seconds; before/after provider and SDK inventories identical. |
| Context comparisons and controls | Identical driver objects, 3 alternating pairs per case; bounded short-record follow-up | Full path and availability results above; fresh reads and final concurrent-append controls pass. |

The standalone build consumed the owned SDK source outside the external binary
kit. The selected targets were `codex_context_tests`, `codex_lifecycle_tests`,
`codex_preparation_tests`, `codex_session_wake_tests`, `codex_macho_tests`,
`codex_jsonl_tests` and `codex_usage_tests`, with package/helper dependencies.

Observed setup and probe failures remain in the evidence. The initial SDK
preflight lacked the kit's CMake/Ninja links; restoring links to its own
binaries corrected it. Initial standalone configuration lacked SQLite's
development header/link name; a task-local official 3.45.1 header matching the
kit's 3.45.1 runtime corrected that input, without changing dependency policy.
The first baseline preparation run encountered a Git ancestor of the default
temporary directory; rerunning only that target in an owned `/var/tmp`
directory passed, and the candidate used that isolated location. A scratch
profiling-driver compile initially rejected an ambiguous stream-offset
addition; it was corrected before any metrics. The first process trace was
denied `PTRACE_TRACEME` inside the ordinary sandbox; one approved isolated trace
then succeeded, outside the timing matrix. The inconclusive append control
and usage comparison's fresh-identity assumption are described above. These
are distinct from the reproduced context-budget failures.

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
