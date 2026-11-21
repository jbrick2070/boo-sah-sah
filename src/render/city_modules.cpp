#include "boo/city_modules.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

namespace boo {
namespace {
using Point = std::array<float, 3>;
using Vec2 = std::array<float, 2>;

uint32_t mixBits(uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

Point subtract(Point a, Point b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Point cross(Point a, Point b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
Point unit(Point v) {
    const float length = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    for (float& component : v) component /= length;
    return v;
}

// Closed four-sided volumes have actual soffits and back faces for reflections.
// Each material gets a contiguous index range, usable by both raster and BLAS.
struct ModuleBuilder {
    struct Surface {
        std::vector<BmeshVertex> vertices;
        std::vector<uint32_t> indices;
    };
    uint32_t part;
    std::map<std::array<uint32_t,4>, Surface> surfaces;
    uint32_t detailTier=0,detailSolid=0;
    Point facadeMeters{};

    void quad(uint32_t mat, Point a, Point b, Point c, Point d) {
        auto& surface = surfaces[{part,mat,detailTier,detailTier ? detailSolid : 0u}];
        const uint32_t first = static_cast<uint32_t>(surface.vertices.size());
        const Point tangent = unit(subtract(b, a));
        const Point normal = unit(cross(subtract(b, a), subtract(c, a)));
        const std::array<Point,4> positions{a,b,c,d};
        auto tileSpan = [&](Point edge) {
            if (facadeMeters[0] == 0.f) return 1.f;
            return std::hypot(edge[0]*facadeMeters[0],edge[1]*facadeMeters[1],edge[2]*facadeMeters[2]);
        };
        const float uSpan = tileSpan(subtract(b,a)), vSpan = tileSpan(subtract(d,a));
        for (size_t i = 0; i < positions.size(); ++i) {
            BmeshVertex vertex{};
            std::copy_n(positions[i].begin(),3,vertex.positionMaterial);
            std::copy_n(normal.begin(),3,vertex.normalPart);
            std::copy_n(tangent.begin(),3,vertex.tangentSign);
            vertex.positionMaterial[3] = static_cast<float>(mat);
            vertex.normalPart[3] = static_cast<float>(part);
            vertex.tangentSign[3] = 1.f;
            vertex.uvThicknessRoughness[0] = i == 1 || i == 2 ? uSpan : 0.f;
            vertex.uvThicknessRoughness[1] = i >= 2 ? vSpan : 0.f;
            vertex.uvThicknessRoughness[2] = .015f;
            vertex.uvThicknessRoughness[3] = mat == 2 ? .58f : .29f;
            surface.vertices.push_back(vertex);
        }
        for (uint32_t i : {0u,1u,2u,0u,2u,3u}) surface.indices.push_back(first + i);
    }

    // x/z center, x/z width, height. Offset and taper between stories.
    using Level = std::array<float,5>;
    void loft(uint32_t mat, Level base, Level roof) {
        if(detailTier) ++detailSolid;
        auto ring = [](Level p) {
            const float x=p[0], z=p[1], w=p[2]*.5f, d=p[3]*.5f, y=p[4];
            return std::array<Point,4>{{{x-w,y,z-d},{x-w,y,z+d},{x+w,y,z+d},{x+w,y,z-d}}};
        };
        const auto a = ring(base), b = ring(roof);
        quad(mat,a[3],a[2],a[1],a[0]);
        quad(mat,b[0],b[1],b[2],b[3]);
        for (size_t i=0; i<4; ++i) {
            const size_t j=(i+1)%4;
            quad(mat,a[i],a[j],b[j],b[i]);
        }
    }
    void box(uint32_t mat, float x, float z, float w, float d, float bottom, float top) {
        loft(mat,{x,z,w,d,bottom},{x,z,w,d,top});
    }
    void tank(uint32_t mat,float x,float z,float w,float d,float bottom,float top) {
        if(detailTier) ++detailSolid;
        constexpr std::array<Vec2,8> outline{{{-.21f,-.5f},{-.5f,-.21f},{-.5f,.21f},{-.21f,.5f},
                                              {.21f,.5f},{.5f,.21f},{.5f,-.21f},{.21f,-.5f}}};
        std::array<Point,8> lo,hi;
        for(size_t i=0;i<8;++i) {
            lo[i]={x+w*outline[i][0],bottom,z+d*outline[i][1]};
            hi[i]={lo[i][0],top,lo[i][2]};
        }
        for(size_t i=0;i<8;++i) quad(mat,lo[i],lo[(i+1)%8],hi[(i+1)%8],hi[i]);
        for(const auto& q : std::array<std::array<int,4>,3>{{{0,1,2,3},{0,3,4,7},{4,5,6,7}}}) {
            quad(mat,hi[q[0]],hi[q[1]],hi[q[2]],hi[q[3]]);
            quad(mat,lo[q[3]],lo[q[2]],lo[q[1]],lo[q[0]]);
        }
    }
    Bmesh finish() const {
        Bmesh mesh;
        mesh.bounds = {1.e20f,1.e20f,1.e20f,-1.e20f,-1.e20f,-1.e20f};
        for (const auto& [key,surface] : surfaces) {
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.submeshes.push_back({static_cast<uint32_t>(mesh.indices.size()),
                static_cast<uint32_t>(surface.indices.size()),key[1],key[0] | (key[2]<<16),base,
                static_cast<uint32_t>(surface.vertices.size())});
            for (const auto& v : surface.vertices) {
                for (size_t axis=0; axis<3; ++axis) {
                    mesh.bounds[axis] = std::min(mesh.bounds[axis],v.positionMaterial[axis]);
                    mesh.bounds[axis+3] = std::max(mesh.bounds[axis+3],v.positionMaterial[axis]);
                }
                mesh.vertices.push_back(v);
            }
            for (const auto index : surface.indices) mesh.indices.push_back(base + index);
        }
        return mesh;
    }
};

// Fixed-size, street-facing bays. The shell is recessed behind the ground-floor
// arcade so the supporting columns and daylight under the occupied floor read
// in silhouette. These dimensions are baked into the UVs as metres.
constexpr uint32_t kWalkup = 5, kShop = 6;
constexpr float kFrontage = 8.4f, kPlotDepth = 17.4f;
float streetHeight(uint32_t style) { return style == kWalkup ? 16.4f : 10.f; }

Bmesh streetFront(uint32_t style, uint32_t lod) {
    ModuleBuilder b{62u,{}};
    const float height=streetHeight(style);
    b.facadeMeters={kFrontage,height,kPlotDepth};
    auto block=[&](uint32_t mat,float x,float z,float w,float depth,float y0,float y1) {
        b.box(mat,x/kFrontage,z/kPlotDepth,w/kFrontage,depth/kPlotDepth,y0/height,y1/height);
    };
    constexpr float front=-kPlotDepth/2, inner=front+3.64f, back=kPlotDepth/2;
    // A corner bay at each side supports the slab. The shop glazing sits 3.64 m
    // behind the front edge, with 3.33 m clear to the underside of the soffit.
    block(13,0,(inner+back)*.5f,kFrontage-.12f,back-inner,0,3.60f);
    for(float x:{-3.92f,0.f,3.92f}) block(2,x,front+.37f,.38f,.42f,0,3.34f);
    block(28,0,0,kFrontage,kPlotDepth,3.33f,3.62f);
    const uint32_t skin=style==kWalkup ? 28u : 24u;
    block(13,0,.85f,kFrontage-.18f,kPlotDepth-1.76f,3.60f,height);
    for (int i=0; i<(style==kWalkup ? 4 : 2); ++i) {
        const float floor=3.60f+i*3.20f;
        block(skin,0,front+.73f,kFrontage-.12f,1.46f,floor,floor+.25f);
        for(float x:{-3.9f,3.9f}) block(skin,x,front+.91f,.36f,1.28f,floor+.24f,floor+3.17f);
        // Balcony aprons and corner returns, with room to see into the recess.
        block(skin,-1.92f,front+.16f,3.62f,.22f,floor+.24f,floor+1.02f);
    }
    block(skin,0,.02f,kFrontage,kPlotDepth-.04f,height-.13f,height+.18f);
    if (style==kWalkup) {
        block(24,1.50f,3.5f,3.2f,3.8f,height+.18f,height+1.72f);
        b.tank(11,-1.7f/kFrontage,3.6f/kPlotDepth,1.7f/kFrontage,1.8f/kPlotDepth,
               (height+.18f)/height,(height+1.50f)/height);
    } else {
        // The contemporary shop row has an open roof stair and a stepped sign
        // parapet. Keep the ornamental historic Dihua frontage out of Xinyi.
        block(skin,0,front+.36f,5.6f,.56f,height+.18f,height+.64f);
        block(skin,0,front+.36f,3.0f,.56f,height+.64f,height+.99f);
    }
    if(lod<2) {
        b.detailTier=2;
        block(15,.92f,inner-.07f,4.4f,.12f,2.28f,2.88f);
        block(11,3.14f,front+.39f,.74f,.52f,3.65f,4.73f);
    }
    if(lod==0) {
        b.detailTier=1;
        for(int floor=0;floor<(style==kWalkup ? 4 : 2);++floor) {
            const float y=3.60f+3.20f*floor;
            for(float x:{.25f,1.26f,2.27f}) block(11,x,front+.88f,.04f,.10f,y+.85f,y+2.78f);
            block(11,-2.45f,front+.87f,1.15f,.62f,y+1.04f,y+1.66f);
        }
    }
    return b.finish();
}

// Major changes in profile persist in all three LODs. Only the finer projections
// disappear, so a direct seek across a threshold gives the same skyline silhouette.
Bmesh makeModule(uint32_t kit, uint32_t style, uint32_t glazing, uint32_t lod) {
    if (kit==2 && (style==kWalkup || style==kShop)) return streetFront(style,lod);
    ModuleBuilder b{kit==1 ? 60u : 61u, {}};
    const uint32_t trim = kit==1 ? (style==1 ? 6u : 16u) : 15u;
    if (style == 9) {
        // A folded clerestory sits over each pair of beams on the existing
        // elevated platform. The open front looks directly onto the guideway.
        for(int bay=0;bay<5;++bay) {
            const float x=-.4f+.2f*bay;
            b.box(11,x,.44f,.016f,.07f,0,.60f);
            b.loft(11,{x,.41f,.017f,.07f,.08f},{x+.055f,-.32f,.017f,.07f,.60f});
            const Point a{x-.095f,.56f,-.47f},c{x+.095f,1.0f,.48f};
            const Point p{x+.095f,.94f,-.47f},q{x-.095f,.62f,.48f};
            const auto above=[](Point v) {v[1]+=.055f;return v;};
            b.quad(11,a,p,c,q); b.quad(11,above(q),above(c),above(p),above(a));
            for(const auto& edge:std::array<std::array<Point,2>,4>{{{a,p},{p,c},{c,q},{q,a}}})
                b.quad(11,edge[1],edge[0],above(edge[0]),above(edge[1]));
            b.box(15,x+.09f,0,.012f,.86f,.62f,.93f);
        }
        return b.finish();
    } else if (style == 4) {
        // Ground extension under the outlying blocks of the original compact set.
        b.box(2,0,0,1,1,0,1);
        return b.finish();
    } else if (style == 7) {
        if(kit==2) {
            // An inhabited service stair and a pair of steel water cisterns on
            // the shared roof. The tank profiles survive the distant skyline.
            b.box(2,0,0,.80f,.77f,0,.16f);
            b.box(glazing,-.22f,.08f,.31f,.58f,.14f,.78f);
            b.box(2,-.22f,.08f,.35f,.61f,.77f,.84f);
            b.tank(11,.16f,-.14f,.24f,.28f,.15f,.62f);
            b.tank(11,.18f,.19f,.23f,.26f,.15f,.51f);
        } else {
            // Open transfer truss tying together two high-rise cores. Sky and
            // neighboring towers remain visible through the top of the frame.
            for(float x:{-.29f,.29f}) {
                b.loft(glazing,{x,-.02f,.24f,.74f,0},{x,.08f,.18f,.57f,1});
            }
            b.box(glazing,0,.08f,.76f,.57f,.74f,.97f);
            b.box(trim,0,-.21f,.74f,.035f,.73f,.78f);
        }
    } else if (style == 8) {
        if(kit==2) {
            // Narrow parapet blades and a shadowed mechanical floor below a
            // warm top slab distinguish commercial offices from the walk-ups.
            b.box(glazing,0,0,.76f,.71f,0,.34f);
            for(float x:{-.30f,-.11f,.11f,.30f})
                b.loft(11,{x,.02f,.07f,.62f,.33f},{x,.11f,.065f,.38f,1.02f});
            b.box(trim,0,.11f,.72f,.40f,.91f,.97f);
        } else {
            // Sloped plant screens step towards the bay in three distinct fins.
            for(int i=0;i<3;++i) {
                const float x=-.26f+.26f*i, top=.62f+.18f*i;
                b.loft(glazing,{x,0,.24f,.80f,0},{x,.16f,.20f,.42f,top});
                b.box(11,x,.16f,.21f,.43f,top,top+.035f);
            }
        }
    } else if (style == 3) {
        // A raised mechanical lantern. The open middle retains sky between the fins.
        b.box(glazing,0,.09f,.86f,.62f,0,.43f);
        b.loft(glazing,{-.29f,.03f,.24f,.64f,.40f},{-.29f,.12f,.16f,.46f,1});
        b.loft(glazing,{.29f,.03f,.24f,.64f,.40f},{.29f,.12f,.16f,.46f,.81f});
        b.box(trim,-.29f,.12f,.17f,.47f,.94f,1.01f);
        b.box(trim,.29f,.12f,.17f,.47f,.75f,.82f);
        b.detailTier=2;
        if(lod<2) b.box(11,0,.09f,.44f,.22f,.54f,.60f);
    } else if (kit == 1 && style == 0) {
        // Two slender offices share a trading podium and an occupied sky lobby.
        b.box(glazing,0,0,.96f,.96f,0,.27f);
        b.loft(glazing,{-.24f,0,.42f,.82f,.26f},{-.27f,.08f,.32f,.72f,1.15f});
        b.loft(glazing,{.24f,.02f,.41f,.86f,.26f},{.26f,.12f,.32f,.62f,.96f});
        b.box(glazing,0,.09f,.39f,.49f,.65f,.73f);
        b.box(trim,0,.09f,.40f,.50f,.65f,.664f);
        b.box(trim,-.27f,.08f,.33f,.73f,1.13f,1.151f);
        b.detailTier=2;
        if(lod<2) b.box(11,0,.10f,.37f,.36f,.45f,.48f);
    } else if (kit == 1 && style == 1) {
        // Tapered, offset waterfront sail with a rising ridge.
        b.loft(glazing,{0,0,1,1,0},{-.04f,.02f,.94f,.89f,.57f});
        b.loft(glazing,{-.04f,.02f,.94f,.89f,.57f},{.09f,.07f,.62f,.74f,.91f});
        b.loft(glazing,{.09f,.07f,.62f,.74f,.91f},{.24f,.08f,.24f,.55f,1.17f});
        b.loft(trim,{.382f,-.012f,.024f,.61f,.90f},{.365f,.08f,.020f,.53f,1.17f});
        b.detailTier=2;
        if(lod<2) b.box(11,-.04f,.02f,.95f,.90f,.565f,.58f);
    } else if (kit == 2 && style == 0) {
        // Staggered apartment wings and a rooftop service core.
        b.box(glazing,0,0,.99f,.98f,0,.38f);
        b.box(glazing,-.20f,.025f,.57f,.89f,.375f,1.025f);
        b.box(glazing,.261f,.10f,.35f,.65f,.375f,.76f);
        b.box(trim,-.20f,.025f,.59f,.91f,1.012f,1.027f);
        b.box(2,-.26f,.16f,.23f,.27f,1.025f,1.10f);
        for(int floor=0; floor<=8; floor+=(1<<lod)) {
            b.detailTier=(floor%4==0) ? 0u : (floor%2==0 ? 2u : 1u);
            const float y=.43f+.52f*(float(floor)/8.f);
            b.box(2,-.20f,-.362f,.59f,.17f,y,y+.012f);
        }
    } else if (kit == 2 && style == 1) {
        // Narrow service spine beside setback offices with a rooftop lantern.
        b.loft(glazing,{0,0,.98f,.98f,0},{0,0,.98f,.98f,.61f});
        b.box(glazing,-.05f,.03f,.80f,.82f,.605f,.88f);
        b.box(glazing,-.07f,.09f,.63f,.65f,.875f,1.025f);
        b.box(2,.28f,.25f,.22f,.25f,.59f,1.08f);
        b.box(trim,-.07f,.09f,.64f,.66f,1.012f,1.029f);
        b.detailTier=2;
        if(lod<2) b.box(11,-.05f,.03f,.82f,.84f,.859f,.878f);
    } else {
        // A sequence of terraces gives the glass top a directional silhouette.
        b.box(glazing,0,0,.98f,.98f,0,.48f);
        b.box(glazing,.035f,.065f,.88f,.79f,.476f,.74f);
        b.box(glazing,.07f,.13f,.75f,.60f,.738f,.94f);
        b.box(glazing,.12f,.19f,.54f,.43f,.937f,1.06f);
        b.box(trim,.07f,.13f,.77f,.62f,.923f,.941f);
        b.detailTier=2;
        if(lod<2) b.box(2,.035f,.065f,.90f,.82f,.723f,.741f);
    }
    if(lod == 0 && style < 3) {
        b.detailTier=1;
        // Recessed ground-floor arcade behind street columns and a shade canopy.
        b.box(2,0,-.48f,.99f,.14f,.115f,.133f);
        for(int i=0;i<5;++i) b.box(11,-.44f+.22f*float(i),-.527f,.032f,.034f,0,.115f);
    }
    return b.finish();
}

// The BMESH exporter splits vertices at UV seams and flat normals. Group connected
// triangles by their shared *positions* before measuring a building's footprint.
struct PointHash {
    size_t operator()(const Point& p) const noexcept {
        size_t hash=0;
        for(float v:p) hash=hash*131u+std::hash<float>{}(v);
        return hash;
    }
};
struct Shell {
    std::vector<uint32_t> faces;
    std::vector<Point> points;
};
std::vector<Shell> connectedShells(const Bmesh& mesh,const BmeshSubmesh& sub) {
    const uint32_t count = sub.indexCount / 3;
    std::vector<uint32_t> parent(count);
    std::iota(parent.begin(), parent.end(),0u);
    auto root = [&](uint32_t n) {
        while(n!=parent[n]) { parent[n]=parent[parent[n]]; n=parent[n]; }
        return n;
    };
    std::unordered_map<Point,uint32_t,PointHash> owners;
    owners.reserve(count);
    for(uint32_t t=0;t<count;++t) {
        for(uint32_t j=0;j<3;++j) {
            const float* v=mesh.vertices[mesh.indices[sub.firstIndex+3*t+j]].positionMaterial;
            auto [it,inserted]=owners.try_emplace({v[0],v[1],v[2]},t);
            if(!inserted) parent[root(t)]=root(it->second);
        }
    }
    std::vector<Shell> shells;
    std::vector<uint32_t> shellAtRoot(count,UINT32_MAX);
    for(uint32_t t=0;t<count;++t) {
        auto& index=shellAtRoot[root(t)];
        if(index==UINT32_MAX) {index=static_cast<uint32_t>(shells.size());shells.emplace_back();}
        shells[index].faces.push_back(t);
    }
    for(const auto& [point,t]:owners) shells[shellAtRoot[root(t)]].points.push_back(point);
    return shells;
}
struct Footprint {
    float x=0,z=0,width=0,depth=0,cosine=1,sine=0;
};

// All supported caps are small convex footprints. Their minimum-area oriented
// rectangle has a side parallel to a hull edge. Test each pair and keep the most
// nearly east-facing basis on ties. Sorted points make that choice reproducible.
Footprint fitFootprint(std::vector<Vec2> p) {
    std::sort(p.begin(),p.end());
    p.erase(std::unique(p.begin(),p.end()),p.end());
    Footprint best{};
    if(p.size()<3 || p.size()>48) return best;
    float bestArea=std::numeric_limits<float>::max();
    for(size_t i=0;i<p.size();++i) for(size_t j=i+1;j<p.size();++j) {
        const float dx=p[j][0]-p[i][0],dz=p[j][1]-p[i][1];
        const float len=std::hypot(dx,dz);
        if(len<.0001f) continue;
        float c=dx/len,s=dz/len;
        if(std::abs(s)>std::abs(c)) {const float t=c;c=-s;s=t;}
        if(c<0) {c=-c;s=-s;}
        float u0=1.e20f,u1=-1.e20f,v0=u0,v1=u1;
        for(const auto& v:p) {
            const float u=c*v[0]+s*v[1], w=-s*v[0]+c*v[1];
            u0=std::min(u0,u);u1=std::max(u1,u);v0=std::min(v0,w);v1=std::max(v1,w);
        }
        const float area=(u1-u0)*(v1-v0);
        if(area<bestArea*.99999f || (std::abs(area-bestArea)<=area*.00001f && std::abs(s)<std::abs(best.sine))) {
            const float u=(u1+u0)*.5f,v=(v1+v0)*.5f;
            best={c*u-s*v,s*u+c*v,u1-u0,v1-v0,c,s};
            bestArea=area;
        }
    }
    return best;
}
struct Site {
    Footprint plot;
    float bottom,height;
    uint32_t part,material,style,seed;
};

void addXinyiStreetFronts(std::vector<Site>& sites) {
    struct Row { float x, z; unsigned bays; bool facesNorth; };
    // The original podiums start north of Z=21.8. The rows occupy their own
    // parcels on either side of the Z=18.5 avenue, with a passage at each end.
    // The short western row stays beyond the edge of the orchid terrace.
    constexpr Row rows[]={{-3.3f,20.65f,9,false}, {8.85f,20.65f,8,false},
                          {-3.3f,16.24f,8,true}, {-3.3f,34.05f,4,true},
                          {19.45f,34.05f,7,true}, {-12.3f,7.6f,5,false}};
    constexpr float scale=.08f;
    for(const auto& row:rows) {
        for(unsigned bay=0;bay<row.bays;++bay) {
            const float x=row.x+.75f*bay;
            const uint32_t seed=mixBits(static_cast<uint32_t>(std::lround((x+64.f)*100.f))^
                                        mixBits(static_cast<uint32_t>(std::lround(row.z*100.f))));
            const uint32_t style=seed%3u ? kWalkup : kShop;
            Footprint plot{(x-18.5f)/scale,(row.z-60.f)/scale,kFrontage,kPlotDepth,
                           row.facesNorth ? -1.f : 1.f,0.f};
            sites.push_back({plot,.01f/scale,streetHeight(style),2,13,style,seed});
        }
    }
}

void addLegacyDistricts(std::vector<Site>& sites,uint32_t kit) {
    const bool taipei=kit==2;
    const float scale=taipei ? .08f : 1.f;
    const float originX=taipei ? 18.5f : 0.f,originZ=taipei ? 60.f : 0.f;
    const float north=taipei ? 207.f : 158.f;
    const float east=taipei ? 165.f : 135.f;
    auto addSite=[&](float x,float z,float width,float depth,float y,float height,
                     float yaw,uint32_t style,uint32_t mat,uint32_t seed) {
        Footprint p{(x-originX)/scale,(z-originZ)/scale,width/scale,depth/scale,std::cos(yaw),std::sin(yaw)};
        sites.push_back({p,y/scale,height/scale,x<0 ? 3u : 4u,mat,style,seed});
    };
    addSite(0,(north+580.f)*.5f,1150.f,580.f-north,-2.1f,1.f,0,4,2,0);
    for(float side:{-1.f,1.f})
        addSite(side*(east+575.f)*.5f,(north-75.f)*.5f,575.f-east,north+75.f,-2.1f,1.f,0,4,2,0);

    // Clip against the full bounds of the original set. The slight skew across
    // rows leaves street channels, with concentrations of taller buildings off axis.
    constexpr float pitch=23.f;
    for(int row=-2;row<=24;++row) for(int col=-24;col<=24;++col) {
        const float x=pitch*(float(col)+.24f*float(row&1));
        const float z=pitch*float(row);
        if(z<north+pitch*.5f && std::abs(x)<east+pitch*.5f) continue;
        if(std::abs(x)>std::min(545.f,205.f+std::max(z,0.f)*.76f)) continue;
        const uint32_t seed=mixBits(kit*7919u ^ mixBits(uint32_t(row+17)) ^ mixBits(uint32_t(col+31)*379u));
        auto noise=[&](uint32_t channel) {return float(mixBits(seed+channel)&65535u)/65535.f;};
        if(noise(1)<.16f) continue;
        const float az=std::max(z,0.f);
        const float lane=std::sin(x*.021f+az*.013f);
        const float district=std::max(0.f,std::cos(x*.021f-az*.018f))*
            std::max(0.f,std::cos(az*.027f+float(kit)));
        float height=(9.f+22.f*district+12.f*noise(5)+.04f*az)*(0.72f+.60f*noise(6));
        if(taipei && std::abs(x-18.5f-az*.22f)<30.f) height*=.55f;
        const float width=9.f+8.f*noise(7),depth=9.f+8.f*noise(8);
        const uint32_t glazing[]={5,13,14};
        addSite(x+(noise(2)-.5f)*3.f,z+lane*2.f,width,depth,-1.10f,height,
                (noise(3)-.5f)*.20f,seed%3u,glazing[mixBits(seed+4)%3u],seed);
    }
}
} // namespace

namespace {
constexpr float dockX=kParcelDockPosition[0], dockZ=kParcelDockPosition[2], dockTop=3.55f;
constexpr float parcelHalfHeight=.22f;

float deliveryEase(float t, float start, float end) noexcept {
    const float u=std::clamp((t-start)/(end-start),0.f,1.f);
    return u*u*(3.f-2.f*u);
}
} // namespace

namespace {
// The middle and upper Shenzhen landings line up with the tower's skybridges.
constexpr std::array<float,4> kGalleryStops{.18f,12.70f,25.36f,12.70f};
constexpr std::array<float,4> kQuayStops{.45f,12.05f,20.30f,12.05f};
}

float cityLiftTravel(bool observatory, float routeSeconds, float motionScale) noexcept {
    const auto& stops=observatory?kGalleryStops:kQuayStops;
    const float clock=std::max(routeSeconds,0.f)*std::clamp(motionScale,0.f,1.f)
                      +(observatory?3.65f:5.5f);
    const float cycle=std::fmod(clock,40.f);
    const auto step=std::min(static_cast<unsigned>(cycle/10.f),3u);
    const float t=std::clamp((cycle-10.f*float(step)-2.f)/7.4f,0.f,1.f);
    return std::lerp(stops[step],stops[(step+1)%stops.size()],t*t*t*(t*(t*6.f-15.f)+10.f));
}

Bmesh buildCityLift(uint32_t part) {
    ModuleBuilder b{part,{}};
    const bool gallery=part==kObservatoryLiftShaft || part==kObservatoryLiftCab;
    const uint32_t trim=gallery?8u:11u, light=gallery?15u:6u;
    if (part==kObservatoryLiftShaft || part==kWaterfrontLiftRails) {
        const auto& stops=gallery?kGalleryStops:kQuayStops;
        const float head=stops[2]+4.15f;
        for (float side:{-1.f,1.f}) {
            b.box(11,side*1.65f,.83f,.12f,.16f,0.f,head);
            b.box(38,side*1.65f,.84f,.22f,.10f,0.f,head);
            if (gallery) {
                b.box(11,side*1.65f,.83f,.38f,.42f,-.55f,0.f);
                b.box(11,side*1.65f,.83f,.22f,.27f,head,30.90f);
            }
        }
        // Leave a clear view through the gallery curtain wall. Short horizontal
        // brackets anchor the exterior rails to the tower at each landing.
        for (unsigned i=0;i<3;++i) {
            const float y=stops[i];
            b.box(11,0.f,1.24f,3.66f,.89f,y-.13f,y+.04f);
            for (float side:{-1.f,1.f}) {
                b.box(trim,side*1.76f,.30f,.14f,1.91f,y-.14f,y+.06f);
                b.box(light,side*1.76f,-.66f,.23f,.09f,y+.18f,y+.38f);
            }
        }
        b.box(11,0.f,.41f,3.67f,2.1f,head-.32f,head);
        b.box(trim,0.f,.40f,2.36f,.93f,head,head+.20f);
    } else {
        // Low-iron windows show the handrails, bench and two riders. The lit
        // lintel gives the moving cab a distinct silhouette at skyline scale.
        b.tank(11,0.f,0.f,3.06f,2.16f,0.f,.26f);
        b.tank(trim,0.f,0.f,3.14f,2.22f,.25f,.38f);
        b.box(38,0.f,.91f,2.86f,.14f,.36f,3.10f);
        b.box(15,0.f,.82f,2.48f,.05f,.95f,2.77f);
        b.box(11,0.f,.72f,2.62f,.34f,.56f,.73f);
        for (float side:{-1.f,1.f}) {
            b.box(trim,side*1.43f,0.f,.11f,1.95f,.36f,.50f);
            b.box(40,side*1.40f,-.02f,.033f,1.72f,.50f,2.94f);
            b.box(trim,side*1.40f,-.91f,.11f,.12f,.36f,3.13f);
            b.box(11,side*1.39f,-.03f,.09f,1.75f,1.26f,1.32f);
            b.box(40,side*.694f,-.93f,1.32f,.034f,.48f,2.93f);
            const float x=side*.64f, height=side<0?1.94f:2.09f;
            b.tank(38,x,.13f,.49f,.35f,.38f,height-.39f);
            b.tank(38,x,.12f,.30f,.29f,height-.38f,height-.08f);
        }
        b.box(11,0.f,-.957f,.045f,.041f,.42f,3.00f);
        b.tank(trim,0.f,0.f,3.20f,2.25f,2.98f,3.22f);
        b.box(light,0.f,-1.09f,2.42f,.045f,3.01f,3.12f);
    }
    return b.finish();
}

Bmesh buildParcelHardware(uint32_t part) {
    ModuleBuilder b{part,{}};
    if (part==kParcelCase) {
        // Reusable seed case. A pair of pale cinch straps encloses the colored shell.
        b.tank(26,0,0,.72f,.54f,-parcelHalfHeight,parcelHalfHeight);
        for(float x:{-.22f,.22f}) b.box(28,x,0,.055f,.55f,-.221f,.224f);
        b.box(11,0,0,.73f,.55f,.11f,.14f);
        b.box(15,0,-.274f,.19f,.012f,-.04f,.075f);
        b.box(11,0,0,.13f,.07f,.22f,.26f);
    } else if (part==kParcelLine) {
        // Unit length paid out from the belly. The lower end meets the case latch.
        b.box(28,0,0,.019f,.019f,-1.f,0.f);
    } else {
        // Fixed depot mounted on the sanctuary's north parapet.
        b.box(11,0,0,.78f,.31f,2.79f,2.86f);
        b.tank(11,0,0,.32f,.29f,2.86f,3.46f);
        b.tank(24,0,0,1.01f,.75f,3.46f,dockTop);
        for(float x:{-.46f,.46f}) b.box(16,x,0,.045f,.52f,dockTop,dockTop+.016f);
    }
    return b.finish();
}

std::array<float,3> sanctuaryCourierPosition(float filmTime,float motionScale,float* heading) noexcept {
    const float a=(filmTime-170.f)*(6.28318531f/45.f)*motionScale;
    if(heading) *heading=std::atan2(6.5f*std::sin(a),-3.5f*std::cos(a));
    const float hold=deliveryEase(filmTime,163.f,169.f)*(1.f-deliveryEase(filmTime,177.f,182.f));
    return {std::lerp(10.5f-6.5f*std::cos(a),dockX,hold),
            std::lerp(6.85f+.20f*motionScale*std::sin(a),6.25f,hold),
            std::lerp(8.1f-3.5f*std::sin(a),dockZ,hold)};
}

ParcelPose sanctuaryParcelPose(float filmTime, float motionScale) noexcept {
    float heading=0.f;
    const auto drone=sanctuaryCourierPosition(filmTime,motionScale,&heading);
    const float mount=drone[1]-.12f;
    const float rest=dockTop+parcelHalfHeight;
    const float lower=deliveryEase(filmTime,169.f,173.5f);
    const float caseY=std::lerp(mount-.36f,rest,lower);
    const bool landed=filmTime>=173.5f;
    const float payout=std::max(.018f,mount-caseY-.26f);
    const float takeup=1.f-deliveryEase(filmTime,174.3f,177.f);
    // Align the case with the dock before lowering, then leave it on the platform.
    const float caseYaw=std::lerp(heading,0.f,deliveryEase(filmTime,163.f,169.f));
    return {{landed?dockX:drone[0],caseY,landed?dockZ:drone[2]},
            {drone[0],mount,drone[2]},std::max(.018f,payout*takeup),caseYaw};
}

Bmesh buildCityAircraft(uint32_t part) {
    ModuleBuilder b{part,{}};
    // Forward is +Z. Both vehicles use closed hulls so the navigation lights
    // and silhouettes also appear in reflective glass and hardware shadows.
    if (part==kSkyPlane || part==kShenzhenRelay) {
        // Fuselage cross-sections taper to the radome and the tail cone.
        const std::array<std::array<float,3>,6> sections{{
            {-2.8f,.045f,.055f},{-2.f,.20f,.20f},{-.8f,.31f,.29f},
            {1.35f,.31f,.29f},{2.05f,.25f,.22f},{2.52f,.035f,.035f}}};
        for(size_t i=1;i<sections.size();++i) {
            auto point=[&](size_t slice,int k) -> Point {
                const auto& s=sections[slice];
                const float angle=float(k%8)*.78539816f;
                return {s[1]*std::cos(angle),s[2]*std::sin(angle),s[0]};
            };
            for(int k=0;k<8;++k)
                b.quad(28,point(i-1,k),point(i-1,k+1),point(i,k+1),point(i,k));
            for(const auto& q : std::array<std::array<int,4>,3>{{{0,1,2,3},{0,3,4,7},{4,5,6,7}}}) {
                if(i==1) b.quad(28,point(0,q[3]),point(0,q[2]),point(0,q[1]),point(0,q[0]));
                if(i+1==sections.size()) b.quad(28,point(i,q[0]),point(i,q[1]),point(i,q[2]),point(i,q[3]));
            }
        }
        auto foil=[&](std::array<Point,4> top) {
            auto bottom=top;
            for(auto& p:bottom)p[1]-=.055f;
            b.quad(28,top[0],top[1],top[2],top[3]);
            b.quad(11,bottom[3],bottom[2],bottom[1],bottom[0]);
            for(int k=0;k<4;++k)b.quad(11,top[k],bottom[k],bottom[(k+1)%4],top[(k+1)%4]);
            if (part==kShenzhenRelay) {
                // Inlaid stripes follow the sweep on both wing surfaces.
                auto stripe = [&](int edge, float chord, float offset) {
                    Point p{};
                    for (int axis=0;axis<3;++axis)
                        p[axis]=std::lerp(top[edge][axis],top[3-edge][axis],chord);
                    p[1]+=offset;
                    return p;
                };
                b.quad(15,stripe(0,.16f,.008f),stripe(1,.16f,.008f),
                          stripe(1,.30f,.008f),stripe(0,.30f,.008f));
                b.quad(15,stripe(0,.30f,-.063f),stripe(1,.30f,-.063f),
                          stripe(1,.16f,-.063f),stripe(0,.16f,-.063f));
            }
        };
        for(float side:{-1.f,1.f}) {
            std::array<Point,4> wing{{{side*.23f,0,.60f},{side*2.9f,.15f,-.65f},
                                     {side*2.93f,.15f,-.98f},{side*.23f,0,-.53f}}};
            std::array<Point,4> tail{{{side*.12f,.18f,-1.72f},{side*1.10f,.23f,-2.26f},
                                     {side*1.1f,.23f,-2.48f},{side*.12f,.18f,-2.24f}}};
            if(side<0.f){std::reverse(wing.begin(),wing.end());std::reverse(tail.begin(),tail.end());}
            foil(wing);foil(tail);
            b.tank(11,side*1.02f,-.02f,.34f,.75f,-.32f,-.08f);
            b.box(side<0.f?16u:6u,side*2.91f,-.81f,.13f,.29f,.095f,.17f);
            // Narrow cabin windows distinguish the airliner in dusk profile.
            for(int seat=0;seat<9;++seat)
                b.box(15,side*.285f,-.68f+float(seat)*.235f,.038f,.10f,.055f,.14f);
            b.box(11,side*.21f,1.78f,.09f,.38f,.11f,.18f);
        }
        b.loft(28,{0,-1.96f,.12f,1.05f,.12f},{0,-2.42f,.07f,.42f,.96f});
        b.box(15,0,2.13f,.22f,.16f,-.13f,-.07f);
        if (part==kShenzhenRelay) {
            // A pale transport and a smaller brass survey jet read against the bay
            // as two distinct silhouettes. Their shared pose keeps the hanging line attached.
            for (float side : {-1.f,1.f}) {
                b.tank(28,side*1.72f,-.28f,.30f,.77f,-.32f,-.08f);
                b.box(15,side*.30f,-.24f,.025f,1.80f,-.06f,-.032f);
            }
            // The survey jet has a cropped delta wing and a bubble canopy.
            b.part=kShenzhenRelayReceiver;
            constexpr float cx = .58f, cy = -.80f, cz = -6.45f;
            b.loft(8,{cx,cz,.30f,2.66f,cy-.14f},{cx,cz+.13f,.39f,2.98f,cy+.03f});
            b.loft(8,{cx,cz+.13f,.39f,2.98f,cy+.03f},{cx,cz-.07f,.19f,1.85f,cy+.24f});
            b.tank(12,cx,cz+.39f,.31f,1.10f,cy+.20f,cy+.36f);
            b.loft(8,{cx,cz-1.02f,.09f,.71f,cy+.14f},{cx,cz-1.34f,.07f,.32f,cy+.70f});
            for (float side : {-1.f,1.f}) {
                std::array<Point,4> delta{{{cx+side*.12f,cy,cz+.63f},{cx+side*1.91f,cy+.12f,cz-.80f},
                                           {cx+side*1.87f,cy+.12f,cz-1.07f},{cx+side*.12f,cy,cz-.86f}}};
                if (side<0.f) std::reverse(delta.begin(),delta.end());
                foil(delta);
                b.box(15,cx+side*1.88f,cz-.97f,.09f,.15f,cy+.11f,cy+.16f);
                b.tank(11,cx+side*.41f,cz-.48f,.23f,1.18f,cy-.23f,cy-.06f);
            }
            // A sagging striped tube catches the light as the paired silhouettes bank.
            b.part=kShenzhenRelayHose;
            const Point anchor{.10f,-.14f,-1.67f}, nose{cx,cy+.035f,cz+1.58f};
            std::array<Point,8> previous{};
            for (int j=0; j<=16; ++j) {
                const float t=float(j)/16.f;
                Point at{};
                for (int k=0;k<3;++k) at[k]=anchor[k]+t*(nose[k]-anchor[k]);
                at[1]-=.63f*4.f*t*(1.f-t);
                std::array<Point,8> next{};
                for (int k=0;k<8;++k) {
                    const float angle=k*.78539816f;
                    next[k]={at[0]+.064f*std::cos(angle),at[1]+.064f*std::sin(angle),at[2]};
                }
                if (j) for (int k=0;k<8;++k)
                    b.quad(j%4==0 ? 15u : 28u,previous[k],next[k],next[(k+1)%8],previous[(k+1)%8]);
                previous=next;
            }
        }
    } else {
        b.tank(11,0,0,.55f,1.12f,-.15f,.16f);
        b.loft(28,{0,.01f,.52f,.95f,.13f},{0,-.08f,.36f,.68f,.26f});
        b.box(15,0,.52f,.26f,.08f,-.09f,.02f);
        for(float z:{-.40f,.40f}) {
            b.box(11,0,z,1.52f,.13f,-.045f,.045f);
            for(float side:{-1.f,1.f}) {
                const float x=side*.77f, radius=.32f;
                // Guarded lift fans. The open centers read against the sky.
                for(int i=0;i<12;++i) {
                    auto ring=[&](int index,float r,float y) -> Point {
                        const float a=float(index%12)*.52359878f;
                        return {x+r*std::cos(a),y,z+r*std::sin(a)};
                    };
                    b.quad(28,ring(i,radius,.08f),ring(i+1,radius,.08f),
                        ring(i+1,radius,-.055f),ring(i,radius,-.055f));
                    b.quad(11,ring(i,radius,.08f),ring(i,radius-.065f,.08f),
                        ring(i+1,radius-.065f,.08f),ring(i+1,radius,.08f));
                }
                b.box(side<0.f?16u:6u,x+side*.28f,z,.09f,.14f,-.02f,.08f);
            }
        }
    }
    return b.finish();
}

RelayFlyby relayFlyby(float localSeconds,float motionScale) noexcept {
    const float angle=(localSeconds-42.f)*.032f*std::clamp(motionScale,0.f,1.f);
    const float sa=std::sin(angle), ca=std::cos(angle);
    auto ease=[](float value) {
        const float t=std::clamp(value,0.f,1.f);
        return t*t*t*(t*(6.f*t-15.f)+10.f);
    };
    const float cut=ease((localSeconds-33.f)/5.f)*(1.f-ease((localSeconds-48.f)/5.f));
    return {{-17.f+62.f*sa,37.4f+1.4f*sa,46.f+14.f*(1.f-ca)},
            std::atan2(62.f*ca,14.f*sa),-.021f*ca,.12f+.09f*sa,cut};
}

namespace {
// An oval cross section by center and two radial axes. The eight sample points
// keep the leading edge rounded and the underside closed when a wing banks.
struct PlumageSection { Point center, across, up; };

void featherVolume(ModuleBuilder& b, uint32_t mat, std::initializer_list<PlumageSection> sections) {
    constexpr float d=.70710678f;
    constexpr Vec2 octagon[]={{1,0},{d,d},{0,1},{-d,d},{-1,0},{-d,-d},{0,-1},{d,-d}};
    std::array<Point,8> previous{};
    for (auto it=sections.begin(); it!=sections.end(); ++it) {
        std::array<Point,8> ring{};
        for (size_t k=0; k<ring.size(); ++k)
            for (size_t axis=0; axis<3; ++axis)
                ring[k][axis]=it->center[axis]+octagon[k][0]*it->across[axis]+octagon[k][1]*it->up[axis];
        if (it!=sections.begin()) {
            for (size_t k=0; k<ring.size(); ++k) {
                const size_t j=(k+1)%ring.size();
                b.quad(mat,previous[k],previous[j],ring[j],ring[k]);
            }
        }
        if (it==sections.begin() || it+1==sections.end()) {
            auto cap=ring;
            if (it==sections.begin()) std::reverse(cap.begin(),cap.end());
            b.quad(mat,cap[0],cap[1],cap[2],cap[3]);
            b.quad(mat,cap[0],cap[3],cap[4],cap[7]);
            b.quad(mat,cap[4],cap[5],cap[6],cap[7]);
        }
        previous=ring;
    }
}

// Station along a body pointing down +Z. Sections can rise or shift sideways
// for the folded neck, hooked bill and diverging tail of the two species.
PlumageSection birdSection(float z,float y,float width,float depth,float x=0.f) {
    return {{x,y,z},{width,0,0},{0,depth,0}};
}

void birdWing(ModuleBuilder& b,uint32_t bodyPart,bool egret,bool left) {
    const float side=left ? -1.f : 1.f;
    auto station=[side](float x,float y,float z,float chord,float thickness) -> PlumageSection {
        return {{side*x,y,z},{0,0,-side*chord},{0,thickness,0}};
    };
    const uint32_t mat=egret ? 33u : 26u;
    b.part=bodyPart+(left ? 1u : 2u);
    featherVolume(b,mat,{
        station(.045f,0,-.018f,.12f,.037f),
        station(.16f,.013f,-.015f,.17f,.038f),
        station(.30f,.028f,egret ? -.023f : .008f,egret ? .185f : .153f,.032f),
        station(.435f,.023f,egret ? -.015f : .030f,.143f,.025f),
        station(kBirdWristX+.015f,.002f,.012f,.108f,.018f)});

    b.part=bodyPart+(left ? 3u : 4u);
    featherVolume(b,mat,{
        station(kBirdWristX-.029f,0,.013f,.114f,.020f),
        station(.60f,.004f,.014f,.107f,.018f),
        station(.675f,.008f,egret ? -.010f : -.001f,.071f,.014f),
        station(.714f,.010f,-.017f,.022f,.007f)});
    // Primaries spread like fingers on a kite. The egret has a rounder hand:
    // its broad overlapping vanes show short notches when seen against the sky.
    for (int i=0;i<5;++i) {
        const float u=float(i)*.25f;
        const float rootX=.68f-.096f*u;
        const float rootZ=.034f-.116f*u;
        const float tipX=egret ? .783f+.083f*std::sin(2.85f*u)-.024f*u
                               : .829f+.013f*std::sin(4.0f*u)-.118f*u;
        const float tipZ=egret ? .020f-.182f*u : .029f-.249f*u;
        const float vane=egret ? .027f : .021f;
        featherVolume(b,egret ? mat : 37u,{
            station(rootX,.004f,rootZ,vane,.010f),
            station(rootX+.54f*(tipX-rootX),.014f+.010f*u,rootZ+.40f*(tipZ-rootZ),vane,.008f),
            station(tipX,.032f-.018f*u,tipZ,.0035f,.003f)});
    }
}
} // namespace

ShanghaiPolicePose shanghaiPolicePose(float riverSeconds, float motionScale) noexcept {
    const float phase = (riverSeconds - 50.f) * .115f * motionScale;
    const float sn = std::sin(phase), cs = std::cos(phase);
    const float bend = 1.f - cs;
    return {{2.f + 50.f * sn, 19.7f + 1.2f * sn + 24.f * bend * bend,
             57.f + 7.f * bend + 2.f * sn},
            std::atan2(50.f * cs, 7.f * sn + 2.f * cs), -.075f * motionScale * cs};
}

Bmesh buildShanghaiPoliceHelicopter() {
    ModuleBuilder b{kShanghaiPoliceCabin,{}};
    constexpr uint32_t white=27, navy=25, canopy=18, carbon=37, alloy=23;
    // +Z is forward. The upper glazing wraps the bulbous nose. Narrow shell
    // stations between the door and windscreen leave actual painted frames.
    struct Station { float z, centerY, width, height; };
    constexpr Station stations[] = {
        {-4.89f,.56f,.16f,.19f},{-3.25f,.48f,.32f,.28f},
        {-2.02f,.24f,.57f,.56f},{-1.37f,.09f,.91f,.85f},
        {-.98f,.05f,1.03f,.94f},{-.89f,.05f,1.04f,.94f},
        {-.03f,.03f,1.07f,.96f},{.06f,.03f,1.07f,.96f},
        {.87f,.00f,1.04f,.90f},{.96f,-.02f,1.02f,.88f},
        {1.54f,-.11f,.90f,.73f},{2.04f,-.25f,.56f,.49f},{2.30f,-.36f,.19f,.23f}
    };
    constexpr Vec2 section[] = {{1,0},{.98f,.43f},{.75f,.84f},{.34f,1},
        {-.34f,1},{-.75f,.84f},{-.98f,.43f},{-1,0},{-.95f,-.42f},
        {-.75f,-.78f},{-.48f,-1},{.48f,-1},{.75f,-.78f},{.95f,-.42f}};
    constexpr size_t sides=std::size(section);
    auto skin=[&](size_t i,size_t j)->Point {
        const auto& s=stations[i]; const auto& p=section[j%sides];
        return {s.width*p[0],s.centerY+s.height*p[1],s.z};
    };
    for(size_t i=0;i+1<std::size(stations);++i) {
        for(size_t j=0;j<sides;++j) {
            const bool upper=j<=6, roof=j>=2 && j<=4;
            const bool door=i==5 || i==7, windscreen=i==9 || i==10;
            const bool glass=upper && ((door && !roof) || (windscreen && j!=3));
            const bool belt=(j==7 || j==13) || (i<4 && (j==0 || j==6));
            b.quad(glass ? canopy : belt ? navy : white,skin(i,j),skin(i,j+1),skin(i+1,j+1),skin(i+1,j));
        }
    }
    for (size_t i : {size_t(0),std::size(stations)-1}) {
        for(size_t j=1;j+2<sides;j+=2) {
            if(i==0) b.quad(white,skin(i,j+2),skin(i,j+1),skin(i,j),skin(i,0));
            else b.quad(white,skin(i,0),skin(i,j),skin(i,j+1),skin(i,j+2));
        }
    }

    // The glazing looks onto the cabin floor, dashboard, and seat backs.
    // Their opaque surfaces also intercept refraction rays from the waterfront.
    b.tank(carbon,0,.08f,1.63f,2.63f,-.50f,-.36f);
    b.loft(carbon,{0,1.21f,1.53f,.36f,-.38f},{0,1.22f,1.57f,.40f,.14f});
    for(float side:{-1.f,1.f}) {
        for(float z:{-.67f,.40f}) {
            b.loft(carbon,{side*.47f,z,.49f,.21f,-.36f},
                          {side*.47f,z-.10f,.47f,.16f,.35f});
            b.tank(carbon,side*.47f,z-.09f,.29f,.19f,.32f,.51f);
        }
    }
    // A pair of low turbine fairings straddles the mast. The bent runners
    // are octagonal tubes with two splayed mounting legs on either side.
    for(float side:{-1.f,1.f}) {
        featherVolume(b,white,{birdSection(-1.78f,.75f,.13f,.13f,side*.38f),
            birdSection(-1.47f,.83f,.33f,.24f,side*.38f),birdSection(-.43f,.92f,.36f,.28f,side*.38f),
            birdSection(.32f,.88f,.20f,.15f,side*.38f)});
        featherVolume(b,carbon,{birdSection(-1.88f,.77f,.12f,.10f,side*.38f),
            birdSection(-1.58f,.81f,.145f,.13f,side*.38f)});
        featherVolume(b,carbon,{birdSection(-1.91f,-1.17f,.055f,.065f,side*1.09f),
            birdSection(-1.70f,-1.23f,.078f,.078f,side*1.09f),birdSection(1.18f,-1.23f,.078f,.078f,side*1.09f),
            birdSection(1.63f,-1.08f,.061f,.073f,side*1.09f),birdSection(1.77f,-.96f,.047f,.057f,side*1.09f)});
        for(float z:{-.99f,.81f}) b.loft(carbon,{side*1.09f,z,.11f,.15f,-1.22f},
                                                    {side*.67f,z+.07f,.16f,.20f,-.48f});
    }
    b.tank(alloy,0,-.28f,.25f,.25f,1.02f,kShanghaiMainHub[1]);
    // Open fenestron with a substantial white shroud and a tapered fin.
    const float fy=kShanghaiTailHub[1], fz=kShanghaiTailHub[2];
    constexpr int segments=24;
    auto ring=[&](int j,float x,float r)->Point {
        const float a=6.28318531f*float(j%segments)/segments;
        return {x,fy+r*std::cos(a),fz+r*std::sin(a)};
    };
    for(int j=0;j<segments;++j) {
        b.quad(white,ring(j,.17f,.78f),ring(j+1,.17f,.78f),ring(j+1,.17f,.49f),ring(j,.17f,.49f));
        b.quad(white,ring(j,-.17f,.49f),ring(j+1,-.17f,.49f),ring(j+1,-.17f,.78f),ring(j,-.17f,.78f));
        b.quad(white,ring(j,-.17f,.78f),ring(j+1,-.17f,.78f),ring(j+1,.17f,.78f),ring(j,.17f,.78f));
        b.quad(navy,ring(j,.17f,.49f),ring(j+1,.17f,.49f),ring(j+1,-.17f,.49f),ring(j,-.17f,.49f));
    }
    b.loft(white,{0,fz-.05f,.31f,1.01f,fy+.56f},{0,fz-.39f,.16f,.56f,fy+1.62f});
    b.loft(white,{0,-3.66f,2.50f,.34f,.47f},{0,-3.68f,2.64f,.29f,.55f});
    // Permanent local police markings (警察) on both cabin doors. Place
    // the lettering directly on the sloping blue belt of this shell station.
    constexpr uint32_t policeMark[2][28]={
        {0x430100,0x430300,0xffff300,0x4307ff,0x1800e1c,0x1ffce18,0x300db18,0x600db30,
         0xdfcc1b0,0x10c80e0,0x10c81f0,0x1fd87bc,0x10fee0e,0x6000,0x7fffffe,0xfffffff,
         0,0xffffe0,0,0,0xffffe0,0,0xffffe0,0xffffe0,0xc00020,0xc00060,0xffffe0,0xc00060},
        {0x6000,0x6000,0x7000,0x7fffffe,0x7fffffe,0x6100006,0x6300006,0x701800,
         0x7fdffc,0xc0cc18,0x1818c38,0x3b38670,0x73f0360,0x60e03c0,0xcc01c0,0x7bfee0,
         0x73fe70,0x1e0003c,0x780000f,0xe3fffe6,0x7ffff0,0x3000,0x3100,0x3831c0,
         0xf030f0,0x3c0703c,0x707e00e,0x7c000}
    };
    for(float side:{-1.f,1.f}) {
        auto decal=[&](int col,int row)->Point {
            float z=-.46f+side*float(col-29)*.012f;
            float y=.005f-float(row)*.012f;
            float t=(z-stations[5].z)/(stations[6].z-stations[5].z);
            float width=std::lerp(stations[5].width,stations[6].width,t);
            float level=(y-std::lerp(stations[5].centerY,stations[6].centerY,t))/
                        std::lerp(stations[5].height,stations[6].height,t);
            return {side*(width*(1.f+level*(.05f/.42f))+.005f),y,z};
        };
        for(int ch=0;ch<2;++ch) for(int r=0;r<28;++r) {
            const auto bits=policeMark[ch][r];
            for(int x=0;x<28;) {
                if(!(bits & (1u<<(27-x)))) {++x;continue;}
                int start=x++;
                while(x<28 && (bits & (1u<<(27-x)))) ++x;
                int left=ch*31+start, right=ch*31+x;
                b.quad(white,decal(left,r+1),decal(left,r),decal(right,r),decal(right,r+1));
            }
        }
    }
    // Belly lettering on the ivory underside follows the Shanghai aircraft.
    constexpr unsigned badge[6][7]={{30,17,17,30,16,16,16},{14,17,17,17,17,17,14},
        {16,16,16,16,16,16,31},{31,4,4,4,4,4,31},{14,17,16,16,16,17,14},{31,16,16,30,16,16,31}};
    for(int ch=0;ch<6;++ch) for(int row=0;row<7;++row) {
        for(int col=0;col<5;++col) if(badge[ch][row] & (16u>>col)) {
            const float z=-.86f+float(6*ch+col)*.05f;
            const float x=.175f-float(row)*.05f;
            b.box(navy,x,z,.051f,.051f,-.949f,-.942f);
        }
    }
    // Each rotor mesh uses its own shaft as the origin; the renderer
    // composes the local spin after the airframe transform.
    b.part=kShanghaiPoliceRotor;
    b.tank(alloy,0,0,.38f,.38f,-.075f,.075f);
    auto blade=[&](float a,float root,float radius,float chord,bool tail) {
        const float sn=std::sin(a),cs=std::cos(a);
        auto vertex=[&](float r,float c,float thick)->Point {
            const float x=r*cs-c*sn,z=r*sn+c*cs;
            return tail ? Point{thick,x,z} : Point{x,thick,z};
        };
        const float thickness=tail?.015f:.044f;
        auto span=[&](float r0,float r1,float offset0,float offset1,uint32_t mat) {
            std::array<Point,4> top{vertex(r0,offset0,thickness),vertex(r0,offset0+chord,thickness),
                                    vertex(r1,offset1+chord,thickness),vertex(r1,offset1,thickness)};
            auto bottom=top;
            for(auto& p:bottom)p[tail?0:1]-=2.f*thickness;
            if(tail){std::reverse(top.begin(),top.end());std::reverse(bottom.begin(),bottom.end());}
            b.quad(mat,top[0],top[1],top[2],top[3]);
            b.quad(mat,bottom[3],bottom[2],bottom[1],bottom[0]);
            for(int j=0;j<4;++j) b.quad(mat,top[j],bottom[j],bottom[(j+1)%4],top[(j+1)%4]);
        };
        span(root,radius*.90f,0.f,-chord*.42f,carbon);
        span(radius*.90f,radius,-chord*.42f,-chord*.60f,tail ? carbon : alloy);
    };
    for(int j=0;j<4;++j) blade(float(j)*1.57079633f,.13f,4.86f,.19f,false);
    b.part=kShanghaiPoliceFan;
    b.box(alloy,0,0,.21f,.16f,-.08f,.08f);
    for(int j=0;j<10;++j) blade(float(j)*.62831853f,.07f,.48f,.051f,true);
    return b.finish();
}

Bmesh buildCityBird(CityBirdSpecies species) {
    const bool egret=species==CityBirdSpecies::Egret;
    const uint32_t body=egret ? kEgretBody : kKiteBody;
    const uint32_t mat=egret ? 33u : 26u;
    ModuleBuilder b{body,{}};
    featherVolume(b,mat,{
        birdSection(-.30f,-.012f,.018f,.025f),birdSection(-.20f,-.008f,.071f,.067f),
        birdSection(-.054f,-.006f,.105f,.091f),birdSection(.072f,.002f,.084f,.085f),
        birdSection(.128f,.018f,.046f,.050f)});
    if (egret) {
        // Great egrets tuck the neck into an S in flight, with the legs visibly
        // extending aft of their short white tail and a long, straight bill.
        featherVolume(b,mat,{
            birdSection(.054f,.027f,.054f,.055f),birdSection(.128f,.043f,.058f,.068f),
            birdSection(.175f,.084f,.044f,.067f),birdSection(.208f,.125f,.044f,.049f),
            birdSection(.257f,.124f,.043f,.040f),birdSection(.284f,.117f,.025f,.023f)});
        featherVolume(b,26,{
            birdSection(.261f,.118f,.025f,.022f),birdSection(.328f,.115f,.018f,.014f),
            birdSection(.452f,.103f,.0025f,.0025f)});
        featherVolume(b,mat,{
            birdSection(-.389f,-.020f,.069f,.011f),birdSection(-.293f,-.009f,.087f,.022f),
            birdSection(-.182f,-.003f,.055f,.025f)});
        for (float side:{-1.f,1.f})
            featherVolume(b,37,{
                birdSection(-.645f,-.090f,.004f,.004f,side*.050f),
                birdSection(-.575f,-.084f,.009f,.006f,side*.047f),
                birdSection(-.380f,-.066f,.009f,.009f,side*.043f),
                birdSection(-.166f,-.050f,.013f,.012f,side*.038f)});
    } else {
        featherVolume(b,mat,{
            birdSection(.078f,.033f,.069f,.065f),birdSection(.155f,.061f,.059f,.063f),
            birdSection(.206f,.062f,.045f,.048f),birdSection(.225f,.052f,.025f,.028f)});
        featherVolume(b,37,{
            birdSection(.202f,.055f,.031f,.023f),birdSection(.244f,.048f,.019f,.016f),
            birdSection(.264f,.019f,.005f,.007f)});
        // Long outer rectrices frame the shallow V of the black kite's tail.
        for (float side:{-1.f,1.f})
            featherVolume(b,mat,{
                birdSection(-.452f,-.025f,.016f,.0045f,side*.112f),
                birdSection(-.396f,-.020f,.040f,.008f,side*.082f),
                birdSection(-.330f,-.018f,.071f,.010f,side*.047f),
                birdSection(-.173f,-.010f,.038f,.018f,side*.019f)});
    }
    birdWing(b,body,egret,true);
    birdWing(b,body,egret,false);
    return b.finish();
}

namespace {
#include "blimp_signs.inc"

bool signPixel(const BlimpSignFace& face, uint32_t x, uint32_t y) noexcept {
    const char digit = face.rows[y*((face.width+3)/4)+x/4];
    const unsigned bits = digit <= '9' ? digit-'0' : digit-'a'+10;
    return (bits & (8u >> (x%4))) != 0u;
}
}

const BlimpSigns& cityBlimpSigns(std::string_view languageTag) noexcept {
    for (const auto& signs : kBlimpSigns) {
        if (signs.languageTag == languageTag) return signs;
    }
    return kBlimpSigns[0]; // The source catalog, en-US.
}

Bmesh buildCityBlimp(const BlimpSigns& signs) {
    ModuleBuilder b{kSkyBlimp,{}};
    constexpr float halfLength=9.f, height=2.75f, beam=2.55f;
    constexpr float pi=3.14159265f;
    auto radius=[](float x) {return std::sqrt(std::max(0.f,1.f-x*x/(9.f*9.f)));};
    auto skin=[&](float x,float angle,float raised) -> Point {
        const float r=radius(x);
        return {x,(r*height+raised)*std::cos(angle),(r*beam+raised)*std::sin(angle)};
    };
    // A broadside airship. Narrow raised strips trace the sewn longitudinal
    // gores, converging at both nose caps just as they would on the real fabric.
    constexpr int rings=24, sectors=32;
    auto sectionX=[&](int i) {return halfLength*std::sin(-1.563f+3.126f*float(i)/rings);};
    for(int i=0;i<rings;++i) {
        const float x0=sectionX(i), x1=sectionX(i+1);
        for(int j=0;j<sectors;++j) {
            const float a=j*(2*pi/sectors), c=(j+1)*(2*pi/sectors);
            b.quad(33,skin(x0,a,0),skin(x0,c,0),skin(x1,c,0),skin(x1,a,0));
            if(j%4==0 && i && i<rings-1) {
                b.quad(26,skin(x0,a-.011f,.013f),skin(x0,a+.011f,.013f),
                          skin(x1,a+.011f,.013f),skin(x1,a-.011f,.013f));
            }
        }
    }
    for(int i:{0,rings}) {
        const float x=sectionX(i);
        const float step=(i ? 1.f : -1.f)*(2*pi/sectors);
        for(int j=1;j<sectors-2;j+=2)
            b.quad(26,skin(x,0,0),skin(x,j*step,0),skin(x,(j+1)*step,0),skin(x,(j+2)*step,0));
    }
    // Fins with physical thickness. The lighter edge stays visible in silhouette.
    for(int vane=0;vane<4;++vane) {
        // The X-tail leaves the passenger-facing equator clear for the sign.
        const float a=(float(vane)+.5f)*pi*.5f;
        auto finPoint=[&](float x,float radial,float offset) -> Point {
            return {x,radial*std::cos(a)-offset*std::sin(a),radial*std::sin(a)+offset*std::cos(a)};
        };
        const std::array<Vec2,4> shape{{{-6.22f,1.12f},{-7.75f,3.25f},{-9.09f,3.36f},{-8.19f,.69f}}};
        std::array<Point,4> nearFace{},farFace{};
        for(int j=0;j<4;++j) {
            nearFace[j]=finPoint(shape[j][0],shape[j][1],.075f);
            farFace[j]=finPoint(shape[j][0],shape[j][1],-.075f);
        }
        b.quad(24,nearFace[0],nearFace[1],nearFace[2],nearFace[3]);
        b.quad(24,farFace[3],farFace[2],farFace[1],farFace[0]);
        for(int j=0;j<4;++j) {
            const int k=(j+1)%4;
            b.quad(26,nearFace[j],farFace[j],farFace[k],nearFace[k]);
        }
    }
    // Frames, glazed cabin and the rounded belly of the suspended gondola.
    for(float x:{-1.28f,1.51f}) b.box(26,x,0,.13f,1.03f,-3.16f,-2.53f);
    b.loft(24,{.14f,0,3.68f,.84f,-3.89f},{.14f,0,4.34f,1.14f,-3.52f});
    b.loft(11,{.14f,0,4.34f,1.14f,-3.52f},{.32f,0,3.37f,.98f,-3.12f});
    for(float x:{-.89f,-.16f,.57f,1.30f})
        b.box(26,x,0,.080f,1.16f,-3.52f,-3.14f);

    // Each face carries a complete shaped phrase. Its positive X points to
    // a viewer's right even on the reverse, including for Arabic lettering.
    constexpr float panelHalfWidth=7.18f, panelMargin=.60f;
    for(float side:{-1.f,1.f}) {
        auto panelPoint=[&](float x,float y,float offset)->Point {
            const float localX=.24f-side*x;
            return {localX,y,side*(beam*std::sqrt(std::max(0.f,1.f-localX*localX/(halfLength*halfLength)-y*y/(height*height)))+offset)};
        };
        auto ink=[&](uint32_t material,float left,float right,float low,float high,float offset) {
            b.quad(material,panelPoint(left,low,offset),panelPoint(left,high,offset),
                            panelPoint(right,high,offset),panelPoint(right,low,offset));
        };
        // Strips follow the convex hull; a single flat slab would cut through it.
        for(int col=0;col<28;++col) {
            constexpr float stripWidth=2.f*panelHalfWidth/28.f;
            const float left=-panelHalfWidth+stripWidth*col,right=left+stripWidth;
            for(int row=0;row<6;++row) {
                const float y=-1.32f+.44f*row;
                ink(11,left,right,y,y+.44f,.075f);
            }
            ink(26,left,right,1.26f,1.32f,.094f);
            ink(26,left,right,-1.32f,-1.26f,.094f);
        }
        const auto& face = side < 0 ? signs.front : signs.back;
        const float pitch = std::min(2.f*(panelHalfWidth-panelMargin)/face.width,2.18f/face.height);
        const float left = -0.5f*face.width*pitch, top = 0.5f*face.height*pitch;
        // Split long connected headline strokes so their chords stay clear of
        // the curved panel. Short horizontal ink runs need just one quad.
        const uint32_t maxRun = std::max(1u,static_cast<uint32_t>(0.48f/pitch));
        for (uint32_t row=0; row<face.height; ++row) {
            const float high = top-row*pitch;
            for (uint32_t col=0; col<face.width;) {
                if (!signPixel(face,col,row)) { ++col; continue; }
                const uint32_t first = col++;
                while (col<face.width && col-first<maxRun && signPixel(face,col,row)) ++col;
                ink(15,left+first*pitch,left+col*pitch,high-pitch,high,.120f);
            }
        }
    }
    return b.finish();
}

uint32_t selectCityModuleLod(float distanceWorld,float worldUnitsPerMeter,uint32_t seed) noexcept {
    // Adjacent buildings change finer details at different positions. All tiers
    // retain their major profile, and no LOD choice depends on playback history.
    const float phase=.94f+.12f*float(seed&255u)/255.f;
    if(distanceWorld < 120.f*worldUnitsPerMeter*phase) return 0;
    if(distanceWorld < 350.f*worldUnitsPerMeter*phase) return 1;
    return 2;
}

float cityDetailScale(float distanceWorld,float worldUnitsPerMeter,uint32_t seed,uint32_t detailTier) noexcept {
    if(detailTier==0) return 1.f;
    const float phase=.94f+.12f*float(seed&255u)/255.f;
    const float end=(detailTier==1 ? 120.f : 350.f)*worldUnitsPerMeter*phase;
    const float t=std::clamp((end-distanceWorld)/(.18f*end),0.f,1.f);
    return t*t*(3.f-2.f*t);
}

void appendCityModules(CityModuleLayout& layout,uint32_t kit,Bmesh& authored) {
    if(authored.proceduralFallback || (kit!=1 && kit!=2)) return;
    const bool taipei=kit==2;
    // The deposited legacy and preview payloads carry different city layouts.
    // Any new surveyed kit stays fully authored until its exporter opts in.
    const bool legacy=taipei ? authored.sourcePayloadCrc==0x0019c664u && authored.vertices.size()==471384u
                             : authored.sourcePayloadCrc==0x751738d4u && authored.vertices.size()==128418u;
    const bool basinKit=taipei ? authored.sourcePayloadCrc==0x252bd557u : authored.sourcePayloadCrc==0x300082cdu;
    if(!legacy && !basinKit) return;
    const float scale=taipei ? .08f : 1.f;
    const float originX=taipei ? 18.5f : 0.f,originZ=taipei ? 60.f : 0.f;
    std::vector<Site> sites;
    // One rooftop feature per parcel. Pick the tallest flat roof at split-level sites.
    std::map<std::array<int32_t,2>,Site> crowns;
    std::vector<uint8_t> replaced(authored.indices.size()/3);
    for(const auto& sub:authored.submeshes) {
        if((sub.part!=3 && sub.part!=4) || (sub.material!=5 && sub.material!=13 && sub.material!=14)) continue;
        for(const auto& shell:connectedShells(authored,sub)) {
            float bottom=1.e20f,top=-1.e20f;
            for(const auto& p:shell.points) {bottom=std::min(bottom,p[1]);top=std::max(top,p[1]);}
            const float height=top-bottom;
            if(height*scale<3.f) continue;
            // Only the procedural basin blocks have these three original topology
            // signatures and the -0.9 ground plane. Custom hero buildings stay intact.
            const size_t triangles=shell.faces.size();
            const bool basin=std::abs(bottom*scale+.9f)<.005f && (triangles==12 || triangles==28 || triangles==44);
            std::vector<Vec2> face;
            for(const auto& p:shell.points) {
                if(std::abs(p[1]-(basin?bottom:top))*scale<.002f) face.push_back({p[0],p[2]});
            }
            const Footprint plot=fitFootprint(std::move(face));
            if(std::min(plot.width,plot.depth)*scale<1.5f) continue;
            const float wx=originX+plot.x*scale,wz=originZ+plot.z*scale;
            const uint32_t seed=mixBits(kit*7919u ^ mixBits(static_cast<uint32_t>(std::lround(wx*16.f))) ^
                                         mixBits(static_cast<uint32_t>(std::lround(wz*16.f))+127u));
            Site site{plot,bottom,height,sub.part,sub.material,seed%3,seed};
            if(basin) {
                sites.push_back(site);
                for(uint32_t f:shell.faces) replaced[sub.firstIndex/3+f]=1;
            } else if(wz>=18.f && wz<=160.f && std::abs(wx)<115.f && height*scale>8.f
                      && std::min(plot.width,plot.depth)*scale>=1.65f
                      && std::max(plot.width,plot.depth)*scale<16.f
                      && std::abs(wx-originX)+std::abs(wz-originZ)>6.f) {
                site.bottom=top-.025f/scale;
                site.height=std::clamp(.23f*height,1.7f/scale,5.5f/scale);
                constexpr uint32_t profiles[]{3,7,8};
                site.style=profiles[seed%3u];
                const std::array<int32_t,2> cell{int32_t(std::floor(wx/7.f)),int32_t(std::floor(wz/7.f))};
                auto [it,inserted]=crowns.try_emplace(cell,site);
                if(!inserted && site.bottom>it->second.bottom) it->second=site;
            }
        }
    }
    if(legacy) addLegacyDistricts(sites,kit);
    if(taipei) addXinyiStreetFronts(sites);
    else if(basinKit) {
        // This platform is present only in the expanded Shenzhen mesh. Its five
        // original supports are 3.72 scene units apart, north of the moving train.
        sites.push_back({{-9.56f,41.90f,18.60f,1.70f,1,0},6.12f,1.28f,2,11,9,mixBits(697u)});
    }
    if(sites.empty()) return;
    for(const auto& [cell,crown]:crowns) sites.push_back(crown);

    std::map<std::array<uint32_t,2>,std::array<uint32_t,3>> prototypes;
    for(const Site& site:sites) {
        const auto& p=site.plot;
        auto [it,inserted]=prototypes.try_emplace({site.style,site.material});
        if(inserted) for(uint32_t lod=0;lod<3;++lod) {
            it->second[lod]=static_cast<uint32_t>(layout.modules.size());
            layout.modules.push_back(makeModule(kit,site.style,site.material,lod));
        }
        CityModulePlacement placement;
        placement.kit=kit;placement.part=site.part;placement.seed=site.seed;placement.modules=it->second;
        placement.rows={p.width*p.cosine,0,-p.depth*p.sine,p.x, 0,site.height,0,site.bottom,
                        p.width*p.sine,0,p.depth*p.cosine,p.z};
        // The maxima cover every tier including the tallest fin and the street canopy.
        placement.center={p.x,site.bottom+.59f*site.height,p.z};
        placement.radius=std::sqrt(.31f*p.width*p.width+.33f*p.depth*p.depth+.35f*site.height*site.height);
        placement.chunk={int32_t(std::floor((originX+p.x*scale)/44.f)),int32_t(std::floor((originZ+p.z*scale)/44.f))};
        layout.placements.push_back(placement);
    }
    // Legacy districts extend the scene around an intact authored footprint.
    if(legacy) return;
    // Compact the source geometry as well. Converted city blocks need neither
    // their old indices nor their split 64-byte UV/normal vertices on the GPU.
    std::vector<uint32_t> indices;
    std::vector<BmeshVertex> vertices;
    std::vector<BmeshSubmesh> submeshes;
    indices.reserve(authored.indices.size());
    vertices.reserve(authored.vertices.size());
    for(auto sub:authored.submeshes) {
        const uint32_t begin=sub.firstIndex,end=begin+sub.indexCount;
        const uint32_t oldBase=sub.firstVertex;
        std::vector<uint32_t> remap(sub.vertexCount,UINT32_MAX);
        sub.firstIndex=static_cast<uint32_t>(indices.size());
        sub.firstVertex=static_cast<uint32_t>(vertices.size());
        for(uint32_t i=begin;i<end;i+=3) {
            if(replaced[i/3]) continue;
            for(uint32_t j=0;j<3;++j) {
                const auto original=authored.indices[i+j];
                auto& compacted=remap[original-oldBase];
                if(compacted==UINT32_MAX) {
                    compacted=static_cast<uint32_t>(vertices.size());
                    vertices.push_back(authored.vertices[original]);
                }
                indices.push_back(compacted);
            }
        }
        sub.indexCount=static_cast<uint32_t>(indices.size())-sub.firstIndex;
        sub.vertexCount=static_cast<uint32_t>(vertices.size())-sub.firstVertex;
        if(sub.indexCount) submeshes.push_back(sub);
    }
    authored.indices=std::move(indices);
    authored.vertices=std::move(vertices);
    authored.submeshes=std::move(submeshes);
}
} // namespace boo
