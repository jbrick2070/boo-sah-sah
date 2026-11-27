#include "boo/bmesh.hpp"
#include "boo/startup_progress.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
namespace boo {
namespace {
uint32_t u32(const uint8_t* p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8); }
float f32(const uint8_t* p) { return std::bit_cast<float>(u32(p)); }
float half(const uint8_t* p) {
    const auto h=u16(p); const int exponent=(h>>10)&31; const int mantissa=h&1023;
    const float value=exponent==0?std::ldexp(float(mantissa),-24):exponent==31?std::numeric_limits<float>::infinity():std::ldexp(float(mantissa+1024),exponent-25);
    return (h&32768)?-value:value;
}
float snorm(const uint8_t* p) { return std::max(-1.0f,float(std::bit_cast<int16_t>(u16(p)))/32767.0f); }
uint32_t crc(const uint8_t* p,size_t size) {
    uint32_t c=~0u;
    for(size_t i=0;i<size;++i) { c^=p[i]; for(int b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u); }
    return ~c;
}
}
bool loadBmesh(const std::filesystem::path& path,Bmesh& output,std::string* error) {
    const auto fail=[&](const char* reason) { if(error)*error=reason; return false; };
    startupReading(path);
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)return fail("Mesh missing or unreadable");
    const auto size=file.tellg();
    if(size<64 || size>256*1024*1024)return fail("Invalid mesh file size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size)); file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),size))return fail("Truncated mesh");
    startupAssetLoaded(path);
    const auto* p=bytes.data();
    if(std::memcmp(p,"BMESH001",8)||u32(p+8)!=1||u32(p+12)!=64||u32(p+16)!=40)return fail("Unsupported mesh format");
    const uint32_t nv=u32(p+20),ni=u32(p+24),ns=u32(p+28);
    if(!nv||!ni||!ns||nv>2000000||ni>6000000||ns>1024||ni%3)return fail("Invalid mesh counts");
    const uint64_t payload=uint64_t(ns)*24+uint64_t(nv)*40+uint64_t(ni)*4;
    if(payload!=u32(p+60)||payload+64!=bytes.size())return fail("Mesh size/count mismatch");
    if(crc(p+64,static_cast<size_t>(payload))!=u32(p+56))return fail("Mesh CRC32 mismatch");
    Bmesh mesh;
    mesh.sourcePayloadCrc = u32(p+56);
    for(int i=0;i<6;++i) { mesh.bounds[i]=f32(p+32+i*4);if(!std::isfinite(mesh.bounds[i]))return fail("Nonfinite bounds"); }
    for(int i=0;i<3;++i)if(mesh.bounds[i]>mesh.bounds[i+3])return fail("Inverted bounds");
    uint32_t expected=0;
    for(uint32_t i=0;i<ns;++i) {
        const auto* q=p+64+i*24;
        BmeshSubmesh sub{u32(q),u32(q+4),u32(q+8),u32(q+12),u32(q+16),u32(q+20)};
        if(sub.firstIndex!=expected||!sub.indexCount||sub.indexCount%3||sub.indexCount>ni-sub.firstIndex||sub.firstVertex>=nv||sub.vertexCount>nv-sub.firstVertex||sub.material>63)return fail("Invalid submesh range");
        expected+=sub.indexCount;mesh.submeshes.push_back(sub);
    }
    if(expected!=ni)return fail("Incomplete submesh coverage");
    const auto* vb=p+64+ns*24;
    for(uint32_t i=0;i<nv;++i) {
        const auto* q=vb+i*40; BmeshVertex v;
        for(int c=0;c<3;++c) {
            v.positionMaterial[c]=f32(q+c*4);v.normalPart[c]=snorm(q+12+c*2);v.tangentSign[c]=snorm(q+20+c*2);
            if(!std::isfinite(v.positionMaterial[c])||v.positionMaterial[c]<mesh.bounds[c]-.001f||v.positionMaterial[c]>mesh.bounds[c+3]+.001f)return fail("Vertex outside bounds");
        }
        v.positionMaterial[3]=float(u16(q+36));v.normalPart[3]=float(u16(q+38));v.tangentSign[3]=snorm(q+26);
        for(int c=0;c<4;++c) {v.uvThicknessRoughness[c]=half(q+28+c*2);if(!std::isfinite(v.uvThicknessRoughness[c]))return fail("Nonfinite vertex attribute");}
        const float normalLength=v.normalPart[0]*v.normalPart[0]+v.normalPart[1]*v.normalPart[1]+v.normalPart[2]*v.normalPart[2];
        if(normalLength<.98f||normalLength>1.02f||v.uvThicknessRoughness[2]<0||v.uvThicknessRoughness[3]<0||v.uvThicknessRoughness[3]>1)return fail("Invalid material/normal");
        mesh.vertices.push_back(v);
    }
    const auto* ib=vb+uint64_t(nv)*40;
    for(uint32_t i=0;i<ni;++i) {auto index=u32(ib+i*4);if(index>=nv)return fail("Index out of range");mesh.indices.push_back(index);}
    for(const auto& sub:mesh.submeshes)for(uint32_t j=0;j<sub.indexCount;++j) {
        const auto index=mesh.indices[sub.firstIndex+j];
        if(index<sub.firstVertex||index>=sub.firstVertex+sub.vertexCount||mesh.vertices[index].positionMaterial[3]!=float(sub.material)||mesh.vertices[index].normalPart[3]!=float(sub.part))return fail("Submesh attribute mismatch");
    }
    output=std::move(mesh);return true;
}
}
