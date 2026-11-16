#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>

#include "codex_current_account_fixture.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <chrono>
#include <thread>

namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;
constexpr const char* k_session = "019933a1-6e43-7102-a520-b3c8294cc763";

Json object(std::initializer_list<addon::Json_member> values) { return Json(Json::Object(values)); }
std::string text(const Json& value, const char* name) { const auto* item = value.find(name); return item && item->is_string() ? item->as_string() : std::string{}; }

void set_configuration(Json& configuration, std::string_view key, const Json& value)
{
    const auto dot = key.find('.');
    if (dot == std::string_view::npos) {
        configuration.set(std::string(key), value);
        return;
    }
    const auto parent = std::string(key.substr(0, dot));
    if (!configuration.find(parent)) configuration.set(parent, Json::object());
    set_configuration(*configuration.find(parent), key.substr(dot + 1), value);
}

void merge_configuration(Json& configuration, const Json& overlay)
{
    for (const auto& entry : overlay.as_object()) {
        auto* selected = configuration.find(entry.name);
        if (selected && selected->is_object() && entry.value.is_object()) {
            merge_configuration(*selected, entry.value);
        }
        else {
            configuration.set(entry.name, entry.value);
        }
    }
}

std::optional<Json> boolean_table(std::string_view value)
{
    if (!value.starts_with('{') || !value.ends_with('}')) return std::nullopt;
    value.remove_prefix(1);
    value.remove_suffix(1);
    auto result = Json::object();
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto entry = value.substr(0, comma);
        const auto equal = entry.find('=');
        if (equal == std::string_view::npos || equal == 0) return std::nullopt;
        const auto selected = addon::parse_json(entry.substr(equal + 1), {}, nullptr);
        if (!selected || !selected->is_boolean()) return std::nullopt;
        result.set(std::string(entry.substr(0, equal)), *selected);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return result;
}

void send_observation(const std::string& endpoint, const std::string& defect)
{
    const auto colon = endpoint.find(':', 7);
    const auto slash = endpoint.find('/', colon);
    const int port = std::stoi(endpoint.substr(colon + 1, slash - colon - 1));
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
    const SOCKET channel = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (channel == INVALID_SOCKET) { WSACleanup(); return; }
#else
    const int channel = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (channel < 0) return;
#endif
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = htons(static_cast<unsigned short>(port));
    if (connect(channel, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        std::string authorization = std::getenv("OTEL_EXPORTER_OTLP_LOGS_HEADERS");
        authorization.erase(0, std::string("Authorization=").size());
        for (std::size_t at; (at = authorization.find("%20")) != std::string::npos;) authorization.replace(at, 3, " ");
        const std::string body = "{\"resourceLogs\":[{\"scopeLogs\":[{\"logRecords\":[{\"attributes\":[{\"key\":\"event.name\",\"value\":{\"stringValue\":\"codex.conversation_starts\"}},{\"key\":\"conversation.id\",\"value\":{\"stringValue\":\"" + std::string(k_session) + "\"}}]}]}]}]}";
        const std::string content_type = defect == "missing-type" ? "" : "Content-Type: application/json\r\n";
        const std::string request = "POST /v1/logs HTTP/1.1\r\nHost: 127.0.0.1\r\n" + content_type + "Authorization: " + authorization + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body + (defect == "surplus" ? "x" : "");
        send(channel, request.data(), static_cast<int>(request.size()), 0);
        char response[256];
        recv(channel, response, sizeof(response), 0);
    }
#ifdef _WIN32
    closesocket(channel);
    WSACleanup();
#else
    close(channel);
#endif
}

// The arguments as a native Codex binary receives them: on Windows the wide
// command line split by the platform's rules and converted to UTF-8, never
// the code-page argv.
std::vector<std::string> received_arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#ifdef _WIN32
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> result;
    for (int index = 0; arguments && index < count; ++index) {
        const std::wstring_view wide(arguments[index]);
        const int length = static_cast<int>(wide.size());
        const int bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, value.data(), bytes, nullptr, nullptr);
        result.push_back(std::move(value));
    }
    LocalFree(arguments);
    return result;
