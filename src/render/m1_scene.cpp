#include "boo/m1_scene.hpp"
#include "boo/bmesh.hpp"

#include <algorithm>
#include <cmath>

namespace boo {

Bmesh buildTrilogyFallbackKit(uint32_t kit) {
    Bmesh m; m.proceduralFallback=true;
    auto box=[&](float x,float y,float z,float sx,float sy,float sz,uint32_t mat) {
        const uint32_t base=static_cast<uint32_t>(m.vertices.size()),first=static_cast<uint32_t>(m.indices.size());
        constexpr int faces[6][4]={{0,4,6,2},{1,3,7,5},{0,1,5,4},{2,6,7,3},{0,2,3,1},{4,5,7,6}};
        constexpr float normals[6][3]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
        for(int f=0;f<6;++f) {for(int j=0;j<4;++j) {int c=faces[f][j];BmeshVertex v{};
            v.positionMaterial[0]=x+((c&1)?sx:-sx);v.positionMaterial[1]=y+((c&2)?sy:-sy);v.positionMaterial[2]=z+((c&4)?sz:-sz);v.positionMaterial[3]=static_cast<float>(mat);
            for(int a=0;a<3;++a)v.normalPart[a]=normals[f][a];v.uvThicknessRoughness[2]=.03f;v.uvThicknessRoughness[3]=.3f;v.tangentSign[0]=1;v.tangentSign[3]=1;m.vertices.push_back(v);}
            for(uint32_t i:{0u,1u,2u,0u,2u,3u})m.indices.push_back(base+static_cast<uint32_t>(f)*4+i);}
        m.submeshes.push_back({first,36,mat,0,base,24});
    };
    if(kit==0) {box(0,-.3f,0,6,.3f,60,1);for(int i=0;i<12;++i){float z=-50+float(i)*10;box(-5,4,z,.25f,4,.3f,0);box(5,4,z,.25f,4,.3f,0);box(0,8,z,5,.2f,.3f,0);}}
    else if(kit==1) {box(0,-1,0,25,1,25,2);box(0,10,0,2,.8f,4,3);}
    else if(kit==2) {box(0,-4,0,900,4,900,1);for(int i=0;i<8;++i)box(0,130+float(i)*35,0,18,16,18,5);box(0,450,0,4,58,4,5);}
    else if(kit==3) {for(int i=0;i<5;++i){float a=float(i)*1.256637f;box(std::cos(a),std::sin(a),0,.65f,.5f,.08f,3);}}
    else {box(0,-.4f,10,35,.4f,65,1);for(int i=0;i<12;++i){float z=-28+float(i)*5;box(-6,5,z,.5f,5,.5f,0);box(6,5,z,.5f,5,.5f,0);}}
    return m;
}

M1CameraState evaluateTrilogyCamera(int64_t samplePosition,int32_t sampleRate,bool reducedFlash) noexcept {
    const float t=static_cast<float>(double(samplePosition)/std::max(1,sampleRate));M1CameraState c;
    auto smooth=[](float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);};
    auto smoother=[](float x){x=std::clamp(x,0.f,1.f);return x*x*x*(x*(x*6-15)+10);};
    c.emissivePulse=evaluateM1MusicalEnvelope(samplePosition,sampleRate,reducedFlash);c.fovYRadians=.86f;
    if(t<60) {
        // Act 1 (0..60s): Developing rail-bridge sequence — low off-axis rail perspective inside the riveted
        // Waibaidu-style steel through-truss (15s) emerging past the z = +6 portal onto the open river span
        // for an unobstructed panoramic reveal of the Huangpu River, barges, Bund bank & Lujiazui skyline (45s)
        const float sRev=smoother((t-22.f)/21.f);
        const float climb=smoother((t-54.f)/6.f);
        const float zTrack=-46.5f+0.92f*t+0.0085f*t*t;
        c.eye[0]=-0.52f*(1.f-sRev)+(-1.48f)*sRev;
        c.eye[1]=0.84f*(1.f-sRev)+2.38f*sRev+climb*12.f;
        c.eye[2]=zTrack;
        c.target[0]=0.42f*(1.f-sRev)+4.85f*sRev;
        c.target[1]=1.95f*(1.f-sRev)+11.65f*sRev+climb*16.f;
        c.target[2]=(zTrack+28.f)*(1.f-sRev)+82.f*sRev;
        c.fovYRadians=.82f+(.80f-.82f)*sRev;
        c.shotIndex=0;
    } else if(t<120) {
        // Act 2 (60..120s): Close aerial tracking of banking eVTOL in golden-hour cruise across Shenzhen Bay (75s)
        // transitioning smoothly to a civic-terrace three-quarter hover/descent framing at night (108s)
        const float u=t-60.f;
        const float sHover=smoother((u-21.f)/19.f);
        const float sTouch=smoother((u-43.f)/15.f);
        const float acX=(3.4f-0.28f*u)*(1.f-sHover)+(0.55f)*sHover;
        const float acY=(9.6f-0.05f*u+0.16f*std::sin(u*0.24f))*(1.f-sHover)+(5.45f-1.15f*sTouch+0.05f*std::sin(u*1.5f))*sHover;
        const float acZ=(18.2f-0.78f*u)*(1.f-sHover)+(-2.0f)*sHover;

        // Slightly elevated aerial chase framing at 75s looks across canopy, wings, forward-tilted rotors, bay canal & skyline crowns
        const float eyeCruise[3]={acX-5.8f, acY+1.65f, acZ-10.8f};
        const float tgtCruise[3]={acX+4.8f, acY+2.05f, acZ+22.0f};

        // Elevated civic-concourse three-quarter vantage at 108s frames full unclipped aircraft, Hero Pad, canal & skyline crowns
        const float eyeHover[3]={7.20f, 6.55f-0.35f*sTouch, -14.80f};
        const float tgtHover[3]={-1.40f, 6.90f-0.35f*sTouch, 16.00f};

        for(int i=0;i<3;++i) {
            c.eye[i]=eyeCruise[i]+(eyeHover[i]-eyeCruise[i])*sHover;
            c.target[i]=tgtCruise[i]+(tgtHover[i]-tgtCruise[i])*sHover;
        }
        c.fovYRadians=.76f+(.78f-.76f)*sHover;
        c.shotIndex=1;
    } else {
        // Act 3 (120..180s):
        // Beat A (120..141s): Close aerial inspection of Taipei 101's warm-lit 88F-91F observatory & true-scale 5.5 m gold TMD
        // Beat B (141..168s): Crane descent across Xinyi District onto the Sanctuary Rooftop Terrace for Glass Orchid bloom
        // Beat C (169..180s): Intentional finale composition — substantial left-foreground Orchid + complete right-center Taipei 101
        const float u=t-120.f;
        const float sA=smoother(u/21.f);
        const float sB=smoother((u-20.f)/25.5f);
        const float sC=smoother((u-49.f)/6.2f);
        const float holdDrift=smoother((u-54.f)/6.f)*.06f;

        const float eyeA[3]={14.60f+1.10f*sA, 33.45f+.35f*sA, 54.20f+.85f*sA};
        const float tgtA[3]={18.50f, 32.55f+.08f*sA, 60.00f};

        const float eyeB[3]={0.86f, 5.64f, -0.22f};
        const float tgtB[3]={0.00f, 5.48f, 5.50f};

        const float eyeC[3]={0.48f+holdDrift, 4.80f+.04f*holdDrift, -0.48f-.08f*holdDrift};
        const float tgtC[3]={7.55f, 16.95f, 46.00f};

        for(int i=0;i<3;++i) {
            const float abEye=eyeA[i]+(eyeB[i]-eyeA[i])*sB;
            const float abTgt=tgtA[i]+(tgtB[i]-tgtA[i])*sB;
            c.eye[i]=abEye+(eyeC[i]-abEye)*sC;
            c.target[i]=abTgt+(tgtC[i]-abTgt)*sC;
        }
        const float fovAB=.52f+(.74f-.52f)*sB;
        c.fovYRadians=fovAB+(.78f-fovAB)*sC;
        c.revealOpen01=smoother((t-148.f)/17.5f);
        c.shotIndex=2;
    }
    // Match adjacent camera endpoints with C1 smooth interpolation through cloud/mist cuts.
    if((t>58 && t<60)||(t>118 && t<120)) {
        const int boundary=t<60?60:120;const auto next=evaluateTrilogyCamera(int64_t(boundary)*sampleRate,sampleRate,reducedFlash);
        const float w=smooth((t-float(boundary-2))/2);
        for(int i=0;i<3;++i){c.eye[i]+=(next.eye[i]-c.eye[i])*w;c.target[i]+=(next.target[i]-c.target[i])*w;}
        c.fovYRadians+=(next.fovYRadians-c.fovYRadians)*w;
    }
    return c;
}

