#include "boo/results_worker.hpp"

#include <windows.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace boo {
#ifdef BOO_RESULTS_CONTAINMENT_TEST
void observeJobKillRequest(std::chrono::milliseconds elapsed) noexcept;
void observeJobKillReturn(bool succeeded, DWORD error) noexcept;
#endif
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
std::atomic<bool> workerActive{false};
constexpr DWORD protocol = 0x31435242; // BRC1
enum : LONG { permitted, admitted, revoked };
struct Channel {
    DWORD version;
    volatile LONG permit;
    BenchmarkRequest request;
};

enum class MessageType : DWORD { ready, progress };
struct Message {
    MessageType type;
    BenchmarkSnapshot snapshot;
};

struct Handle {
    HANDLE value = nullptr;
    void reset() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = nullptr;
    }
    ~Handle() { reset(); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Mapping {
    Channel* channel = nullptr;
    ~Mapping() { if (channel) UnmapViewOfFile(channel); }
};
struct JobReaper {
    HANDLE job{};
    PTP_TIMER timer{};
    std::atomic<Clock::duration::rep> stoppedAt{};
    std::atomic<bool> requested{false};

    static void CALLBACK fire(PTP_CALLBACK_INSTANCE, void* argument, PTP_TIMER) noexcept {
        static_cast<JobReaper*>(argument)->terminate();
    }
    bool initialize(HANDLE ownedJob) {
        job=ownedJob;
        timer=CreateThreadpoolTimer(fire,this,nullptr);
        return timer!=nullptr;
    }
    void arm(Clock::time_point stop, std::chrono::milliseconds grace) noexcept {
        stoppedAt.store(stop.time_since_epoch().count());
        LARGE_INTEGER relative{};
        relative.QuadPart=-(std::max)(1LL,static_cast<long long>(grace.count())*10000LL);
        FILETIME due{relative.LowPart,static_cast<DWORD>(relative.HighPart)};
        SetThreadpoolTimer(timer,&due,0,0);
    }
    void terminate() noexcept {
        if (requested.exchange(true)) return;
#ifdef BOO_RESULTS_CONTAINMENT_TEST
        const Clock::time_point stop{Clock::duration{stoppedAt.load()}};
        observeJobKillRequest(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-stop));
#endif
        // The supervisor keeps the job handle alive until both the child and the callback have exited.
        const BOOL killed=TerminateJobObject(job,1);
#ifdef BOO_RESULTS_CONTAINMENT_TEST
        observeJobKillReturn(killed!=FALSE,killed ? ERROR_SUCCESS : GetLastError());
#else
        (void)killed;
#endif
    }
    ~JobReaper() {
        if (!timer) return;
        SetThreadpoolTimer(timer,nullptr,0,0);
        WaitForThreadpoolTimerCallbacks(timer,TRUE);
        CloseThreadpoolTimer(timer);
    }
};
struct Attributes {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    bool initialize(HANDLE (&inherited)[2], HANDLE job) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        storage.resize(size);
        auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 2, 0, &size)) return false;
        list = candidate;
        job_ = job;
        // JOB_LIST makes containment atomic with process creation, even if the parent dies in CreateProcess.
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                   inherited, sizeof(inherited), nullptr, nullptr) &&
               UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                   &job_, sizeof(job_), nullptr, nullptr);
    }
    HANDLE job_{}; // Attribute values stay valid until after CreateProcess.
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};
bool active(ResultsWorkerState state) {
    return state == ResultsWorkerState::starting || state == ResultsWorkerState::running;
}
bool parseHandle(std::string_view text, HANDLE& result) {
    std::uintptr_t value{};
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), value);
    if (!value || parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) return false;
    result = reinterpret_cast<HANDLE>(value);
    return true;
}
bool send(HANDLE pipe, const Message& message) {
    DWORD count{};
    return WriteFile(pipe, &message, sizeof(message), &count, nullptr) && count == sizeof(message);
}
struct Reporter final : BenchmarkReporter {
    Reporter(Channel* control, HANDLE writer) : channel(control), pipe(writer) {}
    bool cancelled() const override {
        return InterlockedCompareExchange(&channel->permit, admitted, admitted) != admitted;
    }
    bool publish(const BenchmarkSnapshot& snapshot) override {
        return !cancelled() && send(pipe, {MessageType::progress, snapshot});
    }
    void finish(const BenchmarkSnapshot& snapshot) override {
        // The backend has retired the queue. Windows reclaims this private process's D3D/NGX
        // handles on normal exit. The supervisor verifies the actual exit inside its original deadline.
        ExitProcess(publish(snapshot) ? 0 : 2);
    }
    Channel* channel;
    HANDLE pipe;
};
bool validUpdate(const BenchmarkSnapshot& next, const BenchmarkSnapshot& prev) {
    if (next.mode != prev.mode || next.stage < prev.stage || next.stage > BenchmarkStage::Finished ||
        prev.stage == BenchmarkStage::Finished || next.warmupCount < prev.warmupCount ||
        next.measuredCount < prev.measuredCount || next.warmupCount > kResultsWarmupCount ||
        next.measuredCount > kResultsMeasuredCount || next.reason > BenchmarkReason::DlssInitFailed ||
        next.outcome > ResultsOutcome::Unavailable || next.power > BenchmarkPower::Changed) return false;
    for (const auto feature : next.evidence) if (feature > BenchmarkEvidence::Failed) return false;
    if (next.outcome != ResultsOutcome::Completed) return true;
    return next.stage == BenchmarkStage::Finished && next.reason == BenchmarkReason::None &&
        next.warmupCount == kResultsWarmupCount && next.measuredCount == kResultsMeasuredCount &&
        std::isfinite(next.medianMs) && std::isfinite(next.worstMs) &&
        next.medianMs >= 0 && next.worstMs >= next.medianMs;
}
} // namespace

