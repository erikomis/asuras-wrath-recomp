/* PPU side of CellSpursQueue, shared with the title's real SPU consumers.
 * The guest line uses W0/W1 as pop/push counters modulo 2*depth, depth at
 * +0x0C, element size at +0x10, buffer EA at +0x18, taskset EA at +0x70.
 * Share the MFC reservation lock so a PPU commit invalidates SPU snapshots.
 */
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <atomic>

extern "C" {
extern uint8_t* vm_base;
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t);
int spu_taskset_signal_parked_obj(uint32_t, uint32_t);
}

namespace {
constexpr uint32_t AGAIN = 0x80410901, INVAL = 0x80410902;
constexpr uint32_t ALIGN = 0x80410910, NULL_POINTER = 0x80410911;

void store32(uint32_t ea, uint32_t value)
{
    for (int i = 0; i < 4; ++i) vm_base[ea + i] = value >> ((3 - i) * 8);
}

uint32_t transfer(uint32_t q, uint32_t data, bool push, bool peek, bool blocking)
{
    if (!q || !data) return NULL_POINTER;
    if (q & 127) return ALIGN;
    if (uint64_t(q) + 128 > 0x100000000ull) return INVAL;
    bool reported_wait = false;
    for (;;) {
        spu_lockline_lock();
        const uint32_t depth = vm_read32(q + 0x0C);
        const uint32_t size = vm_read32(q + 0x10);
        const uint64_t buffer = vm_read64(q + 0x18);
        if (!depth || depth > 0xFFFF || !size || size > 0x4000 ||
            !buffer || buffer + uint64_t(depth) * size > 0x100000000ull ||
            uint64_t(data) + size > 0x100000000ull) {
            spu_lockline_unlock();
            return INVAL;
        }
        const int32_t raw_pop = (int32_t)vm_read32(q);
        const int32_t raw_push = (int32_t)vm_read32(q + 4);
        const uint32_t pop = raw_pop < 0 ? ~raw_pop : raw_pop;
        const uint32_t tail = raw_push < 0 ? ~raw_push : raw_push;
        const uint32_t span = 2 * depth;
        if (pop >= span || tail >= span) {
            spu_lockline_unlock();
            return INVAL;
        }
        const uint32_t count = (tail + span - pop) % span;
        if (count > depth) {
            spu_lockline_unlock();
            return INVAL;
        }
        const bool ready = push ? (raw_push >= 0 && count < depth)
                                : (raw_pop >= 0 && count > 0);
        if (ready) {
            const uint32_t index = push ? tail : pop;
            uint8_t* slot = vm_base + buffer + uint64_t(index % depth) * size;
            if (push) memcpy(slot, vm_base + data, size);
            else memcpy(vm_base + data, slot, size);
            if (push || !peek) {
                store32(q + (push ? 4 : 0), (index + 1) % span);
                spu_coh_notify_write(q);
            }
            const uint32_t taskset = (uint32_t)vm_read64(q + 0x70);
            spu_lockline_unlock();
            if (taskset && (push || !peek))
                spu_taskset_signal_parked_obj(taskset, q);
            /* ASURA_QUEUE_LOG=<n>: first n transfers are logged (default 16,
             * matching the other first-N diagnostics). The default hid the
             * fios/Bink handshake right where the videos stall: set it high
             * to see whether pushes/pops continue past the first video. */
            static std::atomic<int> cap{-1};
            int c = cap.load();
            if (c < 0) {
                const char* e = getenv("ASURA_QUEUE_LOG");
                c = e ? atoi(e) : 16;
                cap.store(c);
            }
            static std::atomic<unsigned> messages{0};
            if (messages.fetch_add(1) < (unsigned)c)
                fprintf(stderr, "[asura queue] %s q=%08X slot=%u size=%u count=%u\n",
                        push ? "push" : (peek ? "peek" : "pop"), q,
                        index % depth, size, count);
            return 0;
        }
        spu_lockline_unlock();
        if (!blocking) return AGAIN;
        if (!reported_wait) {
            static std::atomic<int> wcap{-1};
            int wc = wcap.load();
            if (wc < 0) {
                const char* e = getenv("ASURA_QUEUE_LOG");
                wc = e ? atoi(e) : 16;
                wcap.store(wc);
            }
            static std::atomic<unsigned> wmessages{0};
            if (wmessages.fetch_add(1) < (unsigned)wc)
                fprintf(stderr, "[asura queue] waiting to %s q=%08X count=%u\n",
                        push ? "push" : "pop", q, count);
            reported_wait = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

extern "C" void asura_spurs_queue_pop(ppu_context* ctx)
{
    const uint32_t rc = transfer(ctx->gpr[3], ctx->gpr[4], false,
                                ctx->gpr[5] != 0, ctx->gpr[6] != 0);
    ctx->gpr[3] = (int64_t)(int32_t)rc;
}

extern "C" void asura_spurs_queue_push(ppu_context* ctx)
{
    const uint32_t rc = transfer(ctx->gpr[3], ctx->gpr[4], true, false,
                                ctx->gpr[5] != 0);
    ctx->gpr[3] = (int64_t)(int32_t)rc;
}
