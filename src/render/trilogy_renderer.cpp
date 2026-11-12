#include "boo/trilogy_renderer.hpp"
#include "boo/bmesh.hpp"
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
struct SceneConstants {float eye[3];float fov;float right[3];float aspect;float up[3];float time;float forward[3];float reveal;uint32_t width,height,act,canonical;float day,pulse;uint32_t showFlags;float moodAccent;};
void normalize(float v[3]) {float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);for(int c=0;c<3;++c)v[c]/=std::max(l,1e-6f);}
void cross(const float a[3],const float b[3],float r[3]) {r[0]=a[1]*b[2]-a[2]*b[1];r[1]=a[2]*b[0]-a[0]*b[2];r[2]=a[0]*b[1]-a[1]*b[0];}
}
struct TrilogyMeshRenderer::Impl {
    D3D12Context& context;
    ShowVisualMode visualMode=ShowVisualMode::M2Trilogy180s;
    struct Part {BmeshSubmesh sub;uint32_t kit;ComPtr<ID3D12Resource> blas;};
    std::vector<Part> parts;
    ComPtr<ID3D12Resource> vb,ib,scratch,tlas,tlasScratch,instances,depth;
    ComPtr<ID3D12DescriptorHeap> rtv,dsv;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> raster,rt,sky;
    std::array<ComPtr<ID3D12Resource>,4> counters,readbacks;
    ComPtr<ID3D12Resource> zero;
    std::array<bool,4> pending{};
    uint32_t w=0,h=0,hits=0,queries=0,fallbacks=0;
    bool verified=false,rtReady=false;
    explicit Impl(D3D12Context& c):context(c){}
    bool isPartActiveInShow(const Part& p,float time) const noexcept {
        const uint32_t pt=p.sub.part;
        if(visualMode==ShowVisualMode::SoloRT) {
            return p.kit==2||p.kit==3||(p.kit==4&&pt>=200&&pt<=207);
        }
        if(visualMode==ShowVisualMode::SoloTensor) {
            return p.kit==2||p.kit==3||(p.kit==4&&pt>=210&&pt<=219);
        }
        if(visualMode==ShowVisualMode::SoloNPU) {
            return p.kit==2||p.kit==3||(p.kit==4&&((pt>=210&&pt<=215)||(pt>=220&&pt<=227)));
        }
        if(visualMode==ShowVisualMode::SoloCUDA) {
            return p.kit==2||p.kit==3||(p.kit==4&&pt>=210&&pt<=215);
        }
        // FullFilm360s 7-act activation
        if(time<45.f) {
            return p.kit==2||p.kit==3||(p.kit==4&&pt>=210&&pt<=215&&time>=30.5f);
        }
        if(time<100.f) {
            return p.kit==2||p.kit==3||(p.kit==4&&((pt>=200&&pt<=207)||(pt>=210&&pt<=215)));
        }
        if(time<150.f) {
            return p.kit==2||p.kit==3||(p.kit==4&&pt>=210&&pt<=219);
        }
        if(time<195.f) {
            return p.kit==2||p.kit==3||(p.kit==4&&((pt>=210&&pt<=215)||(pt>=220&&pt<=227)));
        }
        if(time<245.f) {
            return p.kit==2||p.kit==3||(p.kit==4&&pt>=210&&pt<=215);
        }
        if(time<275.f) {
            return p.kit==1; // Act 5 Signal Hijack over Shenzhen Bay eVTOL
        }
        if(time<338.5f) {
            return p.kit==2||p.kit==3||(p.kit==4&&((pt>=200&&pt<=207)||(pt>=210&&pt<=223)||(time>=335.5f&&pt>=230&&pt<=235)));
        }
        return p.kit==2||p.kit==3||(p.kit==4&&((pt>=210&&pt<=215)||(pt>=230&&pt<=235)));
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
        if(p.kit==2) {scale=.080f;x=18.5f;z=60.0f;}
        if(p.kit==3) {x=0;y=5.85f;z=5.5f;scale=1.05f;yaw=-.10f;if(p.sub.part>=10&&p.sub.part<=16)scale*=.35f+.65f*cam.revealOpen01;}
        if(p.kit==4) {
            const uint32_t pt=p.sub.part;
            if(pt>=200&&pt<=207) {
                yaw=0.055f*std::sin(time*0.45f+float(pt-200)*0.785398f);
                if(visualMode==ShowVisualMode::FullFilm360s&&time>=335.5f) {
                    y=-6.2f*smoother((time-335.5f)/3.0f);
                }
            } else if(pt>=210&&pt<=215) {
                if(visualMode==ShowVisualMode::FullFilm360s&&time<45.f) {
                    y=-4.5f*(1.f-smoother((time-30.5f)/7.5f));
                }
            } else if(pt>=216&&pt<=219) {
                y=0.14f*std::sin(time*0.85f+float(pt-216)*1.15f);
                if(visualMode==ShowVisualMode::FullFilm360s&&time>=335.5f) {
                    y+=5.5f*smoother((time-335.5f)/3.0f);
                }
            } else if(pt>=220&&pt<=223) {
                const float iris=std::clamp(f.moodIrisAngle,0.f,1.f);
                const float dir=(pt&1u)?-1.f:1.f;
                x=0.f; y=5.75f; z=5.50f;
                scale=0.88f+0.28f*iris;
                roll=dir*(time*0.32f+(iris-0.5f)*1.65f);
                yaw=0.08f*std::sin(time*0.40f+float(pt-220));
            } else if(pt>=224&&pt<=227) {
                const float localT=(visualMode==ShowVisualMode::SoloNPU)?std::fmod(time,45.f):(time-150.f);
                const float bloom=smoother((localT-20.f)/6.5f);
                y=-0.65f*(1.f-bloom);
            } else if(pt>=230&&pt<=235) {
                const float rise=smoother((time-335.5f)/6.5f);
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
bool TrilogyMeshRenderer::initialize(bool fault,std::string* error,ShowVisualMode visualMode) {
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
            Bmesh micro=buildEnamelMicrocosmKit();
            for(auto sub:micro.submeshes) {sub.firstIndex+=static_cast<uint32_t>(indices.size());sub.firstVertex+=static_cast<uint32_t>(vertices.size());s.parts.push_back({sub,4u,{}});}
            for(auto i:micro.indices)indices.push_back(i+static_cast<uint32_t>(vertices.size()));
            vertices.insert(vertices.end(),micro.vertices.begin(),micro.vertices.end());
        }
        s.vb=buffer(d,vertices.size()*sizeof(BmeshVertex),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);upload(s.vb.Get(),vertices.data(),vertices.size()*sizeof(BmeshVertex));
        s.ib=buffer(d,indices.size()*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);upload(s.ib.Get(),indices.data(),indices.size()*4);
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=3;
        D3D12_ROOT_PARAMETER params[7]{};
        for(UINT i=0;i<2;++i) {params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[i].Constants.ShaderRegister=i;params[i].Constants.Num32BitValues=i?16:24;}
        for(UINT i=0;i<3;++i) {params[2+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2+i].Descriptor.ShaderRegister=i;}
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[5].DescriptorTable={1,&range};
        params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[6].Descriptor.ShaderRegister=0;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=7;rs.pParameters=params;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> blob,err;check(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err));check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
        const auto vs=shader("m2_mesh_vs.cso"),ps=shader("m2_mesh_ps.cso"),skyvs=shader("m2_sky_vs.cso"),skyps=shader("m2_sky_ps.cso");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.VS={vs.data(),vs.size()};pd.PS={ps.data(),ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=2;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
        check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.raster)));
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};ComPtr<ID3D12Device5> d5;
        s.rtReady=!s.context.isWarp()&&SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))&&options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1&&SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5)));
        if(s.rtReady) {auto rps=shader("m2_mesh_rt_ps.cso");pd.PS={rps.data(),rps.size()};check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.rt)));}
        pd.VS={skyvs.data(),skyvs.size()};pd.PS={skyps.data(),skyps.size()};pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&s.sky)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.rtv)));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.dsv)));
        s.zero=buffer(d,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);uint64_t zero=0;upload(s.zero.Get(),&zero,8);
        for(uint32_t i=0;i<4;++i) {s.counters[i]=buffer(d,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);s.readbacks[i]=buffer(d,256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);}
        if(s.rtReady) {
            const size_t count=s.parts.size();std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(count);std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> inputs(count);uint64_t scratchBytes=256;
            for(size_t i=0;i<count;++i) {
                auto& g=geometries[i];g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
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
        BOO_LOG_INFO("M2Mesh","Loaded "+std::to_string(vertices.size())+" vertices / "+std::to_string(indices.size()/3)+" triangles; fallback kits="+std::to_string(s.fallbacks));
        return true;
    }catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool TrilogyMeshRenderer::resize(uint32_t width,uint32_t height) {
    auto& s=*impl_;if(s.w==width&&s.h==height)return true;s.context.waitForGpu();
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
        const bool thinGlass=(part.sub.material==3||part.sub.material==4||part.sub.material==18||part.sub.material==21);
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
    }
    c.pulse=camera.emissivePulse;
    cmd->SetGraphicsRootSignature(s.root.Get());cmd->SetGraphicsRoot32BitConstants(0,24,&c,0);cmd->SetGraphicsRootShaderResourceView(2,s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(3,s.ib->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(4,s.tlas?s.tlas->GetGPUVirtualAddress():s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootDescriptorTable(5,D3D12_GPU_DESCRIPTOR_HANDLE{tensor});cmd->SetGraphicsRootUnorderedAccessView(6,s.counters[slot]->GetGPUVirtualAddress());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetPipelineState(s.sky.Get());cmd->DrawInstanced(3,1,0,0);cmd->SetPipelineState(useRt?s.rt.Get():s.raster.Get());
    for(size_t i=0;i<s.parts.size();++i)if(inst[i].InstanceMask) {cmd->SetGraphicsRoot32BitConstants(1,16,&draws[i],0);cmd->DrawInstanced(s.parts[i].sub.indexCount,1,0,0);}
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
