#include "boo/physx_suite.hpp"

#include "boo/logging.hpp"
#include "boo/startup_progress.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <iterator>
#include <thread>

#if defined(BOO_HAS_PHYSX)
#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#pragma warning(pop)
#endif

namespace boo {

#if defined(BOO_HAS_PHYSX)
namespace {

class LogErrorCallback final : public physx::PxErrorCallback {
public:
    void reportError(physx::PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        const std::string text = std::string("PhysX: ") + (message ? message : "") + " (" + (file ? file : "?") + ":" +
                                 std::to_string(line) + ")";
        if (code == physx::PxErrorCode::eDEBUG_INFO) BOO_LOG_DEBUG("PhysX", text);
        else if (code == physx::PxErrorCode::eDEBUG_WARNING || code == physx::PxErrorCode::ePERF_WARNING) BOO_LOG_WARN("PhysX", text);
        else BOO_LOG_ERROR("PhysX", text);
    }
};

// The CUDA driver, loaded at run time like the rest of the app does, just enough to find the GPU the
// film renders on and retain its primary context.
struct CudaDriver {
    HMODULE module = nullptr;
    int (*cuInit)(unsigned) = nullptr;
    int (*cuDeviceGetCount)(int*) = nullptr;
    int (*cuDeviceGet)(int*, int) = nullptr;
    int (*cuDeviceGetLuid)(char*, unsigned*, int) = nullptr;
    int (*cuDevicePrimaryCtxRetain)(void**, int) = nullptr;
    int (*cuDevicePrimaryCtxRelease)(int) = nullptr;
    int (*cuCtxSetCurrent)(void*) = nullptr;
    template <typename F> bool sym(F& f, const char* name) {
        f = reinterpret_cast<F>(GetProcAddress(module, name));
        return f != nullptr;
    }
    bool load() {
        module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module && sym(cuInit, "cuInit") && sym(cuDeviceGetCount, "cuDeviceGetCount") && sym(cuDeviceGet, "cuDeviceGet") &&
               sym(cuDeviceGetLuid, "cuDeviceGetLuid") && sym(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain") &&
               sym(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2") && sym(cuCtxSetCurrent, "cuCtxSetCurrent");
    }
    void unload() {
        if (module) FreeLibrary(module);
        module = nullptr;
    }
};

} // namespace
#endif

struct PhysxSuite::Impl {
    int32_t luidLow = 0, luidHigh = 0;
    mutable std::mutex mutex;
    std::condition_variable settled;
    PhysxRuntimeInfo published;
    std::thread worker;

#if defined(BOO_HAS_PHYSX)
    physx::PxDefaultAllocator allocator;
    LogErrorCallback errors;
    physx::PxFoundation* foundation = nullptr;
    physx::PxPhysics* physics = nullptr;
    physx::PxDefaultCpuDispatcher* dispatcher = nullptr;
    physx::PxCudaContextManager* cuda = nullptr;
    CudaDriver driver;
    void* primaryContext = nullptr;
    int cudaDevice = -1;

    void start();
    void release();
#endif
};

#if defined(BOO_HAS_PHYSX)
void PhysxSuite::Impl::start() {
    const auto began = std::chrono::steady_clock::now();
    PhysxRuntimeInfo result;
    auto finish = [&](PhysxRuntimeState state, const std::string& reason) {
        result.state = state;
        result.reason = reason;
        result.startMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count();
        {
            std::lock_guard<std::mutex> lock(mutex);
            published = result;
        }
        settled.notify_all();
        if (state == PhysxRuntimeState::Ready) {
            BOO_LOG_INFO("PhysX", "Runtime ready in " + std::to_string(static_cast<int>(result.startMs)) + " ms: PhysX " +
                                  result.sdkVersion + (result.gpu ? ", GPU solvers on " + result.cudaDevice +
                                                                        (result.sharedContext ? " (shared CUDA context)" : "")
                                                                  : ", CPU solvers only"));
        } else {
            BOO_LOG_WARN("PhysX", "Runtime unavailable: " + reason);
        }
    };

    foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errors);
    if (!foundation) { finish(PhysxRuntimeState::Unavailable, "PhysX foundation could not be created"); return; }
    result.sdkVersion = std::to_string(PX_PHYSICS_VERSION_MAJOR) + "." + std::to_string(PX_PHYSICS_VERSION_MINOR) + "." +
                        std::to_string(PX_PHYSICS_VERSION_BUGFIX);

