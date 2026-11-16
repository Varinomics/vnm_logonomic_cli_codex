#include "codex_launcher.h"
#include "codex_startup_probe.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// The private startup probe proves that an installed Codex reports its
// startup to the observer. Its result is remembered per effective
// configuration and executable chain, and the chain is identified by each
// file's path, size and modification time. These checks hold the
// configuration fixed within a check and vary one component of the chain.

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Environment = std::vector<std::pair<std::string, std::string>>;

#ifdef _WIN32
constexpr const char* k_image_name = "codex.exe";
#else
constexpr const char* k_image_name = "codex";
#endif
constexpr const char* k_unidentified_chain = "The Codex executable chain could not be identified.";

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return false;
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override { m_finished = true; }
    void fail(addon::Result_status, std::string, std::string) override { m_finished = true; }
    bool finished() const override { return m_finished; }

private:
    addon::Invoke_request m_request;
    bool                  m_finished = false;
};

bool check(bool condition, const std::string& failure)
{
    if (!condition) {
        std::cerr << "FAIL: " << failure << '\n';
    }
    return condition;
}

std::string utf8(const fs::path& path) { return addon::path_to_utf8(path); }

// A Codex installation as the launcher verifies it: the native image that
// runs, and a package manifest that only identifies the release, as the
// manifests of an npm installation do.
struct Chain
{
    fs::path image;
    fs::path manifest;
};

Chain install_chain(const fs::path& fake_cli, const fs::path& directory)
{
    fs::create_directories(directory);
    Chain chain{directory / k_image_name, directory / "package.json"};
    fs::copy_file(fake_cli, chain.image, fs::copy_options::overwrite_existing);
    std::ofstream manifest(chain.manifest, std::ios::binary);
    manifest << R"({"name":"@openai/codex","version":"0.150.1"})";
    manifest.close();
    if (!manifest) {
        throw std::runtime_error("The fixture package manifest could not be written.");
    }
    return chain;
}

codex::Verified_launch verified(const Chain& chain)
{
    codex::Verified_launch launch;
    launch.argv        = {utf8(chain.image)};
    launch.version     = "0.150.1";
    launch.files       = {utf8(chain.image), utf8(chain.manifest)};
    launch.prefix_size = 1;
    return launch;
}

// One check's effective configuration and probe record. The model differs
// between checks, so no check can be answered from another check's proof.
struct Check_scope
{
    fs::path    directory;
    std::string model;
};

struct Startup_proof
{
    bool        supported = false;
    std::string error;
    int         probes    = 0;     // private probes this proof started
};

int started_probes(const fs::path& log)
{
    std::ifstream input(log, std::ios::binary);
    int count = 0;
    for (std::string line; std::getline(input, line);) {
        if (line == "thread/start") {
            ++count;
        }
    }
    return count;
}

Startup_proof prove(
    const Check_scope&             scope,
    const codex::Verified_launch&  launch,
    const std::optional<fs::path>& replaced_during_probe = std::nullopt)
{
    const fs::path log = scope.directory / "probe.log";
    Environment environment{
        {"PATH",                        utf8(addon::path_from_utf8(launch.argv.front()).parent_path())},
        {"TEMP",                        utf8(scope.directory)},
        {"TMP",                         utf8(scope.directory)},
        {"CODEX_FIXTURE_CONFIGURATION", R"({"model":")" + scope.model + R"("})"},
        {"CODEX_FIXTURE_PROBE_LOG",     utf8(log)},
    };
#ifdef _WIN32
    if (const char* system_root = std::getenv("SystemRoot")) {
        environment.emplace_back("SystemRoot", system_root);
    }
#endif
    if (replaced_during_probe) {
        environment.emplace_back("CODEX_FIXTURE_PROBE_TOUCH", utf8(*replaced_during_probe));
    }
    const int before = started_probes(log);
    Context context;
    Startup_proof proof;
    proof.supported = codex::startup_supported(launch, {}, environment, utf8(scope.directory), context, &proof.error);
    proof.probes    = started_probes(log) - before;
    return proof;
}

std::string described(const Startup_proof& proof)
{
    return " (supported=" + std::string(proof.supported ? "true" : "false") +
        ", probes=" + std::to_string(proof.probes) +
        (proof.error.empty() ? std::string{} : ", error=" + proof.error) + ")";
}

