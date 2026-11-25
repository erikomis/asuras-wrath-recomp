#include "ppu_recomp.h"
#include <cstring>

namespace {

inline uint32_t rd32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

uint32_t probe(const uint8_t* vb, uint32_t level, uint32_t index, uint64_t name_be)
{
    const uint32_t size = rd32(vb + level + 0xB8);
    if (!size) return 0;
    const uint32_t buckets = rd32(vb + level + 0xB4);
    const uint32_t table = buckets ? buckets : level + 0xB0;
    const uint32_t elements = rd32(vb + level + 0x80);
    for (uint32_t i = rd32(vb + table + 4u * (index & (size - 1u))); (int32_t)i != -1;) {
        const uint8_t* el = vb + elements + 20u * i;
        uint64_t raw;
        std::memcpy(&raw, el, 8);
        if (raw == name_be) return rd32(el + 8);
        i = rd32(el + 0xC);
    }
    return 0;
}

uint32_t walk(const uint8_t* vb, uint32_t level, uint32_t index, uint64_t name_be)
{
    for (; level; level = rd32(vb + level + 0x40))
        if (const uint32_t found = probe(vb, level, index, name_be)) return found;
    return 0;
}

}

void func_000D7644(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint8_t* vb = m.vb;
    const uint32_t object = (uint32_t)ctx->gpr[3];
    const uint64_t name = ctx->gpr[4];
    const uint64_t name_be = __builtin_bswap64(name);
    vm_write64_mm(&m, ctx->gpr[1] + 0x38, name);
    const uint32_t index = (uint32_t)(name >> 32);
    uint32_t found = 0;
    const uint32_t frame = rd32(vb + object + 0x14);
    const uint32_t state = frame ? rd32(vb + frame + 0x28) : 0;
    if (state && (int32_t)ctx->gpr[5] == 0) found = walk(vb, state, index, name_be);
    if (!found) found = walk(vb, rd32(vb + object + 0x34), index, name_be);
    ctx->gpr[3] = found;
}
