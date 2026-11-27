#include "boo/results_worker.hpp"
#include <windows.h>
#include <appmodel.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace boo;
enum Scenario { success, initHang, warmHang, submitHang, teardownHang, finishedHang, crash, disconnect, orphan, resetHang };
HANDLE progressPipe = nullptr;
bool delayedPreparation = false;
std::ofstream receipt;
const auto testStarted = Clock::now();
std::atomic<int64_t> killRequestAfterStop{-1};
std::atomic<int64_t> jobKillError{-1};
template<class... T> void record(T&&... fields) {
    if (receipt.is_open()) {
        receipt << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-testStarted).count() << "ms ";
        (receipt << ... << fields) << '\n';
        receipt.flush();
    }
}
bool packageIdentity() {
    UINT32 n = 0;
    if (GetCurrentPackageFullName(&n, nullptr) != ERROR_INSUFFICIENT_BUFFER) return false;
    std::wstring name(n, L'\0');
    return GetCurrentPackageFullName(&n, name.data()) == ERROR_SUCCESS && name.starts_with(L"BooSahSah.ContainmentTest.");
}
bool live(ResultsWorkerState s) { return s == ResultsWorkerState::starting || s == ResultsWorkerState::running; }
bool startWhenFree(ResultsWorker& worker, Scenario scenario, bool packaged) {
    auto until = Clock::now() + 2200ms;
    BenchmarkRequest request{};
    request.luidLow = scenario;
    request.luidHigh = packaged;
    if (scenario == finishedHang && delayedPreparation) request.viewerLocalUsage = 4500;
    do {
        if (worker.start(request)) return true;
        Sleep(5);
    } while (Clock::now() < until);
    return false;
}
struct Handle {
    HANDLE h{};
    ~Handle() { if (h) CloseHandle(h); }
};
int receivedMessages = 0;
LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP) { ++receivedMessages; return 0; }
    return DefWindowProcW(wnd, msg, wp, lp);
}
bool pump(HWND wnd) {
    const int previous = receivedMessages;
    if (!PostMessageW(wnd, WM_APP, 0, 0)) return false;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    return receivedMessages == previous + 1;
}
std::optional<BenchmarkStage> stalledStage(Scenario scenario) {
    switch (scenario) {
    case initHang: return BenchmarkStage::Preparing;
    case warmHang: return BenchmarkStage::Warming;
    case submitHang: return BenchmarkStage::Measuring;
    case teardownHang: case resetHang: return BenchmarkStage::Finishing;
    case finishedHang: return BenchmarkStage::Finished;
    default: return std::nullopt;
    }
}
bool testScenario(Scenario scenario, bool packaged, HWND wnd, bool cancel) {
    ResultsWorker worker, duplicate;
    record("begin scenario=",int(scenario)," cancel=",cancel);
    killRequestAfterStop.store(-1);
    jobKillError.store(-1);
    auto failed = [&](const char* message) { record("FAIL ",message," error=",GetLastError()); return false; };
    if (!startWhenFree(worker, scenario, packaged)) return failed("scheduler did not admit a new scenario");
    if (duplicate.start({})) return failed("second child admitted");
    // Preparation may legitimately consume 18 seconds before the injected operation begins.
    // Assert the operation's own deadline from the first acknowledged target snapshot.
    const auto until = Clock::now() + 50s + 250ms;
    const auto target = stalledStage(scenario);
    std::optional<Clock::time_point> entered;
    const auto stageDuration = scenario == initHang ? 18s : 2s;
    Handle child;
    DWORD observedPid = 0;
    BenchmarkStage observedStage = BenchmarkStage::Preparing;
    while (live(worker.poll()) && Clock::now() < until) {
        if (!pump(wnd)) return failed("posted window message did not dispatch");
        auto snap = worker.snapshot();
        if (snap.deviceId && !child.h) child.h = OpenProcess(SYNCHRONIZE, FALSE, snap.deviceId);
        if (snap.deviceId != observedPid || snap.stage != observedStage) {
            observedStage=snap.stage;
            observedPid=snap.deviceId;
            record("snapshot pid=",observedPid," opened=",bool(child.h)," stage=",int(observedStage),
                " samples=",snap.measuredCount," reason=",int(snap.reason));
        }
        if (!entered && target && snap.deviceId && snap.stage == *target) entered = Clock::now();
        if (entered && Clock::now() > *entered + stageDuration + 250ms && live(worker.poll()))
            return failed("worker remained active past the acknowledged stage deadline");
        if (cancel && child.h && entered && (scenario != resetHang || snap.reason == BenchmarkReason::GraphicsReset)) {
            const auto click = Clock::now();
            worker.cancel();
            if (worker.poll() != ResultsWorkerState::cancelled) return failed("Cancel did not freeze result");
            const auto cancelTime=Clock::now()-click;
            record("Cancel returned in ",std::chrono::duration_cast<std::chrono::milliseconds>(cancelTime).count(),"ms");
            if (cancelTime > 250ms) return failed("Cancel blocked UI");
            if (duplicate.start({})) return failed("admitted a second child before exit");
            if (!pump(wnd)) return failed("UI message after Cancel did not dispatch");
            break;
        }
        Sleep(4);
    }
    const auto state = worker.poll();
    const auto snap = worker.snapshot();
    record("terminal state=",int(state)," stage=",int(snap.stage)," outcome=",int(snap.outcome),
        " reason=",int(snap.reason)," count=",snap.measuredCount," median=",snap.medianMs," maximum=",snap.worstMs);
    auto wanted = ResultsWorkerState::timedOut;
    if (cancel) wanted = ResultsWorkerState::cancelled;
    else if (scenario == success) wanted = ResultsWorkerState::completed;
    else if (scenario == crash || scenario == disconnect) wanted = ResultsWorkerState::failed;
    if (state != wanted || (scenario == success ? snap.outcome != ResultsOutcome::Completed :
        snap.outcome == ResultsOutcome::Completed || snap.medianMs != 0 || snap.worstMs != 0)) {
        std::cerr << "Scenario " << scenario << " ended in " << int(state) << " expected " << int(wanted) << "\n";
        return failed("wrong terminal result");
    }
    if (target && (!entered || !child.h || snap.stage != *target))
        return failed("target stall never acknowledged by the expected child");
    if (entered) record("terminal observed ",std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-*entered).count(),
        "ms after acknowledged target stage");
    if (child.h) {
        const auto startWait=Clock::now();
        const auto reapDeadline=startWait+8s;
        DWORD wait=WAIT_TIMEOUT;
        while (wait==WAIT_TIMEOUT && Clock::now()<reapDeadline) {
            if (!pump(wnd)) return failed("window stopped during asynchronous child reaping");
            wait=WaitForSingleObject(child.h,8);
        }
        const auto requested=killRequestAfterStop.load();
        record("job kill requested after ",requested,"ms, API result=",jobKillError.load(),"; process exit wait=",wait," elapsed=",
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-startWait).count(),"ms");
        // A one-second grace bounds the kill request. Windows completes process retirement asynchronously.
        if (target && (requested<0 || requested>1050)) return failed("job kill request outside one-second grace (50ms timer tolerance)");
        if (wait!=WAIT_OBJECT_0) return failed("enclosed child never reaped during bounded observation");
        if (target && jobKillError.load()>0) return failed("TerminateJobObject failed");
    }
    if (scenario == resetHang && (snap.reason != BenchmarkReason::GraphicsReset ||
        snap.outcome != ResultsOutcome::Interrupted || snap.measuredCount != 9)) return false;
    // Cancellation must freeze the exact snapshot even after an uncooperative process dies.
    if (worker.poll()!=state || worker.snapshot().measuredCount!=snap.measuredCount) return failed("result changed after terminal state");
    record("PASS scenario=",int(scenario));
    return true;
}
int orphanOwner(const std::filesystem::path& path, bool packaged) {
    ResultsWorker worker;
    if (!startWhenFree(worker, orphan, packaged)) return 1;
    auto until = Clock::now() + 2s;
    while (Clock::now() < until) {
        if (auto pid = worker.snapshot().deviceId) {
            std::ofstream(path) << pid;
            Sleep(10000);
            return 2;
        }
        Sleep(5);
    }
    return 3;
}
bool testAbruptClose(bool packaged) {
    wchar_t temp[MAX_PATH]{}, path[MAX_PATH]{}, exe[32768]{};
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"brc", 0, path) ||
        !GetModuleFileNameW(nullptr, exe, 32768)) return false;
    struct Cleanup { wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{path};
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --orphan-owner \"" + path +
        L"\" --no-activate --mute" + (packaged ? L" --expect-package" : L"");
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    Handle owner{pi.hProcess}, thread{pi.hThread}, child;
    const auto until = Clock::now() + 3s;
    while (Clock::now() < until && !child.h) {
        DWORD pid{};
        std::ifstream(std::filesystem::path(path)) >> pid;
        if (pid) child.h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        Sleep(5);
    }
    if (!TerminateProcess(owner.h, 1)) return false;
    return child.h && WaitForSingleObject(owner.h, 1200) == WAIT_OBJECT_0 &&
        WaitForSingleObject(child.h, 1200) == WAIT_OBJECT_0;
}
} // namespace

