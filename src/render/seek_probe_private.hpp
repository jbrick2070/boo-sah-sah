#pragma once
#include <cstdio>
#include <cstdint>
static void seekProbeHash(const char* name,const void* data,size_t size) {
    char label[16]{};
    if (!GetEnvironmentVariableA("BOO_SEEK_LABEL",label,sizeof(label))) return;
    unsigned long long hash=14695981039346656037ULL;
    for(size_t i=0;i<size;++i) hash=(hash^static_cast<const uint8_t*>(data)[i])*1099511628211ULL;
    std::printf("seek %-7s %-12s bytes=%zu fnv=%016llx\n",label,name,size,hash);
}