bool unchanged_chain_reuses_the_proof(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "unchanged", "gpt-5-codex"};
    const auto launch = verified(install_chain(fake_cli, scope.directory / "installation"));
    const auto first  = prove(scope, launch);
    const auto second = prove(scope, launch);
    bool ok = true;
    ok &= check(first.supported && first.probes == 1,
        "unchanged chain: the first resume must run the private startup probe once" + described(first));
    ok &= check(second.supported && second.probes == 0,
        "unchanged chain: the next resume must reuse the remembered proof without probing" + described(second));
    return ok;
}

bool changed_modification_time_is_proved_again(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "modification-time", "gpt-5.1-codex"};
    const auto chain  = install_chain(fake_cli, scope.directory / "installation");
    const auto launch = verified(chain);
    const auto first  = prove(scope, launch);
    // Same bytes and size; only the modification time moves.
    fs::last_write_time(chain.manifest, fs::last_write_time(chain.manifest) + std::chrono::hours(1));
    const auto second = prove(scope, launch);
    bool ok = true;
    ok &= check(first.supported && first.probes == 1,
        "modification time: the first resume must run the private startup probe" + described(first));
    ok &= check(second.supported && second.probes == 1,
        "modification time: a chain file with a new modification time must be proved again" + described(second));
    return ok;
}

bool changed_size_is_proved_again(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "size", "gpt-5.1-codex-mini"};
    const auto chain  = install_chain(fake_cli, scope.directory / "installation");
    const auto launch = verified(chain);
    const auto first  = prove(scope, launch);
    // A larger manifest with its previous modification time restored.
    const auto modified = fs::last_write_time(chain.manifest);
    std::ofstream(chain.manifest, std::ios::binary | std::ios::app) << '\n';
    fs::last_write_time(chain.manifest, modified);
    const auto second = prove(scope, launch);
    bool ok = true;
    ok &= check(first.supported && first.probes == 1,
        "size: the first resume must run the private startup probe" + described(first));
    ok &= check(second.supported && second.probes == 1,
        "size: a chain file with a new size must be proved again" + described(second));
    return ok;
}

bool relocated_chain_is_proved_again(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "path", "gpt-5.1-codex-max"};
    const auto original  = install_chain(fake_cli, scope.directory / "installation-a");
    const auto relocated = install_chain(fake_cli, scope.directory / "installation-b");
    // Identical bytes, sizes and modification times; only the paths differ.
    fs::last_write_time(relocated.image,    fs::last_write_time(original.image));
    fs::last_write_time(relocated.manifest, fs::last_write_time(original.manifest));
    const auto first  = prove(scope, verified(original));
    const auto second = prove(scope, verified(relocated));
    bool ok = true;
    ok &= check(first.supported && first.probes == 1,
        "path: the first resume must run the private startup probe" + described(first));
    ok &= check(second.supported && second.probes == 1,
        "path: an identical chain at another path must be proved again" + described(second));
    return ok;
}

std::optional<codex::Verified_launch> launch_through(const fs::path& bin, const fs::path& working_directory)
{
    addon::Launcher_request launcher;
    launcher.script_body = "codex";
    launcher.runner_key  = addon::k_runner_direct;
    auto environment = addon::Json_value::object();
    environment.set("PATH", addon::Json_value(utf8(bin)));
#ifdef _WIN32
    if (const char* system_root = std::getenv("SystemRoot")) {
        environment.set("SystemRoot", addon::Json_value(system_root));
    }
#endif
    Context context;
    return codex::verified_launch(launcher, {"codex"}, environment, utf8(working_directory), false, context);
}