namespace {
constexpr double kPi = 3.14159265358979323846;

double smootherstep01(double x) noexcept {
    const double u = std::clamp(x, 0.0, 1.0);
    return u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
}

float lerpF(double a, double b, double t) noexcept {
    return static_cast<float>(a + (b - a) * t);
}
} // namespace

float evaluateM1MusicalEnvelope(int64_t samplePosition, int32_t sampleRate, bool reducedFlash) noexcept {
    if (reducedFlash) {
        return 0.0f;
    }
    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const double tSec = std::max(0.0, static_cast<double>(samplePosition) / sr);
    // Strictly smooth 1.0 Hz cosine (<= 2 Hz flash safety threshold), bounded to [0.0, 0.06]
    return static_cast<float>(0.06 * (0.5 - 0.5 * std::cos(2.0 * kPi * 1.0 * tSec)));
}

M1CameraState evaluateM1Camera(
    int64_t samplePosition,
    int32_t sampleRate,
    float moodIrisAngle,
    bool reducedFlash) noexcept {
    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const double tSec = std::max(0.0, static_cast<double>(samplePosition) / sr);
    const double t60 = std::fmod(tSec, 60.0);

    const float irisOffset = std::clamp((moodIrisAngle - 0.5f) * 0.2f, -0.15f, 0.15f);

    M1CameraState cam{};
    cam.up[0] = 0.0f;
    cam.up[1] = 1.0f;
    cam.up[2] = 0.0f;
    cam.emissivePulse = evaluateM1MusicalEnvelope(samplePosition, sampleRate, reducedFlash);

    if (t60 < 20.0) {
        // Shot 0 [0..20s): Taipei wet asphalt neon rain corridor low-angle glide
        const double s = smootherstep01(t60 / 20.0);
        cam.shotIndex = 0;
        cam.eye[0] = irisOffset * 0.4f;
        cam.eye[1] = lerpF(1.35, 1.85, s);
        cam.eye[2] = lerpF(-14.0, -6.5, s);

        cam.target[0] = 0.0f;
        cam.target[1] = lerpF(1.60, 3.20, s);
        cam.target[2] = lerpF(4.0, 6.0, s);

        cam.fovYRadians = 0.92f;
        cam.revealOpen01 = 0.0f;
    } else if (t60 < 40.0) {
        // Shot 1 [20..40s): Vertical architectural crane ascent up black-enamel monoliths
        const double s = smootherstep01((t60 - 20.0) / 20.0);
        cam.shotIndex = 1;
        cam.eye[0] = irisOffset * 0.5f;
        cam.eye[1] = lerpF(1.85, 7.40, s);
        cam.eye[2] = lerpF(-6.5, -4.2, s);

        cam.target[0] = 0.0f;
        cam.target[1] = lerpF(3.20, 8.20, s);
        cam.target[2] = lerpF(6.0, 5.5, s);

        cam.fovYRadians = lerpF(0.92, 0.78, s);
        cam.revealOpen01 = static_cast<float>(smootherstep01((t60 - 34.0) / 6.0) * 0.35);
    } else {
        // Shot 2 [40..60s): Enamel louvers part -> Glass Orchid opening reveal (peaks across 45..48s),
        // then cranes upward and pulls back across [49..60s) so the Glass Orchid sits in the lower-left
        // foreground while Taipei 101's 8 canted bamboo modules + golden TMD sphere tower into the night sky!
        const double s = smootherstep01((t60 - 40.0) / 20.0);
        const double finaleAscent = smootherstep01((t60 - 49.0) / 10.5);
        cam.shotIndex = 2;
        cam.eye[0] = static_cast<float>(std::sin(s * 0.55) * 1.4 - 1.35 * finaleAscent) + irisOffset;
        cam.eye[1] = lerpF(7.40, 6.45, s) + static_cast<float>(5.85 * finaleAscent);
        cam.eye[2] = lerpF(-4.2, -9.8, s) - static_cast<float>(3.40 * finaleAscent);

        cam.target[0] = static_cast<float>(0.35 * finaleAscent);
        cam.target[1] = lerpF(7.80, 14.60, finaleAscent);
        cam.target[2] = lerpF(5.50, 26.00, finaleAscent);

        cam.fovYRadians = lerpF(0.78, 0.88, s) + static_cast<float>(0.08 * finaleAscent);
        cam.revealOpen01 = lerpF(0.35, 1.0, smootherstep01((t60 - 40.0) / 7.0));
    }
    return cam;
}

