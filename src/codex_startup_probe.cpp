#include "codex_startup_probe.h"
#include "codex_preparation.h"
#include "codex_startup_observation.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <set>
#include <system_error>
#include <string_view>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;
using Environment = std::vector<std::pair<std::string, std::string>>;
using Clock = std::chrono::steady_clock;

Json object(std::initializer_list<addon::Json_member> members)
{
    return Json(Json::Object(members));
}

std::string text(const Json& value, std::string_view key)
{
    const auto* member = value.find(key);
    return member && member->is_string() ? member->as_string() : std::string{};
}

std::string utf8(const fs::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket k_invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
bool nonblocking(Socket socket) { u_long mode = 1; return ioctlsocket(socket, FIONBIO, &mode) == 0; }
#else
using Socket = int;
constexpr Socket k_invalid_socket = -1;
void close_socket(Socket socket) { close(socket); }
bool nonblocking(Socket socket) { return fcntl(socket, F_SETFL, O_NONBLOCK) == 0; }
#endif

class Private_observer
{
public:
    Private_observer()
    {
#ifdef _WIN32
        WSADATA data{};
        m_initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!m_initialized) return;
#endif
        m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listener == k_invalid_socket) return;
#ifdef _WIN32
        SetHandleInformation(reinterpret_cast<HANDLE>(m_listener), HANDLE_FLAG_INHERIT, 0);
#else
        fcntl(m_listener, F_SETFD, FD_CLOEXEC);
#endif
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(m_listener, 8) != 0 || !nonblocking(m_listener)) return;
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        if (getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) return;
        m_endpoint = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/v1/logs";
        m_authorization = "Bearer " + addon::random_nonce_hex();
    }

    ~Private_observer()
    {
        for (const auto& client : m_clients) close_socket(client.socket);
        if (m_listener != k_invalid_socket) close_socket(m_listener);
#ifdef _WIN32
        if (m_initialized) WSACleanup();
#endif
    }

    bool valid() const { return !m_endpoint.empty() && !m_failed; }
    const std::string& endpoint() const { return m_endpoint; }
    std::string authorization_environment() const { return "Authorization=Bearer%20" + m_authorization.substr(7); }
    bool observed(const std::string& id) const { return !m_failed && m_ids.contains(id); }

    void poll()
    {
        while (m_clients.size() < 8) {
            const Socket client = accept(m_listener, nullptr, nullptr);
            if (client == k_invalid_socket) break;
#ifdef _WIN32
            SetHandleInformation(reinterpret_cast<HANDLE>(client), HANDLE_FLAG_INHERIT, 0);
#else
            fcntl(client, F_SETFD, FD_CLOEXEC);
#endif
            if (!nonblocking(client)) { close_socket(client); continue; }
            m_clients.push_back({client, {}, Clock::now()});
        }
        for (auto iterator = m_clients.begin(); iterator != m_clients.end();) {
            bool close = false;
            std::array<char, 8192> buffer{};
            const auto count = recv(iterator->socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (count > 0) iterator->bytes.append(buffer.data(), static_cast<std::size_t>(count));
            if (count == 0 || iterator->bytes.size() > 1024 * 1024 + 16384 ||
                Clock::now() - iterator->started > std::chrono::seconds(3)) close = true;
            const auto boundary = iterator->bytes.find("\r\n\r\n");
            if (boundary == std::string::npos && iterator->bytes.size() > 16384) close = true;
            if (!close && boundary != std::string::npos) {
                std::string headers = iterator->bytes.substr(0, boundary);
                const bool correct_path = headers.starts_with("POST /v1/logs HTTP/1.1\r\n");
                std::size_t content_length = 0;
                bool length_seen = false;
                bool authorization_seen = false;
                bool authorized = false;
                std::string content_type;
                bool malformed = !correct_path || boundary > 16384;
                std::size_t cursor = headers.find("\r\n") + 2;
                while (!malformed && cursor < headers.size()) {
                    const auto end = headers.find("\r\n", cursor);
                    const auto line = headers.substr(cursor, end - cursor);
                    const auto colon = line.find(':');
                    if (colon == std::string::npos || colon == 0) { malformed = true; break; }
                    std::string name = line.substr(0, colon);
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); });
                    const auto begin = line.find_first_not_of(" \t", colon + 1);
                    const auto last = line.find_last_not_of(" \t");
                    const auto value = begin == std::string::npos ? std::string{} : line.substr(begin, last - begin + 1);
                    if (name == "authorization") {
                        malformed |= authorization_seen;
                        authorization_seen = true;
                        authorized = value == m_authorization;
                    }
                    if (name == "content-type") content_type = value;
                    if (name == "content-length") {
                        const auto converted = std::from_chars(value.data(), value.data() + value.size(), content_length);
                        malformed |= length_seen || converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || content_length > 1024 * 1024;
                        length_seen = true;
                    }
                    if (name == "transfer-encoding") malformed = true;
                    if (end == std::string::npos) break;
                    cursor = end + 2;
                }
                if (malformed || !authorized || !length_seen || content_type != "application/json" ||
                    iterator->bytes.size() > boundary + 4 + content_length) close = true;
                else if (iterator->bytes.size() == boundary + 4 + content_length) {
                    const auto ids = conversation_ids(std::string_view(iterator->bytes).substr(boundary + 4, content_length));
                    if (ids) {
                        for (const auto& id : *ids) {
                            if (m_ids.size() == 64 && !m_ids.contains(id)) { m_failed = true; break; }
                            m_ids.insert(id);
                        }
                    }
                    else m_failed = true;
                    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
#ifdef _WIN32
                    send(iterator->socket, response.data(), static_cast<int>(response.size()), 0);
#else
                    send(iterator->socket, response.data(), response.size(), MSG_NOSIGNAL);
#endif
                    close = true;
                }
            }
            if (close) { close_socket(iterator->socket); iterator = m_clients.erase(iterator); }
            else ++iterator;
        }
    }

