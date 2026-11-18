# Native model observations

The optional `model-state@1` capability reports the literal model from a Codex
hook on its root or child execution. Callers opt in to version 1 on the existing
lifecycle carrier. Model reports create no extra lifecycle events. The hooks
identify the model without identifying its serving provider.

| Native source | Execution | When a model is available |
| --- | --- | --- |
| `SessionStart.model` | Root | The hook opens the accepted native transition. Its model has that transition's nonce and sequence zero. |
| `UserPromptSubmit.model` | Root or child | The helper captured the activation and can match the actor's rollout and native turn. |
| `SubagentStart.model` | Child | The start names the child, its own rollout and its turn under the captured root. |
| Structured `exec --json` | Structured session | The event stream supplies no own-model observation. |

Codex builds these hook inputs from the agent's live turn context. The
adapter preserves model aliases and custom model names verbatim. Compaction,
terminal title text, parent models, configuration, and saved usage records do
not supply model evidence. [Native hook runtime](https://github.com/openai/codex/blob/086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a/codex-rs/core/src/hook_runtime.rs)

## Activation and ordering

Each prepared interactive launch gets its own model receipt in the provider
generation's scratch directory. A fresh root's synchronous startup hook can
enroll the launch on its first accepted host epoch. The classifier returns the
activation nonce through the existing helper response. When the terminal title
has already opened that root, the response carries the title's accepted nonce.
The next identifiable prompt can then report the model. A hook that opens the
activation itself can report the start model immediately.

The receipt keeps that first binding for the lifetime of the launch. Helpers
capture it before delivering a later model sample. Root samples must name the
original rollout. Child samples must identify their own rollout under the same
root. Codex's ordered UUIDv7 turn IDs establish source order independently for
each actor. A duplicate or an older turn leaves the sequence unchanged.

The classifier accepts a captured sample only while the host ledger still
contains its root and activation nonce. A delayed delivery from an earlier
visit to the same conversation therefore cannot label a later visit. A resumed
launch, an ambiguous actor, or an unavailable receipt supplies no asynchronous
model evidence. Ordinary lifecycle classification continues in each case.

## Verification

`codex_model_tests` exercises the classifier and the SDK carrier. The packaged
helper and adapter tests in `codex_reply_tests.py` exercise both startup orders,
independent actor sequences, reordered deliveries, and returning to a retired
root. Malformed initial model evidence stays within the helper body limit and
produces a capability-scoped failure.
