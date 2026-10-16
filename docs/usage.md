# Usage evidence and caches

The package scans per-turn native usage under the selected `CODEX_HOME`.
Accounting, cache acceptance and progress follow the independently versioned
[usage-scan contracts](https://github.com/Varinomics/Logonomic/tree/master/sdk/logonomic_cli_addon_sdk/docs).

## Cache and reconciliation

Host-selected usage caches use `codex-rollout-cache@3`, whose records retain
usage-scan@3 native counter provenance. A candidate keeps one metadata line per
rollout in `rollouts` beside the sorted `records`, so it holds the same four
files however many rollouts a home has. An incremental scan parses rollouts whose
size or modification time changed, or whose optional temporal evidence has not
been examined, and streams the selected cache's records for the others; repair
and reset parse every rollout. Equal
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

## Publication and progress

Usage scans publish one durable generation containing per-file evidence, sorted
pages, and native import receipts. Only files bound by that generation's digest
inventory can seed a later scan. Failed candidates cannot replace accepted
history, and a validated empty cache remains evidence. Source diagnostics are
included only when the public usage contract can represent their exact meaning.

A scan that builds a snapshot reports progress: a frame names each stage as it
begins (preparing the cache, importing earlier usage history, reading Codex
sessions, sorting, merging, saving) and, while the stage works, counts the
items it has handled. Working updates are coalesced to one frame per second,
or a quarter of the host's tolerated silence when shorter. Progress continues
for as long as the scan works. A page read from a finished snapshot, and a
scan waiting for another scan of this package, report nothing.

## Native session identity

The optional [usage-session@1](https://github.com/Varinomics/Logonomic/blob/master/sdk/logonomic_cli_addon_sdk/docs/usage_session_1.md)
operation associates scanned record identities with the native lifecycle session
and states that Codex records exclude the current turn still in flight. It does not rewrite
ordinary scan responses, stable record/session keys or accepted cache bytes.
Hosts that do not negotiate it retain the ordinary usage-scan contract.
