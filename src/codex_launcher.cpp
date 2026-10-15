#include "codex_launcher.h"

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
    const addon::Discover_request& request,
    const Environment& environment)
{
    std::string configured = addon::environment_value(environment, "CODEX_HOME");
    if (configured.empty()) {
        const bool windows = request.platform_id.starts_with("windows-");
        std::string home = addon::environment_value(
            environment, windows ? "USERPROFILE" : "HOME");
        if (home.empty()) {
            home = request.default_home_fallback;
        }
        if (home.empty()) {
            return std::nullopt;
        }
        configured = path_text(addon::path_from_utf8(home) / ".codex");
    }

    fs::path path = addon::path_from_utf8(configured);
    if (!path.is_absolute()) {
        return std::nullopt;
    }
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

addon::Launcher_candidate generic_codex_candidate(const addon::Discover_request& request)
{
    return {
        "interactive", "Codex", "codex",
        request.platform_id == "windows-x86_64"
            ? addon::k_runner_powershell
            : addon::k_runner_direct,
        "", true, addon::Json_value::object(),
    };
}

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last  = value.find_last_not_of(" \t\r\n");
    return first == std::string_view::npos
        ? std::string{} : std::string(value.substr(first, last - first + 1));
}

std::string unquote(std::string_view value)
{
    std::string result = trim(value);
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') ||
         (result.front() == '"'  && result.back() == '"')))
    {
        return result.substr(1, result.size() - 2);
    }
    return result;
}

std::string base_name(std::string_view value)
{
    std::string name = lower(unquote(value));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name.erase(0, separator + 1);
    for (const std::string_view suffix : {".exe", ".cmd", ".ps1", ".bat"}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
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

bool environment_name(std::string_view name)
{
    if (name.empty() || (name.front() != '_' &&
        !std::isalpha(static_cast<unsigned char>(name.front())))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character == '_' || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
    });
}

std::optional<std::pair<std::string, std::string>> assignment(std::string_view text)
{
    const std::string value = unquote(text);
    const auto separator = value.find('=');
    if (separator == std::string::npos) return std::nullopt;
    const std::string name = trim(value.substr(0, separator));
    if (!environment_name(name) || value.find('\0') != std::string::npos) return std::nullopt;
    return std::pair{name, unquote(value.substr(separator + 1))};
}

std::vector<std::string> tokens(std::string_view text)
{
    static const std::regex pattern(R"re("(?:\\.|[^"])*"|'[^']*'|\S+)re");
    const std::string input(text);
    std::vector<std::string> result;
    for (std::sregex_iterator match(input.begin(), input.end(), pattern), end;
         match != end; ++match)
    {
        result.push_back(match->str());
    }
    return result;
}

std::vector<std::string> segments(std::string_view line)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '\\' && quote == '"' && index + 1 < line.size()) {
            current += character;
            current += line[++index];
            continue;
        }
        if (character == quote) quote = 0;
        else if (!quote && (character == '\'' || character == '"')) quote = character;
        else if (!quote && (character == ';' || character == '&' || character == '|')) {
            if (!trim(current).empty()) result.push_back(trim(current));
            current.clear();
            continue;
        }
        current += character;
    }
    if (!trim(current).empty()) result.push_back(trim(current));
    return result;
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : segments(line)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = assignment(segment.substr(4));
            else if (normalized.starts_with("export ")) standalone = assignment(segment.substr(7));
            else if (normalized.starts_with("$env:")) standalone = assignment(segment.substr(5));
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
                environment.emplace_back(match[1].str(), unquote(match[2].str()));
                segment.erase(0, match.length());
            }
            const auto words = tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = assignment(words[index]);
                const auto prefix = lower(unquote(words[index]));
                if (value) environment.push_back(*value);
                else if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            const std::string program = unquote(words[index]);
            const std::string base = base_name(program);
            if (base == "codex" || base.starts_with("codex@")) return Launcher_command{program, {}, environment};
            if (base == "npx" || base == "bunx") {
                std::vector<std::string> arguments;
                for (++index; index < words.size(); ++index) {
                    const auto argument = unquote(words[index]);
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
                    const auto argument = lower(unquote(words[index]));
                    if (argument != "/c" && argument != "-c" && argument != "-command") continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    auto command = recognize(unquote(tail), depth - 1);
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

const std::string& semver_pattern()
{
    static const std::string pattern =
        R"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        R"((?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?)"
        R"((?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)";
    return pattern;
}

bool semver(std::string_view value)
{
    static const std::regex pattern("^" + semver_pattern() + "$", std::regex::ECMAScript);
    return std::regex_match(value.begin(), value.end(), pattern);
}

std::optional<std::string> stream_version(std::string_view value)
{
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos ||
        !addon::is_valid_utf8(value)) return std::nullopt;
    static const std::regex labelled("^(?:codex-cli|codex)(?:\\s+version)?(?:\\s*[:=]\\s*|\\s+)v?(" +
        semver_pattern() + ")(?:\\s+\\([^\\r\\n()]{1,128}\\))?\\s*$", std::regex::icase);
    std::vector<std::string> lines;
    std::string text(value);
    std::replace(text.begin(), text.end(), '\r', '\n');
    std::istringstream input(text);
    std::string version;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty()) continue;
        lines.push_back(line);
        std::smatch match;
        if (std::regex_match(line, match, labelled)) {
            if (!version.empty() && version != match[1].str()) return std::nullopt;
            version = match[1].str();
        }
    }
    if (!version.empty()) return version;
    if (lines.size() == 1) {
        auto candidate = lines.front();
        if (candidate.starts_with('v') || candidate.starts_with('V')) candidate.erase(0, 1);
        if (semver(candidate)) return candidate;
    }
    return std::string{};
}

} // namespace

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    return recognize(script, 8);
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
    if (!resolve_program("codex", environment, {}, Launcher_whitespace::ASCII)) {
        return {};
    }

    const auto selected_home = configured_codex_home(request, environment);
    const std::vector<fs::path> profiles = selected_home
        ? authenticated_codex_profiles(*selected_home, out_profile_stats)
        : std::vector<fs::path>{};
    if (profiles.empty()) {
        return {generic_codex_candidate(request)};
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
            "codex",
            request.platform_id == "windows-x86_64"
                ? addon::k_runner_powershell
                : addon::k_runner_direct,
            *environment_text,
            is_selected,
            addon::Json_value::object(),
        });
    }
    return candidates;
}

