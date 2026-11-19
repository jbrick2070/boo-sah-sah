#include "boo/city_modules.hpp"

#include <algorithm>
#include <cmath>
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
    std::map<std::array<uint32_t,3>, Surface> surfaces;
    uint32_t detailTier=0,detailSolid=0;

    void quad(uint32_t mat, Point a, Point b, Point c, Point d) {
        auto& surface = surfaces[{mat,detailTier,detailTier ? detailSolid : 0u}];
        const uint32_t first = static_cast<uint32_t>(surface.vertices.size());
        const Point tangent = unit(subtract(b, a));
        const Point normal = unit(cross(subtract(b, a), subtract(c, a)));
        const std::array<Point,4> positions{a,b,c,d};
        for (size_t i = 0; i < positions.size(); ++i) {
            BmeshVertex vertex{};
            std::copy_n(positions[i].begin(),3,vertex.positionMaterial);
            std::copy_n(normal.begin(),3,vertex.normalPart);
            std::copy_n(tangent.begin(),3,vertex.tangentSign);
            vertex.positionMaterial[3] = static_cast<float>(mat);
            vertex.normalPart[3] = static_cast<float>(part);
            vertex.tangentSign[3] = 1.f;
            vertex.uvThicknessRoughness[0] = i == 1 || i == 2 ? 1.f : 0.f;
            vertex.uvThicknessRoughness[1] = i >= 2 ? 1.f : 0.f;
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
    Bmesh finish() const {
        Bmesh mesh;
        mesh.bounds = {1.e20f,1.e20f,1.e20f,-1.e20f,-1.e20f,-1.e20f};
        for (const auto& [key,surface] : surfaces) {
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.submeshes.push_back({static_cast<uint32_t>(mesh.indices.size()),
                static_cast<uint32_t>(surface.indices.size()),key[0],part | (key[1]<<16),base,
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

// Major changes in profile persist in all three LODs. Only the finer projections
// disappear, so a direct seek across a threshold gives the same skyline silhouette.
Bmesh makeModule(uint32_t kit, uint32_t style, uint32_t glazing, uint32_t lod) {
    ModuleBuilder b{kit==1 ? 60u : 61u, {}};
    const uint32_t trim = kit==1 ? (style==1 ? 6u : 16u) : 15u;
    if (style == 4) {
        // Ground extension under the outlying blocks of the original compact set.
        b.box(2,0,0,1,1,0,1);
        return b.finish();
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
    if(lod == 0 && style != 3) {
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
                site.style=3;
                const std::array<int32_t,2> cell{int32_t(std::floor(wx/7.f)),int32_t(std::floor(wz/7.f))};
                auto [it,inserted]=crowns.try_emplace(cell,site);
                if(!inserted && site.bottom>it->second.bottom) it->second=site;
            }
        }
    }
    if(legacy) addLegacyDistricts(sites,kit);
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
