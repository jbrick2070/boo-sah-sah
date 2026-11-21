#include "boo/frame_generation.hpp"
#include "boo/startup_progress.hpp"
#include "boo/m1_scene.hpp"
#include "boo/logging.hpp"
#include <windows.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstring>
#ifdef BOO_HAS_STREAMLINE
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_security.h>
#endif

namespace boo {
struct FrameGeneration::Impl {
    FrameGenerationStats stats{};
#ifdef BOO_HAS_STREAMLINE
    HMODULE module=nullptr;
    bool initialized=false,fg=false,reflex=false,simOpen=false,renderOpen=false,tagged=false,history=false;
    bool sceneValid=false,reflexActive=false,dynamicSupported=false,optionDynamic=false;
    uint32_t optionFrames=0,frameIndex=0,minDimension=0,lastStatus=0;
    sl::Result lastTagResult=sl::Result::eOk;
    FrameGenerationMode requested=FrameGenerationMode::Off;
    ReflexMode lastReflex=ReflexMode::Off;
    sl::FrameToken* token=nullptr;
    sl::ViewportHandle viewport{0};
    std::filesystem::path pluginFolder;
    PFun_slInit* init{};PFun_slShutdown* shutdown{};PFun_slSetD3DDevice* device{};
    PFun_slIsFeatureSupported* supports{};PFun_slUpgradeInterface* upgrade{};
    PFun_slGetFeatureFunction* featureFn{};PFun_slGetNewFrameToken* newToken{};
    PFun_slSetTagForFrame* setTag{};PFun_slSetConstants* constants{};
    PFun_slDLSSGSetOptions* fgOptions{};PFun_slDLSSGGetState* fgState{};
    PFun_slReflexSetOptions* reflexOptions{};PFun_slReflexGetState* reflexState{};
    PFun_slReflexSleep* sleep{};PFun_slPCLSetMarker* mark{};
    sl::Constants camera{};
    DirectX::XMFLOAT4X4 previousViewProjection{};
    ID3D12Resource* depth=nullptr;ID3D12Resource* motion=nullptr;
    uint32_t inputW=0,inputH=0;
    std::chrono::steady_clock::time_point sampled{};
    uint32_t baseSamples=0,displaySamples=0;
    void clearPresentationMeasurements() {
        stats.generatedPerFrame=0;
        stats.framesActuallyPresented=0;
        stats.displayedFps=0;
        stats.renderedFps=0;
        sampled={};
        baseSamples=displaySamples=0;
    }
    bool setFrames(uint32_t n,bool dynamic=false) {
        if (!fgOptions) return false;
        if (n==optionFrames && dynamic==optionDynamic) return true;
        sl::DLSSGOptions options{};
        options.mode=dynamic ? sl::DLSSGMode::eDynamic : n ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate=std::max(1u,n);
        options.enableUserInterfaceRecomposition=sl::Boolean::eTrue;
        if (fgOptions(viewport,options)!=sl::Result::eOk) return false;
        optionFrames=n;optionDynamic=dynamic;return true;
    }
    void marker(sl::PCLMarker m) {if (token && mark && reflexActive) mark(m,*token);}
    ~Impl() {if (initialized) shutdown();if (module) FreeLibrary(module);}
#endif
};
FrameGeneration::FrameGeneration():impl_(std::make_unique<Impl>()) {}
FrameGeneration::~FrameGeneration()=default;
FrameGenerationStats FrameGeneration::stats() const noexcept {return impl_->stats;}

#ifdef BOO_HAS_STREAMLINE
namespace {
template<typename T> bool symbol(HMODULE module,const char* name,T*& pointer) {
    pointer=reinterpret_cast<T*>(GetProcAddress(module,name));return pointer!=nullptr;
}
template<typename T> bool featureSymbol(PFun_slGetFeatureFunction* load,sl::Feature id,const char* name,T*& out) {
    void* address=nullptr;
    if(load(id,name,address)!=sl::Result::eOk || !address) return false;
    out=reinterpret_cast<T*>(address);return true;
}
sl::float4x4 matrix(DirectX::FXMMATRIX m) {
    DirectX::XMFLOAT4X4 f; DirectX::XMStoreFloat4x4(&f,m);
    sl::float4x4 r;
    for(int i=0;i<4;++i) r[i]={f.m[i][0],f.m[i][1],f.m[i][2],f.m[i][3]};
    return r;
}
}
void FrameGeneration::load() {
    auto& s=*impl_;
    std::array<wchar_t,32768> exe{};
    if (!GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()))) return;
    s.pluginFolder=std::filesystem::path(exe.data()).parent_path()/L"streamline";
    auto dll=s.pluginFolder/L"sl.interposer.dll";
    if (GetFileAttributesW(dll.c_str())==INVALID_FILE_ATTRIBUTES) return;
    if (!sl::security::verifyEmbeddedSignature(dll.c_str())) {
        BOO_LOG_WARN("Streamline","NVIDIA runtime signature verification failed");
        return;
    }
    s.module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.module || !symbol(s.module,"slInit",s.init) || !symbol(s.module,"slShutdown",s.shutdown) ||
        !symbol(s.module,"slSetD3DDevice",s.device) || !symbol(s.module,"slUpgradeInterface",s.upgrade) ||
        !symbol(s.module,"slIsFeatureSupported",s.supports) || !symbol(s.module,"slGetFeatureFunction",s.featureFn) ||
        !symbol(s.module,"slGetNewFrameToken",s.newToken) || !symbol(s.module,"slSetTagForFrame",s.setTag) ||
        !symbol(s.module,"slSetConstants",s.constants)) return;
    sl::Preferences prefs{};
    const sl::Feature features[]{sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
    // Streamline initializes NGX for presentation. Discover the SR/RR feature
    // binaries in the same NGX session so both render paths can run together.
    const auto reconstructionFolder=s.pluginFolder.parent_path()/L"dlss";
    const wchar_t* folders[]{s.pluginFolder.c_str(),reconstructionFolder.c_str()};
    prefs.pathsToPlugins=folders;prefs.numPathsToPlugins=2;prefs.featuresToLoad=features;prefs.numFeaturesToLoad=3;
    prefs.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eUseFrameBasedResourceTagging|sl::PreferenceFlags::eDisableDebugText;
    prefs.renderAPI=sl::RenderAPI::eD3D12;prefs.engine=sl::EngineType::eCustom;prefs.engineVersion="1.0";
    prefs.projectId="5b6c8338-1aa7-5abb-8586-78d064a60981";
    s.initialized=s.init(prefs,sl::kSDKVersion)==sl::Result::eOk;
    BOO_LOG_INFO("Streamline",s.initialized ? "Signed Frame Generation runtime initialized" : "Runtime initialization unavailable");
    startupCheckpoint();
}
bool FrameGeneration::attach(ID3D12Device* dev) {
    auto& s=*impl_;
    if (!s.initialized) return false;
    const auto deviceResult=s.device(dev);
    if (deviceResult!=sl::Result::eOk) {
        BOO_LOG_INFO("Streamline","Direct3D device registration returned "+std::to_string(static_cast<int>(deviceResult)));
        return false;
    }
    LUID id=dev->GetAdapterLuid();sl::AdapterInfo adapter{};
    adapter.deviceLUID=reinterpret_cast<uint8_t*>(&id);adapter.deviceLUIDSizeInBytes=sizeof(id);
    const auto reflexSupport=s.supports(sl::kFeatureReflex,adapter);
    const auto fgSupport=s.supports(sl::kFeatureDLSS_G,adapter);
    s.reflex=reflexSupport==sl::Result::eOk &&
        featureSymbol(s.featureFn,sl::kFeatureReflex,"slReflexSetOptions",s.reflexOptions) &&
        featureSymbol(s.featureFn,sl::kFeatureReflex,"slReflexGetState",s.reflexState) &&
        featureSymbol(s.featureFn,sl::kFeatureReflex,"slReflexSleep",s.sleep) &&
        featureSymbol(s.featureFn,sl::kFeaturePCL,"slPCLSetMarker",s.mark);
    s.fg=s.reflex && fgSupport==sl::Result::eOk &&
        featureSymbol(s.featureFn,sl::kFeatureDLSS_G,"slDLSSGSetOptions",s.fgOptions) &&
        featureSymbol(s.featureFn,sl::kFeatureDLSS_G,"slDLSSGGetState",s.fgState);
    if(s.fg) {
        sl::DLSSGState state{};
        if(s.fgState(s.viewport,state,nullptr)==sl::Result::eOk) {
            s.stats.maxGeneratedPerFrame=state.numFramesToGenerateMax;s.minDimension=state.minWidthOrHeight;
            s.dynamicSupported=state.bIsDynamicMFGSupported==sl::Boolean::eTrue;
        }
        s.stats.state=RendererFeatureState::Idle;
    }
    if(s.reflex) s.stats.reflexState=RendererFeatureState::Idle;
    BOO_LOG_INFO("Streamline","Frame Generation supported="+std::to_string(s.fg)+" (driver "+std::to_string(static_cast<int>(fgSupport))+"), Reflex supported="+std::to_string(s.reflex)+" (driver "+std::to_string(static_cast<int>(reflexSupport))+"), maximum generated="+std::to_string(s.stats.maxGeneratedPerFrame)+", minimum dimension="+std::to_string(s.minDimension));
    startupCheckpoint();
    return s.fg;
}
void FrameGeneration::shutdown() {
    auto& s=*impl_;
    if (s.initialized) {
        s.shutdown();
        s.initialized=false;
    }
}
bool FrameGeneration::upgrade(void** native) {return impl_->fg && impl_->upgrade(native)==sl::Result::eOk;}
void FrameGeneration::disablePresentation() {
    invalidate();
    impl_->fg=false;
    impl_->stats.maxGeneratedPerFrame=0;
    impl_->stats.state=RendererFeatureState::Unavailable;
}
void FrameGeneration::beginSimulation(FrameGenerationMode mode,ReflexMode reflexMode) {
    auto& s=*impl_; if (!s.reflex) return;
    endSimulation();
    if (s.renderOpen) {s.marker(sl::PCLMarker::eRenderSubmitEnd);s.renderOpen=false;}
    s.token=nullptr;s.sceneValid=false;s.tagged=false;
    s.requested=mode;
    if (s.lastReflex!=reflexMode || (reflexMode!=ReflexMode::Off && !s.reflexActive)) {
        sl::ReflexOptions opts{};
        opts.mode=reflexMode==ReflexMode::Off ? sl::ReflexMode::eOff : reflexMode==ReflexMode::Boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
        const auto result=s.reflexOptions(opts);sl::ReflexState state{};
        s.reflexActive=reflexMode!=ReflexMode::Off && result==sl::Result::eOk && s.reflexState(state)==sl::Result::eOk && state.lowLatencyAvailable;
        s.lastReflex=reflexMode;
    }
    s.stats.reflexState=reflexMode==ReflexMode::Off ? RendererFeatureState::Off : RendererFeatureState::Idle;
    if(mode==FrameGenerationMode::Off || !s.reflexActive) {
        s.setFrames(0);s.history=false;
        if(s.fg) s.stats.state=mode==FrameGenerationMode::Off ? RendererFeatureState::Off : RendererFeatureState::Idle;
    }
    ++s.frameIndex;
    if(!s.reflexActive || s.newToken(s.token,&s.frameIndex)!=sl::Result::eOk || !s.token) return;
    if(s.sleep(*s.token)==sl::Result::eOk) s.stats.reflexState=RendererFeatureState::Active;
    s.marker(sl::PCLMarker::eSimulationStart);s.simOpen=true;
}
void FrameGeneration::endSimulation() {auto& s=*impl_;if(s.simOpen) {s.marker(sl::PCLMarker::eSimulationEnd);s.simOpen=false;}}
void FrameGeneration::beginRender() {auto& s=*impl_;if(s.token) {s.marker(sl::PCLMarker::eRenderSubmitStart);s.renderOpen=true;}}
bool FrameGeneration::needsInputs() const noexcept {const auto& s=*impl_;return s.fg && s.reflexActive && s.token && s.requested!=FrameGenerationMode::Off;}
bool FrameGeneration::hasScene() const noexcept {return needsInputs() && impl_->sceneValid;}
void FrameGeneration::invalidate() {
    auto& s=*impl_;s.setFrames(0);s.history=false;s.tagged=false;s.sceneValid=false;s.stats.generatedPerFrame=0;
    if(s.fg) s.stats.state=s.requested==FrameGenerationMode::Off?RendererFeatureState::Off:RendererFeatureState::Idle;
}
void FrameGeneration::scene(const M1CameraState& cam,ID3D12Resource* depth,ID3D12Resource* motion,uint32_t w,uint32_t h,const float* jitter,bool reset) {
    auto& s=*impl_; if(!needsInputs() || !depth || !motion || !w || !h) return;
    using namespace DirectX;
    XMVECTOR eye=XMVectorSet(cam.eye[0],cam.eye[1],cam.eye[2],1), target=XMVectorSet(cam.target[0],cam.target[1],cam.target[2],1);
    XMVECTOR up=XMVectorSet(cam.up[0],cam.up[1],cam.up[2],0), forward=XMVector3Normalize(target-eye);
    XMVECTOR right=XMVector3Normalize(XMVector3Cross(up,forward));up=XMVector3Cross(forward,right);
    const float aspect=float(w)/h;
    auto projection=XMMatrixPerspectiveFovLH(cam.fovYRadians,aspect,0.1f,3200.f);
    auto view=XMMatrixLookAtLH(eye,target,up), vp=view*projection;
    auto previous=(s.history && !reset) ? XMLoadFloat4x4(&s.previousViewProjection) : vp;
    auto& c=s.camera;
    c.cameraViewToClip=matrix(projection);c.clipToCameraView=matrix(XMMatrixInverse(nullptr,projection));
    c.clipToPrevClip=matrix(XMMatrixInverse(nullptr,vp)*previous);c.prevClipToClip=matrix(XMMatrixInverse(nullptr,previous)*vp);
    c.clipToLensClip=matrix(XMMatrixIdentity());c.cameraPinholeOffset={0.f,0.f};
    c.jitterOffset={jitter?jitter[0]:0.f,jitter?jitter[1]:0.f};c.mvecScale={1.f/w,1.f/h};
    auto direction=[](FXMVECTOR v) {return sl::float3{XMVectorGetX(v),XMVectorGetY(v),XMVectorGetZ(v)};};
    c.cameraPos=direction(eye);c.cameraFwd=direction(forward);c.cameraUp=direction(up);c.cameraRight=direction(right);
    c.cameraNear=.1f;c.cameraFar=3200.f;c.cameraAspectRatio=aspect;c.cameraFOV=cam.fovYRadians;
    c.depthInverted=sl::Boolean::eFalse;c.cameraMotionIncluded=sl::Boolean::eTrue;c.motionVectors3D=sl::Boolean::eFalse;
    c.reset=reset || !s.history ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    XMStoreFloat4x4(&s.previousViewProjection,vp);s.inputW=w;s.inputH=h;s.depth=depth;s.motion=motion;s.sceneValid=true;
}
void FrameGeneration::tag(ID3D12GraphicsCommandList* cmd,ID3D12Resource* hudless,ID3D12Resource* alpha) {
    auto& s=*impl_;if(!hasScene() || !hudless || !alpha) return;
    const auto display=hudless->GetDesc();const auto w=static_cast<uint32_t>(display.Width),h=display.Height;
    if (std::min(w,h)<s.minDimension || !s.stats.maxGeneratedPerFrame) return;
    const auto read=static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto resource=[&](ID3D12Resource* r) {return sl::Resource{sl::ResourceType::eTex2d,r,nullptr,nullptr,read};};
    auto depth=resource(s.depth),mv=resource(s.motion),scene=resource(hudless),ui=resource(alpha);
    sl::Extent input{};input.width=s.inputW;input.height=s.inputH;
    sl::Extent region{};region.width=w;region.height=std::min(h,static_cast<uint32_t>(std::lround(w*s.inputH/double(s.inputW))));
    region.width=std::min(w,static_cast<uint32_t>(std::lround(h*s.inputW/double(s.inputH))));region.left=(w-region.width)/2;region.top=(h-region.height)/2;
    const auto life=sl::ResourceLifecycle::eValidUntilPresent;
    sl::ResourceTag tags[]{ {&depth,sl::kBufferTypeDepth,life,&input}, {&mv,sl::kBufferTypeMotionVectors,life,&input},
        {&scene,sl::kBufferTypeHUDLessColor,life,&region}, {&ui,sl::kBufferTypeUIAlpha,life,&region},
        {nullptr,sl::kBufferTypeBackbuffer,life,&region} };
    auto result=s.constants(s.camera,*s.token,s.viewport);
    if(result==sl::Result::eOk) result=s.setTag(*s.token,s.viewport,tags,5,cmd);
    if(result!=s.lastTagResult) {
        BOO_LOG_INFO("Streamline","Frame input status changed to "+std::to_string(static_cast<int>(result)));
        s.lastTagResult=result;
    }
    if(result!=sl::Result::eOk) return;
    const auto effective=availableFrameGenerationMode(s.requested,s.stats.maxGeneratedPerFrame);
    const uint32_t generated=effective==FrameGenerationMode::Auto ? 1u : static_cast<uint32_t>(effective)-1u;
    s.tagged=s.setFrames(generated,effective==FrameGenerationMode::Auto && s.dynamicSupported);
    s.history=s.tagged;
}
void FrameGeneration::beforePresent() {
    auto& s=*impl_;if(!s.tagged) invalidate();if(s.renderOpen) {s.marker(sl::PCLMarker::eRenderSubmitEnd);s.renderOpen=false;}
    s.marker(sl::PCLMarker::ePresentStart);
}
void FrameGeneration::afterPresent(bool presented) {
    auto& s=*impl_;s.marker(sl::PCLMarker::ePresentEnd);
    if(!presented) {
        invalidate();
        s.clearPresentationMeasurements();
        return;
    }
    sl::DLSSGState state{};uint32_t shown=1;
    const bool stateAvailable=s.fg && s.fgState(s.viewport,state,nullptr)==sl::Result::eOk;
    if(stateAvailable) {
        s.stats.maxGeneratedPerFrame=state.numFramesToGenerateMax;s.minDimension=state.minWidthOrHeight;
        s.dynamicSupported=state.bIsDynamicMFGSupported==sl::Boolean::eTrue;
        const auto status=static_cast<uint32_t>(state.status);
        if(status!=s.lastStatus) {
            BOO_LOG_INFO("Streamline","Frame Generation runtime status changed to "+std::to_string(status));
            s.lastStatus=status;
        }
        if(s.tagged && status) {
            // Configuration errors persist for the life of this device.
            // Size, display format, and Reflex availability can change next frame.
            constexpr uint32_t invalidIntegration =
                static_cast<uint32_t>(sl::DLSSGStatus::eFailCommonConstantsInvalid) |
                static_cast<uint32_t>(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled);
            if(status & invalidIntegration) disablePresentation();
            else invalidate();
            s.clearPresentationMeasurements();
            return;
        }
        if(s.tagged) {
            shown=state.numFramesActuallyPresented;
            s.stats.state=shown>1 ? RendererFeatureState::Active : RendererFeatureState::Idle;
        }
    } else if(s.tagged) {
        invalidate();
        s.clearPresentationMeasurements();
        return;
    }
    s.stats.framesActuallyPresented=shown;s.stats.generatedPerFrame=shown>1 ? shown-1 : 0;
    const auto now=std::chrono::steady_clock::now();
    if(s.sampled.time_since_epoch().count()==0) s.sampled=now;
    ++s.baseSamples;s.displaySamples+=shown;
    double elapsed=std::chrono::duration<double>(now-s.sampled).count();
    if(elapsed>=0.5) {s.stats.renderedFps=s.baseSamples/elapsed;s.stats.displayedFps=s.displaySamples/elapsed;s.baseSamples=s.displaySamples=0;s.sampled=now;}
}
#else
void FrameGeneration::load() {}
void FrameGeneration::shutdown() {}
bool FrameGeneration::attach(ID3D12Device*) {return false;}
bool FrameGeneration::upgrade(void**) {return false;}
void FrameGeneration::disablePresentation() {}
void FrameGeneration::beginSimulation(FrameGenerationMode,ReflexMode) {}
void FrameGeneration::endSimulation() {}
void FrameGeneration::beginRender() {}
void FrameGeneration::beforePresent() {}
void FrameGeneration::afterPresent(bool) {}
void FrameGeneration::invalidate() {}
bool FrameGeneration::needsInputs() const noexcept {return false;}
bool FrameGeneration::hasScene() const noexcept {return false;}
void FrameGeneration::scene(const M1CameraState&,ID3D12Resource*,ID3D12Resource*,uint32_t,uint32_t,const float*,bool) {}
void FrameGeneration::tag(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*) {}
#endif
}