#else
    return {argv, argv + argc};
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (const char* receipt = std::getenv("CODEX_FIXTURE_RECEIPT")) {
        Json arguments = Json::array();
        for (const auto& argument : received_arguments(argc, argv)) {
            arguments.push_back(Json(argument));
        }
        const auto cwd = std::filesystem::current_path().generic_u8string();
        Json observed = object({
            {"argv", std::move(arguments)},
            {"cwd", Json(std::string(cwd.begin(), cwd.end()))},
        });
        for (const char* name : {"CODEX_FIXTURE_CMDLINE", "CODEX_FIXTURE_WRAPPER", "CODEX_HOME"}) {
            if (const char* value = std::getenv(name)) {
                observed.set(name, Json(value));
            }
        }
        std::ofstream output(addon::path_from_utf8(receipt), std::ios::binary);
        output << addon::serialize_json(observed);
    }
    // Report the invocation instead of acting on it.
    if (std::getenv("CODEX_FIXTURE_ARGV")) {
        Json arguments = Json::array();
        for (auto& argument : received_arguments(argc, argv)) {
            arguments.push_back(Json(std::move(argument)));
        }
        std::cout << addon::serialize_json(arguments) << std::flush;
        return 0;
    }
    std::string endpoint;
    std::string quota_scenario;
    const char* configured_identity = std::getenv("CODEX_FIXTURE_IDENTITY");
    std::string identity_scenario = configured_identity ? configured_identity : "";
    std::string refresh_scenario;
    bool quota_received = false;
    bool initialized = false;
    bool configured_exporter = false;
    auto configuration = Json::object();
    std::string sqlite_home;
    if (const char* value = std::getenv("CODEX_FIXTURE_SQLITE_HOME")) sqlite_home = value;
    // Model an intrinsic producer defect across the isolated native probe.
    // User -c arguments are intentionally omitted by that probe.
    const char* fixture_defect = std::getenv("CODEX_FIXTURE_OBSERVER");
    std::string defect = fixture_defect ? fixture_defect : "";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.starts_with("--fixture-quota=")) quota_scenario = argument.substr(16);
        if (argument.starts_with("--fixture-identity=")) identity_scenario = argument.substr(19);
        if (argument.starts_with("--fixture-refresh=")) {
            refresh_scenario = argument.substr(18);
            identity_scenario = refresh_scenario;
        }
#ifndef _WIN32
        if (argument == "--fixture-custody") { std::cout << getpgrp() << ':' << getsid(0) << '\n'; return 0; }
