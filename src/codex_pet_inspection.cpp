#include "codex_pet_inspection.h"

#include <logonomic_cli_addon/account_pet.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
namespace json  = addon::json;

constexpr std::size_t k_max_pet_document_bytes = 4 * 1024 * 1024;
constexpr std::size_t k_max_cache_entries = 256;
constexpr std::uint32_t k_max_pet_frames = 256;
constexpr std::uint64_t k_max_sheet_pixels = 16 * 1024 * 1024;

std::string read_document(const fs::path& path, const char* reason)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(reason);
    }
    std::string contents;
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto bytes = static_cast<std::size_t>(input.gcount());
        if (bytes > k_max_pet_document_bytes - contents.size()) {
            throw std::runtime_error(reason);
        }
        contents.append(buffer.data(), bytes);
    }
    if (!input.eof()) {
        throw std::runtime_error(reason);
    }
    return contents;
}

void watch(addon::Account_pet_inspect_result& result, const fs::path& path)
{
    const auto text = addon::path_to_utf8(path.lexically_normal());
    if (std::ranges::find(result.watch_paths, text) == result.watch_paths.end()) {
        result.watch_paths.push_back(text);
    }
}

bool file_exists(const fs::path& path)
{
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        throw std::runtime_error("pet_source_unreadable");
    }
    return exists;
}

std::optional<std::string> configured_pet(const fs::path& home)
{
    const auto path = home / "config.toml";
    if (!file_exists(path)) {
        return std::nullopt;
    }
    toml::table config;
    try {
        config = toml::parse(read_document(path, "pet_configuration_unreadable"));
    }
    catch (const toml::parse_error&) {
        throw std::runtime_error("pet_configuration_invalid");
    }
    if (config["tui"] && !config["tui"].is_table()) {
        throw std::runtime_error("pet_configuration_invalid");
    }
    const auto pet = config["tui"]["pet"];
    if (!pet) {
        return std::nullopt;
    }
    const auto selected = pet.value<std::string>();
    if (!selected || selected->find('\0') != std::string::npos) {
        throw std::runtime_error("pet_selection_invalid");
    }
    return selected;
}

addon::Account_pet default_pet()
{
    addon::Account_pet pet;
    pet.frame_width        = 192;
    pet.frame_height       = 208;
    pet.columns            = 8;
    pet.rows               = 9;
    pet.frames             = {0, 1, 2, 3, 4, 5};
    pet.frame_durations_ms = {1680, 660, 660, 840, 840, 1920};
    return pet;
}

std::uint32_t positive_integer(const addon::Json_value& value)
{
    if (!value.is_integer() || value.as_integer() <= 0 ||
        value.as_integer() > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("pet_manifest_invalid");
    }
    return static_cast<std::uint32_t>(value.as_integer());
}

void load_frame_grid(addon::Account_pet& pet, const addon::Json_value& manifest)
{
    const auto* frame = manifest.find("frame");
    if (!frame || frame->is_null()) {
        return;
    }
    pet.frame_width  = positive_integer(json::member(*frame, "width"));
    pet.frame_height = positive_integer(json::member(*frame, "height"));
    pet.columns      = positive_integer(json::member(*frame, "columns"));
    pet.rows         = positive_integer(json::member(*frame, "rows"));
}

void load_idle_track(addon::Account_pet& pet, const addon::Json_value& manifest)
{
    const auto* animations = manifest.find("animations");
    if (!animations) {
        return;
    }
    if (!animations->is_object()) {
        throw std::runtime_error("pet_manifest_invalid");
    }
    const auto* idle = animations->find("idle");
    if (!idle) {
        return;
    }
    const auto& frames = json::member(*idle, "frames");
    if (!frames.is_array() || frames.as_array().empty() ||
        frames.as_array().size() > k_max_pet_frames)
    {
        throw std::runtime_error("pet_animation_invalid");
    }
    double fps = 8.0;
    if (const auto* configured_fps = idle->find("fps"); configured_fps && !configured_fps->is_null()) {
        if (!configured_fps->is_number()) {
            throw std::runtime_error("pet_animation_invalid");
        }
        fps = configured_fps->as_number();
    }
    const double milliseconds = 1000.0 / fps;
    if (!std::isfinite(fps) || fps <= 0.0 || fps > 60.0 ||
        !std::isfinite(milliseconds) || milliseconds > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("pet_animation_invalid");
    }
    const auto duration = static_cast<std::uint32_t>(std::max(17.0, std::round(milliseconds)));
    pet.frames.clear();
    pet.frame_durations_ms.clear();
    for (const auto& frame : frames.as_array()) {
        if (!frame.is_integer() || frame.as_integer() < 0 ||
            static_cast<std::uint64_t>(frame.as_integer()) >=
                static_cast<std::uint64_t>(pet.columns) * pet.rows)
        {
            throw std::runtime_error("pet_animation_invalid");
        }
        pet.frames.push_back(static_cast<std::uint32_t>(frame.as_integer()));
        pet.frame_durations_ms.push_back(duration);
    }
}