LetterboxViewport computeLetterboxViewport16x9(uint32_t swapchainWidth, uint32_t swapchainHeight) noexcept {
    const double sw = static_cast<double>(std::max<uint32_t>(1, swapchainWidth));
    const double sh = static_cast<double>(std::max<uint32_t>(1, swapchainHeight));
    constexpr double kTargetAspect = 16.0 / 9.0;
    const double curAspect = sw / sh;

    LetterboxViewport vp{};
    if (std::abs(curAspect - kTargetAspect) <= 1e-4) {
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(sw);
        vp.height = static_cast<float>(sh);
    } else if (curAspect > kTargetAspect) {
        // Pillarbox (ultrawide window)
        const double fitW = std::round(sh * kTargetAspect);
        vp.width = static_cast<float>(fitW);
        vp.height = static_cast<float>(sh);
        vp.x = static_cast<float>(std::floor((sw - fitW) * 0.5));
        vp.y = 0.0f;
    } else {
        // Letterbox (16:10, 4:3, portrait window)
        const double fitH = std::round(sw / kTargetAspect);
        vp.width = static_cast<float>(sw);
        vp.height = static_cast<float>(fitH);
        vp.x = 0.0f;
        vp.y = static_cast<float>(std::floor((sh - fitH) * 0.5));
    }
    return vp;
}

