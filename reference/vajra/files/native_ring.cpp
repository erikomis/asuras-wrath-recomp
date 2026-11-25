#include "guest.h"
#include "ps3_sync.h"

extern "C" void ppu_thread_note_block(uint64_t tid);

namespace {

constexpr uint32_t k_write = 0x8;
constexpr uint32_t k_writing = 0xC;
constexpr uint32_t k_read = 0x10;
constexpr uint64_t k_idle_sleep_us = 30;

volatile uint32_t* host_word(uint32_t ea)
{
    return reinterpret_cast<volatile uint32_t*>(ppu_mm_take().vb + ea);
}

void commit(ppu_context* ctx, uint32_t pending)
{
    const uint32_t context = (uint32_t)ctx->gpr[3];
    ctx->gpr[4] = pending;
    if (!pending) return;
    PPU_FENCE(acq_rel);
    const uint32_t end = vm_read32(context + 0x8), ring = vm_read32(context);
    vm_write32(ring + k_write, end);
    vm_write32(ring + k_writing, 0);
    vm_write32(context + 0x4, 0);
    ctx->gpr[4] = 0;
    ctx->gpr[5] = ring;
    ps3_brief_wake(host_word(ring + k_write));
}

}

void func_00027BC8(ppu_context* ctx)
{
    const uint32_t ring = (uint32_t)ctx->gpr[3];
    volatile uint32_t* write = host_word(ring + k_write);
    const uint32_t read = vm_read32(ring + k_read);
    uint32_t seen = *write, written = __builtin_bswap32(seen);
    ctx->gpr[4] = read;
    ctx->gpr[3] = written;
    if (read != written) return;
    for (unsigned spin = 0; spin < 512u; spin++) {
        ps3_pause();
        seen = *write;
        written = __builtin_bswap32(seen);
        if (read != written) {
            ctx->gpr[3] = written;
            return;
        }
    }
    ppu_thread_note_block(ctx->thread_id);
    ps3_brief_wait(write, seen, k_idle_sleep_us);
    ctx->gpr[3] = 0;
}

void func_00027C94(ppu_context* ctx)
{
    commit(ctx, vm_read32((uint32_t)ctx->gpr[3] + 0x4));
}

void func_00027CC0(ppu_context* ctx)
{
    commit(ctx, vm_read32((uint32_t)ctx->gpr[3] + 0x4));
}

void func_00027CC4(ppu_context* ctx)
{
    commit(ctx, (uint32_t)ctx->gpr[4]);
}