#endif
        if (argument == "--fixture-ignore-stdin") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
        if (argument == "--fixture-overflow") { std::cout << std::string(65537, 'x') << std::flush; return 0; }
        if (argument == "--version") {
            const char* version = std::getenv("CODEX_FIXTURE_VERSION");
            std::cout << "codex-cli " << (version ? version : "0.150.1") << '\n';
            return 0;
        }
        if (argument == "--help") {
            // A build without the interactive positional PROMPT omits it from its usage.
            std::cout << (std::getenv("CODEX_FIXTURE_NO_PROMPT") ? "Usage: codex [OPTIONS] <COMMAND> [ARGS]\n"
                : "Usage: codex [OPTIONS] [PROMPT]\n       codex [OPTIONS] <COMMAND> [ARGS]\n");
            if (!std::getenv("CODEX_FIXTURE_NO_READ_ONLY")) {
                std::cout << "--sandbox read-only --ignore-rules\n";
            }
            return 0;
        }
        const auto equal = argument.find('=');
        const auto key = argument.substr(0, equal);
        if (equal != std::string::npos && (key == "features" || key.starts_with("features.") ||
            key.starts_with("mcp_servers.") ||
            key == "windows.sandbox" ||
            key == "model_reasoning_effort" || key == "sandbox_mode" ||
            key == "approval_policy" || key == "approvals_reviewer" || key == "notify" || key == "profile"))
        {
            const auto value = key == "features" ? boolean_table(argument.substr(equal + 1))
                : addon::parse_json(argument.substr(equal + 1), {}, nullptr);
            if (!value) return 31;
            if (key == "features" && configuration.find("features")) {
                merge_configuration(*configuration.find("features"), *value);
            }
            else {
                set_configuration(configuration, key, *value);
            }
        }
        if (argument == "fixture_exporter=true") configured_exporter = true;
        if (argument.starts_with("sqlite_home=")) {
            const auto value = addon::parse_json(argument.substr(12), {}, nullptr);
            if (!value || !value->is_string()) return 24;
            sqlite_home = value->as_string();
        }
        if (argument.starts_with("fixture_observer=")) defect = argument.substr(17);
        const auto begin = argument.find("endpoint=\"");
        if (begin != std::string::npos) {
            const auto first = begin + 10;
            endpoint = argument.substr(first, argument.find('"', first) - first);
        }
    }
    for (std::string line; std::getline(std::cin, line);) {
        const auto message = addon::parse_json(line, {}, nullptr);
        if (!message || !message->is_object()) return 2;
        const auto* id = message->find("id");
        if (!id) continue;
        const auto method = text(*message, "method");
        // Startup-proof tests count the private probes from these records.
        if (const char* path = std::getenv("CODEX_FIXTURE_PROBE_LOG")) {
            std::ofstream log(addon::path_from_utf8(path), std::ios::app | std::ios::binary);
            log << method << '\n';
        }
        // Replace a chain file's modification time while the private probe
        // is still running, before its result can be remembered.
        if (const char* path = std::getenv("CODEX_FIXTURE_PROBE_TOUCH"); path && method == "thread/resume") {
            const auto file = addon::path_from_utf8(path);
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(file, error);
            if (!error) std::filesystem::last_write_time(file, modified + std::chrono::minutes(1), error);
            if (error) return 32;
        }
        const char* wake_scenario = std::getenv("CODEX_FIXTURE_WAKE");
        if (const char* path = std::getenv("CODEX_FIXTURE_WAKE_LOG")) {
            Json record = object({{"method", Json(method)}, {"request", *message}, {"sqlite_home", Json(sqlite_home)}});
            if (const char* home = std::getenv("CODEX_HOME")) record.set("codex_home", Json(home));
            std::ofstream log(addon::path_from_utf8(path), std::ios::app | std::ios::binary);
            log << addon::serialize_json(record) << '\n';
            if (method != "initialize" && method != "config/read" && method != "thread/queue/list" && method != "thread/queue/add") return 25;
        }
        Json result = Json::object();
        if (method == "initialize") {
            if (wake_scenario && std::string_view(wake_scenario) == "init-failure") return 26;
            if (initialized) return 22;
            initialized = true;
            if (quota_scenario == "initialize-error") {
                std::cout << "{\"id\":1,\"error\":{\"code\":-32600,\"message\":\"Initialization failed\"}}\n" << std::flush;
                continue;
            }
            result = object({{"userAgent", Json("codex-fixture")}});
        }
        else if (method == "account/rateLimits/read") {
            if (!initialized || quota_received) return 22;
            quota_received = true;
            if (refresh_scenario == "quota-error") {
                std::cout << addon::serialize_json(object({{"id", *id}, {"error", object({
                    {"code", Json(-32000)}, {"message", Json("secret-token-canary expired 401")}})}})) << '\n' << std::flush;
                continue;
            }
            result = object({{"rateLimits", object({{"primary", object({{"usedPercent", Json(12.5)}})}})}});
        }
        else if (method == "account/read" && !identity_scenario.empty()) {
            if (!refresh_scenario.empty() && !quota_received) return 22;
            if (identity_scenario == "unsupported") {
                std::cout << addon::serialize_json(object({{"id", *id}, {"error", object({
                    {"code", Json(-32601)}, {"message", Json("secret-error-canary")}})}})) << '\n' << std::flush;
                continue;
            }
            result = (identity_scenario == "changed" || identity_scenario == "repeated") && *id != Json(quota_received ? 3 : 2)
                ? codex_fixture::account("changed-workspace") : codex_fixture::account();
        }
        else if (method == "getAuthStatus" && !identity_scenario.empty()) {
            if (!refresh_scenario.empty() && !quota_received) return 22;
            if (const char* gate = std::getenv("CODEX_FIXTURE_AUTH_GATE")) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                while (!std::filesystem::exists(addon::path_from_utf8(gate))) {
                    if (std::chrono::steady_clock::now() >= deadline) return 23;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const auto* params = message->find("params");
            if (!params || !params->find("includeToken") ||
                *params->find("includeToken") != Json(true) ||
                !params->find("refreshToken") || *params->find("refreshToken") != Json(false))
            {
                return 21;
            }
            result = (identity_scenario == "changed" || identity_scenario == "repeated") && *id != Json(quota_received ? 4 : 3)
                ? codex_fixture::authentication(codex_fixture::token("changed-user", "changed-workspace"))
                : codex_fixture::authentication();
            std::cerr << "secret-stderr-canary\n" << std::flush;
            if (identity_scenario == "malformed") {
                std::cout << "secret-malformed-canary\n" << std::flush;
                continue;
            }
        }
        else if (method == "config/read") {
            result = object({{"config", object({{"fixture_observer", Json(defect)}, {"otel", object({{"exporter", Json(configured_exporter ? "existing" : "none")}})}})}});
            if (const char* selected = std::getenv("CODEX_FIXTURE_CONFIGURATION")) {
                const auto configured = addon::parse_json(selected, {}, nullptr);
                if (!configured || !configured->is_object()) return 31;
                merge_configuration(*result.find("config"), *configured);
            }
            merge_configuration(*result.find("config"), configuration);
            if (const char* selected = std::getenv("CODEX_FIXTURE_MANAGED_CONFIGURATION")) {
                const auto configured = addon::parse_json(selected, {}, nullptr);
                if (!configured || !configured->is_object()) return 31;
                merge_configuration(*result.find("config"), *configured);
            }
            if (!sqlite_home.empty()) result.find("config")->set("sqlite_home", Json(sqlite_home));
            if (const char* managed = std::getenv("CODEX_FIXTURE_MANAGED_STORE")) result.find("config")->set("sqlite_home", Json(managed));
            if (const char* configured = std::getenv("CODEX_FIXTURE_TUI")) {
                const auto tui = addon::parse_json(configured, {}, nullptr);
                if (!tui) return 31;
                result.find("config")->set("tui", *tui);
            }
        }
        else if (method == "thread/queue/list" || method == "thread/queue/add") {
            if (!initialized) return 27;
            const auto* params = message->find("params");
            if (!params || text(*params, "threadId") != "019933a1-6e43-7102-a520-b3c8294cc763") return 28;
            if (wake_scenario && std::string_view(wake_scenario) == "unsupported") {
                std::cout << addon::serialize_json(object({{"id", *id}, {"error", object({{"code", Json(-32601)}, {"message", Json("unsupported")}})}})) << '\n' << std::flush;
                continue;
            }
            if (method == "thread/queue/list") {
                auto data = Json::array();
                const std::string scenario = wake_scenario ? wake_scenario : "";
                const bool next_page = !text(*params, "cursor").empty();
                const bool owned = scenario == "owned-pending" || (scenario == "owned-next-page" && next_page);
                if (owned || scenario == "user-pending" || (scenario == "owned-next-page" && !next_page)) {
                    data.push_back(object({{"id", Json("019933a1-6e43-7102-a520-b3c8294cc765")},
                        {"clientUserMessageId", Json(owned ? "logonomic-wake:previous-notification" : "user-follow-up")},
                        {"input", Json::array()}}));
                }
                if (scenario == "invalid-queue") {
                    data.push_back(object({{"id", Json("missing-client-id")}}));
                }
                const bool more = scenario == "repeated-cursor" || (scenario == "owned-next-page" && !next_page);
                result = object({{"data", std::move(data)}, {"nextCursor", more ? Json("following-page") : Json()}});
            }
            else {
                if (wake_scenario && std::string_view(wake_scenario) == "lost-reply") return 29;
                if (wake_scenario && std::string_view(wake_scenario) == "malformed") {
                    std::cout << "unparseable\n" << std::flush;
                    continue;
                }
                const auto* input = params->find("input");
                if (!input || !input->is_array()) return 30;
                result = object({{"queuedSubmission", object({{"id", Json("019933a1-6e43-7102-a520-b3c8294cc764")},
                    {"input", *input}, {"clientUserMessageId", Json(wake_scenario && std::string_view(wake_scenario) == "wrong-echo"
                        ? "other-notification" : text(*params, "clientUserMessageId"))}})}});
            }
        }
        else if (method == "thread/start" || method == "thread/resume") {
            const char* home = std::getenv("CODEX_HOME");
            if (!home) return 2;
            const auto path = (addon::path_from_utf8(home) / "sessions" / "probe.jsonl").u8string();
            result = object({{"thread", object({{"id", Json(k_session)}, {"path", Json(std::string(path.begin(), path.end()))}})}});
        }
        std::cout << addon::serialize_json(object({{"id", *id}, {"result", result}})) << '\n' << std::flush;
        if (method == "account/read" &&
            (((identity_scenario == "readiness" || identity_scenario == "changed" || identity_scenario == "repeated") &&
                *id == Json(quota_received ? 3 : 2)) ||
                (identity_scenario == "repeated" && *id == Json(quota_received ? 5 : 4))))
        {
            std::cout << "{\"method\":\"account/updated\",\"params\":{}}\n" << std::flush;
        }
        if (method == "thread/resume" && !endpoint.empty()) send_observation(endpoint, defect);
    }
    if (!identity_scenario.empty()) {
        std::cerr << "secret-stderr-tail-canary\n";
        std::cout << "{\"method\":\"fixture/privateNotification\",\"params\":{\"token\":\"secret-notification-canary\"}}\n";
        if (identity_scenario == "after-close") std::cout << "{\"method\":\"account/updated\",\"params\":{}}\n";
        if (identity_scenario == "native-failure") return 19;
    }
    if (!quota_scenario.empty()) {
        if (!quota_received) return 19;
        if (quota_scenario == "malformed") std::cout << '{';
        else if (quota_scenario == "server-request") std::cout << "{\"id\":17,\"method\":\"account/refresh\",\"params\":{}}\n";
        else std::cout << "{\"method\":\"account/updated\",\"params\":{}}\n";
        if (quota_scenario == "native-failure") return 19;
    }
    return 0;
}