    // The film's GPU and its primary CUDA context: the same context the app's CUDA work runs in.
    if (driver.load() && driver.cuInit(0) == 0) {
        int count = 0;
        driver.cuDeviceGetCount(&count);
        for (int i = 0; i < count && cudaDevice < 0; ++i) {
            int device = -1;
            char luid[8]{};
            unsigned nodeMask = 0;
            if (driver.cuDeviceGet(&device, i) != 0 || driver.cuDeviceGetLuid(luid, &nodeMask, device) != 0) continue;
            LUID wanted{};
            wanted.LowPart = static_cast<DWORD>(luidLow);
            wanted.HighPart = static_cast<LONG>(luidHigh);
            if (std::memcmp(luid, &wanted, sizeof(LUID)) == 0) cudaDevice = device;
        }
        if (cudaDevice >= 0 && driver.cuDevicePrimaryCtxRetain(&primaryContext, cudaDevice) != 0) {
            primaryContext = nullptr;
            cudaDevice = -1;
        }
    }

    physx::PxTolerancesScale scale; // metres, kilograms, seconds
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale, false, nullptr);
    if (!physics) { finish(PhysxRuntimeState::Unavailable, "PhysX could not be created"); return; }
    const unsigned cores = std::thread::hardware_concurrency();
    dispatcher = physx::PxDefaultCpuDispatcherCreate(cores > 4u ? 4u : (cores > 1u ? cores - 1u : 1u));
    if (!dispatcher) { finish(PhysxRuntimeState::Unavailable, "PhysX CPU dispatcher could not be created"); return; }

    if (primaryContext) {
        physx::PxCudaContextManagerDesc desc;
        CUcontext context = static_cast<CUcontext>(primaryContext);
        desc.ctx = &context;
        // PhysX reads the device from the calling thread's current context, so make it current here.
        driver.cuCtxSetCurrent(primaryContext);
        cuda = PxCreateCudaContextManager(*foundation, desc, PxGetProfilerCallback());
        if (cuda && cuda->contextIsValid()) {
            result.gpu = true;
            result.sharedContext = true;
            result.cudaDevice = cuda->getDeviceName();
        } else {
            if (cuda) cuda->release();
            cuda = nullptr;
            BOO_LOG_WARN("PhysX", "GPU solvers unavailable (PhysXGpu_64.dll or the CUDA context); CPU solvers only.");
        }
    }
    // Count the SDK's libraries on the loading bar when this happens during start-up (no-op later).
    for (const wchar_t* name : {L"PhysX_64.dll", L"PhysXCommon_64.dll", L"PhysXFoundation_64.dll", L"PhysXGpu_64.dll"}) {
        if (HMODULE loaded = GetModuleHandleW(name)) {
            wchar_t path[MAX_PATH * 2]{};
            if (GetModuleFileNameW(loaded, path, static_cast<DWORD>(std::size(path))) > 0) startupAssetLoaded(path);
        }
    }
    finish(PhysxRuntimeState::Ready, "");
}

void PhysxSuite::Impl::release() {
    if (cuda) cuda->release();
    if (dispatcher) dispatcher->release();
    if (physics) physics->release();
    if (foundation) foundation->release();
    cuda = nullptr;
    dispatcher = nullptr;
    physics = nullptr;
    foundation = nullptr;
    if (primaryContext && cudaDevice >= 0) driver.cuDevicePrimaryCtxRelease(cudaDevice);
    primaryContext = nullptr;
    cudaDevice = -1;
    driver.unload();
}
#endif

PhysxSuite::PhysxSuite(int32_t adapterLuidLow, int32_t adapterLuidHigh) : impl_(std::make_unique<Impl>()) {
    impl_->luidLow = adapterLuidLow;
    impl_->luidHigh = adapterLuidHigh;
#if defined(BOO_HAS_PHYSX)
    impl_->published.state = PhysxRuntimeState::Stopped;
#else
    impl_->published.reason = "This build does not include the NVIDIA PhysX SDK";
#endif
}

PhysxSuite::~PhysxSuite() { stop(); }

void PhysxSuite::requestStart() {
#if defined(BOO_HAS_PHYSX)
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->published.state != PhysxRuntimeState::Stopped) return;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->published.state = PhysxRuntimeState::Starting;
    impl_->worker = std::thread([this] { impl_->start(); });
#endif
}

