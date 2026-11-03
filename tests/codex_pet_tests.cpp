#include "codex_accounts.h"

#include <logonomic_cli_addon/account_pet.h>
#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(static_cast<bool>(output), "write isolated pet fixture");
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::temp_directory_path() / ("codex-pets-" + addon::random_nonce_hex()))
    {
        check(fs::create_directory(m_path), "create isolated pet fixture directory");
        m_path = fs::canonical(m_path);
    }

    ~Temporary_directory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

addon::Account_context account(const fs::path& profile)
{
    const auto path = addon::path_to_utf8(profile);
    return {2, codex::profile_account_key(path), path};
}

addon::Account_pet_inspect_result inspect(
    addon::Host_simulator& host,
    const fs::path& profile,
    const fs::path& directory = {},
    const fs::path& home = {})
{
    const auto selected = account(profile);
    const auto id = host.invoke(
        {addon::k_account_pet_capability_id, addon::k_account_pet_version}, "inspect",
        addon::encode_account_pet_request(selected, addon::path_to_utf8(directory), addon::path_to_utf8(home)), 5000);
    const auto outcome = host.wait_outcome(id, 6000);
    check(outcome && outcome->status == addon::Host_status::SUCCESS,
        "packaged pet inspection completes without retiring the provider");
    check(!addon::account_context_acknowledgment_failure(selected, outcome->payload),
        "packaged helper preserves the exact selected account");
    const auto result = addon::decode_account_pet_inspect_result(outcome->payload, nullptr);
    check(result.has_value(), "packaged helper result conforms to account-pet@1");
    return *result;
}

bool watches(const addon::Account_pet_inspect_result& result, const fs::path& path)
{
    return std::ranges::find(result.watch_paths, addon::path_to_utf8(path)) != result.watch_paths.end();
}

void check_sheet(const addon::Account_pet_inspect_result& result, const fs::path& sheet)
{
    check(result.pet && result.unavailable_reason.empty(), "selected local pet metadata is available");
    check(addon::path_from_utf8(result.pet->spritesheet_path) == sheet,
        "the selected account determines the exact sheet path");
    check(watches(result, sheet), "sheet changes remain observable");
}

void native_default_track(const addon::Account_pet& pet)
{
    // Oracle: openai/codex rust-v0.160.0 tui/src/pets/{catalog,model}.rs.
    check(pet.frame_width == 192 && pet.frame_height == 208 && pet.columns == 8 && pet.rows == 9,
        "native default grid covers the 1536 by 1872 sheet");
    check(pet.frames == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5} &&
        pet.frame_durations_ms == std::vector<std::uint32_t>{1680, 660, 660, 840, 840, 1920},
        "native idle frames retain their individual durations");
}

void check_builtin_discovery(
    const addon::Account_pet_inspect_result& result, const fs::path& root)
{
    const auto discovery = std::ranges::find_if(result.watch_discoveries, [&root](const auto& item) {
        return addon::path_from_utf8(item.directory) == root;
    });
    check(discovery != result.watch_discoveries.end() &&
        discovery->relative_directory_pattern == "*/assets" && discovery->name_prefix == "codex-spritesheet-" &&
        discovery->suffixes == std::vector<std::string>{".webp", ".png"},
        "built-in repair observes matching sheets in existing and future pack assets directories");
    check(!watches(result, root), "cache discovery stays separate from exact file inputs");
}

void builtin_repair_discovery(addon::Host_simulator& host, const fs::path& profile)
{
    const auto root = profile / "cache" / "tui-pets";
    write(profile / "config.toml", "tui.pet = 'codex'\n");
    auto result = inspect(host, profile);
    check(!result.pet && !result.unavailable_reason.empty(), "a missing cache reports unavailable metadata");
    check_builtin_discovery(result, root);

    const auto pack = root / "future-pack";
    fs::create_directories(pack);
    result = inspect(host, profile);
    check(!result.pet, "an empty pack cannot supply a selected sheet");
    check_builtin_discovery(result, root);
    fs::create_directory(pack / "assets");
    result = inspect(host, profile);
    check(!result.pet, "an empty assets directory keeps repair discovery active");
    check_builtin_discovery(result, root);

    const auto sheet = pack / "assets" / "codex-spritesheet-v5.webp";
    write(sheet, "metadata fixture");
    result = inspect(host, profile);
    check_sheet(result, sheet);
    check_builtin_discovery(result, root);
    check(!watches(result, sheet.parent_path()), "a cache assets directory is a filtered discovery input");

    fs::remove(sheet);
    result = inspect(host, profile);
    check(!result.pet && !result.unavailable_reason.empty(), "deleting a selected built-in makes it unavailable");
    check_builtin_discovery(result, root);
    const auto replacement = sheet.parent_path() / "replacement.tmp";
    write(replacement, "metadata fixture");
    fs::rename(replacement, sheet);
    result = inspect(host, profile);
    check_sheet(result, sheet);
    check_builtin_discovery(result, root);
}