struct ResultsWorker::Shared {
    std::mutex mutex;
    ResultsWorkerState state = ResultsWorkerState::idle;
    BenchmarkSnapshot last{};
    Clock::time_point absolute{}, stageLimit{}, frameLimit = (Clock::time_point::max)();
    Clock::time_point stopped{};
    Channel* channel = nullptr; // Published and detached under mutex; the supervisor owns the mapping.
    JobReaper* reaper = nullptr; // Armed without waiting for process creation, pipe reads or the supervisor.
    static std::chrono::milliseconds graceFor(ResultsWorkerState terminal) {
        return terminal == ResultsWorkerState::cancelled ? 200ms : 0ms;
    }

    void stopLocked(ResultsWorkerState terminal, BenchmarkReason reason) {
        if (!active(state)) return;
        if (channel) InterlockedExchange(&channel->permit, revoked);
        stopped = Clock::now();
        if (reaper) reaper->arm(stopped,graceFor(terminal));
        state = terminal;
        // Once the worker has observed device removal, a later Cancel, pipe loss or deadline
        // must keep the session on the reset card, even if it wins the race with cleanup.
        if (last.reason != BenchmarkReason::GraphicsReset) last.reason = reason;
        last.outcome = last.reason == BenchmarkReason::Cancelled ? ResultsOutcome::Cancelled : ResultsOutcome::Interrupted;
        last.medianMs = last.worstMs = 0;
    }
    bool checkLocked() {
        const auto now = Clock::now();
        if (active(state) && (now >= absolute || now >= stageLimit || now >= frameLimit))
            stopLocked(ResultsWorkerState::timedOut, BenchmarkReason::TimeLimit);
        return active(state);
    }
    void fail() {
        std::lock_guard lock(mutex);
        if (checkLocked()) stopLocked(ResultsWorkerState::failed, BenchmarkReason::WorkerExited);
    }
    bool receive(const Message& message) {
        std::lock_guard lock(mutex);
        if (!checkLocked()) return false;
        const auto now = Clock::now();
        if (state == ResultsWorkerState::starting && message.type == MessageType::ready) {
            state = ResultsWorkerState::running;
            stageLimit = now + 18s;
            return true;
        }
        const auto& next = message.snapshot;
        if (state != ResultsWorkerState::running || message.type != MessageType::progress || !validUpdate(next, last)) {
            stopLocked(ResultsWorkerState::failed, BenchmarkReason::WorkerExited);
            return false;
        }
        if (next.stage != last.stage) {
            switch (next.stage) {
            case BenchmarkStage::Warming: stageLimit = now + 5s; break;
            case BenchmarkStage::Measuring: stageLimit = now + 20s; break;
            case BenchmarkStage::Finishing: stageLimit = now + 2s; break;
            case BenchmarkStage::Finished: stageLimit = (std::min)(stageLimit, now + 2s); break;
            default: break;
            }
        }
        if (next.stage == BenchmarkStage::Warming || next.stage == BenchmarkStage::Measuring) {
            if (next.stage != last.stage || next.warmupCount != last.warmupCount || next.measuredCount != last.measuredCount)
                frameLimit = now + 2s;
        } else frameLimit = (Clock::time_point::max)();
        last = next;
        return true;
    }
    void supervise(BenchmarkRequest request) {
        Handle job;
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return fail();
        JobReaper killer;
        if (!killer.initialize(job.value)) return fail();
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle read, write, section;
        if (!CreatePipe(&read.value, &write.value, &security, sizeof(Message)*8) ||
            !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) return fail();
        section.value = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Channel), nullptr);
        Mapping mapping;
        if (!section.value) return fail();
        mapping.channel = static_cast<Channel*>(MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, sizeof(Channel)));
        if (!mapping.channel) return fail();
        *mapping.channel = {protocol, permitted, request};
        HANDLE inherited[] = {section.value, write.value};
        Attributes attributes;
        if (!attributes.initialize(inherited, job.value)) return fail();
        wchar_t exe[32768]{};
        const auto len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (!len || len >= std::size(exe)) return fail();
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --internal-results-worker " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(section.value)) + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(write.value)) + L" --no-activate --mute";
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION pi{};
        {
            std::lock_guard lock(mutex);
            if (!checkLocked()) return;
            channel = mapping.channel;
            reaper = &killer;
        }
        struct Detach {
            Shared& owner;
            ~Detach() {
                std::lock_guard lock(owner.mutex);
                owner.channel = nullptr;
                owner.reaper = nullptr;
            }
        } detach{*this};
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &pi)) return fail();
        Handle process, thread;
        process.value = pi.hProcess;
        thread.value = pi.hThread;
        write.reset();
        bool revokedDuringCreation;
        { std::lock_guard lock(mutex); revokedDuringCreation = !checkLocked(); }
        // A timer can fire while CreateProcess is still assigning its handles. If this late
        // process exists after revocation, kill that exact child before monitoring its exit.
        if (revokedDuringCreation) TerminateProcess(process.value,1);
        while (true) {
            {
                std::lock_guard lock(mutex);
                if (!checkLocked()) break;
            }
            DWORD available{};
            BOOL connected = PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr);
            if (connected && available >= sizeof(Message)) {
                Message message{};
                DWORD count{};
                if (!ReadFile(read.value, &message, sizeof(message), &count, nullptr) ||
                    count != sizeof(message) || !receive(message)) break;
                continue;
            }
            if (WaitForSingleObject(process.value, connected ? 8 : 0) == WAIT_OBJECT_0) {
                // An exit can race the empty-pipe observation; drain any final complete message.
                if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available >= sizeof(Message)) continue;
                DWORD code{};
                const BOOL queried = GetExitCodeProcess(process.value, &code);
                std::lock_guard lock(mutex);
                if (checkLocked()) {
                    if (queried && code == 0 && last.stage == BenchmarkStage::Finished) state = ResultsWorkerState::completed;
                    else stopLocked(ResultsWorkerState::failed, BenchmarkReason::WorkerExited);
                }
                return;
            }
            // The runtime can close inherited handles shortly before process exit.
            if (!connected) {
                bool finished;
                { std::lock_guard lock(mutex); finished = last.stage == BenchmarkStage::Finished; }
                if (finished) { Sleep(2); continue; }
                break;
            }
        }
        fail();
        Clock::time_point grace;
        { std::lock_guard lock(mutex); grace = stopped + graceFor(state); }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(grace - Clock::now()).count();
        if (remaining > 0 && WaitForSingleObject(process.value, static_cast<DWORD>(remaining)) == WAIT_OBJECT_0) return;
        // Either the independent timer or this supervisor can initiate termination first.
        // KILL_ON_JOB_CLOSE also covers an abrupt parent exit at any point.
        killer.terminate();
        // Retain a direct process-handle fallback if Windows fails or delays job termination.
        if (WaitForSingleObject(process.value,100)==WAIT_TIMEOUT) TerminateProcess(process.value,1);
        while (WaitForSingleObject(process.value, 200) == WAIT_TIMEOUT) {}
    }
};

