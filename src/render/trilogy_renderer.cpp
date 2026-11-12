#include "boo/trilogy_renderer.hpp"
#include "boo/bmesh.hpp"
#include "boo/btex.hpp"
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
struct SceneConstants {float eye[3];float fov;float right[3];float aspect;float up[3];float time;float forward[3];float reveal;uint32_t width,height,act,canonical;float day,pulse;uint32_t showFlags;float moodAccent;float moodBloom,moodLight,moodCut,pad;};
void normalize(float v[3]) {float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);for(int c=0;c<3;++c)v[c]/=std::max(l,1e-6f);}
void cross(const float a[3],const float b[3],float r[3]) {r[0]=a[1]*b[2]-a[2]*b[1];r[1]=a[2]*b[0]-a[0]*b[2];r[2]=a[0]*b[1]-a[1]*b[0];}
bool isCutoutMaterial(uint32_t m) noexcept { return m == 29u || m == 36u; }

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
    ComPtr<ID3D12Resource> vb,ib,scratch,tlas,tlasScratch,instances,depth;
    ComPtr<ID3D12Resource> albedoTex,normalTex,ormtTex,envIblTex,fallbackTensorTex;
    ComPtr<ID3D12DescriptorHeap> rtv,dsv,srvHeap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> depthOpaque,depthCutout,raster,rasterCutout,rt,rtCutout,sky;
    std::array<ComPtr<ID3D12Resource>,4> counters,readbacks;
    ComPtr<ID3D12Resource> zero;
    std::array<bool,4> pending{};
    uint32_t w=0,h=0,hits=0,queries=0,fallbacks=0;
    bool verified=false,rtReady=false;
    explicit Impl(D3D12Context& c):context(c){}
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
        s.vb=buffer(d,vertices.size()*sizeof(BmeshVertex),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);upload(s.vb.Get(),vertices.data(),vertices.size()*sizeof(BmeshVertex));
        s.ib=buffer(d,indices.size()*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);upload(s.ib.Get(),indices.data(),indices.size()*4);

        // Load & upload authored/scanned PBR BTEX001 texture arrays (with deterministic fallback when fault==true or missing)
        Btex bAlb, bNor, bOrm, bEnv;
        std::string texWhy;
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_albedo_alpha.btex"), bAlb, &texWhy)) {
            bAlb = buildFallbackAlbedoArray(42u);
            BOO_LOG_WARN("M2Tex", "pbr_albedo_alpha.btex fallback: " + texWhy);
        }
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_normal.btex"), bNor, &texWhy)) {
            bNor = buildFallbackNormalArray(42u);
            BOO_LOG_WARN("M2Tex", "pbr_normal.btex fallback: " + texWhy);
        }
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_ormt.btex"), bOrm, &texWhy)) {
            bOrm = buildFallbackOrmtArray(42u);
            BOO_LOG_WARN("M2Tex", "pbr_ormt.btex fallback: " + texWhy);
        }
        if (fault || !loadBtex(resolveResourcePath("assets/textures/pbr_ibl_env.btex"), bEnv, &texWhy)) {
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
        shd.NumDescriptors = 5;
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
        for(UINT i=0;i<2;++i) {params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[i].Constants.ShaderRegister=i;params[i].Constants.Num32BitValues=i?16:28;}
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
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=2;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
        check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.raster)));
        pd.PS={psCut.data(),psCut.size()};check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.rasterCutout)));
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};ComPtr<ID3D12Device5> d5;
        s.rtReady=!s.context.isWarp()&&SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))&&options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1&&SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5)));
        if(s.rtReady) {
            auto rps=shader("m2_mesh_rt_ps.cso");pd.PS={rps.data(),rps.size()};check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.rt)));
            auto rpsCut=shader("m2_mesh_rt_cutout_ps.cso");pd.PS={rpsCut.data(),rpsCut.size()};check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.rtCutout)));
        }
        pd.VS={skyvs.data(),skyvs.size()};pd.PS={skyps.data(),skyps.size()};pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.sky)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.rtv)));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.dsv)));
        s.zero=buffer(d,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);uint64_t zero=0;upload(s.zero.Get(),&zero,8);
        for(uint32_t i=0;i<4;++i) {s.counters[i]=buffer(d,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);s.readbacks[i]=buffer(d,256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);}
        if(s.rtReady) {
            const size_t count=s.parts.size();std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(count);std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> inputs(count);uint64_t scratchBytes=256;
            for(size_t i=0;i<count;++i) {
                auto& g=geometries[i];g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags=isCutoutMaterial(s.parts[i].sub.material)?D3D12_RAYTRACING_GEOMETRY_FLAG_NONE:D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                g.Triangles.VertexBuffer={s.vb->GetGPUVirtualAddress(),sizeof(BmeshVertex)};g.Triangles.VertexCount=static_cast<UINT>(vertices.size());g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexBuffer=s.ib->GetGPUVirtualAddress()+uint64_t(s.parts[i].sub.firstIndex)*4;g.Triangles.IndexCount=s.parts[i].sub.indexCount;g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
                auto& in=inputs[i];in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;in.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;in.NumDescs=1;in.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;in.pGeometryDescs=&g;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&in,&pre);scratchBytes=std::max(scratchBytes,pre.ScratchDataSizeInBytes);
                s.parts[i].blas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            s.scratch=buffer(d,scratchBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};ti.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;ti.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;ti.NumDescs=static_cast<UINT>(count);ti.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&ti,&pre);
            s.tlas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            s.tlasScratch=buffer(d,pre.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            s.instances=buffer(d,count*sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            if(!s.context.executeSingleCommandListAndWait([&](void* raw) {
                ComPtr<ID3D12GraphicsCommandList4> cmd;check(static_cast<ID3D12GraphicsCommandList*>(raw)->QueryInterface(IID_PPV_ARGS(&cmd)));
                for(size_t i=0;i<count;++i) {D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=inputs[i];build.ScratchAccelerationStructureData=s.scratch->GetGPUVirtualAddress();build.DestAccelerationStructureData=s.parts[i].blas->GetGPUVirtualAddress();cmd->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(cmd.Get(),s.scratch.Get());uav(cmd.Get(),s.parts[i].blas.Get());}
            },error))return false;
        }
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
        check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)));s.depth=std::move(depth);d->CreateDepthStencilView(s.depth.Get(),nullptr,s.dsv->GetCPUDescriptorHandleForHeapStart());s.w=width;s.h=height;return true;
    }catch(...) {return false;}
}
void TrilogyMeshRenderer::retire(uint32_t slot) {
    auto& s=*impl_;if(!s.pending[slot])return;void* ptr=nullptr;D3D12_RANGE rr{0,8};
    if(SUCCEEDED(s.readbacks[slot]->Map(0,&rr,&ptr))) {s.hits=*static_cast<uint32_t*>(ptr);s.queries=static_cast<uint32_t*>(ptr)[1];s.verified=s.hits>0;D3D12_RANGE wr{0,0};s.readbacks[slot]->Unmap(0,&wr);}s.pending[slot]=false;
}
bool TrilogyMeshRenderer::record(void* raw,const FrameContext& f,const M1CameraState& camera,void* hdrRaw,void* distRaw,uint64_t tensor,uint32_t width,uint32_t height,uint32_t slot,bool raytracing,bool canonical) {
    auto& s=*impl_;auto* cmd=static_cast<ID3D12GraphicsCommandList*>(raw);auto* hdr=static_cast<ID3D12Resource*>(hdrRaw);auto* dist=static_cast<ID3D12Resource*>(distRaw);auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());
    if(canonical)retire(slot);
    const bool useRt=raytracing&&s.rtReady;
    if(!useRt&&canonical)s.verified=false;
    const bool isM2=(s.visualMode==ShowVisualMode::M2Trilogy180s);
    const float filmTime=float(double(f.samplePosition)/f.sampleRate);
    const uint32_t act=isM2
        ? static_cast<uint32_t>(std::clamp<int64_t>(f.samplePosition/2880000,0,2))
        : ((s.visualMode==ShowVisualMode::FullFilm360s&&filmTime>=245.f&&filmTime<275.f)?1u:2u);
    std::vector<DrawConstants> draws;std::vector<D3D12_RAYTRACING_INSTANCE_DESC> inst;
    for(const auto& part:s.parts) {
        auto draw=s.transform(part,f,camera);draws.push_back(draw);
        const bool active=isM2?(part.kit==act||(act==2&&part.kit>=2)):s.isPartActiveInShow(part,filmTime);
        const bool thinGlass=(part.sub.material==3||part.sub.material==4||part.sub.material==18||part.sub.material==21||part.sub.material==35||part.sub.material==40);
        D3D12_RAYTRACING_INSTANCE_DESC id{};std::memcpy(id.Transform,draw.rows,48);id.InstanceID=part.sub.firstIndex;id.InstanceMask=active?(thinGlass?2u:1u):0u;id.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;if(part.blas)id.AccelerationStructure=part.blas->GetGPUVirtualAddress();inst.push_back(id);
    }
    if(useRt&&canonical) {
        const size_t offset=slot*inst.size()*sizeof(inst[0]);upload(s.instances.Get(),inst.data(),inst.size()*sizeof(inst[0]),offset);
        ComPtr<ID3D12GraphicsCommandList4> c4;check(cmd->QueryInterface(IID_PPV_ARGS(&c4)));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.NumDescs=static_cast<UINT>(inst.size());build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.InstanceDescs=s.instances->GetGPUVirtualAddress()+offset;build.DestAccelerationStructureData=s.tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=s.tlasScratch->GetGPUVirtualAddress();uav(cmd,s.tlas.Get());uav(cmd,s.tlasScratch.Get());c4->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(cmd,s.tlas.Get());
    }
    cmd->CopyBufferRegion(s.counters[slot].Get(),0,s.zero.Get(),0,8);transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,hdr,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);transition(cmd,dist,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto handle=s.rtv->GetCPUDescriptorHandleForHeapStart();d->CreateRenderTargetView(hdr,nullptr,handle);auto second=handle;second.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);d->CreateRenderTargetView(dist,nullptr,second);
    auto depthHandle=s.dsv->GetCPUDescriptorHandleForHeapStart();cmd->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);cmd->OMSetRenderTargets(2,&handle,TRUE,&depthHandle);
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
    (void)tensor;
    ID3D12DescriptorHeap* m2Heaps[] = {s.srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, m2Heaps);
    cmd->SetGraphicsRootSignature(s.root.Get());cmd->SetGraphicsRoot32BitConstants(0,28,&c,0);cmd->SetGraphicsRootShaderResourceView(2,s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(3,s.ib->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(4,s.tlas?s.tlas->GetGPUVirtualAddress():s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootDescriptorTable(5,s.srvHeap->GetGPUDescriptorHandleForHeapStart());cmd->SetGraphicsRootUnorderedAccessView(6,s.counters[slot]->GetGPUVirtualAddress());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetPipelineState(s.sky.Get());cmd->DrawInstanced(3,1,0,0);
    // Hardware Z-Prepass eliminates all hidden-surface DXR ray-query & cutout overdraw across overlapping kits
    cmd->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
    cmd->SetPipelineState(s.depthOpaque.Get());
    bool hasActiveCutout=false;
    for(size_t i=0;i<s.parts.size();++i) {
        if(!inst[i].InstanceMask) continue;
        if(isCutoutMaterial(s.parts[i].sub.material)) { hasActiveCutout=true; continue; }
        cmd->SetGraphicsRoot32BitConstants(1,16,&draws[i],0);
        cmd->DrawInstanced(s.parts[i].sub.indexCount,1,0,0);
    }
    if(hasActiveCutout) {
        cmd->SetPipelineState(s.depthCutout.Get());
        for(size_t i=0;i<s.parts.size();++i) {
            if(inst[i].InstanceMask&&isCutoutMaterial(s.parts[i].sub.material)) {
                cmd->SetGraphicsRoot32BitConstants(1,16,&draws[i],0);
                cmd->DrawInstanced(s.parts[i].sub.indexCount,1,0,0);
            }
        }
    }
    cmd->OMSetRenderTargets(2,&handle,TRUE,&depthHandle);
    cmd->SetPipelineState(useRt?s.rt.Get():s.raster.Get());
    for(size_t i=0;i<s.parts.size();++i) {
        if(!inst[i].InstanceMask||isCutoutMaterial(s.parts[i].sub.material)) continue;
        cmd->SetGraphicsRoot32BitConstants(1,16,&draws[i],0);
        cmd->DrawInstanced(s.parts[i].sub.indexCount,1,0,0);
    }
    if(hasActiveCutout) {
        cmd->SetPipelineState(useRt?s.rtCutout.Get():s.rasterCutout.Get());
        for(size_t i=0;i<s.parts.size();++i) {
            if(inst[i].InstanceMask&&isCutoutMaterial(s.parts[i].sub.material)) {
                cmd->SetGraphicsRoot32BitConstants(1,16,&draws[i],0);
                cmd->DrawInstanced(s.parts[i].sub.indexCount,1,0,0);
            }
        }
    }
    transition(cmd,hdr,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);transition(cmd,dist,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(canonical&&useRt) {cmd->CopyBufferRegion(s.readbacks[slot].Get(),0,s.counters[slot].Get(),0,8);s.pending[slot]=true;}
    transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    return useRt;
}
bool TrilogyMeshRenderer::hardwareVerified() const {return impl_->verified;}
uint32_t TrilogyMeshRenderer::queriedRays() const {return impl_->queries;}
uint32_t TrilogyMeshRenderer::verifiedHits() const {return impl_->hits;}
uint32_t TrilogyMeshRenderer::fallbackKits() const {return impl_->fallbacks;}
}