void validate_grid(const addon::Account_pet& pet)
{
    const auto columns = static_cast<std::uint64_t>(pet.columns);
    const auto rows    = static_cast<std::uint64_t>(pet.rows);
    const auto width   = static_cast<std::uint64_t>(pet.frame_width) * columns;
    const auto height  = static_cast<std::uint64_t>(pet.frame_height) * rows;
    if (columns * rows > k_max_pet_frames || width > k_max_sheet_pixels ||
        height > k_max_sheet_pixels || width * height > k_max_sheet_pixels ||
        std::ranges::any_of(pet.frames, [columns, rows](std::uint32_t frame) {
            return frame >= columns * rows;
        }))
    {
        throw std::runtime_error("pet_frame_grid_invalid");
    }
}

addon::Account_pet load_custom_pet(
    const fs::path& directory, addon::Account_pet_inspect_result& result)
{
    const auto pet_manifest    = directory / "pet.json";
    const auto avatar_manifest = directory / "avatar.json";
    watch(result, pet_manifest);
    watch(result, avatar_manifest);
    const auto manifest_path = file_exists(pet_manifest) ? pet_manifest : avatar_manifest;
    const auto manifest = addon::parse_json(
        read_document(manifest_path, "pet_manifest_unreadable"),
        {k_max_pet_document_bytes}, nullptr);
    if (!manifest || !manifest->is_object()) {
        throw std::runtime_error("pet_manifest_invalid");
    }

    auto pet = default_pet();
    load_frame_grid(pet, *manifest);
    load_idle_track(pet, *manifest);
    validate_grid(pet);

    std::string filename = "spritesheet.webp";
    if (const auto* configured_path = manifest->find("spritesheetPath"); configured_path && !configured_path->is_null()) {
        if (!configured_path->is_string()) {
            throw std::runtime_error("pet_manifest_invalid");
        }
        const auto trimmed = addon::trim(configured_path->as_string());
        if (trimmed.find('\0') != std::string::npos) {
            throw std::runtime_error("pet_manifest_invalid");
        }
        if (!trimmed.empty()) {
            filename = trimmed;
        }
    }
    const auto relative = addon::path_from_utf8(filename);
    if (relative.has_root_name() || relative.has_root_directory() ||
        std::ranges::any_of(relative, [](const fs::path& component) { return component == ".."; }))
    {
        throw std::runtime_error("pet_manifest_invalid");
    }
    const auto sheet = (directory / relative).lexically_normal();
    watch(result, sheet);
    if (!file_exists(sheet)) {
        throw std::runtime_error("pet_asset_unavailable");
    }
    pet.spritesheet_path = addon::path_to_utf8(sheet);
    return pet;
}

bool native_builtin(std::string_view selector)
{
    constexpr std::array<std::string_view, 8> names = {
        "codex", "dewey", "fireball", "rocky", "seedy", "stacky", "bsod", "null-signal"};
    return std::ranges::find(names, selector) != names.end();
}

