# Building and qualification

`CMakeLists.txt` fetches the SDK from `vnm_logonomic_cli_addon` on `master`. Set
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to an explicit local
`sdk/logonomic_cli_addon_sdk` directory for coordinated development. The
`codex_package` target produces an indexed package under
`package/logonomic.cli.codex` in a single-configuration build directory, or
`package/<config>/logonomic.cli.codex` with a multi-configuration generator.

Use `cmake --install` to export the indexed package to a fresh directory. See [SDK package export](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#exporting-a-package).

Package validation, conformance, preparation, startup-observer, lifecycle,
implementation-protocol, account/probe and usage contract fixtures are registered
with CTest. Run them from the configured build directory after building the
corresponding targets.

To check standalone execution against an installed native Codex, run
`python tests/codex_native_structured_tests.py --package <indexed-package> --codex <native-executable> --scratch-root <test-directory>`.
Choose a scratch directory readable by the native command sandbox.
The test uses disposable profiles and a loopback Responses server. Each run
checks a shell read, verifies whether the native sandbox allowed the repository
write, and inspects the final Codex output through the packaged adapter.

`codex_terminal_identity_tests` is a standalone provider contract target with
the real provider and SQLite dependencies. Its idle `/clear` regression does
not require a packaged artifact or a model turn. Passing it does not establish
package assembly, native TUI delivery or the host's card/Note behavior; those
retain their existing independent gates.

## Manual CI

The repository's CI workflow builds the standalone package and runs its existing
provider contract tests on Windows/Linux x86-64 and macOS ARM64. The SDK's
compiler-target proof excludes macOS Intel and FreeBSD package builds.

Run the workflow manually in GitHub Actions. The maintainer workflow in Logonomic consumes SDK
source from `vnm_logonomic_cli_addon` using its configured repository read token. Provider credentials are not required. Retained
artifacts are development packages and validation evidence; official bundle
assembly and publication remain separate operations.