Bmesh buildEnamelMicrocosmKit() {
    Bmesh m;
    m.proceduralFallback = false;

    auto addPatch = [&](uint32_t part, uint32_t mat, float rough, float thick, uint32_t nu, uint32_t nv, auto&& evalPos) {
        const uint32_t baseV = static_cast<uint32_t>(m.vertices.size());
        const uint32_t firstI = static_cast<uint32_t>(m.indices.size());
        const float eps = 1e-3f;

        for (uint32_t iv = 0; iv <= nv; ++iv) {
            const float v = static_cast<float>(iv) / static_cast<float>(nv);
            for (uint32_t iu = 0; iu <= nu; ++iu) {
                const float u = static_cast<float>(iu) / static_cast<float>(nu);
                float p[3]{}, pu[3]{}, pv[3]{};
                evalPos(u, v, p);
                evalPos(std::min(1.0f, u + eps), v, pu);
                evalPos(u, std::min(1.0f, v + eps), pv);
                if (u + eps > 1.0f) {
                    float pm[3]{};
                    evalPos(std::max(0.0f, u - eps), v, pm);
                    for (int c = 0; c < 3; ++c) pu[c] = p[c] - pm[c];
                } else {
                    for (int c = 0; c < 3; ++c) pu[c] -= p[c];
                }
                if (v + eps > 1.0f) {
                    float pm[3]{};
                    evalPos(u, std::max(0.0f, v - eps), pm);
                    for (int c = 0; c < 3; ++c) pv[c] = p[c] - pm[c];
                } else {
                    for (int c = 0; c < 3; ++c) pv[c] -= p[c];
                }

                float n[3] = {
                    pu[1] * pv[2] - pu[2] * pv[1],
                    pu[2] * pv[0] - pu[0] * pv[2],
                    pu[0] * pv[1] - pu[1] * pv[0]
                };
                float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl < 1e-6f) { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; nl = 1.0f; }
                for (int c = 0; c < 3; ++c) n[c] /= nl;

                float tl = std::sqrt(pu[0] * pu[0] + pu[1] * pu[1] + pu[2] * pu[2]);
                float t[3] = {1.0f, 0.0f, 0.0f};
                if (tl > 1e-6f) {
                    for (int c = 0; c < 3; ++c) t[c] = pu[c] / tl;
                }

                BmeshVertex vtx{};
                vtx.positionMaterial[0] = p[0];
                vtx.positionMaterial[1] = p[1];
                vtx.positionMaterial[2] = p[2];
                vtx.positionMaterial[3] = static_cast<float>(mat);
                vtx.normalPart[0] = n[0];
                vtx.normalPart[1] = n[1];
                vtx.normalPart[2] = n[2];
                vtx.normalPart[3] = static_cast<float>(part);
                vtx.uvThicknessRoughness[0] = u;
                vtx.uvThicknessRoughness[1] = v;
                vtx.uvThicknessRoughness[2] = thick;
                vtx.uvThicknessRoughness[3] = std::clamp(rough, 0.04f, 0.95f);
                vtx.tangentSign[0] = t[0];
                vtx.tangentSign[1] = t[1];
                vtx.tangentSign[2] = t[2];
                vtx.tangentSign[3] = 1.0f;
                m.vertices.push_back(vtx);
            }
        }

        const uint32_t stride = nu + 1;
        for (uint32_t iv = 0; iv < nv; ++iv) {
            for (uint32_t iu = 0; iu < nu; ++iu) {
                const uint32_t i0 = baseV + iv * stride + iu;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + stride;
                const uint32_t i3 = i2 + 1;
                m.indices.push_back(i0); m.indices.push_back(i2); m.indices.push_back(i1);
                m.indices.push_back(i1); m.indices.push_back(i2); m.indices.push_back(i3);
            }
        }

        const uint32_t vCount = (nu + 1) * (nv + 1);
        const uint32_t iCount = nu * nv * 6;
        m.submeshes.push_back({firstI, iCount, mat, part, baseV, vCount});
    };

    auto addTorus = [&](uint32_t part, uint32_t mat, float rough, float cx, float cy, float cz,
                        float majorR, float minorR, bool verticalXY, uint32_t nu = 28, uint32_t nv = 12) {
        addPatch(part, mat, rough, minorR * 1.5f, nu, nv, [&](float u, float v, float out[3]) {
            const float a = u * static_cast<float>(2.0 * kPi);
            const float b = v * static_cast<float>(2.0 * kPi);
            const float r = majorR + minorR * std::cos(b);
            if (verticalXY) {
                out[0] = cx + r * std::cos(a);
                out[1] = cy + r * std::sin(a);
                out[2] = cz + minorR * std::sin(b);
            } else {
                out[0] = cx + r * std::cos(a);
                out[1] = cy + minorR * std::sin(b);
                out[2] = cz + r * std::sin(a);
            }
        });
    };

    // 1. Hall of Mirrors Colonnade (part 200..207): 8 curved concave obsidian-chrome mirror stelae + 24k gold collars
    //    Arranged around the Glass Orchid (0, 5.85, 5.50) leaving the south-central camera sightline wide open.
    static constexpr float kMirrorAngles[8] = {
        -2.28f, -1.68f, -1.08f, -0.46f, 0.46f, 1.08f, 1.68f, 2.28f
    };
    for (uint32_t i = 0; i < 8; ++i) {
        const float ang = kMirrorAngles[i];
        const float rad = 4.35f;
        const float cx = std::sin(ang) * rad;
        const float cz = 5.50f + std::cos(ang) * rad;
        const float faceYaw = ang + static_cast<float>(kPi); // Concave mirror faces inward toward Glass Orchid

        addPatch(200u + i, 12u, 0.045f, 0.08f, 14, 16, [&](float u, float v, float out[3]) {
            const float s = (u - 0.5f) * 1.18f;
            const float curveZ = 0.16f * (s * s);
            const float taper = 1.0f - 0.14f * v;
            const float lx = s * taper;
            const float lz = curveZ;
            const float ca = std::cos(faceYaw), sa = std::sin(faceYaw);
            out[0] = cx + lx * ca + lz * sa;
            out[1] = 2.66f + v * 5.75f;
            out[2] = cz - lx * sa + lz * ca;
        });
        addTorus(200u + i, 8u, 0.16f, cx, 2.74f, cz, 0.62f, 0.048f, false, 20, 8);
        addTorus(200u + i, 8u, 0.16f, cx, 8.38f, cz, 0.52f, 0.042f, false, 20, 8);
    }

    // 2. Sculpted Ivory Hydroxyapatite Enamel Tooth-Crown Vaults (part 210..215, mat 20)
    //    6 smooth anatomic molar-crown cusps forming a luminous living amphitheater around the sanctuary pool.
    static constexpr float kCuspAngles[6] = {-2.15f, -1.32f, -0.58f, 0.58f, 1.32f, 2.15f};
    for (uint32_t i = 0; i < 6; ++i) {
        const float ang0 = kCuspAngles[i];
        addPatch(210u + i, 20u, 0.14f, 0.09f, 18, 16, [&](float u, float v, float out[3]) {
            const float a = ang0 + (u - 0.5f) * 0.64f;
            const float arch = std::sin(static_cast<float>(kPi) * u);
            const float crownProfile = std::sin(static_cast<float>(kPi) * std::min(1.0f, v * 0.92f));
            const float r = 4.95f - 0.95f * crownProfile * (0.55f + 0.45f * arch);
            const float cuspPeak = 4.35f + 1.55f * arch;
            out[0] = std::sin(a) * r;
            out[1] = 2.64f + v * cuspPeak;
            out[2] = 5.50f + std::cos(a) * r;
        });
    }

    // 3. Neural Velvet SIREN Caustic Ribbons (part 216..219, mat 21)
    //    4 sweeping sinusoidal silk-glass canopy ribbons overhead driven by the 64-layer Tensor Core SIREN field.
    for (uint32_t i = 0; i < 4; ++i) {
        const float zBase = 3.45f + static_cast<float>(i) * 1.45f;
        const float yPeak = 7.55f + 0.42f * std::sin(static_cast<float>(i) * 1.2f);
        addPatch(216u + i, 21u, 0.18f, 0.03f, 28, 10, [&](float u, float v, float out[3]) {
            const float xSpan = (u - 0.5f) * 8.6f;
            const float arch = std::sin(static_cast<float>(kPi) * u);
            const float wave = 0.24f * std::sin(u * static_cast<float>(4.0 * kPi) + static_cast<float>(i) * 0.9f);
            const float twist = (v - 0.5f) * 0.62f;
            out[0] = xSpan;
            out[1] = 3.55f + (yPeak - 3.55f) * arch + wave + twist * 0.28f;
            out[2] = zBase + twist + 0.35f * std::cos(u * static_cast<float>(2.0 * kPi));
        });
    }

    // 4. Madame N Art-Deco Brass Iris Proscenium (part 220..223, mat 8) & Symbiotic Bioluminescent Micro-Flora (part 224..227, mat 22)
    //    Local coordinates centered at (0, 0, 0) for parts 220..223 so TrilogyMeshRenderer::transform can rotate/dilate cleanly around (0, 5.75, 5.50)!
    static constexpr float kIrisRadii[4] = {2.15f, 2.62f, 3.08f, 3.52f};
    for (uint32_t i = 0; i < 4; ++i) {
        addTorus(220u + i, 8u, 0.15f, 0.0f, 0.0f, 0.0f, kIrisRadii[i], 0.048f + 0.008f * static_cast<float>(i), true, 36, 10);
    }
    static constexpr float kFloraClusters[4][3] = {
        {-2.35f, 2.68f, 4.35f},
        { 2.35f, 2.68f, 4.35f},
        {-3.10f, 2.68f, 6.45f},
        { 3.10f, 2.68f, 6.45f}
    };
    for (uint32_t i = 0; i < 4; ++i) {
        const float fx = kFloraClusters[i][0];
        const float fy = kFloraClusters[i][1];
        const float fz = kFloraClusters[i][2];
        for (uint32_t b = 0; b < 3; ++b) {
            const float offA = static_cast<float>(b) * 2.094395f + static_cast<float>(i) * 0.5f;
            const float bx = fx + std::cos(offA) * 0.38f;
            const float bz = fz + std::sin(offA) * 0.34f;
            const float domeR = 0.28f + 0.06f * static_cast<float>(b);
            const float domeH = 0.34f + 0.12f * static_cast<float>(b);
            addPatch(224u + i, 22u, 0.22f, 0.05f, 16, 10, [&](float u, float v, float out[3]) {
                const float a = u * static_cast<float>(2.0 * kPi);
                const float el = v * static_cast<float>(0.5 * kPi);
                const float r = domeR * std::cos(el);
                out[0] = bx + r * std::cos(a);
                out[1] = fy + domeH * std::sin(el);
                out[2] = bz + r * std::sin(a);
            });
        }
    }

    // 5. Everyday Dawn Care Reveal (part 230..235): Brass Vanity Mirror Arch (230..231) + Crystal Bristle Colonnade (232..235)
    addTorus(230u, 8u, 0.14f, 0.0f, 5.75f, 7.65f, 3.35f, 0.085f, true, 40, 12);
    addTorus(231u, 12u, 0.06f, 0.0f, 5.75f, 7.75f, 3.18f, 0.055f, true, 40, 10);
    for (uint32_t c = 0; c < 4; ++c) {
        const float side = (c < 2u) ? -1.0f : 1.0f;
        const float colIdx = static_cast<float>(c & 1u);
        const float bx = side * (2.05f + colIdx * 0.62f);
        const float bz = 4.65f + colIdx * 0.55f;
        const float topH = 5.45f + 0.35f * colIdx;
        addPatch(232u + c, (c & 1u) ? 4u : 3u, 0.05f, 0.06f, 14, 12, [&](float u, float v, float out[3]) {
            const float a = u * static_cast<float>(2.0 * kPi);
            const float tipRound = (v > 0.88f) ? std::cos((v - 0.88f) / 0.12f * static_cast<float>(0.5 * kPi)) : 1.0f;
            const float rad = (0.11f - 0.025f * v) * std::max(0.08f, tipRound);
            const float sway = 0.08f * side * (v * v);
            out[0] = bx + sway + rad * std::cos(a);
            out[1] = 2.66f + v * (topH - 2.66f);
            out[2] = bz + rad * std::sin(a);
        });
    }

    if (!m.vertices.empty()) {
        for (int c = 0; c < 3; ++c) {
            m.bounds[c] = m.vertices[0].positionMaterial[c];
            m.bounds[c + 3] = m.vertices[0].positionMaterial[c];
        }
        for (const auto& v : m.vertices) {
            for (int c = 0; c < 3; ++c) {
                m.bounds[c] = std::min(m.bounds[c], v.positionMaterial[c]);
                m.bounds[c + 3] = std::max(m.bounds[c + 3], v.positionMaterial[c]);
            }
        }
    }
    return m;
}