private:
    struct Client
    {
        Socket socket;
        std::string bytes;
        Clock::time_point started;
    };
    Socket m_listener = k_invalid_socket;
    std::vector<Client> m_clients;
    std::set<std::string> m_ids;
    std::string m_endpoint;
    std::string m_authorization;
    bool m_failed = false;
#ifdef _WIN32
    bool m_initialized = false;
#endif
};

std::optional<Json> app_server_call(
    const Verified_launch& launch, const std::vector<std::string>& configuration,
    const Environment& environment, const std::string& directory,
    std::string method, Json parameters, Clock::time_point deadline,
    addon::Invocation_context& context, Private_observer* observer = nullptr,
    const std::string& expected = {})
{
    std::vector<std::string> argv(launch.argv.begin(), launch.argv.begin() + launch.prefix_size);
    argv.insert(argv.end(), configuration.begin(), configuration.end());
    argv.push_back("app-server");
    addon::Probe_exchange exchange;
    exchange.initial_input = addon::serialize_json(object({{"id", Json(1)}, {"method", Json("initialize")},
        {"params", object({{"clientInfo", object({{"name", Json("logonomic-startup-probe")}, {"version", Json("1")}})}})}})) + '\n';
    std::size_t consumed = 0;
    bool initialized = false;
    std::optional<Json> response;
    exchange.on_output = [&](std::string_view bytes) -> std::optional<std::string> {
        std::string input;
        while (true) {
            const auto end = bytes.find('\n', consumed);
            if (end == std::string_view::npos) break;
            const auto message = addon::parse_json(bytes.substr(consumed, end - consumed), {}, nullptr);
            consumed = end + 1;
            const auto* id = message ? message->find("id") : nullptr;
            if (!message || !message->is_object()) return std::nullopt;
            if (!id || !id->is_integer()) continue;
            if (message->find("error")) return std::nullopt;
            const auto* result = message->find("result");
            if (!result || !result->is_object()) return std::nullopt;
            if (id->as_integer() == 1 && !initialized) {
                if (result->as_object().empty()) return std::nullopt;
                initialized = true;
                input += "{\"method\":\"initialized\"}\n";
                input += addon::serialize_json(object({{"id", Json(2)}, {"method", Json(method)}, {"params", parameters}})) + '\n';
            }
            else if (id->as_integer() == 2 && initialized && !response) {
                response = *result;
                exchange.complete = observer == nullptr;
            }
        }
        return input;
    };
    if (observer) {
        exchange.tick = [&] {
            observer->poll();
            exchange.complete = response && observer->observed(expected);
        };
    }
    const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
    if (remaining <= 0 || !run_probe(argv, environment, directory, remaining, context, &exchange) || !exchange.complete) return std::nullopt;
    return response;
}

class Private_directory
{
public:
    explicit Private_directory(const Environment& environment)
    {
        std::string temporary;
        for (const auto& [name, value] : environment) {
            if ((name == "TEMP" || name == "TMP" || name == "TMPDIR") && !value.empty()) { temporary = value; break; }
        }
#ifndef _WIN32
        if (temporary.empty()) temporary = "/tmp";
#endif
        if (!addon::path_from_utf8(temporary).is_absolute()) return;
        m_path = addon::path_from_utf8(temporary) / ("logonomic-codex-probe-" + addon::random_nonce_hex());
        std::error_code error;
        if (!fs::create_directory(m_path, error) || error) { m_path.clear(); return; }
#ifndef _WIN32
        fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::replace, error);
        if (error) { fs::remove(m_path, error); m_path.clear(); }
#endif
    }
    ~Private_directory() { if (!m_path.empty()) { std::error_code error; fs::remove_all(m_path, error); } }
    const fs::path& path() const { return m_path; }
private:
    fs::path m_path;
};

} // namespace

