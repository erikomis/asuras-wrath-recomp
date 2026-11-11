/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "ppu_recomp.h"

extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*));
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);
extern "C" void asura_spurs_queue_pop(ppu_context*);
extern "C" void asura_spurs_queue_push(ppu_context*);
void func_000F3BE0(ppu_context*);

/* UE3's fatal-error Serialize implementation is reached through a vtable.
 * Mirror its UTF-16BE message before the original enters error handling;
 * otherwise the release build can leave only a black screen in the host log.
 * This hook observes the message and preserves the guest's error path. */
/* Diagnostic: the "CellMemoryManager" thread (func_00A7901C) backs a table of
 * chunk lists that SPU tasks and PPU callers bump-allocate from. The table is
 * at [TOC-0x5BA0]; slots start at +0x200, 16 bytes each: head, current,
 * pending flag, spinlock. A node is {generation, base, cursor, size,
 * remaining, next}. Printing the chain lengths at the fatal error shows which
 * slot grew until user memory ran out. */
static void asura_dump_chunk_manager(void)
{
    const uint32_t mgr = vm_read32(0x019AF8F8u - 0x5BA0u);
    if (mgr < 0x10000u) return;
    fprintf(stderr, "[asura chunks] mgr=0x%08X dirty=%u init=%u port=%u queue=%u alloc=0x%08X\n",
            mgr, vm_read8(mgr), vm_read8(mgr + 1), vm_read32(mgr + 4),
            vm_read32(mgr + 0x10), vm_read32(mgr + 0x14));
    unsigned total = 0;
    for (uint32_t i = 0; i < 512; ++i) {
        const uint32_t slot = mgr + 0x200 + i * 16;
        const uint32_t head = vm_read32(slot);
        if (!head) continue;
        unsigned nodes = 0, backed = 0;
        uint64_t bytes = 0;
        uint32_t gen = vm_read32(head), last = head;
        for (uint32_t n = head; n >= 0x10000u && nodes < 100000; n = vm_read32(n + 0x14)) {
            ++nodes; last = n;
            if (vm_read32(n + 4)) { ++backed; bytes += vm_read32(n + 0xC); }
        }
        total += backed;
        fprintf(stderr, "[asura chunks] slot %3u head=0x%08X cur=0x%08X pend=%u lock=%u "
                "nodes=%u backed=%u bytes=%llu gen(head)=%u last=0x%08X last.rem=%u/%u\n",
                i, head, vm_read32(slot + 4), vm_read32(slot + 8), vm_read32(slot + 0xC),
                nodes, backed, (unsigned long long)bytes, gen, last,
                vm_read32(last + 0x10), vm_read32(last + 0xC));
    }
    fprintf(stderr, "[asura chunks] backed chunks total=%u\n", total);
}

static void asura_error_serialize(ppu_context* ctx)
{
    const uint32_t address = (uint32_t)ctx->gpr[4];
    char message[4096];
    size_t n = 0;
    for (uint32_t i = 0; address && i < 1024; ++i) {
        const uint64_t ea = (uint64_t)address + i * 2;
        if (ea + 1 > UINT32_MAX) break;
        const uint32_t c = vm_read16((uint32_t)ea);
        if (!c) break;
        if (c < 0x80) message[n++] = (char)c;
        else if (c < 0x800) {
            message[n++] = (char)(0xC0 | (c >> 6));
            message[n++] = (char)(0x80 | (c & 63));
        } else {
            message[n++] = (char)(0xE0 | (c >> 12));
            message[n++] = (char)(0x80 | ((c >> 6) & 63));
            message[n++] = (char)(0x80 | (c & 63));
        }
    }
    message[n] = 0;
    fprintf(stderr, "[asura error] lr=0x%08X category=0x%X: %s\n",
            (uint32_t)ctx->lr, (uint32_t)ctx->gpr[5], message);
    ppu_dump_guest_stack(ctx, "asura-error");
    asura_dump_chunk_manager();
    func_000F3BE0(ctx);
}

/* The SDK stub declares GetOverlayTableSize(path, size_out), but this title
 * passes one in-memory SPU ELF and uses the RETURN VALUE as the byte count.
 * Its NOT_INITIALIZED error becomes a 0x80410785-byte allocation. Match the
 * current RPCS3 fallback (no overlay table) for these two imports until the
 * runtime implements SPU overlay tables. This does not implement overlays.
 * Reference: RPCS3/rpcs3, Emu/Cell/Modules/cellOvis.cpp. */
static void asura_ovis_no_table(ppu_context* ctx)
{
    fprintf(stderr, "[asura ovis] no overlay table, arg=0x%08X lr=0x%08X\n",
            (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr);
    ctx->gpr[3] = 0;
}

extern "C" void ps3_install_title_hooks(void)
{
    ps3_hle_register_ctx(0x91066667, "cellSpursQueuePopBody", asura_spurs_queue_pop);
    ps3_hle_register_ctx(0x92CFF6ED, "cellSpursQueuePushBody", asura_spurs_queue_push);
    const char* e = getenv("ASURA_OVIS_COMPAT");
    if (!e || e[0] != '0') {
        ps3_hle_register_ctx(0x82F294B2, "cellOvisGetOverlayTableSize", asura_ovis_no_table);
        ps3_hle_register_ctx(0xA876C911, "cellOvisInitializeOverlayTable", asura_ovis_no_table);
    }
}

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

extern "C" void ps3_load_prx_modules(void)
{
    if (const char* e = getenv("ASURA_GUEST_LOG"); e && e[0] == '1')
        ppu_register_function(0x000F3BE0, asura_error_serialize);
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
