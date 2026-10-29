# Logonomic Codex add-on

[![CI](https://github.com/Varinomics/vnm_logonomic_cli_codex/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_logonomic_cli_codex/actions/workflows/ci.yml)

Standalone C++20 provider package `logonomic.cli.codex` for Logonomic. It uses
the public CLI add-on SDK without Qt or desktop-private headers. Codex itself
is installed separately.

The package recognizes Codex launchers and selected `CODEX_HOME` accounts,
prepares interactive launches and exact resume, supplies authenticated hooks,
and supports delegated implementation, authentication, quota and per-turn
usage. Structured read-only review is not advertised.

## Use with Logonomic

Install the indexed package through Logonomic's generic add-on ingestion. The
selected launcher and native Codex configuration retain their sandbox,
approval, model and account choices; this package does not grant general file
or command permissions. Native executable and stock npm launcher chains are
verified before interactive or delegated preparation.

Lifecycle support requires Codex 0.146.0 or newer; delegated implementation
requires 0.150.1 or newer. These minimums are capability requirements, not an
upper compatibility limit. Required native options and protocol records are
still validated.

Optional live-session wake requires a prepared, supported original session.
Queue acceptance is a native receipt, not mailbox consumption or agent
acknowledgment. Exact resume has its own private startup observer; optional
wake unavailability does not prevent ordinary launching.

## Build and reference

The SDK dependency tracks Logonomic `master`.
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` can select an explicit local SDK for coordinated
development. Build `codex_package` in a separate tree. Windows/Linux x86-64 and
macOS ARM64 have separate package manifests; follow the build queue and
platform toolchain policy.

- [Building and qualification](docs/building.md): package outputs, runtime
  export rules and focused CTest registrations.
- [Provider integration](docs/integration.md): hooks, wake/resume, initial
  prompts, account observations and delegated permission posture.
- [Usage evidence and caches](docs/usage.md): incremental scanning,
  reconciliation, provenance, optional session identity and progress.
- [Public Logonomic CLI add-on SDK](https://github.com/Varinomics/Logonomic/tree/master/sdk/logonomic_cli_addon_sdk).

## Attribution

The OpenAI icons are covered by the bundled MIT attribution in
`package/notices/lobe_icons_mit.txt`; per-resource source and normalization details
are in `package/provenance/lobe_icons.json`.