bool startup_supported(const Verified_launch& launch, const std::vector<std::string>& configuration,
    const Environment& environment, const std::string& working_directory,
    addon::Invocation_context& context, std::string* error)
{
    const auto deadline = Clock::now() + std::chrono::seconds(12);
    const auto config = app_server_call(launch, configuration, environment, working_directory, "config/read",
        object({{"cwd", Json(working_directory)}, {"includeLayers", Json(false)}}), deadline, context);
    const auto* effective = config ? config->find("config") : nullptr;
    if (!effective || !effective->is_object()) {
        *error = "Codex effective startup configuration could not be inspected.";
        return false;
    }
    const auto* otel = effective->find("otel");
    const auto* exporter = otel ? otel->find("exporter") : nullptr;
    if (exporter && !exporter->is_null() && !(exporter->is_string() && exporter->as_string() == "none")) {
        *error = "Exact Codex resume conflicts with the launcher's existing telemetry log exporter.";
        return false;
    }
    addon::Sha256 digest;
    digest.update(addon::serialize_json_canonical(*effective));
    for (const auto& file : launch.files) {
        std::ifstream input(addon::path_from_utf8(file), std::ios::binary);
        std::array<char, 65536> buffer{};
        if (!input) { *error = "The Codex executable chain could not be identified."; return false; }
        while (input) { input.read(buffer.data(), buffer.size()); digest.update(buffer.data(), static_cast<std::size_t>(input.gcount())); }
        if (!input.eof()) { *error = "The Codex executable chain could not be identified."; return false; }
    }
    const auto key = addon::to_hex(digest.finish());
    static std::mutex cache_mutex;
    static std::set<std::string> supported;
    { std::lock_guard lock(cache_mutex); if (supported.contains(key)) return true; }
    Private_directory directory(environment);
    Private_observer observer;
    if (directory.path().empty() || !observer.valid()) { *error = "Private startup probe storage or observer is unavailable."; return false; }
    Environment probe_environment;
    for (const auto& [name, value] : environment) {
        std::string normalized = name;
#ifdef _WIN32
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : char(c); });
#endif
        if (!normalized.starts_with("OPENAI_") && !normalized.starts_with("LOGONOMIC_") &&
            !normalized.starts_with("VNM_CONTROL") && !normalized.starts_with("OTEL_") && normalized != "CODEX_HOME")
        {
            probe_environment.emplace_back(name, value);
        }
    }
    const std::string root = utf8(directory.path());
    probe_environment.emplace_back("CODEX_HOME", root);
    probe_environment.emplace_back("OTEL_EXPORTER_OTLP_LOGS_HEADERS", observer.authorization_environment());
    const auto created = app_server_call(launch, {}, probe_environment, root, "thread/start",
        object({{"cwd", Json(root)}, {"approvalPolicy", Json("never")}, {"sandbox", Json("read-only")}, {"persistExtendedHistory", Json(true)}}), deadline, context);
    const auto* thread = created ? created->find("thread") : nullptr;
    const std::string id = thread ? text(*thread, "id") : std::string{};
    const auto path = addon::path_from_utf8(thread ? text(*thread, "path") : std::string{}).lexically_normal();
    const auto relative = path.lexically_relative(directory.path());
    if (id.empty() || !path.is_absolute() || relative.empty() || *relative.begin() == "..") { *error = "Codex could not create an isolated startup probe."; return false; }
    std::error_code filesystem_error;
    fs::create_directories(path.parent_path(), filesystem_error);
    std::ofstream history(path, std::ios::binary | std::ios::trunc);
    if (filesystem_error || !history) { *error = "Private probe history could not be written."; return false; }
    // The saved conversation is entirely synthetic; no model turn is requested.
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::array<char, 32> time_buffer{};
    std::strftime(time_buffer.data(), time_buffer.size(), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    const std::string timestamp = time_buffer.data();
    const auto metadata = object({{"id", Json(id)}, {"timestamp", Json(timestamp)}, {"cwd", Json(root)},
        {"originator", Json("codex")}, {"cli_version", Json("startup-probe")}, {"source", Json("cli")},
        {"model_provider", Json("openai")}, {"base_instructions", object({{"text", Json("Private startup probe; no turn is requested.")}})}});
    const auto message = object({{"type", Json("message")}, {"role", Json("user")},
        {"content", Json(Json::Array{object({{"type", Json("input_text")}, {"text", Json("Private saved probe history.")}})})}});
    history << addon::serialize_json(object({{"timestamp", Json(timestamp)}, {"type", Json("session_meta")}, {"payload", metadata}})) << '\n';
    history << addon::serialize_json(object({{"timestamp", Json(timestamp)}, {"type", Json("response_item")}, {"payload", message}})) << '\n';
    history.close();
    if (!history) { *error = "Private probe history could not be written."; return false; }
    const std::vector<std::string> arguments{"-c", "otel.exporter={otlp-http={endpoint=" + toml_string(observer.endpoint()) + ",protocol=\"json\"}}", "-c", "otel.log_user_prompt=false"};
    const auto resumed = app_server_call(launch, arguments, probe_environment, root, "thread/resume",
        object({{"threadId", Json(id)}, {"cwd", Json(root)}}), deadline, context, &observer, id);
    const auto* resumed_thread = resumed ? resumed->find("thread") : nullptr;
    if (!resumed_thread || text(*resumed_thread, "id") != id || !observer.observed(id)) {
        *error = "Installed Codex did not confirm private startup-observer support before the probe deadline.";
        return false;
    }
    std::lock_guard lock(cache_mutex);
    if (supported.size() >= 64) supported.clear();
    supported.insert(key);
    return true;
}

} // namespace codex