std::optional<fs::path> builtin_asset(
    const fs::path& home, const std::string& selector,
    addon::Account_pet_inspect_result& result)
{
    const auto root = home / "cache" / "tui-pets";
    const auto prefix = selector + "-spritesheet-";
    result.watch_discoveries.push_back({
        addon::path_to_utf8(root), "*/assets", prefix, {".webp", ".png"}});
    if (!file_exists(root)) {
        return std::nullopt;
    }
    std::optional<fs::path> selected;
    fs::file_time_type newest{};
    std::size_t entries = 0;
    std::error_code error;
    fs::directory_iterator packs(root, error);
    if (error) {
        throw std::runtime_error("pet_cache_unreadable");
    }
    for (auto end = fs::directory_iterator(); packs != end; packs.increment(error)) {
        if (error || ++entries > k_max_cache_entries) {
            throw std::runtime_error("pet_cache_unreadable");
        }
        const bool is_pack = packs->is_directory(error);
        if (error) {
            throw std::runtime_error("pet_cache_unreadable");
        }
        if (!is_pack) {
            continue;
        }
        const auto assets = packs->path() / "assets";
        if (!file_exists(assets)) {
            continue;
        }
        fs::directory_iterator files(assets, error);
        if (error) {
            throw std::runtime_error("pet_cache_unreadable");
        }
        for (; files != end; files.increment(error)) {
            if (error || ++entries > k_max_cache_entries) {
                throw std::runtime_error("pet_cache_unreadable");
            }
            const auto name = addon::path_to_utf8(files->path().filename());
            if (!name.starts_with(prefix) ||
                (files->path().extension() != ".webp" && files->path().extension() != ".png"))
            {
                continue;
            }
            const bool is_sheet = files->is_regular_file(error);
            if (error) {
                throw std::runtime_error("pet_cache_unreadable");
            }
            if (!is_sheet) {
                continue;
            }
            const auto modified = files->last_write_time(error);
            if (error) {
                throw std::runtime_error("pet_cache_unreadable");
            }
            if (!selected || modified > newest) {
                selected = files->path();
                newest   = modified;
            }
        }
        if (error) {
            throw std::runtime_error("pet_cache_unreadable");
        }
    }
    if (error) {
        throw std::runtime_error("pet_cache_unreadable");
    }
    return selected;
}

addon::Account_pet resolve_pet(
    const fs::path& home, const std::string& selector,
    const std::string& normalization_directory,
    const std::string& home_directory,
    addon::Account_pet_inspect_result& result)
{
    const bool custom = selector.starts_with("custom:");
    const auto id = custom ? selector.substr(7) : selector;
    auto path = addon::path_from_utf8(id);
    if (!custom && (path.has_parent_path() || path.is_absolute() || id == "." || id == "..")) {
        if (id.starts_with("~/")) {
            if (home_directory.empty()) {
                throw std::runtime_error("pet_home_path_unavailable");
            }
            path = addon::path_from_utf8(home_directory) / addon::path_from_utf8(id.substr(2));
        }
        if (!path.is_absolute()) {
            if (normalization_directory.empty()) {
                throw std::runtime_error("pet_relative_path_unavailable");
            }
            path = (addon::path_from_utf8(normalization_directory) / path).lexically_normal();
        }
        std::error_code error;
        const bool directory_selected = fs::is_directory(path, error);
        if (error) {
            watch(result, path);
            throw std::runtime_error("pet_source_unreadable");
        }
        if (!directory_selected) {
            watch(result, path);
        }
        const auto directory = directory_selected ? path : path.parent_path();
        return load_custom_pet(directory, result);
    }
    if (id.empty() || path.has_parent_path() || path.has_root_name()) {
        throw std::runtime_error("pet_selection_invalid");
    }
    if (!custom) {
        const auto asset = builtin_asset(home, id, result);
        if (asset) {
            auto pet = default_pet();
            pet.spritesheet_path = addon::path_to_utf8(*asset);
            watch(result, *asset);
            return pet;
        }
        if (native_builtin(id)) {
            throw std::runtime_error("pet_asset_unavailable");
        }
    }
    const auto directory = home / "pets" / path;
    watch(result, directory / "pet.json");
    if (file_exists(directory / "pet.json")) {
        return load_custom_pet(directory, result);
    }
    return load_custom_pet(home / "avatars" / path, result);
}

} // namespace

addon::Account_pet_inspect_result inspect_account_pet(const addon::Account_pet_request& request)
{
    const auto& account = request.account_context;
    addon::Account_pet_inspect_result result;
    result.account_context = account;
    const auto home = addon::path_from_utf8(*account.profile_path);
    watch(result, home / "config.toml");
    try {
        const auto selector = configured_pet(home);
        if (selector && *selector != "disabled") {
            result.pet = resolve_pet(
                home, *selector, request.normalization_directory, request.home_directory, result);
        }
    }
    catch (const std::runtime_error& error) {
        result.pet.reset();
        result.unavailable_reason = error.what();
    }
    return result;
}

} // namespace codex