namespace boo {
void observeJobKillRequest(std::chrono::milliseconds elapsed) noexcept { killRequestAfterStop.store(elapsed.count()); }
void observeJobKillReturn(bool succeeded,DWORD error) noexcept { jobKillError.store(succeeded ? 0 : error); }
void runResultsBenchmark(const BenchmarkRequest& request, BenchmarkReporter& reporter) {
    if (request.luidHigh && !packageIdentity()) return;
    const auto scenario = static_cast<Scenario>(request.luidLow);
    BenchmarkSnapshot next{};
    next.mode = request.mode;
    next.deviceId = GetCurrentProcessId(); // Synthetic test data to observe actual process exit.
    if (!reporter.publish(next)) return;
    if (scenario == finishedHang && request.viewerLocalUsage == 4500) Sleep(4500); // Inside the 18-second Preparing bound.
    if (scenario == initHang || scenario == orphan) Sleep(INFINITE);
    if (scenario == disconnect) { CloseHandle(progressPipe); Sleep(INFINITE); }
    if (scenario == crash) ExitProcess(7);
    next.stage = BenchmarkStage::Warming;
    if (!reporter.publish(next)) return;
    if (scenario == warmHang) Sleep(INFINITE);
    next.warmupCount = kResultsWarmupCount;
    next.stage = BenchmarkStage::Measuring;
    if (!reporter.publish(next)) return;
    if (scenario == submitHang) Sleep(INFINITE);
    next.stage = BenchmarkStage::Finishing;
    next.measuredCount = scenario == resetHang ? 9 : kResultsMeasuredCount;
    if (scenario == resetHang) { next.reason = BenchmarkReason::GraphicsReset; next.outcome = ResultsOutcome::Interrupted; }
    if (!reporter.publish(next)) return;
    if (scenario == teardownHang || scenario == resetHang) Sleep(INFINITE);
    next.stage = BenchmarkStage::Finished;
    next.outcome = ResultsOutcome::Completed;
    next.medianMs = 1;
    next.worstMs = 3;
    if (scenario == success) { reporter.finish(next); return; }
    reporter.publish(next);
    if (scenario == finishedHang) Sleep(INFINITE);
}
} // namespace boo