void builtins_and_configuration(addon::Host_simulator& host, const fs::path& profile)
{
    fs::create_directories(profile);
    auto result = inspect(host, profile);
    check(!result.pet && result.unavailable_reason.empty() && watches(result, profile / "config.toml"),
        "an unselected pet stays absent while configuration creation remains observable");

    write(profile / "config.toml", "[tui]\npet = 'disabled'\n");
    result = inspect(host, profile);
    check(!result.pet && result.unavailable_reason.empty(), "the native disabled selection stays absent");

    // Pixel decoding belongs to the host; these fixtures exercise metadata only.
    const auto sheet = profile / "cache" / "tui-pets" / "v1" / "assets" / "codex-spritesheet-v4.webp";
    write(sheet, "metadata fixture");
    const std::vector<std::string> configurations = {
        "[tui]\npet = 'codex'\n",
        "\"tui\".\"pet\" = \"codex\"\n",
        "tui = { pet = 'codex' }\n",
        "notes = '''\n[tui]\npet = 'disabled'\n'''\ntui.pet = 'codex'\n",
        "[tui]\npet = \"co\\u0064ex\" # selected account pet\n",
    };
    for (const auto& config : configurations) {
        write(profile / "config.toml", config);
        result = inspect(host, profile);
        check_sheet(result, sheet);
        native_default_track(*result.pet);
        std::ifstream after(profile / "config.toml", std::ios::binary);
        const std::string retained((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
        check(retained == config, "inspection leaves native configuration byte-for-byte unchanged");
    }

    write(profile / "config.toml", "[tui\npet = 'codex'\n");
    result = inspect(host, profile);
    check(!result.pet && !result.unavailable_reason.empty() && watches(result, profile / "config.toml"),
        "malformed TOML remains a repairable pet-specific failure");
    write(profile / "config.toml", "tui.pet = 'codex'\n");
    check_sheet(inspect(host, profile), sheet);

    write(profile / "config.toml", "tui.pet = 'dewey'\n");
    result = inspect(host, profile);
    check(!result.pet && !result.unavailable_reason.empty(),
        "a selected built-in with no local cache is unavailable without downloading");
}

void custom_selectors(addon::Host_simulator& host, const fs::path& profile, const fs::path& fixture)
{
    const auto pet_directory = profile / "pets" / "account-pet";
    const auto sheet = pet_directory / "spritesheet.png";
    write(pet_directory / "pet.json",
        R"({"spritesheetPath":"spritesheet.png","animations":{"idle":{"frames":[0,2,4],"fps":12.5}}})");
    write(sheet, "metadata fixture");
    for (const char* selector : {"custom:account-pet", "account-pet"}) {
        write(profile / "config.toml", std::string("tui.pet = '") + selector + "'\n");
        const auto result = inspect(host, profile);
        check_sheet(result, sheet);
        check(result.pet->frames == std::vector<std::uint32_t>{0, 2, 4} &&
            result.pet->frame_durations_ms == std::vector<std::uint32_t>{80, 80, 80},
            "custom idle frames and native fps define the hover track");
        check(watches(result, pet_directory / "pet.json"), "custom manifest edits remain observable");
    }

    fs::remove(sheet);
    auto result = inspect(host, profile);
    check(!result.pet && !result.unavailable_reason.empty() && watches(result, sheet),
        "a missing selected sheet remains observable for repair");
    write(sheet, "metadata fixture");
    check_sheet(inspect(host, profile), sheet);

    const auto shared = fixture / "shared-custom";
    write(shared / "pet.json", R"({"spritesheetPath":"spritesheet.webp"})");
    write(shared / "spritesheet.webp", "metadata fixture");
    const auto select = [&](const std::string& selector, const fs::path& directory, const fs::path& home) {
        const auto text = addon::serialize_json(addon::Json_value(selector));
        write(profile / "config.toml", "tui.pet = " + text + '\n');
        const auto selected = inspect(host, profile, directory, home);
        check_sheet(selected, shared / "spritesheet.webp");
        check(!watches(selected, shared), "explicit directory selectors expose exact manifest and sheet inputs");
    };
    select(addon::path_to_utf8(shared), {}, {});
    select(addon::path_to_utf8(shared / "pet.json"), {}, {});
    select("./shared-custom", fixture, {});
    select("~/shared-custom", {}, fixture);

    const auto avatar = profile / "avatars" / "installed-avatar";
    write(avatar / "avatar.json", R"({"spritesheetPath":"spritesheet.webp"})");
    write(avatar / "spritesheet.webp", "metadata fixture");
    write(profile / "config.toml", "tui.pet = 'installed-avatar'\n");
    check_sheet(inspect(host, profile), avatar / "spritesheet.webp");

    write(profile / "config.toml", "tui.pet = 'custom:account-pet'\n");
    write(pet_directory / "pet.json",
        R"({"spritesheetPath":"spritesheet.png","animations":{"idle":{"frames":[72],"fps":8}}})");
    result = inspect(host, profile);
    check(!result.pet && !result.unavailable_reason.empty() && watches(result, pet_directory / "pet.json"),
        "a frame outside the native grid is a repairable manifest failure");
}

class Cancellation_context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int milliseconds) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return m_cancelled;
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }
    void cancel() { m_cancelled = true; }