// A version manager selects a release by retargeting a link. The launcher
// canonicalizes the selected program, so the new target is a new path.
bool retargeted_link_is_proved_again(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "link", "gpt-5.2-codex"};
    const auto release_a = install_chain(fake_cli, scope.directory / "release-a");
    const auto release_b = install_chain(fake_cli, scope.directory / "release-b");
    fs::last_write_time(release_b.image, fs::last_write_time(release_a.image));
    const fs::path bin  = scope.directory / "bin";
    const fs::path link = bin / k_image_name;
    fs::create_directories(bin);
    std::error_code link_error;
    fs::create_symlink(release_a.image, link, link_error);
    if (link_error) {
        std::cerr << "NOTE: symbolic links cannot be created here (" << link_error.message()
            << "); a retargeted link is not covered, only a changed canonical path\n";
        return true;
    }
    const auto first_launch = launch_through(bin, scope.directory);
    const auto first = first_launch ? prove(scope, *first_launch) : Startup_proof{};
    fs::remove(link);
    fs::create_symlink(release_b.image, link);
    const auto second_launch = launch_through(bin, scope.directory);
    const auto second = second_launch ? prove(scope, *second_launch) : Startup_proof{};
    bool ok = true;
    ok &= check(first_launch && first_launch->files == std::vector<std::string>{utf8(fs::canonical(release_a.image))},
        "link: the launcher must record the link's target, not the link");
    ok &= check(second_launch && second_launch->files == std::vector<std::string>{utf8(fs::canonical(release_b.image))},
        "link: the launcher must record the link's new target after it is retargeted");
    ok &= check(first.supported && first.probes == 1,
        "link: the first resume must run the private startup probe" + described(first));
    ok &= check(second.supported && second.probes == 1,
        "link: a retargeted link must prove the newly selected release" + described(second));
    return ok;
}

bool missing_evidence_is_refused(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "missing", "gpt-5"};
    const auto chain = install_chain(fake_cli, scope.directory / "installation");
    fs::remove(chain.manifest);
    const auto proof = prove(scope, verified(chain));
    bool ok = true;
    ok &= check(!proof.supported && proof.error == k_unidentified_chain,
        "missing evidence: a chain file that cannot be examined must refuse the resume" + described(proof));
    ok &= check(proof.probes == 0,
        "missing evidence: no private startup probe may run for an unidentified chain" + described(proof));
    return ok;
}

bool chain_replaced_during_probe_is_not_remembered(const fs::path& fake_cli, const fs::path& root)
{
    const Check_scope scope{root / "replaced", "gpt-5.1"};
    const auto chain    = install_chain(fake_cli, scope.directory / "installation");
    const auto launch   = verified(chain);
    const auto original = fs::last_write_time(chain.manifest);
    const auto refused  = prove(scope, launch, chain.manifest);
    const auto replaced = fs::last_write_time(chain.manifest);
    // The refused proof was started under the original chain's evidence, so
    // that is the key it must not have left behind.
    fs::last_write_time(chain.manifest, original);
    const auto original_reproved = prove(scope, launch);
    fs::last_write_time(chain.manifest, replaced);
    const auto replaced_proved     = prove(scope, launch);
    const auto replaced_remembered = prove(scope, launch);
    bool ok = true;
    ok &= check(replaced != original, "replaced during probe: the fixture did not replace the manifest during the probe");
    ok &= check(refused.probes == 1 && !refused.supported && refused.error == k_unidentified_chain,
        "replaced during probe: a chain replaced while it was being proved must be refused" + described(refused));
    ok &= check(original_reproved.supported && original_reproved.probes == 1,
        "replaced during probe: the refused proof must not be remembered for the chain it started with" +
            described(original_reproved));
    ok &= check(replaced_proved.supported && replaced_proved.probes == 1,
        "replaced during probe: the refused proof must not be remembered for the replaced chain" +
            described(replaced_proved));
    ok &= check(replaced_remembered.supported && replaced_remembered.probes == 0,
        "replaced during probe: the replaced chain's own proof must then be remembered" +
            described(replaced_remembered));
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: codex_startup_proof_cache_tests <fake-codex>\n";
        return 2;
    }
    const fs::path fake_cli = addon::path_from_utf8(argv[1]);
    const fs::path root     = fs::temp_directory_path() / ("codex-startup-proof-cache-" + addon::random_nonce_hex());
    bool ok = true;
    try {
        fs::create_directories(root);
        ok &= unchanged_chain_reuses_the_proof(fake_cli, root);
        ok &= changed_modification_time_is_proved_again(fake_cli, root);
        ok &= changed_size_is_proved_again(fake_cli, root);
        ok &= relocated_chain_is_proved_again(fake_cli, root);
        ok &= retargeted_link_is_proved_again(fake_cli, root);
        ok &= missing_evidence_is_refused(fake_cli, root);
        ok &= chain_replaced_during_probe_is_not_remembered(fake_cli, root);
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ok = false;
    }
    std::error_code error;
    fs::remove_all(root, error);
    return ok ? 0 : 1;
}
