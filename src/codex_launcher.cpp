#include "codex_launcher.h"
#include "codex_accounts.h"
#include "codex_macho.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/presentation_record.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>

namespace codex {
namespace addon = logonomic::cli_addon;
namespace {

using addon::lower;
using addon::environment_value;
using addon::set_environment;
using addon::regular_file;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;

using addon::Environment;
using addon::Launcher_whitespace;
namespace fs = std::filesystem;

bool is_codex_program(std::string_view program)
{
    const auto base = addon::base_name(program, Launcher_whitespace::ASCII);
    return base == "codex" || base == "codex-pet" || base.starts_with("codex@");
}

std::string path_text(const fs::path& path)
{
    const auto bytes = path.generic_u8string();
    return std::string(bytes.begin(), bytes.end());
}

std::string profile_display_name(const fs::path& profile)
{
    std::string name = path_text(profile.filename());
    std::string folded = addon::lower(name);
    if (folded.starts_with(".codex")) {
        name.erase(0, std::string(".codex").size());
    }
    const auto first = name.find_first_not_of("-_. ");
    if (first == std::string::npos) {
        return "Codex";
    }
    name.erase(0, first);
    for (char& character : name) {
        if (character == '-' || character == '_' || character == '.') {
            character = ' ';
        }
    }
    std::string display_name = "Codex " + name;
    if (!addon::is_valid_utf8(display_name)) {
        return "Codex profile";
    }
    return addon::fitted_display_name(display_name).value_or("Codex profile");
}

struct Configured_codex_home
{
    fs::path scan_path;
    fs::path identity_path;
};

std::optional<Configured_codex_home> configured_codex_home(
    const addon::Discover_request& request)
{
    addon::Launch_environment environment;
    environment.platform_id           = request.platform_id;
    environment.effective_environment = request.ambient_environment;
    std::string profile_error;
    const auto selected = profile_from_environment(environment, native_home_fallback(), {}, &profile_error);
    if (!selected) {
        return std::nullopt;
    }
    const fs::path path = addon::path_from_utf8(selected->profile_path);
    Configured_codex_home home;
    home.scan_path = path.lexically_normal();
    std::error_code error;
    home.identity_path = fs::weakly_canonical(path, error);
    if (error) {
        home.identity_path = home.scan_path;
    }
    return home;
}

bool has_codex_auth_file(const fs::path& profile)
{
    std::error_code error;
    return fs::is_regular_file(profile / "auth.json", error) && !error;
}

bool safe_for_environment_line(const fs::path& profile)
{
    const std::string value = path_text(profile);
    return value.find_first_of("\r\n") == std::string::npos;
}

std::optional<std::string> codex_profile_environment_text_impl(
    std::string_view profile_path)
{
    std::string assignment = "CODEX_HOME=";
    assignment.append(profile_path);
    if (assignment.size() > addon::limits::k_max_environment_text_bytes ||
        assignment.find('\0') != std::string::npos || !addon::is_valid_utf8(assignment))
    {
        return std::nullopt;
    }
    return assignment;
}

std::vector<fs::path> authenticated_codex_profiles(
    const Configured_codex_home& selected_home,
    codex::Codex_profile_discovery_stats* out_profile_stats)
{
    std::vector<fs::path> profiles;
    const auto append_if_authenticated = [&profiles](const fs::path& candidate) {
        if (profiles.size() == addon::limits::k_max_discovery_candidates) {
            return;
        }
        if (!has_codex_auth_file(candidate)) {
            return;
        }
        std::error_code error;
        fs::path normalized = fs::weakly_canonical(candidate, error);
        if (error) {
            normalized = candidate.lexically_normal();
        }
        if (!safe_for_environment_line(normalized)) {
            return;
        }
        if (!codex_profile_environment_text_impl(path_text(normalized))) {
            return;
        }
        const bool already_present = std::ranges::any_of(
            profiles,
            [&normalized](const fs::path& existing) {
                std::error_code equivalent_error;
                if (fs::equivalent(existing, normalized, equivalent_error) &&
                    !equivalent_error)
                {
                    return true;
                }
                return existing == normalized;
            });
        if (!already_present) {
            profiles.push_back(std::move(normalized));
        }
    };

    append_if_authenticated(selected_home.scan_path);
    std::error_code error;
    fs::directory_iterator entry(
        selected_home.scan_path.parent_path(),
        fs::directory_options::skip_permission_denied,
        error);
    const fs::directory_iterator end;
    std::size_t entries_examined = 0;
    for (; !error && entry != end &&
           entries_examined < codex::k_max_codex_profile_scan_entries &&
           profiles.size() < addon::limits::k_max_discovery_candidates;
         entry.increment(error))
    {
        ++entries_examined;
        const std::string name = path_text(entry->path().filename());
        if (!addon::lower(name).starts_with(".codex") ||
            !entry->is_directory(error) || error)
        {
            error.clear();
            continue;
        }
        append_if_authenticated(entry->path());
    }
    if (out_profile_stats) {
        out_profile_stats->entries_examined = entries_examined;
    }

    std::ranges::sort(
        profiles,
        [&selected_home](const fs::path& left, const fs::path& right) {
            std::error_code left_error;
            std::error_code right_error;
            const bool left_selected = fs::equivalent(
                left, selected_home.identity_path, left_error) && !left_error;
            const bool right_selected = fs::equivalent(
                right, selected_home.identity_path, right_error) && !right_error;
            if (left_selected != right_selected) {
                return left_selected;
            }
            return path_text(left.filename()) < path_text(right.filename());
        });
    if (out_profile_stats) {
        out_profile_stats->profiles_retained = profiles.size();
    }
    return profiles;
}

addon::Launcher_candidate generic_codex_candidate(
    const std::string& program, const std::string& runner_key)
{
    return {
        "interactive", "Codex", program, runner_key,
        "", true, addon::Json_value::object(),
    };
}

bool package_flag(std::string_view value)
{
    return value == "-y" || value == "--yes" || value == "--quiet" ||
        value == "--silent" || value == "--no-install" || value == "--bun";
}

// Whether Codex's argument parser (clap) takes the argument after an option as
// that option's value. None of the session options allows hyphen values, so
// "--" ends the options and anything else starting with '-' is parsed as an
// option, leaving the first option without its value; a lone "-" is a value.
bool separate_option_value(std::string_view argument)
{
    return !argument.empty() && (argument == "-" || argument.front() != '-');
}

std::optional<Launcher_command> recognize(std::string_view script, std::string_view runner, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = addon::trim(line, Launcher_whitespace::ASCII);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : addon::launcher_segments(line, runner, Launcher_whitespace::ASCII)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = addon::environment_assignment(
                segment.substr(4), Launcher_whitespace::ASCII);
            else if (normalized.starts_with("export ")) standalone = addon::environment_assignment(
                segment.substr(7), Launcher_whitespace::ASCII);
            else if (normalized.starts_with("$env:")) standalone = addon::environment_assignment(
                segment.substr(5), Launcher_whitespace::ASCII);
            if (standalone) {
                inherited.push_back(*standalone);
                continue;
            }
            Environment environment = inherited;
            // Leading assignment values may contain quoted whitespace, unlike
            // the command-token grammar used for the remainder of the line.
            static const std::regex leading(R"re(^([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s]*)\s*)re");
            std::smatch match;
            while (std::regex_search(segment, match, leading)) {
                environment.emplace_back(match[1].str(), addon::unquote(match[2].str(), Launcher_whitespace::ASCII));
                segment.erase(0, match.length());
            }
            const auto words = addon::launcher_tokens(segment, runner);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = addon::environment_assignment(words[index], Launcher_whitespace::ASCII);
                const auto prefix = lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                if (value) environment.push_back(*value);
                else if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            const std::string program = addon::unquote(words[index], Launcher_whitespace::ASCII);
            const std::string base = addon::base_name(program, Launcher_whitespace::ASCII);
            if (is_codex_program(program)) return Launcher_command{program, {}, environment};
            if (base == "npx" || base == "bunx") {
                std::vector<std::string> arguments;
                for (++index; index < words.size(); ++index) {
                    const auto argument = addon::unquote(words[index], Launcher_whitespace::ASCII);
                    arguments.push_back(argument);
                    if (argument.starts_with('-')) {
                        if (!package_flag(argument)) break;
                    }
                    else {
                        const auto package = lower(argument);
                        if (package == "@openai/codex" || package.starts_with("@openai/codex@")) {
                            return Launcher_command{program, arguments, environment};
                        }
                        break;
                    }
                }
            }
            else if (base == "cmd" || base == "powershell" || base == "pwsh" ||
                     base == "bash" || base == "sh")
            {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                    if (argument != "/c" && argument != "-c" && argument != "-command") continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    auto command = recognize(addon::unquote(tail, Launcher_whitespace::ASCII),
                        base == "cmd" ? "cmd" : base == "bash" || base == "sh" ? "bash" : "powershell", depth - 1);
                    if (command) {
                        command->environment.insert(command->environment.begin(), environment.begin(), environment.end());
                        return command;
                    }
                    break;
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<Launcher_command> recognize_launcher(std::string_view script, std::string_view runner)
{
    return recognize(script, runner, 8);
}

std::optional<std::string> codex_profile_environment_text(std::string_view profile_path)
{
    return codex_profile_environment_text_impl(profile_path);
}

std::vector<addon::Launcher_candidate> discover_launchers(
    const addon::Discover_request& request)
{
    return discover_launchers(request, nullptr);
}

std::vector<addon::Launcher_candidate> discover_launchers(
    const addon::Discover_request& request,
    codex::Codex_profile_discovery_stats* out_profile_stats)
{
    if (out_profile_stats) {
        *out_profile_stats = {};
    }
    Environment environment;
    for (const auto& member : request.ambient_environment.as_object()) {
        environment.emplace_back(member.name, member.value.as_string());
    }
    std::string program = "codex-pet";
    auto selected = resolve_program(program, environment, {}, Launcher_whitespace::ASCII);
    if (!selected) {
        program = "codex";
        selected = resolve_program(program, environment, {}, Launcher_whitespace::ASCII);
        if (!selected) {
            return {};
        }
    }
    const auto runner_key = addon::preferred_discovery_runner(
        *selected,
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct);

    const auto selected_home = configured_codex_home(request);
    const std::vector<fs::path> profiles = selected_home
        ? authenticated_codex_profiles(*selected_home, out_profile_stats)
        : std::vector<fs::path>{};
    if (profiles.empty()) {
        return {generic_codex_candidate(program, runner_key)};
    }

    std::vector<addon::Launcher_candidate> candidates;
    candidates.reserve(profiles.size());
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (candidates.size() == addon::limits::k_max_discovery_candidates) {
            break;
        }
        const std::string profile = path_text(profiles[index]);
        const std::optional<std::string> environment_text =
            codex_profile_environment_text(profile);
        if (!environment_text) {
            continue;
        }
        std::error_code selected_error;
        const bool is_selected = selected_home && fs::equivalent(
            profiles[index], selected_home->identity_path, selected_error) &&
            !selected_error;
        candidates.push_back({
            "codex_profile_" + std::to_string(index + 1),
            profile_display_name(profiles[index]),
            program,
            runner_key,
            *environment_text,
            is_selected,
            addon::Json_value::object(),
        });
    }
    return candidates;
}

bool version_is_at_least(std::string_view version, int major, int minor, int patch)
{
    if (!addon::is_semantic_version(version)) return false;
    std::array<std::string_view, 3> components;
    auto remaining = version.substr(0, version.find_first_of("-+"));
    for (auto& component : components) {
        const auto separator = remaining.find('.');
        component = remaining.substr(0, separator);
        if (separator != std::string_view::npos) remaining.remove_prefix(separator + 1);
    }
    const std::array minima{major, minor, patch};
    for (std::size_t index = 0; index < minima.size(); ++index) {
        const auto minimum = std::to_string(minima[index]);
        if (components[index].size() != minimum.size()) return components[index].size() > minimum.size();
        if (components[index] != minimum) return components[index] > minimum;
    }
    const auto suffix = version.find_first_of("-+");
    return suffix == std::string_view::npos || version[suffix] != '-';
}

std::string parse_version_output(std::string_view output, std::string_view errors)
{
    constexpr std::string_view labels[] = {"codex-cli", "codex"};
    const auto first  = addon::parse_version_stream(output, labels, Launcher_whitespace::ASCII);
    const auto second = addon::parse_version_stream(errors, labels, Launcher_whitespace::ASCII);
    if (!first || !second || (!first->empty() && !second->empty() && *first != *second)) return {};
    return first->empty() ? *second : *first;
}

std::optional<addon::Probe_output> run_probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, addon::Invocation_context& context, addon::Probe_exchange* exchange)
{
    return addon::probe(argv, environment, working_directory, timeout_ms, context, exchange);
}

std::optional<addon::Probe_output> probe_cli_options(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, addon::Invocation_context& context, bool* out_timed_out)
{
    // Even --version and --help start the native binary through the selected
    // launcher. The host cancels the whole preparation at its original deadline.
    const auto timeout_ms = std::min<std::int64_t>(
        context.request().deadline_ms, addon::limits::k_launch_prepare_deadline_ms);
    bool timed_out;
    return addon::probe_with_timeout_status(argv, environment, working_directory,
        static_cast<int>(timeout_ms), context, out_timed_out ? *out_timed_out : timed_out);
}

std::optional<std::vector<std::string>> resolve_probe_launcher(
    const Launcher_command& command, const std::vector<std::string>& operation_arguments,
    const addon::Json_value& effective_environment, std::string_view working_directory,
    [[maybe_unused]] bool direct_batch)
{
    Environment environment;
    for (const auto& member : effective_environment.as_object()) {
        set_environment(environment, member.name, member.value.as_string());
    }
    // Script prefixes have already been applied to the frozen environment.
    // Recognizer values retain shell spelling and cannot replace that result.
    const auto program = resolve_program(
        command.program, environment, working_directory, Launcher_whitespace::ASCII);
    if (!program) return std::nullopt;
    std::vector<std::string> result{path_to_utf8(*program)};
    result.insert(result.end(), command.prefix_arguments.begin(), command.prefix_arguments.end());
    result.insert(result.end(), operation_arguments.begin(), operation_arguments.end());
#ifdef _WIN32
    const auto extension = lower(program->extension().string());
    if (direct_batch && (extension == ".cmd" || extension == ".bat") &&
        std::ranges::none_of(result, [](const std::string& argument) {
            return argument.find_first_of("\"\r\n") != std::string::npos;
        }))
    {
        // Embedded quotes require PowerShell's native argument binder. The
        // quota helper can reproduce its plain batch command line directly.
        result.insert(result.begin(), "--batch");
        return result;
    }
    if (extension == ".cmd" || extension == ".bat" || extension == ".ps1") {
        const auto shell = resolve_program(
            "powershell.exe", environment, working_directory, Launcher_whitespace::ASCII);
        if (!shell || !native_image(*shell)) return std::nullopt;
        std::string script = "&";
        for (const auto& argument : result) {
            script += " '";
            for (const char character : argument) {
                script += character;
                if (character == '\'') script += '\'';
            }
            script += '\'';
        }
        script += "; exit $LASTEXITCODE";
        return std::vector<std::string>{path_to_utf8(*shell), "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script};
    }
    if (!native_image(*program)) return std::nullopt;
#endif
    return result;
}

bool supported_session_arguments(const std::vector<std::string>& argv, bool resume, std::string* approval_policy)
{
    if (approval_policy) {
        approval_policy->clear();
    }
    if (argv.empty() || !is_codex_program(argv.front())) {
        return false;
    }
    std::vector<std::string> values{
        "-c", "--config", "--enable", "--disable", "-m", "--model", "--local-provider", "-p", "--profile",
        "-s", "--sandbox", "-a", "--ask-for-approval"};
    if (!resume) {
        values.insert(values.end(), {"-i", "--image", "--add-dir"});
    }
    const std::vector<std::string> switches{
        "--oss", "--yolo", "--dangerously-bypass-approvals-and-sandbox", "--search", "--no-alt-screen"};
    // Codex's parser (clap) rejects a second occurrence of a flag or of a
    // single-value option; configuration, feature and attachment options
    // accumulate.
    const std::vector<std::string> repeatable{"-c", "--config", "--enable", "--disable", "-i", "--image", "--add-dir"};
    std::vector<std::string> seen;
    std::string approval;
    bool bypass = false;
    const auto repeated = [&](const std::string& option) {
        auto name = option;
        if (name == "-m") { name = "--model"; } else
        if (name == "-p") { name = "--profile"; } else
        if (name == "-s") { name = "--sandbox"; } else
        if (name == "-a") { name = "--ask-for-approval"; } else
        if (name == "--dangerously-bypass-approvals-and-sandbox") { name = "--yolo"; }

        if (std::find(repeatable.begin(), repeatable.end(), name) != repeatable.end()) {
            return false;
        }
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
            return true;
        }
        seen.push_back(name);
        return false;
    };
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& argument = argv[index];
        const auto equal = argument.find('=');
        auto key = argument.substr(0, equal);
        const bool compact_permission = argument.size() > 2 && argument[0] == '-' &&
            (argument[1] == 's' || argument[1] == 'a') && argument[2] != '=';
        if (compact_permission) {
            key = argument.substr(0, 2);
        }
        if (std::find(values.begin(), values.end(), key) != values.end()) {
            if (repeated(key)) {
                return false;
            }
            std::string_view value;
            if (compact_permission) {
                value = std::string_view(argument).substr(2);
            }
            else
            if (equal != std::string::npos) {
                if (equal + 1 == argument.size()) {
                    return false;
                }
                value = std::string_view(argument).substr(equal + 1);
            }
            else {
                if (++index == argv.size() || !separate_option_value(argv[index])) {
                    return false;
                }
                value = argv[index];
            }
            if (key == "-s" || key == "--sandbox") {
                if (value != "read-only" && value != "workspace-write" && value != "danger-full-access") {
                    return false;
                }
            }
            else
            if (key == "-a" || key == "--ask-for-approval") {
                if (value != "on-request" && value != "never") {
                    return false;
                }
                approval = value;
            }
            continue;
        }
        if (resume && argument.starts_with("-c") && !argument.starts_with("--") && argument.size() > 2) {
            continue;
        }
        if (std::find(switches.begin(), switches.end(), argument) == switches.end() || repeated(argument)) {
            return false;
        }
        if (argument == "--yolo" || argument == "--dangerously-bypass-approvals-and-sandbox") {
            bypass = true;
        }
    }
    // Native clap rejects an explicit approval policy together with bypass.
    // A sandbox flag alone may coexist with bypass; Codex owns its precedence.
    if (bypass && !approval.empty()) {
        return false;
    }
    if (approval_policy) {
        *approval_policy = std::move(approval);
    }
    return true;
}

namespace {

std::optional<addon::Json_value> object_file(const fs::path& file)
{
    std::error_code error;
    if (fs::file_size(file, error) > 1024 * 1024 || error) {
        return std::nullopt;
    }
    std::ifstream input(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    const auto result = addon::parse_json(bytes, {}, nullptr);
    return result && result->is_object() ? result : std::nullopt;
}

std::string member_text(const addon::Json_value& object, std::string_view key)
{
    const auto* value = object.find(key);
    return value && value->is_string() ? value->as_string() : std::string{};
}

std::optional<std::pair<std::string, std::string>> native_target(
    const fs::path& file, [[maybe_unused]] std::string_view preferred_target = {})
{
    std::ifstream input(file, std::ios::binary);
#ifdef _WIN32
    std::array<unsigned char, 64> dos{};
    input.read(reinterpret_cast<char*>(dos.data()), dos.size());
    if (!input || dos[0] != 'M' || dos[1] != 'Z') {
        return std::nullopt;
    }
    const std::uint32_t offset = dos[60] | (std::uint32_t(dos[61]) << 8) |
        (std::uint32_t(dos[62]) << 16) | (std::uint32_t(dos[63]) << 24);
    if (offset > 16 * 1024 * 1024) {
        return std::nullopt;
    }
    input.seekg(offset);
    std::array<unsigned char, 6> pe{};
    input.read(reinterpret_cast<char*>(pe.data()), pe.size());
    if (!input || pe[0] != 'P' || pe[1] != 'E' || pe[2] || pe[3]) {
        return std::nullopt;
    }
    const unsigned machine = pe[4] | (unsigned(pe[5]) << 8);
    if (machine == 0x8664) {
        return std::pair{"win32-x64", "x86_64-pc-windows-msvc"};
    }
    if (machine == 0xaa64) {
        return std::pair{"win32-arm64", "aarch64-pc-windows-msvc"};
    }
#elif defined(__linux__)
    std::array<unsigned char, 20> elf{};
    input.read(reinterpret_cast<char*>(elf.data()), elf.size());
    if (!input || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
        elf[4] != 2 || elf[5] != 1)
    {
        return std::nullopt;
    }
    const unsigned machine = elf[18] | (unsigned(elf[19]) << 8);
    if (machine == 0x3e) {
        return std::pair{"linux-x64", "x86_64-unknown-linux-musl"};
    }
    if (machine == 0xb7) {
        return std::pair{"linux-arm64", "aarch64-unknown-linux-musl"};
    }
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    auto preferred = detail::Macho_architecture::ARM64;
#else
    auto preferred = detail::Macho_architecture::X86_64;
#endif
    if (preferred_target == "darwin-x64") {
        preferred = detail::Macho_architecture::X86_64;
    }
    else
    if (preferred_target == "darwin-arm64") {
        preferred = detail::Macho_architecture::ARM64;
    }
    const auto architecture = detail::macho_target(input, preferred);
    if (architecture == detail::Macho_architecture::ARM64) {
        return std::pair{"darwin-arm64", "aarch64-apple-darwin"};
    }
    if (architecture == detail::Macho_architecture::X86_64) {
        return std::pair{"darwin-x64", "x86_64-apple-darwin"};
    }
#endif
    return std::nullopt;
}

} // namespace

std::optional<Verified_launch> verified_launch(
    const addon::Launcher_request& launcher, const std::vector<std::string>& launcher_argv,
    const addon::Json_value& effective_environment, std::string_view working_directory,
    bool delegated, addon::Invocation_context& context, addon::Contract_failure* out_failure)
{
    const auto refuse = [&](const char* reason, std::string message) {
        if (out_failure) *out_failure = {reason, std::move(message)};
        return std::nullopt;
    };
    const auto unsupported = [&] {
        return refuse("launcher_command_unsupported", "Logonomic could not verify the saved Codex launcher command.");
    };
    const auto recognized = recognize_launcher(launcher.script_body, launcher.runner_key);
    if (!recognized || !recognized->prefix_arguments.empty() || launcher_argv.empty()) {
        return unsupported();
    }
    std::vector<std::string> literal;
    if (launcher.runner_key == addon::k_runner_direct) {
        literal = launcher_argv;
    }
    else
    if (launcher.runner_key == addon::k_runner_powershell && launcher_argv.size() == 4 &&
        addon::base_name(launcher_argv[0], Launcher_whitespace::ASCII) == "powershell" && launcher_argv[1] == "-NoLogo" &&
        launcher_argv[2] == "-Command" && launcher_argv[3] == launcher.script_body)
    {
        for (const unsigned char character : launcher.script_body) {
            if (!std::isalnum(character) && std::string_view(" \t-_./\\:=+").find(character) == std::string_view::npos &&
                !(delegated && character == '"'))
            {
                return unsupported();
            }
        }
        literal = addon::literal_argv(launcher.script_body);
    }
    else {
        return unsupported();
    }
    if (literal.empty() || !is_codex_program(literal.front()) ||
        (delegated && !supported_session_arguments(literal, false)) || !effective_environment.is_object())
    {
        return unsupported();
    }
    Environment environment;
    for (const auto& member : effective_environment.as_object()) {
        if (!member.value.is_string()) {
            return unsupported();
        }
        set_environment(environment, member.name, member.value.as_string());
    }
    const bool pet = addon::base_name(literal.front(), Launcher_whitespace::ASCII) == "codex-pet";
    const std::string name = pet ? "codex-pet" : "Codex";
    const auto installation_unverified = [&] {
        return refuse("launcher_installation_unverified", "Logonomic found " + name +
            " but could not verify its installation. Check that the selected launcher and its dependencies are installed.");
    };
    const auto selected = resolve_program(
        literal.front(), environment, working_directory, Launcher_whitespace::ASCII);
    if (!selected) {
        return refuse("launcher_program_not_found", "Logonomic could not find an executable for " + name +
            ". Check the saved launcher path and its PATH setting.");
    }
    std::vector<fs::path> files{*selected};
    std::vector<std::string> prefix{path_to_utf8(*selected)};
    Environment updates;
    std::string package_version;
#ifdef _WIN32
    const bool npm = lower(selected->extension().string()) == ".cmd";
#else
    const bool npm = pet && selected->filename() == "codex.js";
#endif
    if (npm) {
        const auto directory = selected->parent_path();
        const auto package_name = pet ? "@imakris/codex-pet" : "@openai/codex";
#ifdef _WIN32
        const auto root = directory / "node_modules" / package_name;
#else
        const auto root = directory.parent_path();
#endif
        const auto javascript = regular_file(root / "bin" / "codex.js");
        const auto manifest   = regular_file(root / "package.json");
        if (!javascript || !manifest) {
            return installation_unverified();
        }
#ifdef _WIN32
        auto node = regular_file(directory / "node.exe");
        if (!fs::exists(directory / "node.exe")) {
            std::string extensions;
            std::istringstream entries(environment_value(environment, "PATHEXT"));
            for (std::string extension; std::getline(entries, extension, ';');) {
                extension = addon::trim(extension, Launcher_whitespace::ASCII);
                if (lower(extension) == ".js" || extension.empty()) {
                    continue;
                }
                if (!extensions.empty()) {
                    extensions += ';';
                }
                extensions += extension;
            }
            if (environment_value(environment, "PATHEXT").empty()) {
                extensions = ".COM;.EXE;.BAT;.CMD";
            }
            set_environment(environment, "PATHEXT", extensions);
            updates.emplace_back("PATHEXT", extensions);
            node = resolve_program("node", environment, working_directory, Launcher_whitespace::ASCII);
        }
#else
        const auto node = resolve_program("node", environment, working_directory, Launcher_whitespace::ASCII);
#endif
        const auto target = node ? native_target(*node) : std::nullopt;
        const auto package = object_file(*manifest);
        if (!target || !package) {
            return installation_unverified();
        }
#ifdef _WIN32
        if (lower(node->extension().string()) != ".exe") {
            return installation_unverified();
        }
#endif
        package_version = member_text(*package, "version");
        const auto* bin = package->find("bin");
        const std::string bin_path = bin && bin->is_object()
            ? member_text(*bin, pet ? "codex-pet" : "codex")
            : member_text(*package, "bin");
        if (!addon::is_semantic_version(package_version) || member_text(*package, "name") != package_name ||
            addon::path_from_utf8(bin_path).lexically_normal() != fs::path("bin/codex.js"))
        {
            return installation_unverified();
        }
        fs::path native_root = root;
        if (pet) {
            // The fork bundles the native payload and gives only its npm
            // package a pet build suffix; the CLI reports the native release.
            package_version = package_version.substr(0, package_version.find('-'));
        }
        else {
            const std::string platform_name = "@openai/codex-" + target->first;
            const std::string native_version = package_version + "-" + target->first;
            const auto* dependencies = package->find("optionalDependencies");
            if (!dependencies || member_text(*dependencies, platform_name) != "npm:@openai/codex@" + native_version) {
                return installation_unverified();
            }
            std::optional<fs::path> platform_manifest;
            for (auto cursor = javascript->parent_path(); !cursor.empty();) {
                platform_manifest = regular_file(cursor / "node_modules" / addon::path_from_utf8(platform_name) / "package.json");
                if (platform_manifest || cursor == cursor.parent_path()) {
                    break;
                }
                cursor = cursor.parent_path();
            }
            const auto platform = platform_manifest ? object_file(*platform_manifest) : std::nullopt;
            if (!platform || member_text(*platform, "name") != "@openai/codex" || member_text(*platform, "version") != native_version) {
                return installation_unverified();
            }
            native_root = platform_manifest->parent_path();
            files.push_back(*platform_manifest);
        }
#ifdef _WIN32
        constexpr auto image_name = "codex.exe";
#else
        constexpr auto image_name = "codex";
#endif
        const auto image = regular_file(native_root / "vendor" / target->second / "bin" / image_name);
        if (!image || native_target(*image, target->first) != target) {
            return installation_unverified();
        }
        prefix = {path_to_utf8(*node), path_to_utf8(*javascript)};
        files.insert(files.end(), {*node, *javascript, *manifest, *image});
    }
    else
    if (!native_image(*selected)) {
        return installation_unverified();
    }
    std::vector<std::string> probe_argv = prefix;
    probe_argv.push_back("--version");
    std::vector<std::string> file_names;
    for (const auto& file : files) {
        file_names.push_back(path_to_utf8(file));
    }
    const auto evidence = file_evidence(file_names);
    if (!evidence) {
        return installation_unverified();
    }
    addon::Sha256 digest;
    const auto append = [&](std::string_view value) {
        digest.update(std::to_string(value.size()));
        digest.update(":");
        digest.update(value);
    };
    append(working_directory);
    for (const auto& argument : probe_argv) {
        append(argument);
    }
    for (const auto& [name, value] : environment) {
        append(name);
        append(value);
    }
    append(addon::serialize_json_canonical(*evidence));
    const auto key = addon::to_hex(digest.finish());
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    std::string version;
    {
        std::lock_guard lock(cache_mutex);
        const auto found = cache.find(key);
        if (found != cache.end()) {
            version = found->second;
        }
    }
    if (version.empty()) {
        if (context.cancel_requested()) return refuse("cancelled", {});
        bool timed_out = false;
        const auto output = probe_cli_options(probe_argv, environment, working_directory, context, &timed_out);
        if (!output) {
            if (timed_out) {
                return refuse("launcher_version_timeout", "Logonomic found " + name +
                    ", but its version check (--version) timed out. Retry the launch.");
            }
            if (context.cancel_requested()) {
                return refuse(addon::reason::k_launcher_version_cancelled,
                    "Preparation stopped while checking the version of " + name + ".");
            }
            return refuse("launcher_version_unavailable", "Logonomic found " + name +
                ", but could not complete its version check (--version).");
        }
        version = parse_version_output(output->output, output->errors);
        if (version.empty() || (!package_version.empty() && version != package_version)) {
            return refuse("launcher_version_invalid", "Logonomic found " + name +
                ", but its version output did not identify the expected Codex installation.");
        }
        if (file_evidence(file_names) != evidence) {
            return installation_unverified();
        }
        std::lock_guard lock(cache_mutex);
        if (cache.size() >= 32) {
            cache.erase(cache.begin());
        }
        cache[key] = version;
    }
    if (context.cancel_requested()) return refuse("cancelled", {});
    if (delegated && !version_is_at_least(version, 0, 150, 1)) return unsupported();
    const std::size_t prefix_size = prefix.size();
    prefix.insert(prefix.end(), literal.begin() + 1, literal.end());
    return Verified_launch{std::move(prefix), std::move(version), std::move(updates), std::move(file_names), prefix_size};
}

std::optional<addon::Json_value> file_evidence(const std::vector<std::string>& files)
{
    if (files.empty()) {
        return std::nullopt;
    }
    auto result = addon::Json_value::array();
    for (const auto& name : files) {
        const auto path = addon::path_from_utf8(name);
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error) {
            return std::nullopt;
        }
        const auto modified = fs::last_write_time(path, error);
        if (error) {
            return std::nullopt;
        }
        auto file = addon::Json_value::object();
        file.set("path",     addon::Json_value(name));
        file.set("size",     addon::Json_value(std::to_string(size)));
        file.set("modified", addon::Json_value(addon::file_time_ticks_text(modified)));
        result.push_back(std::move(file));
    }
    return result;
}

} // namespace codex