std::vector<std::string> literal_launcher_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    int quotes = 0;
    for (const char character : script) {
        if (character == '"') {
            if (++quotes == 3) { current += '"'; quotes = 0; }
            continue;
        }
        if (quotes == 1) quoted = !quoted;
        quotes = 0;
        if (!quoted && std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        }
        else current += character;
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

bool version_is_at_least(std::string_view version, int major, int minor, int patch)
{
    if (!semver(version)) return false;
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
    const auto first  = stream_version(output);
    const auto second = stream_version(errors);
    if (!first || !second || (!first->empty() && !second->empty() && *first != *second)) return {};
    return first->empty() ? *second : *first;
}

std::optional<addon::Probe_output> run_probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, addon::Invocation_context& context, addon::Probe_exchange* exchange)
{
    return addon::probe_exchange(argv, environment, working_directory, timeout_ms, context,
        exchange, "\\\\.\\pipe\\logonomic-codex-probe-");
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
    for (const auto& entry : command.environment) set_environment(environment, entry.first, entry.second);
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

bool supported_session_arguments(const std::vector<std::string>& argv, bool resume)
{
    if (argv.empty() || (base_name(argv.front()) != "codex" && !base_name(argv.front()).starts_with("codex@"))) {
        return false;
    }
    std::vector<std::string> values{
        "-c", "--config", "--enable", "--disable", "-m", "--model", "--local-provider", "-p", "--profile"};
    if (!resume) {
        values.insert(values.end(), {"-i", "--image", "--add-dir"});
    }
    const std::vector<std::string> switches{"--oss", "--yolo", "--search", "--no-alt-screen"};
    // Codex's parser (clap) rejects a second occurrence of a flag or of a
    // single-value option; configuration, feature and attachment options
    // accumulate.
    const std::vector<std::string> repeatable{"-c", "--config", "--enable", "--disable", "-i", "--image", "--add-dir"};
    std::vector<std::string> seen;
    const auto repeated = [&](const std::string& option) {
        const auto name = option == "-m" ? "--model" : option == "-p" ? "--profile" : option;
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
        const auto key = argument.substr(0, equal);
        if (std::find(values.begin(), values.end(), key) != values.end()) {
            if (repeated(key)) {
                return false;
            }
            if (equal != std::string::npos) {
                if (equal + 1 == argument.size()) {
                    return false;
                }
            }
            else
            if (++index == argv.size() || !separate_option_value(argv[index])) {
                return false;
            }
            continue;
        }
        if (resume && argument.starts_with("-c") && !argument.starts_with("--") && argument.size() > 2) {
            continue;
        }
        if (std::find(switches.begin(), switches.end(), argument) == switches.end() || repeated(argument)) {
            return false;
        }
    }
    return true;
}

namespace {

#ifdef _WIN32
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

std::optional<std::pair<std::string, std::string>> native_target(const fs::path& file)
{
    std::ifstream input(file, std::ios::binary);
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
    return std::nullopt;
}
#endif

} // namespace

std::optional<Verified_launch> verified_launch(
    const addon::Launcher_request& launcher, const std::vector<std::string>& launcher_argv,
    const addon::Json_value& effective_environment, std::string_view working_directory,
    bool delegated, addon::Invocation_context& context)
{
    const auto recognized = recognize_launcher(launcher.script_body);
    if (!recognized || !recognized->prefix_arguments.empty() || launcher_argv.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> literal;
    if (launcher.runner_key == addon::k_runner_direct) {
        literal = launcher_argv;
    }
    else
    if (launcher.runner_key == addon::k_runner_powershell && launcher_argv.size() == 4 &&
        base_name(launcher_argv[0]) == "powershell" && launcher_argv[1] == "-NoLogo" &&
        launcher_argv[2] == "-Command" && launcher_argv[3] == launcher.script_body)
    {
        for (const unsigned char character : launcher.script_body) {
            if (!std::isalnum(character) && std::string_view(" \t-_./\\:=+").find(character) == std::string_view::npos &&
                !(delegated && character == '"'))
            {
                return std::nullopt;
            }
        }
        literal = literal_launcher_argv(launcher.script_body);
    }
    else {
        return std::nullopt;
    }
    if (literal.empty() || (base_name(literal.front()) != "codex" && !base_name(literal.front()).starts_with("codex@")) ||
        (delegated && !supported_session_arguments(literal, false)) || !effective_environment.is_object())
    {
        return std::nullopt;
    }
    Environment environment;
    for (const auto& member : effective_environment.as_object()) {
        if (!member.value.is_string()) {
            return std::nullopt;
        }
        set_environment(environment, member.name, member.value.as_string());
    }
    const auto selected = resolve_program(
        literal.front(), environment, working_directory, Launcher_whitespace::ASCII);
    if (!selected) {
        return std::nullopt;
    }
    std::vector<fs::path> files{*selected};
    std::vector<std::string> prefix{path_to_utf8(*selected)};
    Environment updates;
    std::string package_version;
    if (lower(selected->extension().string()) == ".cmd") {
#ifdef _WIN32
        const auto directory = selected->parent_path();
        const auto root = directory / "node_modules" / "@openai" / "codex";
        const auto javascript = regular_file(root / "bin" / "codex.js");
        const auto manifest   = regular_file(root / "package.json");
        if (!javascript || !manifest) {
            return std::nullopt;
        }
        auto node = regular_file(directory / "node.exe");
        if (!fs::exists(directory / "node.exe")) {
            std::string extensions;
            std::istringstream entries(environment_value(environment, "PATHEXT"));
            for (std::string extension; std::getline(entries, extension, ';');) {
                extension = trim(extension);
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
        const auto target = node ? native_target(*node) : std::nullopt;
        const auto package = object_file(*manifest);
        if (!target || !package || lower(node->extension().string()) != ".exe") {
            return std::nullopt;
        }
        package_version = member_text(*package, "version");
        const auto* bin = package->find("bin");
        const std::string bin_path = bin && bin->is_object() ? member_text(*bin, "codex") : member_text(*package, "bin");
        const std::string platform_name = "@openai/codex-" + target->first;
        const std::string native_version = package_version + "-" + target->first;
        const auto* dependencies = package->find("optionalDependencies");
        if (!semver(package_version) || member_text(*package, "name") != "@openai/codex" ||
            addon::path_from_utf8(bin_path).lexically_normal() != fs::path("bin/codex.js") ||
            !dependencies || member_text(*dependencies, platform_name) != "npm:@openai/codex@" + native_version)
        {
            return std::nullopt;
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
            return std::nullopt;
        }
        const auto image = regular_file(platform_manifest->parent_path() / "vendor" / target->second / "bin" / "codex.exe");
        if (!image || native_target(*image) != target) {
            return std::nullopt;
        }
        prefix = {path_to_utf8(*node), path_to_utf8(*javascript)};
        files.insert(files.end(), {*node, *javascript, *manifest, *platform_manifest, *image});
#else
        return std::nullopt;
#endif
    }
    else
    if (!native_image(*selected)) {
        return std::nullopt;
    }
    std::vector<std::string> probe_argv = prefix;
    probe_argv.push_back("--version");
    std::vector<std::pair<std::uintmax_t, fs::file_time_type>> identities;
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
    std::vector<std::string> file_names;
    for (const auto& file : files) {
        std::error_code error;
        const auto size = fs::file_size(file, error);
        if (error) {
            return std::nullopt;
        }
        const auto modified = fs::last_write_time(file, error);
        if (error) {
            return std::nullopt;
        }
        identities.emplace_back(size, modified);
        file_names.push_back(path_to_utf8(file));
        append(file_names.back());
        append(std::to_string(size));
        append(std::to_string(modified.time_since_epoch().count()));
    }
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
        const auto output = run_probe(probe_argv, environment, working_directory, 3000, context);
        if (!output) {
            return std::nullopt;
        }
        version = parse_version_output(output->output, output->errors);
        if (version.empty() || (!package_version.empty() && version != package_version)) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < files.size(); ++index) {
            std::error_code error;
            if (fs::file_size(files[index], error) != identities[index].first || error ||
                fs::last_write_time(files[index], error) != identities[index].second || error)
            {
                return std::nullopt;
            }
        }
        std::lock_guard lock(cache_mutex);
        if (cache.size() >= 32) {
            cache.erase(cache.begin());
        }
        cache[key] = version;
    }
    if (context.cancel_requested() || (delegated && !version_is_at_least(version, 0, 150, 1))) {
        return std::nullopt;
    }
    const std::size_t prefix_size = prefix.size();
    prefix.insert(prefix.end(), literal.begin() + 1, literal.end());
    return Verified_launch{std::move(prefix), std::move(version), std::move(updates), std::move(file_names), prefix_size};
}

} // namespace codex
