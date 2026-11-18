#include "codex_accounts.h"
#include "codex_launcher.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/sha256.h>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace codex {
namespace addon = logonomic::cli_addon;
namespace {

namespace fs = std::filesystem;

std::string path_text(const fs::path& path)
{
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

std::string ascii_lower(std::string text)
{
    for (char& byte : text) {
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    }
    return text;
}

#ifdef _WIN32
fs::path win32_full_path(const fs::path& path)
{
    const auto size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size) return {};
    std::wstring name(size, L'\0');
    const auto length = GetFullPathNameW(path.c_str(), size, name.data(), nullptr);
    if (!length || length >= size) return {};
    name.resize(length);
    return name;
}

fs::path dos_path(const fs::path& path)
{
    const auto& name = path.native();
    if (!name.starts_with(LR"(\\?\)") && !name.starts_with(LR"(\\.\)")) return path;
    if (name.substr(4, 4) == L"UNC\\") return L"\\\\" + name.substr(8);
    return name.size() >= 7 && name[5] == L':' && name[6] == L'\\' ? fs::path(name.substr(4)) : path;
}

fs::path windows_profile(const fs::path& path)
{
    // Codex canonicalizes the supplied directory, strips a drive/UNC namespace
    // prefix, then opens its children. Win32 applies different rules to a final
    // component and the same component followed by a child.
    const auto preferred = fs::path(path).make_preferred();
    const auto& spelling = path.native();
    // GetFullPathNameW trims the final component even in a device path.
    // Native directory handles open an explicit device spelling verbatim.
    const auto input = spelling.starts_with(LR"(\\?\)") || spelling.starts_with(LR"(\\.\)")
        ? preferred : win32_full_path(preferred);
    if (input.empty()) return {};
    std::error_code error;
    const auto canonical = fs::canonical(input, error);
    const auto ordinary = dos_path(error ? input : canonical);
    if (error) return win32_full_path(ordinary) == ordinary ? ordinary : input;
    const auto child = win32_full_path(ordinary / L"config.toml");
    if (child.empty()) return {};
    const auto directory = child.parent_path();
    if (win32_full_path(directory) == directory) return directory;
    // Preserve an exact name such as a trailing space when CODEX_HOME is
    // subsequently passed to Codex as a directory selector again.
    const auto& name = directory.native();
    return name.starts_with(L"\\\\") ? LR"(\\?\UNC\)" + name.substr(2) : LR"(\\?\)" + name;
}
#endif

fs::path normalized_profile(const fs::path& path)
{
    if (!path.is_absolute()) return {};
#ifdef _WIN32
    return windows_profile(path);
#else
    // Native lookup traverses a directory symlink before applying a following
    // "..". Keep prospective leaf names while resolving that existing prefix.
    auto suffix = path.end();
    while (suffix != path.begin()) {
        if (*--suffix != "..") continue;
        ++suffix;
        fs::path prefix;
        for (auto part = path.begin(); part != suffix; ++part) prefix /= *part;
        std::error_code error;
        auto resolved = fs::canonical(prefix, error);
        if (error) return {};
        for (; suffix != path.end(); ++suffix) resolved /= *suffix;
        return resolved.lexically_normal();
    }
    return path.lexically_normal();
#endif
}

std::string account_key(std::optional<std::string> identity)
{
    if (!identity) return {};
#ifdef _WIN32
    // Codex issued keys with generic separators. Use a bijection so the
    // SDK's opaque filesystem identity, including binary data, stays distinct.
    for (char& byte : *identity) {
        if (byte == '\\') byte = '/';
        else if (byte == '/') byte = '\\';
    }
#endif
    return "profile-" + addon::to_hex(addon::Sha256::digest(*identity));
}

std::optional<std::string> environment_value(const addon::Json_value& environment, std::string_view name, bool windows)
{
    for (const auto& item : environment.as_object()) {
        if ((windows ? ascii_lower(item.name) == ascii_lower(std::string(name)) : item.name == name) &&
            item.value.is_string())
        {
            return item.value.as_string();
        }
    }
    return {};
}

} // namespace

std::string native_home_fallback()
{
#if defined(_WIN32)
    // dirs 6 uses the known profile folder, irrespective of HOME/USERPROFILE.
    PWSTR path = nullptr;
    const auto status = SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> profile(path, CoTaskMemFree);
    if (FAILED(status)) {
        return {};
    }
    return path_text(fs::path(profile.get()));
#else
    const auto size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size < 0 ? 512 : static_cast<std::size_t>(size));
    passwd entry{};
    passwd* result = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) != 0 || !result) {
        return {};
    }
    return entry.pw_dir;
#endif
}

std::string profile_account_key(std::string_view absolute_path)
{
    const auto path = normalized_profile(addon::path_from_utf8(absolute_path));
    return account_key(addon::native_account_profile_identity(path));
}

