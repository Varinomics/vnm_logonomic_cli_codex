# Native model observations

The optional `model-state@1` capability carries literal native model evidence
beside the existing lifecycle classification. A caller must request version 1;
legacy callers receive no model extension. A malformed optional model produces
a capability failure without discarding the native lifecycle event.

## Supported source

| Native source | Actor | Behavior |
| --- | --- | --- |
| Root `SessionStart.model` accepted as a new transition | Root session | Confirmed live model, with that same transition nonce and sequence zero |
| `SessionStart` already selected through terminal identity | Root session | No model update: this delivery cannot claim the earlier selection's source activation |
| Later model-request hooks or saved transcript reads | Root or child | Not emitted by this implementation |
| `SubagentStart` / `SubagentStop` | Child | Lifecycle remains available; no model is borrowed from the parent |
| Structured `exec --json` inspection | Structured session | No model observation; the consumed native event contract does not identify an own serving model |

Codex constructs the start hook from the agent's live turn context. The
[native hook runtime](https://github.com/openai/codex/blob/086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a/codex-rs/core/src/hook_runtime.rs)
is the source contract. The implementation preserves the literal model; it
does not normalize aliases or infer a serving provider from a model prefix or
from the Codex CLI brand. Native custom providers are possible, and this hook
does not identify one.

Compaction starts, internal events, and child starts cannot relabel the root.
The model extension adds no lifecycle observation, attention event, or log row.

## Freshness boundary

The shared SDK requires a source-captured activation nonce and source order for
an asynchronous observation. The current Codex command helper is short lived
and has no such binding for later model hooks. The startup observer identifies
conversations, and the wake binding identifies launch/profile/database scope;
neither supplies the missing model activation capture.

Reading the host's current nonce when a delayed event arrives would attach old
evidence to a later activation, especially after A → B → A. This implementation
therefore leaves those cells unconfirmed. A future extension needs a native
source route that captures the activation before asynchronous model work; a
parent model, launcher selection, usage aggregate, or arrival-time ledger
lookup cannot fill that gap.

## Validation

`codex_model_tests` exercises the actual classifier and SDK carrier, including
opt-in behavior, literal model preservation, omitted provider, malformed model
isolation, and compaction/child exclusion. `codex_reply_tests.py` exercises the
compiled helper and adapter transport with an account-free fake host wrapper.
These component tests are distinct from a native Codex session or a rendered
Logonomic timeline.

The account-free `codex_native_wake_tests.py` harness uses a disposable native
profile and a loopback Responses endpoint, but qualifies wake behavior rather
than model evidence. Native source, helper/adapter components, package staging,
and the complete host UI must be reported as separate qualification scopes.
