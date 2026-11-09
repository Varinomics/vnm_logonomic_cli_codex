# Building and qualification

`CMakeLists.txt` fetches the SDK from the owned Logonomic `master` branch. Set
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to an explicit local
`sdk/logonomic_cli_addon_sdk` directory for coordinated development. The
`codex_package` target produces an indexed package under
`package/logonomic.cli.codex` in a single-configuration build directory, or
`package/<config>/logonomic.cli.codex` with a multi-configuration generator.
Windows and Linux x86-64 and macOS ARM64
artifacts have separate manifests. Follow the workstation build queue and
toolchain policy for every configure and build command.

The package recipe proves the Windows/Linux x86-64 or macOS ARM64 compiler target
during configuration. `cmake --install` exports a fresh artifact beneath its install-time
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

`codex_terminal_identity_tests` is a standalone provider contract target with
the real provider and SQLite dependencies. Its idle `/clear` regression does
not require a packaged artifact or a model turn. Passing it does not establish
package assembly, native TUI delivery or the host's card/Note behavior; those
retain their existing independent gates.

## Manual CI

The repository's CI workflow builds the standalone package and runs its existing
provider contract tests on Windows/Linux x86-64 and macOS ARM64. The SDK's
compiler-target proof excludes macOS Intel and FreeBSD package builds.

Run the workflow manually in GitHub Actions. Its shared build workflow lives
with the Logonomic CLI add-on SDK and consumes SDK source from `master` using
`VARINOMICS_REPO_READ_TOKEN`. Provider credentials are not required. Retained
artifacts are development packages and validation evidence; official bundle
assembly and publication remain separate operations.
