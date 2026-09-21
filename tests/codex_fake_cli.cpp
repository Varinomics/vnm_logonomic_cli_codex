#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>

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
#include <iostream>
#include <initializer_list>
#include <string>
#include <string_view>
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
    bool configured_exporter = false;
    // Model an intrinsic producer defect across the isolated native probe.
    // User -c arguments are intentionally omitted by that probe.
    const char* fixture_defect = std::getenv("CODEX_FIXTURE_OBSERVER");
    std::string defect = fixture_defect ? fixture_defect : "";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
#ifndef _WIN32
        if (argument == "--fixture-custody") { std::cout << getpgrp() << ':' << getsid(0) << '\n'; return 0; }
#endif
        if (argument == "--fixture-ignore-stdin") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
        if (argument == "--fixture-overflow") { std::cout << std::string(65537, 'x') << std::flush; return 0; }
        if (argument == "--version") { std::cout << "codex-cli 0.150.1\n"; return 0; }
        if (argument == "--help") {
            // A build without the interactive positional PROMPT omits it from its usage.
            std::cout << (std::getenv("CODEX_FIXTURE_NO_PROMPT") ? "Usage: codex [OPTIONS] <COMMAND> [ARGS]\n"
                : "Usage: codex [OPTIONS] [PROMPT]\n       codex [OPTIONS] <COMMAND> [ARGS]\n");
            return 0;
        }
        if (argument == "fixture_exporter=true") configured_exporter = true;
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
        Json result = Json::object();
        if (method == "initialize") {
            result = object({{"userAgent", Json("codex-fixture")}});
        }
        else if (method == "config/read") {
            result = object({{"config", object({{"fixture_observer", Json(defect)}, {"otel", object({{"exporter", Json(configured_exporter ? "existing" : "none")}})}})}});
        }
        else if (method == "thread/start" || method == "thread/resume") {
            const char* home = std::getenv("CODEX_HOME");
            if (!home) return 2;
            const auto path = (addon::path_from_utf8(home) / "sessions" / "probe.jsonl").u8string();
            result = object({{"thread", object({{"id", Json(k_session)}, {"path", Json(std::string(path.begin(), path.end()))}})}});
        }
        std::cout << addon::serialize_json(object({{"id", *id}, {"result", result}})) << '\n' << std::flush;
        if (method == "thread/resume" && !endpoint.empty()) send_observation(endpoint, defect);
    }
    return 0;
}
