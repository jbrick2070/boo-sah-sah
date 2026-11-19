#include "boo/trilogy_renderer.hpp"
#include "boo/audio_reactive.hpp"
#include "boo/bmesh.hpp"
#include "boo/btex.hpp"
#include "boo/nrd_bridge.hpp"
#include "boo/logging.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
namespace boo {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr) {if(FAILED(hr))throw std::runtime_error("M2 D3D12 resource/pipeline failure: "+std::to_string(static_cast<uint32_t>(hr)));}
std::vector<uint8_t> shader(const char* name) {
    std::ifstream f(resolveResourcePath(std::filesystem::path("assets/shaders")/name),std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error(std::string("Missing M2 shader: ")+name);
    std::vector<uint8_t> data(static_cast<size_t>(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));return data;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* d,uint64_t size,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=std::max<uint64_t>(size,256);rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.Flags=flags;
    ComPtr<ID3D12Resource> r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
}
void upload(ID3D12Resource* resource,const void* data,size_t size,size_t offset=0) {void* p=nullptr;D3D12_RANGE rr{0,0};check(resource->Map(0,&rr,&p));std::memcpy(static_cast<uint8_t*>(p)+offset,data,size);D3D12_RANGE wr{offset,offset+size};resource->Unmap(0,&wr);}
void transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);
}
void uav(ID3D12GraphicsCommandList* cmd,ID3D12Resource* r) {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=r;cmd->ResourceBarrier(1,&b);}
struct DrawConstants {float rows[12]{};uint32_t first=0,mat=0,part=0,pad=0;};
struct SceneConstants {float eye[3];float fov;float right[3];float aspect;float up[3];float time;float forward[3];float reveal;uint32_t width,height,act,canonical;float day,pulse;uint32_t showFlags;float moodAccent;float moodBloom,moodLight,moodCut,pad;float sunDir[3];float sunScale;uint32_t accumFrame,interFlags;float jitter[2];};
    static_assert(sizeof(SceneConstants)==36*4,"SceneConstants must match m2_mesh.hlsl cbuffer Scene (36 dwords)");
    float halton(uint32_t i,uint32_t b){float f=1,r=0;while(i){f/=float(b);r+=f*float(i%b);i/=b;}return r;}
void normalize(float v[3]) {float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);for(int c=0;c<3;++c)v[c]/=std::max(l,1e-6f);}
void cross(const float a[3],const float b[3],float r[3]) {r[0]=a[1]*b[2]-a[2]*b[1];r[1]=a[2]*b[0]-a[0]*b[2];r[2]=a[0]*b[1]-a[1]*b[0];}
bool isCutoutMaterial(uint32_t m) noexcept { return m == 29u || m == 36u; }

uint16_t floatToHalfBits(float val) noexcept {
    if (!std::isfinite(val)) return 0u;
    val = std::clamp(val, -65504.0f, 65504.0f);
    uint32_t bits = 0;
    std::memcpy(&bits, &val, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    const int32_t exp32 = static_cast<int32_t>((bits >> 23u) & 0xFFu) - 127;
    uint32_t mant = bits & 0x007FFFFFu;
    if (exp32 < -24) return static_cast<uint16_t>(sign);
    if (exp32 < -14) {
        mant |= 0x00800000u;
        const int32_t shift = -14 - exp32;
        const uint32_t sub = (mant + (1u << (shift + 12))) >> (shift + 13);
        return static_cast<uint16_t>(sign | (sub & 0x03FFu));
    }
    if (exp32 > 15) return static_cast<uint16_t>(sign | 0x7BFFu);
    const uint32_t exp16 = static_cast<uint32_t>(exp32 + 15) << 10u;
    const uint32_t mant16 = (mant + 0x00001000u) >> 13u;
    return static_cast<uint16_t>(sign | (exp16 + mant16));
}

void packDrawMotionVectors(DrawConstants& dc, float mvX, float mvY, float mvZ) noexcept {
    const uint16_t hx = floatToHalfBits(mvX);
    const uint16_t hy = floatToHalfBits(mvY);
    const uint16_t hz = floatToHalfBits(mvZ);
    dc.part = (dc.part & 0xFFFFu) | (static_cast<uint32_t>(hx) << 16u);
    dc.pad  = static_cast<uint32_t>(hy) | (static_cast<uint32_t>(hz) << 16u);
}

void buildColumnMajorCameraMatrices(
    const float eye[3],
    const float right[3],
    const float up[3],
    const float forward[3],
    float fovY,
    float aspect,
    float outWorldToView[16],
    float outViewToClip[16]) noexcept {
    // Column-major LH WorldToView matching VSMain & NRD MathLib (m_ViewToWorld[3].xyz == eye)
    const float tx = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    const float ty = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    const float tz = -(forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2]);
    outWorldToView[0]  = right[0]; outWorldToView[1]  = up[0]; outWorldToView[2]  = forward[0]; outWorldToView[3]  = 0.0f;
    outWorldToView[4]  = right[1]; outWorldToView[5]  = up[1]; outWorldToView[6]  = forward[1]; outWorldToView[7]  = 0.0f;
    outWorldToView[8]  = right[2]; outWorldToView[9]  = up[2]; outWorldToView[10] = forward[2]; outWorldToView[11] = 0.0f;
    outWorldToView[12] = tx;       outWorldToView[13] = ty;    outWorldToView[14] = tz;         outWorldToView[15] = 1.0f;

    // Column-major un-jittered LH D3D projection [0.1 .. 1000.0] matching VSMain o.pos
    const float fy = 1.0f / std::tan(std::max(fovY * 0.5f, 0.05f));
    constexpr float zNear = 0.1f;
    constexpr float zFar = 1000.0f;
    std::memset(outViewToClip, 0, sizeof(float) * 16);
    outViewToClip[0]  = fy / std::max(aspect, 1e-4f);
    outViewToClip[5]  = fy;
    outViewToClip[10] = zFar / (zFar - zNear);
    outViewToClip[11] = 1.0f;
    outViewToClip[14] = -zNear * zFar / (zFar - zNear);
}

static constexpr std::array<uint32_t, 6> kMarbleTemplateMats = {3u, 4u, 35u, 8u, 23u, 31u};

BmeshSubmesh appendSmoothSphereTemplate(
    std::vector<BmeshVertex>& vertices,
    std::vector<uint32_t>& indices,
    uint32_t matId,
    uint32_t partId) {
    constexpr uint32_t kSlices = 48u;
    constexpr uint32_t kStacks = 32u;
    constexpr float kPi = 3.14159265358979323846f;
    BmeshSubmesh sub{};
    sub.firstVertex = static_cast<uint32_t>(vertices.size());
    sub.firstIndex  = static_cast<uint32_t>(indices.size());
    sub.material    = matId;
    sub.part        = partId;

    for (uint32_t st = 0; st <= kStacks; ++st) {
        const float v = static_cast<float>(st) / static_cast<float>(kStacks);
        const float theta = v * kPi;
        const float stSin = std::sin(theta);
        const float stCos = std::cos(theta);
        for (uint32_t sl = 0; sl <= kSlices; ++sl) {
            const float u = static_cast<float>(sl) / static_cast<float>(kSlices);
            const float phi = u * (2.0f * kPi);
            const float sp = std::sin(phi);
            const float cp = std::cos(phi);
            BmeshVertex vert{};
            vert.positionMaterial[0] = stSin * cp;
            vert.positionMaterial[1] = stCos;
            vert.positionMaterial[2] = stSin * sp;
            vert.positionMaterial[3] = static_cast<float>(matId);
            vert.normalPart[0] = vert.positionMaterial[0];
            vert.normalPart[1] = vert.positionMaterial[1];
            vert.normalPart[2] = vert.positionMaterial[2];
            vert.normalPart[3] = static_cast<float>(partId);
            vert.uvThicknessRoughness[0] = u * 2.0f;
            vert.uvThicknessRoughness[1] = v * 2.0f;
            vert.uvThicknessRoughness[2] = 0.044f;
            vert.uvThicknessRoughness[3] = (matId == 3u || matId == 4u || matId == 35u) ? 0.025f : 0.16f;
            vert.tangentSign[0] = -sp;
            vert.tangentSign[1] = 0.0f;
            vert.tangentSign[2] = cp;
            vert.tangentSign[3] = 1.0f;
            vertices.push_back(vert);
        }
    }
    sub.vertexCount = static_cast<uint32_t>(vertices.size()) - sub.firstVertex;

    for (uint32_t st = 0; st < kStacks; ++st) {
        for (uint32_t sl = 0; sl < kSlices; ++sl) {
            const uint32_t i0 = sub.firstVertex + st * (kSlices + 1u) + sl;
            const uint32_t i1 = i0 + 1u;
            const uint32_t i2 = i0 + (kSlices + 1u);
            const uint32_t i3 = i2 + 1u;
            indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
            indices.push_back(i1); indices.push_back(i3); indices.push_back(i2);
        }
    }
    sub.indexCount = static_cast<uint32_t>(indices.size()) - sub.firstIndex;
    return sub;
}