private:
    addon::Invoke_request m_request;
    bool m_cancelled = false;
};

void cancel_packaged_helper(addon::Host_simulator& host, const fs::path& package)
{
    addon::Environment environment;
    for (const auto& entry : host.adapter_environment()) {
        const auto equal = entry.find('=');
        check(equal != std::string::npos, "simulated adapter environment has name-value entries");
        environment.emplace_back(entry.substr(0, equal), entry.substr(equal + 1));
    }
#ifdef _WIN32
    const auto helper = package / "helpers" / "codex_pet_helper.exe";
#else
    const auto helper = package / "helpers" / "codex_pet_helper";
#endif
    Cancellation_context context;
    addon::Probe_exchange exchange;
    exchange.on_output = [](std::string_view) { return std::optional<std::string>(std::string{}); };
    // The helper waits for its request. The probe's post-launch tick cancels
    // that live process using the same custody path as metadata cancellation.
    exchange.tick = [&context] { context.cancel(); };
    const auto result = addon::probe(
        {addon::path_to_utf8(helper)}, environment, addon::path_to_utf8(package), 3000, context, &exchange);
    check(context.cancel_requested() && !result,
        "a live packaged helper waiting for input is retired through SDK cancellation");
}

void cancel_packaged_request(addon::Host_simulator& host, const fs::path& profile)
{
    const auto selected = account(profile);
    const auto id = host.invoke(
        {addon::k_account_pet_capability_id, addon::k_account_pet_version}, "inspect",
        addon::encode_account_pet_request(selected), 5000);
    host.cancel(id);
    const auto outcome = host.wait_outcome(id, 6000);
    check(outcome &&
        (outcome->status == addon::Host_status::CANCELLED || outcome->status == addon::Host_status::SUCCESS),
        "cancelling packaged metadata settles the request without retiring its provider");
    if (outcome->status == addon::Host_status::SUCCESS) {
        check(!addon::account_context_acknowledgment_failure(selected, outcome->payload),
            "metadata completed before cancellation still belongs to the selected account");
    }
    check(host.state() == addon::Simulator_state::READY,
        "metadata cancellation leaves the ordinary provider generation available");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "pet tests require the assembled package directory");
        const auto package = fs::absolute(addon::path_from_utf8(argv[1]));
        Temporary_directory fixture;
        addon::Simulator_options options;
        options.package_directory = package;
        options.scratch_directory = fixture.path() / "scratch";
        options.profile_storage_root = fixture.path() / "storage";
        addon::Host_simulator host(std::move(options), nullptr);
        addon::Contract_failure failure;
        check(host.start(&failure), "packaged Codex starts: " + failure.detail);
        const auto live = host.live_capabilities();
        check(std::ranges::find(live, addon::Capability_pair{
                addon::k_account_pet_capability_id, addon::k_account_pet_version}) != live.end(),
            "the assembled package declares and implements account-pet@1");
        const auto first = fixture.path() / "first-account";
        const auto second = fixture.path() / "second-account";
        builtins_and_configuration(host, first);
        builtin_repair_discovery(host, fixture.path() / "repair-account");
        custom_selectors(host, second, fixture.path());
        write(first / "config.toml", "tui.pet = 'codex'\n");
        auto retained = inspect(host, first);
        check(retained.pet.has_value(), "the first account retains its selected built-in pet");
        native_default_track(*retained.pet);
        check(!inspect(host, second).pet, "a malformed second account cannot replace the first account's pet");
        cancel_packaged_helper(host, package);
        cancel_packaged_request(host, first);
        retained = inspect(host, first);
        check(retained.pet.has_value(), "metadata remains available after cancellation");
        native_default_track(*retained.pet);
        check(host.shutdown(2000, nullptr), "packaged Codex exits after pet inspection");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