int main(int argc, char** argv) {
    if (argc > 3 && std::string_view(argv[1]) == "--internal-results-worker")
        progressPipe = reinterpret_cast<HANDLE>(std::strtoull(argv[3], nullptr, 10));
    if (const int child = runResultsWorkerIfRequested(argc, argv); child >= 0) return child;
    bool packaged = false;
    bool repeatFinish = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--expect-package") packaged = true;
        if (arg == "--repeat-finish") repeatFinish = true;
        if (arg == "--delayed-finish") delayedPreparation = true;
        if (arg == "--receipt" && i+1<argc) {
            receipt.open(std::filesystem::path(argv[++i]),std::ios::out);
            if (!receipt) return 24;
        }
    }
    record("identity confirmed=",packageIdentity()," required=",packaged);
    if (packaged && !packageIdentity()) return 20;
    if (argc > 2 && std::string_view(argv[1]) == "--orphan-owner")
        return orphanOwner(std::filesystem::path(argv[2]), packaged);
    const auto klass = L"BooBenchmarkContainmentWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = klass;
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, klass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!wnd) return 21;
    if (delayedPreparation) return testScenario(finishedHang,packaged,wnd,false) ? 0 : 35;
    if (repeatFinish) {
        for (unsigned i=0;i<10;++i) if (!testScenario(finishedHang,packaged,wnd,false)) return 35;
        record("PASS repeated provisional-finish hang");
        return 0;
    }
    // A cancellation issued during process startup must be terminal without waiting for launch.
    {
        ResultsWorker worker;
        if (!worker.start({})) return 22;
        worker.cancel();
        if (worker.poll() != ResultsWorkerState::cancelled || !pump(wnd)) return 23;
    }
    int number = 0;
    for (auto scenario : {success, initHang, warmHang, submitHang, teardownHang, finishedHang, crash, disconnect}) {
        if (!testScenario(scenario, packaged, wnd, false)) return 30+number;
        ++number;
    }
    if (!testScenario(initHang, packaged, wnd, true)) return 40;
    if (!testScenario(submitHang, packaged, wnd, true)) return 41;
    if (!testScenario(teardownHang, packaged, wnd, true)) return 42;
    if (!testScenario(resetHang, packaged, wnd, true)) return 45;
    if (!testAbruptClose(packaged)) return 43;
    if (!testScenario(success, packaged, wnd, false)) return 44;
    delayedPreparation = true;
    if (!testScenario(finishedHang, packaged, wnd, false)) return 46;
    DestroyWindow(wnd);
    record("PASS containment matrix, abrupt parent exit and fresh run");
    std::cout << "Contained initialization, submission, cleanup, crash, disconnection, cancellation and abrupt parent exit\n";
    return 0;
}