ResultsWorker::ResultsWorker() : shared_(std::make_shared<Shared>()) {}
ResultsWorker::~ResultsWorker() { cancel(); }

bool ResultsWorker::start(const BenchmarkRequest& request) {
    if (request.mode > BenchmarkMode::DlssQuality) return false;
    {
        std::lock_guard lock(shared_->mutex);
        if (shared_->state != ResultsWorkerState::idle || workerActive.exchange(true)) return false;
        shared_->state = ResultsWorkerState::starting;
        shared_->last.mode = request.mode;
        auto start = Clock::now();
        shared_->absolute = start + 50s;
        shared_->stageLimit = start + 2s;
    }
    try {
        std::thread([shared = shared_, request] {
            try { shared->supervise(request); } catch (...) { shared->fail(); }
            workerActive.store(false);
        }).detach();
        return true;
    } catch (...) {
        shared_->fail();
        workerActive.store(false);
        return false;
    }
}
void ResultsWorker::cancel() {
    std::lock_guard lock(shared_->mutex);
    if (shared_->checkLocked()) shared_->stopLocked(ResultsWorkerState::cancelled, BenchmarkReason::Cancelled);
}
ResultsWorkerState ResultsWorker::poll() {
    std::lock_guard lock(shared_->mutex);
    shared_->checkLocked();
    return shared_->state;
}
BenchmarkSnapshot ResultsWorker::snapshot() const {
    std::lock_guard lock(shared_->mutex);
    shared_->checkLocked();
    auto result = shared_->last;
    // Final durations are provisional until the child finishes cleanup and exits normally.
    if (active(shared_->state) && result.outcome == ResultsOutcome::Completed) {
        result.outcome = ResultsOutcome::Unavailable;
        result.medianMs = result.worstMs = 0;
    }
    return result;
}

int runResultsWorkerIfRequested(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) != "--internal-results-worker") return -1;
    if (argc != 6 || std::string_view(argv[4]) != "--no-activate" || std::string_view(argv[5]) != "--mute") return 2;
    HANDLE section{}, writer{};
    if (!parseHandle(argv[2], section) || !parseHandle(argv[3], writer) || GetFileType(writer) != FILE_TYPE_PIPE) return 2;
    BOOL inJob{};
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) return 2;
    Mapping mapping;
    mapping.channel = static_cast<Channel*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(Channel)));
    if (!mapping.channel || mapping.channel->version != protocol || mapping.channel->request.mode > BenchmarkMode::DlssQuality) return 2;
    // Cancel and workload admission are ordered by the same interprocess atomic. This also covers a
    // cancellation that happens inside CreateProcessW, before the supervisor gets a process handle.
    if (InterlockedCompareExchange(&mapping.channel->permit, admitted, permitted) != permitted) return 2;
    Reporter reporter(mapping.channel, writer);
    if (!send(writer, {MessageType::ready, {}}) || reporter.cancelled()) return 2;
    try { runResultsBenchmark(mapping.channel->request, reporter); } catch (...) { return 2; }
    return 0;
}
} // namespace boo
