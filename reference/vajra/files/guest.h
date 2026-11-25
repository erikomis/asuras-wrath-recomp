#pragma once

#include "ppu_recomp.h"
#include "vm_page_map.h"

#include <bit>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" const int32_t* ppu_exec_tab;
extern "C" uint32_t ppu_exec_lo, ppu_exec_n;

double tick_scale();
bool frame_site(uint32_t stack);
bool camera_assist_blocked(uint32_t function);
void note_stick_rotation();
int mouse_route();

inline float from_bits(uint32_t bits) { return std::bit_cast<float>(bits); }
inline uint32_t to_bits(float v) { return std::bit_cast<uint32_t>(v); }
inline double from_bits64(uint64_t bits) { return std::bit_cast<double>(bits); }
inline uint64_t to_bits64(double v) { return std::bit_cast<uint64_t>(v); }
inline double single(double d) { return (double)(float)d; }
inline uint32_t stfs(double d) { return to_bits((float)d); }

inline bool plain_store(const ppu_mm& m, uint32_t ea, uint32_t n)
{
    if (!vm_mm_in(m.st_span, ea, n) || (uint32_t)(ea - m.gcm) < 0x10000u || (uint32_t)(m.gcm - ea) < n) return false;
    if (const unsigned char* bits = g_spu_coh_bits)
        for (uint32_t line = ea >> 7; line <= (ea + n - 1) >> 7; line++)
            if ((bits[line >> 3] >> (line & 7)) & 1u) return false;
    return true;
}

inline void drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*next)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        next(ctx);
    }
}

inline void run(void (*body)(ppu_context*), ppu_context* ctx)
{
    body(ctx);
    drain(ctx);
}

inline void guest_icall(ppu_context* ctx)
{
    const uint32_t d = (uint32_t)ctx->ctr - ppu_exec_lo;
    const uint32_t i = (d >> 2) | (d << 30);
    if (__builtin_expect(i < ppu_exec_n && ppu_exec_tab[i], 1)) {
        const auto fn = (void (*)(ppu_context*))((const char*)ps3_indirect_call + ppu_exec_tab[i]);
        fn(ctx);
        drain(ctx);
        return;
    }
    ps3_indirect_call(ctx);
    drain(ctx);
}

inline void guest_call(ppu_context* ctx, uint32_t opd)
{
    ctx->ctr = vm_read32(opd);
    guest_icall(ctx);
}

struct GuestFrame {
    ppu_context* ctx;
    uint64_t caller;
    uint32_t base;

    GuestFrame(ppu_context* c, uint32_t size) : ctx(c), caller(c->gpr[1]), base((uint32_t)c->gpr[1] - size)
    {
        vm_write64(base, caller);
        ctx->gpr[1] = base;
    }

    ~GuestFrame() { ctx->gpr[1] = caller; }
};