M1CameraState evaluateShowCamera(
    ShowVisualMode mode,
    int64_t samplePosition,
    int32_t sampleRate,
    float moodIrisAngle,
    bool reducedFlash,
    float motionScale) noexcept {
    if (mode == ShowVisualMode::M1Slice60s || mode == ShowVisualMode::LegacyM0) {
        return evaluateM1Camera(samplePosition, sampleRate, moodIrisAngle, reducedFlash);
    }
    if (mode == ShowVisualMode::M2Trilogy180s) {
        return evaluateTrilogyCamera(samplePosition, sampleRate, reducedFlash);
    }

    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const double tSec = std::max(0.0, static_cast<double>(samplePosition) / sr);
    const double mScale = static_cast<double>(std::clamp(motionScale, 0.15f, 1.0f));
    const float irisShift = std::clamp((moodIrisAngle - 0.5f) * 1.10f, -0.55f, 0.55f) * static_cast<float>(mScale);

    M1CameraState cam{};
    cam.up[0] = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;
    cam.emissivePulse = evaluateM1MusicalEnvelope(samplePosition, sampleRate, reducedFlash);

    if (mode == ShowVisualMode::SoloRT) {
        // 55s Hall of Mirrors DXR loop: smooth orbital sweep across 8 concave mirror stelae & Glass Orchid
        const double u = std::fmod(tSec, 55.0) / 55.0;
        const double phase = u * 2.0 * kPi;
        cam.shotIndex = 1;
        cam.eye[0] = static_cast<float>(2.05 * mScale * std::sin(phase)) + irisShift * 0.25f;
        cam.eye[1] = static_cast<float>(5.05 + 0.42 * mScale * std::sin(phase * 2.0));
        cam.eye[2] = static_cast<float>(-0.35 + 0.55 * mScale * (1.0 - std::cos(phase)));
        cam.target[0] = static_cast<float>(0.45 * mScale * std::cos(phase));
        cam.target[1] = 5.65f;
        cam.target[2] = 5.65f;
        cam.fovYRadians = 0.78f;
        cam.revealOpen01 = 1.0f;
        return cam;
    }

    if (mode == ShowVisualMode::SoloCUDA) {
        // 50s Particle Mutiny loop: wide-to-medium dynamic crane around 100k+ morphing double-helix <-> crown torus
        const double u = std::fmod(tSec, 50.0) / 50.0;
        const double phase = u * 2.0 * kPi;
        cam.shotIndex = 4;
        cam.eye[0] = static_cast<float>(2.95 * mScale * std::sin(phase));
        cam.eye[1] = static_cast<float>(5.35 + 0.85 * mScale * (0.5 - 0.5 * std::cos(phase)));
        cam.eye[2] = static_cast<float>(-1.65 + 0.65 * mScale * std::sin(phase * 0.5));
        cam.target[0] = 0.0f;
        cam.target[1] = 5.55f;
        cam.target[2] = 5.50f;
        cam.fovYRadians = 0.84f;
        cam.revealOpen01 = 1.0f;
        return cam;
    }

    if (mode == ShowVisualMode::SoloTensor) {
        // 50s Neural Velvet loop: low-angle caustic glide beneath undulating Tensor Core SIREN ribbons & pool
        const double u = std::fmod(tSec, 50.0) / 50.0;
        const double phase = u * 2.0 * kPi;
        cam.shotIndex = 2;
        cam.eye[0] = static_cast<float>(1.85 * mScale * std::sin(phase));
        cam.eye[1] = static_cast<float>(4.15 + 0.55 * mScale * (0.5 - 0.5 * std::cos(phase)));
        cam.eye[2] = static_cast<float>(-0.55 + 0.70 * mScale * (0.5 - 0.5 * std::cos(phase)));
        cam.target[0] = static_cast<float>(0.25 * mScale * std::cos(phase));
        cam.target[1] = 5.85f;
        cam.target[2] = 5.60f;
        cam.fovYRadians = 0.82f;
        cam.revealOpen01 = 1.0f;
        return cam;
    }

    if (mode == ShowVisualMode::SoloNPU) {
        // 45s Madame N Intel NPU stage loop (midpoint_ecology_reveal at 25.0s)
        const double t45 = std::fmod(tSec, 45.0);
        const double sEco = smootherstep01((t45 - 20.0) / 7.0);
        const double phase = (t45 / 45.0) * 2.0 * kPi;
        cam.shotIndex = 3;
        cam.eye[0] = static_cast<float>(1.25 * mScale * std::sin(phase)) + irisShift * 0.85f;
        cam.eye[1] = lerpF(5.45, 4.65, sEco) + irisShift * 0.22f;
        cam.eye[2] = lerpF(0.10, -0.85, sEco);
        cam.target[0] = irisShift * 0.30f;
        cam.target[1] = lerpF(5.72, 4.85, sEco);
        cam.target[2] = 5.50f;
        cam.fovYRadians = lerpF(0.74, 0.82, sEco);
        cam.revealOpen01 = lerpF(0.55, 1.0, sEco);
        return cam;
    }

    // FullFilm360s: 7-Act 360.000s (17,280,000-sample) Glass Orchid master film
    auto evalFilmAt = [&](double t) noexcept -> M1CameraState {
        M1CameraState c{};
        c.up[0] = 0.0f; c.up[1] = 1.0f; c.up[2] = 0.0f;
        c.emissivePulse = cam.emissivePulse;

        if (t < 45.0) {
            // Act 0 [0..45s): Taipei 101 88F-91F TMD lantern crane descent -> Xinyi rain -> Enamel Crown scale reveal @ 36.000s
            const double sDesc = smootherstep01(t / 28.0);
            const double sScale = smootherstep01((t - 31.0) / 8.0);
            c.shotIndex = 0;
            c.eye[0] = lerpF(15.20, 1.15, sDesc) + static_cast<float>(-0.55 * sScale) + irisShift * 0.20f;
            c.eye[1] = lerpF(33.55, 5.55, sDesc) + static_cast<float>(-0.45 * sScale);
            c.eye[2] = lerpF(54.60, 0.05, sDesc) + static_cast<float>(-0.95 * sScale);
            c.target[0] = lerpF(18.50, 0.00, sDesc);
            c.target[1] = lerpF(32.55, 5.45, sDesc) + static_cast<float>(-0.20 * sScale);
            c.target[2] = lerpF(60.00, 5.50, sDesc);
            c.fovYRadians = lerpF(0.54, 0.78, sDesc) + static_cast<float>(0.05 * sScale);
            c.revealOpen01 = static_cast<float>(smootherstep01((t - 28.0) / 10.0));
        } else if (t < 100.0) {
            // Act 1 [45..100s): Hall of Mirrors DXR orbital tracking around the 8 mirror stelae & Glass Orchid
            const double u = (t - 45.0) / 55.0;
            const double phase = u * 2.0 * kPi;
            c.shotIndex = 1;
            c.eye[0] = static_cast<float>(2.10 * mScale * std::sin(phase)) + irisShift * 0.25f;
            c.eye[1] = static_cast<float>(5.05 + 0.40 * mScale * std::sin(phase * 2.0));
            c.eye[2] = static_cast<float>(-0.40 + 0.55 * mScale * (1.0 - std::cos(phase)));
            c.target[0] = static_cast<float>(0.42 * mScale * std::cos(phase));
            c.target[1] = 5.65f;
            c.target[2] = 5.65f;
            c.fovYRadians = 0.78f;
            c.revealOpen01 = 1.0f;
        } else if (t < 150.0) {
            // Act 2 [100..150s): Neural Velvet Tensor Core SIREN caustic canopy & reflecting pool glide
            const double u = (t - 100.0) / 50.0;
            const double phase = u * 2.0 * kPi;
            c.shotIndex = 2;
            c.eye[0] = static_cast<float>(1.85 * mScale * std::sin(phase)) + irisShift * 0.20f;
            c.eye[1] = static_cast<float>(4.15 + 0.55 * mScale * (0.5 - 0.5 * std::cos(phase)));
            c.eye[2] = static_cast<float>(-0.55 + 0.70 * mScale * (0.5 - 0.5 * std::cos(phase)));
            c.target[0] = static_cast<float>(0.25 * mScale * std::cos(phase));
            c.target[1] = 5.85f;
            c.target[2] = 5.60f;
            c.fovYRadians = 0.82f;
            c.revealOpen01 = 1.0f;
        } else if (t < 195.0) {
            // Act 3 [150..195s): Madame N Intel NPU iris stage -> midpoint_ecology_reveal @ 175.000s (02:55.000)
            const double u = t - 150.0;
            const double sEco = smootherstep01((u - 20.0) / 7.5);
            const double phase = (u / 45.0) * 2.0 * kPi;
            c.shotIndex = 3;
            c.eye[0] = static_cast<float>(1.25 * mScale * std::sin(phase)) + irisShift * 0.85f;
            c.eye[1] = lerpF(5.45, 4.62, sEco) + irisShift * 0.22f;
            c.eye[2] = lerpF(0.10, -0.88, sEco);
            c.target[0] = irisShift * 0.30f;
            c.target[1] = lerpF(5.72, 4.82, sEco);
            c.target[2] = 5.50f;
            c.fovYRadians = lerpF(0.74, 0.82, sEco);
            c.revealOpen01 = 1.0f;
        } else if (t < 245.0) {
            // Act 4 [195..245s): Particle Mutiny 100k+ CUDA double-helix <-> crown torus choreography
            const double u = (t - 195.0) / 50.0;
            const double phase = u * 2.0 * kPi;
            c.shotIndex = 4;
            c.eye[0] = static_cast<float>(2.95 * mScale * std::sin(phase)) + irisShift * 0.20f;
            c.eye[1] = static_cast<float>(5.35 + 0.85 * mScale * (0.5 - 0.5 * std::cos(phase)));
            c.eye[2] = static_cast<float>(-1.65 + 0.65 * mScale * std::sin(phase * 0.5));
            c.target[0] = 0.0f;
            c.target[1] = 5.55f;
            c.target[2] = 5.50f;
            c.fovYRadians = 0.84f;
            c.revealOpen01 = 1.0f;
        } else if (t < 275.0) {
            // Act 5 [245..275s): Signal Hijack NVENC/NVDEC broadcast intercept over night-lit Shenzhen Bay & eVTOL
            // Strictly ignores moodIrisAngle because allow_director_modulation = false in film_360s.json!
            const double u = t - 245.0;
            const double sHover = smootherstep01((u - 8.0) / 14.0);
            const double acX = (2.6 - 0.22 * u) * (1.0 - sHover) + 0.55 * sHover;
            const double acY = (8.8 - 0.05 * u) * (1.0 - sHover) + 4.85 * sHover;
            const double acZ = (14.5 - 0.65 * u) * (1.0 - sHover) + (-2.0) * sHover;
            c.shotIndex = 5;
            c.eye[0] = lerpF(acX - 5.6, 6.85, sHover);
            c.eye[1] = lerpF(acY + 1.55, 6.25, sHover);
            c.eye[2] = lerpF(acZ - 10.2, -14.20, sHover);
            c.target[0] = lerpF(acX + 4.4, -1.25, sHover);
            c.target[1] = lerpF(acY + 1.85, 6.55, sHover);
            c.target[2] = lerpF(acZ + 20.0, 15.50, sHover);
            c.fovYRadians = 0.78f;
            c.revealOpen01 = 1.0f;
        } else {
            // Act 6 [275..360s): Silicon Rebellion 4-pillar synthesis -> final_everyday_care_reveal @ 342.000s -> 10s architectural move -> Dawn end card
            const double u = t - 275.0;
            const double sCare = smootherstep01((t - 335.5) / 6.3);
            const double sFinale = smootherstep01((t - 342.0) / 10.0);
            const double holdDrift = smootherstep01((t - 352.0) / 8.0) * 0.025;
            const double orbit = (std::min(u, 60.5) / 60.5) * 2.0 * kPi;

            const double synEye[3] = {
                2.25 * mScale * std::sin(orbit) + irisShift * 0.25,
                5.15 + 0.55 * mScale * (0.5 - 0.5 * std::cos(orbit)),
                -0.85 + 0.45 * mScale * std::sin(orbit * 0.5)
            };
            const double synTgt[3] = {0.35 * mScale * std::cos(orbit), 5.75, 5.65};

            // 342.0s: Intimate 1:1 metric architectural three-quarter view inside the sunlit Taipei penthouse bathroom
            const double careEye[3] = {-0.58, 3.88, 4.38};
            const double careTgt[3] = {-0.06, 3.84, 6.38};

            // 352.0s..360.0s: 10-second slider pullback & subtle pan framing left plaster title zone, full oak vanity, mirror & Taipei 101 window
            const double finEye[3] = {-0.36 + holdDrift * 0.6, 4.08 + holdDrift * 0.2, 3.48 - holdDrift * 0.5};
            const double finTgt[3] = {-0.28, 4.06, 6.55};

            c.shotIndex = 6;
            for (int i = 0; i < 3; ++i) {
                const double e01 = synEye[i] + (careEye[i] - synEye[i]) * sCare;
                const double t01 = synTgt[i] + (careTgt[i] - synTgt[i]) * sCare;
                c.eye[i] = static_cast<float>(e01 + (finEye[i] - e01) * sFinale);
                c.target[i] = static_cast<float>(t01 + (finTgt[i] - t01) * sFinale);
            }
            c.fovYRadians = lerpF(lerpF(0.80, 0.58, sCare), 0.66, sFinale);
            c.revealOpen01 = 1.0f;
        }
        return c;
    };

    const double tClamped = std::clamp(tSec, 0.0, 360.0);
    M1CameraState out = evalFilmAt(tClamped);

    // Smooth C1 boundary blend across multi-act cuts
    static constexpr double kCuts[6] = {45.0, 100.0, 150.0, 195.0, 245.0, 275.0};
    for (double cut : kCuts) {
        if (tClamped > cut - 1.8 && tClamped < cut) {
            const M1CameraState next = evalFilmAt(cut + 0.001);
            const double w = smootherstep01((tClamped - (cut - 1.8)) / 1.8);
            for (int i = 0; i < 3; ++i) {
                out.eye[i] = lerpF(out.eye[i], next.eye[i], w);
                out.target[i] = lerpF(out.target[i], next.target[i], w);
            }
            out.fovYRadians = lerpF(out.fovYRadians, next.fovYRadians, w);
            break;
        }
    }
    return out;
}

} // namespace boo
