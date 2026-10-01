#include "codex_quota_exchange.h"
#include "codex_current_account.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
using Stream = HANDLE;
constexpr Stream k_closed = nullptr;

void close_stream(Stream stream) { CloseHandle(stream); }

bool write_stream(Stream stream, std::string_view bytes)
{
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(stream, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || !written) {
            return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}

int read_stream(Stream stream, char* bytes, int size)
{
    DWORD count = 0;
    if (ReadFile(stream, bytes, static_cast<DWORD>(size), &count, nullptr)) {
        return static_cast<int>(count);
    }
    return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
}
#else
using Stream = int;
constexpr Stream k_closed = -1;

void close_stream(Stream stream) { close(stream); }

bool write_stream(Stream stream, std::string_view bytes)
{
    while (!bytes.empty()) {
        const auto written = write(stream, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

int read_stream(Stream stream, char* bytes, int size)
{
    ssize_t count;
    do {
        count = read(stream, bytes, static_cast<std::size_t>(size));
    } while (count < 0 && errno == EINTR);
    return static_cast<int>(count);
}
#endif

class Owned_stream
{
public:
    explicit Owned_stream(Stream value = k_closed) : m_value(value) {}
    ~Owned_stream() { reset(); }
    Owned_stream(const Owned_stream&) = delete;
    Owned_stream& operator=(const Owned_stream&) = delete;
    Stream get() const { return m_value; }
    Stream release() { return std::exchange(m_value, k_closed); }
    void reset()
    {
        if (m_value != k_closed) {
            close_stream(std::exchange(m_value, k_closed));
        }
    }

private:
    Stream m_value;
};

template<class Protocol>
bool exchange(Stream input, Stream output, Stream forwarded_output)
{
    Owned_stream child_input(input);
    Owned_stream child_output(output);
    Protocol protocol;
    bool healthy = write_stream(child_input.get(), protocol.take_input());
    if (!healthy) {
        child_input.reset();
    }
    char buffer[16384];
    int count = 0;
    while ((count = read_stream(child_output.get(), buffer, static_cast<int>(sizeof(buffer)))) > 0) {
        const std::string_view bytes(buffer, static_cast<std::size_t>(count));
        // Quota keeps its established native transcript. The opted-in AUTH
        // exchange contains a credential and must never forward native bytes.
        bool forwarded = true;
        if constexpr (std::is_same_v<Protocol, codex::Quota_exchange>) {
            forwarded = write_stream(forwarded_output, bytes);
        }
        const bool valid = protocol.consume(bytes);
        healthy = healthy && forwarded && valid;
        if (child_input.get() != k_closed) {
            if (!healthy || protocol.close_input()) {
                child_input.reset();
            }
            else
            if (!write_stream(child_input.get(), protocol.take_input())) {
                healthy = false;
                child_input.reset();
            }
        }
    }
    if (!healthy || count != 0 || !protocol.finish()) {
        return false;
    }
    if constexpr (std::is_same_v<Protocol, codex::Current_account_exchange>) {
        return write_stream(forwarded_output,
            logonomic::cli_addon::serialize_json(protocol.observation()));
    }
    return true;
}

bool exchange(Stream input, Stream output, Stream forwarded_output, bool current_account)
{
    return current_account
        ? exchange<codex::Current_account_exchange>(input, output, forwarded_output)
        : exchange<codex::Quota_exchange>(input, output, forwarded_output);
}

#ifdef _WIN32
std::wstring quote_argument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (character == L'"' ? 2 : 1) + (character == L'"' ? 1 : 0), L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    const bool current_account = argc > 1 && std::wstring_view(argv[1]) == L"--current-account";
    const int native_index = current_account ? 2 : 1;
    if (argc <= native_index) {
        return 2;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, &security, 0)) {
        return 3;
    }
    Owned_stream native_input(input_read);
    Owned_stream helper_input(input_write);
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 0)) {
        return 3;
    }
    Owned_stream helper_output(output_read);
    Owned_stream native_output(output_write);
    HANDLE errors = nullptr;
    if (!SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0))
    {
        return 3;
    }
    if (current_account) {
        errors = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (errors == INVALID_HANDLE_VALUE) {
            return 3;
        }
    }
    else
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(),
            &errors, 0, TRUE, DUPLICATE_SAME_ACCESS))
    {
        return 3;
    }
    Owned_stream native_errors(errors);
    HANDLE inherited[] = {input_read, output_write, errors};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        return 3;
    }
    std::wstring command;
    for (int i = native_index; i < argc; ++i) {
        if (!command.empty()) {
            command += L' ';
        }
        command += quote_argument(argv[i]);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_read;
    startup.StartupInfo.hStdOutput = output_write;
    startup.StartupInfo.hStdError = errors;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const DWORD console = GetConsoleCP() != 0 ? 0 : CREATE_NO_WINDOW;
    const bool started = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr) &&
        CreateProcessW(argv[native_index], command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | console,
            nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (!started) {
        return 3;
    }
    Owned_stream child(process.hProcess);
    Owned_stream thread(process.hThread);
    native_input.reset();
    native_output.reset();
    native_errors.reset();
    // The native child inherits the host's Job and the frozen environment/cwd.
    // Keep the helper alive until the native stream and process both end.
    const bool healthy = exchange(helper_input.release(), helper_output.release(),
        GetStdHandle(STD_OUTPUT_HANDLE), current_account);
    DWORD exit_code = 3;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    }
    else {
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    ExitProcess(exit_code != 0 ? exit_code : healthy ? 0 : 4);
}
#else
int main(int argc, char** argv)
{
    const bool current_account = argc > 1 && std::string_view(argv[1]) == "--current-account";
    const int native_index = current_account ? 2 : 1;
    if (argc <= native_index) {
        return 2;
    }
    int input[2];
    if (pipe(input) != 0) {
        return 3;
    }
    Owned_stream native_input(input[0]);
    Owned_stream helper_input(input[1]);
    int output[2];
    if (pipe(output) != 0) {
        return 3;
    }
    Owned_stream helper_output(output[0]);
    Owned_stream native_output(output[1]);
    const pid_t child = fork();
    if (child < 0) {
        return 3;
    }
    if (child == 0) {
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0) {
            _exit(3);
        }
        if (current_account) {
            const int errors = open("/dev/null", O_WRONLY);
            if (errors < 0 || dup2(errors, STDERR_FILENO) < 0) {
                _exit(3);
            }
            if (errors != STDERR_FILENO) close(errors);
        }
        if (input[0] != STDIN_FILENO) {
            close(input[0]);
        }
        close(input[1]);
        close(output[0]);
        close(output[1]);
        // No new process group: native Codex remains in the host's custody.
        execv(argv[native_index], argv + native_index);
        _exit(3);
    }
    signal(SIGPIPE, SIG_IGN);
    native_input.reset();
    native_output.reset();
    const bool healthy = exchange(helper_input.release(), helper_output.release(), STDOUT_FILENO, current_account);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return 3;
    }
    if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        signal(number, SIG_DFL);
        raise(number);
        return 128 + number;
    }
    const int exit_code = WEXITSTATUS(status);
    return exit_code != 0 ? exit_code : healthy ? 0 : 4;
}
#endif
