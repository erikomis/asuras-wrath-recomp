#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

static std::array<uint8_t, 0x20000> memory{};
static std::mutex reservation;
extern "C" {
uint8_t* vm_base = memory.data();
void asura_spurs_queue_pop(ppu_context*);
void asura_spurs_queue_push(ppu_context*);
void spu_lockline_lock() { reservation.lock(); }
void spu_lockline_unlock() { reservation.unlock(); }
void spu_coh_notify_write(uint32_t) {}
int spu_taskset_signal_parked_obj(uint32_t, uint32_t) { return 0; }
uint32_t vm_read32(uint64_t ea) {
    assert(ea + 4 <= memory.size());
    return uint32_t(memory[ea]) << 24 | uint32_t(memory[ea+1]) << 16 |
           uint32_t(memory[ea+2]) << 8 | memory[ea+3];
}
uint64_t vm_read64(uint64_t ea) { return uint64_t(vm_read32(ea)) << 32 | vm_read32(ea+4); }
}

static void write32(uint32_t ea, uint32_t v) {
    for (int i = 0; i < 4; ++i) memory[ea+i] = v >> ((3-i)*8);
}
constexpr uint32_t Q = 0x1000, BUFFER = 0x2000, IN = 0x3000, OUT = 0x4000;
static uint32_t push(bool block = false) {
    ppu_context c{}; c.gpr[3]=Q; c.gpr[4]=IN; c.gpr[5]=block;
    asura_spurs_queue_push(&c); return c.gpr[3];
}
static uint32_t pop(bool block = false, bool peek = false) {
    ppu_context c{}; c.gpr[3]=Q; c.gpr[4]=OUT; c.gpr[5]=peek; c.gpr[6]=block;
    asura_spurs_queue_pop(&c); return c.gpr[3];
}
int main() {
    write32(Q+0x0C, 2); write32(Q+0x10, 16); write32(Q+0x14, 2);
    write32(Q+0x1C, BUFFER);
    write32(OUT, 0xDEADBEEF);
    assert(pop() == 0x80410901 && vm_read32(OUT) == 0xDEADBEEF);
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        write32(IN, cycle*2); assert(push() == 0);
        write32(IN, cycle*2+1); assert(push() == 0);
        assert(push() == 0x80410901);
        assert(pop(false, true) == 0 && vm_read32(OUT) == cycle*2);
        assert(pop() == 0 && vm_read32(OUT) == cycle*2);
        assert(pop() == 0 && vm_read32(OUT) == cycle*2+1);
        assert(pop() == 0x80410901);
    }
    std::atomic<bool> entered{false}, done{false};
    std::thread consumer([&] {
        entered = true;
        assert(pop(true) == 0 && vm_read32(OUT) == 1234);
        done = true;
    });
    while (!entered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(!done);
    write32(IN, 1234); assert(push() == 0);
    consumer.join(); assert(done);
    assert(push() == 0 && push() == 0);
    entered = false; done = false;
    std::thread producer([&] { entered = true; assert(push(true) == 0); done = true; });
    while (!entered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(!done); assert(pop() == 0); producer.join(); assert(done);
    puts("SPURS queue: wrap, full, empty, peek and blocking handoff passed");
}
