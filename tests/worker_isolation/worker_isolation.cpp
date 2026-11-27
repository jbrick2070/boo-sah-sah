// Small independent check of the Windows job and inherited-pipe primitives.
// results_worker_test.cpp exercises the application's supervisor in portable and installed builds.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE h = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = h;
    }
};

// This is a test fixture, not UI-thread-safe production code: it deliberately
// uses synchronous process creation and termination on the calling test thread.
class WorkerFixture {
public:
    bool start(const wchar_t* mode) {
        if (process_.value) return false;
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_.value) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;

        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 256)) return false;
        read_.reset(read);
        Handle writer;
        writer.reset(write);
        if (!SetHandleInformation(read_.value, HANDLE_FLAG_INHERIT, 0)) return false;

        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
        const bool listOk = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                       &writer.value, sizeof(writer.value), nullptr, nullptr) != FALSE;
        if (!listOk) { DeleteProcThreadAttributeList(attributes); return false; }

        wchar_t executable[32768]{};
        const DWORD len = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (!len || len >= std::size(executable)) { DeleteProcThreadAttributeList(attributes); return false; }
        const std::wstring path = executable;
        std::wstring command = L"\"" + path + L"\" --isolation-child " + mode + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(writer.value));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION pi{};
        const BOOL created = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attributes);
        if (!created) return false;
        process_.reset(pi.hProcess);
        Handle thread;
        thread.reset(pi.hThread);
        if (!AssignProcessToJobObject(job_.value, process_.value)) {
            TerminateProcess(process_.value, 1);
            return false;
        }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            TerminateJobObject(job_.value, 1);
            return false;
        }
        return true;
    }

    bool handshake(unsigned timeoutMs) const {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(read_.value, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available) {
                char value = 0;
                DWORD got = 0;
                return ReadFile(read_.value, &value, 1, &got, nullptr) && got == 1 && value == 'R';
            }
            if (WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0) return false;
            Sleep(10);
        }
        return false;
    }

    bool terminate() {
        return TerminateJobObject(job_.value, 1) && WaitForSingleObject(process_.value, 1000) == WAIT_OBJECT_0;
    }
    bool closeJobAndObserveExit() {
        job_.reset();
        return WaitForSingleObject(process_.value, 1000) == WAIT_OBJECT_0;
    }
private:
    Handle job_;
    Handle process_;
    Handle read_;
};

int child(const std::wstring& mode, const wchar_t* handleText) {
    HANDLE pipe = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(handleText, nullptr, 10)));
    if (mode == L"ready") {
        DWORD sent = 0;
        if (!WriteFile(pipe, "R", 1, &sent, nullptr) || sent != 1) return 2;
    }
    Sleep(INFINITE); // Simulates a child that never returns, without touching a GPU.
    return 3;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--isolation-child") return child(argv[2], argv[3]);
    WorkerFixture stalled;
    if (!stalled.start(L"silent") || stalled.start(L"ready") || stalled.handshake(200) || !stalled.terminate()) {
        std::cerr << "stalled worker / admission / termination test failed\n";
        return 1;
    }
    WorkerFixture ready;
    if (!ready.start(L"ready") || !ready.handshake(2000) || !ready.closeJobAndObserveExit()) {
        std::cerr << "handshake / kill-on-close test failed\n";
        return 1;
    }
    std::cout << "Kill-on-close and stalled child checks passed\n";
    return 0;
}