ComPtr<ID3D12Resource> uploadBtexArray(ID3D12Device* d, D3D12Context& ctx, const Btex& tex, std::string* err) {
    D3D12_HEAP_PROPERTIES defHp{}; defHp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = tex.width;
    rd.Height = tex.height;
    rd.DepthOrArraySize = static_cast<UINT16>(tex.arraySize);
    rd.MipLevels = static_cast<UINT16>(tex.mipCount);
    rd.Format = static_cast<DXGI_FORMAT>(tex.dxgiFormat);
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> gpuTex;
    check(d->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpuTex)));

    const UINT numSubs = static_cast<UINT>(tex.subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(numSubs);
    std::vector<UINT> numRows(numSubs);
    std::vector<UINT64> rowSizes(numSubs);
    UINT64 totalUploadBytes = 0;
    d->GetCopyableFootprints(&rd, 0, numSubs, 0, footprints.data(), numRows.data(), rowSizes.data(), &totalUploadBytes);

    ComPtr<ID3D12Resource> upBuf = buffer(d, totalUploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    D3D12_RANGE rr{0, 0};
    check(upBuf->Map(0, &rr, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < numSubs; ++i) {
        const auto& sub = tex.subresources[i];
        const auto& fp = footprints[i];
        const uint8_t* srcBase = tex.payload.data() + sub.byteOffset;
        uint8_t* dstBase = mapped + fp.Offset;
        const size_t copyBytes = std::min<size_t>(sub.rowBytes, static_cast<size_t>(fp.Footprint.RowPitch));
        const UINT rows = std::min<UINT>(sub.numRows, numRows[i]);
        for (UINT r = 0; r < rows; ++r) {
            std::memcpy(dstBase + size_t(r) * fp.Footprint.RowPitch, srcBase + size_t(r) * sub.rowBytes, copyBytes);
        }
    }
    D3D12_RANGE wr{0, static_cast<SIZE_T>(totalUploadBytes)};
    upBuf->Unmap(0, &wr);

    if (!ctx.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        for (UINT i = 0; i < numSubs; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = gpuTex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upBuf.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[i];
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        transition(cmd, gpuTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }, err)) {
        throw std::runtime_error(err && !err->empty() ? *err : "BTEX GPU upload failed");
    }
    return gpuTex;
}
}
struct TrilogyMeshRenderer::Impl {
    D3D12Context& context;
    ShowVisualMode visualMode=ShowVisualMode::M2Trilogy180s;
    struct Part {BmeshSubmesh sub;uint32_t kit;ComPtr<ID3D12Resource> blas;};
    std::vector<Part> parts;
    std::array<Part,6> marbleTemplates{};
    size_t maxTlasInstances=0;
    ComPtr<ID3D12Resource> vb,ib,scratch,tlas,tlasScratch,instances,depth;
    ComPtr<ID3D12Resource> albedoTex,normalTex,ormtTex,envIblTex,fallbackTensorTex;
    ComPtr<ID3D12Resource> gbufNormRough,gbufDiffRadHit,gbufSpecRadHit,gbufModViewZ,nrdMv;
    ComPtr<ID3D12Resource> nrdNormRough,nrdViewZ,nrdDiffIn,nrdSpecIn;
    ComPtr<ID3D12DescriptorHeap> rtv,dsv,srvHeap;
    ComPtr<ID3D12RootSignature> root,denoiseRoot,nrdPackRoot;
    ComPtr<ID3D12PipelineState> depthOpaque,depthCutout,raster,rasterCutout,rt,rtCutout,sky,skyRt;
    ComPtr<ID3D12PipelineState> denoiseResolve,nrdPackCs,nrdUnpackPs;
    std::array<ComPtr<ID3D12Resource>,4> counters,readbacks;
    ComPtr<ID3D12Resource> zero;
    std::array<bool,4> pending{};
    // Explore-mode progressive accumulation (RGBA32F history, canonical pass only)
    ComPtr<ID3D12Resource> accum;ComPtr<ID3D12RootSignature> accumRoot;ComPtr<ID3D12PipelineState> accumBlend,accumResolve;
    bool accumValid=false;
    NrdBridge nrd;
    uint32_t reblurFrameCounter=0,denoiseMode=0;
    bool reblurPrevValid=false,prevSunOverride=false,prevLampsOff=false;
    float prevWorldToView[16]{},prevViewToClip[16]{},prevEye[3]{},prevForward[3]{},prevSunDir[3]{};
    uint32_t w=0,h=0,hits=0,queries=0,fallbacks=0;
    bool usedFallbackTexture = false;
    bool verified=false,rtReady=false;
    explicit Impl(D3D12Context& c):context(c),nrd(c){}
    bool isPartActiveInShow(const Part& p,float time) const noexcept {
        const uint32_t pt=p.sub.part;
        const bool hero=p.kit>=4;
        if(visualMode==ShowVisualMode::SoloRT) {
            return p.kit==2||p.kit==3||(hero&&pt>=200&&pt<=215);
        }
        if(visualMode==ShowVisualMode::SoloTensor) {
            return p.kit==2||p.kit==3||(hero&&pt>=210&&pt<=219);
        }
        if(visualMode==ShowVisualMode::SoloNPU) {
            return p.kit==2||p.kit==3||(hero&&((pt>=210&&pt<=215)||(pt>=220&&pt<=227)));
        }
        if(visualMode==ShowVisualMode::SoloCUDA) {
            return p.kit==2||p.kit==3||(hero&&pt>=210&&pt<=215);
        }
        // FullFilm360s 7-act activation
        if(time<45.f) {
            return p.kit==2||p.kit==3||(hero&&pt>=210&&pt<=215&&time>=30.5f);
        }
        if(time<100.f) {
            return p.kit==2||p.kit==3||(hero&&pt>=200&&pt<=215);
        }
        if(time<150.f) {
            return p.kit==2||p.kit==3||(hero&&pt>=210&&pt<=219);
        }
        if(time<195.f) {
            return p.kit==2||p.kit==3||(hero&&((pt>=210&&pt<=215)||(pt>=220&&pt<=227)));
        }
        if(time<245.f) {
            return p.kit==2||p.kit==3||(hero&&pt>=210&&pt<=215);
        }
        if(time<275.f) {
            return p.kit==1; // Act 5 Signal Hijack over Shenzhen Bay eVTOL
        }
        if(time<338.5f) {
            return p.kit==2||p.kit==3||(hero&&((pt>=200&&pt<=207)||(pt>=210&&pt<=227)||(time>=335.5f&&pt>=230&&pt<=249)));
        }
        // Dawn Bathroom / Alcove finale: show distant Taipei 101, Xinyi skyline & Taipei Basin ground/ridges (excluding street trees & rooftop altar)
        if(p.kit==2) return pt==0u||pt==3u||pt==4u||pt==6u;
        return p.kit==3||(hero&&((pt>=210&&pt<=215&&time<341.8f)||(pt>=230&&pt<=249)));
    }
    DrawConstants transform(const Part& p,const FrameContext& f,const M1CameraState& cam) {
        DrawConstants dc{};float scale=1,x=0,y=0,z=0,yaw=0,pitch=0,roll=0;
        auto smoother=[](float v){v=std::clamp(v,0.f,1.f);return v*v*v*(v*(v*6.f-15.f)+10.f);};
        const float time=static_cast<float>(double(f.samplePosition)/f.sampleRate);
        float sHover=0.f;
        if(p.kit==1 && p.sub.part>=100) {
            const float u=(visualMode==ShowVisualMode::FullFilm360s)
                ? std::clamp((time-245.f)*2.f,0.f,60.f)
                : std::clamp(time-60.f,0.f,60.f);
            sHover=smoother((u-21.f)/19.f);
            const float sTouch=smoother((u-43.f)/15.f);
            // Cruise sweep across Shenzhen Bay channel -> decelerating transition -> precision VTOL hover over Hero Pad (0, 3.64, -2)
            const float cx=3.4f-0.28f*u, cy=9.6f-0.05f*u+0.16f*std::sin(u*0.24f), cz=18.2f-0.78f*u;
            const float hx=0.55f, hy=5.45f-1.15f*sTouch+0.05f*std::sin(u*1.5f), hz=-2.0f;
            x=cx+(hx-cx)*sHover; y=cy+(hy-cy)*sHover; z=cz+(hz-cz)*sHover;
            yaw=0.88f*(1.f-sHover)-0.92f*sHover;
            pitch=-0.05f*(1.f-sHover)+0.02f*sHover;
            roll=-0.24f*(1.f-sHover);
        }
        if(p.kit==2) {
            scale=.080f;x=18.5f;z=60.0f;
            if(visualMode==ShowVisualMode::FullFilm360s&&time>=335.5f) {
                // Elevate Dawn Bathroom to 18th-floor Taipei penthouse: frame bamboo modules, 88F golden TMD & 508m spire cleanly in picture window
                const float care=smoother((time-335.5f)/6.0f);
                scale=.080f-.016f*care;
                x=18.5f+14.5f*care;
                y=-9.6f*care;
                z=60.0f+6.0f*care;
            }
        }
        if(p.kit==3) {
            x=0;y=5.85f;z=5.5f;scale=1.05f;yaw=-.10f;
            if(visualMode==ShowVisualMode::FullFilm360s) {
                // Settle delicate 1:1 miniature keepsake Glass Orchid stem directly through the throat of the celadon bud vase (0.395, 3.550, 6.250)
                const float care=smoother((time-335.5f)/6.0f);
                scale*=1.f-.962f*care;
                x=0.402f*care;
                y-=2.113f*care;
                z+=0.735f*care;
                yaw=-.10f-.08f*care;
            }
            if(p.sub.part>=10&&p.sub.part<=16)scale*=.35f+.65f*cam.revealOpen01;
        }
        if(p.kit>=4) {
            const uint32_t pt=p.sub.part;
            if(pt>=200&&pt<=207) {
                // Grounded architectural feet: optical variation belongs in the material, not whole-vault sway.
                yaw=0.f;
                if(visualMode==ShowVisualMode::FullFilm360s&&time>=335.5f) {
                    y=-6.2f*smoother((time-335.5f)/3.0f);
                }
            } else if(pt>=210&&pt<=215) {
                if(visualMode==ShowVisualMode::FullFilm360s&&time<45.f) {
                    y=-4.5f*(1.f-smoother((time-30.5f)/7.5f));
                }
                if(visualMode==ShowVisualMode::FullFilm360s&&time>=335.5f)
                    y=-9.f*smoother((time-335.5f)/6.0f);
            } else if(pt>=216&&pt<=219) {
                y=0.14f*std::sin(time*0.85f+float(pt-216)*1.15f);
                if(visualMode==ShowVisualMode::FullFilm360s&&time>=335.5f) {
                    y+=5.5f*smoother((time-335.5f)/3.0f);
                }
            } else if(pt>=220&&pt<=223) {
                const float iris=std::clamp(f.moodIrisAngle,0.f,1.f);
                const float dir=(pt&1u)?-1.f:1.f;
                x=0.f; y=5.75f; z=5.50f;
                scale=0.96f+0.08f*iris;
                roll=dir*(0.04f*std::sin(time*.32f)+(iris-.5f)*.12f);
                yaw=.018f*std::sin(time*.4f+float(pt-220));
            } else if(pt>=224&&pt<=227) {
                const float localT=(visualMode==ShowVisualMode::SoloNPU)?std::fmod(time,45.f):(time-150.f);
                const float bloom=smoother((localT-20.f)/6.5f);
                // Rooted terraces stay on the floor; only foliage warmth is modulated by the director.
                (void)bloom;
            } else if(pt>=230&&pt<=249) {
                const float rise=smoother((time-335.5f)/6.0f);
                y=-4.8f*(1.f-rise);
            }
        }
        // Airframe transform: R_y(yaw) * R_x(pitch) * R_z(roll) * scale + translation
        const float cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch),cr=std::cos(roll),sr=std::sin(roll);
        const float rows[12]={
            (cy*cr+sy*sp*sr)*scale, (-cy*sr+sy*sp*cr)*scale, (sy*cp)*scale, x,
            (cp*sr)*scale,          (cp*cr)*scale,           (-sp)*scale,   y,
            (-sy*cr+cy*sp*sr)*scale,(sy*sr+cy*sp*cr)*scale,  (cy*cp)*scale, z
        };
        std::copy_n(rows,12,dc.rows);
        if(p.kit==1 && p.sub.part>=110 && p.sub.part<126) {
            static constexpr float kPivots[6][3]={
                {-3.05f,0.36f,-1.65f},{3.05f,0.36f,-1.65f},
                {-4.10f,0.40f, 0.15f},{4.10f,0.40f, 0.15f},
                {-2.60f,0.46f, 1.95f},{2.60f,0.46f, 1.95f}
            };
            const uint32_t rotor=std::min<uint32_t>(5u,p.sub.part>=120?p.sub.part-120:p.sub.part-110);
            const float px=kPivots[rotor][0],py=kPivots[rotor][1],pz=kPivots[rotor][2];
            const float dir=(rotor&1u)?-1.f:1.f;
            const float spin=p.sub.part>=120?(dir*time*38.f+float(rotor)*1.05f):0.f;
            // Forward cruise tilt (49 deg) articulating smoothly upright (2.3 deg) for vertical hover
            const float tilt=0.86f*(1.f-sHover)+0.04f*sHover;
            const float cs=std::cos(spin),ss=std::sin(spin),ct=std::cos(tilt),st=std::sin(tilt);
            // Exact R_x(tilt) * R_y(spin) around pivot (px, py, pz): blades spin strictly within tilted rotor plane
            const float local[12]={
                cs,     0.f, -ss,   px-cs*px+ss*pz,
                ss*st,  ct,  cs*st, py-ss*st*px-ct*py-cs*st*pz,
                ss*ct, -st,  cs*ct, pz-ss*ct*px+st*py-cs*ct*pz
            };
            for(int row=0;row<3;++row) {for(int col=0;col<3;++col)dc.rows[row*4+col]=rows[row*4]*local[col]+rows[row*4+1]*local[4+col]+rows[row*4+2]*local[8+col];dc.rows[row*4+3]=rows[row*4]*local[3]+rows[row*4+1]*local[7]+rows[row*4+2]*local[11]+rows[row*4+3];}
        }
        dc.first=p.sub.firstIndex;dc.mat=p.sub.material;dc.part=p.sub.part;return dc;
    }
};
TrilogyMeshRenderer::TrilogyMeshRenderer(D3D12Context& c):impl_(std::make_unique<Impl>(c)){}
TrilogyMeshRenderer::~TrilogyMeshRenderer() {impl_->context.waitForGpu();}
bool TrilogyMeshRenderer::initialize(bool fault,std::string* error,ShowVisualMode visualMode,void* tensorTexture) {
    try {
        auto& s=*impl_;s.visualMode=visualMode;auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());
        std::vector<BmeshVertex> vertices;std::vector<uint32_t> indices;
        const char* names[]={"shanghai_trestle","shenzhen_evtol","taipei_101_hero","glass_orchid_hero"};
        for(uint32_t k=0;k<4;++k) {
            Bmesh mesh;std::string why;
            if(fault||!loadBmesh(resolveResourcePath(std::string("assets/meshes/")+names[k]+".bmesh"),mesh,&why)) {
                mesh=buildTrilogyFallbackKit(k);++s.fallbacks;BOO_LOG_WARN("M2Mesh",std::string(names[k])+": procedural fallback "+why);
            }
            for(auto sub:mesh.submeshes) {sub.firstIndex+=static_cast<uint32_t>(indices.size());sub.firstVertex+=static_cast<uint32_t>(vertices.size());s.parts.push_back({sub,k,{}});}
            for(auto i:mesh.indices)indices.push_back(i+static_cast<uint32_t>(vertices.size()));
            vertices.insert(vertices.end(),mesh.vertices.begin(),mesh.vertices.end());
        }
        if(visualMode!=ShowVisualMode::M2Trilogy180s) {
            const char* heroes[]={"enamel_cathedral","microbial_sanctuary","dawn_alcove"};
            for(uint32_t k=0;k<3;++k) {
                Bmesh micro;std::string why;
                if(fault||!loadBmesh(resolveResourcePath(std::string("assets/meshes/")+heroes[k]+".bmesh"),micro,&why)) {
                    micro=buildEnamelMicrocosmKit();++s.fallbacks;
                    const uint32_t lo=k==0?200u:(k==1?220u:230u),hi=k==0?219u:(k==1?227u:235u);
                    std::erase_if(micro.submeshes,[&](const auto& sub){return sub.part<lo||sub.part>hi;});
                    BOO_LOG_WARN("FilmMesh",std::string(heroes[k])+": deterministic fallback "+why);
                }
                for(auto sub:micro.submeshes) {sub.firstIndex+=static_cast<uint32_t>(indices.size());sub.firstVertex+=static_cast<uint32_t>(vertices.size());s.parts.push_back({sub,k+4u,{}});}
                for(auto i:micro.indices)indices.push_back(i+static_cast<uint32_t>(vertices.size()));
                vertices.insert(vertices.end(),micro.vertices.begin(),micro.vertices.end());
            }
        }
        for (size_t m = 0; m < kMarbleTemplateMats.size(); ++m) {
            BmeshSubmesh sph = appendSmoothSphereTemplate(vertices, indices, kMarbleTemplateMats[m], 900u + static_cast<uint32_t>(m));
            s.marbleTemplates[m] = Impl::Part{sph, 99u, {}};
        }
        const size_t vbBytes = vertices.size() * sizeof(BmeshVertex);
        const size_t ibBytes = indices.size() * sizeof(uint32_t);
        auto vbUpload = buffer(d, vbBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto ibUpload = buffer(d, ibBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(vbUpload.Get(), vertices.data(), vbBytes);
        upload(ibUpload.Get(), indices.data(), ibBytes);
        s.vb = buffer(d, vbBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        s.ib = buffer(d, ibBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!s.context.executeSingleCommandListAndWait([&](void* raw) {
            auto* copyCmd = static_cast<ID3D12GraphicsCommandList*>(raw);
            copyCmd->CopyBufferRegion(s.vb.Get(), 0, vbUpload.Get(), 0, vbBytes);
            copyCmd->CopyBufferRegion(s.ib.Get(), 0, ibUpload.Get(), 0, ibBytes);
            const auto srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transition(copyCmd, s.vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srvState);
            transition(copyCmd, s.ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srvState);
        }, error)) {
            return false;
        }

        // Load & upload authored/scanned PBR BTEX001 texture arrays (with deterministic fallback when fault==true or missing)
        Btex bAlb, bNor, bOrm, bEnv;
        std::string texWhy;
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_albedo_alpha.btex"), bAlb, &texWhy)) {
            s.usedFallbackTexture = true;
            bAlb = buildFallbackAlbedoArray(42u);
            BOO_LOG_WARN("M2Tex", "pbr_albedo_alpha.btex fallback: " + texWhy);
        }
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_normal.btex"), bNor, &texWhy)) {
            s.usedFallbackTexture = true;
            bNor = buildFallbackNormalArray(42u);
            BOO_LOG_WARN("M2Tex", "pbr_normal.btex fallback: " + texWhy);
        }
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_ormt.btex"), bOrm, &texWhy)) {
            s.usedFallbackTexture = true;
            bOrm = buildFallbackOrmtArray(42u);
            BOO_LOG_WARN("M2Tex", "pbr_ormt.btex fallback: " + texWhy);
        }
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_ibl_env.btex"), bEnv, &texWhy)) {
            s.usedFallbackTexture = true;
            bEnv = buildFallbackEnvIblArray(3u);
            BOO_LOG_WARN("M2Tex", "pbr_ibl_env.btex fallback: " + texWhy);
        }
        s.albedoTex = uploadBtexArray(d, s.context, bAlb, error);
        s.normalTex = uploadBtexArray(d, s.context, bNor, error);
        s.ormtTex   = uploadBtexArray(d, s.context, bOrm, error);
        s.envIblTex = uploadBtexArray(d, s.context, bEnv, error);

        auto* tensorRes = static_cast<ID3D12Resource*>(tensorTexture);
        if (!tensorRes) {
            Btex fbT = buildFallbackEnvIblArray(1u);
            s.fallbackTensorTex = uploadBtexArray(d, s.context, fbT, error);
            tensorRes = s.fallbackTensorTex.Get();
        }

        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        shd.NumDescriptors = 96; // 5 static + 4 slots * 20 per-pass denoise/pack/unpack/accum descriptors
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(d->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&s.srvHeap)));
        const UINT srvInc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto srvAt = [&](UINT idx) {
            auto h = s.srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(idx) * srvInc;
            return h;
        };

        // [0] = t3 (tensorField Texture2D<float4>)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvT3{};
        srvT3.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvT3.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvT3.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvT3.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(tensorRes, &srvT3, srvAt(0u));

        auto bindArraySrv = [&](UINT idx, ID3D12Resource* res, const Btex& bt) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = static_cast<DXGI_FORMAT>(bt.dxgiFormat);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MostDetailedMip = 0;
            sd.Texture2DArray.MipLevels = bt.mipCount;
            sd.Texture2DArray.FirstArraySlice = 0;
            sd.Texture2DArray.ArraySize = bt.arraySize;
            d->CreateShaderResourceView(res, &sd, srvAt(idx));
        };
        bindArraySrv(1u, s.albedoTex.Get(), bAlb); // t4
        bindArraySrv(2u, s.normalTex.Get(), bNor); // t5
        bindArraySrv(3u, s.ormtTex.Get(),   bOrm); // t6
        bindArraySrv(4u, s.envIblTex.Get(), bEnv); // t7

        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=5;range.BaseShaderRegister=3;
        D3D12_ROOT_PARAMETER params[7]{};
        for(UINT i=0;i<2;++i) {params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[i].Constants.ShaderRegister=i;params[i].Constants.Num32BitValues=i?16:36;}
        for(UINT i=0;i<3;++i) {params[2+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2+i].Descriptor.ShaderRegister=i;}
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[5].DescriptorTable={1,&range};
        params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[6].Descriptor.ShaderRegister=0;

        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].ShaderRegister = 0;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[1].MipLODBias = -0.25f;
        samplers[1].MaxAnisotropy = 16;
        samplers[1].ShaderRegister = 1;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[2].ShaderRegister = 2;
        samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=7;rs.pParameters=params;rs.NumStaticSamplers=3;rs.pStaticSamplers=samplers;
        ComPtr<ID3DBlob> blob,err;check(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err));check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
        const auto vs=shader("m2_mesh_vs.cso"),psDepthCut=shader("m2_depth_cutout_ps.cso"),ps=shader("m2_mesh_ps.cso"),psCut=shader("m2_mesh_cutout_ps.cso"),skyvs=shader("m2_sky_vs.cso"),skyps=shader("m2_sky_ps.cso");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC dp{};dp.pRootSignature=s.root.Get();dp.VS={vs.data(),vs.size()};dp.PS={nullptr,0};dp.SampleMask=UINT_MAX;
        dp.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;dp.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;dp.RasterizerState.DepthClipEnable=TRUE;
        dp.DepthStencilState.DepthEnable=TRUE;dp.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;dp.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        dp.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;dp.NumRenderTargets=0;dp.DSVFormat=DXGI_FORMAT_D32_FLOAT;dp.SampleDesc.Count=1;
        check(d->CreateGraphicsPipelineState(&dp,IID_PPV_ARGS(&s.depthOpaque)));
        dp.PS={psDepthCut.data(),psDepthCut.size()};check(d->CreateGraphicsPipelineState(&dp,IID_PPV_ARGS(&s.depthCutout)));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.VS={vs.data(),vs.size()};pd.PS={ps.data(),ps.size()};
        pd.BlendState.IndependentBlendEnable=FALSE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=2;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
        check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.raster)));
        pd.PS={psCut.data(),psCut.size()};check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.rasterCutout)));
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};ComPtr<ID3D12Device5> d5;
        s.rtReady=!s.context.isWarp()&&SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))&&options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1&&SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5)));
        if(s.rtReady) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pdRt=pd;
            pdRt.NumRenderTargets=7;
            pdRt.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            pdRt.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;
            pdRt.RTVFormats[2]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            pdRt.RTVFormats[3]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            pdRt.RTVFormats[4]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            pdRt.RTVFormats[5]=DXGI_FORMAT_R32G32B32A32_UINT;
            pdRt.RTVFormats[6]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            auto rps=shader("m2_mesh_rt_ps.cso");pdRt.PS={rps.data(),rps.size()};check(d->CreateGraphicsPipelineState(&pdRt,IID_PPV_ARGS(&s.rt)));
            auto rpsCut=shader("m2_mesh_rt_cutout_ps.cso");pdRt.PS={rpsCut.data(),rpsCut.size()};check(d->CreateGraphicsPipelineState(&pdRt,IID_PPV_ARGS(&s.rtCutout)));
            auto skyRtPs=shader("m2_sky_rt_ps.cso");pdRt.VS={skyvs.data(),skyvs.size()};pdRt.PS={skyRtPs.data(),skyRtPs.size()};
            pdRt.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pdRt.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
            check(d->CreateGraphicsPipelineState(&pdRt,IID_PPV_ARGS(&s.skyRt)));
        }
        pd.VS={skyvs.data(),skyvs.size()};pd.PS={skyps.data(),skyps.size()};pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.sky)));
        const auto avs=shader("m2_accum_vs.cso");
        {
            // Accumulation root signature: one SRV table (t0, space1), pixel-only
            D3D12_DESCRIPTOR_RANGE ar{};ar.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ar.NumDescriptors=1;ar.BaseShaderRegister=0;ar.RegisterSpace=1;
            D3D12_ROOT_PARAMETER ap{};ap.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;ap.DescriptorTable={1,&ar};ap.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC ars{};ars.NumParameters=1;ars.pParameters=&ap;
            ComPtr<ID3DBlob> ab,ae;check(D3D12SerializeRootSignature(&ars,D3D_ROOT_SIGNATURE_VERSION_1,&ab,&ae));check(d->CreateRootSignature(0,ab->GetBufferPointer(),ab->GetBufferSize(),IID_PPV_ARGS(&s.accumRoot)));
            const auto abps=shader("m2_accum_blend_ps.cso"),arps=shader("m2_accum_resolve_ps.cso");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC ad{};ad.pRootSignature=s.accumRoot.Get();ad.VS={avs.data(),avs.size()};ad.PS={abps.data(),abps.size()};ad.SampleMask=UINT_MAX;
            ad.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;ad.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;ad.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;ad.NumRenderTargets=1;ad.SampleDesc.Count=1;
            auto& bt=ad.BlendState.RenderTarget[0];bt.BlendEnable=TRUE;bt.SrcBlend=D3D12_BLEND_BLEND_FACTOR;bt.DestBlend=D3D12_BLEND_INV_BLEND_FACTOR;bt.BlendOp=D3D12_BLEND_OP_ADD;bt.SrcBlendAlpha=D3D12_BLEND_BLEND_FACTOR;bt.DestBlendAlpha=D3D12_BLEND_INV_BLEND_FACTOR;bt.BlendOpAlpha=D3D12_BLEND_OP_ADD;bt.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            ad.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;check(d->CreateGraphicsPipelineState(&ad,IID_PPV_ARGS(&s.accumBlend)));
            bt.BlendEnable=FALSE;ad.PS={arps.data(),arps.size()};ad.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;check(d->CreateGraphicsPipelineState(&ad,IID_PPV_ARGS(&s.accumResolve)));
        }
        if (s.rtReady) {
            // Denoise resolve + NRD unpack graphics root signature: b0(space1) 4 dwords + t0..t3(space1)
            D3D12_DESCRIPTOR_RANGE dr{};dr.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;dr.NumDescriptors=4;dr.BaseShaderRegister=0;dr.RegisterSpace=1;
            D3D12_ROOT_PARAMETER dp2[2]{};
            dp2[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;dp2[0].Constants={0,1,4};dp2[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            dp2[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;dp2[1].DescriptorTable={1,&dr};dp2[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC drs{};drs.NumParameters=2;drs.pParameters=dp2;
            ComPtr<ID3DBlob> db,de;check(D3D12SerializeRootSignature(&drs,D3D_ROOT_SIGNATURE_VERSION_1,&db,&de));
            check(d->CreateRootSignature(0,db->GetBufferPointer(),db->GetBufferSize(),IID_PPV_ARGS(&s.denoiseRoot)));

            const auto denPs=shader("m2_denoise_resolve_ps.cso"),unpPs=shader("m2_nrd_unpack_ps.cso");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC dd{};dd.pRootSignature=s.denoiseRoot.Get();dd.VS={avs.data(),avs.size()};dd.PS={denPs.data(),denPs.size()};dd.SampleMask=UINT_MAX;
            dd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;dd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;dd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;dd.NumRenderTargets=1;dd.SampleDesc.Count=1;
            auto& dbt=dd.BlendState.RenderTarget[0];
            dbt.BlendEnable=TRUE;dbt.SrcBlend=D3D12_BLEND_ONE;dbt.DestBlend=D3D12_BLEND_ONE;dbt.BlendOp=D3D12_BLEND_OP_ADD;
            dbt.SrcBlendAlpha=D3D12_BLEND_ZERO;dbt.DestBlendAlpha=D3D12_BLEND_ONE;dbt.BlendOpAlpha=D3D12_BLEND_OP_ADD;dbt.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            dd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            check(d->CreateGraphicsPipelineState(&dd,IID_PPV_ARGS(&s.denoiseResolve)));
            dd.PS={unpPs.data(),unpPs.size()};
            check(d->CreateGraphicsPipelineState(&dd,IID_PPV_ARGS(&s.nrdUnpackPs)));

            // NRD REBLUR pack compute root signature: b0(space1) 4 dwords + t0..t3(space1) + u0..u3(space1)
            D3D12_DESCRIPTOR_RANGE csSrv{};csSrv.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;csSrv.NumDescriptors=4;csSrv.BaseShaderRegister=0;csSrv.RegisterSpace=1;
            D3D12_DESCRIPTOR_RANGE csUav{};csUav.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;csUav.NumDescriptors=4;csUav.BaseShaderRegister=0;csUav.RegisterSpace=1;
            D3D12_ROOT_PARAMETER cp[3]{};
            cp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;cp[0].Constants={0,1,4};cp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
            cp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;cp[1].DescriptorTable={1,&csSrv};cp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
            cp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;cp[2].DescriptorTable={1,&csUav};cp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC crs{};crs.NumParameters=3;crs.pParameters=cp;
            ComPtr<ID3DBlob> cb,ce;check(D3D12SerializeRootSignature(&crs,D3D_ROOT_SIGNATURE_VERSION_1,&cb,&ce));
            check(d->CreateRootSignature(0,cb->GetBufferPointer(),cb->GetBufferSize(),IID_PPV_ARGS(&s.nrdPackRoot)));
            const auto packCs=shader("m2_nrd_pack_cs.cso");
            D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};cpd.pRootSignature=s.nrdPackRoot.Get();cpd.CS={packCs.data(),packCs.size()};
            check(d->CreateComputePipelineState(&cpd,IID_PPV_ARGS(&s.nrdPackCs)));
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=8;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.rtv)));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.dsv)));
        s.zero=buffer(d,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);uint64_t zero=0;upload(s.zero.Get(),&zero,8);
        for(uint32_t i=0;i<4;++i) {s.counters[i]=buffer(d,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);s.readbacks[i]=buffer(d,256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);}
        if(s.rtReady) {
            const size_t count=s.parts.size();
            const size_t totalBlas=count+s.marbleTemplates.size();
            s.maxTlasInstances=count+InteractiveView::kMaxMarbles;
            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(totalBlas);
            std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> inputs(totalBlas);
            uint64_t scratchBytes=256;
            for(size_t i=0;i<totalBlas;++i) {
                Impl::Part& pRef = (i < count) ? s.parts[i] : s.marbleTemplates[i - count];
                auto& g=geometries[i];g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags=isCutoutMaterial(pRef.sub.material)?D3D12_RAYTRACING_GEOMETRY_FLAG_NONE:D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                g.Triangles.VertexBuffer={s.vb->GetGPUVirtualAddress(),sizeof(BmeshVertex)};g.Triangles.VertexCount=static_cast<UINT>(vertices.size());g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexBuffer=s.ib->GetGPUVirtualAddress()+uint64_t(pRef.sub.firstIndex)*4;g.Triangles.IndexCount=pRef.sub.indexCount;g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
                auto& in=inputs[i];in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;in.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;in.NumDescs=1;in.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;in.pGeometryDescs=&g;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&in,&pre);scratchBytes=std::max(scratchBytes,pre.ScratchDataSizeInBytes);
                pRef.blas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            s.scratch=buffer(d,scratchBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};ti.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;ti.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;ti.NumDescs=static_cast<UINT>(s.maxTlasInstances);ti.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&ti,&pre);
            s.tlas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            s.tlasScratch=buffer(d,pre.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            s.instances=buffer(d,s.maxTlasInstances*sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            if(!s.context.executeSingleCommandListAndWait([&](void* raw) {
                ComPtr<ID3D12GraphicsCommandList4> cmd;check(static_cast<ID3D12GraphicsCommandList*>(raw)->QueryInterface(IID_PPV_ARGS(&cmd)));
                for(size_t i=0;i<totalBlas;++i) {
                    Impl::Part& pRef = (i < count) ? s.parts[i] : s.marbleTemplates[i - count];
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=inputs[i];build.ScratchAccelerationStructureData=s.scratch->GetGPUVirtualAddress();build.DestAccelerationStructureData=pRef.blas->GetGPUVirtualAddress();cmd->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(cmd.Get(),s.scratch.Get());uav(cmd.Get(),pRef.blas.Get());
                }
            },error))return false;
        }
        s.nrd.initialize(nullptr);
        BOO_LOG_INFO("M2Mesh","Loaded "+std::to_string(vertices.size())+" vertices / "+std::to_string(indices.size()/3)+" triangles + 42 PBR material arrays; fallback kits="+std::to_string(s.fallbacks));
        return true;
    }catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool TrilogyMeshRenderer::resize(uint32_t width,uint32_t height) {
    auto& s=*impl_;
    if(!width||!height||!s.context.deviceRaw())return false;
    if(s.w==width&&s.h==height)return s.context.checkDeviceHealthy();
    if(!s.context.waitForGpu()||!s.context.checkDeviceHealthy())return false;
    try {
        auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=width;rd.Height=height;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_D32_FLOAT;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=rd.Format;clear.DepthStencil.Depth=1;ComPtr<ID3D12Resource> depth;
        check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)));s.depth=std::move(depth);d->CreateDepthStencilView(s.depth.Get(),nullptr,s.dsv->GetCPUDescriptorHandleForHeapStart());
        rd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;ComPtr<ID3D12Resource> acc;
        check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&acc)));s.accum=std::move(acc);s.accumValid=false;

        if (s.rtReady) {
            auto makeTex = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState) {
                D3D12_RESOURCE_DESC td = rd;
                td.Format = fmt;
                td.Flags = flags;
                ComPtr<ID3D12Resource> tex;
                check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, initState, nullptr, IID_PPV_ARGS(&tex)));
                return tex;
            };
            s.gbufNormRough  = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            s.gbufDiffRadHit = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            s.gbufSpecRadHit = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            s.gbufModViewZ   = makeTex(DXGI_FORMAT_R32G32B32A32_UINT,  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            s.nrdMv          = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);

            s.nrdNormRough   = makeTex(DXGI_FORMAT_R10G10B10A2_UNORM,  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.nrdViewZ       = makeTex(DXGI_FORMAT_R32_FLOAT,          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.nrdDiffIn      = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.nrdSpecIn      = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // Contiguous RTV slots [2..6] match SV_Target2..SV_Target6; slot [7] holds s.accum
            const UINT rtvInc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto rtvAt = [&](UINT idx) {
                auto h = s.rtv->GetCPUDescriptorHandleForHeapStart();
                h.ptr += SIZE_T(idx) * rtvInc;
                return h;
            };
            d->CreateRenderTargetView(s.gbufNormRough.Get(),  nullptr, rtvAt(2u));
            d->CreateRenderTargetView(s.gbufDiffRadHit.Get(), nullptr, rtvAt(3u));
            d->CreateRenderTargetView(s.gbufSpecRadHit.Get(), nullptr, rtvAt(4u));
            d->CreateRenderTargetView(s.gbufModViewZ.Get(),   nullptr, rtvAt(5u));
            d->CreateRenderTargetView(s.nrdMv.Get(),          nullptr, rtvAt(6u));
        }
        s.reblurPrevValid=false;
        s.nrd.resize(width,height);
        s.w=width;s.h=height;return true;
    }catch(...) {return false;}
}
void TrilogyMeshRenderer::retire(uint32_t slot) {
    auto& s=*impl_;if(!s.pending[slot])return;void* ptr=nullptr;D3D12_RANGE rr{0,8};
    if(SUCCEEDED(s.readbacks[slot]->Map(0,&rr,&ptr))) {s.hits=*static_cast<uint32_t*>(ptr);s.queries=static_cast<uint32_t*>(ptr)[1];s.verified=s.hits>0;D3D12_RANGE wr{0,0};s.readbacks[slot]->Unmap(0,&wr);}s.pending[slot]=false;
}
bool TrilogyMeshRenderer::record(void* raw,const FrameContext& f,const M1CameraState& camera,void* hdrRaw,void* distRaw,uint64_t tensor,uint32_t width,uint32_t height,uint32_t slot,bool raytracing,bool canonical) {
    auto& s=*impl_;auto* cmd=static_cast<ID3D12GraphicsCommandList*>(raw);auto* hdr=static_cast<ID3D12Resource*>(hdrRaw);auto* dist=static_cast<ID3D12Resource*>(distRaw);auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());
    if(canonical)retire(slot);
    const bool useRt=raytracing&&s.rtReady&&(s.w==width&&s.h==height&&s.gbufNormRough!=nullptr);
    if(!useRt&&canonical){s.verified=false;s.denoiseMode=0u;}
    const bool isM2=(s.visualMode==ShowVisualMode::M2Trilogy180s);
    const float filmTime=float(double(f.samplePosition)/f.sampleRate);
    const uint32_t act=isM2
        ? static_cast<uint32_t>(std::clamp<int64_t>(f.samplePosition/2880000,0,2))
        : ((s.visualMode==ShowVisualMode::FullFilm360s&&filmTime>=245.f&&filmTime<275.f)?1u:2u);

    const InteractiveView& iv=f.interactive;
    const uint32_t dynMarbles=std::min<uint32_t>(iv.marbleCount,InteractiveView::kMaxMarbles);

    struct DrawEntry { DrawConstants dc; uint32_t indexCount; uint32_t material; uint32_t mask; };
    std::vector<DrawEntry> drawList;
    drawList.reserve(s.parts.size() + dynMarbles);
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> inst;
    inst.reserve(s.parts.size() + dynMarbles);

    for(const auto& part:s.parts) {
        auto draw=s.transform(part,f,camera);
        const bool active=isM2?(part.kit==act||(act==2&&part.kit>=2)):s.isPartActiveInShow(part,filmTime);
        const bool thinGlass=(part.sub.material==3||part.sub.material==4||part.sub.material==18||part.sub.material==21||part.sub.material==35||part.sub.material==40);
        const uint32_t mask=active?(thinGlass?2u:1u):0u;
        drawList.push_back({draw, part.sub.indexCount, part.sub.material, mask});
        D3D12_RAYTRACING_INSTANCE_DESC id{};std::memcpy(id.Transform,draw.rows,48);id.InstanceID=part.sub.firstIndex;id.InstanceMask=mask;id.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;if(part.blas)id.AccelerationStructure=part.blas->GetGPUVirtualAddress();inst.push_back(id);
    }

    // Append dynamic rigid-body ray-traced physics marbles (with 3D rotation quaternion + world motion vectors!)
    for (uint32_t m = 0; m < dynMarbles; ++m) {
        const float rad = std::clamp(iv.marblePosRad[m][3], 0.005f, 0.25f);
        const uint32_t reqMat = iv.marbleMat[m];
        size_t tplIdx = 0;
        for (size_t k = 0; k < kMarbleTemplateMats.size(); ++k) {
            if (kMarbleTemplateMats[k] == reqMat) { tplIdx = k; break; }
        }
        const Impl::Part& tpl = s.marbleTemplates[tplIdx];
        const float qx = iv.marbleRotQuat[m][0], qy = iv.marbleRotQuat[m][1], qz = iv.marbleRotQuat[m][2];
        float qw = iv.marbleRotQuat[m][3];
        const float qLen2 = qx * qx + qy * qy + qz * qz + qw * qw;
        const float invQ = (qLen2 > 1e-8f) ? (1.0f / std::sqrt(qLen2)) : 0.0f;
        const float x = (qLen2 > 1e-8f) ? (qx * invQ) : 0.0f;
        const float y = (qLen2 > 1e-8f) ? (qy * invQ) : 0.0f;
        const float z = (qLen2 > 1e-8f) ? (qz * invQ) : 0.0f;
        qw = (qLen2 > 1e-8f) ? (qw * invQ) : 1.0f;

        DrawConstants dc{};
        dc.rows[0]  = (1.0f - 2.0f * (y * y + z * z)) * rad;
        dc.rows[1]  = (2.0f * (x * y - z * qw)) * rad;
        dc.rows[2]  = (2.0f * (x * z + y * qw)) * rad;
        dc.rows[3]  = iv.marblePosRad[m][0];
        dc.rows[4]  = (2.0f * (x * y + z * qw)) * rad;
        dc.rows[5]  = (1.0f - 2.0f * (x * x + z * z)) * rad;
        dc.rows[6]  = (2.0f * (y * z - x * qw)) * rad;
        dc.rows[7]  = iv.marblePosRad[m][1];
        dc.rows[8]  = (2.0f * (x * z - y * qw)) * rad;
        dc.rows[9]  = (2.0f * (y * z + x * qw)) * rad;
        dc.rows[10] = (1.0f - 2.0f * (x * x + y * y)) * rad;
        dc.rows[11] = iv.marblePosRad[m][2];
        dc.first = tpl.sub.firstIndex;
        dc.mat   = tpl.sub.material;
        dc.part  = tpl.sub.part;
        packDrawMotionVectors(
            dc,
            iv.marblePrevPos[m][0] - iv.marblePosRad[m][0],
            iv.marblePrevPos[m][1] - iv.marblePosRad[m][1],
            iv.marblePrevPos[m][2] - iv.marblePosRad[m][2]);

        const bool thinGlass = (dc.mat == 3u || dc.mat == 4u || dc.mat == 35u);
        const uint32_t mask = thinGlass ? 2u : 1u;
        drawList.push_back({dc, tpl.sub.indexCount, dc.mat, mask});

        D3D12_RAYTRACING_INSTANCE_DESC id{};
        std::memcpy(id.Transform, dc.rows, 48);
        id.InstanceID = tpl.sub.firstIndex;
        id.InstanceMask = mask;
        id.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        if (tpl.blas) id.AccelerationStructure = tpl.blas->GetGPUVirtualAddress();
        inst.push_back(id);
    }

    if(useRt&&canonical) {
        const size_t offset=(slot&3u)*s.maxTlasInstances*sizeof(inst[0]);
        upload(s.instances.Get(),inst.data(),inst.size()*sizeof(inst[0]),offset);
        ComPtr<ID3D12GraphicsCommandList4> c4;check(cmd->QueryInterface(IID_PPV_ARGS(&c4)));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.NumDescs=static_cast<UINT>(inst.size());build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.InstanceDescs=s.instances->GetGPUVirtualAddress()+offset;build.DestAccelerationStructureData=s.tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=s.tlasScratch->GetGPUVirtualAddress();uav(cmd,s.tlas.Get());uav(cmd,s.tlasScratch.Get());c4->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(cmd,s.tlas.Get());
    }
    cmd->CopyBufferRegion(s.counters[slot].Get(),0,s.zero.Get(),0,8);transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,hdr,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);transition(cmd,dist,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const UINT rtvInc=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle=s.rtv->GetCPUDescriptorHandleForHeapStart();d->CreateRenderTargetView(hdr,nullptr,handle);auto second=handle;second.ptr+=rtvInc;d->CreateRenderTargetView(dist,nullptr,second);
    auto depthHandle=s.dsv->GetCPUDescriptorHandleForHeapStart();cmd->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    cmd->OMSetRenderTargets(useRt?7u:2u,&handle,TRUE,&depthHandle);
    D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};D3D12_RECT sc{0,0,LONG(width),LONG(height)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);

    SceneConstants c{};std::copy_n(camera.eye,3,c.eye);c.fov=camera.fovYRadians;c.aspect=float(width)/float(height);for(int i=0;i<3;++i)c.forward[i]=camera.target[i]-camera.eye[i];normalize(c.forward);cross(camera.up,c.forward,c.right);normalize(c.right);cross(c.forward,c.right,c.up);c.time=filmTime;c.reveal=camera.revealOpen01;c.width=width;c.height=height;c.act=act;c.canonical=canonical?1u:0u;
    if(isM2) {
        c.day=act==1?1-std::clamp((c.time-72)/32,0.0f,1.0f):0;
        c.showFlags=0u;
        c.moodAccent=0.0f;
    } else {
        auto sm=[](float v){v=std::clamp(v,0.f,1.f);return v*v*v*(v*(v*6.f-15.f)+10.f);};
        c.day=(s.visualMode==ShowVisualMode::FullFilm360s&&c.time>=335.5f)?(0.84f*sm((c.time-335.5f)/12.5f)):0.0f;
        const bool isHijack=(s.visualMode==ShowVisualMode::FullFilm360s&&c.time>=245.f&&c.time<275.f);
        c.showFlags=1u|((f.edition==Edition::Midnight)?2u:0u)|(std::min<uint32_t>(camera.shotIndex,7u)<<2u);
        c.moodAccent=isHijack?0.5f:std::clamp(f.moodPaletteAccent,0.0f,1.0f);
        c.moodBloom=isHijack?0.35f:std::clamp(f.moodBloomStrength,0.f,1.f);
        c.moodLight=isHijack?0.35f:std::clamp(f.moodIntensity,0.f,1.f);
        c.moodCut=isHijack?0.f:std::clamp(f.moodCutAccent,0.f,1.f);
    }
    c.pulse=camera.emissivePulse;
    // Audio drive packed as RGBA8 into the one spare scene DWORD (low, mid, high, beat). Debug AOVs and progressive accumulation stay static.
    {const uint32_t packed=(f.interactive.aovMode==0u&&!f.interactive.accumulate)?AudioReactiveFollower::pack(f.audioReactive):0u;std::memcpy(&c.pad,&packed,sizeof(packed));}

    const uint32_t curAov = std::min<uint32_t>(iv.aovMode, 7u);
    const bool accumulate = canonical && (curAov == 0u) && iv.accumulate && s.accum && s.accumBlend && s.w == width && s.h == height;
    const bool interactiveMotion = canonical && useRt && (curAov == 0u) && !accumulate &&
                                   (iv.exploreActive || iv.sunOverride || iv.lampsOff || dynMarbles > 0u) &&
                                   s.nrd.isAvailable();
    const bool splitDenoise = canonical && useRt && (curAov == 0u);

    if(iv.sunOverride){std::copy_n(iv.sunDir,3,c.sunDir);c.sunScale=iv.sunScale;c.interFlags|=1u;}
    if(iv.lampsOff)c.interFlags|=4u;
    if(iv.exploreActive)c.interFlags|=8u;
    if(splitDenoise)c.interFlags|=16u;
    c.interFlags |= (curAov << 8u);

    float reblurJitterPx[2]{0.0f, 0.0f};
    if(accumulate){
        c.interFlags|=2u;c.accumFrame=iv.accumFrame;
        const uint32_t k=(iv.accumFrame%64u)+1u;
        c.jitter[0]=(halton(k,2)-0.5f)*2.0f/float(width);c.jitter[1]=(halton(k,3)-0.5f)*2.0f/float(height);
    } else if (interactiveMotion) {
        c.interFlags|=32u;
        c.accumFrame = ++s.reblurFrameCounter;
        const uint32_t k = (c.accumFrame % 16u) + 1u;
        reblurJitterPx[0] = halton(k, 2) - 0.5f;
        reblurJitterPx[1] = halton(k, 3) - 0.5f;
        c.jitter[0] = reblurJitterPx[0] * 2.0f / float(width);
        c.jitter[1] = reblurJitterPx[1] * 2.0f / float(height);
    }

    (void)tensor;
    ID3D12DescriptorHeap* m2Heaps[] = {s.srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, m2Heaps);
    cmd->SetGraphicsRootSignature(s.root.Get());cmd->SetGraphicsRoot32BitConstants(0,36,&c,0);cmd->SetGraphicsRootShaderResourceView(2,s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(3,s.ib->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(4,s.tlas?s.tlas->GetGPUVirtualAddress():s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootDescriptorTable(5,s.srvHeap->GetGPUDescriptorHandleForHeapStart());cmd->SetGraphicsRootUnorderedAccessView(6,s.counters[slot]->GetGPUVirtualAddress());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Hardware Z-Prepass eliminates all hidden-surface DXR ray-query & cutout overdraw across overlapping kits
    cmd->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
    cmd->SetPipelineState(s.depthOpaque.Get());
    bool hasActiveCutout=false;
    for(const auto& de : drawList) {
        if(!de.mask) continue;
        if(isCutoutMaterial(de.material)) { hasActiveCutout=true; continue; }
        cmd->SetGraphicsRoot32BitConstants(1,16,&de.dc,0);
        cmd->DrawInstanced(de.indexCount,1,0,0);
    }
    if(hasActiveCutout) {
        cmd->SetPipelineState(s.depthCutout.Get());
        for(const auto& de : drawList) {
            if(de.mask && isCutoutMaterial(de.material)) {
                cmd->SetGraphicsRoot32BitConstants(1,16,&de.dc,0);
                cmd->DrawInstanced(de.indexCount,1,0,0);
            }
        }
    }
    cmd->OMSetRenderTargets(useRt?7u:2u,&handle,TRUE,&depthHandle);
    cmd->SetPipelineState(useRt?s.rt.Get():s.raster.Get());
    for(const auto& de : drawList) {
        if(!de.mask || isCutoutMaterial(de.material)) continue;
        cmd->SetGraphicsRoot32BitConstants(1,16,&de.dc,0);
        cmd->DrawInstanced(de.indexCount,1,0,0);
    }
    if(hasActiveCutout) {
        cmd->SetPipelineState(useRt?s.rtCutout.Get():s.rasterCutout.Get());
        for(const auto& de : drawList) {
            if(de.mask && isCutoutMaterial(de.material)) {
                cmd->SetGraphicsRoot32BitConstants(1,16,&de.dc,0);
                cmd->DrawInstanced(de.indexCount,1,0,0);
            }
        }
    }
    // Sky last, at the far plane: only pixels the Z-prepass left at the cleared depth run the sky
    // shader, so its ray-marched light shafts never trace under geometry.
    cmd->SetPipelineState(useRt ? s.skyRt.Get() : s.sky.Get());cmd->DrawInstanced(3,1,0,0);

    const UINT srvInc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT slotBase = 5u + 20u * (slot & 3u);
    auto cpuAt = [&](UINT i) { auto h = s.srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * srvInc; return h; };
    auto gpuAt = [&](UINT i) { auto h = s.srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(i) * srvInc; return h; };

    auto runDeterministicSpatialResolve = [&](float spatialScale) {
        transition(cmd, s.gbufNormRough.Get(),  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(cmd, s.gbufDiffRadHit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(cmd, s.gbufSpecRadHit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(cmd, s.gbufModViewZ.Get(),   D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        const UINT b = slotBase + 2u;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d->CreateShaderResourceView(s.gbufNormRough.Get(),  &sd, cpuAt(b + 0u));
        d->CreateShaderResourceView(s.gbufDiffRadHit.Get(), &sd, cpuAt(b + 1u));
        d->CreateShaderResourceView(s.gbufSpecRadHit.Get(), &sd, cpuAt(b + 2u));
        sd.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        d->CreateShaderResourceView(s.gbufModViewZ.Get(),   &sd, cpuAt(b + 3u));

        struct { uint32_t w, h; float scale; uint32_t flags; } rcb{width, height, spatialScale, 0u};
        cmd->SetDescriptorHeaps(1, m2Heaps);
        cmd->OMSetRenderTargets(1u, &handle, FALSE, nullptr);
        cmd->SetGraphicsRootSignature(s.denoiseRoot.Get());
        cmd->SetPipelineState(s.denoiseResolve.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->SetGraphicsRoot32BitConstants(0u, 4u, &rcb, 0u);
        cmd->SetGraphicsRootDescriptorTable(1u, gpuAt(b));
        cmd->DrawInstanced(3u, 1u, 0u, 0u);

        transition(cmd, s.gbufNormRough.Get(),  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(cmd, s.gbufDiffRadHit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(cmd, s.gbufSpecRadHit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(cmd, s.gbufModViewZ.Get(),   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    };

    if (splitDenoise) {
        bool reblurDone = false;
        if (interactiveMotion) {
            float curW2V[16]{}, curV2C[16]{};
            buildColumnMajorCameraMatrices(c.eye, c.right, c.up, c.forward, c.fov, c.aspect, curW2V, curV2C);

            const float dx = c.eye[0] - s.prevEye[0], dy = c.eye[1] - s.prevEye[1], dz = c.eye[2] - s.prevEye[2];
            const float eyeJump2 = dx * dx + dy * dy + dz * dz;
            const float fwdDot = c.forward[0] * s.prevForward[0] + c.forward[1] * s.prevForward[1] + c.forward[2] * s.prevForward[2];
            float sunDot = 1.0f;
            if (iv.sunOverride && s.prevSunOverride) {
                sunDot = c.sunDir[0] * s.prevSunDir[0] + c.sunDir[1] * s.prevSunDir[1] + c.sunDir[2] * s.prevSunDir[2];
            }
            const bool resetHist = !s.reblurPrevValid || (eyeJump2 > 1.44f) || (fwdDot < 0.70f) ||
                                   (iv.lampsOff != s.prevLampsOff) || (iv.sunOverride != s.prevSunOverride) || (sunDot < 0.94f);

            const auto srvBoth = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transition(cmd, s.gbufNormRough.Get(),  D3D12_RESOURCE_STATE_RENDER_TARGET, srvBoth);
            transition(cmd, s.gbufDiffRadHit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, srvBoth);
            transition(cmd, s.gbufSpecRadHit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, srvBoth);
            transition(cmd, s.gbufModViewZ.Get(),   D3D12_RESOURCE_STATE_RENDER_TARGET, srvBoth);
            transition(cmd, s.nrdMv.Get(),          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            const UINT pb = slotBase + 6u;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d->CreateShaderResourceView(s.gbufNormRough.Get(),  &sd, cpuAt(pb + 0u));
            d->CreateShaderResourceView(s.gbufDiffRadHit.Get(), &sd, cpuAt(pb + 1u));
            d->CreateShaderResourceView(s.gbufSpecRadHit.Get(), &sd, cpuAt(pb + 2u));
            sd.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            d->CreateShaderResourceView(s.gbufModViewZ.Get(),   &sd, cpuAt(pb + 3u));

            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ud.Format = DXGI_FORMAT_R10G10B10A2_UNORM; d->CreateUnorderedAccessView(s.nrdNormRough.Get(), nullptr, &ud, cpuAt(pb + 4u));
            ud.Format = DXGI_FORMAT_R32_FLOAT;         d->CreateUnorderedAccessView(s.nrdViewZ.Get(),     nullptr, &ud, cpuAt(pb + 5u));
            ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d->CreateUnorderedAccessView(s.nrdDiffIn.Get(), nullptr, &ud, cpuAt(pb + 6u));
            d->CreateUnorderedAccessView(s.nrdSpecIn.Get(), nullptr, &ud, cpuAt(pb + 7u));

            const uint32_t packCb[4] = {width, height, 0u, 0u};
            cmd->SetComputeRootSignature(s.nrdPackRoot.Get());
            cmd->SetPipelineState(s.nrdPackCs.Get());
            cmd->SetComputeRoot32BitConstants(0u, 4u, packCb, 0u);
            cmd->SetComputeRootDescriptorTable(1u, gpuAt(pb));
            cmd->SetComputeRootDescriptorTable(2u, gpuAt(pb + 4u));
            cmd->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

            transition(cmd, s.nrdNormRough.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(cmd, s.nrdViewZ.Get(),     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(cmd, s.nrdDiffIn.Get(),    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(cmd, s.nrdSpecIn.Get(),    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            NrdBridge::ReblurInputs rin{};
            rin.inMv              = s.nrdMv.Get();
            rin.inNormalRoughness = s.nrdNormRough.Get();
            rin.inViewZ           = s.nrdViewZ.Get();
            rin.inDiffRadHit      = s.nrdDiffIn.Get();
            rin.inSpecRadHit      = s.nrdSpecIn.Get();
            std::memcpy(rin.worldToView,     curW2V, sizeof(curW2V));
            std::memcpy(rin.worldToViewPrev, resetHist ? curW2V : s.prevWorldToView, sizeof(curW2V));
            std::memcpy(rin.viewToClip,      curV2C, sizeof(curV2C));
            std::memcpy(rin.viewToClipPrev,  resetHist ? curV2C : s.prevViewToClip,  sizeof(curV2C));
            rin.jitterPixels[0] = reblurJitterPx[0];
            rin.jitterPixels[1] = reblurJitterPx[1];
            rin.resetHistory    = resetHist;

            const bool nrdOk = s.nrd.recordReblurMotion(cmd, width, height, slot, rin);
            cmd->SetDescriptorHeaps(1, m2Heaps);

            if (nrdOk && s.nrd.outDiffResourceRaw() && s.nrd.outSpecResourceRaw()) {
                const UINT ub = slotBase + 14u;
                sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                d->CreateShaderResourceView(static_cast<ID3D12Resource*>(s.nrd.outDiffResourceRaw()), &sd, cpuAt(ub + 0u));
                d->CreateShaderResourceView(static_cast<ID3D12Resource*>(s.nrd.outSpecResourceRaw()), &sd, cpuAt(ub + 1u));
                d->CreateShaderResourceView(s.gbufNormRough.Get(), &sd, cpuAt(ub + 2u));
                sd.Format = DXGI_FORMAT_R32G32B32A32_UINT;
                d->CreateShaderResourceView(s.gbufModViewZ.Get(),  &sd, cpuAt(ub + 3u));

                const uint32_t ucb[4] = {width, height, 0u, 0u};
                cmd->OMSetRenderTargets(1u, &handle, FALSE, nullptr);
                cmd->SetGraphicsRootSignature(s.denoiseRoot.Get());
                cmd->SetPipelineState(s.nrdUnpackPs.Get());
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->SetGraphicsRoot32BitConstants(0u, 4u, ucb, 0u);
                cmd->SetGraphicsRootDescriptorTable(1u, gpuAt(ub));
                cmd->DrawInstanced(3u, 1u, 0u, 0u);

                std::memcpy(s.prevWorldToView, curW2V, sizeof(curW2V));
                std::memcpy(s.prevViewToClip,  curV2C, sizeof(curV2C));
                std::copy_n(c.eye, 3, s.prevEye);
                std::copy_n(c.forward, 3, s.prevForward);
                std::copy_n(c.sunDir, 3, s.prevSunDir);
                s.prevSunOverride = iv.sunOverride;
                s.prevLampsOff    = iv.lampsOff;
                s.reblurPrevValid = true;
                s.denoiseMode     = 1u;
                reblurDone        = true;
            }

            transition(cmd, s.nrdNormRough.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cmd, s.nrdViewZ.Get(),     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cmd, s.nrdDiffIn.Get(),    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cmd, s.nrdSpecIn.Get(),    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cmd, s.nrdMv.Get(),        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(cmd, s.gbufNormRough.Get(),  srvBoth, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(cmd, s.gbufDiffRadHit.Get(), srvBoth, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(cmd, s.gbufSpecRadHit.Get(), srvBoth, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(cmd, s.gbufModViewZ.Get(),   srvBoth, D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else if (canonical) {
            s.reblurPrevValid = false;
        }

        if (!reblurDone) {
            const float spatialScale = accumulate ? (1.0f / (1.0f + 0.35f * float(iv.accumFrame))) : 1.0f;
            runDeterministicSpatialResolve(spatialScale);
            if (canonical) s.denoiseMode = accumulate ? 2u : 0u;
        }
    } else if (canonical) {
        s.reblurPrevValid = false;
        s.denoiseMode = 0u;
    }

    if(accumulate) {
        // Prefer NVIDIA NRD.dll dynamic DXIL compute sidecar; fall back cleanly to built-in HLSL accumulator
        const uint32_t kJ=(iv.accumFrame%64u)+1u;
        const float jitterPx[2]={halton(kJ,2)-0.5f,halton(kJ,3)-0.5f};
        const bool reset=(iv.accumFrame==0u)||!s.accumValid;
        const bool nrdRan=s.nrd.isAvailable()&&s.nrd.recordAccumulation(cmd,hdr,width,height,slot,reset?0u:iv.accumFrame,jitterPx);
        cmd->SetDescriptorHeaps(1, m2Heaps);
        if(nrdRan) {
            s.accumValid=true;
            s.denoiseMode=2u;
        } else {
            const float wgt=reset?1.0f:1.0f/float(std::min<uint32_t>(iv.accumFrame,4095u)+1u);
            const UINT base=slotBase;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
            sv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d->CreateShaderResourceView(hdr,&sv,cpuAt(base));
            sv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d->CreateShaderResourceView(s.accum.Get(),&sv,cpuAt(base+1u));
            auto accRtv=s.rtv->GetCPUDescriptorHandleForHeapStart();accRtv.ptr+=SIZE_T(7)*rtvInc;d->CreateRenderTargetView(s.accum.Get(),nullptr,accRtv);
            transition(cmd,hdr,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(cmd,s.accum.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->SetGraphicsRootSignature(s.accumRoot.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->OMSetRenderTargets(1,&accRtv,FALSE,nullptr);const float bf[4]={wgt,wgt,wgt,wgt};cmd->OMSetBlendFactor(bf);
            cmd->SetPipelineState(s.accumBlend.Get());cmd->SetGraphicsRootDescriptorTable(0,gpuAt(base));cmd->DrawInstanced(3,1,0,0);
            transition(cmd,s.accum.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(cmd,hdr,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->OMSetRenderTargets(1,&handle,FALSE,nullptr);
            cmd->SetPipelineState(s.accumResolve.Get());cmd->SetGraphicsRootDescriptorTable(0,gpuAt(base+1u));cmd->DrawInstanced(3,1,0,0);
            s.accumValid=true;
            s.denoiseMode=2u;
        }
    } else if(canonical) {
        s.accumValid=false;
    }
    transition(cmd,hdr,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);transition(cmd,dist,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(canonical&&useRt) {cmd->CopyBufferRegion(s.readbacks[slot].Get(),0,s.counters[slot].Get(),0,8);s.pending[slot]=true;}
    transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    return useRt;
}
bool TrilogyMeshRenderer::hardwareVerified() const {return impl_->verified;}
bool TrilogyMeshRenderer::nrdActive() const {return impl_&&impl_->nrd.isAvailable();}
uint32_t TrilogyMeshRenderer::activeDenoiseMode() const noexcept {return impl_?impl_->denoiseMode:0u;}
uint32_t TrilogyMeshRenderer::queriedRays() const {return impl_->queries;}
uint32_t TrilogyMeshRenderer::verifiedHits() const {return impl_->hits;}
uint32_t TrilogyMeshRenderer::fallbackKits() const {return impl_->fallbacks;}
bool TrilogyMeshRenderer::hasAllAuthoredSceneAssets() const noexcept {
    return impl_->fallbacks == 0 && !impl_->usedFallbackTexture;
}
}