void PhysxSuite::stop() {
#if defined(BOO_HAS_PHYSX)
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool wasRunning = impl_->published.state == PhysxRuntimeState::Ready;
    impl_->release();
    impl_->published = PhysxRuntimeInfo{};
    impl_->published.state = PhysxRuntimeState::Stopped;
    if (wasRunning) BOO_LOG_INFO("PhysX", "Runtime released.");
#endif
}

bool PhysxSuite::waitUntilSettled(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->settled.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [&] { return impl_->published.state != PhysxRuntimeState::Starting; });
}

PhysxRuntimeInfo PhysxSuite::info() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->published;
}

void* PhysxSuite::physics() const noexcept {
#if defined(BOO_HAS_PHYSX)
    return info().state == PhysxRuntimeState::Ready ? impl_->physics : nullptr;
#else
    return nullptr;
#endif
}
void* PhysxSuite::dispatcher() const noexcept {
#if defined(BOO_HAS_PHYSX)
    return info().state == PhysxRuntimeState::Ready ? impl_->dispatcher : nullptr;
#else
    return nullptr;
#endif
}
void* PhysxSuite::cudaContextManager() const noexcept {
#if defined(BOO_HAS_PHYSX)
    return info().state == PhysxRuntimeState::Ready ? impl_->cuda : nullptr;
#else
    return nullptr;
#endif
}

bool PhysxSuite::selfTest(std::string* reason) {
#if !defined(BOO_HAS_PHYSX)
    if (reason) *reason = impl_->published.reason;
    return false;
#else
    using namespace physx;
    if (info().state != PhysxRuntimeState::Ready) {
        if (reason) *reason = "runtime not ready";
        return false;
    }
    PxSceneDesc desc(impl_->physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = impl_->dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    if (impl_->cuda) {
        desc.cudaContextManager = impl_->cuda;
        desc.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS;
        desc.broadPhaseType = PxBroadPhaseType::eGPU;
    }
    PxScene* scene = impl_->physics->createScene(desc);
    if (!scene) {
        if (reason) *reason = "scene creation failed";
        return false;
    }
    PxMaterial* material = impl_->physics->createMaterial(0.5f, 0.5f, 0.3f);
    PxRigidStatic* ground = PxCreatePlane(*impl_->physics, PxPlane(0, 1, 0, 0), *material);
    PxRigidDynamic* box = PxCreateDynamic(*impl_->physics, PxTransform(PxVec3(0, 2, 0)), PxBoxGeometry(0.5f, 0.5f, 0.5f), *material, 10.0f);
    scene->addActor(*ground);
    scene->addActor(*box);
    for (int i = 0; i < 120; ++i) {
        scene->simulate(1.0f / 60.0f);
        scene->fetchResults(true);
    }
    const float height = box->getGlobalPose().p.y;
    scene->removeActor(*box);
    scene->removeActor(*ground);
    box->release();
    ground->release();
    scene->release();
    material->release();
    // The box must have fallen and come to rest on the ground, half its height above it.
    if (height < 0.4f || height > 0.6f) {
        if (reason) *reason = "box came to rest at " + std::to_string(height) + " m";
        return false;
    }
    return true;
#endif
}

} // namespace boo
