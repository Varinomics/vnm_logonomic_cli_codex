# Usage evidence and caches

The package scans per-turn native usage under the selected `CODEX_HOME`.
Each completed turn uses its matching native `turn_context.cwd`; an ID-less
context belongs to the active turn. Without matching context it falls back to
the session directory, never the preceding turn's directory. An explicitly
unknown cwd remains unattributed.
Both `sessions` and `archived_sessions` contribute native history, including
on a fresh import or reset. Archiving preserves stable native turn identities;
overlapping copies contribute each turn once and retained history survives moves.
Accounting, cache acceptance and progress follow the independently versioned
[usage-scan contracts](https://github.com/Varinomics/Logonomic/tree/master/sdk/logonomic_cli_addon_sdk/docs).

## Cache and reconciliation

Host-selected usage caches use `codex-rollout-cache@3`, whose records retain
usage-scan@3 native counter provenance. A candidate keeps one metadata line per
rollout in `rollouts` beside the sorted `records`, so it holds the same four
files however many rollouts a home has. An incremental scan parses rollouts whose
size or modification time changed, or whose optional temporal evidence has not
been examined, and streams the selected cache's records for the others; repair
and reset parse every rollout. When a source is parsed, unchanged current copies
of that native session are also compared. Native identities come from rollout
metadata or saved observations; a copy absent from both needs only its native
session metadata read to identify it. Unrelated unchanged sources keep
fingerprint reuse. Equal
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
New turns are attributed as their checkout stands at the scan. A contradicting
observation is presented for the host to accept or reject. If a proved numeric
increase also changes the observed directory, it keeps prior allocations and
records only the growth as unknown until the new total is accepted. A selected
`codex-rollout-cache@1` or `@2` cache, which kept two files per rollout, is
validated and converted into a fresh candidate without modifying the selected
input or inventing provenance absent from its records. Native imports preserve
`cumulative_turn_delta` together with the validated rollout/turn record identity;
unknown present source semantics fail the import. Aggregate evidence remains
partial. The usage-scan@1/2 mutable cache namespaces and record grammar are
unchanged. Legacy numeric reductions retain the prior repository, consuming
deferred unknown tokens before reducing the sole known allocation. A mixed
external allocation can consume unknown tokens, but a reduction that would
require choosing among known repositories is refused.

The optional `usage-repository-attribution@1` capability provides `scan` and
`incremental` operations with the existing usage request and result grammars.
Only these operations can correct an accepted turn's repository. A fresh parse
must confirm its accepted total and witness a different matching turn cwd that
resolves to a repository. The correction preserves accepted accounting, dates,
categories and native provenance, and tags the repository-only replacement for
host validation. A validated host-selected native seed supplies that accepted
observation during first import. Missing source, session fallback, unresolved
cwd and the disappearance of a Git marker cannot retract accepted attribution.
Existing unchanged caches gain no automatic reread; explicit repair or a
naturally changed source supplies the evidence. Continuations remain bound to
the capability and operation that sealed their candidate.

The optional `usage-incremental@1` operation binds each update to the exact
host-accepted cache. When rollout metadata, parser evidence and historical
coverage are unchanged, it retains that generation without reading, rewriting
or exporting its accounting records. It verifies the selected manifest and the
metadata files it consumes. A metadata change still publishes a candidate even
when it does not change the token total. Changed updates export only changed
absolute observations; omissions retain accepted accounting. Their
incremental coverage retains previously accepted gaps; repair or reset verifies
current sources in full before declaring complete coverage. The
`codex-rollout-cache@4` candidates partition records by stable session identity,
reconcile the affected pieces, and reuse immutable unaffected pieces in a
self-contained generation. Reused files retain their sealed identity; changed
files receive fresh digests. Adoption partitions saved cache records without
forcing a native transcript recount. A `usage-scan@3` request still produces a
complete snapshot, including when its selected input uses record pieces.
Unchanged sources with missing optional temporal evidence retain that honest
absence during incremental updates; changed sources and explicit full scans
can supply the evidence.

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
