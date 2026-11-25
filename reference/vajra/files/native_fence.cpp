#include "guest.h"
#include "rsx_core.h"

extern "C" uint64_t ppu_timebase_now(void);
extern "C" void ppu_thread_note_block(uint64_t tid);

namespace {

constexpr uint32_t k_rhi = 0x019CF6B0;
constexpr uint32_t k_gcm = 0x01B42038;
constexpr uint32_t k_completed = 0x01A750D0;
constexpr uint32_t k_mark = 0x019CF8A8;
constexpr uint32_t k_mark_copy = 0x01A75728;
constexpr uint32_t k_stall_ticks = 0x01A55D90;
constexpr uint32_t k_fence_label = 0xFF;
constexpr int64_t k_wrap = 0x40000000;

bool pending(uint64_t target, uint64_t done)
{
    const uint64_t d = target - done;
    const bool wrap = (int64_t)((int64_t)d < 0 ? 0 - d : d) > k_wrap;
    return (int64_t)target > (int64_t)done || (wrap && (int64_t)target < (int64_t)done);
}

uint64_t timebase()
{
    uint64_t t;
    do t = ppu_timebase_now();
    while (!(uint32_t)t);
    return t;
}

void call(ppu_context* ctx, uint64_t sp, uint32_t lr, void (*fn)(ppu_context*))
{
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    run(fn, ctx);
}

}

void func_00874EF0(ppu_context* ctx)
{
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr, frame = sp - 0xC0, target = ctx->gpr[4];
    const uint32_t rhi = (uint32_t)ctx->gpr[3];
    uint32_t seen = 0;
    const auto done = [&] {
        if (!pending(target, vm_read64(k_completed))) return true;
        seen = vm_read32(vm_read32(rhi + 0x54));
        vm_write64(k_completed, seen);
        return !pending(target, seen);
    };
    if (done()) return;
    vm_write64(frame, sp);
    vm_write64(sp + 0x10, lr);
    const uint64_t start = timebase();
    const uint32_t label = vm_read32(rhi + 0x54), owner = vm_read32(k_rhi);
    ctx->gpr[3] = vm_read32(label);
    ctx->gpr[4] = label;
    call(ctx, frame, 0x00874FB0, func_00874C0C);
    const uint32_t fence = vm_read32(owner + 0x50) + 1;
    vm_write32(owner + 0x50, fence);
    vm_write32(k_mark_copy, vm_read32(k_mark));
    ctx->gpr[3] = vm_read32(k_gcm);
    ctx->gpr[4] = k_fence_label;
    ctx->gpr[5] = fence;
    call(ctx, frame, 0x00874FE0, func_00CEE39C);
    ctx->gpr[3] = vm_read32(k_gcm);
    call(ctx, frame, 0x00874FE8, func_00CEE02C);
    do {
        ctx->gpr[3] = vm_read32(k_gcm);
        call(ctx, frame, 0x00875018, func_00CEE02C);
        ppu_thread_note_block(ctx->thread_id);
        rsx_core_wait_label(vm_read32(rhi + 0x54), seen);
    } while (!done());
    vm_write32(k_stall_ticks, vm_read32(k_stall_ticks) + (uint32_t)(timebase() - start));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}
