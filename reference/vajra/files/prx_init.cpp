#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "prx_loader.h"
#include "spurs.h"

struct ppu_context;

extern "C" const prx_module* libsre_module(void);
extern "C" const prx_module* libresc_module(void);
extern "C" void libsre_spu_register(void);
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

static void register_lifted(uint32_t addr, void (*host)(void*))
{
    ppu_register_function(addr, reinterpret_cast<void (*)(ppu_context*)>(host));
}

extern "C" void ps3_load_prx_modules(void)
{
    for (const prx_module* m : { libsre_module(), libresc_module() }) {
        if (!prx_load_module(m, register_lifted).ok) {
            std::fprintf(stderr, "[prx] %s failed to load\n", m->name);
            return;
        }
    }
    spurs_register(0x30021480u, 0x30023680u, 0x3002A180u, 0x30026300u);
    libsre_spu_register();
}
