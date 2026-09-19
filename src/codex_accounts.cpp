#include "codex_accounts.h"
#include "codex_launcher.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/sha256.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace codex {
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

std::string profile_account_key(std::string_view absolute_path)
{
    fs::path path = fs::u8path(absolute_path).lexically_normal();
#if defined(_WIN32)
    std::wstring wide = path.generic_wstring();
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size == 0) return {};
    std::wstring folded(static_cast<std::size_t>(size), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
            static_cast<int>(wide.size()), folded.data(), size, nullptr, nullptr, 0))
    {
        return {};
    }
    path = fs::path(folded);
#endif
    const auto bytes = path.generic_u8string();
    return "profile-" + addon::to_hex(addon::Sha256::digest(std::string(bytes.begin(), bytes.end())));
}

std::optional<Profile_selection> profile_selection(const addon::Launch_environment& environment,
    const addon::Launcher_request& launcher, std::string_view fallback,
    std::string_view normalization_directory, std::string* error)
{
    const auto command = recognize_launcher(launcher.script_body);
    if (!command) { *error = "launcher_unrecognized"; return std::nullopt; }
    const bool windows = environment.platform_id.starts_with("windows-");
    const auto resolved_value = [&](std::string_view name) {
        auto value = environment_value(environment.effective_environment, name, windows);
        for (const auto& assignment : command->environment) {
            if (windows ? ascii_lower(assignment.first) == ascii_lower(std::string(name)) : assignment.first == name) {
                value = assignment.second;
            }
        }
        return value;
    };
    std::string profile = resolved_value("CODEX_HOME").value_or("");
    if (profile.empty()) {
        std::string home = resolved_value(windows ? "USERPROFILE" : "HOME").value_or(std::string(fallback));
        profile = path_text(fs::u8path(home) / ".codex");
    }
    fs::path path = fs::u8path(profile);
    if (!path.is_absolute()) path = fs::u8path(normalization_directory) / path;
    path = path.lexically_normal();
    if (!path.is_absolute()) { *error = "profile_path_invalid"; return std::nullopt; }
    Profile_selection selected{profile_account_key(path_text(path)), path_text(path)};
    if (selected.adapter_account_key.empty()) { *error = "profile_path_invalid"; return std::nullopt; }
    return selected;
}

std::optional<Profile_selection> selected_profile(const addon::Session_launch_context& request,
    addon::Invocation_context& context)
{
    if (!request.account_context || !request.account_context->profile_path) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_profile_unavailable");
        return std::nullopt;
    }
    Profile_selection selected{request.account_context->adapter_account_key, *request.account_context->profile_path};
    if (selected.adapter_account_key != profile_account_key(selected.profile_path)) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_context_mismatch");
        return std::nullopt;
    }
    return selected;
}

addon::Legacy_import_result import_account(const addon::Import_record_request& request)
{
    addon::Contract_failure failure;
    const auto legacy = addon::decode_legacy_account_record(request.record, &failure);
    if (!legacy || legacy->provider_id != "codex" || !legacy->profile_path) return {};
    const std::string key = profile_account_key(*legacy->profile_path);
    if (key.empty()) return {};
    auto conversion = addon::Json_value::object();
    conversion.set("adapter_account_key", addon::Json_value(key));
    return *addon::Legacy_import_result::from_conversion(std::move(conversion));
}

void handle_accounts(addon::Invocation_context& context)
{
    if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
    addon::Contract_failure failure;
    const auto request = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    std::string error;
    const auto selected = profile_selection(request->launch_environment, request->launcher,
        request->default_home_fallback, request->normalization_directory, &error);
    if (!selected) { context.fail(addon::Result_status::FAILED, error); return; }
    if (context.request().operation == addon::k_accounts_discover_operation) {
        std::error_code failure;
        const bool exists = fs::is_directory(fs::u8path(selected->profile_path), failure);
        addon::Account_record account;
        account.adapter_account_key = selected->adapter_account_key;
        account.profile_path = selected->profile_path;
        account.display_name = "Codex";
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
