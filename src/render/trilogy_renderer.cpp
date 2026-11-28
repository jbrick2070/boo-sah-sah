#include "boo/trilogy_renderer.hpp"
#include "boo/startup_progress.hpp"
#include "boo/pipeline_compiler.hpp"
#include "boo/audio_reactive.hpp"
#include "boo/bmesh.hpp"
#include "boo/city_modules.hpp"
#include "boo/ui_text.hpp"
#include "boo/btex.hpp"
#include "bird_flight.hpp"
#include "boo/wind_field.hpp"
#include "boo/baked_physics.hpp"
#include "boo/canopy_cloth.hpp"
#include "boo/last_light.hpp"
#include "boo/wind_banner.hpp"
#include "boo/nrd_bridge.hpp"
#include "boo/logging.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <execution>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>
#include <tuple>
#include <stdexcept>
#include <source_location>
namespace boo {
using Microsoft::WRL::ComPtr;
namespace {
// The failing line is in the message: a start-up that drops to the fallback picture must say why.
void check(HRESULT hr,std::source_location at=std::source_location::current()) {
    if(FAILED(hr))throw std::runtime_error("M2 D3D12 resource/pipeline failure: "+std::to_string(static_cast<uint32_t>(hr))+
                                           " at trilogy_renderer.cpp:"+std::to_string(at.line()));
}
std::vector<uint8_t> shader(const char* name) {
    startupReading(resolveResourcePath(std::filesystem::path("assets/shaders")/name));
    std::ifstream f(resolveResourcePath(std::filesystem::path("assets/shaders")/name),std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error(std::string("Missing M2 shader: ")+name);
    std::vector<uint8_t> data(static_cast<size_t>(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
    startupAssetLoaded(resolveResourcePath(std::filesystem::path("assets/shaders")/name));
    return data;
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
// Row-major affine transforms in the same convention as D3D12_RAYTRACING_INSTANCE_DESC.
// Keep the layout in sync with InstanceData in m2_mesh.hlsl.
struct alignas(16) InstanceData {
    float world[12];
    float worldToObject[12];
    uint32_t meshId, materialSlice, buildingSeed, flags;
};
static_assert(sizeof(InstanceData) == 112 && offsetof(InstanceData, meshId) == 96);

void composeAffine(const float* outer, const float* inner, float* result) noexcept {
    for (int row = 0; row < 3; ++row) {
        const float* a = outer + 4 * row;
        for (int col = 0; col < 4; ++col) {
            result[4 * row + col] = a[0]*inner[col] + a[1]*inner[4+col] + a[2]*inner[8+col];
        }
        result[4 * row + 3] += a[3];
    }
}

using BirdTransforms = std::array<std::array<float,12>,5>;

BirdTransforms articulateCityBird(const BirdFlightPose& pose) noexcept {
    BirdTransforms transforms{};
    writeModelTransform(transforms[0].data(), pose.center.data(), pose.yaw, pose.pitch, pose.roll, pose.scale);
    auto hinge = [](const auto& parent, float x, float angle, auto& child) {
        const float c = std::cos(angle), s = std::sin(angle);
        // T(pivot) * Rz(angle) * T(-pivot); tips inherit the shoulder rotation.
        const float local[12] = {c,-s,0.f,x*(1.f-c), s,c,0.f,-x*s, 0.f,0.f,1.f,0.f};
        composeAffine(parent.data(),local,child.data());
    };
    for (unsigned side = 0; side < 2; ++side) {
        const float sign = side ? 1.f : -1.f;
        hinge(transforms[0],sign*kBirdShoulderX,sign*pose.wingLift,transforms[1+side]);
        hinge(transforms[1+side],sign*kBirdWristX,sign*pose.wingFlex,transforms[3+side]);
    }
    return transforms;
}

// Model-space foot of the authored Phalaenopsis stem. Keep this point fixed in
// the terrace collar (or the miniature vase) as the flower leans into a breeze.
constexpr float kOrchidRoot[3]{0.03f, -2.68f, 0.34f};
constexpr uint32_t kPlanterOrchids = 2;
constexpr uint32_t kMaxDriftingPetals = 18;

float cityFlightMotion(const FrameContext& f) {
    return std::clamp(f.motionScale,0.f,f.reducedFlash?.45f:1.f);
}

void poseOrchid(DrawConstants& draw, uint32_t part, float reveal, float time,
                float movement, float phase, float rotorWash = 0.f) noexcept {
    const float wind = terraceBreeze(time, phase); // the shared wind field (boo/wind_field.hpp)
    const float nod = std::sin(time*.62f+.65f+phase);
    const float origin[3]{};
    float stem[12];
    writeModelTransform(stem, origin, 0.f, movement*(.043f*nod-.13f*rotorWash),
                        movement*(.07f*wind+.28f*rotorWash), 1.f);
    for (int row=0; row<3; ++row) {
        const float* axis = stem+4*row;
        stem[4*row+3] = kOrchidRoot[row] -
            (axis[0]*kOrchidRoot[0] + axis[1]*kOrchidRoot[1] + axis[2]*kOrchidRoot[2]);
    }
    float rooted[12];
    composeAffine(draw.rows, stem, rooted);
    if (part < 10 || part > 16) {
        std::copy_n(rooted, 12, draw.rows);
        return;
    }
    // These parts were sculpted around the flower's throat. Opposite
    // wings cup toward the viewer together, with a small offset in timing.
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
    if (part <= 14) {
        const float wingPhase = float(part-10)*.58f + phase;
        const float wave = std::sin(time*.97f+wingPhase) + .28f*std::sin(time*1.61f+wingPhase);
        const float side = (part==11 || part==13) ? -1.f : 1.f;
        yaw = part==10 ? 0.f : side*.13f*wave*movement;
        pitch = (part==10 ? .09f : .036f)*wave*movement;
        roll = part>=13 ? side*.065f*wave*movement : 0.f;
    }
    float blade[12];
    writeModelTransform(blade, origin, yaw, pitch, roll, .35f+.65f*reveal);
    composeAffine(rooted, blade, draw.rows);
}

DrawConstants driftingPetal(uint32_t index, float time, float movement, float visibility) noexcept {
    // Stagger launch, fall and tumble. Shrink at both ends so the reset is invisible.
    const float seed = float(index);
    const float clock = time*(.45f+.55f*movement);
    const float cycles = clock*(.075f+.004f*float(index%5u)) + seed*.618034f;
    const float fall = cycles-std::floor(cycles);
    const float angle = seed*2.39996f + fall*(1.35f+.65f*movement);
    const float radius = 1.1f + fall*(1.1f+.25f*float(index%4u));
    const float position[3] = {
        radius*std::cos(angle) + .17f*std::sin(12.f*fall+seed),
        6.65f - 3.7f*fall + .17f*std::sin(8.f*fall+seed),
        5.55f + radius*.65f*std::sin(angle)
    };
    const float envelope = std::clamp(std::min(fall,1.f-fall)*9.f,0.f,1.f);
    const float scale = (.074f+.014f*float(index%4u))*visibility*envelope*envelope*(3.f-2.f*envelope);
    DrawConstants draw{};
    writeModelTransform(draw.rows, position, angle+fall*4.f, .65f*std::sin(9.f*fall+seed),
                        seed+.85f*std::sin(11.f*fall+seed), std::max(.0001f,scale));
    draw.mat = index%3u==0 ? 4u : 9u; // Celadon and ruby from the parent blossom.
    draw.part = 14u;
    return draw;
}

// False when the transform cannot be inverted (out is then not usable).
bool fillInstanceData(const DrawConstants& dc, uint32_t meshId, uint32_t seed, InstanceData& out) noexcept {
    out = InstanceData{};
    std::copy_n(dc.rows, 12, out.world);
    const float* m = dc.rows;
    float* inv = out.worldToObject;
    inv[0] = m[5]*m[10] - m[6]*m[9]; inv[1] = m[2]*m[9] - m[1]*m[10]; inv[2] = m[1]*m[6] - m[2]*m[5];
    inv[4] = m[6]*m[8] - m[4]*m[10]; inv[5] = m[0]*m[10] - m[2]*m[8]; inv[6] = m[2]*m[4] - m[0]*m[6];
    inv[8] = m[4]*m[9] - m[5]*m[8]; inv[9] = m[1]*m[8] - m[0]*m[9]; inv[10] = m[0]*m[5] - m[1]*m[4];
    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8];
    // Optional LOD details can be tiny but well conditioned near their fade endpoint.
    const float cofactorMagnitude = std::abs(m[0]*inv[0]) + std::abs(m[1]*inv[4]) + std::abs(m[2]*inv[8]);
    if (!std::isfinite(det) || std::abs(det) <= 1e-6f * cofactorMagnitude) return false;
    for (int row = 0; row < 3; ++row) {
        float* r = inv + 4*row;
        for (int col = 0; col < 3; ++col) r[col] /= det;
        r[3] = -(r[0]*m[3] + r[1]*m[7] + r[2]*m[11]);
    }
    out.meshId = meshId;
    out.materialSlice = dc.mat;
    out.buildingSeed = seed;
    out.flags = dc.part;
    return true;
}
InstanceData makeInstanceData(const DrawConstants& dc, uint32_t meshId, uint32_t seed) {
    InstanceData out;
    if (!fillInstanceData(dc, meshId, seed, out)) throw std::runtime_error("Singular city instance transform");
    return out;
}
// Persistently map an upload buffer. The pointer stays valid for the life of the resource.
uint8_t* mapForWriting(ID3D12Resource* resource) {
    void* p = nullptr; D3D12_RANGE noRead{0,0};
    check(resource->Map(0,&noRead,&p));
    return static_cast<uint8_t*>(p);
}
// The workout flock starts with room for one flock unit and grows when the dial asks for more.
constexpr uint32_t kInitialWorkoutBirds = kFlockUnit;
constexpr uint32_t kInitialRasterBirds = 64;
static_assert(kInitialRasterBirds >= kLastLightKites);
constexpr uint32_t kMaxBirdParts = 8;
constexpr uint32_t kFlockChunk = 256; // birds one task poses and writes
// Taipei 101's tuned mass damper, split out of the tower at load time so it can swing on its baked
// PhysX pendulum. The committed tower mesh is untouched (its CRC still selects the basin relief).
constexpr uint32_t kTaipeiDamperPart = 178;
constexpr float kDamperCentre[3] = {0.f, 409.5f, 0.f};  // kit-local, Y up (tools/gen_trilogy_meshes.py: (0,0,409.5) Z up)
constexpr float kDamperPivotY = 426.8f;                  // where its suspension rods meet the tower

// Moves the damper ball and its plate ridges (material 8 triangles of part 0 within its sphere) into
// their own submesh with part kTaipeiDamperPart. The cables and bumpers stay with the tower.
uint32_t splitTaipeiDamper(Bmesh& mesh) {
    const size_t subCount = mesh.submeshes.size();
    uint32_t moved = 0;
    for (size_t si = 0; si < subCount; ++si) {
        BmeshSubmesh sub = mesh.submeshes[si];
        if (sub.part != 0u || sub.material != 8u) continue;
        auto inside = [&](uint32_t index) {
            const float* p = mesh.vertices[index].positionMaterial;
            const float dx = p[0] - kDamperCentre[0], dz = p[2] - kDamperCentre[2];
            return std::abs(p[1] - kDamperCentre[1]) <= 3.1f && dx * dx + dz * dz <= 3.0f * 3.0f;
        };
        std::vector<uint32_t> keep, damper;
        keep.reserve(sub.indexCount);
        for (uint32_t t = 0; t < sub.indexCount; t += 3) {
            const uint32_t* tri = &mesh.indices[sub.firstIndex + t];
            auto& to = (inside(tri[0]) && inside(tri[1]) && inside(tri[2])) ? damper : keep;
            to.insert(to.end(), tri, tri + 3);
        }
        if (damper.empty()) continue;
        // The tower keeps the rest of the submesh (its unused tail of index slots is simply not drawn).
        std::copy(keep.begin(), keep.end(), mesh.indices.begin() + sub.firstIndex);
        mesh.submeshes[si].indexCount = static_cast<uint32_t>(keep.size());
        // The damper gets copies of its vertices, tagged with its own part, and its own submesh.
        BmeshSubmesh own = sub;
        own.part = kTaipeiDamperPart;
        own.firstVertex = static_cast<uint32_t>(mesh.vertices.size());
        own.firstIndex = static_cast<uint32_t>(mesh.indices.size());
        std::unordered_map<uint32_t, uint32_t> remap;
        for (uint32_t index : damper) {
            auto [it, fresh] = remap.try_emplace(index, static_cast<uint32_t>(mesh.vertices.size()));
            if (fresh) {
                BmeshVertex v = mesh.vertices[index];
                v.normalPart[3] = float(kTaipeiDamperPart);
                mesh.vertices.push_back(v);
            }
            mesh.indices.push_back(it->second);
        }
        own.vertexCount = static_cast<uint32_t>(mesh.vertices.size()) - own.firstVertex;
        own.indexCount = static_cast<uint32_t>(damper.size());
        mesh.submeshes.push_back(own);
        moved += own.indexCount / 3u;
    }
    return moved;
}
#if defined(BOO_HAS_PHYSX)
constexpr uint32_t kMaxPhysxBodies = 24576; // the rigid genre's bodies; reserved only in builds with PhysX
#else
constexpr uint32_t kMaxPhysxBodies = 0;
#endif
struct SceneConstants {float eye[3];float fov;float right[3];float aspect;float up[3];float time;float forward[3];float reveal;uint32_t width,height,act,canonical;float day,pulse;uint32_t showFlags;float moodAccent;float moodBloom,moodLight,moodCut,pad;float sunDir[3];float sunScale;uint32_t accumFrame,interFlags;float jitter[2];std::array<float,4> courierLamp;};
using MotionRows = std::array<float,12>;
MotionRows cameraClipRows(const SceneConstants& c) noexcept {
    MotionRows rows{};
    const float fy=1.f/std::tan(c.fov*0.5f);
    for (size_t axis=0; axis<3; ++axis) {
        rows[axis]=c.right[axis]*fy/c.aspect;
        rows[4+axis]=c.up[axis]*fy;
        rows[8+axis]=c.forward[axis];
    }
    for (size_t row=0; row<3; ++row)
        for (size_t axis=0; axis<3; ++axis) rows[4*row+3]-=rows[4*row+axis]*c.eye[axis];
    return rows;
}
    constexpr UINT kSceneDwords = sizeof(SceneConstants) / sizeof(uint32_t);
static_assert(kSceneDwords == 40, "Must match m2_mesh.hlsl cbuffer Scene");
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

    // Column-major un-jittered LH D3D projection matching VSMain o.pos.
    const float fy = 1.0f / std::tan(std::max(fovY * 0.5f, 0.05f));
    constexpr float zNear = 0.1f;
    constexpr float zFar = 3200.0f;
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
    struct Part {
        BmeshSubmesh sub;
        uint32_t kit;
        ComPtr<ID3D12Resource> blas;
        uint32_t meshId=0;
        D3D12_GPU_VIRTUAL_ADDRESS compactBlas=0;
        std::array<float,3> detailCenter{};
    };
    std::vector<Part> parts;
    std::array<std::vector<Part>,3> birdTemplates;
    struct Chunk {
        uint32_t kit = 0;
        std::array<float,3> center{};
        float radius = 0;
        std::vector<uint32_t> placements;
    };
    struct SelectedPlacement {
        uint32_t id;
        std::array<float,3> detailScale;
    };
    struct ModuleSelection {
        std::vector<SelectedPlacement> visible;
        std::vector<SelectedPlacement> rayOnly;
    };
    struct DrawBatch { uint32_t first, count, indexCount, meshId; bool cutout; };
    CityModuleLayout city;
    std::vector<Chunk> chunks;
    std::vector<std::vector<Part>> moduleParts;
    std::vector<ModuleSelection> selectedModules;
    std::vector<InstanceData> frameInstances;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> rayInstances;
    std::vector<DrawBatch> batches, liftGlazing;
    ComPtr<ID3D12Resource> instanceBuffer, meshOffsets, compactBlasPool;
    size_t maxSceneInstances = 0;
    uint64_t blasBytes = 0, tlasBytes = 0;
    uint32_t templateBlases = 0;
    CityDrawStats lastStats{};
    std::array<Part,6> marbleTemplates{};
    size_t maxTlasInstances=0;
    ComPtr<ID3D12Resource> vb,ib,scratch,tlas,tlasScratch,instances,depth;
    ComPtr<ID3D12Resource> albedoTex,normalTex,ormtTex,envIblTex,roomTex,fallbackTensorTex;
    ComPtr<ID3D12Resource> gbufNormRough,gbufDiffRadHit,gbufSpecRadHit,gbufModViewZ,nrdMv;
    ComPtr<ID3D12Resource> nrdNormRough,nrdViewZ,nrdDiffIn,nrdSpecIn;
    ComPtr<ID3D12DescriptorHeap> rtv,dsv,srvHeap;
    ComPtr<ID3D12RootSignature> root,denoiseRoot,nrdPackRoot;
    ComPtr<ID3D12PipelineState> depthOpaque,depthCutout,raster,rasterCutout,rt,rtCutout,sky,skyRt,orbitalSky;
    ComPtr<ID3D12PipelineState> rrOpaque, rrCutout, rrSky;
    ComPtr<ID3D12Resource> rrHitDistance;
    bool rrReadable=false;
    float rrWorldToView[16]{}, rrViewToClip[16]{};
    ComPtr<ID3D12PipelineState> rasterLiftGlazing;
    ComPtr<ID3D12PipelineState> motionOpaque,motionCutout,motionSky;
    ComPtr<ID3D12Resource> dlssMotion,previousTransforms,motionCamera;
    struct PreviousInstance { MotionRows rows{}; uint64_t frame=0; };
    std::unordered_map<uint64_t, PreviousInstance> motionHistory;
    uint32_t lastBirdCount = 0;
    FlockFocus flockFocus{};
    // Workout flock. Its instances are posed on every core and written straight into the upload
    // buffers, never through append(), so the frame's vectors hold everything else.
    size_t baseSceneInstances = 0;        // everything except the workout flock
    size_t physxCapacity = 0;
    uint32_t workoutBirdCapacity = 0;     // birds the buffers have room for
    uint32_t birdParts = 0;               // mesh parts in one bird
    uint8_t* instanceMapped = nullptr;    // instanceBuffer
    uint8_t* rayMapped = nullptr;         // instances
    uint8_t* motionMapped = nullptr;      // previousTransforms
    // The A/B comparison pass uploads to a pair of its own, made the first time it runs, so a
    // session that never compares does not pay for a second copy of the flock.
    ComPtr<ID3D12Resource> compareInstanceBuffer, compareInstances;
    uint8_t* compareInstanceMapped = nullptr;
    uint8_t* compareRayMapped = nullptr;
    std::array<std::vector<MotionRows>,2> flockRows; // this frame's and last frame's transforms, bird by bird
    uint32_t flockRowsNow = 0;            // which of the two was written last
    uint32_t flockRowsBirds = 0;          // birds it holds
    uint64_t flockRowsFrame = 0;          // dlssFrame it was written on
    uint32_t flockRowsKey = ~0u;          // city, species and formation it was written for
    std::vector<std::array<float,3>> flockCenters;
    std::vector<std::array<float,6>> moduleBounds;      // near-LOD bounds of each city module (min xyz, max xyz)
    std::vector<CityCollisionBox> collisionBoxes;       // the last city's buildings, world space
    uint32_t collisionAct = UINT32_MAX;
    std::vector<uint32_t> flockChunks;
    bool flockFaultLogged = false;
    std::vector<MotionRows> previousRows;
    MotionRows previousClip{};
    M1CameraState lastDlssCamera{};
    uint64_t dlssFrame=0;
    int64_t lastDlssSample=0;
    uint32_t previousCanonicalShot=UINT32_MAX;
    bool dlssDepthReadable=false,dlssHistoryValid=false,dlssReset=true;
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
    bool highPathExecuted = false;
    bool neuralMaterialDrawn = false;
    RendererFeatureState pathTracingState = RendererFeatureState::Unavailable;
    PathTracingLevel lastPathTracingLevel = PathTracingLevel::Off;
    // Deforming meshes (docs/physx_suite_plan.md, step 5): baked PhysX cloth played back on a mesh. The
    // canopy ribbons (include/boo/canopy_cloth.hpp) and the "Last Light" banner (include/boo/wind_banner.hpp).
    // Their vertices are rewritten inside the vertex buffer each frame, so raster, ray queries and the path
    // tracer all see the same cloth; their acceleration structures come from a small ring, so a frame
    // still in flight keeps its own; and last frame's positions go to the motion-vector pass, so DLSS and
    // Ray Reconstruction track the cloth. The mesh samples a baked grid of node offsets by its texture
    // coordinates (u along, v across, each 0..1).
    struct DeformingSurface {
        static constexpr uint32_t kRing = 8;
        // What it is.
        const char* name = "";
        uint32_t kit = 0, firstPart = 0, ribbons = 0, material = 0;
        int gridU = 0, gridV = 0;
        bool rebuild = false; // rebuild the structures each frame (motion of metres); otherwise refit from rest
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
        std::array<float,3> (*restPoint)(int ribbon, float u, float v) = nullptr; // u, v in 0..1
        int gridNodes() const noexcept { return gridU * gridV; }
        int trackNodes() const noexcept { return int(ribbons) * gridNodes(); }
        // State.
        bool ready = false;
        std::vector<uint32_t> partIndices;           // its submeshes in `parts`
        uint32_t first = 0, count = 0;               // the vertex range that is rewritten
        std::vector<BmeshVertex> rest, current;
        struct Moving { uint32_t local, ribbon; float gu, gv; };
        std::vector<Moving> moving;
        std::vector<float> restNodes;                // trackNodes x xyz, the rest mid-surface
        std::vector<float> offsets, deformedNodes, restNormals, deformedNormals;
        std::vector<float> lastPositions, positions; // xyz per vertex in range
        bool haveLast = false;
        uint32_t ring = 0;
        ComPtr<ID3D12Resource> upload, previous, scratch;
        uint8_t* uploadMapped = nullptr;
        uint8_t* previousMapped = nullptr;
        // The acceleration structures are built from compact inputs of their own: each part's indices
        // rebased to its first vertex, and its positions in an upload ring. Built against the whole
        // vertex buffer, four canopy rebuilds cost over a millisecond.
        std::vector<uint32_t> localIndices;
        std::vector<uint32_t> partIndexStart, partPositionStart;
        uint32_t rayVertices = 0;
        ComPtr<ID3D12Resource> rayIndexBuffer, rayPositions;
        uint8_t* rayPositionsMapped = nullptr;
        std::vector<std::array<ComPtr<ID3D12Resource>,kRing>> blases; // per part, per ring slot
        std::vector<ComPtr<ID3D12Resource>> restBlases;  // per part: the rest shape, refitted from each frame
        uint64_t scratchStride = 0;                      // one scratch region per part, so builds overlap
        float lastOffset = 0.f;                          // widest node offset this frame, metres
        float keyRate = 0.f;                             // of the track that played this frame
    };
    DeformingSurface canopy, banner;
    const DeformingSurface* activeDeforming = nullptr; // the surface playing this frame, for the dashboard
    ComPtr<ID3D12Resource> noDeform;                 // an empty range for the motion pass

    void configureDeformingSurfaces() {
        canopy = DeformingSurface{};
        canopy.name = "canopy ribbons"; canopy.kit = 4; canopy.firstPart = uint32_t(kCanopyFirstPart);
        canopy.ribbons = uint32_t(kCanopyRibbons); canopy.material = kCanopyMaterial;
        canopy.gridU = kCanopyGridU; canopy.gridV = kCanopyGridV; canopy.rebuild = false;
        // Fast trace: the ribbons are glass that fills the frame, so their rays matter more than the build.
        canopy.buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        canopy.restPoint = [](int ribbon, float u, float v) { return canopyRibbonPoint(ribbon, u, v * 2.f - 1.f); };
        banner = DeformingSurface{};
        banner.name = "banner"; banner.kit = 2; banner.firstPart = kBannerCloth; banner.ribbons = 1; banner.material = kBannerClothMaterial;
        banner.gridU = kBannerGridU; banner.gridV = kBannerGridV;
        // The banner swings through tens of degrees: a refit from the hanging shape would degrade, so it
        // is rebuilt each frame (one small mesh).
        banner.rebuild = true;
        banner.buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        banner.restPoint = [](int, float u, float v) { return bannerRestPoint(u, v); };
    }

    // The track a surface plays at `time`, the time to sample it at, and the scale for its offsets.
    const BakedGridTrack* deformTrack(const DeformingSurface& c, float time, float motion, float& sampleTime, float& scale) const {
        if (&c == &banner) {
            sampleTime = time;
            scale = 1.f;
            const auto* g = filmBakedPhysics().findGrid(bannerTrackFor(motion));
            return g && g->nodes == uint32_t(c.trackNodes()) ? g : nullptr;
        }
        sampleTime = canopyBakeTime(time, visualMode == ShowVisualMode::SoloTensor);
        scale = motion;
        const BakedGridTrack* track = nullptr;
        for (const auto& span : kCanopySpans)
            if (const auto* g = filmBakedPhysics().findGrid(span.track); g && g->nodes == uint32_t(c.trackNodes()) &&
                                                                          sampleTime >= g->firstTime - 0.5f && sampleTime <= g->lastTime() + 0.5f)
                track = g;
        return track;
    }

    // Finds a surface's vertices and the rest shape they sample. Called with the assembled vertex array,
    // before it is uploaded.
    void prepareDeforming(DeformingSurface& c, const std::vector<BmeshVertex>& vertices, const std::vector<uint32_t>& indices) {
        const DeformingSurface config = c;
        auto off = [&](const std::string& why) {
            if (!why.empty()) BOO_LOG_WARN("Cloth", std::string(config.name) + ": " + why + "; it stays still");
            c = config;
        };
        float unusedTime = 0.f, unusedScale = 1.f;
        if (!deformTrack(c, &c == &banner ? 1.f : 120.f, 1.f, unusedTime, unusedScale) &&
            !(&c == &canopy && filmBakedPhysics().findGrid(kCanopySpans[0].track))) return off("");
        const int nodesPerRibbon = c.gridNodes();
        uint32_t lo = UINT32_MAX, hi = 0;
        std::vector<bool> seen(c.ribbons, false);
        for (uint32_t i = 0; i < parts.size(); ++i) {
            const auto& p = parts[i];
            if (p.kit != c.kit || p.sub.material != c.material || p.sub.part < c.firstPart || p.sub.part >= c.firstPart + c.ribbons || !p.sub.vertexCount) continue;
            c.partIndices.push_back(i);
            seen[p.sub.part - c.firstPart] = true;
            lo = std::min(lo, p.sub.firstVertex);
            hi = std::max(hi, p.sub.firstVertex + p.sub.vertexCount);
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) return off("its mesh was not found");
        c.first = lo;
        c.count = hi - lo;
        c.rest.assign(vertices.begin() + lo, vertices.begin() + hi);
        // The rest node at a grid point is the mean of the vertices there (both faces of the fabric),
        // which is the mid-surface.
        std::vector<double> sums(size_t(c.trackNodes()) * 3u, 0.0);
        std::vector<uint32_t> nodeHits(size_t(c.trackNodes()), 0u);
        for (uint32_t index : c.partIndices) {
            const auto& p = parts[index];
            const uint32_t ribbon = p.sub.part - c.firstPart;
            for (uint32_t v = p.sub.firstVertex; v < p.sub.firstVertex + p.sub.vertexCount; ++v) {
                const auto& vertex = vertices[v];
                const float gu = std::clamp(vertex.uvThicknessRoughness[0], 0.f, 1.f) * float(c.gridU - 1);
                const float gv = std::clamp(vertex.uvThicknessRoughness[1], 0.f, 1.f) * float(c.gridV - 1);
                c.moving.push_back({v - lo, ribbon, gu, gv});
                const float ru = std::round(gu), rv = std::round(gv);
                if (std::fabs(gu - ru) < 0.02f && std::fabs(gv - rv) < 0.02f) {
                    const size_t node = size_t(ribbon) * nodesPerRibbon + size_t(ru) * c.gridV + size_t(rv);
                    for (int axis = 0; axis < 3; ++axis) sums[node * 3 + axis] += vertex.positionMaterial[axis];
                    ++nodeHits[node];
                }
            }
        }
        if (std::find(nodeHits.begin(), nodeHits.end(), 0u) != nodeHits.end()) return off("its texture coordinates do not cover the cloth grid");
        c.restNodes.resize(sums.size());
        for (size_t n = 0; n < nodeHits.size(); ++n)
            for (int axis = 0; axis < 3; ++axis) c.restNodes[n * 3 + axis] = float(sums[n * 3 + axis] / nodeHits[n]);
        // The bake's grid is laid out with the shared rest formula. Check the mesh agrees with it and in
        // which sense its texture coordinates run.
        double best = 1e30; int bestFlip = 0;
        for (int flip = 0; flip < 4; ++flip) {
            double error = 0.0;
            for (int r = 0; r < int(c.ribbons); ++r)
                for (int i = 0; i < c.gridU; ++i)
                    for (int j = 0; j < c.gridV; ++j) {
                        const int mi = (flip & 1) ? c.gridU - 1 - i : i, mj = (flip & 2) ? c.gridV - 1 - j : j;
                        const auto f = c.restPoint(r, float(i) / float(c.gridU - 1), float(j) / float(c.gridV - 1));
                        const float* m = &c.restNodes[(size_t(r) * nodesPerRibbon + size_t(mi) * c.gridV + size_t(mj)) * 3];
                        for (int axis = 0; axis < 3; ++axis) error += double(m[axis] - f[size_t(axis)]) * double(m[axis] - f[size_t(axis)]);
                    }
            if (error < best) { best = error; bestFlip = flip; }
        }
        const double rms = std::sqrt(best / double(c.trackNodes()));
        if (rms > 0.05) return off("its mesh does not match the cloth grid (" + std::to_string(rms) + " m)");
        if (bestFlip) {
            for (auto& m : c.moving) {
                if (bestFlip & 1) m.gu = float(c.gridU - 1) - m.gu;
                if (bestFlip & 2) m.gv = float(c.gridV - 1) - m.gv;
            }
            std::vector<float> nodes(c.restNodes.size());
            for (int r = 0; r < int(c.ribbons); ++r)
                for (int i = 0; i < c.gridU; ++i)
                    for (int j = 0; j < c.gridV; ++j) {
                        const int mi = (bestFlip & 1) ? c.gridU - 1 - i : i, mj = (bestFlip & 2) ? c.gridV - 1 - j : j;
                        std::copy_n(&c.restNodes[(size_t(r) * nodesPerRibbon + size_t(mi) * c.gridV + size_t(mj)) * 3], 3,
                                    &nodes[(size_t(r) * nodesPerRibbon + size_t(i) * c.gridV + size_t(j)) * 3]);
                    }
            c.restNodes = std::move(nodes);
        }
        for (uint32_t index : c.partIndices) {
            const auto& sub = parts[index].sub;
            c.partIndexStart.push_back(static_cast<uint32_t>(c.localIndices.size()));
            c.partPositionStart.push_back(c.rayVertices);
            for (uint32_t i = 0; i < sub.indexCount; ++i) c.localIndices.push_back(indices[sub.firstIndex + i] - sub.firstVertex);
            c.rayVertices += sub.vertexCount;
        }
        c.current = c.rest;
        c.offsets.resize(c.restNodes.size());
        c.deformedNodes.resize(c.restNodes.size());
        c.restNormals.resize(c.restNodes.size());
        c.deformedNormals.resize(c.restNodes.size());
        gridNormals(c, c.restNodes, c.restNormals);
        c.positions.resize(size_t(c.count) * 3u);
        c.lastPositions.resize(size_t(c.count) * 3u);
        c.ready = true;
        BOO_LOG_INFO("Cloth", std::string(c.name) + " on baked PhysX cloth: " + std::to_string(c.moving.size()) + " vertices in a range of " +
                     std::to_string(c.count) + ", mesh matches the cloth grid to " + std::to_string(int(std::lround(rms * 1000.0))) + " mm");
    }
    static void gridNormals(const DeformingSurface& c, const std::vector<float>& nodes, std::vector<float>& normals) {
        const int perRibbon = c.gridNodes();
        for (int r = 0; r < int(c.ribbons); ++r)
            for (int i = 0; i < c.gridU; ++i)
                for (int j = 0; j < c.gridV; ++j) {
                    auto at = [&](int a, int b) {
                        return &nodes[(size_t(r) * perRibbon + size_t(std::clamp(a, 0, c.gridU - 1)) * c.gridV + size_t(std::clamp(b, 0, c.gridV - 1))) * 3];
                    };
                    const float* a = at(i + 1, j); const float* b = at(i - 1, j); const float* cc = at(i, j + 1); const float* e = at(i, j - 1);
                    const float du[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}, dv[3] = {cc[0] - e[0], cc[1] - e[1], cc[2] - e[2]};
                    float n[3] = {du[1] * dv[2] - du[2] * dv[1], du[2] * dv[0] - du[0] * dv[2], du[0] * dv[1] - du[1] * dv[0]};
                    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    float* out = &normals[(size_t(r) * perRibbon + size_t(i) * c.gridV + size_t(j)) * 3];
                    for (int axis = 0; axis < 3; ++axis) out[axis] = len > 1e-9f ? n[axis] / len : 0.f;
                }
    }
    // Rotates v by the shortest rotation taking unit vector a onto unit vector b.
    static void rotateBetween(const float a[3], const float b[3], float* v) {
        const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        if (d < -0.999f) return; // a half turn has no unique axis; cloth never folds that far between nodes
        const float k = 1.f / (1.f + d);
        const float vc[3] = {c[1] * v[2] - c[2] * v[1], c[2] * v[0] - c[0] * v[2], c[0] * v[1] - c[1] * v[0]};
        const float cvc[3] = {c[1] * vc[2] - c[2] * vc[1], c[2] * vc[0] - c[0] * vc[2], c[0] * vc[1] - c[1] * vc[0]};
        for (int axis = 0; axis < 3; ++axis) v[axis] += vc[axis] + k * cvc[axis];
    }
    // Poses a surface at `time`, copies it into the vertex buffer, rebuilds or refits its acceleration
    // structures and points its parts at them. Returns the motion pass's previous-position view.
    D3D12_GPU_VIRTUAL_ADDRESS updateDeforming(DeformingSurface& c, ID3D12GraphicsCommandList* cmd, float time, float motion, bool rebuildRays) {
        float sampleTime = time, scale = 1.f;
        const BakedGridTrack* track = deformTrack(c, time, motion, sampleTime, scale);
        if (track) track->sample(sampleTime, c.offsets.data());
        else std::fill(c.offsets.begin(), c.offsets.end(), 0.f);
        c.keyRate = track ? track->keyRate : 0.f;
        c.lastOffset = 0.f;
        for (size_t n = 0; n < c.offsets.size(); n += 3) {
            for (int axis = 0; axis < 3; ++axis) c.offsets[n + axis] *= scale;
            c.lastOffset = std::max(c.lastOffset, std::sqrt(c.offsets[n] * c.offsets[n] + c.offsets[n + 1] * c.offsets[n + 1] + c.offsets[n + 2] * c.offsets[n + 2]));
        }
        for (size_t n = 0; n < c.offsets.size(); ++n) c.deformedNodes[n] = c.restNodes[n] + c.offsets[n];
        gridNormals(c, c.deformedNodes, c.deformedNormals);
        const int perRibbon = c.gridNodes();
        for (const auto& m : c.moving) {
            const int i0 = std::min(int(m.gu), c.gridU - 2), j0 = std::min(int(m.gv), c.gridV - 2);
            const float fu = m.gu - float(i0), fv = m.gv - float(j0);
            const size_t base = size_t(m.ribbon) * perRibbon;
            const size_t n00 = (base + size_t(i0) * c.gridV + size_t(j0)) * 3, n10 = n00 + size_t(c.gridV) * 3, n01 = n00 + 3, n11 = n10 + 3;
            const float w00 = (1.f - fu) * (1.f - fv), w10 = fu * (1.f - fv), w01 = (1.f - fu) * fv, w11 = fu * fv;
            float d[3], nr[3], nd[3];
            for (int axis = 0; axis < 3; ++axis) {
                d[axis] = w00 * c.offsets[n00 + axis] + w10 * c.offsets[n10 + axis] + w01 * c.offsets[n01 + axis] + w11 * c.offsets[n11 + axis];
                nr[axis] = w00 * c.restNormals[n00 + axis] + w10 * c.restNormals[n10 + axis] + w01 * c.restNormals[n01 + axis] + w11 * c.restNormals[n11 + axis];
                nd[axis] = w00 * c.deformedNormals[n00 + axis] + w10 * c.deformedNormals[n10 + axis] + w01 * c.deformedNormals[n01 + axis] + w11 * c.deformedNormals[n11 + axis];
            }
            const float lr = std::sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]), ld = std::sqrt(nd[0] * nd[0] + nd[1] * nd[1] + nd[2] * nd[2]);
            const BmeshVertex& rest = c.rest[m.local];
            BmeshVertex& out = c.current[m.local];
            out = rest;
            for (int axis = 0; axis < 3; ++axis) out.positionMaterial[axis] = rest.positionMaterial[axis] + d[axis];
            if (lr > 1e-6f && ld > 1e-6f) {
                for (int axis = 0; axis < 3; ++axis) { nr[axis] /= lr; nd[axis] /= ld; }
                rotateBetween(nr, nd, out.normalPart);
                rotateBetween(nr, nd, out.tangentSign);
            }
        }
        for (uint32_t v = 0; v < c.count; ++v)
            std::copy_n(c.current[v].positionMaterial, 3, &c.positions[size_t(v) * 3]);
        const uint32_t slot = c.ring++ % DeformingSurface::kRing;
        const size_t bytes = size_t(c.count) * sizeof(BmeshVertex);
        // Only the fabric's own vertices move; anything between its parts keeps its loaded copy.
        for (uint32_t index : c.partIndices) {
            const auto& sub = parts[index].sub;
            std::memcpy(c.uploadMapped + slot * bytes + size_t(sub.firstVertex - c.first) * sizeof(BmeshVertex),
                        c.current.data() + (sub.firstVertex - c.first), size_t(sub.vertexCount) * sizeof(BmeshVertex));
        }
        // Last frame's positions for motion vectors, then this frame's.
        const size_t previousBytes = (size_t(c.count) + 1u) * 16u;
        uint8_t* previous = c.previousMapped + slot * previousBytes;
        const uint32_t header[4] = {c.first, c.count, 0u, 0u};
        std::memcpy(previous, header, 16);
        const auto& prior = (c.haveLast && !dlssReset) ? c.lastPositions : c.positions;
        for (uint32_t v = 0; v < c.count; ++v) {
            const float pp[4] = {prior[size_t(v) * 3], prior[size_t(v) * 3 + 1], prior[size_t(v) * 3 + 2], 1.f};
            std::memcpy(previous + 16u * (v + 1u), pp, 16);
        }
        std::swap(c.lastPositions, c.positions);
        c.haveLast = true;

        const auto srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transition(cmd, vb.Get(), srvState, D3D12_RESOURCE_STATE_COPY_DEST);
        for (uint32_t index : c.partIndices) {
            const auto& sub = parts[index].sub;
            cmd->CopyBufferRegion(vb.Get(), uint64_t(sub.firstVertex) * sizeof(BmeshVertex), c.upload.Get(),
                                  uint64_t(slot) * bytes + uint64_t(sub.firstVertex - c.first) * sizeof(BmeshVertex), uint64_t(sub.vertexCount) * sizeof(BmeshVertex));
        }
        transition(cmd, vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srvState);
        if (rebuildRays && !c.blases.empty()) {
            writeDeformRayPositions(c, slot);
            ComPtr<ID3D12GraphicsCommandList4> c4;
            check(cmd->QueryInterface(IID_PPV_ARGS(&c4)));
            uav(cmd, c.scratch.Get());
            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry(c.partIndices.size());
            for (size_t k = 0; k < c.partIndices.size(); ++k) {
                Part& part = parts[c.partIndices[k]];
                geometry[k] = deformGeometry(c, k, slot);
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                build.Inputs.Flags = c.buildFlags;
                build.Inputs.NumDescs = 1;
                build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                build.Inputs.pGeometryDescs = &geometry[k];
                if (!c.rebuild) {
                    // A refit of the rest shape's structure onto this frame's cloth: cheap, and since it
                    // always starts from the rest shape its quality never drifts.
                    build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                    build.SourceAccelerationStructureData = c.restBlases[k]->GetGPUVirtualAddress();
                }
                build.DestAccelerationStructureData = c.blases[k][slot]->GetGPUVirtualAddress();
                build.ScratchAccelerationStructureData = c.scratch->GetGPUVirtualAddress() + k * c.scratchStride;
                c4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                part.compactBlas = c.blases[k][slot]->GetGPUVirtualAddress();
            }
            std::vector<D3D12_RESOURCE_BARRIER> done(c.partIndices.size());
            for (size_t k = 0; k < done.size(); ++k) { done[k].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; done[k].UAV.pResource = c.blases[k][slot].Get(); }
            cmd->ResourceBarrier(static_cast<UINT>(done.size()), done.data());
        }
        return c.previous->GetGPUVirtualAddress() + slot * previousBytes;
    }
    // Part k's positions for ring slot `slot`, from the surface's current pose.
    void writeDeformRayPositions(DeformingSurface& c, uint32_t slot) {
        float* out = reinterpret_cast<float*>(c.rayPositionsMapped + size_t(slot) * c.rayVertices * 12u);
        for (size_t k = 0; k < c.partIndices.size(); ++k) {
            const auto& sub = parts[c.partIndices[k]].sub;
            float* dst = out + size_t(c.partPositionStart[k]) * 3u;
            for (uint32_t v = 0; v < sub.vertexCount; ++v)
                std::copy_n(c.current[sub.firstVertex - c.first + v].positionMaterial, 3, dst + size_t(v) * 3u);
        }
    }
    // Triangles in the same order as the part's own indices, so a hit's primitive index still finds
    // its vertices through the scene's index buffer.
    D3D12_RAYTRACING_GEOMETRY_DESC deformGeometry(const DeformingSurface& c, size_t k, uint32_t slot) const {
        const auto& sub = parts[c.partIndices[k]].sub;
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = isCutoutMaterial(sub.material) ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexBuffer = {c.rayPositions->GetGPUVirtualAddress() + (uint64_t(slot) * c.rayVertices + c.partPositionStart[k]) * 12u, 12u};
        g.Triangles.VertexCount = sub.vertexCount;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.IndexBuffer = c.rayIndexBuffer->GetGPUVirtualAddress() + uint64_t(c.partIndexStart[k]) * 4u;
        g.Triangles.IndexCount = sub.indexCount;
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        return g;
    }
    explicit Impl(D3D12Context& c):context(c),nrd(c){}
    void buildChunks() {
        std::map<std::tuple<uint32_t,int32_t,int32_t>, std::vector<uint32_t>> cells;
        for (uint32_t i=0; i<city.placements.size(); ++i) {
            const auto& p = city.placements[i];
            cells[{p.kit,p.chunk[0],p.chunk[1]}].push_back(i);
        }
        for (auto& [cell,members] : cells) {
            auto& chunk = chunks.emplace_back();
            chunk.kit = std::get<0>(cell);
            chunk.placements = std::move(members);
            std::array<float,3> lo, hi;
            lo.fill(std::numeric_limits<float>::max());
            hi.fill(std::numeric_limits<float>::lowest());
            for (auto idx : chunk.placements) {
                const auto& p = city.placements[idx];
                for (int a=0; a<3; ++a) {
                    lo[a] = std::min(lo[a],p.center[a]-p.radius);
                    hi[a] = std::max(hi[a],p.center[a]+p.radius);
                }
            }
            for (int a=0; a<3; ++a) chunk.center[a] = (lo[a]+hi[a])*0.5f;
            chunk.radius = std::hypot(hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2])*0.5f;
        }
    }
    void selectCity(const M1CameraState& camera, float aspect, uint32_t activeKit,
                    const DrawConstants& kit, bool trace, CityDrawStats& stats) {
        for (auto& group : selectedModules) {
            group.visible.clear();
            group.rayOnly.clear();
        }
        float forward[3], right[3], up[3];
        for (int a=0; a<3; ++a) forward[a]=camera.target[a]-camera.eye[a];
        normalize(forward); cross(camera.up,forward,right); normalize(right); cross(forward,right,up);
        const float tanY=std::tan(camera.fovYRadians*.5f), tanX=tanY*aspect;
        const float sideX=std::hypot(1.f,tanX), sideY=std::hypot(1.f,tanY);
        const float kitScale=kit.rows[0];
        auto eyeRelative = [&](const std::array<float,3>& center) {
            return std::array<float,3>{
                center[0]*kitScale+kit.rows[3]-camera.eye[0],
                center[1]*kitScale+kit.rows[7]-camera.eye[1],
                center[2]*kitScale+kit.rows[11]-camera.eye[2]
            };
        };
        auto inFrustum = [&](const std::array<float,3>& p, float r) {
            auto project = [&](const float* axis) { return p[0]*axis[0]+p[1]*axis[1]+p[2]*axis[2]; };
            const float z=project(forward);
            return z+r>=.1f && z-r<=3200.f && std::abs(project(right))<=z*tanX+r*sideX
                   && std::abs(project(up))<=z*tanY+r*sideY;
        };
        // The longest reflected/transmitted segment is 180 world units, plus a
        // secondary shadow segment. Retain nearby offscreen casters and reflectors.
        constexpr float rayReach=256.f;
        for (const auto& chunk : chunks) {
            if (chunk.kit!=activeKit) continue;
            stats.cityPlacements+=static_cast<uint32_t>(chunk.placements.size());
            auto center=eyeRelative(chunk.center);
            const float radius=chunk.radius*kitScale;
            bool directChunk=inFrustum(center,radius+.1f);
            if (!directChunk && (!trace || !inFrustum(center,radius+rayReach))) continue;
            for (auto id : chunk.placements) {
                const auto& placement=city.placements[id];
                auto offset=eyeRelative(placement.center);
                const float bounds=placement.radius*kitScale;
                const bool direct=directChunk && inFrustum(offset,bounds+.1f);
                if (!direct && (!trace || !inFrustum(offset,bounds+rayReach))) continue;
                const float distance=std::max(0.f,std::hypot(offset[0],offset[1],offset[2])-bounds);
                const auto unitsPerMeter=activeKit==2 ? kitScale : .08f;
                const auto lod=selectCityModuleLod(distance,unitsPerMeter,placement.seed);
                auto& group=selectedModules[placement.modules[lod]];
                (direct ? group.visible : group.rayOnly).push_back({id,{
                    1.f,cityDetailScale(distance,unitsPerMeter,placement.seed,1),
                    cityDetailScale(distance,unitsPerMeter,placement.seed,2)}});
                if (direct) {
                    ++stats.visibleCityPlacements;
                    ++stats.lodPlacements[lod];
                }
            }
        }
    }
    bool isPartActiveInShow(const Part& p,float time) const noexcept {
        const uint32_t pt=p.sub.part;
        const bool hero=p.kit>=4;
        // "Last Light" keeps the dusk sky clean: the blimp joins the city after the opening's 45 s cut.
        if (p.kit==2 && pt==kSkyBlimp && visualMode==ShowVisualMode::FullFilm360s && time<float(kLastLightEnd)) return false;
        // Its banner stands on the terrace only in the full film's opening.
        if (p.kit==2 && (pt==kBannerMast || pt==kBannerCloth))
            return visualMode==ShowVisualMode::FullFilm360s && time<float(kLastLightEnd);
        if (p.kit==2 && (pt==kSkyPlane || pt==kSkyDrone || pt==kSkyBlimp || (pt>=kParcelCase && pt<=kParcelDock)) &&
            visualMode==ShowVisualMode::FullFilm360s && time>=335.5f) return false;
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
        if(p.kit==2) return pt==0u||pt==3u||pt==4u||pt==6u||pt==kTaipeiDamperPart;
        return p.kit==3||(hero&&((pt>=210&&pt<=215&&time<341.8f)||(pt>=230&&pt<=249)));
    }
    // The buildings of one city as world-space boxes (see TrilogyMeshRenderer::cityCollision).
    void computeCityCollision(uint32_t forAct, const float* kitRows, std::vector<CityCollisionBox>& out) const {
        out.clear();
        for (const auto& location : city.placements) {
            // Street furniture (part 63), rooftop sets (64) and groves (65) are not buildings: birds and bodies ignore them.
            if (location.kit!=forAct || location.modules[0]>=moduleBounds.size() || (location.part>=63u && location.part<=65u)) continue;
            const auto& b=moduleBounds[location.modules[0]];
            float rows[12];
            composeAffine(kitRows,location.rows.data(),rows);
            CityCollisionBox box;
            const float c[3]={.5f*(b[0]+b[3]),.5f*(b[1]+b[4]),.5f*(b[2]+b[5])};
            float axis[3][3]; float length[3];
            for (int col=0; col<3; ++col) {
                for (int row=0; row<3; ++row) axis[col][row]=rows[row*4+col];
                length[col]=std::sqrt(axis[col][0]*axis[col][0]+axis[col][1]*axis[col][1]+axis[col][2]*axis[col][2]);
                for (int row=0; row<3; ++row) axis[col][row]/=std::max(length[col],1e-6f);
                box.half[col]=std::max(.05f,.5f*(b[col+3]-b[col])*length[col]);
            }
            for (int row=0; row<3; ++row) box.center[row]=rows[row*4]*c[0]+rows[row*4+1]*c[1]+rows[row*4+2]*c[2]+rows[row*4+3];
            // Rotation matrix (columns = axes) to quaternion.
            const float m00=axis[0][0], m11=axis[1][1], m22=axis[2][2];
            const float trace=m00+m11+m22;
            float q[4];
            if (trace>0.f) {
                const float s4=std::sqrt(trace+1.f)*2.f;
                q[3]=.25f*s4; q[0]=(axis[1][2]-axis[2][1])/s4; q[1]=(axis[2][0]-axis[0][2])/s4; q[2]=(axis[0][1]-axis[1][0])/s4;
            } else if (m00>m11 && m00>m22) {
                const float s4=std::sqrt(1.f+m00-m11-m22)*2.f;
                q[3]=(axis[1][2]-axis[2][1])/s4; q[0]=.25f*s4; q[1]=(axis[1][0]+axis[0][1])/s4; q[2]=(axis[2][0]+axis[0][2])/s4;
            } else if (m11>m22) {
                const float s4=std::sqrt(1.f+m11-m00-m22)*2.f;
                q[3]=(axis[2][0]-axis[0][2])/s4; q[0]=(axis[1][0]+axis[0][1])/s4; q[1]=.25f*s4; q[2]=(axis[2][1]+axis[1][2])/s4;
            } else {
                const float s4=std::sqrt(1.f+m22-m00-m11)*2.f;
                q[3]=(axis[0][1]-axis[1][0])/s4; q[0]=(axis[2][0]+axis[0][2])/s4; q[1]=(axis[2][1]+axis[1][2])/s4; q[2]=.25f*s4;
            }
            std::copy(q,q+4,box.rotation);
            out.push_back(box);
        }
    }
    DrawConstants transform(const Part& p,const FrameContext& f,float time,bool oppositeAirliner=false) {
        DrawConstants dc{};float scale=1,x=0,y=0,z=0,yaw=0,pitch=0,roll=0;
        auto smoother=[](float v){v=std::clamp(v,0.f,1.f);return v*v*v*(v*(v*6.f-15.f)+10.f);};
        float rotorTilt = 0.0f;
        const bool police = p.kit==0 && p.sub.part>=kShanghaiPoliceCabin && p.sub.part<=kShanghaiPoliceFan;
        if (police) {
            // Follow the open water beyond the bridge. The level, broadside
            // section reads against the sky after the egrets clear the span.
            const auto helicopter=shanghaiPolicePose(time,cityFlightMotion(f));
            x=helicopter.center[0]; y=helicopter.center[1]; z=helicopter.center[2];
            yaw=helicopter.yaw; roll=helicopter.bank;
        }
        if (p.kit == 1 && p.sub.part >= 20u && p.sub.part <= 24u) {
            // Centre of the inner track on the curved Houhai viaduct. Measure
            // each car along the rail so couplings stay together through the turn.
            constexpr float radius = 12.f - 0.67f;
            constexpr float halfPi = 1.57079632679f;
            const float routeTime = visualMode == ShowVisualMode::FullFilm360s
                ? 2.f * (time - 245.f) : time - 60.f;
            const float along = -23.f + 1.1f * std::clamp(routeTime, 0.f, 60.f)
                + 4.25f * float(p.sub.part - 20u);
            const float turn = std::clamp(along / radius, 0.f, halfPi);
            x = -19.4f - radius * std::cos(turn) + std::max(0.f, along - radius * halfPi);
            z = 27.6f + radius * std::sin(turn) + std::min(0.f, along);
            yaw = turn - halfPi;
        }
        if(p.kit==1 && p.sub.part>=100 && p.sub.part<=125) {
            const auto pose = evaluateEvtolPose(visualMode, time);
            x = pose.position[0]; y = pose.position[1]; z = pose.position[2];
            yaw = pose.yaw; pitch = pose.pitch; roll = pose.roll;
            rotorTilt = pose.rotorTilt();
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
        }
        if(p.sub.part==kSkyDrone && p.kit==2) {
            const float motion=cityFlightMotion(f);
            const auto pos=sanctuaryCourierPosition(time,motion,&yaw);
            x=pos[0]; y=pos[1]; z=pos[2]; scale=.61f;
            roll=.10f*motion;
        }
        if (p.kit==1 && p.sub.part>=kShenzhenRelay && p.sub.part<=kShenzhenRelayHose) {
            const float localTime=visualMode==ShowVisualMode::FullFilm360s ? (time-245.f)*2.f : time-60.f;
            const auto relay=relayFlyby(localTime,cityFlightMotion(f));
            x=relay.center[0]; y=relay.center[1]; z=relay.center[2];
            yaw=relay.yaw; pitch=relay.pitch; roll=relay.bank; scale=1.8f;
        }
        if(p.sub.part==kSkyBlimp && p.kit==2) {
            const float crossing=visualMode==ShowVisualMode::M2Trilogy180s ? 148.f : 13.f;
            const float phase=(time-crossing)*.019f*cityFlightMotion(f);
            const float sn=std::sin(phase), cs=std::cos(phase);
            x=26.f+30.f*sn; y=32.f+.50f*sn; z=104.f+12.f*(1.f-cs);
            // The nose points along local +X. Keep its heading tangent to the circuit.
            yaw=-std::atan2(12.f*sn,30.f*cs);
            scale=1.f;
        }
        if(p.sub.part==kSkyPlane && p.kit<3) {
            // A shallow circuit above the skyline. Using a closed path keeps
            // distant traffic continuous under scrubs and long Explore sessions.
            const bool trilogy=visualMode==ShowVisualMode::M2Trilogy180s;
            const float crossing=p.kit==2?(trilogy?148.f:13.f):(p.kit==1?(trilogy?84.f:257.f):40.f);
            const float motion=cityFlightMotion(f);
            if(oppositeAirliner) {
                // The return flight runs farther north and below the first.
                const float a=(time-crossing-4.f)*.026f*motion;
                x=-21.f-111.f*std::sin(a); z=162.f-20.f*std::cos(a);
                y=38.f+1.6f*std::sin(a);
                yaw=std::atan2(-111.f*std::cos(a),20.f*std::sin(a));
                roll=-.07f*motion;scale=.76f;
            } else {
                const float a=(time-crossing)*.031f*motion;
                x=7.f+125.f*std::sin(a); z=119.f-29.f*std::cos(a);
                y=(p.kit==2?41.f:(p.kit==1?26.f:29.f))+2.f*std::sin(a);
                yaw=std::atan2(125.f*std::cos(a),29.f*std::sin(a));
                roll=.085f*motion;scale=.90f;
            }
        }
        if (p.kit==2 && (p.sub.part==kObservatoryLiftShaft || p.sub.part==kObservatoryLiftCab)) {
            // Inside the tower's south glazing, west of the suspended damper.
            const float local=visualMode==ShowVisualMode::M2Trilogy180s ? time-120.f : time;
            const float lift=p.sub.part==kObservatoryLiftCab ? cityLiftTravel(true,local,cityFlightMotion(f)) : 0.f;
            x+=-10.9f*scale;
            y+=(396.5f+lift)*scale;
            z+=-11.6f*scale;
        }
        if (p.kit==1 && (p.sub.part==kWaterfrontLiftRails || p.sub.part==kWaterfrontLiftCab)) {
            // The twin tech towers address the bay. The cab runs up the south
            // face of the east tower, aligned to its existing 0.05 radian skew.
            const float local=visualMode==ShowVisualMode::FullFilm360s ? time-245.f : time-60.f;
            const float lift=p.sub.part==kWaterfrontLiftCab ? cityLiftTravel(false,local,cityFlightMotion(f)) : 0.f;
            scale=.80f;
            x=-7.28f; y=-.44f+lift*scale; z=57.45f; yaw=-.05f;
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
                // With the baked mechanism the director's iris value turns the vanes on their pivots
                // (below); without it, the old lean stands in for the opening.
                const bool geared=filmBakedPhysics().find("iris_vane_0")!=nullptr;
                roll=dir*(0.04f*std::sin(time*.32f)+(geared ? 0.f : (iris-.5f)*.12f));
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
        float lineLength=1.f;
        if(p.kit==2 && (p.sub.part==kBannerMast || p.sub.part==kBannerCloth)) {
            x=0.f; y=0.f; z=0.f; yaw=0.f; pitch=0.f; roll=0.f; scale=1.f; // built in world coordinates
        }
        if(p.kit==2 && p.sub.part==kParcelDock) {
            x=kParcelDockPosition[0]; y=kParcelDockPosition[1]; z=kParcelDockPosition[2]; scale=1.f;
        } else if(p.kit==2 && (p.sub.part==kParcelCase || p.sub.part==kParcelLine)) {
            const auto parcel=sanctuaryParcelPose(time,cityFlightMotion(f));
            const bool line=p.sub.part==kParcelLine;
            const auto& position=line ? parcel.lineOrigin : parcel.center;
            x=position[0]; y=position[1]; z=position[2];
            yaw=line ? 0.f : parcel.yaw;
            scale=1.f;
            if(line) lineLength=parcel.lineLength;
        }
        const float position[3] = {x, y, z};
        float rows[12];
        writeModelTransform(rows, position, yaw, pitch, roll, scale);
        rows[5]*=lineLength; // A vertical winch line has identity rotation.
        std::copy_n(rows,12,dc.rows);
        if(p.kit==2 && p.sub.part==kTaipeiDamperPart) {
            // The tuned mass damper on its baked PhysX pendulum (assets/physics/taipei_damper.bphys): a turn
            // about the point where its rods meet the tower, scaled by the motion setting.
            if (const BakedTrack* damper=filmBakedPhysics().find("taipei_damper")) {
                const BakedPose swing=damper->sample(time);
                const float motion=cityFlightMotion(f);
                float q[4]={swing.rotation[0]*motion,swing.rotation[1]*motion,swing.rotation[2]*motion,1.f+(swing.rotation[3]-1.f)*motion};
                const float n=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
                for (float& c : q) c/=std::max(n,1e-9f);
                const float qx=q[0],qy=q[1],qz=q[2],qw=q[3];
                const float r[9]={1.f-2.f*(qy*qy+qz*qz),2.f*(qx*qy-qz*qw),2.f*(qx*qz+qy*qw),
                                  2.f*(qx*qy+qz*qw),1.f-2.f*(qx*qx+qz*qz),2.f*(qy*qz-qx*qw),
                                  2.f*(qx*qz-qy*qw),2.f*(qy*qz+qx*qw),1.f-2.f*(qx*qx+qy*qy)};
                const float pv[3]={0.f,kDamperPivotY,0.f};
                float local[12];
                for (int row=0;row<3;++row) {
                    local[row*4]=r[row*3]; local[row*4+1]=r[row*3+1]; local[row*4+2]=r[row*3+2];
                    local[row*4+3]=pv[row]-(r[row*3]*pv[0]+r[row*3+1]*pv[1]+r[row*3+2]*pv[2]);
                }
                float composed[12];
                composeAffine(dc.rows,local,composed);
                std::copy_n(composed,12,dc.rows);
            }
        }
        if(p.kit>=4 && p.sub.part>=220 && p.sub.part<=223) {
            // Madame N's iris as a geared mechanism (assets/physics/iris_mechanism.bphys, baked by PhysX):
            // each vane turns about its knuckle by the angle the gear train gives for the director's
            // current iris opening.
            if (const BakedTrack* vane=filmBakedPhysics().find("iris_vane_"+std::to_string(p.sub.part-220))) {
                const BakedPose turn=vane->sample(std::clamp(f.moodIrisAngle,0.f,1.f));
                const float qx=turn.rotation[0],qy=turn.rotation[1],qz=turn.rotation[2],qw=turn.rotation[3];
                const float r[9]={1.f-2.f*(qy*qy+qz*qz),2.f*(qx*qy-qz*qw),2.f*(qx*qz+qy*qw),
                                  2.f*(qx*qy+qz*qw),1.f-2.f*(qx*qx+qz*qz),2.f*(qy*qz-qx*qw),
                                  2.f*(qx*qz-qy*qw),2.f*(qy*qz+qx*qw),1.f-2.f*(qx*qx+qy*qy)};
                const float* pv=turn.position.data();
                // T(pivot) * R * T(-pivot), in the iris's own model space.
                float local[12];
                for (int row=0;row<3;++row) {
                    local[row*4]=r[row*3]; local[row*4+1]=r[row*3+1]; local[row*4+2]=r[row*3+2];
                    local[row*4+3]=pv[row]-(r[row*3]*pv[0]+r[row*3+1]*pv[1]+r[row*3+2]*pv[2]);
                }
                float composed[12];
                composeAffine(dc.rows,local,composed);
                std::copy_n(composed,12,dc.rows);
            }
        }
        if(p.kit==2 && (p.sub.part==kParcelCase || p.sub.part==kParcelLine)) {
            // Baked PhysX swing (assets/physics/parcel_winch.bphys), scaled by the motion setting about the
            // authored pose: a pure function of film time, so seeking and replays are exact.
            if (const BakedTrack* track=filmBakedPhysics().find("parcel_case"); track && track->covers(time)) {
                const float motion=cityFlightMotion(f);
                const auto rest=sanctuaryParcelPose(time,motion);
                const auto rest1=sanctuaryParcelPose(time,1.f);
                const BakedPose baked=track->sample(time);
                auto mul=[](const float a[4],const float b[4],float out[4]) {
                    out[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
                    out[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
                    out[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
                    out[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
                };
                const float yawInv[4]={0.f,-std::sin(.5f*rest1.yaw),0.f,std::cos(.5f*rest1.yaw)};
                const float yawNow[4]={0.f,std::sin(.5f*rest.yaw),0.f,std::cos(.5f*rest.yaw)};
                float swing[4]; mul(baked.rotation.data(),yawInv,swing);           // the sway alone
                if (swing[3]<0.f) for (float& c : swing) c=-c;
                float scaled[4]; float n=0.f;
                for (int k=0;k<4;++k) { scaled[k]=(k==3?1.f:0.f)*(1.f-motion)+swing[k]*motion; n+=scaled[k]*scaled[k]; }
                n=std::sqrt(std::max(n,1e-12f));
                for (float& c : scaled) c/=n;
                float q[4]; mul(scaled,yawNow,q);
                const float centre[3]={rest.center[0]+motion*(baked.position[0]-rest1.center[0]),
                                       rest.center[1]+motion*(baked.position[1]-rest1.center[1]),
                                       rest.center[2]+motion*(baked.position[2]-rest1.center[2])};
                const float qx=q[0],qy=q[1],qz=q[2],qw=q[3];
                const float r[9]={1.f-2.f*(qy*qy+qz*qz),2.f*(qx*qy-qz*qw),2.f*(qx*qz+qy*qw),
                                  2.f*(qx*qy+qz*qw),1.f-2.f*(qx*qx+qz*qz),2.f*(qy*qz-qx*qw),
                                  2.f*(qx*qz-qy*qw),2.f*(qy*qz+qx*qw),1.f-2.f*(qx*qx+qy*qy)};
                if (p.sub.part==kParcelCase) {
                    for (int row=0;row<3;++row) {
                        dc.rows[row*4]=r[row*3]; dc.rows[row*4+1]=r[row*3+1]; dc.rows[row*4+2]=r[row*3+2]; dc.rows[row*4+3]=centre[row];
                    }
                } else {
                    // The line runs from the drone's belly to the case latch, 0.26 m above the case centre.
                    const float latch[3]={centre[0]+r[1]*.26f,centre[1]+r[4]*.26f,centre[2]+r[7]*.26f};
                    const float d[3]={latch[0]-rest.lineOrigin[0],latch[1]-rest.lineOrigin[1],latch[2]-rest.lineOrigin[2]};
                    const float length=std::max(.018f,std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]));
                    // Model -Y (0 .. -1) maps onto d: the line's Y axis is -d/|d|, scaled by its length.
                    const float yAxis[3]={-d[0]/length,-d[1]/length,-d[2]/length};
                    const float ref[3]={std::abs(yAxis[2])<.9f?0.f:1.f,0.f,std::abs(yAxis[2])<.9f?1.f:0.f};
                    float xAxis[3]={yAxis[1]*ref[2]-yAxis[2]*ref[1],yAxis[2]*ref[0]-yAxis[0]*ref[2],yAxis[0]*ref[1]-yAxis[1]*ref[0]};
                    const float xl=std::sqrt(xAxis[0]*xAxis[0]+xAxis[1]*xAxis[1]+xAxis[2]*xAxis[2]);
                    for (float& c : xAxis) c/=std::max(xl,1e-6f);
                    const float zAxis[3]={xAxis[1]*yAxis[2]-xAxis[2]*yAxis[1],xAxis[2]*yAxis[0]-xAxis[0]*yAxis[2],xAxis[0]*yAxis[1]-xAxis[1]*yAxis[0]};
                    for (int row=0;row<3;++row) {
                        dc.rows[row*4]=xAxis[row]; dc.rows[row*4+1]=yAxis[row]*length; dc.rows[row*4+2]=zAxis[row];
                        dc.rows[row*4+3]=rest.lineOrigin[row];
                    }
                }
            }
        }
        if (police && p.sub.part!=kShanghaiPoliceCabin) {
            const bool mainRotor=p.sub.part==kShanghaiPoliceRotor;
            const auto& hub=mainRotor ? kShanghaiMainHub : kShanghaiTailHub;
            const float spin=(mainRotor ? 12.7f : -19.1f)*time*cityFlightMotion(f);
            float local[12];
            writeModelTransform(local,hub.data(),mainRotor ? spin : 0.f,mainRotor ? 0.f : spin,0.f,1.f);
            composeAffine(rows,local,dc.rows);
        }
        if(p.kit==1 && p.sub.part>=110 && p.sub.part<126) {
            const uint32_t rotor=std::min<uint32_t>(5u,p.sub.part>=120?p.sub.part-120:p.sub.part-110);
            const auto& pivot = kEvtolRotorPivots[rotor];
            const float px = pivot[0], py = pivot[1], pz = pivot[2];
            const float dir=(rotor&1u)?-1.f:1.f;
            const float spin=p.sub.part>=120?(dir*time*38.f+float(rotor)*1.05f):0.f;
            // Forward cruise tilt (49 deg) articulating smoothly upright (2.3 deg) for vertical hover
            const float cs=std::cos(spin),ss=std::sin(spin),ct=std::cos(rotorTilt),st=std::sin(rotorTilt);
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
        auto next=std::make_unique<Impl>(impl_->context);
        auto& s=*next;s.visualMode=visualMode;auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());
        std::vector<BmeshVertex> vertices;std::vector<uint32_t> indices;
        auto appendMesh = [&](const Bmesh& source, uint32_t kit, std::vector<Impl::Part>& destination) {
            const auto baseVertex=static_cast<uint32_t>(vertices.size());
            for (auto sub : source.submeshes) {
                sub.firstIndex+=static_cast<uint32_t>(indices.size());
                sub.firstVertex+=baseVertex;
                destination.push_back({sub,kit,{}});
            }
            for (auto i : source.indices) indices.push_back(i+baseVertex);
            vertices.insert(vertices.end(),source.vertices.begin(),source.vertices.end());
        };
        startupStage(StartupWork::Files);
        const char* names[]={"shanghai_trestle","shenzhen_evtol","taipei_101_hero","glass_orchid_hero"};
        for(uint32_t k=0;k<4;++k) {
            Bmesh mesh;std::string why;
            if(fault||!loadBmesh(resolveResourcePath(std::string("assets/meshes/")+names[k]+".bmesh"),mesh,&why)) {
                mesh=buildTrilogyFallbackKit(k);++s.fallbacks;BOO_LOG_WARN("M2Mesh",std::string(names[k])+": procedural fallback "+why);
            }
            if ((k == 1 || k == 2) && !mesh.proceduralFallback && !fault) appendCityModules(s.city, k, mesh);
            auto appendKit=[&](const Bmesh& source) {
                appendMesh(source,k,s.parts);
            };
            if (k == 2 && mesh.sourcePayloadCrc == 0x252bd557u) {
                // This version of Xinyi has a rectangular floor and hills on its north side only.
                // Keep its district placements and swap only that backdrop for the surveyed basin.
                Bmesh relief;
                if (loadBmesh(resolveResourcePath("assets/meshes/taipei_basin_relief.bmesh"), relief, &why) &&
                    relief.submeshes.size() == 1 && relief.submeshes.front().part == 6 &&
                    relief.submeshes.front().material == 19) {
                    std::erase_if(mesh.submeshes, [](const auto& sub) {
                        return sub.part == 6 && (sub.material == 2 || sub.material == 19);
                    });
                    appendKit(relief);
                }
            }
            if (k == 2 && !mesh.proceduralFallback && !fault) {
                const uint32_t damperTriangles = splitTaipeiDamper(mesh);
                BOO_LOG_INFO("M2Mesh", "Taipei 101 tuned mass damper split out for its PhysX pendulum: " +
                                       std::to_string(damperTriangles) + " triangles");
            }
            appendKit(mesh);
            if (!mesh.proceduralFallback && !fault) {
                if (k==2) {
                    appendKit(buildCityLift(kObservatoryLiftShaft));
                    appendKit(buildCityLift(kObservatoryLiftCab));
                } else if (k==1) {
                    appendKit(buildCityLift(kWaterfrontLiftRails));
                    appendKit(buildCityLift(kWaterfrontLiftCab));
                }
            }
            if(!mesh.proceduralFallback && !fault && k<3) appendKit(buildCityAircraft(kSkyPlane));
            // The high camera cut also needs its subject with the fallback waterfront kit.
            if(k==0) appendKit(buildShanghaiPoliceHelicopter());
            if(k==1) appendKit(buildCityAircraft(kShenzhenRelay));
            if(!mesh.proceduralFallback && !fault && k==2) {
                appendKit(buildCityAircraft(kSkyDrone));
                appendKit(buildCityBlimp(cityBlimpSigns(UiStrings::instance().languageTag())));
                for (auto part : {kParcelCase,kParcelLine,kParcelDock}) appendKit(buildParcelHardware(part));
                for (auto part : {kBannerMast,kBannerCloth}) appendKit(buildWindBanner(part)); // "Last Light"
            }
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
        if (!fault) {
            appendMesh(buildCityBird(CityBirdSpecies::BlackKite),2,s.birdTemplates[0]);
            appendMesh(buildCityBird(CityBirdSpecies::Egret),1,s.birdTemplates[1]);
            appendMesh(buildCityBird(CityBirdSpecies::NightHeron),2,s.birdTemplates[2]);
        }
        for (size_t m = 0; m < kMarbleTemplateMats.size(); ++m) {
            BmeshSubmesh sph = appendSmoothSphereTemplate(vertices, indices, kMarbleTemplateMats[m], 900u + static_cast<uint32_t>(m));
            s.marbleTemplates[m] = Impl::Part{sph, 99u, {}};
        }
        s.buildChunks();
        startupCheckpoint(); // meshes are in memory
        startupStep("trilogy meshes");
        startupStage(StartupWork::Scene);
        s.moduleParts.resize(s.city.modules.size());
        s.selectedModules.resize(s.city.modules.size());
        for (size_t m=0; m<s.city.modules.size(); ++m) {
            const Bmesh& mesh = s.city.modules[m];
            s.moduleBounds.push_back({mesh.bounds[0],mesh.bounds[1],mesh.bounds[2],mesh.bounds[3],mesh.bounds[4],mesh.bounds[5]});
            for (auto sub : mesh.submeshes) {
                std::array<float,3> detailCenter{};
                if (sub.part >> 16) {
                    std::array<float,3> lo,hi;
                    lo.fill(std::numeric_limits<float>::max());
                    hi.fill(std::numeric_limits<float>::lowest());
                    for (uint32_t i=0; i<sub.vertexCount; ++i) {
                        const auto& pos=mesh.vertices[sub.firstVertex+i].positionMaterial;
                        for (int axis=0; axis<3; ++axis) {
                            lo[axis]=std::min(lo[axis],pos[axis]);
                            hi[axis]=std::max(hi[axis],pos[axis]);
                        }
                    }
                    for (int axis=0; axis<3; ++axis) detailCenter[axis]=(lo[axis]+hi[axis])*.5f;
                }
                sub.firstIndex += static_cast<uint32_t>(indices.size());
                sub.firstVertex += static_cast<uint32_t>(vertices.size());
                s.moduleParts[m].push_back({sub,0,{}});
                s.moduleParts[m].back().detailCenter=detailCenter;
            }
            const uint32_t baseVertex = static_cast<uint32_t>(vertices.size());
            for (auto index : mesh.indices) indices.push_back(baseVertex+index);
            vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        }
        s.city.modules.clear();
        // A film shot selects exactly one city. Reserve its worst-case near detail
        // across every visible and ray-only placement, including headless/A-B slots.
        std::array<size_t,3> cityCapacity{};
        for (const auto& p : s.city.placements) {
            size_t largest = 0;
            for (auto module : p.modules) largest = std::max(largest, s.moduleParts.at(module).size());
            cityCapacity.at(p.kit) += largest;
        }
        const size_t orchidParts = std::count_if(s.parts.begin(),s.parts.end(),[](const auto& p){ return p.kit==3; });
        const size_t taipeiPlaneParts = std::count_if(s.parts.begin(),s.parts.end(),[](const auto& p){return p.kit==2 && p.sub.part==kSkyPlane;});
        s.birdParts = static_cast<uint32_t>(std::max({s.birdTemplates[0].size(),s.birdTemplates[1].size(),s.birdTemplates[2].size()}));
        s.physxCapacity = s.context.rayTracingAllowed() ? kMaxPhysxBodies : 0u;
        s.baseSceneInstances = s.parts.size() + InteractiveView::kMaxMarbles
            + kPlanterOrchids*orchidParts + kMaxDriftingPetals + taipeiPlaneParts
            + size_t(kCityBirdCount + 1u) * s.birdParts + s.physxCapacity
            + *std::max_element(cityCapacity.begin(),cityCapacity.end());
        // Keep the authored opening kettle on raster, and grow on demand for workouts.
        s.workoutBirdCapacity = s.context.rayTracingAllowed() ? kInitialWorkoutBirds : kInitialRasterBirds;
        s.maxSceneInstances = s.baseSceneInstances + size_t(s.workoutBirdCapacity) * s.birdParts;
        if (s.baseSceneInstances + size_t(kMaxWorkoutBirds) * s.birdParts > (1u<<24))
            throw std::runtime_error("City instances exceed the DXR 24-bit identifier range");
        s.frameInstances.reserve(s.baseSceneInstances);
        s.rayInstances.reserve(s.baseSceneInstances);
        // Each buffered frame has independent canonical and A/B uploads; both passes can
        // be recorded before the GPU reads either. Slot 3 is the serialized headless path.
        // This buffer holds the canonical pass's four; the A/B pass has compareInstanceBuffer.
        s.instanceBuffer = buffer(d, s.maxSceneInstances*sizeof(InstanceData)*4,
                                 D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        s.instanceMapped = mapForWriting(s.instanceBuffer.Get());
        std::vector<Impl::Part*> geometriesById;
        for (auto& part : s.parts) geometriesById.push_back(&part);
        for (auto& species : s.birdTemplates)
            for (auto& part : species) geometriesById.push_back(&part);
        for (auto& part : s.marbleTemplates) geometriesById.push_back(&part);
        const size_t firstCityModule = geometriesById.size();
        for (auto& module : s.moduleParts)
            for (auto& part : module) geometriesById.push_back(&part);
        std::vector<uint32_t> firstIndices;
        for (auto* part : geometriesById) {
            part->meshId = static_cast<uint32_t>(firstIndices.size());
            firstIndices.push_back(part->sub.firstIndex);
        }
        s.meshOffsets = buffer(d, firstIndices.size()*sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(s.meshOffsets.Get(),firstIndices.data(),firstIndices.size()*sizeof(uint32_t));
        s.configureDeformingSurfaces();
        s.prepareDeforming(s.canopy, vertices, indices);
        s.prepareDeforming(s.banner, vertices, indices);
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
        startupStage(StartupWork::Files);
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
        Btex rooms;
        if (fault || !loadBtex(resolveResourcePath("assets/textures/city_rooms.btex"), rooms, &texWhy) ||
            rooms.arraySize != 30u || rooms.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
            s.usedFallbackTexture = true;
            rooms = buildFallbackAlbedoArray(30u);
            BOO_LOG_WARN("M2Tex", "Using fallback city rooms");
        }
        startupStage(StartupWork::Scene);
        s.roomTex = uploadBtexArray(d, s.context, rooms, error);
        s.albedoTex = uploadBtexArray(d, s.context, bAlb, error);
        s.normalTex = uploadBtexArray(d, s.context, bNor, error);
        s.ormtTex   = uploadBtexArray(d, s.context, bOrm, error);
        s.envIblTex = uploadBtexArray(d, s.context, bEnv, error);
        startupCheckpoint(); // textures are on the GPU
        startupStep("trilogy textures");

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

        // Five static SRVs and four 20-descriptor frame regions precede the room array.
        constexpr UINT roomIndex = 5u + 4u * 20u;
        bindArraySrv(roomIndex, s.roomTex.Get(), rooms);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 5;
        ranges[0].BaseShaderRegister = 3;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 10;
        ranges[1].OffsetInDescriptorsFromTableStart = roomIndex;
        D3D12_ROOT_PARAMETER params[12]{};
        for(UINT i=0;i<2;++i) {params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[i].Constants.ShaderRegister=i;params[i].Constants.Num32BitValues=i?5:kSceneDwords;}
        for(UINT i=0;i<3;++i) {params[2+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2+i].Descriptor.ShaderRegister=i;}
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[5].DescriptorTable={2,ranges};
        params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[6].Descriptor.ShaderRegister=0;
        params[7].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[7].Descriptor.ShaderRegister=8;
        params[8].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[8].Descriptor.ShaderRegister=9;
        params[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[9].Descriptor.ShaderRegister=11;
        params[10].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[10].Descriptor.ShaderRegister=2;
        params[11].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[11].Descriptor.ShaderRegister=12; // last frame's deformed vertices

        D3D12_STATIC_SAMPLER_DESC samplers[4]{};
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

        // Lite uses a separate trilinear wrap sampler so changing presets never
        // changes the established anisotropic sampler used by the other tiers.
        samplers[3].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[3].AddressU = samplers[3].AddressV = samplers[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[3].MipLODBias = 0.25f;
        samplers[3].ShaderRegister = 3;
        samplers[3].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=12;rs.pParameters=params;rs.NumStaticSamplers=4;rs.pStaticSamplers=samplers;
        ComPtr<ID3DBlob> blob,err;check(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err));check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
        const auto vs=shader("m2_mesh_vs.cso"),psDepthCut=shader("m2_depth_cutout_ps.cso"),ps=shader("m2_mesh_ps.cso"),psCut=shader("m2_mesh_cutout_ps.cso"),skyvs=shader("m2_sky_vs.cso"),skyps=shader("m2_sky_ps.cso");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC dp{};dp.pRootSignature=s.root.Get();dp.VS={vs.data(),vs.size()};dp.PS={nullptr,0};dp.SampleMask=UINT_MAX;
        dp.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;dp.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;dp.RasterizerState.DepthClipEnable=TRUE;
        dp.DepthStencilState.DepthEnable=TRUE;dp.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;dp.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        dp.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;dp.NumRenderTargets=0;dp.DSVFormat=DXGI_FORMAT_D32_FLOAT;dp.SampleDesc.Count=1;
        // Each compile runs off the window's thread, so the loading screen keeps painting (compileWhilePainting).
        auto gfxPso=[&](const char* name,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,ComPtr<ID3D12PipelineState>& out) {
            return static_cast<HRESULT>(compileWhilePainting(name,desc.VS.BytecodeLength+desc.PS.BytecodeLength,[&] {
                return d->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&out));
            }));
        };
        check(gfxPso("m2_mesh_vs.cso (depth)",dp,s.depthOpaque));
        dp.PS={psDepthCut.data(),psDepthCut.size()};check(gfxPso("m2_depth_cutout_ps.cso",dp,s.depthCutout));
        startupStep("trilogy depth pipelines (2)");

        // Piggyback motion on the normal Z prepass only when DLSS consumes it.
        if (!s.context.benchmark() || s.context.benchmark()->mode == BenchmarkMode::DlssQuality) try {
            const auto mvVs=shader("m2_motion_vs.cso"),mvPs=shader("m2_motion_ps.cso"),
                   mvCut=shader("m2_motion_cutout_ps.cso"),mvSky=shader("m2_sky_motion_ps.cso");
            auto md=dp;
            md.NumRenderTargets=1;
            md.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;
            md.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            md.VS={mvVs.data(),mvVs.size()};md.PS={mvPs.data(),mvPs.size()};
            check(gfxPso("m2_motion_ps.cso",md,s.motionOpaque));
            md.PS={mvCut.data(),mvCut.size()};
            check(gfxPso("m2_motion_cutout_ps.cso",md,s.motionCutout));
            md.VS={skyvs.data(),skyvs.size()};md.PS={mvSky.data(),mvSky.size()};
            md.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
            check(gfxPso("m2_sky_motion_ps.cso",md,s.motionSky));
        } catch (const std::exception& e) {
            BOO_LOG_WARN("DLSS",std::string("Motion shaders unavailable: ")+e.what());
        }
        startupStep("trilogy motion pipelines (3)");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.VS={vs.data(),vs.size()};pd.PS={ps.data(),ps.size()};
        pd.BlendState.IndependentBlendEnable=FALSE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=2;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
        check(gfxPso("m2_mesh_ps.cso",pd,s.raster));
        // Without ray transmission, layer a faint glass reflection over the
        // gallery or cab interior after opaque depth and sky are complete.
        auto pane=pd;
        pane.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pane.BlendState.IndependentBlendEnable=TRUE;
        auto& glassColor=pane.BlendState.RenderTarget[0];
        glassColor.BlendEnable=TRUE;
        glassColor.SrcBlend=D3D12_BLEND_BLEND_FACTOR;
        glassColor.DestBlend=D3D12_BLEND_INV_BLEND_FACTOR;
        glassColor.BlendOp=D3D12_BLEND_OP_ADD;
        glassColor.SrcBlendAlpha=D3D12_BLEND_ONE;
        glassColor.DestBlendAlpha=D3D12_BLEND_ZERO;
        glassColor.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        pane.BlendState.RenderTarget[1].RenderTargetWriteMask=0;
        check(gfxPso("m2_mesh_ps.cso (glazing)",pane,s.rasterLiftGlazing));
        s.liftGlazing.reserve(4);
        pd.PS={psCut.data(),psCut.size()};check(gfxPso("m2_mesh_cutout_ps.cso",pd,s.rasterCutout));
        startupStep("trilogy raster pipelines (3)");
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};ComPtr<ID3D12Device5> d5;
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{}; // the ray-traced city shaders use wave operations
        s.rtReady=!s.context.isWarp()&&s.context.rayTracingAllowed()&&SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&waves,sizeof(waves)))&&waves.WaveOps&&SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))&&options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1&&SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5)));
        // The ray-traced pipelines carry the whole city shader with its ray queries. On a cold shader cache
        // the driver spends many seconds on each, so they compile side by side on threads of their own while
        // the rest of the renderer is built, and are waited for at the end. Each ray reconstruction variant
        // differs only in one target format and follows its base pipeline on the same thread, where the
        // driver reuses the shader it has just compiled.
        // Declared before the compiler: on an early return its destructor waits for the compiles, which use these.
        std::vector<uint8_t> rps,rpsCut,skyRtPs;
        std::atomic<bool> rrSkyOk{false},rrOpaqueOk{false};
        PipelineCompiler rtCompiler;
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
            // Use the very same PSOs as ordinary viewing so a loaded film warms this worker's driver cache.
            rps = shader("m2_mesh_rt_ps.cso");
            rpsCut = shader("m2_mesh_rt_cutout_ps.cso");
            skyRtPs = shader("m2_sky_rt_ps.cso");
            // Keep the ordinary DXR pipelines usable if optional RR setup fails.
            // MRT 2..4 carry material guides; MRT 6 carries scalar reflection distance.
            auto rrFormats=[](D3D12_GRAPHICS_PIPELINE_STATE_DESC desc){desc.RTVFormats[6]=DXGI_FORMAT_R16_FLOAT;return desc;};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC opaque=pdRt;opaque.PS={rps.data(),rps.size()};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cutout=pdRt;cutout.PS={rpsCut.data(),rpsCut.size()};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC skyDesc=pdRt;skyDesc.VS={skyvs.data(),skyvs.size()};skyDesc.PS={skyRtPs.data(),skyRtPs.size()};
            skyDesc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;skyDesc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
            rtCompiler.start("M2 ray-traced city",vs.size()+rps.size(),[&s,d,opaque,rr=rrFormats(opaque),&rrOpaqueOk] {
                if(FAILED(d->CreateGraphicsPipelineState(&opaque,IID_PPV_ARGS(&s.rt))))return false;
                if (s.context.benchmark()) return true;
                rrOpaqueOk=SUCCEEDED(d->CreateGraphicsPipelineState(&rr,IID_PPV_ARGS(&s.rrOpaque)));
                return true;
            });
            rtCompiler.start("M2 ray-traced city, alpha cutout",vs.size()+rpsCut.size(),[&s,d,cutout,rr=rrFormats(cutout)] {
                if(FAILED(d->CreateGraphicsPipelineState(&cutout,IID_PPV_ARGS(&s.rtCutout))))return false;
                if (!s.context.benchmark()) d->CreateGraphicsPipelineState(&rr,IID_PPV_ARGS(&s.rrCutout));
                return true;
            });
            rtCompiler.start("M2 ray-traced sky",skyvs.size()+skyRtPs.size(),[&s,d,skyDesc,rr=rrFormats(skyDesc),&rrSkyOk] {
                if(FAILED(d->CreateGraphicsPipelineState(&skyDesc,IID_PPV_ARGS(&s.skyRt))))return false;
                if (s.context.benchmark()) return true;
                rrSkyOk=SUCCEEDED(d->CreateGraphicsPipelineState(&rr,IID_PPV_ARGS(&s.rrSky)));
                return true;
            });
        }
        pd.VS={skyvs.data(),skyvs.size()};pd.PS={skyps.data(),skyps.size()};pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;check(gfxPso("m2_sky_ps.cso",pd,s.sky));
        auto orbital=pd;
        orbital.DepthStencilState.DepthEnable=FALSE;
        orbital.DSVFormat=DXGI_FORMAT_UNKNOWN;
        check(gfxPso("m2_sky_ps.cso (orbit)",orbital,s.orbitalSky));
        startupStep("trilogy sky pipelines (2)");
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
            ad.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;check(gfxPso("m2_accum_blend_ps.cso",ad,s.accumBlend));
            bt.BlendEnable=FALSE;ad.PS={arps.data(),arps.size()};ad.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;check(gfxPso("m2_accum_resolve_ps.cso",ad,s.accumResolve));
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
            check(gfxPso("m2_denoise_resolve_ps.cso",dd,s.denoiseResolve));
            dd.PS={unpPs.data(),unpPs.size()};
            check(gfxPso("m2_nrd_unpack_ps.cso",dd,s.nrdUnpackPs));

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
            check(static_cast<HRESULT>(compileWhilePainting("m2_nrd_pack_cs.cso",packCs.size(),[&] {
                return d->CreateComputePipelineState(&cpd,IID_PPV_ARGS(&s.nrdPackCs));
            })));
        }
        startupStep("trilogy accumulation and denoise pipelines (5)");
        startupStage(StartupWork::Scene);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=10;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.rtv)));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.dsv)));
        s.zero=buffer(d,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);std::array<uint32_t, 3> zeros{};upload(s.zero.Get(), zeros.data(), sizeof(zeros));
        for(uint32_t i=0;i<4;++i) {s.counters[i]=buffer(d,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);s.readbacks[i]=buffer(d,256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);}
        if(s.rtReady) {
            const size_t totalBlas=geometriesById.size();
            s.templateBlases=static_cast<uint32_t>(totalBlas-firstCityModule);
            s.maxTlasInstances=s.maxSceneInstances;
            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(totalBlas);
            std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> inputs(totalBlas);
            uint64_t scratchBytes=256;
            for(size_t i=0;i<totalBlas;++i) {
                Impl::Part& pRef = *geometriesById[i];
                auto& g=geometries[i];g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags=isCutoutMaterial(pRef.sub.material)?D3D12_RAYTRACING_GEOMETRY_FLAG_NONE:D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                g.Triangles.VertexBuffer={s.vb->GetGPUVirtualAddress(),sizeof(BmeshVertex)};g.Triangles.VertexCount=static_cast<UINT>(vertices.size());g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexBuffer=s.ib->GetGPUVirtualAddress()+uint64_t(pRef.sub.firstIndex)*4;g.Triangles.IndexCount=pRef.sub.indexCount;g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
                auto& in=inputs[i];in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;in.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;in.NumDescs=1;in.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;in.pGeometryDescs=&g;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&in,&pre);scratchBytes=std::max(scratchBytes,pre.ScratchDataSizeInBytes);
                pRef.blas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            s.scratch=buffer(d,scratchBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};ti.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;ti.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;ti.NumDescs=static_cast<UINT>(s.maxTlasInstances);ti.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&ti,&pre);
            s.tlas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const auto tlasDesc=s.tlas->GetDesc();
            s.tlasBytes=d->GetResourceAllocationInfo(0,1,&tlasDesc).SizeInBytes;
            s.tlasScratch=buffer(d,pre.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            s.instances=buffer(d,s.maxTlasInstances*sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            s.rayMapped=mapForWriting(s.instances.Get());
            const size_t sizeBytes=totalBlas*sizeof(uint64_t);
            auto sizes=buffer(d,sizeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto sizesReadback=buffer(d,sizeBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            if(!s.context.executeSingleCommandListAndWait([&](void* raw) {
                ComPtr<ID3D12GraphicsCommandList4> cmd;check(static_cast<ID3D12GraphicsCommandList*>(raw)->QueryInterface(IID_PPV_ARGS(&cmd)));
                for(size_t i=0;i<totalBlas;++i) {
                    Impl::Part& pRef = *geometriesById[i];
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{};
                    info.DestBuffer=sizes->GetGPUVirtualAddress()+i*sizeof(uint64_t);
                    info.InfoType=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=inputs[i];build.ScratchAccelerationStructureData=s.scratch->GetGPUVirtualAddress();build.DestAccelerationStructureData=pRef.blas->GetGPUVirtualAddress();cmd->BuildRaytracingAccelerationStructure(&build,1,&info);uav(cmd.Get(),s.scratch.Get());uav(cmd.Get(),pRef.blas.Get());
                }
                transition(cmd.Get(),sizes.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyBufferRegion(sizesReadback.Get(),0,sizes.Get(),0,sizeBytes);
            },error))return false;
            s.scratch.Reset();
            // Pack each driver-reported compact BLAS into a single AS buffer. Individual
            // module BLASes require 256-byte offsets, not separate 64 KiB heap allocations.
            std::vector<uint64_t> compactSizes(totalBlas);
            D3D12_RANGE readRange{0,sizeBytes};
            void* mapped=nullptr;
            check(sizesReadback->Map(0,&readRange,&mapped));
            std::memcpy(compactSizes.data(),mapped,sizeBytes);
            D3D12_RANGE noWrite{0,0};
            sizesReadback->Unmap(0,&noWrite);
            uint64_t poolBytes=0;
            constexpr uint64_t align=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
            for (size_t i=0; i<totalBlas; ++i) {
                const uint64_t bytes=compactSizes[i];
                if (!bytes || bytes>geometriesById[i]->blas->GetDesc().Width) throw std::runtime_error("Invalid compact BLAS size");
                compactSizes[i]=poolBytes;
                poolBytes+=(bytes+align-1)&~(align-1);
            }
            s.compactBlasPool=buffer(d,poolBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (!s.context.executeSingleCommandListAndWait([&](void* raw) {
                ComPtr<ID3D12GraphicsCommandList4> cmd;
                check(static_cast<ID3D12GraphicsCommandList*>(raw)->QueryInterface(IID_PPV_ARGS(&cmd)));
                for (size_t i=0; i<totalBlas; ++i) {
                    auto* part=geometriesById[i];
                    part->compactBlas=s.compactBlasPool->GetGPUVirtualAddress()+compactSizes[i];
                    cmd->CopyRaytracingAccelerationStructure(part->compactBlas,part->blas->GetGPUVirtualAddress(),D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
                }
                uav(cmd.Get(),s.compactBlasPool.Get());
            },error))return false;
            for (auto* part : geometriesById) part->blas.Reset();
            startupCheckpoint(); // acceleration structures built and compacted
            startupStep("trilogy acceleration structures");
            const auto poolDesc=s.compactBlasPool->GetDesc();
            s.blasBytes=d->GetResourceAllocationInfo(0,1,&poolDesc).SizeInBytes;
        }
        {
            const uint32_t emptyRange[4]{};
            s.noDeform=buffer(d,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            upload(s.noDeform.Get(),emptyRange,sizeof(emptyRange));
        }
        for (Impl::DeformingSurface* surface : {&s.canopy, &s.banner}) {
            if (!surface->ready) continue;
            auto& c=*surface;
            constexpr uint32_t ring=Impl::DeformingSurface::kRing;
            c.upload=buffer(d,uint64_t(c.count)*sizeof(BmeshVertex)*ring,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            c.uploadMapped=mapForWriting(c.upload.Get());
            c.previous=buffer(d,(uint64_t(c.count)+1u)*16u*ring,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            c.previousMapped=mapForWriting(c.previous.Get());
            if (s.rtReady) {
                // One small acceleration structure per part per ring slot, each built from the rest shape
                // now so every slot is valid before the cloth first moves.
                c.rayIndexBuffer=buffer(d,c.localIndices.size()*4u,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                upload(c.rayIndexBuffer.Get(),c.localIndices.data(),c.localIndices.size()*4u);
                c.rayPositions=buffer(d,uint64_t(c.rayVertices)*12u*ring,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                c.rayPositionsMapped=mapForWriting(c.rayPositions.Get());
                for (uint32_t slot=0;slot<ring;++slot) s.writeDeformRayPositions(c,slot);
                uint64_t scratchBytes=256;
                std::vector<std::array<D3D12_RAYTRACING_GEOMETRY_DESC,ring>> geometry(c.partIndices.size());
                c.blases.resize(c.partIndices.size());
                if (!c.rebuild) c.restBlases.resize(c.partIndices.size());
                for (size_t k=0;k<c.partIndices.size();++k) {
                    for (uint32_t slot=0;slot<ring;++slot) geometry[k][slot]=s.deformGeometry(c,k,slot);
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
                    in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;in.Flags=c.buildFlags;
                    in.NumDescs=1;in.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;in.pGeometryDescs=&geometry[k][0];
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};d5->GetRaytracingAccelerationStructurePrebuildInfo(&in,&pre);
                    scratchBytes=std::max({scratchBytes,pre.ScratchDataSizeInBytes,pre.UpdateScratchDataSizeInBytes});
                    for (auto& blas : c.blases[k])
                        blas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    if (!c.rebuild)
                        c.restBlases[k]=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                }
                constexpr uint64_t align=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
                c.scratchStride=(scratchBytes+align-1)&~(align-1);
                c.scratch=buffer(d,c.scratchStride*c.partIndices.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                if(!s.context.executeSingleCommandListAndWait([&](void* raw) {
                    ComPtr<ID3D12GraphicsCommandList4> cmd;check(static_cast<ID3D12GraphicsCommandList*>(raw)->QueryInterface(IID_PPV_ARGS(&cmd)));
                    for (size_t k=0;k<c.blases.size();++k)
                        for (uint32_t slot=0;slot<=ring;++slot) {
                            if (slot==ring && c.rebuild) break;
                            auto& blas=slot<ring ? c.blases[k][slot] : c.restBlases[k];
                            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                            build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;build.Inputs.Flags=c.buildFlags;
                            build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.pGeometryDescs=&geometry[k][slot<ring ? slot : 0u];
                            build.DestAccelerationStructureData=blas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=c.scratch->GetGPUVirtualAddress();
                            cmd->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(cmd.Get(),c.scratch.Get());uav(cmd.Get(),blas.Get());
                        }
                },error))return false;
                for (size_t k=0;k<c.partIndices.size();++k) s.parts[c.partIndices[k]].compactBlas=c.blases[k][0]->GetGPUVirtualAddress();
            }
        }
        startupStep("trilogy cloth acceleration structures");
        if(s.rtReady && !s.context.benchmark()) s.nrd.initialize(nullptr); // it only denoises ray-traced light
        startupStep("NRD");
        StartupProgress::instance().stageDone();
        if(!rtCompiler.waitAll()) {
            // The raster city is already built: a pipeline this driver cannot make costs ray tracing, not the city.
            BOO_LOG_WARN("M2Mesh","M2 ray-traced pipelines could not be created; the city draws with raster lighting.");
            s.rtReady=false;
            s.rt.Reset();s.rtCutout.Reset();s.skyRt.Reset();s.rrSky.Reset();s.rrOpaque.Reset();s.rrCutout.Reset();
        }
        // As before: ray reconstruction needs all three of its pipelines, built in order sky, opaque, cutout.
        else if(!rrSkyOk){s.rrSky.Reset();s.rrOpaque.Reset();s.rrCutout.Reset();}
        else if(!rrOpaqueOk){s.rrOpaque.Reset();s.rrCutout.Reset();}
        startupStep("trilogy ray-traced pipelines (3, with 3 ray reconstruction variants)");
        startupCheckpoint();
        if (impl_->vb && !s.context.waitForGpu()) throw std::runtime_error("Could not retire previous scene resources");
        impl_.swap(next);
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
        rd.Format=DXGI_FORMAT_R32_TYPELESS;
        D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1;ComPtr<ID3D12Resource> depth;
        check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)));s.depth=std::move(depth);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};dsvDesc.Format=DXGI_FORMAT_D32_FLOAT;dsvDesc.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        d->CreateDepthStencilView(s.depth.Get(),&dsvDesc,s.dsv->GetCPUDescriptorHandleForHeapStart());
        s.dlssDepthReadable=false;s.dlssHistoryValid=false;s.dlssMotion.Reset();
        s.rrReadable=false;s.rrHitDistance.Reset();
        rd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        s.accum.Reset();s.accumValid=false;

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
            if (s.rrCutout) {
                auto td=s.gbufNormRough->GetDesc();
                td.Format=DXGI_FORMAT_R16_FLOAT;
                if (SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&s.rrHitDistance))))
                    d->CreateRenderTargetView(s.rrHitDistance.Get(),nullptr,rtvAt(9u));
            }
        }
        s.reblurPrevValid=false;
        s.nrd.resize(width,height);
        s.w=width;s.h=height;return true;
    }catch(...) {s.w=0;s.h=0;return false;}
}
void TrilogyMeshRenderer::retire(uint32_t slot) {
    auto& s = *impl_;
    if (!s.pending[slot]) return;
    s.pending[slot] = false;
    s.hits = s.queries = 0;
    s.verified = s.highPathExecuted = s.neuralMaterialDrawn = false;
    void* ptr = nullptr;
    D3D12_RANGE read{0, 12};
    if (SUCCEEDED(s.readbacks[slot]->Map(0, &read, &ptr)) && ptr) {
        const auto* counts = static_cast<const uint32_t*>(ptr);
        s.hits = counts[0];
        s.queries = counts[1];
        s.verified = s.hits > 0;
        s.highPathExecuted = (counts[2] & 1u) != 0;
        s.neuralMaterialDrawn = (counts[2] & 2u) != 0;
        D3D12_RANGE written{0, 0};
        s.readbacks[slot]->Unmap(0, &written);
    }
}
bool TrilogyMeshRenderer::record(void* raw,const FrameContext& f,const M1CameraState& camera,void* hdrRaw,void* distRaw,uint64_t tensor,uint32_t width,uint32_t height,uint32_t slot,bool raytracing,bool canonical,bool compareDirectorLighting,const float* dlssJitterPixels,bool rayReconstruction,bool temporalSuperResolution) {
    auto& s=*impl_;auto* cmd=static_cast<ID3D12GraphicsCommandList*>(raw);auto* hdr=static_cast<ID3D12Resource*>(hdrRaw);auto* dist=static_cast<ID3D12Resource*>(distRaw);auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());
    if(canonical)retire(slot);
    const bool useRt=raytracing&&s.rtReady&&(s.w==width&&s.h==height&&s.gbufNormRough!=nullptr);
    if (canonical) s.highPathExecuted = s.neuralMaterialDrawn = false;
    if (!useRt && canonical) {
        s.pending.fill(false);
        s.verified=false;
        s.hits=0;
        s.queries=0;
        s.denoiseMode=0;
    }
    const bool isM2=(s.visualMode==ShowVisualMode::M2Trilogy180s);
    const float filmTime=float(double(f.samplePosition)/f.sampleRate);
    const int64_t riverSample=shanghaiRiverSample(s.visualMode,f);
    const float sceneTime=riverSample>=0 ? float(double(riverSample)/f.sampleRate) : filmTime;
    const uint32_t act=riverSample>=0 ? 0u : isM2
        ? static_cast<uint32_t>(std::clamp<int64_t>(f.samplePosition/2880000,0,2))
        : ((s.visualMode==ShowVisualMode::FullFilm360s&&filmTime>=245.f&&filmTime<275.f)?1u:2u);

    const InteractiveView& iv=f.interactive;
    const bool orbital = isOrbitalCutaway(s.visualMode, f);
    const bool traceIndirect = useRt && !orbital && iv.aovMode == 0u && f.pathTracing != PathTracingLevel::Off;
    if (canonical) {
        // Hard editorial cuts must clear motion, NRD, and reconstruction
        // history even when two cameras are close enough to pass the pose heuristic.
        const bool crown=s.visualMode==ShowVisualMode::FullFilm360s &&
            filmTime>=kCrownOrbitStartSeconds && filmTime<kCrownOrbitEndSeconds;
        const uint32_t shot=(act<<8u) | (camera.shotIndex<<4u) | (uint32_t(camera.insert)<<16u) |
                            (orbital?1u:0u) | (riverSample>=0?2u:0u) | (crown?4u:0u);
        const auto previous = s.pathTracingState;
        s.pathTracingState = !s.rtReady ? RendererFeatureState::Unavailable :
            f.pathTracing == PathTracingLevel::Off ? RendererFeatureState::Off :
            traceIndirect ? RendererFeatureState::Active : RendererFeatureState::Idle;
        if (previous != s.pathTracingState || s.lastPathTracingLevel != f.pathTracing || shot != s.previousCanonicalShot)
            resetTemporalHistory();
        s.previousCanonicalShot=shot;
        s.lastPathTracingLevel = f.pathTracing;
    }
    bool writeMotion=canonical && dlssJitterPixels && s.motionSky && width==s.w && height==s.h;
    if (writeMotion && (!s.dlssMotion || !s.previousTransforms || !s.motionCamera)) {
        try {
            if (!s.previousTransforms) {
                s.previousTransforms=buffer(d,4ull*s.maxSceneInstances*sizeof(MotionRows),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                s.motionMapped=mapForWriting(s.previousTransforms.Get());
            }
            if (!s.motionCamera) s.motionCamera=buffer(d,4u*256u,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            auto desc=s.depth->GetDesc();desc.Format=DXGI_FORMAT_R16G16_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&s.dlssMotion)));
            auto rtv=s.rtv->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=SIZE_T(8)*d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            d->CreateRenderTargetView(s.dlssMotion.Get(),nullptr,rtv);
        } catch (const std::exception& e) {
            BOO_LOG_WARN("DLSS",std::string("Motion buffer unavailable: ")+e.what());
            writeMotion=false;
        }
    }
    if (writeMotion) {
        const double deltaTime=(double(f.samplePosition)-double(s.lastDlssSample))/double(std::max(1,f.sampleRate));
        const auto& prev=s.lastDlssCamera;
        float move2=0.f, alignment=0.f, oldLength2=0.f, newLength2=0.f;
        for (int k=0; k<3; ++k) {
            const float step=camera.eye[k]-prev.eye[k];move2+=step*step;
            const float old=prev.target[k]-prev.eye[k],now=camera.target[k]-camera.eye[k];
            alignment+=old*now;oldLength2+=old*old;newLength2+=now*now;
        }
        s.dlssReset=!s.dlssHistoryValid || deltaTime<0. || deltaTime>0.5 ||
            move2>36.f || alignment<0.75f*std::sqrt(oldLength2*newLength2) || std::abs(camera.fovYRadians-prev.fovYRadians)>0.22f;
        s.lastDlssCamera=camera;s.lastDlssSample=f.samplePosition;
        ++s.dlssFrame;s.previousRows.clear();
    } else if (canonical) {
        s.dlssHistoryValid=false;s.dlssReset=true;
    }
    const uint32_t dynMarbles=orbital ? 0u : std::min<uint32_t>(iv.marbleCount,InteractiveView::kMaxMarbles);

    // Baked PhysX cloth (the canopy, the "Last Light" banner), posed before any instance is written so
    // their parts point at this frame's acceleration structures. The motion pass carries one moving
    // range; the two surfaces are never on screen together (the banner before 45 s, the canopy later).
    D3D12_GPU_VIRTUAL_ADDRESS deformedPrevious=s.noDeform ? s.noDeform->GetGPUVirtualAddress() : 0;
    s.activeDeforming=nullptr;
    for (Impl::DeformingSurface* surface : {&s.banner, &s.canopy}) {
        const bool active=surface->ready && !s.activeDeforming && !isM2 && riverSample<0 && !orbital &&
                          s.isPartActiveInShow(s.parts[surface->partIndices[0]],filmTime);
        if (active) {
            deformedPrevious=s.updateDeforming(*surface,cmd,sceneTime,cityFlightMotion(f),useRt);
            s.activeDeforming=surface;
        } else {
            surface->haveLast=false;
        }
    }

    s.frameInstances.clear();
    s.batches.clear();
    s.liftGlazing.clear();
    auto& inst = s.rayInstances;
    inst.clear();
    CityDrawStats stats{};
    stats.templateBlases=s.templateBlases; stats.blasBytes=s.blasBytes; stats.tlasBytes=s.tlasBytes;
    auto append = [&](const Impl::Part& part, const DrawConstants& draw, uint32_t seed, bool visible, bool city, uint32_t actor=0u) {
        const uint32_t id=static_cast<uint32_t>(s.frameInstances.size());
        s.frameInstances.push_back(makeInstanceData(draw,part.meshId,seed));
        if (writeMotion) {
            // Mesh and actor are stable across city culling, LOD and draw reordering.
            auto& prior=s.motionHistory[(uint64_t(part.meshId)<<32u)|actor];
            MotionRows current{};std::copy_n(draw.rows,12,current.begin());
            s.previousRows.push_back(!s.dlssReset && prior.frame+1u==s.dlssFrame ? prior.rows : current);
            prior={current,s.dlssFrame};
        }
        if (useRt) {
            const uint32_t m=draw.mat;
            const bool glass=(m==3 || m==4 || m==18 || m==21 || m==35 || m==40);
            auto& ray=inst.emplace_back();
            std::memcpy(ray.Transform,draw.rows,sizeof(ray.Transform));
            ray.InstanceID=id;
            ray.InstanceMask=glass?2u:1u;
            ray.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            ray.AccelerationStructure=part.compactBlas;
        }
        if (!visible) return;
        const bool liftPane=draw.part==kObservatoryLiftCab || draw.part==kWaterfrontLiftCab ||
                            (part.kit==2 && draw.part==0u);
        if (!useRt && iv.aovMode==0u && draw.mat==40u && liftPane) {
            s.liftGlazing.push_back({id,1,part.sub.indexCount,part.meshId,false});
            return;
        }
        if (city) ++stats.visibleCityInstances;
        if (!s.batches.empty()) {
            auto& last=s.batches.back();
            if (last.meshId==part.meshId && last.first+last.count==id) {
                ++last.count;
                return;
            }
        }
        s.batches.push_back({id,1,part.sub.indexCount,part.meshId,isCutoutMaterial(draw.mat)});
        if (city) ++stats.cityDrawBatches;
    };
    std::array<float,4> courierLamp{};
    const float orchidMotion = std::clamp(f.motionScale, 0.f, 1.f)
        * (f.edition==Edition::Midnight ? 1.24f : 1.f)
        * (f.qualityPreset==QualityPresetId::Experimental4K ? 1.28f : 1.f);
    const bool terrace = act==2 && !(s.visualMode==ShowVisualMode::FullFilm360s && filmTime>=338.5f);
    const float terraceFade = (s.visualMode==ShowVisualMode::FullFilm360s)
        ? std::clamp((338.5f-filmTime)/3.f,0.f,1.f) : 1.f;
    std::array<float,kPlanterOrchids> planterGust{};
    if (terrace && orchidMotion>0.f) {
        for (uint32_t plant=0; plant<kPlanterOrchids; ++plant) {
            // The courier sweeps over the east planter. Its wake travels across
            // the 9.7 m between planters at about 4 m/s and loses strength.
            const float crossing = plant ? 0.f : 9.7f/4.f;
            const auto pos = sanctuaryCourierPosition(filmTime-crossing, cityFlightMotion(f));
            const float dx=pos[0]-4.85f, dz=pos[2]-7.02f;
            const float pulse=std::clamp(1.f-(dx*dx+dz*dz)/11.f,0.f,1.f);
            planterGust[plant]=(plant ? 1.f : .55f)*pulse*pulse*terraceFade;
        }
    }
    for(const auto& part:s.parts) {
        if (orbital) break;
        const bool active=(isM2 || riverSample>=0)?(part.kit==act||(act==2&&part.kit>=2)):s.isPartActiveInShow(part,filmTime);
        if (!active) continue;
        auto draw=s.transform(part,f,sceneTime);
        if (part.kit!=3) {
            if(part.kit==2 && part.sub.part==kSkyDrone)
                courierLamp={draw.rows[3],draw.rows[7],draw.rows[11],1.f};
            append(part,draw,0,true,false);
            if(part.kit==2 && part.sub.part==kSkyPlane)
                append(part,s.transform(part,f,sceneTime,true),0,true,false,1u);
            continue;
        }
        poseOrchid(draw,part.sub.part,camera.revealOpen01,filmTime,orchidMotion,0.f);
        append(part,draw,0,true,false);
        if (!terrace) continue;

        for (uint32_t plant=0; plant<kPlanterOrchids; ++plant) {
            const float side=plant ? 1.f : -1.f;
            const float scale=(plant ? .50f : .62f)*terraceFade;
            // Sink each stem into the soil near the inside corner of its existing planter.
            const float hub[3]={side*4.85f,2.86f-scale*kOrchidRoot[1],7.02f};
            writeModelTransform(draw.rows,hub,-side*.46f,0.f,0.f,scale);
            poseOrchid(draw,part.sub.part,1.f,filmTime,.87f*orchidMotion,side*1.8f,planterGust[plant]);
            append(part,draw,0,true,false,plant+1u);
        }
        if (part.sub.part==14u && part.sub.material==3u && camera.revealOpen01>.01f && orchidMotion>0.f) {
            // Reuse the actual wing geometry and its BLAS for all loose petals.
            constexpr uint32_t counts[]{4u,10u,kMaxDriftingPetals};
            const uint32_t count=counts[std::min(2u,effectiveEffectsDetail(f))];
            for (uint32_t i=0; i<count; ++i) {
                append(part,driftingPetal(i,filmTime,orchidMotion,terraceFade*camera.revealOpen01),0,true,false,3u+i);
            }
        }
    }
    Impl::Part kitPart{};
    kitPart.kit=2;
    const DrawConstants taipei=s.transform(kitPart,f,sceneTime);
    const float districtKit[4]={taipei.rows[3],taipei.rows[7],taipei.rows[11],.08f/taipei.rows[0]};
    if (!orbital && (act==1 || act==2)) {
        kitPart.kit=act;
        const auto kit=s.transform(kitPart,f,sceneTime);
        if (canonical && s.collisionAct!=act) {
            s.computeCityCollision(act,kit.rows,s.collisionBoxes);
            s.collisionAct=act;
        }
        s.selectCity(camera,float(width)/height,act,kit,useRt,stats);
        for (size_t m=0; m<s.moduleParts.size(); ++m) {
            const auto& group=s.selectedModules[m];
            for (const auto& part : s.moduleParts[m]) {
                auto emit = [&](const Impl::SelectedPlacement& placement,bool visible) {
                    const auto& location=s.city.placements[placement.id];
                    const uint32_t tier=part.sub.part>>16;
                    const float fade=placement.detailScale[tier];
                    if (fade<=.001f) return;
                    DrawConstants dc{};
                    composeAffine(kit.rows,location.rows.data(),dc.rows);
                    if (fade<1.f) {
                        // Optional projections shrink continuously to their own anchors.
                        // Both raster and TLAS use this same world transform.
                        for (int row=0; row<3; ++row) {
                            float* basis=dc.rows+row*4;
                            for (int axis=0; axis<3; ++axis) {
                                basis[3]+=(1.f-fade)*basis[axis]*part.detailCenter[axis];
                                basis[axis]*=fade;
                            }
                        }
                    }
                    dc.mat=part.sub.material;
                    dc.part=part.sub.part&0xffffu;
                    // City flags use the high half only for static module instances.
                    if (m != location.modules[2]) dc.part |= 1u << 24;
                    append(part,dc,location.seed,visible,true,placement.id);
                };
                for (const auto& place : group.visible) emit(place,true);
                for (const auto& place : group.rayOnly) emit(place,false);
            }
        }
    }

    if (canonical) { s.lastBirdCount = 0; s.flockFocus.valid = false; }
    struct FlockPlan {
        bool shown = false;
        uint32_t authored = 0, birds = 0;
        BirdSpecies filmSpecies = BirdSpecies::GreatEgret, species = BirdSpecies::GreatEgret;
        FlockFormation formation = FlockFormation::ExtendedCluster;
        float motion = 1.f, leadYaw = 0.f;
        bool trilogyRoutes = false;
        uint32_t filmKettle = 0;  // "Last Light": the film's own kettle, flown whatever the workout dial says
        std::array<std::array<float,3>,kCityBirdCount> authoredCenters{};
    } flockPlan;
    if (!orbital && (s.visualMode != ShowVisualMode::FullFilm360s || filmTime < 335.5f)) {
        const bool showKites = act == 2;
        const BirdSpecies filmSpecies = showKites ? BirdSpecies::BlackKite : BirdSpecies::GreatEgret;
        const auto formation = static_cast<FlockFormation>(std::min(f.flockFormation, kFlockFormationCount - 1u));
        // The workout flock is the species that flies the chosen formation in life.
        const BirdSpecies joinSpecies = flockSpecies(formation, showKites);
        const uint32_t authored = std::min(kCityBirdCount,3u+effectiveEffectsDetail(f));
        const float flightMotion = cityFlightMotion(f);
        const bool trilogyRoutes = isM2 || riverSample>=0;
        std::array<BirdTransforms,kCityBirdCount> flock;
        // The authored birds fly exactly as in the film and lead whatever joins them.
        for (uint32_t bird = 0; bird < authored; ++bird) {
            const auto pose = birdFlightPose(act,bird,sceneTime,flightMotion,trilogyRoutes);
            if (bird == 0) flockPlan.leadYaw = pose.yaw;
            flockPlan.authoredCenters[bird] = pose.center;
            flock[bird] = articulateCityBird(pose);
        }
        // Group like mesh parts across birds into one instanced raster draw. Each
        // articulated wing uses the same world transform in the ray instance list.
        {
            const bool kites = filmSpecies == BirdSpecies::BlackKite;
            const auto firstPart = kites ? kKiteBody : kEgretBody;
            for (const auto& part : s.birdTemplates[kites ? 0 : 1]) {
                DrawConstants draw{};
                draw.mat = part.sub.material;
                draw.part = part.sub.part;
                const auto joint = part.sub.part - firstPart;
                for (uint32_t bird = 0; bird < authored; ++bird) {
                    const auto& transform = flock[bird].at(joint);
                    std::copy(transform.begin(),transform.end(),draw.rows);
                    append(part,draw,0u,true,false,bird);
                }
            }
        }
        // An off-camera world actor, visible in the Act 2 mirror through the normal RT path.
        if (!s.birdTemplates[2].empty() && f.motionScale >= 1.f &&
            s.visualMode == ShowVisualMode::FullFilm360s && act == 2 && !trilogyRoutes &&
            filmTime >= kMirrorWitnessStart && filmTime <= kMirrorWitnessEnd) {
            const auto witness = articulateCityBird(mirrorWitnessPose(filmTime,flightMotion));
            for (const auto& part : s.birdTemplates[2]) {
                DrawConstants draw{};
                draw.mat = part.sub.material;
                draw.part = part.sub.part;
                const auto& transform = witness.at(part.sub.part - kNightHeronBody);
                std::copy(transform.begin(),transform.end(),draw.rows);
                append(part,draw,0u,true,false,kCityBirdCount);
            }
        }
        // The workout flock joins them below, once everything that goes through append() is in.
        flockPlan.shown = true;
        flockPlan.authored = authored;
        flockPlan.filmSpecies = filmSpecies;
        flockPlan.species = joinSpecies;
        flockPlan.formation = formation;
        flockPlan.motion = flightMotion;
        flockPlan.trilogyRoutes = trilogyRoutes;
        const auto& joinTemplates = s.birdTemplates[joinSpecies == BirdSpecies::BlackKite ? 0 : 1];
        if (!joinTemplates.empty() && joinTemplates.size() <= kMaxBirdParts)
            flockPlan.birds = std::min({f.workout.birds, s.workoutBirdCapacity, kMaxWorkoutBirds});
        // "Last Light": over Taipei at dusk the film flies its own kettle of kites. A workout released
        // during the opening joins that kettle rather than replacing it.
        const auto& kiteTemplates = s.birdTemplates[0];
        if (s.visualMode == ShowVisualMode::FullFilm360s && act == 2 && !trilogyRoutes && filmTime < float(kLastLightEnd) &&
            !kiteTemplates.empty() && kiteTemplates.size() <= kMaxBirdParts && s.workoutBirdCapacity >= kLastLightKites) {
            flockPlan.filmKettle = f.qualityPreset == QualityPresetId::Lite ? 12u : kLastLightKites;
            flockPlan.formation = FlockFormation::Kettle;
            flockPlan.species = BirdSpecies::BlackKite;
            flockPlan.birds = std::max(flockPlan.birds, flockPlan.filmKettle);
        }
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

        // Dynamic marbles use the seed DWORD for the two remaining FP16 motion components.
        append(tpl,dc,dc.pad,true,false,m);
    }

    const size_t frameSlot=slot&3u;
    if (!canonical && !s.compareInstanceBuffer) {
        s.compareInstanceBuffer=buffer(d,s.maxSceneInstances*sizeof(InstanceData)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        s.compareInstanceMapped=mapForWriting(s.compareInstanceBuffer.Get());
    }
    if (!canonical && useRt && !s.compareInstances) {
        s.compareInstances=buffer(d,s.maxTlasInstances*sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        s.compareRayMapped=mapForWriting(s.compareInstances.Get());
    }
    ID3D12Resource* const instanceResource=canonical ? s.instanceBuffer.Get() : s.compareInstanceBuffer.Get();
    ID3D12Resource* const rayResource=canonical ? s.instances.Get() : s.compareInstances.Get();
    uint8_t* const instanceMapped=canonical ? s.instanceMapped : s.compareInstanceMapped;
    uint8_t* const rayMapped=canonical ? s.rayMapped : s.compareRayMapped;
    const size_t appended=s.frameInstances.size();
    const uint32_t flockBirds=flockPlan.birds;
    const auto& flockTemplates=s.birdTemplates[flockPlan.species==BirdSpecies::BlackKite ? 0 : 1];
    const uint32_t flockParts=flockBirds ? static_cast<uint32_t>(flockTemplates.size()) : 0u;
    const size_t flockInstances=size_t(flockBirds)*flockParts;
    const PhysxBodyFrame* bodies=(f.physxBodies && !orbital && iv.aovMode==0u) ? f.physxBodies : nullptr;
    const size_t bodyInstances=bodies ? std::min<size_t>(bodies->count,s.physxCapacity) : 0u;
    if (appended+flockInstances+bodyInstances>s.maxSceneInstances) throw std::runtime_error("City instance upload exceeds reserved slot");
    const size_t instanceOffset=frameSlot*s.maxSceneInstances*sizeof(InstanceData);
    const size_t rayOffset=frameSlot*s.maxTlasInstances*sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    const bool flockRays=useRt && rayMapped;
    if (flockBirds) {
        // The workout flock. Every bird is posed and written on all cores, straight into this
        // pass's slice of the upload buffers, after everything that went through append().
        const auto firstPart=flockPlan.species==BirdSpecies::BlackKite ? kKiteBody : kEgretBody;
        struct PartInfo { uint32_t meshId, material, part, joint, mask; D3D12_GPU_VIRTUAL_ADDRESS blas; };
        std::array<PartInfo,kMaxBirdParts> partInfo{};
        for (uint32_t p=0; p<flockParts; ++p) {
            const auto& part=flockTemplates[p];
            const uint32_t m=part.sub.material;
            const bool glass=(m==3 || m==4 || m==18 || m==21 || m==35 || m==40);
            partInfo[p]={part.meshId,m,part.sub.part,std::min<uint32_t>(part.sub.part-firstPart,4u),glass?2u:1u,part.compactBlas};
        }
        auto flockFrame=workoutFlockFrame(act,sceneTime,flockPlan.motion,flockPlan.trilogyRoutes);
        if (flockPlan.filmKettle) flockFrame.lean=kLastLightKettleLean;
        const WorkoutFlock arrival=f.workout;
        // The film's kettle is always fully grown; only birds the dial adds beyond it arrive.
        const float flockSize=flockPlan.filmKettle ? std::max(float(flockPlan.filmKettle), arrival.size>0.f ? std::min(arrival.size,float(flockBirds)) : float(flockBirds))
                                                   : (arrival.size>0.f ? std::min(arrival.size,float(flockBirds)) : float(flockBirds));

        // Last frame's transforms, for motion vectors. They are kept bird by bird so that a flock
        // which has grown since still finds each bird where it was.
        const bool keepRows=writeMotion && s.motionMapped;
        const uint32_t rowsKey=act | (uint32_t(flockPlan.species)<<8u) | (uint32_t(flockPlan.formation)<<16u) | (flockParts<<24u);
        if (keepRows) {
            const size_t room=size_t(s.workoutBirdCapacity)*s.birdParts;
            for (auto& rows : s.flockRows) if (rows.size()<room) rows.resize(room);
        }
        const bool history=keepRows && !s.dlssReset && s.flockRowsFrame+1u==s.dlssFrame && s.flockRowsKey==rowsKey;
        const MotionRows* rowsBefore=history ? s.flockRows[s.flockRowsNow].data() : nullptr;
        const uint32_t birdsBefore=history ? s.flockRowsBirds : 0u;
        MotionRows* rowsNow=keepRows ? s.flockRows[1u-s.flockRowsNow].data() : nullptr;

        uint8_t* instanceOut=instanceMapped+instanceOffset+appended*sizeof(InstanceData);
        uint8_t* rayOut=flockRays ? rayMapped+rayOffset+inst.size()*sizeof(D3D12_RAYTRACING_INSTANCE_DESC) : nullptr;
        uint8_t* previousOut=keepRows ? s.motionMapped+(frameSlot*s.maxSceneInstances+appended)*sizeof(MotionRows) : nullptr;
        if (s.flockCenters.size()<flockBirds) s.flockCenters.resize(flockBirds);
        std::array<float,3>* centers=s.flockCenters.data();
        const uint32_t chunks=(flockBirds+kFlockChunk-1u)/kFlockChunk;
        if (s.flockChunks.size()<chunks) { s.flockChunks.resize(chunks); std::iota(s.flockChunks.begin(),s.flockChunks.end(),0u); }
        std::atomic<bool> fault{false};
        // Nothing in here may throw: it runs inside a parallel algorithm.
        auto work=[&](uint32_t chunk) noexcept {
            const uint32_t b0=chunk*kFlockChunk, b1=std::min(flockBirds,b0+kFlockChunk);
            std::array<BirdTransforms,kFlockChunk> local;
            for (uint32_t b=b0; b<b1; ++b) {
                auto pose=workoutBirdPose(act,flockPlan.formation,b,flockBirds,flockSize,sceneTime,
                                          flockPlan.motion,flockPlan.trilogyRoutes,flockFrame);
                if (b>=flockPlan.filmKettle) pose.scale*=workoutBirdArrival(b,arrival.grown,arrival.front,arrival.band);
                centers[b]=pose.center;
                local[b-b0]=articulateCityBird(pose);
            }
            // One part at a time, so each task writes one stream of the write-combined upload
            // memory at once: in order, each byte once, never read back.
            for (uint32_t p=0; p<flockParts; ++p) {
                const PartInfo& info=partInfo[p];
                const size_t first=size_t(p)*flockBirds+b0;
                DrawConstants draw{};
                draw.mat=info.material;
                draw.part=info.part;
                uint8_t* out=instanceOut+first*sizeof(InstanceData);
                for (uint32_t b=b0; b<b1; ++b, out+=sizeof(InstanceData)) {
                    auto& rows=local[b-b0][info.joint];
                    std::copy(rows.begin(),rows.end(),draw.rows);
                    InstanceData data;
                    if (!fillInstanceData(draw,info.meshId,0u,data)) {
                        // A bird that cannot be placed becomes a speck at the flock's centre.
                        fault.store(true,std::memory_order_relaxed);
                        rows={1e-3f,0.f,0.f,flockFrame.centre[0], 0.f,1e-3f,0.f,flockFrame.centre[1], 0.f,0.f,1e-3f,flockFrame.centre[2]};
                        std::copy(rows.begin(),rows.end(),draw.rows);
                        fillInstanceData(draw,info.meshId,0u,data);
                    }
                    std::memcpy(out,&data,sizeof(data));
                }
                if (rayOut) {
                    D3D12_RAYTRACING_INSTANCE_DESC ray{};
                    ray.InstanceMask=info.mask;
                    ray.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
                    ray.AccelerationStructure=info.blas;
                    uint8_t* rout=rayOut+first*sizeof(ray);
                    for (uint32_t b=b0; b<b1; ++b, rout+=sizeof(ray)) {
                        std::memcpy(ray.Transform,local[b-b0][info.joint].data(),sizeof(ray.Transform));
                        ray.InstanceID=static_cast<UINT>(appended+first+(b-b0));
                        std::memcpy(rout,&ray,sizeof(ray));
                    }
                }
                if (previousOut) {
                    uint8_t* pout=previousOut+first*sizeof(MotionRows);
                    for (uint32_t b=b0; b<b1; ++b, pout+=sizeof(MotionRows)) {
                        const MotionRows& now=local[b-b0][info.joint];
                        const size_t at=size_t(b)*flockParts+p;
                        std::memcpy(pout,b<birdsBefore ? rowsBefore[at].data() : now.data(),sizeof(MotionRows));
                        rowsNow[at]=now;
                    }
                }
            }
        };
        if (chunks<=2u) for (uint32_t chunk=0; chunk<chunks; ++chunk) work(chunk);
        else std::for_each(std::execution::par,s.flockChunks.begin(),s.flockChunks.begin()+chunks,work);
        if (fault.load(std::memory_order_relaxed) && !s.flockFaultLogged) {
            s.flockFaultLogged=true;
            BOO_LOG_WARN("Workout","A workout bird had no valid place this frame and was hidden.");
        }
        if (keepRows) {
            s.flockRowsNow=1u-s.flockRowsNow;
            s.flockRowsBirds=flockBirds;
            s.flockRowsFrame=s.dlssFrame;
            s.flockRowsKey=rowsKey;
        }
        // One instanced raster draw for each part of the bird.
        for (uint32_t p=0; p<flockParts; ++p) {
            const auto& part=flockTemplates[p];
            s.batches.push_back({static_cast<uint32_t>(appended+size_t(p)*flockBirds),flockBirds,
                                 part.sub.indexCount,part.meshId,isCutoutMaterial(part.sub.material)});
        }
    }
    if (bodyInstances) {
        // PhysX rigid bodies (the suite's rigid genre): marbles, each its own instance after the flock,
        // written straight into this pass's upload slices like the flock.
        const size_t first=appended+flockInstances;
        uint8_t* instanceOut=instanceMapped+instanceOffset+first*sizeof(InstanceData);
        uint8_t* rayOut=flockRays ? rayMapped+rayOffset+(inst.size()+flockInstances)*sizeof(D3D12_RAYTRACING_INSTANCE_DESC) : nullptr;
        uint8_t* previousOut=(writeMotion && s.motionMapped) ? s.motionMapped+(frameSlot*s.maxSceneInstances+first)*sizeof(MotionRows) : nullptr;
        auto rowsOf=[](const float* pose, const float* stretch, float* rows) {
            const float x=pose[3], y=pose[4], z=pose[5], w=pose[6], r=std::max(pose[7],1e-3f);
            rows[0]=(1.f-2.f*(y*y+z*z))*r; rows[1]=2.f*(x*y-z*w)*r;       rows[2]=2.f*(x*z+y*w)*r;       rows[3]=pose[0];
            rows[4]=2.f*(x*y+z*w)*r;       rows[5]=(1.f-2.f*(x*x+z*z))*r; rows[6]=2.f*(y*z-x*w)*r;       rows[7]=pose[1];
            rows[8]=2.f*(x*z-y*w)*r;       rows[9]=2.f*(y*z+x*w)*r;       rows[10]=(1.f-2.f*(x*x+y*y))*r; rows[11]=pose[2];
            if (stretch)
                for (int row=0; row<3; ++row) { rows[row*4]*=stretch[0]; rows[row*4+1]*=stretch[1]; rows[row*4+2]*=stretch[2]; }
        };
        for (uint32_t look=0; look<6u; ++look) {
            const size_t b0=std::min<size_t>(bodies->lookStart[look],bodyInstances);
            const size_t b1=std::min<size_t>(bodies->lookStart[look+1],bodyInstances);
            if (b1<=b0) continue;
            const Impl::Part& tpl=s.marbleTemplates[look];
            const uint32_t chunks=static_cast<uint32_t>((b1-b0+1023u)/1024u);
            auto work=[&](uint32_t chunk) noexcept {
                const size_t c0=b0+size_t(chunk)*1024u, c1=std::min(b1,c0+1024u);
                DrawConstants draw{};
                draw.mat=tpl.sub.material;
                draw.part=tpl.sub.part;
                draw.first=tpl.sub.firstIndex;
                D3D12_RAYTRACING_INSTANCE_DESC ray{};
                const uint32_t m=tpl.sub.material;
                ray.InstanceMask=(m==3 || m==4 || m==18 || m==21 || m==35 || m==40) ? 2u : 1u;
                ray.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
                ray.AccelerationStructure=tpl.compactBlas;
                for (size_t i=c0; i<c1; ++i) {
                    rowsOf(bodies->poses+8*i,bodies->stretch ? bodies->stretch+3*i : nullptr,draw.rows);
                    InstanceData data;
                    if (!fillInstanceData(draw,tpl.meshId,0u,data)) continue;
                    std::memcpy(instanceOut+i*sizeof(InstanceData),&data,sizeof(data));
                    if (rayOut) {
                        std::memcpy(ray.Transform,draw.rows,sizeof(ray.Transform));
                        ray.InstanceID=static_cast<UINT>(first+i);
                        std::memcpy(rayOut+i*sizeof(ray),&ray,sizeof(ray));
                    }
                    if (previousOut) {
                        float before[12];
                        if (bodies->previousPoses)
                            rowsOf(bodies->previousPoses+8*i,bodies->previousStretch ? bodies->previousStretch+3*i : (bodies->stretch ? bodies->stretch+3*i : nullptr),before);
                        else
                            std::copy(draw.rows,draw.rows+12,before);
                        std::memcpy(previousOut+i*sizeof(MotionRows),before,sizeof(MotionRows));
                    }
                }
            };
            if (chunks<=2u) { for (uint32_t c=0; c<chunks; ++c) work(c); }
            else {
                std::vector<uint32_t> ids(chunks);
                std::iota(ids.begin(),ids.end(),0u);
                std::for_each(std::execution::par,ids.begin(),ids.end(),work);
            }
            s.batches.push_back({static_cast<uint32_t>(first+b0),static_cast<uint32_t>(b1-b0),tpl.sub.indexCount,tpl.meshId,isCutoutMaterial(tpl.sub.material)});
        }
    }
    if (canonical && flockPlan.shown) {
        // Where the flock is and how big, for the bird camera.
        s.lastBirdCount=flockPlan.authored+flockBirds;
        // Frame the first skein of a line formation or the first kettle (the rest fill the sky
        // behind it), otherwise the whole flock with the film's birds.
        const bool skeinOnly=flockBirds && isLineFormation(flockPlan.formation);
        const uint32_t leaders=skeinOnly ? 0u : flockPlan.authored;
        const uint32_t followers=flockFocusCount(flockPlan.formation,flockBirds);
        const double framed=double(leaders+followers);
        double mean[3]{}, spread=0.0;
        auto each=[&](auto&& visit) {
            for (uint32_t i=0; i<leaders; ++i) visit(flockPlan.authoredCenters[i]);
            for (uint32_t i=0; i<followers; ++i) visit(s.flockCenters[i]);
        };
        each([&](const std::array<float,3>& c) { for (int axis=0; axis<3; ++axis) mean[axis]+=c[axis]; });
        for (double& m : mean) m/=framed;
        each([&](const std::array<float,3>& c) { for (int axis=0; axis<3; ++axis) spread+=(c[axis]-mean[axis])*(c[axis]-mean[axis]); });
        s.flockFocus.valid=true;
        for (int axis=0; axis<3; ++axis) s.flockFocus.center[axis]=static_cast<float>(mean[axis]);
        s.flockFocus.radius=std::max(3.f,1.7f*static_cast<float>(std::sqrt(spread/framed)));
        s.flockFocus.headingYaw=flockPlan.leadYaw;
        s.flockFocus.species=static_cast<uint32_t>(flockBirds ? flockPlan.species : flockPlan.filmSpecies);
        // Where the air is, for anything that rides it (the PhysX smoke marks the thermals).
        const auto air=workoutFlockFrame(act,sceneTime,flockPlan.motion,flockPlan.trilogyRoutes);
        for (int axis=0; axis<3; ++axis) s.flockFocus.air[axis]=air.centre[axis];
        s.flockFocus.kettles=flockPlan.formation==FlockFormation::Kettle ? (flockBirds+kFlockUnit-1u)/kFlockUnit : 0u;
    }
    if (appended) upload(instanceResource,s.frameInstances.data(),appended*sizeof(InstanceData),instanceOffset);
    const size_t rayCount=inst.size()+(flockRays ? flockInstances+bodyInstances : size_t(0));
    if (canonical) {
        stats.activeInstances=static_cast<uint32_t>(appended+flockInstances+bodyInstances);
        stats.rayInstances=static_cast<uint32_t>(useRt ? rayCount : 0u);
        stats.rasterBatches=static_cast<uint32_t>(s.batches.size());
        s.lastStats=stats;
    }
    if(useRt && rayCount) {
        if (!inst.empty()) upload(rayResource,inst.data(),inst.size()*sizeof(inst[0]),rayOffset);
        ComPtr<ID3D12GraphicsCommandList4> c4;check(cmd->QueryInterface(IID_PPV_ARGS(&c4)));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.NumDescs=static_cast<UINT>(rayCount);build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.InstanceDescs=rayResource->GetGPUVirtualAddress()+rayOffset;build.DestAccelerationStructureData=s.tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=s.tlasScratch->GetGPUVirtualAddress();uav(cmd,s.tlas.Get());uav(cmd,s.tlasScratch.Get());c4->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(cmd,s.tlas.Get());
    }
    cmd->CopyBufferRegion(s.counters[slot].Get(),0,s.zero.Get(),0,12);transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,hdr,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);transition(cmd,dist,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const UINT rtvInc=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const bool rr=rayReconstruction && useRt && writeMotion && !orbital && !iv.aovMode && rrGuidesReady();
    auto transitionGuides = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        for (auto* tex : {s.gbufNormRough.Get(),s.gbufDiffRadHit.Get(),s.gbufSpecRadHit.Get(),s.rrHitDistance.Get()})
            transition(cmd,tex,before,after);
    };
    if (s.rrReadable) {
        transitionGuides(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        s.rrReadable=false;
    }
    auto handle=s.rtv->GetCPUDescriptorHandleForHeapStart();d->CreateRenderTargetView(hdr,nullptr,handle);auto second=handle;second.ptr+=rtvInc;d->CreateRenderTargetView(dist,nullptr,second);
    if (s.dlssDepthReadable) {
        transition(cmd,s.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        s.dlssDepthReadable=false;
    }
    auto depthHandle=s.dsv->GetCPUDescriptorHandleForHeapStart();cmd->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};D3D12_RECT sc{0,0,LONG(width),LONG(height)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);

    SceneConstants c{};std::copy_n(camera.eye,3,c.eye);c.fov=camera.fovYRadians;c.aspect=float(width)/float(height);for(int i=0;i<3;++i)c.forward[i]=camera.target[i]-camera.eye[i];normalize(c.forward);cross(camera.up,c.forward,c.right);normalize(c.right);cross(c.forward,c.right,c.up);c.time=filmTime;c.reveal=camera.revealOpen01;c.width=width;c.height=height;c.act=act;c.canonical=canonical?1u:0u;
    if(isM2 || riverSample>=0) {
        c.time=sceneTime;
        c.day=act==1?1-std::clamp((c.time-72)/32,0.0f,1.0f):0;
        c.showFlags=0u;
        c.moodAccent=0.0f;
    } else {
        auto sm=[](float v){v=std::clamp(v,0.f,1.f);return v*v*v*(v*(v*6.f-15.f)+10.f);};
        c.day=(s.visualMode==ShowVisualMode::FullFilm360s&&c.time>=335.5f)?(0.84f*sm((c.time-335.5f)/12.5f)):0.0f;
        // "Last Light": the opening is at dusk, and the city's lights come on as the sun sets.
        if (s.visualMode==ShowVisualMode::FullFilm360s&&c.time<float(kLastLightEnd)) c.day=lastLightDaylight(c.time);
        const bool isHijack=(s.visualMode==ShowVisualMode::FullFilm360s&&c.time>=245.f&&c.time<275.f);
        c.showFlags=1u|((f.edition==Edition::Midnight)?2u:0u)|(std::min<uint32_t>(camera.shotIndex,7u)<<2u);
        if (orbital) c.showFlags |= 128u;
        const bool atCrown = filmTime >= kCrownOrbitStartSeconds && filmTime < kCrownOrbitEndSeconds;
        if (s.visualMode == ShowVisualMode::FullFilm360s && (filmTime < 45.f || isHijack || atCrown))
            c.showFlags |= 64u;
        if (s.visualMode == ShowVisualMode::SoloNPU ||
            (s.visualMode == ShowVisualMode::FullFilm360s && filmTime >= 150.f && filmTime < 195.f))
            c.showFlags |= 32u;
        c.moodAccent=isHijack?0.5f:std::clamp(f.moodPaletteAccent,0.0f,1.0f);
        c.moodBloom=isHijack?0.35f:std::clamp(f.moodBloomStrength,0.f,1.f);
        c.moodLight=isHijack?0.35f:std::clamp(f.moodIntensity,0.f,1.f);
        c.moodCut=isHijack?0.f:std::clamp(f.moodCutAccent,0.f,1.f);
    }
    c.pulse=camera.emissivePulse;
    c.courierLamp=courierLamp;
    c.interFlags |= effectiveEffectsDetail(f) << 12u;
    if (f.qualityPreset == QualityPresetId::Lite) c.interFlags |= 1u << 20u;
    if (canonical && s.context.benchmark()) c.interFlags |= 1u << 19u;
    if (traceIndirect) c.interFlags |= static_cast<uint32_t>(f.pathTracing) << 15u;
    if (f.reducedFlash || f.motionScale < 0.99f) c.interFlags |= 1u << 14u;
    // Audio drive packed as RGBA8 into the one spare scene DWORD (low, mid, high, beat). Debug AOVs and progressive accumulation stay static.
    uint32_t packedAudio = (f.interactive.aovMode == 0u && !f.interactive.accumulate)
        ? AudioReactiveFollower::pack(f.audioReactive) : 0u;
    if (f.reducedFlash) packedAudio &= 0x00ffffffu; // A held or injected beat cannot retrigger a flash.
    std::memcpy(&c.pad, &packedAudio, sizeof(packedAudio));

    const uint32_t curAov = std::min<uint32_t>(iv.aovMode, 7u);
    // A synchronized lighting comparison must use the same spatial resolve on both sides.
    const bool temporalAllowed = canonical && !compareDirectorLighting && !rr;
    if (temporalAllowed && curAov == 0u && iv.accumulate && !s.accum && s.accumBlend && s.w == width && s.h == height) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&s.accum)));
    }
    const bool accumulate = temporalAllowed && (curAov == 0u) && iv.accumulate && s.accum && s.accumBlend && s.w == width && s.h == height;
    const bool interactiveMotion = temporalAllowed && useRt && (curAov == 0u) && !accumulate &&
                                   (iv.exploreActive || iv.sunOverride || iv.lampsOff || dynMarbles > 0u) &&
                                   s.nrd.isAvailable();
    const bool splitDenoise = (canonical || compareDirectorLighting) && useRt && (curAov == 0u) && !rr;
    if (rr) {
        c.interFlags |= 1u << 18u;
        buildColumnMajorCameraMatrices(c.eye,c.right,c.up,c.forward,c.fov,c.aspect,s.rrWorldToView,s.rrViewToClip);
    }

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

    const size_t motionSlot=slot&3u;
    const size_t motionOffset=motionSlot*s.maxSceneInstances*sizeof(MotionRows);
    if (writeMotion) {
        // D3D screen Y points down, while the mesh projection's clip Y points up.
        c.jitter[0]=2.f*dlssJitterPixels[0]/width;c.jitter[1]=-2.f*dlssJitterPixels[1]/height;
        reblurJitterPx[0]=dlssJitterPixels[0];reblurJitterPx[1]=-dlssJitterPixels[1];
        if (temporalSuperResolution || rr) c.interFlags|=1u<<17u;
        if (!accumulate && !interactiveMotion) {
            // Replaying a film sample must reproduce its indirect-light pattern.
            // dlssFrame tracks instance identity across live draws, independently of film time.
            const uint64_t sample=static_cast<uint64_t>(std::max<int64_t>(f.samplePosition,0));
            const uint32_t rate=static_cast<uint32_t>(std::max(1,f.sampleRate));
            c.accumFrame=iv.exploreActive ? static_cast<uint32_t>(s.dlssFrame) :
                static_cast<uint32_t>((sample/rate)*60u + ((sample%rate)*60u)/rate);
        }
        const auto currentClip=cameraClipRows(c);
        const auto& previous=s.dlssReset ? currentClip : s.previousClip;
        upload(s.motionCamera.Get(),previous.data(),sizeof(MotionRows),256u*motionSlot);
        if (!s.previousRows.empty()) upload(s.previousTransforms.Get(),s.previousRows.data(),s.previousRows.size()*sizeof(MotionRows),motionOffset);
        s.previousClip=currentClip;
    }

    if (s.context.benchmark() && s.context.benchmark()->mode == BenchmarkMode::PathTracing) {
        constexpr uint32_t openingFlags = (1u << 19u) | (static_cast<uint32_t>(PathTracingLevel::High) << 15u) | 16u;
        if (!useRt || c.time < 7.f || c.time >= 14.f || c.act != 2 || c.canonical != 1 ||
            c.showFlags != 65 || packedAudio != 0 || c.interFlags != openingFlags || c.accumFrame != 0)
            throw BenchmarkWorkloadChanged{};
    }
    (void)tensor;
    ID3D12DescriptorHeap* m2Heaps[] = {s.srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, m2Heaps);
    cmd->SetGraphicsRootSignature(s.root.Get());cmd->SetGraphicsRoot32BitConstants(0,kSceneDwords,&c,0);cmd->SetGraphicsRootShaderResourceView(2,s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(3,s.ib->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(4,s.tlas?s.tlas->GetGPUVirtualAddress():s.vb->GetGPUVirtualAddress());cmd->SetGraphicsRootDescriptorTable(5,s.srvHeap->GetGPUDescriptorHandleForHeapStart());cmd->SetGraphicsRootUnorderedAccessView(6,s.counters[slot]->GetGPUVirtualAddress());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd->SetGraphicsRoot32BitConstants(1,4,districtKit,0);
    cmd->SetGraphicsRootShaderResourceView(7,instanceResource->GetGPUVirtualAddress()+instanceOffset);
    cmd->SetGraphicsRootShaderResourceView(8,s.meshOffsets->GetGPUVirtualAddress());
    auto drawBatches = [&](ID3D12PipelineState* pipeline, bool cutout) {
        cmd->SetPipelineState(pipeline);
        for (const auto& batch : s.batches) {
            if (batch.cutout != cutout) continue;
            cmd->SetGraphicsRoot32BitConstant(1,batch.first,4);
            cmd->DrawInstanced(batch.indexCount,batch.count,0,0);
        }
    };
    // Reuse identical ordering and transforms in the depth and color passes.
    if (writeMotion) {
        cmd->SetGraphicsRootShaderResourceView(9,s.previousTransforms->GetGPUVirtualAddress()+motionOffset);
        cmd->SetGraphicsRootConstantBufferView(10,s.motionCamera->GetGPUVirtualAddress()+256u*motionSlot);
        cmd->SetGraphicsRootShaderResourceView(11,deformedPrevious);
        auto mvRtv=handle;mvRtv.ptr+=SIZE_T(8)*rtvInc;
        transition(cmd,s.dlssMotion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->OMSetRenderTargets(1,&mvRtv,FALSE,&depthHandle);
        drawBatches(s.motionOpaque.Get(),false);
        drawBatches(s.motionCutout.Get(),true);
        cmd->SetPipelineState(s.motionSky.Get());cmd->DrawInstanced(3,1,0,0);
        transition(cmd,s.dlssMotion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        cmd->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
        drawBatches(s.depthOpaque.Get(),false);
        drawBatches(s.depthCutout.Get(),true);
    }
    if (rr) {
        D3D12_CPU_DESCRIPTOR_HANDLE targets[7]{};
        for (UINT i=0;i<7;++i) targets[i].ptr=handle.ptr+SIZE_T(i==6 ? 9 : i)*rtvInc;
        cmd->OMSetRenderTargets(7,targets,FALSE,&depthHandle);
    } else {
        cmd->OMSetRenderTargets(useRt?7u:2u,&handle,TRUE,&depthHandle);
    }
    drawBatches(rr?s.rrOpaque.Get():(useRt?s.rt.Get():s.raster.Get()),false);
    drawBatches(rr?s.rrCutout.Get():(useRt?s.rtCutout.Get():s.rasterCutout.Get()),true);
    // Sky last, at the far plane: only pixels the Z-prepass left at the cleared depth run the sky
    // shader, so its ray-marched light shafts never trace under geometry.
    cmd->SetPipelineState(rr ? s.rrSky.Get() : (useRt ? s.skyRt.Get() : s.sky.Get()));cmd->DrawInstanced(3,1,0,0);
    if (!s.liftGlazing.empty()) {
        constexpr float reflection[4]={.065f,.065f,.065f,1.f};
        cmd->OMSetBlendFactor(reflection);
        cmd->SetPipelineState(s.rasterLiftGlazing.Get());
        for (const auto& batch:s.liftGlazing) {
            cmd->SetGraphicsRoot32BitConstant(1,batch.first,4);
            cmd->DrawInstanced(batch.indexCount,batch.count,0,0);
        }
    }

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
    if(canonical && (useRt || s.context.benchmark())) {cmd->CopyBufferRegion(s.readbacks[slot].Get(),0,s.counters[slot].Get(),0,12);s.pending[slot]=true;}
    transition(cmd,s.counters[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    if (writeMotion) {
        transition(cmd,s.depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.dlssDepthReadable=true;s.dlssHistoryValid=true;
    }
    if (rr) {
        transitionGuides(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.rrReadable=true;
    }
    return useRt;
}
void TrilogyMeshRenderer::recordOrbital(void* commandList, const FrameContext& frame, void* hdrRaw, void* distanceRaw,
                                       uint32_t width, uint32_t height, uint32_t slot, bool canonical) {
    // The orbital station, atmosphere and stars are generated in screen space.
    // Draw at the output size without resizing any of the suspended city G-buffers.
    auto& s = *impl_;
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* device = static_cast<ID3D12Device*>(s.context.deviceRaw());
    auto* hdr = static_cast<ID3D12Resource*>(hdrRaw);
    auto* distance = static_cast<ID3D12Resource*>(distanceRaw);
    if (canonical) {
        retire(slot);
        constexpr uint32_t orbitalShot = UINT32_MAX - 1;
        if (s.previousCanonicalShot != orbitalShot) resetTemporalHistory();
        s.previousCanonicalShot = orbitalShot;
        s.pathTracingState = !s.rtReady ? RendererFeatureState::Unavailable :
            frame.pathTracing == PathTracingLevel::Off ? RendererFeatureState::Off : RendererFeatureState::Idle;
        s.lastStats = {};
        s.hits = s.queries = s.denoiseMode = 0;
        s.verified = false;
    }
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto colorView = s.rtv->GetCPUDescriptorHandleForHeapStart();
    auto distanceView = colorView;
    distanceView.ptr += stride;
    device->CreateRenderTargetView(hdr,nullptr,colorView);
    device->CreateRenderTargetView(distance,nullptr,distanceView);
    transition(cmd,hdr,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(cmd,distance,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    SceneConstants scene{};
    scene.width = width; scene.height = height;
    scene.aspect = float(width)/float(height);
    scene.forward[2] = scene.up[1] = scene.right[0] = scene.fov = 1.f;
    scene.time = float(double(frame.samplePosition)/frame.sampleRate);
    scene.showFlags = 128u;
    if (frame.qualityPreset == QualityPresetId::Lite) scene.interFlags |= 1u << 20u;
    if (frame.reducedFlash || frame.motionScale < .99f) scene.interFlags |= 1u << 14u;
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT bounds{0,0,LONG(width),LONG(height)};
    cmd->RSSetViewports(1,&viewport);
    cmd->RSSetScissorRects(1,&bounds);
    ID3D12DescriptorHeap* heaps[] = {s.srvHeap.Get()};
    cmd->SetDescriptorHeaps(1,heaps);
    cmd->SetGraphicsRootSignature(s.root.Get());
    cmd->SetGraphicsRoot32BitConstants(0,kSceneDwords,&scene,0);
    cmd->SetGraphicsRootDescriptorTable(5,s.srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(s.orbitalSky.Get());
    cmd->OMSetRenderTargets(2,&colorView,TRUE,nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3,1,0,0);
    transition(cmd,distance,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cmd,hdr,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
void* TrilogyMeshRenderer::dlssMotionResource() const noexcept {return impl_->dlssDepthReadable ? impl_->dlssMotion.Get() : nullptr;}
void* TrilogyMeshRenderer::dlssDepthResource() const noexcept {return impl_->dlssDepthReadable ? impl_->depth.Get() : nullptr;}
bool TrilogyMeshRenderer::rrGuidesReady() const noexcept { return impl_->rrHitDistance && impl_->rrCutout && impl_->motionSky; }
void* TrilogyMeshRenderer::rrDiffuseAlbedo() const noexcept { return impl_->rrReadable ? impl_->gbufDiffRadHit.Get() : nullptr; }
void* TrilogyMeshRenderer::rrSpecularAlbedo() const noexcept { return impl_->rrReadable ? impl_->gbufSpecRadHit.Get() : nullptr; }
void* TrilogyMeshRenderer::rrNormalRoughness() const noexcept { return impl_->rrReadable ? impl_->gbufNormRough.Get() : nullptr; }
void* TrilogyMeshRenderer::rrSpecularHitDistance() const noexcept { return impl_->rrReadable ? impl_->rrHitDistance.Get() : nullptr; }
void TrilogyMeshRenderer::rrViewMatrices(float worldToView[16], float viewToClip[16]) const noexcept {
    std::copy_n(impl_->rrWorldToView,16,worldToView);
    std::copy_n(impl_->rrViewToClip,16,viewToClip);
}
bool TrilogyMeshRenderer::dlssHistoryReset() const noexcept {return impl_->dlssReset;}
bool TrilogyMeshRenderer::hardwareVerified() const {return impl_->verified;}
bool TrilogyMeshRenderer::nrdActive() const {return impl_&&impl_->nrd.isAvailable();}
uint32_t TrilogyMeshRenderer::activeDenoiseMode() const noexcept {return impl_?impl_->denoiseMode:0u;}
uint32_t TrilogyMeshRenderer::queriedRays() const {return impl_->queries;}
uint32_t TrilogyMeshRenderer::birdCount() const noexcept { return impl_ ? impl_->lastBirdCount : 0u; }
std::vector<CityCollisionBox> TrilogyMeshRenderer::buildCityCollision(uint32_t act) {
    std::vector<CityCollisionBox> out;
    if (!impl_ || (act!=1u && act!=2u)) return out;
    Impl::Part kitPart{};
    kitPart.kit=act;
    FrameContext f{};
    const DrawConstants kit=impl_->transform(kitPart,f,0.f);
    impl_->computeCityCollision(act,kit.rows,out);
    return out;
}
BakedClothInfo TrilogyMeshRenderer::bakedCloth() const noexcept {
    BakedClothInfo info;
    if (!impl_ || !impl_->activeDeforming) return info;
    const auto& c = *impl_->activeDeforming;
    info.active = c.keyRate > 0.f;
    info.nodes = uint32_t(c.trackNodes());
    info.keyRate = c.keyRate;
    info.widestOffset = c.lastOffset;
    return info;
}
const std::vector<CityCollisionBox>& TrilogyMeshRenderer::cityCollision(uint32_t* act) const noexcept {
    if (act) *act = impl_->collisionAct;
    return impl_->collisionBoxes;
}
uint32_t TrilogyMeshRenderer::workoutBirdCapacity() const noexcept { return impl_ ? impl_->workoutBirdCapacity : 0u; }
bool TrilogyMeshRenderer::reserveWorkoutBirds(uint32_t birds, bool reservePhysx) {
    if (!impl_) return false;
    auto& s=*impl_;
    birds=std::min(birds,kMaxWorkoutBirds);
    const size_t addedPhysx = reservePhysx ? kMaxPhysxBodies - s.physxCapacity : 0u;
    if (birds<=s.workoutBirdCapacity && !addedPhysx) return true;
    if (!s.instanceBuffer || !s.birdParts || !s.context.deviceRaw()) return false;
    // Room to spare, so a flock that is still growing does not come back every few frames.
    const uint32_t capacity=birds > s.workoutBirdCapacity
        ? std::min(kMaxWorkoutBirds,std::max(birds,s.workoutBirdCapacity*2u)) : s.workoutBirdCapacity;
    const size_t room=s.baseSceneInstances+addedPhysx+size_t(capacity)*s.birdParts;
    try {
        auto* d=static_cast<ID3D12Device*>(s.context.deviceRaw());
        // Make the larger buffers first. If the machine cannot give them, the flock keeps what it has.
        const size_t descBytes=room*sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*4;
        auto instanceBuffer=buffer(d,room*sizeof(InstanceData)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* instanceMapped=mapForWriting(instanceBuffer.Get());
        ComPtr<ID3D12Resource> previousTransforms,tlas,tlasScratch,instances,compareInstanceBuffer,compareInstances;
        uint8_t* motionMapped=nullptr;
        uint8_t* rayMapped=nullptr;
        uint8_t* compareInstanceMapped=nullptr;
        uint8_t* compareRayMapped=nullptr;
        uint64_t tlasBytes=s.tlasBytes;
        if (s.compareInstanceBuffer) {
            compareInstanceBuffer=buffer(d,room*sizeof(InstanceData)*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            compareInstanceMapped=mapForWriting(compareInstanceBuffer.Get());
        }
        if (s.compareInstances) {
            compareInstances=buffer(d,descBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            compareRayMapped=mapForWriting(compareInstances.Get());
        }
        if (s.previousTransforms) {
            previousTransforms=buffer(d,4ull*room*sizeof(MotionRows),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            motionMapped=mapForWriting(previousTransforms.Get());
        }
        if (s.tlas) {
            ComPtr<ID3D12Device5> d5;check(d->QueryInterface(IID_PPV_ARGS(&d5)));
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
            ti.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            ti.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            ti.NumDescs=static_cast<UINT>(room);
            ti.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
            d5->GetRaytracingAccelerationStructurePrebuildInfo(&ti,&pre);
            tlas=buffer(d,pre.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            tlasScratch=buffer(d,pre.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            instances=buffer(d,descBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            rayMapped=mapForWriting(instances.Get());
            const auto tlasDesc=tlas->GetDesc();
            tlasBytes=d->GetResourceAllocationInfo(0,1,&tlasDesc).SizeInBytes;
        }
        // Nothing still in flight may read the buffers being replaced.
        if (!s.context.waitForGpu()) return false;
        s.instanceBuffer=std::move(instanceBuffer);
        s.instanceMapped=instanceMapped;
        if (compareInstanceBuffer) { s.compareInstanceBuffer=std::move(compareInstanceBuffer); s.compareInstanceMapped=compareInstanceMapped; }
        if (compareInstances) { s.compareInstances=std::move(compareInstances); s.compareRayMapped=compareRayMapped; }
        if (previousTransforms) { s.previousTransforms=std::move(previousTransforms); s.motionMapped=motionMapped; }
        if (tlas) {
            s.tlas=std::move(tlas);
            s.tlasScratch=std::move(tlasScratch);
            s.instances=std::move(instances);
            s.rayMapped=rayMapped;
            s.tlasBytes=tlasBytes;
            s.maxTlasInstances=room;
        }
        s.maxSceneInstances=room;
        s.baseSceneInstances+=addedPhysx;
        s.physxCapacity+=addedPhysx;
        s.workoutBirdCapacity=capacity;
        BOO_LOG_INFO("Workout","Room made for "+std::to_string(capacity)+" workout birds ("+std::to_string(room)+" instances).");
        return true;
    } catch (const std::exception& e) {
        BOO_LOG_WARN("Workout",std::string("Could not make room for ")+std::to_string(capacity)+" workout birds: "+e.what());
        return false;
    }
}
bool TrilogyMeshRenderer::reservePhysxBodies() {
    return impl_ && reserveWorkoutBirds(impl_->workoutBirdCapacity, true);
}
FlockFocus TrilogyMeshRenderer::flockFocus() const noexcept { return impl_ ? impl_->flockFocus : FlockFocus{}; }
bool TrilogyMeshRenderer::neuralMaterialDrawn() const noexcept { return impl_->neuralMaterialDrawn; }
bool TrilogyMeshRenderer::highPathExecuted() const noexcept { return impl_->highPathExecuted; }
uint32_t TrilogyMeshRenderer::verifiedHits() const {return impl_->hits;}
RendererFeatureState TrilogyMeshRenderer::pathTracingState() const noexcept {return impl_->pathTracingState;}
uint32_t TrilogyMeshRenderer::fallbackKits() const {return impl_->fallbacks;}
void TrilogyMeshRenderer::resetTemporalHistory() noexcept {
    impl_->dlssHistoryValid = false;
    impl_->nrd.resetHistory();
    impl_->reblurFrameCounter = 0;
    impl_->reblurPrevValid = false;
    impl_->accumValid = false;
}
CityDrawStats TrilogyMeshRenderer::lastCityDrawStats() const noexcept {return impl_->lastStats;}
bool TrilogyMeshRenderer::hasAllAuthoredSceneAssets() const noexcept {
    return impl_->fallbacks == 0 && !impl_->usedFallbackTexture;
}
}
