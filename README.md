# Logonomic Codex add-on

Standalone C++20 provider package for Logonomic. The package links only the public
Logonomic CLI add-on SDK; it does not load Qt or import desktop-private headers.
Package identity is `logonomic.cli.codex`.

Host-selected usage caches use `codex-rollout-cache@3`, whose records retain
usage-scan@3 native counter provenance. A candidate keeps one metadata line per
rollout in `rollouts` beside the sorted `records`, so it holds the same four
files however many rollouts a home has. An incremental scan parses only the
rollouts whose size or modification time changed and streams the selected
cache's records for the others; repair and reset parse every rollout. Equal
size and modification time are taken to mean unchanged content, the check the
pre-cutover scanner used, because rollouts are append-only and repair rereads
every rollout. Turns reconcile by their stable record IDs: a rollout parsed
again keeps its accepted turns that it no longer holds, a moved rollout keeps
one observation of each turn under its accepted identity, and differing
observations of one turn within a scan fail it. A turn whose cumulative counter
had a baseline before it, which observed samples, and whose total took in no
counter reset or tokens pending from an unfinished turn, carries a
usage-cross-source-event@1 proof in namespace `openai.codex.completed-turn`
with event ID `<rollout>:<turn>`. A turn observed again keeps its accepted
record whole unless the observation contradicts it: a proved observation with
another total, or an unproved one with a lower total, since an unproved total
can include tokens of events lost before the turn. So a checkout removed or
moved since acceptance does not change the accepted attribution, a turn
imported from the native cache keeps its native provenance, and a confirmed
record without a proof gains one when its rollout is parsed again, for example
by a repair scan.
New turns, and turns an observation contradicts, are attributed as their
checkout stands at the scan; a contradicting observation is presented for the
host to accept or reject. A selected
`codex-rollout-cache@1` or `@2` cache, which kept two files per rollout, is
validated and converted into a fresh candidate without modifying the selected
input or inventing provenance absent from its records. Native imports preserve
`cumulative_turn_delta` together with the validated rollout/turn record identity;
unknown present source semantics fail the import. Aggregate evidence remains
partial. The usage-scan@1/2 mutable cache namespaces and record grammar are
unchanged.

The provider recognizes installed Codex launchers, preserves the selected
`CODEX_HOME` account, injects session hooks through command-line configuration,
and prepares delegated implementation sessions with required task-control MCP
servers. Native executable and stock npm launcher chains are verified before
interactive or delegated preparation. Package runners can be recognized without
being admitted as interactive launch chains.

Codex hooks carry session, turn, attention and subagent facts through the SDK's
authenticated helper lane. Title and coordination instructions are requested
from the host's one-shot admission service and returned using Codex's
`hookSpecificOutput.additionalContext` format. The helper does not send prompt
content through the lifecycle ledger or fabricate instructions after admission
fails.

Exact interactive resume preserves the canonical provider session UUID and
requires a private startup observer. Before opening the requested conversation,
the package inspects effective configuration, refuses an existing telemetry log
exporter, and verifies startup telemetry using an isolated saved conversation.
The probe does not request a model turn. Authentication headers remain in the
launched process environment, while vendor OTLP interpretation belongs to this
package.

On Linux, probes inherit the generation custodian's process group and session.
Probe cleanup confirms its direct child within a bounded settlement interval;
descendants remain under generation custody until retirement. A successful probe
establishes the startup protocol evidence, not that its entire descendant tree
has exited.

Authentication, quota, account discovery and per-turn usage scanning preserve the
native integration's semantics. Historical account, resume and usage imports
match the exact `codex` provider attribution. Structured read-only review is not
advertised. Lifecycle support requires Codex 0.146.0 or newer; delegated
implementation requires 0.150.1 or newer. Version checks have no upper bound,
and accepted protocol records still undergo structural and identity validation.

Usage scans publish one durable generation containing per-file evidence, sorted
pages, and native import receipts. Only files bound by that generation's digest
inventory can seed a later scan. Failed candidates cannot replace accepted
history, and a validated empty cache remains evidence. Source diagnostics are
included only when the public usage contract can represent their exact meaning.

`CMakeLists.txt` fetches the SDK from the owned Logonomic `master` branch. Set
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to an explicit local
`sdk/logonomic_cli_addon_sdk` directory for coordinated development. The
`codex_package` target produces an indexed package under
`package/logonomic.cli.codex` in a single-configuration build directory, or
`package/<config>/logonomic.cli.codex` with a multi-configuration generator.
Windows and Linux x86-64
artifacts have separate manifests. Follow the workstation build queue and
toolchain policy for every configure and build command.

The package recipe proves the Windows/Linux x86-64 compiler target during
configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

Package validation, conformance, preparation, startup-observer, lifecycle,
implementation-protocol, account/probe and usage contract fixtures are registered
with CTest. Run them from the configured build directory after building the
corresponding targets.

The OpenAI icons are covered by the bundled MIT attribution in
`package/notices/lobe_icons_mit.txt`; per-resource source and normalization details
are in `package/provenance/lobe_icons.json`.