std::optional<Profile_selection> granted_profile(const addon::Account_context& account, std::string_view selected_path)
{
    if (!account.profile_path || account.adapter_account_key.empty()) return {};
    const auto original = addon::path_from_utf8(*account.profile_path).lexically_normal();
    const auto granted  = normalized_profile(addon::path_from_utf8(*account.profile_path));
    const auto selected = normalized_profile(addon::path_from_utf8(selected_path));
    if (granted.empty() || selected.empty() || !addon::same_native_account_profile(granted, selected)) return {};
    const bool current = account.adapter_account_key == account_key(addon::native_account_profile_identity(selected));
    // Old keys were issued from the spelling in the grant. Its selected
    // native root has already been checked against the independent locator.
    const bool historical = !current && account.version == 2 &&
        account.adapter_account_key == account_key(addon::historical_account_profile_identity(original, original));
    if (!current && !historical) return {};
    return Profile_selection{account.adapter_account_key, path_text(selected)};
}

std::optional<Profile_selection> profile_selection(const addon::Launch_environment& environment,
    const addon::Launcher_request& launcher, std::string_view fallback,
    std::string_view normalization_directory, std::string* error)
{
    const auto command = recognize_launcher(launcher.script_body, launcher.runner_key);
    if (!command) { *error = "launcher_unrecognized"; return std::nullopt; }
    return profile_from_environment(environment, fallback, normalization_directory, error);
}

std::optional<Profile_selection> profile_from_environment(const addon::Launch_environment& environment,
    std::string_view fallback, std::string_view normalization_directory, std::string* error)
{
    auto path = configured_profile_directory(environment, fallback);
    if (path.empty()) { *error = "profile_unavailable"; return std::nullopt; }
    if (!path.is_absolute()) path = addon::path_from_utf8(normalization_directory) / path;
    path = normalized_profile(path);
    if (!path.is_absolute()) { *error = "profile_path_invalid"; return std::nullopt; }
    Profile_selection selected{account_key(addon::native_account_profile_identity(path)), path_text(path)};
    if (selected.adapter_account_key.empty()) { *error = "profile_path_invalid"; return std::nullopt; }
    return selected;
}

fs::path configured_profile_directory(const addon::Launch_environment& environment, std::string_view fallback)
{
    const bool windows = environment.platform_id.starts_with("windows-");
    // Shell assignments have already been expanded in the host's frozen environment.
    std::string profile = environment_value(environment.effective_environment, "CODEX_HOME", windows).value_or("");
    if (profile.empty()) {
        std::string home = windows ? std::string{} :
            environment_value(environment.effective_environment, "HOME", false).value_or("");
        if (home.empty()) {
            home = fallback;
        }
        if (home.empty()) return {};
        return addon::path_from_utf8(home) / ".codex";
    }
    return addon::path_from_utf8(profile);
}

fs::path canonical_profile_directory(const fs::path& path)
{
    if (!path.is_absolute()) return {};
#ifdef _WIN32
    return windows_profile(path);
#else
    std::error_code error;
    const auto canonical = fs::weakly_canonical(path, error);
    return error ? fs::path{} : canonical;
#endif
}

std::optional<Profile_selection> selected_profile(const addon::Session_launch_context& request,
    addon::Invocation_context& context)
{
    if (!request.account_context || !request.account_context->profile_path) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_profile_unavailable");
        return std::nullopt;
    }
    const auto selected = granted_profile(*request.account_context, *request.account_context->profile_path);
    if (!selected) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_context_mismatch");
        return std::nullopt;
    }
    return selected;
}

void handle_accounts(addon::Invocation_context& context)
{
    if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
    addon::Contract_failure failure;
    const auto request = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    std::string error;
    const auto selected = profile_selection(request->launch_environment, request->launcher,
        native_home_fallback(), request->normalization_directory, &error);
    if (!selected) { context.fail(addon::Result_status::FAILED, error); return; }
    if (context.request().operation == addon::k_accounts_discover_operation) {
        std::error_code failure;
        const bool exists = fs::is_directory(addon::path_from_utf8(selected->profile_path), failure);
        addon::Account_record account;
        account.adapter_account_key = selected->adapter_account_key;
        account.profile_path = selected->profile_path;
        account.display_name = addon::account_display_name(selected->profile_path);
        account.status = exists ? addon::Account_status::AVAILABLE : addon::Account_status::UNAVAILABLE;
        context.succeed(addon::encode_accounts({account}));
    }
    else
    if (context.request().operation == addon::k_accounts_resolve_operation) {
        context.succeed(addon::encode_account_resolution({selected->adapter_account_key}));
    }
    else context.reject_unknown_operation();
}

} // namespace codex
