#include "child_process.h"

#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
extern char** environ;
#endif

namespace cli {

namespace {

std::string not_found(const std::string& name) {
    return "`" + name + "` was not found in the absolute folders of PATH (the current folder is never searched)";
}

}  // namespace

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// An environment variable, "" when it is unset or empty.
std::wstring environment(const wchar_t* name) {
    std::wstring value(512, L'\0');
    for (;;) {
        const DWORD n = GetEnvironmentVariableW(name, &value[0], static_cast<DWORD>(value.size()));
        if (n < value.size()) {
            value.resize(n);
            return value;
        }
        value.resize(n);  // too small: `n` is the size needed, terminating NUL included
    }
}

// PATH and PATHEXT: `;`-separated entries, where a quoted entry may hold `;` (the quotes are not part of it).
std::vector<std::wstring> split_list(const std::wstring& list) {
    std::vector<std::wstring> out(1);
    bool quoted = false;
    for (const wchar_t c : list) {
        if (c == L'"')
            quoted = !quoted;
        else if (c == L';' && !quoted)
            out.emplace_back();
        else
            out.back() += c;
    }
    return out;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

std::wstring find_program_wide(const std::wstring& name) {
    if (name.empty() || name.find_first_of(L"/\\:") != std::wstring::npos) return name;
    std::vector<std::wstring> candidates;
    if (std::filesystem::path(name).has_extension()) {
        candidates.push_back(name);
    } else {
        std::wstring extensions = environment(L"PATHEXT");
        if (extensions.empty()) extensions = L".COM;.EXE;.BAT;.CMD";
        for (const std::wstring& extension : split_list(extensions))
            if (lower(extension) == L".com" || lower(extension) == L".exe") candidates.push_back(name + extension);
        if (candidates.empty()) candidates.push_back(name + L".exe");  // what CreateProcess itself would append
    }
    for (const std::wstring& entry : split_list(environment(L"PATH"))) {
        const std::filesystem::path folder(entry);
        if (entry.empty() || !folder.is_absolute()) continue;  // "", "." and relative entries: never searched
        for (const std::wstring& candidate : candidates) {
            const std::wstring path = (folder / candidate).native();
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return path;
        }
    }
    return std::wstring();
}

// One argument quoted for CommandLineToArgvW (the rules of the Microsoft C runtime).
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        out += arg[i];
    }
    return out + L"\"";
}

std::string drain(HANDLE pipe) {
    std::string out;
    char buffer[65536];
    DWORD got = 0;
    while (ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) && got > 0) out.append(buffer, got);
    return out;
}

}  // namespace

std::string find_program(const std::string& name) { return narrow(find_program_wide(widen(name))); }

ProcessResult run_process(const std::vector<std::string>& argv) {
    ProcessResult result;
    if (argv.empty()) return result;
    // The program is named explicitly: with no application name, CreateProcess would look in the current folder first.
    const std::wstring program = find_program_wide(widen(argv[0]));
    if (program.empty()) {
        result.errors = not_found(argv[0]);
        return result;
    }
    std::wstring command;
    for (const std::string& a : argv) command += (command.empty() ? L"" : L" ") + quote(widen(a));
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE out_read, out_write, err_read, err_write;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        result.errors = "cannot create a pipe";
        return result;
    }
    if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        result.errors = "cannot create a pipe";
        return result;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_write;
    si.hStdError = err_write;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(program.c_str(), &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    const DWORD error = started ? 0 : GetLastError();
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (started) {
        std::thread errors([&] { result.errors = drain(err_read); });
        result.output = drain(out_read);
        errors.join();
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        result.errors = "cannot start " + narrow(program) + " (Windows error " + std::to_string(error) + ")";
    }
    CloseHandle(out_read);
    CloseHandle(err_read);
    return result;
}

#else

namespace {

std::string drain(int fd) {
    std::string out;
    char buffer[65536];
    for (ssize_t got; (got = read(fd, buffer, sizeof buffer)) > 0;) out.append(buffer, static_cast<size_t>(got));
    return out;
}

}  // namespace

std::string find_program(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) return name;
    std::string list;
    if (const char* path = std::getenv("PATH")) {
        list = path;
    } else {  // unset: the system's default search path, which execvp would use
        const size_t n = confstr(_CS_PATH, nullptr, 0);
        if (n > 1) {
            list.assign(n, '\0');
            confstr(_CS_PATH, &list[0], n);
            list.resize(n - 1);
        }
        if (list.empty()) list = "/usr/bin:/bin";
    }
    for (size_t start = 0;;) {
        const size_t end = list.find(':', start);
        const std::string folder = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!folder.empty() && folder[0] == '/') {  // "", "." and relative entries: never searched
            const std::string path = folder + (folder.back() == '/' ? "" : "/") + name;
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0) return path;
        }
        if (end == std::string::npos) return std::string();
        start = end + 1;
    }
}

ProcessResult run_process(const std::vector<std::string>& argv) {
    ProcessResult result;
    if (argv.empty()) return result;
    // posix_spawnp would also search the empty, `.` and relative entries of PATH, that is the current folder.
    const std::string program = find_program(argv[0]);
    if (program.empty()) {
        result.errors = not_found(argv[0]);
        return result;
    }
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) {
        result.errors = "cannot create a pipe";
        return result;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        result.errors = "cannot create a pipe";
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    std::vector<std::string> args = argv;
    std::vector<char*> cargs;
    for (std::string& a : args) cargs.push_back(&a[0]);
    cargs.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, nullptr, cargs.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (rc == 0) {
        std::thread errors([&] { result.errors = drain(err_pipe[0]); });
        result.output = drain(out_pipe[0]);
        errors.join();
        int status = 0;
        waitpid(pid, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else {
        result.errors = "cannot start " + program + ": " + std::strerror(rc);
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    return result;
}

#endif

}  // namespace cli
