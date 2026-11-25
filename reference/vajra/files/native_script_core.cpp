#include "guest.h"
#include "pc_options.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

extern "C" const int32_t* ppu_exec_tab;
extern "C" uint32_t ppu_exec_lo, ppu_exec_n;

namespace {

static inline void (*fast_host_fn(uint32_t code))(ppu_context*)
{
    const uint32_t d = code - ppu_exec_lo;
    const uint32_t i = (d >> 2) | (d << 30);
    if (__builtin_expect(i < ppu_exec_n, 1)) {
        const int32_t off = ppu_exec_tab[i];
        if (__builtin_expect(off != 0, 1))
            return (void (*)(ppu_context*))((const char*)ps3_indirect_call + off);
    }
    return ps3_indirect_call;
}

constexpr uint32_t k_natives = 0x01A01824;
constexpr uint32_t k_casts = 0x01A09828;
constexpr uint32_t k_property = 0x019B1BB4;
constexpr uint32_t k_prop_addr = 0x019B1BD4;
constexpr uint32_t k_prop_object = 0x019B1BD8;
constexpr uint32_t k_log = 0x019B1B84;
constexpr uint32_t k_context_hook = 0x019B1E68;
constexpr uint32_t k_bool_class = 0x019C28C8;
constexpr uint32_t k_array_class = 0x019C28E8;
constexpr uint32_t k_function_class = 0x019BA34C;
constexpr uint32_t k_runtime_flags = 0x019EEC88;
constexpr uint32_t k_frame_vtable = 0x016937B0;
constexpr uint32_t k_core = 0x01608814;
constexpr uint32_t k_none_text = 0x01691EEC;
constexpr uint32_t k_null_owner_text = 0x01692C80;
constexpr uint32_t k_empty_text = 0x01607ED4;
constexpr uint32_t k_let_none = 0x0160B09C;
constexpr uint32_t k_context_none = 0x0160B0EC;
constexpr uint32_t k_context_no_property = 0x0160B114;
constexpr uint32_t k_context_bad_size = 0x01694CB8;
constexpr uint32_t k_divide_zero = 0x0160B1DC;
constexpr uint32_t k_param_bounds = 0x0160AAE0;
constexpr uint32_t k_member_bounds = 0x0160AA84;
constexpr uint32_t k_let_sink = 0x01A0A3F0;
constexpr uint32_t k_float_zero = 0x0002E050;
constexpr uint32_t k_vector_divide = 0x00051438;
constexpr uint32_t k_engine = 0x019CCE80;
constexpr uint32_t k_object_remote = 0x0003F270;
constexpr uint32_t k_actor_remote = 0x004A2E48;
constexpr uint32_t k_slots = 0x1000;

struct Vm {
    ppu_context* ctx;
    ppu_mm m;

    explicit Vm(ppu_context* c) : ctx(c), m(ppu_mm_take()) {}

    const ppu_mm* mm() const
    {
        __builtin_assume_separate_storage(m.vb, this);
        return &m;
    }

    uint8_t r8(uint32_t a) const { return vm_read8_mm(mm(), a); }
    uint16_t r16(uint32_t a) const { return vm_read16_mm(mm(), a); }
    uint32_t r32(uint32_t a) const { return vm_read32_mm(mm(), a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(mm(), a); }
    void w8(uint32_t a, uint8_t v) const { vm_write8_mm(mm(), a, v); }
    void w32(uint32_t a, uint32_t v) const { vm_write32_mm(mm(), a, v); }
    void w64(uint32_t a, uint64_t v) const { vm_write64_mm(mm(), a, v); }

    void frame(uint32_t sp, uint32_t size, uint64_t lr) const
    {
        (void)lr;
        w64(sp - size, sp);
    }

    uint32_t stack_alloc(uint32_t sp, uint32_t size) const
    {
        const uint64_t chain = r64(sp);
        sp -= (((size + 7) & ~7u) + 0x7F) & ~0xFu;
        w64(sp, chain);
        return sp;
    }

    template <typename... A>
    uint32_t call(uint32_t sp, uint64_t lr, void (*fn)(ppu_context*), A... a)
    {
        uint64_t* r = ctx->gpr + 3;
        ((*r++ = (uint64_t)a), ...);
        ctx->gpr[1] = sp;
        ctx->lr = lr;
        fn(ctx);
        drain(ctx);
        return (uint32_t)ctx->gpr[3];
    }

    template <typename... A>
    uint32_t icall(uint32_t sp, uint64_t lr, uint32_t code, A... a)
    {
        ctx->ctr = code;
        return call(sp, lr, fast_host_fn(code), a...);
    }

    template <typename... A>
    uint32_t vcall(uint32_t sp, uint64_t lr, uint32_t self, uint32_t slot, A... a)
    {
        return icall(sp, lr, r32(r32(r32(self) + slot)), self, a...);
    }

    void zero(uint32_t sp, uint64_t lr, uint32_t dst, uint32_t n)
    {
        if (!n) return;
        if (plain_store(m, dst, n)) {
            memset(m.vb + dst, 0, n);
            vm_track_write(dst, n);
        }
        else call(sp, lr, func_00F35E2C, dst, 0, n);
    }
};

using Exec = void (*)(Vm&, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t);

void step(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result, uint32_t op);
void call_index(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result, uint32_t index);
void call_cast(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result, uint32_t index);
void process_internal(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result);

double fp_quiet(double x)
{
    return from_bits64(to_bits64(x) | 0x0008000000000000ull);
}

double fp_result(double r)
{
    return r == r ? r : from_bits64(0x7FF8000000000000ull);
}

double fp_bin(double a, double b, double r)
{
    if (a != a) return fp_quiet(a);
    if (b != b) return fp_quiet(b);
    return fp_result(r);
}

double fadds(double a, double b) { return single(fp_bin(a, b, a + b)); }
double fsubs(double a, double b) { return single(fp_bin(a, b, a - b)); }
double fmuls(double a, double b) { return single(fp_bin(a, b, a * b)); }
double fdivs(double a, double b) { return single(fp_bin(a, b, a / b)); }

double fmadds(double a, double c, double b)
{
    if (a != a) return single(fp_quiet(a));
    if (b != b) return single(fp_quiet(b));
    if (c != c) return single(fp_quiet(c));
    return single(fp_result(std::fma(a, c, b)));
}

double lfs(uint32_t bits) { return from_bits(bits); }

void next(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 1);
    step(v, sp, lr, obj, stack, result, v.r8(code));
}

void arg(Vm& v, uint32_t sp, uint64_t lr, uint32_t stack, uint32_t at)
{
    next(v, sp, lr, v.r32(stack + 0x14), stack, at);
}

void arg0(Vm& v, uint32_t sp, uint64_t lr, uint32_t stack, uint32_t at)
{
    v.w32(at, 0);
    arg(v, sp, lr, stack, at);
}

void finish(Vm& v, uint32_t stack)
{
    v.w32(stack + 0x18, v.r32(stack + 0x18) + 1);
}

uint32_t remote(Vm& v, uint32_t sp, uint64_t lr, uint32_t self, uint32_t function, uint32_t a, uint32_t b)
{
    const uint32_t code = v.r32(v.r32(v.r32(self) + 0x10C));
    if (code == k_object_remote) return 0;
    if (code == k_actor_remote && !(v.r32(function + 0x74) & 0x2000) && !(v.r32(self + 0xAC) & 0x10000000) &&
        !(v.r64(self + 8) & (1ull << 61)) && !v.r32(v.r32(k_engine) + 0x120) && !v.r8(v.r32(self + 0xD4) + 0x398))
        return 0;
    return v.icall(sp, lr, code, self, function, a, b);
}

uint32_t find_probe(const Vm& v, uint32_t level, uint32_t index, uint64_t name)
{
    const uint8_t* vb = v.m.vb;
    auto rd32 = [vb](uint32_t a) {
        uint32_t x;
        memcpy(&x, vb + a, 4);
        return __builtin_bswap32(x);
    };
    const uint32_t size = rd32(level + 0xB8);
    if (!size) return 0;
    const uint32_t buckets = rd32(level + 0xB4);
    const uint32_t table = buckets ? buckets : level + 0xB0;
    const uint32_t elements = rd32(level + 0x80);
    const uint64_t name_be = __builtin_bswap64(name);
    for (uint32_t i = rd32(table + 4u * (index & (size - 1u))); (int32_t)i != -1;) {
        const uint8_t* ep = vb + elements + 20u * i;
        uint64_t k;
        memcpy(&k, ep, 8);
        if (k == name_be) {
            uint32_t val;
            memcpy(&val, ep + 8, 4);
            return __builtin_bswap32(val);
        }
        uint32_t nxt;
        memcpy(&nxt, ep + 12, 4);
        i = __builtin_bswap32(nxt);
    }
    return 0;
}

uint32_t find_walk(const Vm& v, uint32_t level, uint32_t index, uint64_t name)
{
    const uint8_t* vb = v.m.vb;
    for (; level;) {
        if (const uint32_t found = find_probe(v, level, index, name)) return found;
        uint32_t next_lvl;
        memcpy(&next_lvl, vb + level + 0x40, 4);
        level = __builtin_bswap32(next_lvl);
    }
    return 0;
}

uint32_t find_checked(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint64_t name, uint64_t global)
{
    const uint32_t index = (uint32_t)(name >> 32);
    uint32_t found = 0;
    const uint32_t frame = v.r32(self + 0x14);
    const uint32_t state = frame ? v.r32(frame + 0x28) : 0;
    if (state && (int32_t)global == 0) found = find_walk(v, state, index, name);
    if (!found) found = find_walk(v, v.r32(self + 0x34), index, name);
    if (__builtin_expect(found != 0, 1)) return found;
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    v.w32(f + 0x38, (uint32_t)(name >> 32));
    v.w32(f + 0x3C, (uint32_t)name);
    v.w64(sp0 + 0x38, name);
    return v.call(sp0, lr0, func_000D78A0_lifted, self, name, global);
}

void copy_value_fast(Vm& v, uint32_t sp, uint64_t lr, uint32_t prop, uint32_t result, uint32_t addr)
{
    const uint32_t fn = v.r32(v.r32(v.r32(prop) + 0x15C));
    if (__builtin_expect(fn == 0x000E5548u, 1)) {
        const int32_t count = (int32_t)v.r32(prop + 0x40);
        if (count == 1) {
            v.w32(result, v.r32(addr));
            return;
        }
        if (count > 1) {
            const uint32_t stride = v.r32(prop + 0x44);
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t off = stride * (uint32_t)i;
                v.w32(result + off, v.r32(addr + off));
            }
            return;
        }
        return;
    }
    v.icall(sp, lr, fn, prop, result, addr, 0, 0, 0);
}

void copy_value_lazy(Vm& v, uint32_t sp0, uint64_t lr0, uint64_t lr, uint32_t prop, uint32_t result, uint32_t addr)
{
    const uint32_t fn = v.r32(v.r32(v.r32(prop) + 0x15C));
    if (__builtin_expect(fn == 0x000E5548u, 1)) {
        const int32_t count = (int32_t)v.r32(prop + 0x40);
        if (count == 1) {
            v.w32(result, v.r32(addr));
            return;
        }
        if (count > 1) {
            const uint32_t stride = v.r32(prop + 0x44);
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t off = stride * (uint32_t)i;
                v.w32(result + off, v.r32(addr + off));
            }
            return;
        }
        return;
    }
    v.frame(sp0, 0x70, lr0);
    v.icall(sp0 - 0x70, lr, fn, prop, result, addr, 0, 0, 0);
}

void call_native(Vm& v, uint32_t sp, uint64_t lr, uint32_t self, uint32_t function, uint32_t stack, uint32_t result)
{
    const uint32_t target = self + v.r32(function + 0x8C);
    uint32_t fn = v.r32(function + 0x88);
    if (fn & 1) fn = v.r32(v.r32(target) + fn - 1);
    v.icall(sp, lr, v.r32(fn), target, stack, result);
}

bool is_bool(Vm& v, uint32_t sp, uint32_t prop, uint64_t lr_class, uint64_t lr_init)
{
    uint32_t cls = v.r32(k_bool_class);
    const uint32_t prop_class = v.r32(prop + 0x34);
    if (!cls) {
        const uint32_t made = v.call(sp, lr_class, func_000DFF70, k_core);
        v.w32(k_bool_class, made);
        v.call(sp, lr_init, func_000DFDA8, made);
        cls = v.r32(k_bool_class);
    }
    return prop_class == cls;
}

void link_out(Vm& v, uint32_t& last, uint32_t out)
{
    if (const uint32_t head = v.r32(last)) {
        v.w32(head + 8, out);
        last = v.r32(last) + 8;
    } else {
        v.w32(last, out);
    }
}

void call_script(Vm& v, uint32_t f, uint32_t self, uint32_t stack, uint32_t result, uint32_t function)
{
    const uint32_t size = v.r32(function + 0x48);
    uint32_t sp = f, frame = 0;
    if (size) {
        sp = v.stack_alloc(f, size);
        frame = sp + 0x70;
        v.zero(sp, 0x00040870, frame, size);
    }
    const uint32_t nf = f + 0x90;
    const uint32_t code0 = v.r32(function + 0x4C);
    if (__builtin_expect((nf >> 12) == ((nf + 39u) >> 12) && !ppu_slow_page[nf >> 12], 1)) {
        const uint32_t hdr[10] = {
            __builtin_bswap32(k_frame_vtable), __builtin_bswap32(1u), 0u, __builtin_bswap32(1u),
            __builtin_bswap32(function), __builtin_bswap32(self), __builtin_bswap32(code0),
            __builtin_bswap32(frame), __builtin_bswap32(stack), 0u
        };
        memcpy(v.m.vb + nf, hdr, 40);
        if (__builtin_expect(*(const volatile unsigned char*)&ppu_slow_page[nf >> 12], 0)) ppu_store_recheck(nf);
    } else {
        v.w32(f + 0x98, 0);
        v.w32(f + 0x94, 1);
        v.w32(f + 0x9C, 1);
        v.w32(f + 0xA0, function);
        v.w32(f + 0x90, k_frame_vtable);
        v.w32(f + 0xA4, self);
        v.w32(f + 0xA8, code0);
        v.w32(f + 0xAC, frame);
        v.w32(f + 0xB0, stack);
        v.w32(f + 0xB4, 0);
    }
    uint32_t last = f + 0xB4;
    uint32_t prop = v.r32(function + 0x44);
    uint32_t code = v.r32(stack + 0x18);
    if (v.r8(code) != 0x16) {
        do {
            const uint32_t uc = v.r32(k_runtime_flags);
            v.w32(k_prop_addr, 0);
            v.w32(k_prop_object, 0);
            if (__builtin_expect((uc & 2u) != 0, 0)) v.w32(k_runtime_flags, uc & ~2u);
            uint32_t addr;
            if (v.r64(prop + 0x48) & 0x100) {
                next(v, sp, 0x00040958, v.r32(stack + 0x14), stack, 0);
                sp = v.stack_alloc(sp, 0x10);
                addr = v.r32(k_prop_addr);
                if (!addr) addr = v.r32(prop + 0x54) + frame;
                v.w32(sp + 0x74, addr);
                v.w32(sp + 0x70, prop);
                link_out(v, last, sp + 0x70);
                if (const uint32_t obj = v.r32(k_prop_object))
                    if (const uint32_t gp = v.r32(k_property))
                        if ((v.r64(gp + 0x48) & 0x20) && !(v.r64(prop + 0x48) & 2)) v.vcall(sp, 0x00040C4C, obj, 0xC4, gp);
            } else {
                code = v.r32(stack + 0x18);
                const uint32_t locals = v.r32(f + 0xAC);
                v.w32(stack + 0x18, code + 1);
                addr = locals + v.r32(prop + 0x54);
                step(v, sp, 0x00040B44, v.r32(stack + 0x14), stack, addr, v.r8(code));
                if (prop && is_bool(v, sp, prop, 0x00040C24, 0x00040C30) && v.r32(addr)) v.w32(addr, 0x80000000u);
            }
            if (v.r64(prop + 0x48) & 0x10) {
                if (prop && (v.r32(k_runtime_flags) & 2) && (v.r32(v.r32(prop + 0x34) + 0xC0) & 0x100000))
                    v.call(sp, 0x00040A50, func_000EAEEC, prop, addr);
                const uint32_t ncode = v.r32(f + 0xA8);
                v.w32(f + 0xA8, ncode + 1);
                step(v, sp, 0x00040AA0, self, nf, addr, v.r8(ncode));
                if (prop && is_bool(v, sp, prop, 0x00040C08, 0x00040C14) && v.r32(addr)) v.w32(addr, 0x80000000u);
            }
            code = v.r32(stack + 0x18);
            prop = v.r32(prop + 0x3C);
        } while (v.r8(code) != 0x16);
    }
    v.w32(stack + 0x18, code + 1);
    while (prop && (v.r64(prop + 0x48) & 0x10)) {
        uint32_t next_sp = sp;
        v.w32(k_prop_addr, 0);
        v.w32(k_prop_object, 0);
        if (v.r64(prop + 0x48) & 0x100) {
            next_sp = v.stack_alloc(sp, 0x10);
            v.w32(next_sp + 0x70, prop);
            v.w32(next_sp + 0x74, frame + v.r32(prop + 0x54));
            link_out(v, last, next_sp + 0x70);
        }
        const uint32_t ncode = v.r32(f + 0xA8);
        const uint32_t addr = frame + v.r32(prop + 0x54);
        v.w32(f + 0xA8, ncode + 1);
        step(v, next_sp, 0x00040DD8, self, nf, addr, v.r8(ncode));
        prop = v.r32(prop + 0x3C);
        sp = next_sp;
    }
    for (uint32_t p = v.r32(function + 0x84); p; p = v.r32(p + 0x3C))
        if (v.r32(v.r32(p + 0x34) + 0xC0) & 0x100000) v.call(sp, 0x00040CF8, func_000EAEEC, p, v.r32(f + 0xAC) + v.r32(p + 0x54));
    process_internal(v, sp, 0x00040E2C, self, nf, result);
    if (result && camera_assist_blocked(function)) v.w32(result, 0);
    for (uint32_t p = v.r32(function + 0x64); p; p = v.r32(p + 0x5C)) {
        const uint64_t flags = v.r64(p + 0x48);
        if (!(flags & 0x100) || (flags & 0x10)) v.vcall(sp, 0x00040E8C, p, 0x168, v.r32(f + 0xAC) + v.r32(p + 0x54));
    }
}

void call_function(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result, uint32_t function)
{
    const uint32_t f = sp0 - 0x940;
    v.frame(sp0, 0x940, lr0);
    const uint32_t watch = pc_options_call(self, function);
    if (v.r16(function + 0x78)) {
        call_native(v, f, 0x000407C4, self, function, stack, result);
    } else {
        const uint32_t flags = v.r32(function + 0x74);
        if (flags & 0x400) {
            if (!remote(v, f, 0x00040EF0, self, function, f + 0xB8, stack))
                call_native(v, f, 0x00040F30, self, function, stack, result);
            else
                v.call(f, 0x00040F60, func_000404B8, self, stack, result, function);
        } else if (!(flags & 2)) {
            remote(v, f, 0x00040EB4, self, function, f + 0x4B8, stack);
            v.call(f, 0x00040EC8, func_000404B8, self, stack, result, function);
        } else {
            call_script(v, f, self, stack, result, function);
        }
    }
    if (__builtin_expect(watch != 0, 0) && result) pc_options_returned(watch, result);
    const uint32_t rf = v.r32(k_runtime_flags);
    if (__builtin_expect((rf & 2u) != 0, 0)) v.w32(k_runtime_flags, rf & ~2u);
}

bool probing(Vm& v, uint32_t f, uint32_t self, uint32_t node)
{
    int32_t index;
    if ((int32_t)v.r32(node + 0x20) == -1) {
        v.call(f, 0x000404B0, func_000A4148, f + 0x70, k_none_text, 1, 1);
        index = (int32_t)v.r32(f + 0x70);
    } else {
        index = (int32_t)v.r32(node + 0x2C);
    }
    if (index < 0x12C || index >= 0x14C) return true;
    const uint32_t state = v.r32(self + 0x14);
    return !state || (v.r32(state + 0x2C) & (1u << (index - 0x12C)));
}

void process_internal(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t node = v.r32(stack + 0x10);
    const uint32_t single = v.r32(node + 0x74) & 0x20;
    if (!remote(v, f, 0x00040264, self, node, v.r32(stack + 0x1C), 0) && probing(v, f, self, v.r32(stack + 0x10))) {
        const uint64_t flags = v.r64(self + 8);
        if (!((flags & 2) && single)) {
            if (single) v.w64(self + 8, flags | 2);
            const uint32_t obj = v.r32(stack + 0x14);
            uint32_t code = v.r32(stack + 0x18);
            uint8_t op = v.r8(code);
            while (op != 4) {
                v.w32(stack + 0x18, code + 1);
                step(v, f, 0x000403D0, obj, stack, f + 0x78, op);
                code = v.r32(stack + 0x18);
                op = v.r8(code);
            }
            v.w32(stack + 0x18, code + 2);
            step(v, f, 0x00040458, obj, stack, result, v.r8(code + 1));
            if (single) v.w64(self + 8, v.r64(self + 8) & ~2ull);
            return;
        }
    }
    if (const uint32_t ret = v.call(f, 0x000402D0, func_000400A8, v.r32(stack + 0x10))) {
        if (v.r64(ret + 0x48) & 0x400000) v.vcall(f, 0x00040308, ret, 0x168, result);
        v.zero(f, 0x00040324, result, v.r32(ret + 0x40) * v.r32(ret + 0x44));
    }
}

void named_call(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result, uint64_t global, uint64_t lr_find,
                uint64_t lr_call)
{
    (void)lr0;
    const uint32_t f = sp0 - 0x90;
    const uint32_t code = v.r32(stack + 0x18);
    const uint64_t name = v.r64(code);
    v.w32(stack + 0x18, code + 8);
    call_function(v, f, lr_call, self, stack, result, find_checked(v, f, lr_find, self, name, global));
}

void virtual_function(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    named_call(v, sp0, lr0, self, stack, result, 0, 0x00047A60, 0x00047A74);
}

void global_function(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    named_call(v, sp0, lr0, self, stack, result, 1, 0x00047B4C, 0x00047B60);
}

void final_function(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    (void)lr0;
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t function = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    call_function(v, sp0 - 0x70, 0x00047AB4, self, stack, result, function);
}

void variable(Vm& v, uint32_t sp0, uint64_t lr0, uint64_t lr, uint32_t stack, uint32_t result, uint32_t base, uint32_t object)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    const uint32_t addr = base + v.r32(prop + 0x54);
    if (__builtin_expect(!ppu_slow_page[k_property >> 12], 1)) {
        const uint32_t p_be = __builtin_bswap32(prop);
        const uint32_t a_be = __builtin_bswap32(addr);
        const uint32_t o_be = __builtin_bswap32(object);
        memcpy(v.m.vb + k_property, &p_be, 4);
        memcpy(v.m.vb + k_prop_addr, &a_be, 4);
        memcpy(v.m.vb + k_prop_object, &o_be, 4);
        if (__builtin_expect(*(const volatile unsigned char*)&ppu_slow_page[k_property >> 12], 0)) ppu_store_recheck(k_property);
    } else {
        v.w32(k_property, prop);
        v.w32(k_prop_object, object);
        v.w32(k_prop_addr, addr);
    }
    if (result) copy_value_lazy(v, sp0, lr0, lr, prop, result, addr);
}

void local_variable(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    variable(v, sp0, lr0, 0x0004147C, stack, result, v.r32(stack + 0x1C), 0);
}

void instance_variable(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    variable(v, sp0, lr0, 0x00041514, stack, result, self, self);
}

void local_out_variable(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    v.w32(k_prop_object, 0);
    v.w32(k_property, prop);
    uint32_t out = v.r32(stack + 0x24);
    while (v.r32(out) != prop) out = v.r32(out + 8);
    const uint32_t addr = v.r32(out + 4);
    v.w32(k_prop_addr, addr);
    if (result) copy_value_lazy(v, sp0, lr0, 0x00041AC8, prop, result, addr);
}

void bool_variable(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    v.frame(sp0, 0x90, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 1);
    const uint32_t prop = (uint32_t)v.r64(code + 1);
    step(v, sp0 - 0x90, 0x00045CF0, self, stack, 0, v.r8(code));
    v.w32(k_property, prop);
    v.w32(k_prop_object, self);
    if (!result) return;
    const uint32_t addr = v.r32(k_prop_addr);
    v.w32(result, addr && (v.r32(addr) & v.r32(prop + 0x64)) ? 1 : 0);
}

void struct_member(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 8);
    const uint32_t member = (uint32_t)v.r64(code);
    const uint32_t owner = (uint32_t)v.r64(code + 8);
    uint32_t at = code + 0x11;
    v.w32(stack + 0x18, at);
    uint32_t sp = f, copy = 0;
    if (v.r8(code + 0x10)) {
        const uint32_t align = v.r32(owner + 0x58);
        const uint32_t size = (v.r32(owner + 0x48) + align - 1) & ~(align - 1);
        if (size) {
            sp = v.stack_alloc(f, size);
            copy = sp + 0x70;
        }
        v.zero(sp, 0x00048AF8, copy, size);
        at = v.r32(stack + 0x18);
    }
    v.w32(stack + 0x18, at + 1);
    const uint32_t modifies = v.r8(at);
    const uint32_t flags = v.r32(k_runtime_flags);
    if (v.r8(at + 1) == 0x10) v.w32(k_runtime_flags, flags | 4);
    v.w32(k_prop_addr, 0);
    next(v, sp, 0x000489DC, self, stack, copy);
    v.w32(k_runtime_flags, flags);
    if (modifies)
        if (const uint32_t obj = v.r32(k_prop_object))
            if (const uint32_t gp = v.r32(k_property))
                if (v.r64(gp + 0x48) & 0x20) v.vcall(sp, 0x00048A30, obj, 0xC4, gp);
    v.w32(k_prop_object, self);
    uint32_t addr = v.r32(k_prop_addr);
    v.w32(k_property, member);
    if (addr) {
        addr += v.r32(member + 0x54);
        v.w32(k_prop_addr, addr);
    }
    if (!copy) {
        if (!result) return;
        if (addr) {
            copy_value_fast(v, sp, 0x00048BDC, member, result, addr);
            return;
        }
        if (v.r64(member + 0x48) & 0x400000) v.vcall(sp, 0x00048B90, member, 0x168, result);
        v.zero(sp, 0x00048BAC, result, v.r32(member + 0x40) * v.r32(member + 0x44));
        return;
    }
    if (result) copy_value_fast(v, sp, 0x00048C48, member, result, copy + v.r32(member + 0x54));
    for (uint32_t p = v.r32(owner + 0x64); p; p = v.r32(p + 0x5C)) v.vcall(sp, 0x00048C04, p, 0x168, copy + v.r32(p + 0x54));
}

void literal(Vm& v, uint32_t sp, uint32_t at, uint32_t text, uint64_t lr_len, uint64_t lr_alloc, uint64_t lr_copy)
{
    uint32_t len = v.r16(text) ? v.call(sp, lr_len, func_00F2CE40, text) + 1 : 0;
    v.w32(at, 0);
    v.w32(at + 4, len);
    v.w32(at + 8, len);
    uint32_t data = 0;
    if (len) {
        data = v.call(sp, lr_alloc, func_0002AE4C, 0, len << 1, 8);
        len = v.r32(at + 4);
        v.w32(at, data);
    }
    if (len) v.call(sp, lr_copy, func_00F297C8, data, text, len << 1);
}

void object_name(Vm& v, uint32_t sp, uint32_t at, uint32_t obj, uint64_t lr_name, uint64_t lr_len, uint64_t lr_alloc, uint64_t lr_copy)
{
    if ((int32_t)v.r32(obj + 0x20) == -1) literal(v, sp, at, k_none_text, lr_len, lr_alloc, lr_copy);
    else v.call(sp, lr_name, func_000AEB38, at, obj + 0x2C);
}

uint32_t text(Vm& v, uint32_t at)
{
    return v.r32(at + 4) ? v.r32(at) : k_empty_text;
}

void release(Vm& v, uint32_t sp, uint32_t at, uint64_t lr)
{
    const uint32_t data = v.r32(at);
    v.w32(at + 8, 0);
    v.w32(at + 4, 0);
    if (data) {
        v.call(sp, lr, func_0002ADF0, data);
        v.w32(at, 0);
    }
}

void dyn_error(Vm& v, uint32_t f, uint32_t self, uint32_t stack, uint32_t result, uint32_t array, uint32_t addr)
{
    if (v.r32(v.r32(array + 0x28) + 0x34) == v.r32(k_function_class)) {
        object_name(v, f, f + 0x94, array, 0x00042A3C, 0x00042980, 0x000429B0, 0x000429D4);
        v.call(f, 0x00042600, func_000AA46C, stack, 0x315, k_param_bounds, text(v, f + 0x94), (int64_t)(int32_t)v.r32(f + 0x90),
               (int64_t)(int32_t)v.r32(addr + 4));
        release(v, f, f + 0x94, 0x00042620);
    } else {
        if (!self) literal(v, f, f + 0xA0, k_null_owner_text, 0x00042720, 0x00042750, 0x00042774);
        else object_name(v, f, f + 0xA0, self, 0x00042A28, 0x00042864, 0x00042894, 0x000428B8);
        const uint32_t owner = text(v, f + 0xA0);
        object_name(v, f, f + 0xAC, array, 0x00042A18, 0x000428F8, 0x00042928, 0x0004294C);
        v.call(f, 0x000427D8, func_000AA46C, stack, 0x315, k_member_bounds, owner, text(v, f + 0xAC), (int64_t)(int32_t)v.r32(f + 0x90),
               (int64_t)(int32_t)v.r32(addr + 4));
        release(v, f, f + 0xAC, 0x000427F8);
        release(v, f, f + 0xA0, 0x0004281C);
    }
    v.w32(k_prop_addr, 0);
    v.w32(k_prop_object, 0);
    if (result) v.zero(f, 0x00042658, result, v.r32(v.r32(array + 0x64) + 0x44));
}

bool child_of(Vm& v, uint32_t c, uint32_t base)
{
    for (; c; c = v.r32(c + 0x40))
        if (c == base) return true;
    return false;
}

uint32_t dyn_reserve(Vm& v, uint32_t sp, uint32_t addr, uint32_t len, uint32_t elem, uint64_t lr_slack, uint64_t lr_alloc)
{
    const int32_t max = (int32_t)v.r32(addr + 8);
    uint32_t data = v.r32(addr);
    v.w32(addr + 4, len);
    if ((int32_t)len > max) {
        const uint32_t slack = v.call(sp, lr_slack, func_00010230, (int64_t)(int32_t)len, (int64_t)max, elem);
        data = v.r32(addr);
        v.w32(addr + 8, slack);
        if (data | slack) {
            data = v.call(sp, lr_alloc, func_0002AE4C, data, elem * slack, 8);
            v.w32(addr, data);
        }
    }
    return data;
}

void dyn_shrink(Vm& v, uint32_t sp, uint32_t addr, uint32_t len, uint32_t elem, uint64_t lr_slack, uint64_t lr_alloc)
{
    const int32_t max = (int32_t)v.r32(addr + 8);
    v.w32(addr + 4, len);
    const uint32_t slack = v.call(sp, lr_slack, func_00010230, (int64_t)(int32_t)len, (int64_t)max, elem);
    if (slack == v.r32(addr + 8)) return;
    const uint32_t data = v.r32(addr);
    v.w32(addr + 8, slack);
    if (data | slack) v.w32(addr, v.call(sp, lr_alloc, func_0002AE4C, data, elem * slack, 8));
}

void dyn_grow(Vm& v, uint32_t f, uint32_t array, uint32_t addr, uint32_t index, uint32_t num)
{
    const uint32_t extra = index - num;
    const uint32_t elem = v.r32(v.r32(array + 0x64) + 0x44);
    const uint32_t data = dyn_reserve(v, f, addr, num + extra + 1, elem, 0x0004249C, 0x000424C4);
    v.zero(f, 0x000424EC, data + num * elem, extra * elem + elem);
    const uint32_t inner = v.r32(array + 0x64);
    if (!inner || !(v.r32(v.r32(inner + 0x34) + 0xC0) & 0x100000) || !v.r32(v.r32(inner + 0x64) + 0x88)) return;
    if ((int32_t)v.r32(f + 0x90) <= (int32_t)num) return;
    uint32_t i = num;
    do {
        v.call(f, 0x00042548, func_000EAEEC, inner, v.r32(addr) + i * v.r32(v.r32(array + 0x64) + 0x44));
        i++;
    } while ((int32_t)i < (int32_t)v.r32(f + 0x90));
}

void dyn_element(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    if (!v.r32(k_function_class)) {
        v.call(sp0, lr0, func_00042288_lifted, self, stack, result);
        return;
    }
    const uint32_t f = sp0 - 0x100;
    v.frame(sp0, 0x100, lr0);
    arg0(v, f, 0x00042324, stack, f + 0x90);
    v.w32(k_property, 0);
    next(v, f, 0x0004237C, self, stack, 0);
    const uint32_t array = v.r32(k_property);
    v.w32(k_prop_object, self);
    if (!array) return;
    const uint32_t addr = v.r32(k_prop_addr);
    if (!addr) return;
    int32_t index = (int32_t)v.r32(f + 0x90);
    const int32_t num = (int32_t)v.r32(addr + 4);
    if (index < 0 || index >= num) {
        if (index < 0 || result || (v.r32(k_runtime_flags) & 4)) return dyn_error(v, f, self, stack, result, array, addr);
        dyn_grow(v, f, array, addr, (uint32_t)index, (uint32_t)num);
        index = (int32_t)v.r32(f + 0x90);
    }
    const uint32_t inner = v.r32(array + 0x64);
    const uint32_t at = v.r32(addr) + (uint32_t)index * v.r32(inner + 0x44);
    v.w32(k_prop_addr, at);
    if (result) v.vcall(f, 0x00042A04, inner, 0x158, result, at, 0, 0, 0);
}

void dyn_length(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    v.frame(sp0, 0x80, lr0);
    v.w32(k_property, 0);
    next(v, sp0 - 0x80, 0x00042AC4, self, stack, 0);
    v.w32(k_prop_object, self);
    const uint32_t array = v.r32(k_prop_addr);
    if (!array) return;
    if (result) v.w32(result, v.r32(array + 4));
    else v.w32(k_runtime_flags, v.r32(k_runtime_flags) | 1);
}

void jump_if_not(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t obj = v.r32(stack + 0x14);
    const uint32_t offset = v.r16(code);
    v.w32(f + 0x70, 0);
    v.w32(stack + 0x18, code + 3);
    step(v, f, 0x000465CC, obj, stack, f + 0x70, v.r8(code + 2));
    if (!v.r32(f + 0x70)) v.w32(stack + 0x18, v.r32(v.r32(stack + 0x10) + 0x4C) + offset);
}

void let_array(Vm& v, uint32_t f, uint32_t stack, uint32_t addr, uint32_t flags)
{
    v.w32(f + 0x70, 0);
    v.w32(k_runtime_flags, flags & ~1u);
    const uint32_t array = v.r32(k_property);
    arg(v, f, 0x00046D5C, stack, f + 0x70);
    int32_t len = (int32_t)v.r32(f + 0x70);
    int32_t num = (int32_t)v.r32(addr + 4);
    if (len > num) {
        const uint32_t add = (uint32_t)(len - num);
        const uint32_t elem = v.r32(v.r32(array + 0x64) + 0x44);
        const uint32_t data = dyn_reserve(v, f, addr, (uint32_t)len, elem, 0x00046DA0, 0x00046DC8);
        v.zero(f, 0x00046DE8, data + (uint32_t)num * elem, add * elem);
        const uint32_t inner = v.r32(array + 0x64);
        len = (int32_t)v.r32(f + 0x70);
        if (!inner || !(v.r32(v.r32(inner + 0x34) + 0xC0) & 0x100000) || !v.r32(v.r32(inner + 0x64) + 0x88)) return;
        while (len > num) {
            v.call(f, 0x00047008, func_000EAEEC, inner, v.r32(addr) + (uint32_t)num * v.r32(v.r32(array + 0x64) + 0x44));
            len = (int32_t)v.r32(f + 0x70);
            num++;
        }
        return;
    }
    if (len >= num) return;
    for (int32_t i = num - 1; !(i < len); i--) {
        const uint32_t inner = v.r32(array + 0x64);
        v.vcall(f, 0x00046F58, inner, 0x168, v.r32(addr) + v.r32(inner + 0x44) * (uint32_t)i);
        len = (int32_t)v.r32(f + 0x70);
    }
    const uint32_t elem = v.r32(v.r32(array + 0x64) + 0x44);
    dyn_shrink(v, f, addr, (uint32_t)len, elem, 0x00046F94, 0x00046FC8);
}

void let(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(k_prop_addr, 0);
    arg(v, f, 0x00046C6C, stack, 0);
    uint32_t addr = v.r32(k_prop_addr);
    if (!addr) {
        v.call(f, 0x00046C90, func_000AA46C, stack, 0x301, k_let_none);
        addr = k_let_sink;
        v.w32(k_prop_addr, addr);
        for (uint32_t i = 0; i < 12; i++) v.w8(addr + i, 0);
    } else if (const uint32_t obj = v.r32(k_prop_object)) {
        const uint32_t gp = v.r32(k_property);
        if (gp && (v.r64(gp + 0x48) & 0x20)) {
            v.vcall(f, 0x00046EC4, obj, 0xC4, gp);
            addr = v.r32(k_prop_addr);
        }
    }
    const uint32_t flags = v.r32(k_runtime_flags);
    if (flags & 1) return let_array(v, f, stack, addr, flags);
    arg(v, f, 0x00046E7C, stack, addr);
}

void let_bool(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    v.w32(k_prop_addr, 0);
    v.w32(k_property, 0);
    v.w32(k_prop_object, 0);
    arg(v, f, 0x000470C0, stack, 0);
    const uint32_t prop = v.r32(k_property);
    const uint32_t addr = v.r32(k_prop_addr);
    uint32_t target = prop, boolean = 0;
    if (prop) {
        const bool known = v.r32(k_bool_class) != 0;
        if (is_bool(v, f, prop, 0x00047100, 0x0004710C)) boolean = prop;
        if (!known) target = v.r32(k_property);
    }
    if (target && !boolean) {
        uint32_t cls = v.r32(k_array_class);
        const uint32_t target_class = v.r32(target + 0x34);
        if (!cls) {
            const uint32_t made = v.call(f, 0x0004715C, func_000DCCE0, k_core);
            v.w32(k_array_class, made);
            v.call(f, 0x00047168, func_000DCB18, made);
            cls = v.r32(k_array_class);
        }
        if (target_class == cls) {
            const uint32_t inner = v.r32(target + 0x64);
            if (inner && is_bool(v, f, inner, 0x000472A0, 0x000472AC)) boolean = inner;
        }
    }
    v.w32(f + 0x70, 0);
    if (const uint32_t obj = v.r32(k_prop_object))
        if (const uint32_t gp = v.r32(k_property))
            if (v.r64(gp + 0x48) & 0x20) v.vcall(f, 0x000471C0, obj, 0xC4, gp);
    arg(v, f, 0x0004720C, stack, f + 0x70);
    if (!addr) return;
    const uint32_t mask = v.r32(boolean + 0x64);
    v.w32(addr, v.r32(f + 0x70) ? v.r32(addr) | mask : v.r32(addr) & ~mask);
}

uint32_t value_size(Vm& v, uint32_t sp, uint64_t lr, uint32_t prop, uint32_t size)
{
    if (prop) {
        const uint32_t cast = v.r32(v.r32(prop + 0x34) + 0xC0);
        if (cast & 0x8000) return (v.r32(prop + 0x40) * v.r32(prop + 0x44)) & 0xFFFF;
        if (cast & 0x4) return 1;
        return (cast & 0x80000) ? 8 : 0;
    }
    if (size > 4) {
        if (size == 0xC || size == 0xB || size == 7) return 0xC;
        if (size == 6) return 8;
        v.call(sp, lr, func_0009DB34, v.r32(k_log), k_context_bad_size, size);
        return 0;
    }
    return size > 1 ? 4 : size ? 1 : 0;
}

void context_none(Vm& v, uint32_t f, uint32_t stack, uint32_t result)
{
    if (const uint32_t prop = v.r32(k_property)) {
        object_name(v, f, f + 0x74, prop, 0x000479D0, 0x00047808, 0x00047838, 0x0004785C);
        v.call(f, 0x000476A4, func_000AA46C, stack, 0x301, k_context_none, text(v, f + 0x74));
        release(v, f, f + 0x74, 0x000476C4);
    } else {
        v.call(f, 0x00047988, func_000AA46C, stack, 0x301, k_context_no_property);
    }
    if (const uint32_t hook = v.r32(k_context_hook)) v.vcall(f, 0x000476F0, hook, 0x14);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t skip = v.r16(code);
    v.w32(stack + 0x18, code + 2);
    const uint32_t prop = (uint32_t)v.r64(code + 2);
    const uint32_t base = code + 0xB;
    v.w32(stack + 0x18, base);
    const uint32_t size = v.r8(code + 0xA);
    const uint32_t n = value_size(v, f, 0x0004792C, prop, size);
    v.w32(stack + 0x18, base + skip);
    v.w32(k_prop_addr, 0);
    v.w32(k_property, 0);
    v.w32(k_prop_object, 0);
    if (result) v.zero(f, 0x000477B8, result, n);
}

void context(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(k_property, 0);
    v.w32(f + 0x70, 0);
    next(v, f, 0x00047640, self, stack, f + 0x70);
    const uint32_t target = v.r32(f + 0x70);
    if (!target) return context_none(v, f, stack, result);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 0xC);
    step(v, f, 0x000478AC, target, stack, result, v.r8(code + 0xB));
}

void primitive_cast(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    v.frame(sp0, 0x70, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t object = v.r32(stack + 0x14);
    v.w32(stack + 0x18, code + 1);
    call_cast(v, sp0 - 0x70, 0x00049D2C, object, stack, result, v.r8(code));
}

void dynamic_cast_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t cls = (uint32_t)v.r64(code);
    if (v.r32(cls + 0xBC) & 0x4000) {
        v.call(sp0, lr0, func_00049530_lifted, self, stack, result);
        return;
    }
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t object = v.r32(stack + 0x14);
    v.w32(f + 0x90, 0);
    v.w32(stack + 0x18, code + 9);
    step(v, f, 0x000495CC, object, stack, f + 0x90, v.r8(code + 8));
    v.w32(result, 0);
    const uint32_t obj = v.r32(f + 0x90);
    if (!obj || (cls && !child_of(v, v.r32(obj + 0x34), cls))) return;
    v.w32(result, obj);
}

void case_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x470;
    v.frame(sp0, 0x470, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 2);
    if (v.r16(code) == 0xFFFF) return;
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    v.w32(f + 0x78, 0);
    v.w32(stack + 0x18, code + 3);
    step(v, f, 0x00046514, v.r32(stack + 0x14), stack, f + 0x70, v.r8(code + 2));
}

template <uint32_t page, uint64_t ret>
void extended(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    v.frame(sp0, 0x70, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 1);
    call_index(v, sp0 - 0x70, ret, self, stack, result, page * 0x100 + v.r8(code));
}

void jump(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t)
{
    v.w32(stack + 0x18, v.r32(v.r32(stack + 0x10) + 0x4C) + v.r16(v.r32(stack + 0x18)));
}

void empty_parm(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t)
{
    const uint32_t flags = v.r32(k_runtime_flags);
    v.w32(k_prop_object, 0);
    v.w32(k_prop_addr, 0);
    v.w32(k_property, 0);
    v.w32(k_runtime_flags, flags | 2);
}

void nothing(Vm&, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t) {}

void end_parms(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t)
{
    v.w32(k_prop_object, 0);
    v.w32(stack + 0x18, v.r32(stack + 0x18) - 1);
}

void stop(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t)
{
    v.w32(stack + 0x18, 0);
}

void self_const(Vm& v, uint32_t, uint64_t, uint32_t self, uint32_t, uint32_t result)
{
    v.w32(result, self);
}

template <uint32_t value>
void store_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t result)
{
    v.w32(result, value);
}

uint32_t code_word(Vm& v, uint32_t stack)
{
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 4);
    return v.r32(code);
}

void int_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    v.w32(result, code_word(v, stack));
}

void float_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    v.w32(result, stfs(lfs(code_word(v, stack))));
}

void object_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 8);
    v.w32(result, (uint32_t)v.r64(code));
}

void name_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint64_t name = v.r64(code);
    v.w32(stack + 0x18, code + 8);
    v.w32(result, (uint32_t)(name >> 32));
    v.w32(result + 4, (uint32_t)name);
}

void rotation_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, code_word(v, stack));
}

void vector_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(lfs(code_word(v, stack))));
}

void byte_const(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 1);
    v.w8(result, v.r8(code));
}

void int_const_byte(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 1);
    v.w32(result, v.r8(code));
}

template <uint64_t lr_a, uint32_t (*op)(uint32_t)>
void unary(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    finish(v, stack);
    v.w32(result, op(v.r32(f + 0x70)));
}

template <uint64_t lr_a, uint64_t lr_b, uint32_t (*op)(uint32_t, uint32_t)>
void binary(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    arg0(v, f, lr_b, stack, f + 0x74);
    finish(v, stack);
    v.w32(result, op(v.r32(f + 0x70), v.r32(f + 0x74)));
}

template <uint64_t lr_a, uint64_t lr_b, uint64_t lr_c, uint32_t (*op)(uint32_t, uint32_t, uint32_t)>
void ternary(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    arg0(v, f, lr_b, stack, f + 0x74);
    arg0(v, f, lr_c, stack, f + 0x78);
    finish(v, stack);
    v.w32(result, op(v.r32(f + 0x70), v.r32(f + 0x74), v.r32(f + 0x78)));
}

template <uint64_t lr_a, uint64_t lr_b, bool equal>
void bool_compare(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    v.w32(f + 0x74, 0);
    v.w32(f + 0x70, v.r32(f + 0x70) != 0);
    arg(v, f, lr_b, stack, f + 0x74);
    finish(v, stack);
    v.w32(result, ((v.r32(f + 0x70) == 0) == (v.r32(f + 0x74) == 0)) == equal);
}

template <uint64_t lr_a, uint64_t lr_b, bool any>
void short_circuit(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    const uint32_t a = v.r32(f + 0x70);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 1);
    v.w32(f + 0x70, a != 0);
    const uint32_t skip = v.r16(code + 1);
    if ((a != 0) == any) {
        v.w32(stack + 0x18, code + 3);
        v.w32(result, any);
        v.w32(stack + 0x18, v.r32(stack + 0x18) + skip);
        return;
    }
    v.w32(f + 0x74, 0);
    v.w32(stack + 0x18, code + 4);
    step(v, f, lr_b, v.r32(stack + 0x14), stack, f + 0x74, v.r8(code + 3));
    const bool x = v.r32(f + 0x70) != 0, y = v.r32(f + 0x74) != 0;
    v.w32(result, any ? (x || y) : (x && y));
    finish(v, stack);
}

uint32_t lvalue(Vm& v, uint32_t f, uint64_t lr, uint64_t lr_dirty, uint32_t stack, uint32_t temp)
{
    arg(v, f, lr, stack, temp);
    if (const uint32_t obj = v.r32(k_prop_object)) v.vcall(f, lr_dirty, obj, 0xC4, v.r32(k_property));
    const uint32_t at = v.r32(k_prop_addr);
    return at ? at : temp;
}

template <uint64_t lr_a, uint64_t lr_dirty, uint32_t delta, bool post>
void crement(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    v.w32(f + 0x70, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x70);
    finish(v, stack);
    const uint32_t old = v.r32(at);
    v.w32(at, old + delta);
    v.w32(result, post ? old : old + delta);
}

template <uint64_t lr_a, uint64_t lr_dirty, uint64_t lr_b, uint32_t (*op)(uint32_t, uint32_t)>
void compound(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x70);
    arg0(v, f, lr_b, stack, f + 0x74);
    finish(v, stack);
    const uint32_t value = op(v.r32(at), v.r32(f + 0x74));
    v.w32(at, value);
    v.w32(result, value);
}

void add_vector_to(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, 0x00050074, 0x000500A4, stack, f + 0x70);
    arg(v, f, 0x00050104, stack, f + 0x7C);
    finish(v, stack);
    const double x = fadds(lfs(v.r32(at)), lfs(v.r32(f + 0x7C)));
    double y = lfs(v.r32(at + 4)), z = lfs(v.r32(at + 8));
    v.w32(at, stfs(x));
    y = fadds(y, lfs(v.r32(f + 0x80)));
    v.w32(at + 4, stfs(y));
    z = fadds(z, lfs(v.r32(f + 0x84)));
    v.w32(at + 8, stfs(z));
    v.w32(result, stfs(x));
    v.w32(result + 4, stfs(y));
    v.w32(result + 8, stfs(z));
}

template <uint64_t lr_a, uint64_t lr_b, uint64_t lr_log>
void divide_float(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const double zero = lfs(v.r32(k_float_zero));
    arg0(v, f, lr_a, stack, f + 0x70);
    arg0(v, f, lr_b, stack, f + 0x74);
    finish(v, stack);
    if (lfs(v.r32(f + 0x74)) == zero) {
        v.call(f, lr_log, func_000AA46C, stack, 0x301, k_divide_zero);
    }
    v.w32(result, stfs(fdivs(lfs(v.r32(f + 0x70)), lfs(v.r32(f + 0x74)))));
}

void divide_vector(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    arg(v, f, 0x000514C0, stack, f + 0x74);
    const double zero = lfs(v.r32(k_vector_divide));
    arg0(v, f, 0x00051528, stack, f + 0x70);
    finish(v, stack);
    if (lfs(v.r32(f + 0x70)) == zero) {
        v.call(f, 0x00051554, func_000AA46C, stack, 0x301, k_divide_zero);
    }
    const double scale = fdivs(lfs(v.r32(k_vector_divide + 4)), lfs(v.r32(f + 0x70)));
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(fmuls(lfs(v.r32(f + 0x74 + i)), scale)));
}

template <uint64_t lr_a, uint64_t lr_b, uint32_t first, uint32_t second, void (*op)(Vm&, uint32_t, uint32_t, uint32_t)>
void vector_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    if (first == 0x70 && second == 0x74) v.w32(f + 0x70, 0);
    arg(v, f, lr_a, stack, f + first);
    if (first == 0x74) v.w32(f + 0x70, 0);
    arg(v, f, lr_b, stack, f + second);
    finish(v, stack);
    op(v, f + 0x70, f + 0x7C, result);
}

void vadd(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(fadds(lfs(v.r32(a + i)), lfs(v.r32(b + i)))));
}

void vsub(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(fsubs(lfs(v.r32(a + i)), lfs(v.r32(b + i)))));
}

void vscale(Vm& v, uint32_t a, uint32_t, uint32_t result)
{
    const double s = lfs(v.r32(a));
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(fmuls(lfs(v.r32(a + 4 + i)), s)));
}

void vdot(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    double d = fmuls(lfs(v.r32(a + 4)), lfs(v.r32(b + 4)));
    d = fmadds(lfs(v.r32(a)), lfs(v.r32(b)), d);
    v.w32(result, stfs(fmadds(lfs(v.r32(a + 8)), lfs(v.r32(b + 8)), d)));
}

bool same_vector(Vm& v, uint32_t a, uint32_t b)
{
    for (uint32_t i = 0; i < 12; i += 4)
        if (!(lfs(v.r32(a + i)) == lfs(v.r32(b + i)))) return false;
    return true;
}

void vequal(Vm& v, uint32_t a, uint32_t b, uint32_t result) { v.w32(result, same_vector(v, a, b)); }
void vunequal(Vm& v, uint32_t a, uint32_t b, uint32_t result) { v.w32(result, !same_vector(v, a, b)); }

void negate_vector(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg(v, f, 0x00051A5C, stack, f + 0x70);
    finish(v, stack);
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(-lfs(v.r32(f + 0x70 + i))));
}

template <uint64_t lr_a, uint64_t lr_b, bool equal>
void name_compare(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    arg(v, f, lr_a, stack, f + 0x70);
    v.w32(f + 0x78, 0);
    v.w32(f + 0x7C, 0);
    arg(v, f, lr_b, stack, f + 0x78);
    finish(v, stack);
    v.w32(result, (v.r64(f + 0x70) == v.r64(f + 0x78)) == equal);
}

uint32_t imul(uint32_t a, uint32_t b) { return (uint32_t)((int64_t)(int32_t)a * (int32_t)b); }
uint32_t iadd(uint32_t a, uint32_t b) { return a + b; }
uint32_t isub(uint32_t a, uint32_t b) { return a - b; }
uint32_t ilt(uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b; }
uint32_t igt(uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b; }
uint32_t ile(uint32_t a, uint32_t b) { return (int32_t)a <= (int32_t)b; }
uint32_t ige(uint32_t a, uint32_t b) { return (int32_t)a >= (int32_t)b; }
uint32_t ieq(uint32_t a, uint32_t b) { return a == b; }
uint32_t ine(uint32_t a, uint32_t b) { return a != b; }
uint32_t iand(uint32_t a, uint32_t b) { return a & b; }
uint32_t ixor(uint32_t a, uint32_t b) { return a ^ b; }
uint32_t ior(uint32_t a, uint32_t b) { return a | b; }
uint32_t imin(uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b ? b : a; }
uint32_t imax(uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b ? b : a; }
uint32_t iclamp(uint32_t x, uint32_t lo, uint32_t hi) { return (int32_t)lo > (int32_t)x ? lo : (int32_t)hi > (int32_t)x ? x : hi; }
uint32_t ineg(uint32_t a) { return 0 - a; }
uint32_t bnot(uint32_t a) { return a == 0; }
uint32_t fmul_bits(uint32_t a, uint32_t b) { return stfs(fmuls(lfs(a), lfs(b))); }
uint32_t fadd_bits(uint32_t a, uint32_t b) { return stfs(fadds(lfs(a), lfs(b))); }
uint32_t fsub_bits(uint32_t a, uint32_t b) { return stfs(fsubs(lfs(a), lfs(b))); }
uint32_t flt(uint32_t a, uint32_t b) { return !(lfs(a) > lfs(b) || lfs(a) == lfs(b)); }
uint32_t fgt(uint32_t a, uint32_t b) { return !(lfs(a) < lfs(b) || lfs(a) == lfs(b)); }
uint32_t fle(uint32_t a, uint32_t b) { return !(lfs(a) > lfs(b)); }
uint32_t fge(uint32_t a, uint32_t b) { return !(lfs(a) < lfs(b)); }
uint32_t feq(uint32_t a, uint32_t b) { return lfs(a) == lfs(b); }
uint32_t fne(uint32_t a, uint32_t b) { return !(lfs(a) == lfs(b)); }
uint32_t fneg(uint32_t a) { return stfs(-lfs(a)); }
uint32_t fabs_bits(uint32_t a) { return stfs(std::fabs(lfs(a))); }
uint32_t fmin_bits(uint32_t a, uint32_t b) { return stfs(fsubs(lfs(a), lfs(b)) >= 0.0 ? lfs(b) : lfs(a)); }
uint32_t fmax_bits(uint32_t a, uint32_t b) { return stfs(fsubs(lfs(a), lfs(b)) >= 0.0 ? lfs(a) : lfs(b)); }

uint32_t fclamp_bits(uint32_t x, uint32_t lo, uint32_t hi)
{
    const double v = lfs(x), a = lfs(lo), b = lfs(hi);
    const double over = fp_bin(v, b, v - b), under = fp_bin(v, a, v - a);
    const double r = over >= 0.0 ? b : v;
    return stfs(under >= 0.0 ? r : a);
}

template <void (*wrapper)(ppu_context*), Exec body>
void sited(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    if (!frame_site(stack)) return body(v, sp0, lr0, self, stack, result);
    v.call(sp0, lr0, wrapper, self, stack, result);
}

void w16(Vm& v, uint32_t a, uint16_t x)
{
    vm_write16_mm(&v.m, a, x);
}

void drop(Vm& v, uint32_t sp, uint32_t at, uint64_t lr)
{
    if (const uint32_t data = v.r32(at)) {
        v.w32(at + 4, 0);
        v.w32(at + 8, 0);
        v.call(sp, lr, func_0002ADF0, data);
    }
}

void end_of_script(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const uint32_t log = v.r32(k_log);
    v.call(f, 0x00045DCC, func_000BD3C4, f + 0x70, v.r32(stack + 0x10), 0);
    const uint32_t node = text(v, f + 0x70);
    v.call(f, 0x00045DFC, func_000BD3C4, f + 0x7C, v.r32(stack + 0x14), 0);
    v.call(f, 0x00045E2C, func_0009DB34, log, 0x0160AE90, node, text(v, f + 0x7C));
    release(v, f, f + 0x7C, 0x00045E4C);
    drop(v, f, f + 0x70, 0x00045E70);
}

void iterator_pop(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const uint32_t log = v.r32(k_log);
    v.call(f, 0x00045F6C, func_000BD3C4, f + 0x70, v.r32(stack + 0x10), 0);
    const uint32_t offset = v.r32(stack + 0x18) - v.r32(v.r32(stack + 0x10) + 0x4C);
    v.call(f, 0x00045FA8, func_0009DB34, log, 0x0160AFC8, text(v, f + 0x70), (int64_t)(int32_t)offset);
    drop(v, f, f + 0x70, 0x00045FC8);
}

void undefined_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    v.frame(sp0, 0x70, lr0);
    v.call(sp0 - 0x70, 0x000413D4, func_000AA46C, stack, 0x2F9, 0x0160AA54, v.r8(v.r32(stack + 0x18) - 1));
}

void return_nothing(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x80;
    v.frame(sp0, 0x80, lr0);
    v.call(f, 0x00045EBC, func_000AA46C, stack, 0x301, 0x0160AEE8);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    if (v.r64(prop + 0x48) & 0x400000) v.vcall(f, 0x00045EFC, prop, 0x168, result);
    v.zero(f, 0x00045F18, result, v.r32(prop + 0x40) * v.r32(prop + 0x44));
}

void assign_text(Vm& v, uint32_t f, uint32_t result, uint32_t data, uint32_t len, uint64_t lr_alloc, uint64_t lr_copy, uint64_t lr_free)
{
    if (result != f + 0x70) {
        uint32_t dst = v.r32(result);
        v.w32(result + 8, len);
        v.w32(result + 4, len);
        uint32_t n = len;
        if (dst | len) {
            dst = v.call(f, lr_alloc, func_0002AE4C, dst, len << 1, 8);
            n = v.r32(result + 4);
            v.w32(result, dst);
        }
        if (n) v.call(f, lr_copy, func_00F297C8, dst, len ? data : k_empty_text, n << 1);
    }
    if (data) v.call(f, lr_free, func_0002ADF0, data);
}

void string_const(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x1C0;
    v.frame(sp0, 0x1C0, lr0);
    const uint32_t src = v.r32(stack + 0x18);
    const uint32_t len = src && v.r8(src) ? v.call(f, 0x00049090, func_00F2C3E8, src) + 1 : 0;
    uint32_t data = 0;
    if (len) {
        data = v.call(f, 0x000490B0, func_0002AE4C, 0, len << 1, 8);
        uint32_t buf = f + 0x7C;
        const uint32_t n = v.call(f, 0x000490C8, func_00F2C3E8, src) + 1;
        if (n > 0x80) buf = v.call(f, 0x000490E0, func_0002AED0, n << 2, 8);
        for (uint32_t i = 0; i < n; i++) w16(v, buf + 2 * i, v.r8(src + i));
        v.w32(f + 0x17C, buf);
        v.call(f, 0x00049134, func_00F297C8, data, buf, len << 1);
        if (const uint32_t temp = v.r32(f + 0x17C)) {
            w16(v, temp, 0);
            if (temp != f + 0x7C) v.call(f, 0x0004915C, func_0002ADF0, temp);
        }
    }
    assign_text(v, f, result, data, len, 0x00049190, 0x000491C4, 0x000491D4);
    uint32_t code = v.r32(stack + 0x18);
    while (v.r8(code)) code++;
    v.w32(stack + 0x18, code + 1);
}

void unicode_string_const(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    const uint32_t src = v.r32(stack + 0x18);
    const uint32_t len = src && v.r16(src) ? v.call(f, 0x000492A8, func_00F2CE40, src) + 1 : 0;
    uint32_t data = 0;
    if (len) {
        data = v.call(f, 0x000492C4, func_0002AE4C, 0, len << 1, 8);
        v.call(f, 0x000492D4, func_00F297C8, data, src, len << 1);
    }
    assign_text(v, f, result, data, len, 0x0004930C, 0x00049348, 0x00049358);
    uint32_t code = v.r32(stack + 0x18);
    while (v.r16(code)) code += 2;
    v.w32(stack + 0x18, code + 2);
}

void native_parm(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    if (!result) return;
    v.w32(k_prop_object, 0);
    if (v.r64(prop + 0x48) & 0x100) {
        uint32_t out = v.r32(stack + 0x24);
        while (v.r32(out) != prop) out = v.r32(out + 8);
        v.w32(k_prop_addr, v.r32(out + 4));
        return;
    }
    const uint32_t addr = v.r32(stack + 0x1C) + v.r32(prop + 0x54);
    v.w32(k_prop_addr, addr);
    v.frame(sp0, 0x70, lr0);
    v.vcall(sp0 - 0x70, 0x00067328, prop, 0x15C, result, addr, 0, 0, 0);
}

void eat_return_value(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    const uint32_t size = v.r32(prop + 0x40) * v.r32(prop + 0x44);
    uint32_t sp = f, temp = 0;
    if (size) {
        sp = v.stack_alloc(f, size);
        temp = sp + 0x70;
    }
    v.zero(sp, 0x000474C0, temp, size);
    next(v, sp, 0x0004751C, v.r32(stack + 0x14), stack, temp);
    v.vcall(sp, 0x00047538, prop, 0x168, temp);
}

void default_variable(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x80;
    v.frame(sp0, 0x80, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint64_t flags = v.r64(self + 8);
    uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 8);
    v.w32(k_property, prop);
    uint32_t base = self;
    if (!(flags & 0x200)) {
        base = v.r32(self + 0x38);
        if ((int32_t)v.r32(prop + 0x54) >= (int32_t)v.r32(v.r32(base + 0x34) + 0x48)) {
            base = v.call(f, 0x00041990, func_00041768, v.r32(self + 0x34), 0);
            prop = v.r32(k_property);
        }
    }
    v.w32(k_prop_object, 0);
    const uint32_t addr = base + v.r32(prop + 0x54);
    v.w32(k_prop_addr, addr);
    if (result) v.vcall(f, 0x00041A08, prop, 0x15C, result, addr, 0, 0, 0);
}

void state_variable(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    variable(v, sp0, lr0, 0x00041B68, stack, result, v.r32(v.r32(self + 0x14) + 0x1C), 0);
}

void default_parm_value(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    uint32_t flags = v.r32(k_runtime_flags);
    uint32_t at = code + 2;
    if (!(flags & 2)) {
        at += v.r16(code);
    } else {
        v.w32(stack + 0x18, at);
        if (v.r8(at) != 0x15) {
            const uint32_t f = sp0 - 0x90;
            v.frame(sp0, 0x90, lr0);
            do {
                next(v, f, 0x00041D18, v.r32(stack + 0x14), stack, result);
                at = v.r32(stack + 0x18);
            } while (v.r8(at) != 0x15);
            flags = v.r32(k_runtime_flags);
        }
        at++;
    }
    v.w32(stack + 0x18, at);
    v.w32(k_runtime_flags, flags & ~2u);
}

void conditional(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x000655D8, stack, f + 0x70);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t obj = v.r32(stack + 0x14);
    uint32_t at = code + 2;
    if (!v.r32(f + 0x70)) at += v.r16(code) + 2;
    v.w32(stack + 0x18, at + 1);
    step(v, f, 0x00065648, obj, stack, result, v.r8(at));
    if (!v.r32(f + 0x70)) return;
    const uint32_t skip = v.r32(stack + 0x18);
    v.w32(stack + 0x18, skip + 2 + v.r16(skip));
}

void interface_context(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x80;
    v.frame(sp0, 0x80, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    next(v, f, 0x00041BF0, self, stack, f + 0x70);
    if (result) v.w32(result, v.r32(f + 0x70));
}

void interface_cast(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    v.frame(sp0, 0x70, lr0);
    call_cast(v, sp0 - 0x70, 0x00049DA4, v.r32(stack + 0x14), stack, result, 0x46);
}

void empty_delegate(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t result)
{
    v.w32(result, 0);
    v.w32(result + 4, 0);
    v.w32(result + 8, 0);
}

void instance_delegate(Vm& v, uint32_t, uint64_t, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t hi = v.r32(code), lo = v.r32(code + 4);
    v.w32(stack + 0x18, code + 8);
    v.w32(result, self);
    v.w32(result + 4, hi);
    v.w32(result + 8, lo);
}

void goto_label(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    arg(v, f, 0x00046B14, stack, f + 0x70);
    v.w32(f + 0x38, v.r32(f + 0x70));
    v.w32(f + 0x3C, v.r32(f + 0x74));
    if (v.vcall(f, 0x00046B40, self, 0x114, v.r64(f + 0x38))) return;
    v.call(f, 0x00046B5C, func_000AEB38, f + 0x78, f + 0x70);
    v.call(f, 0x00046B8C, func_000AA46C, stack, 0x315, 0x0160B05C, text(v, f + 0x78));
    drop(v, f, f + 0x78, 0x00046BAC);
}

void let_delegate(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(k_prop_addr, 0);
    v.w32(k_property, 0);
    v.w32(k_prop_object, 0);
    arg(v, f, 0x00047390, stack, 0);
    const uint32_t addr = v.r32(k_prop_addr);
    arg(v, f, 0x000473E8, stack, f + 0x70);
    if (!addr) return;
    v.w32(addr + 4, v.r32(f + 0x74));
    v.w32(addr + 8, v.r32(f + 0x78));
    v.w32(addr, v.r32(f + 0x70));
}

void delegate_property(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t hi = v.r32(code), lo = v.r32(code + 4);
    v.w32(stack + 0x18, code + 8);
    const uint32_t prop = (uint32_t)v.r64(code + 8);
    v.w32(stack + 0x18, code + 0x10);
    if (prop) {
        const uint32_t at = self + v.r32(prop + 0x54);
        if (v.r64(at + 4))
            if (const uint32_t obj = v.r32(at)) {
                v.frame(sp0, 0xB0, lr0);
                if (!v.vcall(sp0 - 0xB0, 0x0004806C, obj, 0x30)) {
                    const uint32_t from = self + v.r32(prop + 0x54);
                    const uint32_t a = v.r32(from + 4), b = v.r32(from);
                    v.w32(result + 4, a);
                    v.w32(result, b);
                    v.w32(result + 8, v.r32(from + 8));
                    return;
                }
            }
    }
    v.w32(result + 4, hi);
    v.w32(result + 8, lo);
    v.w32(result, hi || lo ? self : 0);
}

constexpr uint32_t k_class_class = 0x019BA358;
constexpr uint32_t k_class_context_none = 0x0160B3D4;
constexpr uint32_t k_class_context_no_property = 0x0160B41C;
constexpr uint32_t k_assert_stack = 0x0160B020;
constexpr uint32_t k_assert_failed = 0x0160B028;
constexpr uint32_t k_assert_header = 0x01692DC8;
constexpr uint32_t k_assert_frame = 0x01692DF0;
constexpr uint32_t k_new_none = 0x0160B2C0;
constexpr uint32_t k_new_bad_flags = 0x0160B308;
constexpr uint32_t k_new_abstract = 0x0160B340;
constexpr uint32_t k_new_masks = 0x000656B8;
constexpr uint32_t k_delegate_none = 0x0160B130;

uint32_t static_bounds(Vm& v, uint32_t f, uint32_t self, uint32_t stack)
{
    if (!self) literal(v, f, f + 0x74, k_null_owner_text, 0x00041EFC, 0x00041F2C, 0x00041F50);
    else object_name(v, f, f + 0x74, self, 0x00042204, 0x0004210C, 0x0004213C, 0x00042160);
    const uint32_t owner = text(v, f + 0x74);
    if (const uint32_t prop = v.r32(k_property)) object_name(v, f, f + 0x80, prop, 0x0004221C, 0x000421A0, 0x000421D0, 0x000421F4);
    else literal(v, f, f + 0x80, k_null_owner_text, 0x00041FAC, 0x00041FDC, 0x00042000);
    v.call(f, 0x0004204C, func_000AA46C, stack, 0x315, k_member_bounds, owner, text(v, f + 0x80), (int64_t)(int32_t)v.r32(f + 0x70),
           (int64_t)(int32_t)v.r32(v.r32(k_property) + 0x40));
    release(v, f, f + 0x80, 0x0004206C);
    release(v, f, f + 0x74, 0x00042090);
    const uint32_t prop = v.r32(k_property);
    const int32_t index = (int32_t)v.r32(f + 0x70), last = (int32_t)(v.r32(prop + 0x40) - 1);
    v.w32(f + 0x70, (uint32_t)(index < 0 ? 0 : index < last ? index : last));
    return prop;
}

void array_element(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    arg0(v, f, 0x00041DE0, stack, f + 0x70);
    v.w32(k_property, 0);
    next(v, f, 0x00041E38, self, stack, 0);
    uint32_t prop = v.r32(k_property);
    v.w32(k_prop_object, self);
    if (!prop || !v.r32(k_prop_addr)) return;
    const int32_t index = (int32_t)v.r32(f + 0x70);
    if (index >= (int32_t)v.r32(prop + 0x40) || index < 0) prop = static_bounds(v, f, self, stack);
    const uint32_t at = v.r32(k_prop_addr) + v.r32(f + 0x70) * v.r32(prop + 0x44);
    v.w32(k_prop_addr, at);
    if (result) v.vcall(f, 0x00042278, prop, 0x158, result, at, 0, 0, 0);
}

void switch_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x8C0;
    v.frame(sp0, 0x8C0, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t prop = (uint32_t)v.r64(code);
    v.w32(stack + 0x18, code + 9);
    uint32_t size = value_size(v, f, 0x000463D0, prop, v.r8(code + 8));
    const uint32_t str = prop && (v.r32(v.r32(prop + 0x34) + 0xC0) & 1) ? prop : 0;
    if (str && !size && (v.r32(v.r32(str + 0x34) + 0xC0) & 0x80)) size = 4;
    for (const uint32_t at : {0x474u, 0x478u, 0x47Cu, 0x74u, 0x78u, 0x7Cu}) v.w32(f + at, 0);
    arg(v, f, 0x000461B8, stack, f + 0x74);
    uint32_t at = v.r32(stack + 0x18);
    const bool strings = str && (v.r32(v.r32(str + 0x34) + 0xC0) & 0x4000);
    for (;;) {
        v.w32(stack + 0x18, at + 1);
        const uint32_t skip = v.r16(at + 1);
        v.w32(stack + 0x18, at + 3);
        if (skip == 0xFFFF) break;
        v.w32(stack + 0x18, at + 4);
        step(v, f, 0x00046260, v.r32(stack + 0x14), stack, f + 0x474, v.r8(at + 3));
        const uint32_t diff = strings ? v.call(f, 0x000462A4, func_008A8B50, text(v, f + 0x74), text(v, f + 0x474))
                                      : v.call(f, 0x000462F8, func_00F35F00, f + 0x74, f + 0x474, size);
        if (!diff) break;
        at = v.r32(v.r32(stack + 0x10) + 0x4C) + skip;
    }
    if (!strings) return;
    release(v, f, f + 0x74, 0x00046414);
    drop(v, f, f + 0x474, 0x00046430);
}

void assert_fail(Vm& v, uint32_t f, uint32_t stack, uint32_t line, uint32_t debug)
{
    uint32_t frames = 0, count = 0, max = 0;
    for (uint32_t s = stack; s; s = v.r32(s + 0x20)) {
        if ((int32_t)(count + 1) > (int32_t)max) {
            max = v.call(f, 0x000467D4, func_00010230, count + 1, (int64_t)(int32_t)max, 4);
            if (frames | max) frames = v.call(f, 0x000467F4, func_0002AE4C, frames, max << 2, 8);
        }
        if (const uint32_t slot = frames + count * 4) v.w32(slot, s);
        count++;
    }
    const uint32_t len = v.r16(k_assert_header) ? v.call(f, 0x0004682C, func_00F2CE40, k_assert_header) + 1 : 0;
    uint32_t temp = 0, data = 0, num = 0, cap = 0;
    if (len) {
        temp = v.call(f, 0x0004684C, func_0002AE4C, 0, len << 1, 8);
        v.call(f, 0x0004685C, func_00F297C8, temp, k_assert_header, len << 1);
        num = len;
        if ((int32_t)num > 0) {
            cap = v.call(f, 0x00046A48, func_00010230, (int64_t)(int32_t)num, 0, 2);
            if (cap) data = v.call(f, 0x00046A64, func_0002AE4C, 0, cap << 1, 8);
        }
        v.call(f, 0x00046A78, func_00F297C8, data, temp, len << 1);
    }
    if (temp) v.call(f, 0x00046874, func_0002ADF0, temp);
    for (int32_t i = (int32_t)count - 1; i >= 0; i--) {
        v.call(f, 0x00046918, func_000BD3C4, f + 0x80, v.r32(v.r32(frames + (uint32_t)i * 4) + 0x10), 0);
        v.call(f, 0x00046940, func_0009E0B4, f + 0x74, k_assert_frame, text(v, f + 0x80));
        if (uint32_t n = v.r32(f + 0x78)) {
            uint32_t pos = num;
            if (pos) {
                n--;
                pos--;
            }
            num += n;
            if ((int32_t)num > (int32_t)cap) {
                cap = v.call(f, 0x000469B8, func_00010230, (int64_t)(int32_t)num, (int64_t)(int32_t)cap, 2);
                if (data | cap) data = v.call(f, 0x000469D8, func_0002AE4C, data, cap << 1, 8);
            }
            v.call(f, 0x000469F8, func_00F297C8, data + pos * 2, v.r32(f + 0x74), v.r32(f + 0x78) << 1);
        }
        release(v, f, f + 0x74, 0x00046A0C);
        release(v, f, f + 0x80, 0x00046A00);
    }
    if (frames) v.call(f, 0x00046890, func_0002ADF0, frames);
    v.call(f, 0x000468B8, func_0009DB34, stack, k_assert_stack, num ? data : k_empty_text);
    if (data) v.call(f, 0x000468C8, func_0002ADF0, data);
    v.call(f, 0x000468EC, func_000AA46C, stack, debug ? 0x2F9 : 0x301, k_assert_failed, line);
}

void assert_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x100;
    v.frame(sp0, 0x100, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t line = v.r16(code);
    v.w32(stack + 0x18, code + 3);
    const uint32_t debug = v.r8(code + 2);
    arg0(v, f, 0x000466D0, stack, f + 0x70);
    if (v.r32(f + 0x70)) return;
    if (const uint32_t hook = v.r32(k_context_hook))
        if (v.vcall(f, 0x00046704, hook, 0x18, line)) return;
    assert_fail(v, f, stack, line, debug);
}

void class_context(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    next(v, f, 0x00065F5C, self, stack, f + 0x70);
    if (const uint32_t cls = v.r32(f + 0x70)) {
        const uint32_t target = v.call(f, 0x00065F70, func_00041768, cls, 0);
        const uint32_t code = v.r32(stack + 0x18);
        v.w32(stack + 0x18, code + 0xC);
        step(v, f, 0x00065FBC, target, stack, result, v.r8(code + 0xB));
        return;
    }
    if (const uint32_t prop = v.r32(k_property)) {
        object_name(v, f, f + 0x74, prop, 0x000662F4, 0x0006617C, 0x000661AC, 0x000661D0);
        v.call(f, 0x00066034, func_000AA46C, stack, 0x315, k_class_context_none, text(v, f + 0x74));
        release(v, f, f + 0x74, 0x00066054);
    } else {
        v.call(f, 0x000662AC, func_000AA46C, stack, 0x315, k_class_context_no_property);
    }
    if (const uint32_t hook = v.r32(k_context_hook)) v.vcall(f, 0x00066080, hook, 0x14);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t skip = v.r16(code);
    v.w32(stack + 0x18, code + 2);
    const uint32_t prop = (uint32_t)v.r64(code + 2);
    v.w32(stack + 0x18, code + 0xB);
    const uint32_t n = value_size(v, f, 0x00066250, prop, v.r8(code + 0xA));
    v.w32(stack + 0x18, v.r32(stack + 0x18) + skip);
    v.w32(k_prop_addr, 0);
    v.w32(k_property, 0);
    v.w32(k_prop_object, 0);
    if (result) v.zero(f, 0x00066148, result, n);
}

void meta_cast(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    if (!v.r32(k_class_class)) {
        v.call(sp0, lr0, func_00049868_lifted, self, stack, result);
        return;
    }
    const uint32_t f = sp0 - 0xD0;
    v.frame(sp0, 0xD0, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t object = v.r32(stack + 0x14);
    const uint32_t cls = (uint32_t)v.r64(code);
    v.w32(f + 0x90, 0);
    v.w32(stack + 0x18, code + 9);
    step(v, f, 0x00049908, object, stack, f + 0x90, v.r8(code + 8));
    const uint32_t obj = v.r32(f + 0x90);
    v.w32(result, obj && child_of(v, v.r32(obj + 0x34), v.r32(k_class_class)) && child_of(v, obj, cls) ? obj : 0);
}

void parm(Vm& v, uint32_t f, uint64_t lr, uint32_t stack, uint32_t at)
{
    v.w32(k_runtime_flags, v.r32(k_runtime_flags) & ~2u);
    arg(v, f, lr, stack, at);
}

void new_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    v.w32(f + 0x78, 0);
    parm(v, f, 0x00065768, stack, f + 0x78);
    v.w32(f + 0xA0, 0);
    v.w32(f + 0x9C, 0);
    v.w32(f + 0x98, 0);
    parm(v, f, 0x000657DC, stack, f + 0x98);
    v.w32(f + 0x7C, 0);
    parm(v, f, 0x00065844, stack, f + 0x7C);
    v.w32(f + 0x80, 0);
    parm(v, f, 0x000658AC, stack, f + 0x80);
    v.w32(f + 0x84, 0);
    parm(v, f, 0x00065914, stack, f + 0x84);
    uint32_t cls = v.r32(f + 0x80);
    if (!cls) {
        v.call(f, 0x00065934, func_000AA46C, stack, 0x301, k_new_none);
        drop(v, f, f + 0x98, 0x00065954);
        return;
    }
    if (const uint64_t bad = (uint64_t)(int64_t)(int32_t)v.r32(f + 0x7C) & v.r64(k_new_masks)) {
        v.call(f, 0x00065D58, func_0009DB34, stack, k_new_bad_flags, bad);
        cls = v.r32(f + 0x80);
    }
    const uint64_t forbidden = cls ? v.r64(k_new_masks + 8) : 0;
    for (uint32_t c = cls; c; c = v.r32(c + 0x40)) {
        if ((int32_t)v.r32(c + 0x20) == -1) {
            v.call(f, 0x00065A34, func_000A4148, f + 0x90, k_none_text, 1, 1);
        } else {
            v.w32(f + 0x90, v.r32(c + 0x2C));
            v.w32(f + 0x94, v.r32(c + 0x30));
        }
        if (v.r64(f + 0x90) != forbidden) continue;
        const uint32_t named = v.r32(f + 0x80);
        if (!named) literal(v, f, f + 0xA4, k_null_owner_text, 0x00065CB0, 0x00065CE0, 0x00065D04);
        else object_name(v, f, f + 0xA4, named, 0x00065D34, 0x00065BA8, 0x00065BD8, 0x00065BFC);
        v.call(f, 0x00065C2C, func_000AA46C, stack, 0x301, k_new_abstract, text(v, f + 0xA4));
        release(v, f, f + 0xA4, 0x00065C4C);
        v.w32(result, 0);
        drop(v, f, f + 0x98, 0x00065C7C);
        return;
    }
    uint32_t outer = v.r32(f + 0x78);
    if (!outer) {
        outer = v.call(f, 0x00065A80, func_000BC27C);
        v.w32(f + 0x78, outer);
    }
    const uint32_t chars = v.r32(f + 0x9C);
    cls = v.r32(f + 0x80);
    if (chars && chars - 1) {
        v.call(f, 0x00065AB8, func_000A4148, f + 0x88, v.r32(f + 0x98), 1, 1);
    } else {
        v.w32(f + 0x88, 0);
        v.w32(f + 0x8C, 0);
    }
    const uint32_t hi = v.r32(f + 0x88), lo = v.r32(f + 0x8C);
    v.w32(f + 0x70, hi);
    const uint64_t flags = (uint64_t)(int64_t)(int32_t)v.r32(f + 0x7C) & v.r64(k_new_masks + 0x10);
    const uint32_t tmpl = v.r32(f + 0x84);
    v.w32(f + 0x40, hi);
    v.w32(f + 0x44, lo);
    v.w32(result, v.call(f, 0x00065B1C, func_000CF140, cls, outer, v.r64(f + 0x40), flags, tmpl, stack, tmpl ? ~0ull : 0ull, 0));
    drop(v, f, f + 0x98, 0x00065B40);
}

void delegate_call(Vm& v, uint32_t f, uint64_t lr_find, uint64_t lr_call, uint32_t obj, uint32_t stack, uint32_t result, uint32_t hi,
                   uint32_t lo)
{
    v.w32(f + 0x38, hi);
    v.w32(f + 0x3C, lo);
    call_function(v, f, lr_call, obj, stack, result, find_checked(v, f, lr_find, obj, v.r64(f + 0x38), 0));
}

void delegate_function(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t local = v.r8(code);
    v.w32(stack + 0x18, code + 1);
    const uint32_t prop = (uint32_t)v.r64(code + 1);
    v.w32(stack + 0x18, code + 9);
    const uint32_t addr = (local ? v.r32(stack + 0x1C) : self) + v.r32(prop + 0x54);
    const uint32_t hi = v.r32(code + 9), lo = v.r32(code + 0xD);
    v.w32(stack + 0x18, code + 0x11);
    uint32_t obj = v.r32(addr);
    if (obj) {
        const uint32_t dead = v.vcall(f, 0x00047C88, obj, 0x30);
        obj = v.r32(addr);
        if (dead) {
            v.w32(addr, 0);
            v.w32(addr + 4, 0);
            v.w32(addr + 8, 0);
            obj = 0;
        }
    }
    if (obj) return delegate_call(v, f, 0x00047EAC, 0x00047EC0, obj, stack, result, v.r32(addr + 4), v.r32(addr + 8));
    if (v.r64(addr + 4)) return delegate_call(v, f, 0x00047F28, 0x00047F3C, self, stack, result, v.r32(addr + 4), v.r32(addr + 8));
    if (!v.r32(prop + 0x68)) return delegate_call(v, f, 0x00047EF0, 0x00047F04, self, stack, result, hi, lo);
    if (!prop) literal(v, f, f + 0x70, k_null_owner_text, 0x00047D00, 0x00047D30, 0x00047D54);
    else object_name(v, f, f + 0x70, prop, 0x00047ED4, 0x00047E2C, 0x00047E5C, 0x00047E80);
    v.call(f, 0x00047D84, func_000AA46C, stack, 0x301, k_delegate_none, text(v, f + 0x70));
    release(v, f, f + 0x70, 0x00047DA4);
    v.call(f, 0x00047DC0, func_000404B8, self, stack, result, v.r32(prop + 0x68));
}

uint32_t dyn_array(Vm& v, uint32_t f, uint64_t lr, uint32_t self, uint32_t stack)
{
    v.w32(k_property, 0);
    v.w32(k_prop_object, self);
    next(v, f, lr, self, stack, 0);
    const uint32_t prop = v.r32(k_property);
    return prop && (v.r32(v.r32(prop + 0x34) + 0xC0) & 0x200000) ? prop : 0;
}

bool needs_init(Vm& v, uint32_t inner)
{
    return inner && (v.r32(v.r32(inner + 0x34) + 0xC0) & 0x100000) && v.r32(v.r32(inner + 0x64) + 0x88);
}

template <typename... A>
void dyn_log(Vm& v, uint32_t f, uint32_t stack, uint32_t array, uint32_t at, const uint64_t (&lr)[8], uint32_t message, A... a)
{
    if (!array) literal(v, f, at, k_null_owner_text, lr[0], lr[1], lr[2]);
    else object_name(v, f, at, array, lr[3], lr[4], lr[5], lr[6]);
    v.call(f, lr[7], func_0009DB34, stack, message, a..., text(v, at));
}

void dyn_count_error(Vm& v, uint32_t f, uint32_t stack, uint32_t array, uint32_t at, const uint64_t (&lr)[8], uint32_t message,
                     uint64_t lr_free)
{
    dyn_log(v, f, stack, array, at, lr, message);
    if (const uint32_t data = v.r32(at)) {
        v.w32(at + 4, 0);
        v.w32(at + 8, 0);
        v.call(f, lr_free, func_0002ADF0, data);
    }
}

void dyn_add(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    const uint32_t array = dyn_array(v, f, 0x00044248, self, stack);
    const uint32_t addr = v.r32(k_prop_addr);
    arg0(v, f, 0x000442DC, stack, f + 0x70);
    finish(v, stack);
    if (!addr) return;
    const uint32_t count = v.r32(f + 0x70);
    if (!count) return;
    if ((int32_t)count < 0)
        return dyn_count_error(v, f, stack, array, f + 0x74,
                               {0x00044340, 0x00044370, 0x00044394, 0x000445DC, 0x0004451C, 0x0004454C, 0x00044570, 0x000443C0}, 0x0160AD94,
                               0x000443E0);
    const uint32_t inner = v.r32(array + 0x64);
    const uint32_t num = v.r32(addr + 4);
    const uint32_t elem = v.r32(inner + 0x44);
    const uint32_t data = dyn_reserve(v, f, addr, num + count, elem, 0x00044454, 0x0004447C);
    v.zero(f, 0x0004449C, data + num * elem, count * elem);
    const uint32_t init = v.r32(array + 0x64);
    if (needs_init(v, init))
        for (uint32_t i = num; (int32_t)i < (int32_t)(v.r32(f + 0x70) + num); i++)
            v.call(f, 0x000445B4, func_000EAEEC, init, v.r32(addr) + i * v.r32(v.r32(array + 0x64) + 0x44));
    v.w32(result, num);
}

void dyn_insert(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xD0;
    v.frame(sp0, 0xD0, lr0);
    const uint32_t array = dyn_array(v, f, 0x00042BD0, self, stack);
    const uint32_t addr = v.r32(k_prop_addr);
    arg0(v, f, 0x00042C60, stack, f + 0x70);
    arg0(v, f, 0x00042CB8, stack, f + 0x74);
    finish(v, stack);
    if (!addr) return;
    const uint32_t count = v.r32(f + 0x74);
    if (!count) return;
    if ((int32_t)count < 0)
        return dyn_count_error(v, f, stack, array, f + 0x78,
                               {0x00042D1C, 0x00042D4C, 0x00042D70, 0x000431F8, 0x0004317C, 0x000431AC, 0x000431D0, 0x00042D9C}, 0x0160AB34,
                               0x00042DBC);
    uint32_t index = v.r32(f + 0x70);
    if ((int32_t)index < 0 || (int32_t)index > (int32_t)v.r32(addr + 4)) {
        const uint32_t num = v.r32(addr + 4);
        dyn_log(v, f, stack, array, f + 0x84, {0x00042F98, 0x00042FC8, 0x00042FEC, 0x000431E4, 0x000430B8, 0x000430E8, 0x0004310C, 0x00043024},
                0x0160ABA4, (int64_t)(int32_t)count, (int64_t)(int32_t)index, (int64_t)(int32_t)num);
        release(v, f, f + 0x84, 0x00043044);
        const int32_t i = (int32_t)v.r32(f + 0x70), n = (int32_t)v.r32(addr + 4);
        index = (uint32_t)(i < 0 ? 0 : n > i ? i : n);
        v.w32(f + 0x70, index);
    }
    const uint32_t num = v.r32(addr + 4);
    const uint32_t add = v.r32(f + 0x74);
    const uint32_t elem = v.r32(v.r32(array + 0x64) + 0x44);
    dyn_reserve(v, f, addr, num + add, elem, 0x00042E50, 0x00042E78);
    const uint32_t data = v.r32(addr);
    v.call(f, 0x00042EAC, func_00F29D8C, data + (add + index) * elem, data + index * elem, (num - index) * elem);
    v.zero(f, 0x00042EC8, v.r32(addr) + index * elem, add * elem);
    const uint32_t init = v.r32(array + 0x64);
    if (!needs_init(v, init)) return;
    for (uint32_t i = v.r32(f + 0x70); (int32_t)i < (int32_t)(v.r32(f + 0x74) + v.r32(f + 0x70)); i++)
        v.call(f, 0x00042F2C, func_000EAEEC, init, v.r32(addr) + i * v.r32(v.r32(array + 0x64) + 0x44));
}

void dyn_remove(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t array = dyn_array(v, f, 0x0004329C, self, stack);
    const uint32_t addr = v.r32(k_prop_addr);
    arg0(v, f, 0x0004332C, stack, f + 0x70);
    arg0(v, f, 0x00043384, stack, f + 0x74);
    finish(v, stack);
    if (!addr) return;
    uint32_t count = v.r32(f + 0x74);
    if (!count) return;
    if ((int32_t)count < 0)
        return dyn_count_error(v, f, stack, array, f + 0x78,
                               {0x000433E8, 0x00043418, 0x0004343C, 0x00043A7C, 0x000439A8, 0x000439D8, 0x000439FC, 0x00043468}, 0x0160AC20,
                               0x00043488);
    uint32_t index = v.r32(f + 0x70);
    const int32_t size = (int32_t)v.r32(addr + 4);
    if ((int32_t)index < 0 || (int32_t)index >= size || (int32_t)(count + index) > size) {
        const uint32_t num = v.r32(addr + 4);
        if (count == 1) {
            dyn_log(v, f, stack, array, f + 0x84,
                    {0x000435EC, 0x0004361C, 0x00043640, 0x00043A24, 0x000438CC, 0x000438FC, 0x00043920, 0x00043674}, 0x0160AC90,
                    (int64_t)(int32_t)index, (int64_t)(int32_t)num);
            release(v, f, f + 0x84, 0x00043694);
        } else {
            dyn_log(v, f, stack, array, f + 0x90,
                    {0x00043744, 0x00043774, 0x00043798, 0x00043A10, 0x00043838, 0x00043868, 0x0004388C, 0x000437D0}, 0x0160AD08,
                    (int64_t)(int32_t)index, (int64_t)(int32_t)(count + index - 1), (int64_t)(int32_t)num);
            release(v, f, f + 0x90, 0x000437F0);
        }
        const int32_t i = (int32_t)v.r32(f + 0x70), n = (int32_t)v.r32(addr + 4);
        index = (uint32_t)(i < 0 ? 0 : n > i ? i : n);
        count = v.r32(f + 0x74);
        v.w32(f + 0x70, index);
        const uint32_t m = v.r32(addr + 4);
        if ((int32_t)(count + index) > (int32_t)m) {
            count = m - index;
            v.w32(f + 0x74, count);
        }
    }
    for (uint32_t last = count + index - 1; (int32_t)last >= (int32_t)index; last--) {
        const uint32_t inner = v.r32(array + 0x64);
        v.vcall(f, 0x00043A54, inner, 0x168, v.r32(addr) + v.r32(inner + 0x44) * last);
        index = v.r32(f + 0x70);
        count = v.r32(f + 0x74);
    }
    uint32_t num = v.r32(addr + 4);
    const uint32_t elem = v.r32(v.r32(array + 0x64) + 0x44);
    if (const uint32_t tail = num - index - count) {
        const uint32_t data = v.r32(addr);
        v.call(f, 0x0004353C, func_00F29D8C, data + index * elem, data + (count + index) * elem, elem * tail);
        num = v.r32(addr + 4);
    }
    dyn_shrink(v, f, addr, num - count, elem, 0x0004355C, 0x00043590);
}

void dyn_skip(Vm& v, uint32_t stack, uint32_t code, uint32_t result)
{
    v.w32(stack + 0x18, code + v.r16(code) + 2);
    v.w32(result, ~0u);
}

uint32_t dyn_item(Vm& v, uint32_t f, uint32_t& sp, uint64_t lr_zero, uint64_t lr_arg, uint32_t stack, uint32_t inner)
{
    uint32_t size = v.r32(inner + 0x44), temp = 0;
    if (size) {
        sp = v.stack_alloc(f, size);
        temp = sp + 0x70;
        size = v.r32(inner + 0x44);
    }
    v.zero(sp, lr_zero, temp, size);
    arg(v, sp, lr_arg, stack, temp);
    finish(v, stack);
    return temp;
}

void dyn_drop(Vm& v, uint32_t sp, uint64_t lr, uint32_t inner, uint32_t temp)
{
    if (v.r64(inner + 0x48) & 0x400000) v.vcall(sp, lr, inner, 0x168, temp);
}

void dyn_add_item(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    const uint32_t array = dyn_array(v, f, 0x0004468C, self, stack);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t addr = v.r32(k_prop_addr);
    if (!addr) return dyn_skip(v, stack, code, result);
    v.w32(stack + 0x18, code + 2);
    const uint32_t inner = v.r32(array + 0x64);
    uint32_t sp = f;
    const uint32_t temp = dyn_item(v, f, sp, 0x00044704, 0x00044750, stack, inner);
    const uint32_t num = v.r32(addr + 4);
    const uint32_t elem = v.r32(inner + 0x44);
    const uint32_t data = dyn_reserve(v, sp, addr, num + 1, elem, 0x0004478C, 0x000447B4);
    v.zero(sp, 0x000447D8, data + num * elem, elem);
    v.vcall(sp, 0x00044814, inner, 0x158, v.r32(addr) + num * v.r32(inner + 0x44), temp, 0, 0, 0);
    dyn_drop(v, sp, 0x00044840, inner, temp);
    v.w32(result, num);
}

void dyn_insert_item(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t array = dyn_array(v, f, 0x000449AC, self, stack);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t object = v.r32(stack + 0x14);
    const uint32_t addr = v.r32(k_prop_addr);
    if (!addr) return dyn_skip(v, stack, code, result);
    const uint32_t inner = v.r32(array + 0x64);
    v.w32(f + 0x80, 0);
    v.w32(stack + 0x18, code + 3);
    step(v, f, 0x00044AA4, object, stack, f + 0x80, v.r8(code + 2));
    uint32_t sp = f;
    const uint32_t temp = dyn_item(v, f, sp, 0x00044AC8, 0x00044B14, stack, inner);
    const uint32_t num = v.r32(addr + 4);
    const uint32_t index = v.r32(f + 0x80);
    const uint32_t elem = v.r32(inner + 0x44);
    const uint32_t data = dyn_reserve(v, sp, addr, num + 1, elem, 0x00044B54, 0x00044B7C);
    v.call(sp, 0x00044BAC, func_00F29D8C, data + index * elem + elem, data + index * elem, (num - index) * elem);
    v.zero(sp, 0x00044BC4, v.r32(addr) + index * elem, elem);
    v.vcall(sp, 0x00044C04, inner, 0x158, v.r32(addr) + v.r32(f + 0x80) * v.r32(inner + 0x44), temp, 0, 0, 0);
    dyn_drop(v, sp, 0x00044C30, inner, temp);
    v.w32(result, v.r32(f + 0x80));
}

void dyn_remove_item(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xD0;
    v.frame(sp0, 0xD0, lr0);
    const uint32_t array = dyn_array(v, f, 0x00044D6C, self, stack);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t addr = v.r32(k_prop_addr);
    if (!addr) return dyn_skip(v, stack, code, result);
    v.w32(stack + 0x18, code + 2);
    const uint32_t inner = v.r32(array + 0x64);
    uint32_t sp = f;
    const uint32_t temp = dyn_item(v, f, sp, 0x00044DE8, 0x00044E34, stack, inner);
    uint32_t found = ~0u;
    for (uint32_t i = 0; (int32_t)i < (int32_t)v.r32(addr + 4); i++) {
        if (!v.vcall(sp, 0x00044E84, inner, 0x13C, temp, v.r32(addr) + i * v.r32(inner + 0x44), 0)) continue;
        found = i;
        v.vcall(sp, 0x00044EC4, inner, 0x168, v.r32(addr) + v.r32(inner + 0x44) * i);
        uint32_t num = v.r32(addr + 4);
        const uint32_t elem = v.r32(inner + 0x44);
        if (num - i - 1) {
            const uint32_t data = v.r32(addr);
            v.call(sp, 0x00044F14, func_00F29D8C, data + i * elem, data + i * elem + elem, elem * (num - i) - elem);
            num = v.r32(addr + 4);
        }
        dyn_shrink(v, sp, addr, num - 1, elem, 0x00044F34, 0x00044F68);
        i--;
    }
    dyn_drop(v, sp, 0x00044FFC, inner, temp);
    v.w32(result, found);
}

constexpr uint32_t k_object_class = 0x019C28D0;
constexpr uint32_t k_sort_bad_delegate = 0x0160ADF8;
constexpr uint32_t k_frame_base_vtable = 0x01691DE8;

uint32_t cast_flags(Vm& v, uint32_t prop)
{
    return v.r32(v.r32(prop + 0x34) + 0xC0);
}

uint32_t array_inner(Vm& v)
{
    const uint32_t prop = v.r32(k_property);
    return v.r32((prop && (cast_flags(v, prop) & 0x200000) ? prop : 0) + 0x64);
}

void array_missing(Vm& v, uint32_t stack, uint32_t result)
{
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + v.r16(code) + 2);
    v.w32(result, ~0u);
}

void bool_fix(Vm& v, uint32_t prop, uint32_t at)
{
    if (prop && (cast_flags(v, prop) & 0x20000) && v.r32(at)) v.w32(at, v.r32(prop + 0x64));
}

void each_copy(Vm& v, uint32_t f, uint64_t lr_item, uint64_t lr_index, uint32_t inner, uint32_t item, uint32_t array, uint32_t index_prop,
               uint32_t index)
{
    v.vcall(f, lr_item, inner, 0x158, item, v.r32(array) + v.r32(f + 0x70) * v.r32(inner + 0x44), 0, 0, 0);
    if (index_prop) v.vcall(f, lr_index, index_prop, 0x158, index, f + 0x70, 0, 0, 0);
}

void dyn_iterator(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x110;
    v.frame(sp0, 0x110, lr0);
    v.w32(k_prop_object, self);
    v.w32(k_property, 0);
    next(v, f, 0x00045110, self, stack, 0);
    const uint32_t prop = v.r32(k_property);
    if (!prop) return;
    const uint32_t array = v.r32(k_prop_addr);
    const uint32_t inner = v.r32(prop + 0x64);
    next(v, f, 0x000451B4, self, stack, 0);
    const uint32_t item = v.r32(k_prop_addr);
    finish(v, stack);
    v.w32(k_prop_addr, 0);
    v.w32(k_property, 0);
    next(v, f, 0x00045224, self, stack, 0);
    uint32_t cls = v.r32(k_object_class);
    const uint32_t index_prop = v.r32(k_property);
    const uint32_t index = v.r32(k_prop_addr);
    if (!cls) {
        const uint32_t made = v.call(f, 0x00045250, func_000E34E0, k_core);
        v.w32(k_object_class, made);
        v.call(f, 0x0004525C, func_000E35C8, made);
        cls = v.r32(k_object_class);
    }
    const bool objects = !cls || child_of(v, v.r32(inner + 0x34), cls);
    v.w32(f + 0x70, 0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t loop = code + 2;
    const uint32_t skip = v.r16(code);
    v.w32(stack + 0x18, loop);
    for (;;) {
        bool more = false;
        if (objects) {
            v.w32(item, 0);
            while (!more && (int32_t)v.r32(f + 0x70) < (int32_t)v.r32(array + 4)) {
                each_copy(v, f, 0x000452E0, 0x00045330, inner, item, array, index_prop, index);
                v.w32(f + 0x70, v.r32(f + 0x70) + 1);
                more = v.r32(item) != 0;
            }
        } else if ((int32_t)v.r32(f + 0x70) < (int32_t)v.r32(array + 4)) {
            each_copy(v, f, 0x00045384, 0x00045444, inner, item, array, index_prop, index);
            v.w32(f + 0x70, v.r32(f + 0x70) + 1);
            more = true;
        }
        if (!more) {
            v.w32(stack + 0x18, v.r32(v.r32(stack + 0x10) + 0x4C) + skip + 1);
            return;
        }
        uint32_t at = v.r32(stack + 0x18), op = v.r8(at);
        while (op != 0x30 && op != 0x31) {
            arg(v, f, 0x000453FC, stack, f + 0x78);
            at = v.r32(stack + 0x18);
            op = v.r8(at);
        }
        v.w32(stack + 0x18, at + 1);
        if (v.r8(at) == 0x31) v.w32(stack + 0x18, loop);
        if (op == 0x30) return;
    }
}

void dyn_find(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    v.w32(k_property, 0);
    v.w32(k_prop_addr, 0);
    v.w32(k_prop_object, self);
    next(v, f, 0x00043B34, self, stack, 0);
    if (!v.r32(k_prop_addr)) return array_missing(v, stack, result);
    v.w32(stack + 0x18, v.r32(stack + 0x18) + 2);
    const uint32_t array = v.r32(k_prop_addr);
    const uint32_t inner = array_inner(v);
    const uint32_t size = v.r32(inner + 0x44);
    uint32_t sp = f, temp = 0;
    if (size) {
        sp = v.stack_alloc(f, size);
        temp = sp + 0x70;
    }
    v.zero(sp, 0x00043BB4, temp, size);
    arg(v, sp, 0x00043C00, stack, temp);
    finish(v, stack);
    bool_fix(v, inner, temp);
    uint32_t found = ~0u;
    for (int32_t i = 0; i < (int32_t)v.r32(array + 4) && found == ~0u; i++)
        if (v.vcall(sp, 0x00043C78, inner, 0x13C, temp, v.r32(array) + (uint32_t)i * v.r32(inner + 0x44), 0)) found = (uint32_t)i;
    if (v.r64(inner + 0x48) & 0x400000) v.vcall(sp, 0x00043D70, inner, 0x168, temp);
    v.w32(result, found);
}

void dyn_find_struct(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xF0;
    v.frame(sp0, 0xF0, lr0);
    v.w32(k_property, 0);
    v.w32(k_prop_addr, 0);
    v.w32(k_prop_object, self);
    next(v, f, 0x00043E48, self, stack, 0);
    if (!v.r32(k_prop_addr)) return array_missing(v, stack, result);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 2);
    const uint32_t array = v.r32(k_prop_addr);
    uint32_t outer = array_inner(v);
    v.w32(f + 0x80, 0);
    v.w32(f + 0x84, 0);
    v.w32(stack + 0x18, code + 3);
    step(v, f, 0x00043EF0, v.r32(stack + 0x14), stack, f + 0x80, v.r8(code + 2));
    if (outer && !(cast_flags(v, outer) & 0x100000)) outer = 0;
    v.w32(f + 0x38, v.r32(f + 0x80));
    v.w32(f + 0x3C, v.r32(f + 0x84));
    const uint32_t member = v.call(f, 0x00043F38, func_00F6399C, v.r32(outer + 0x64), v.r64(f + 0x38));
    const uint32_t size = v.r32(member + 0x44) * v.r32(member + 0x40);
    uint32_t sp = f, temp = 0;
    if (size) {
        sp = v.stack_alloc(f, size);
        temp = sp + 0x70;
    }
    v.zero(sp, 0x00043F6C, temp, size);
    arg(v, sp, 0x00043FBC, stack, temp);
    finish(v, stack);
    bool_fix(v, member, temp);
    uint32_t found = ~0u;
    for (int32_t i = 0; i < (int32_t)v.r32(array + 4) && found == ~0u; i++) {
        const uint32_t elem = v.r32(array) + (uint32_t)i * v.r32(outer + 0x44);
        bool same = true;
        for (int32_t j = 0; same && j < (int32_t)v.r32(member + 0x40); j++) {
            const uint32_t at = (uint32_t)j * v.r32(member + 0x44);
            same = v.vcall(sp, 0x00044054, member, 0x13C, temp + at, elem + v.r32(member + 0x54) + at, 0) != 0;
        }
        if (same) found = (uint32_t)i;
    }
    if (v.r64(outer + 0x48) & 0x400000) v.vcall(sp, 0x00044178, member, 0x168, temp);
    v.w32(result, found);
}

void sort_error(Vm& v, uint32_t f, uint32_t stack)
{
    v.call(f, 0x000456D0, func_000AEB38, f + 0xA0, f + 0x94);
    const uint32_t name = text(v, f + 0xA0);
    if (const uint32_t target = v.r32(f + 0x90)) object_name(v, f, f + 0xAC, target, 0x00045930, 0x000458C4, 0x000458F4, 0x00045918);
    else literal(v, f, f + 0xAC, k_null_owner_text, 0x00045728, 0x00045758, 0x0004577C);
    v.call(f, 0x000457B0, func_000AA46C, stack, 0x301, k_sort_bad_delegate, name, text(v, f + 0xAC));
    release(v, f, f + 0xAC, 0x000457CC);
    if (const uint32_t data = v.r32(f + 0xA0)) {
        v.w32(f + 0xA4, 0);
        v.w32(f + 0xA8, 0);
        v.call(f, 0x000457E8, func_0002ADF0, data);
    }
}

void dyn_sort(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x160;
    v.frame(sp0, 0x160, lr0);
    v.w32(k_property, 0);
    v.w32(k_prop_object, self);
    next(v, f, 0x000455E0, self, stack, 0);
    const uint32_t inner = array_inner(v);
    const uint32_t array = v.r32(k_prop_addr);
    v.zero(f, 0x00045630, result, v.r32(inner + 0x44));
    if (!v.r32(k_prop_addr)) return array_missing(v, stack, result);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(f + 0x90, 0);
    v.w32(f + 0x94, 0);
    v.w32(f + 0x98, 0);
    v.w32(stack + 0x18, code + 3);
    step(v, f, 0x00045690, self, stack, f + 0x90, v.r8(code + 2));
    finish(v, stack);
    uint32_t function = 0;
    if (const uint64_t name = v.r64(f + 0x94))
        if (const uint32_t target = v.r32(f + 0x90)) {
            const uint32_t pending = v.r32(v.r32(v.r32(target) + 0x30));
            if (!v.icall(f, 0x00045948, pending, target, pending, v.r32(stack + 0x18), name)) {
                v.w32(f + 0x38, v.r32(f + 0x94));
                v.w32(f + 0x3C, v.r32(f + 0x98));
                function = v.call(f, 0x00045974, func_000D7644, v.r32(f + 0x90), v.r64(f + 0x38), 0);
            }
        }
    if (!function) return sort_error(v, f, stack);
    if ((int32_t)v.r32(array + 4) <= 0) return;
    uint32_t sp = f, parms = 0, temp = 0;
    if (const uint32_t size = v.r32(function + 0x48)) {
        sp = v.stack_alloc(sp, size);
        parms = sp + 0x70;
    }
    if (const uint32_t size = v.r32(inner + 0x44)) {
        sp = v.stack_alloc(sp, size);
        temp = sp + 0x70;
    }
    bool swapped;
    do {
        swapped = false;
        for (uint32_t i = 0; (int32_t)i < (int32_t)(v.r32(array + 4) - 1); i++) {
            v.zero(sp, 0x00045A1C, parms, v.r32(function + 0x48));
            v.vcall(sp, 0x00045A58, inner, 0x15C, parms, v.r32(array) + i * v.r32(inner + 0x44), 0, 0, 0);
            uint32_t elem = v.r32(inner + 0x44);
            v.vcall(sp, 0x00045A9C, inner, 0x15C, parms + elem, v.r32(array) + (i * elem + elem), 0, 0, 0);
            v.w32(f + 0x9C, 0);
            v.w32(f + 0xBC, 1);
            v.w32(f + 0xC0, 0);
            v.w32(f + 0xC4, 1);
            v.w32(f + 0xB8, k_frame_vtable);
            v.w32(f + 0xC8, function);
            v.w32(f + 0xCC, self);
            const uint32_t script = v.r32(function + 0x4C);
            v.w32(f + 0xD4, parms);
            v.w32(f + 0xD0, script);
            v.w32(f + 0xD8, 0);
            v.w32(f + 0xDC, 0);
            process_internal(v, sp, 0x00045ADC, self, f + 0xB8, f + 0x9C);
            if ((int32_t)v.r32(f + 0x9C) < 0) {
                v.zero(sp, 0x00045B04, temp, v.r32(inner + 0x44));
                elem = v.r32(inner + 0x44);
                v.vcall(sp, 0x00045B44, inner, 0x15C, temp, v.r32(array) + (i * elem + elem), 0, 0, 0);
                elem = v.r32(inner + 0x44);
                const uint32_t data = v.r32(array);
                v.vcall(sp, 0x00045B88, inner, 0x15C, data + (i * elem + elem), data + i * elem, 0, 0, 0);
                v.vcall(sp, 0x00045BC4, inner, 0x15C, v.r32(array) + i * v.r32(inner + 0x44), temp, 0, 0, 0);
                if (v.r64(inner + 0x48) & 0x400000) v.vcall(sp, 0x00045BF8, inner, 0x168, temp);
                swapped = true;
            }
            v.w32(f + 0xB8, k_frame_base_vtable);
        }
    } while (swapped);
    v.vcall(sp, 0x00045C30, inner, 0x15C, result, v.r32(array), 0, 0, 0);
}

uint32_t script_field(Vm& v, uint32_t sp, uint64_t lr, uint32_t& owner, uint32_t prop)
{
    while (owner) {
        for (; prop; prop = v.r32(prop + 0x3C))
            if (cast_flags(v, prop) & 0x8000) return prop;
        owner = v.vcall(sp, lr, owner, 0x130);
        if (owner) prop = v.r32(owner + 0x44);
    }
    return prop;
}

struct CompareSites {
    uint32_t frame;
    uint64_t zero_a, zero_b, a, b, super_a, super_b, compare, free_a, free_b;
};

constexpr CompareSites k_struct_eq{0xE0, 0x00048144, 0x00048154, 0x000481AC, 0x000481FC, 0x0004825C, 0x00048338, 0x000482CC, 0x00048478, 0x0004849C};
constexpr CompareSites k_struct_ne{0xF0, 0x00048558, 0x00048568, 0x000485C0, 0x00048610, 0x00048674, 0x00048750, 0x000486E4, 0x00048890, 0x000488B4};

template <const CompareSites& s, bool equal>
void struct_compare(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - s.frame;
    v.frame(sp0, s.frame, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    v.w32(stack + 0x18, code + 8);
    const uint32_t strukt = (uint32_t)v.r64(code);
    const uint32_t align = v.r32(strukt + 0x58);
    const uint32_t size = (v.r32(strukt + 0x48) + align - 1) & ~(align - 1);
    uint32_t sp = f, a = 0, b = 0;
    if (size) {
        sp = v.stack_alloc(sp, size);
        a = sp + 0x70;
        sp = v.stack_alloc(sp, size);
        b = sp + 0x70;
    }
    v.zero(sp, s.zero_a, a, size);
    v.zero(sp, s.zero_b, b, size);
    next(v, sp, s.a, self, stack, a);
    next(v, sp, s.b, self, stack, b);
    uint32_t owner = strukt;
    uint32_t prop = script_field(v, sp, s.super_a, owner, strukt ? v.r32(strukt + 0x44) : 0);
    bool same = true;
    while (same && prop) {
        for (int32_t i = 0; same && i < (int32_t)v.r32(prop + 0x40); i++) {
            const uint32_t at = (uint32_t)i * v.r32(prop + 0x44) + v.r32(prop + 0x54);
            same = v.vcall(sp, s.compare, prop, 0x13C, a + at, b ? b + at : 0, 0) != 0;
        }
        if (same) prop = script_field(v, sp, s.super_b, owner, v.r32(prop + 0x3C));
    }
    v.w32(result, same == equal);
    for (uint32_t p = v.r32(strukt + 0x64); p; p = v.r32(p + 0x5C)) {
        v.vcall(sp, s.free_a, p, 0x168, a + v.r32(p + 0x54));
        v.vcall(sp, s.free_b, p, 0x168, b + v.r32(p + 0x54));
    }
}

template <uint64_t lr_a, uint64_t lr_b, bool equal>
void delegate_compare(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    for (uint32_t i = 0; i < 12; i += 4) v.w32(f + 0x70 + i, 0);
    arg(v, f, lr_a, stack, f + 0x70);
    for (uint32_t i = 0; i < 12; i += 4) v.w32(f + 0x80 + i, 0);
    arg(v, f, lr_b, stack, f + 0x80);
    finish(v, stack);
    const uint64_t name_a = v.r64(f + 0x74), name_b = v.r64(f + 0x84);
    uint32_t a = v.r32(f + 0x70), b = v.r32(f + 0x80);
    if (!a && name_a) a = self;
    if (!b && name_b) b = self;
    v.w32(result, (a == b && name_a == name_b) == equal);
}

constexpr uint32_t k_space_text = 0x01607F8C;



void str_arg(Vm& v, uint32_t f, uint64_t lr, uint32_t stack, uint32_t at)
{
    v.w32(at, 0);
    v.w32(at + 4, 0);
    v.w32(at + 8, 0);
    arg(v, f, lr, stack, at);
}

uint32_t str_lvalue(Vm& v, uint32_t f, uint32_t stack, uint64_t lr, uint64_t lr_dirty)
{
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    v.w32(k_prop_addr, 0);
    v.w32(f + 0x78, 0);
    return lvalue(v, f, lr, lr_dirty, stack, f + 0x70);
}

void str_optional(Vm& v)
{
    v.w32(k_runtime_flags, v.r32(k_runtime_flags) & ~2u);
}

uint32_t str_len(Vm& v, uint32_t at)
{
    const uint32_t n = v.r32(at + 4);
    return n ? n - 1 : 0;
}

uint32_t str_size(uint32_t count)
{
    return count ? count + 1 : 0;
}

uint32_t str_left(Vm& v, uint32_t at, uint32_t count)
{
    const uint32_t len = str_len(v, at);
    return (int32_t)count < 0 ? 0 : (int32_t)len > (int32_t)count ? count : len;
}

uint32_t str_span(Vm& v, uint32_t at, uint32_t start, uint32_t count, uint32_t& src)
{
    const uint32_t len = str_len(v, at);
    const uint32_t from = len > start ? start : len;
    const uint32_t end = start + count;
    const uint32_t to = end < from ? from : end < len ? end : len;
    src = text(v, at) + from * 2;
    return to - from;
}

void str_grow(Vm& v, uint32_t f, uint32_t at, uint32_t num, uint32_t max, uint64_t lr_slack, uint64_t lr_alloc)
{
    const uint32_t slack = v.call(f, lr_slack, func_00010230, (int64_t)(int32_t)num, (int64_t)(int32_t)max, 2);
    const uint32_t data = v.r32(at);
    v.w32(at + 8, slack);
    if (data | slack) v.w32(at, v.call(f, lr_alloc, func_0002AE4C, data, slack << 1, 8));
}

uint32_t str_extend(Vm& v, uint32_t f, uint32_t at, uint32_t num, uint32_t len, uint64_t lr_slack, uint64_t lr_alloc)
{
    uint32_t pos = num;
    if (pos) {
        len--;
        pos--;
    }
    const uint32_t max = v.r32(at + 8);
    num += len;
    v.w32(at + 4, num);
    if ((int32_t)num > (int32_t)max) str_grow(v, f, at, num, max, lr_slack, lr_alloc);
    return v.r32(at) + pos * 2;
}

uint32_t str_append(Vm& v, uint32_t f, uint32_t at, uint32_t num, uint32_t src, uint64_t lr_slack, uint64_t lr_alloc, uint64_t lr_copy)
{
    const uint32_t len = v.r32(src + 4);
    if (!len) return num;
    const uint32_t dst = str_extend(v, f, at, num, len, lr_slack, lr_alloc);
    v.call(f, lr_copy, func_00F297C8, dst, v.r32(src), v.r32(src + 4) << 1);
    return v.r32(at + 4);
}

void str_reserve(Vm& v, uint32_t f, uint32_t at, uint32_t extra, bool empty, uint64_t lr)
{
    const uint32_t a = v.r32(f + 0x74), b = v.r32(f + 0x80);
    const uint32_t base = (a ? a - 1 : 0) + (b ? b - 1 : 0) + extra;
    const uint32_t want = (int32_t)base > 0 ? base + 1 : base;
    const uint32_t max = v.r32(at + 8), data = v.r32(at);
    if (empty) v.w32(at + 4, 0);
    if (max == want) return;
    v.w32(at + 8, want);
    if (!(data | want)) return;
    if (!empty) v.w32(at + 4, 0);
    v.w32(at, v.call(f, lr, func_0002AE4C, data, want << 1, 8));
}

void str_assign(Vm& v, uint32_t f, uint32_t dst, uint32_t src, uint64_t lr_alloc, uint64_t lr_copy)
{
    if (dst == src) return;
    uint32_t n = v.r32(src + 4), data = v.r32(dst);
    v.w32(dst + 8, n);
    v.w32(dst + 4, n);
    if (data | n) {
        data = v.call(f, lr_alloc, func_0002AE4C, data, n << 1, 8);
        n = v.r32(dst + 4);
        v.w32(dst, data);
    }
    if (n) v.call(f, lr_copy, func_00F297C8, data, text(v, src), n << 1);
}

void str_take(Vm& v, uint32_t f, uint32_t dst, uint32_t slot, uint32_t data, uint32_t size, uint64_t lr_alloc, uint64_t lr_copy,
              uint64_t lr_free)
{
    if (dst != slot) {
        uint32_t to = v.r32(dst), n = size;
        v.w32(dst + 8, size);
        v.w32(dst + 4, size);
        if (to | size) {
            to = v.call(f, lr_alloc, func_0002AE4C, to, size << 1, 8);
            n = v.r32(dst + 4);
            v.w32(dst, to);
        }
        if (n) v.call(f, lr_copy, func_00F297C8, to, size ? data : k_empty_text, n << 1);
    }
    if (data) v.call(f, lr_free, func_0002ADF0, data);
}

void str_clear(Vm& v, uint32_t f, uint32_t at, uint64_t lr_alloc, uint64_t lr_copy)
{
    const uint32_t data = v.r32(at);
    if (data == k_empty_text) return;
    v.w32(at + 8, 0);
    v.w32(at + 4, 0);
    if (!data) return;
    const uint32_t fresh = v.call(f, lr_alloc, func_0002AE4C, data, 0, 8);
    const uint32_t n = v.r32(at + 4);
    v.w32(at, fresh);
    if (n) v.call(f, lr_copy, func_00F297C8, fresh, k_empty_text, n << 1);
}

uint32_t str_dup(Vm& v, uint32_t f, uint32_t src, uint32_t size, uint64_t lr_alloc, uint64_t lr_copy)
{
    if (!size) return 0;
    const uint32_t data = v.call(f, lr_alloc, func_0002AE4C, 0, size << 1, 8);
    v.call(f, lr_copy, func_0002B724, data, src, (int64_t)(int32_t)size);
    return data;
}

uint32_t str_chars(Vm& v, uint32_t f, uint32_t p, uint64_t lr)
{
    return p && v.r16(p) ? v.call(f, lr, func_00F2CE40, p) + 1 : 0;
}

uint32_t str_clone(Vm& v, uint32_t f, uint32_t p, uint32_t size, uint64_t lr_alloc, uint64_t lr_copy)
{
    if (!size) return 0;
    const uint32_t data = v.call(f, lr_alloc, func_0002AE4C, 0, size << 1, 8);
    v.call(f, lr_copy, func_00F297C8, data, p, size << 1);
    return data;
}

uint32_t str_slot(Vm& v, uint32_t f, uint32_t slot, uint32_t size, uint64_t lr_alloc, uint32_t& data)
{
    v.w32(slot, 0);
    v.w32(slot + 4, size);
    v.w32(slot + 8, size);
    data = 0;
    if (!size) return 0;
    data = v.call(f, lr_alloc, func_0002AE4C, 0, size << 1, 8);
    const uint32_t n = v.r32(slot + 4);
    v.w32(slot, data);
    return n;
}

uint32_t str_upper(uint32_t c)
{
    if (c == 0xFF) return 0x9F;
    if (c == 0xF7 || c == 0xF0 || c == 0xDF || c == 0xD0) return c;
    if (c == 0x9C) return 0x8C;
    return (c >= 0x61 && c <= 0x7A) || (c > 0xDF && c < 0xFF) ? c - 32 : c;
}

uint32_t str_lower(uint32_t c)
{
    if (c == 0xF7 || c == 0xF0 || c == 0xDF || c == 0xD0) return c;
    if (c == 0x9F) return 0xFF;
    if (c == 0x8C) return 0x9C;
    return (c >= 0xC0 && c < 0xDF) || (c >= 0x41 && c <= 0x5A) ? c + 0x20 : c;
}

void str_map(Vm& v, uint32_t data, uint32_t size, uint32_t (*map)(uint32_t))
{
    for (int32_t i = 0; i < (int32_t)size; i++) w16(v, data + 2 * i, map(v.r16(data + 2 * i)));
}

void str_upper_slot(Vm& v, uint32_t slot, uint32_t n)
{
    for (uint32_t i = 0; (int32_t)i < (int32_t)n; n = v.r32(slot + 4)) {
        const uint32_t a = v.r32(slot) + 2 * i++;
        w16(v, a, str_upper(v.r16(a)));
    }
}

template <uint64_t lr, void (*cmp)(ppu_context*), uint32_t (*op)(uint32_t, uint32_t)>
void str_compare(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    str_arg(v, f, lr, stack, f + 0x70);
    str_arg(v, f, lr + 0x5C, stack, f + 0x7C);
    finish(v, stack);
    v.w32(result, op(v.call(f, lr + 0xA8, cmp, text(v, f + 0x70), text(v, f + 0x7C)), 0));
    release(v, f, f + 0x7C, lr + 0xE0);
    drop(v, f, f + 0x70, lr + 0x104);
}

void str_concat(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    str_arg(v, f, 0x0005C324, stack, f + 0x70);
    str_arg(v, f, 0x0005C380, stack, f + 0x7C);
    finish(v, stack);
    str_reserve(v, f, result, 1, true, 0x0005C408);
    str_append(v, f, result, v.r32(result + 4), f + 0x70, 0x0005C468, 0x0005C490, 0x0005C4B4);
    str_append(v, f, result, v.r32(result + 4), f + 0x7C, 0x0005C500, 0x0005C524, 0x0005C548);
    release(v, f, f + 0x7C, 0x0005C564);
    drop(v, f, f + 0x70, 0x0005C588);
}

void str_at(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    str_arg(v, f, 0x0005C65C, stack, f + 0x70);
    str_arg(v, f, 0x0005C6B8, stack, f + 0x7C);
    finish(v, stack);
    str_reserve(v, f, result, 2, false, 0x0005C744);
    const uint32_t num = str_append(v, f, result, 0, f + 0x70, 0x0005C7A4, 0x0005C7CC, 0x0005C7F0);
    const uint32_t pos = (int32_t)num > 0 ? num - 1 : 0;
    const uint32_t len = num + ((int32_t)num > 0 ? 1 : 2);
    const uint32_t max = v.r32(result + 8);
    v.w32(result + 4, len);
    if ((int32_t)len > (int32_t)max) str_grow(v, f, result, len, max, 0x0005C83C, 0x0005C860);
    w16(v, v.r32(result) + pos * 2, 0x20);
    w16(v, v.r32(result) + (pos + 1) * 2, 0);
    str_append(v, f, result, v.r32(result + 4), f + 0x7C, 0x0005C8E8, 0x0005C910, 0x0005C934);
    release(v, f, f + 0x7C, 0x0005C950);
    drop(v, f, f + 0x70, 0x0005C974);
}

void str_concat_to(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    const uint32_t at = str_lvalue(v, f, stack, 0x0005D75C, 0x0005D788);
    str_arg(v, f, 0x0005D7F8, stack, f + 0x7C);
    finish(v, stack);
    str_append(v, f, at, v.r32(at + 4), f + 0x7C, 0x0005D850, 0x0005D878, 0x0005D89C);
    str_assign(v, f, result, at, 0x0005D8CC, 0x0005D904);
    release(v, f, f + 0x7C, 0x0005D920);
    drop(v, f, f + 0x70, 0x0005D944);
}

void str_at_to(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    const uint32_t at = str_lvalue(v, f, stack, 0x0005DA30, 0x0005DA5C);
    str_arg(v, f, 0x0005DACC, stack, f + 0x7C);
    finish(v, stack);
    const uint32_t old = v.r32(at + 4);
    const uint32_t chars = v.call(f, 0x0005DAEC, func_00F2CE40, k_space_text);
    const uint32_t num = v.r32(at + 4), max = v.r32(at + 8);
    const uint32_t pos = num ? old - 1 : old;
    const uint32_t len = num + (num ? chars : chars + 1);
    v.w32(at + 4, len);
    if ((int32_t)len > (int32_t)max) str_grow(v, f, at, len, max, 0x0005DB28, 0x0005DB54);
    v.call(f, 0x0005DB74, func_00F297C8, v.r32(at) + pos * 2, k_space_text, (chars + 1) << 1);
    str_append(v, f, at, v.r32(at + 4), f + 0x7C, 0x0005DCA4, 0x0005DCCC, 0x0005DCF0);
    str_assign(v, f, result, at, 0x0005DBB0, 0x0005DBE8);
    release(v, f, f + 0x7C, 0x0005DC04);
    drop(v, f, f + 0x70, 0x0005DC28);
}

void str_subtract_from(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t at = str_lvalue(v, f, stack, 0x0005DDC0, 0x0005DDEC);
    str_arg(v, f, 0x0005DE5C, stack, f + 0x7C);
    finish(v, stack);
    str_clear(v, f, result, 0x0005E30C, 0x0005E330);
    for (uint32_t pos = v.call(f, 0x0005DEB0, func_00F53A94, at, text(v, f + 0x7C), 0, 0, (int64_t)-1); pos != ~0u;
         pos = v.call(f, 0x0005E02C, func_00F53A94, at, text(v, f + 0x7C), 0, 0, (int64_t)-1)) {
        const uint32_t size = str_size(str_left(v, at, pos));
        const uint32_t left = str_dup(v, f, text(v, at), size, 0x0005E0C4, 0x0005E0D8);
        if (size) {
            const uint32_t dst = str_extend(v, f, result, v.r32(result + 4), size, 0x0005E068, 0x0005E090);
            v.call(f, 0x0005E0B0, func_00F297C8, dst, left, size << 1);
        }
        if (left) v.call(f, 0x0005E0F8, func_0002ADF0, left);
        uint32_t src;
        const uint32_t rest = str_size(str_span(v, at, pos + str_len(v, f + 0x7C), 0x7FFFFFFF, src));
        const uint32_t right = str_dup(v, f, src, rest, 0x0005E15C, 0x0005E170);
        str_take(v, f, at, f + 0x88, right, rest, 0x0005E140, 0x0005E120, 0x0005E12C);
    }
    str_append(v, f, result, v.r32(result + 4), at, 0x0005E1D4, 0x0005E1FC, 0x0005E220);
    str_assign(v, f, at, result, 0x0005E250, 0x0005E288);
    release(v, f, f + 0x7C, 0x0005E2A0);
    drop(v, f, f + 0x70, 0x0005E2BC);
}

void str_length(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    str_arg(v, f, 0x0005E3D4, stack, f + 0x70);
    finish(v, stack);
    v.w32(result, str_len(v, f + 0x70));
    drop(v, f, f + 0x70, 0x0005E418);
}

void str_instr(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    str_arg(v, f, 0x0005E4D8, stack, f + 0x7C);
    str_arg(v, f, 0x0005E538, stack, f + 0x88);
    v.w32(f + 0x70, 0);
    str_optional(v);
    arg(v, f, 0x0005E5A0, stack, f + 0x70);
    v.w32(f + 0x74, 0);
    v.w32(f + 0x70, v.r32(f + 0x70) != 0);
    str_optional(v);
    arg(v, f, 0x0005E620, stack, f + 0x74);
    v.w32(f + 0x78, ~0u);
    v.w32(f + 0x74, v.r32(f + 0x74) != 0);
    str_optional(v);
    arg(v, f, 0x0005E6A0, stack, f + 0x78);
    finish(v, stack);
    v.w32(result, v.call(f, 0x0005E6F0, func_00F53A94, f + 0x7C, text(v, f + 0x88), v.r32(f + 0x70), v.r32(f + 0x74),
                         (int64_t)(int32_t)v.r32(f + 0x78)));
    release(v, f, f + 0x88, 0x0005E714);
    drop(v, f, f + 0x7C, 0x0005E738);
}

void str_mid(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    str_arg(v, f, 0x0005E860, stack, f + 0x84);
    arg0(v, f, 0x0005E8BC, stack, f + 0x70);
    v.w32(f + 0x74, 0x7FFFFFFF);
    str_optional(v);
    arg(v, f, 0x0005E928, stack, f + 0x74);
    finish(v, stack);
    uint32_t src;
    const uint32_t size = str_size(str_span(v, f + 0x84, v.r32(f + 0x70), v.r32(f + 0x74), src));
    const uint32_t data = str_dup(v, f, src, size, 0x0005E9E0, 0x0005E9F4);
    str_take(v, f, result, f + 0x78, data, size, 0x0005EA28, 0x0005EA5C, 0x0005EA6C);
    drop(v, f, f + 0x84, 0x0005EA88);
}

void str_left_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    str_arg(v, f, 0x0005EB88, stack, f + 0x80);
    arg0(v, f, 0x0005EBDC, stack, f + 0x70);
    finish(v, stack);
    const uint32_t size = str_size(str_left(v, f + 0x80, v.r32(f + 0x70)));
    const uint32_t data = str_dup(v, f, text(v, f + 0x80), size, 0x0005EC5C, 0x0005EC70);
    str_take(v, f, result, f + 0x74, data, size, 0x0005ECA8, 0x0005ECE4, 0x0005ECF4);
    drop(v, f, f + 0x80, 0x0005ED10);
}

void str_right(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    str_arg(v, f, 0x0005EDF0, stack, f + 0x80);
    arg0(v, f, 0x0005EE4C, stack, f + 0x70);
    finish(v, stack);
    const uint32_t p = text(v, f + 0x80) + str_len(v, f + 0x80) * 2 - str_left(v, f + 0x80, v.r32(f + 0x70)) * 2;
    const uint32_t size = str_chars(v, f, p, 0x0005EEEC);
    const uint32_t data = str_clone(v, f, p, size, 0x0005EF0C, 0x0005EF1C);
    str_take(v, f, result, f + 0x74, data, size, 0x0005EF50, 0x0005EF84, 0x0005EF94);
    drop(v, f, f + 0x80, 0x0005EFB0);
}

template <uint64_t lr_a, uint64_t lr_len, uint64_t lr_take, uint32_t (*map)(uint32_t)>
void str_case(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    str_arg(v, f, lr_a, stack, f + 0x7C);
    finish(v, stack);
    const uint32_t p = text(v, f + 0x7C);
    const uint32_t size = str_chars(v, f, p, lr_len);
    const uint32_t data = str_clone(v, f, p, size, lr_len + 0x20, lr_len + 0x30);
    str_map(v, data, size, map);
    str_take(v, f, result, f + 0x70, data, size, lr_take, lr_take + 0x3C, lr_take + 0x4C);
    drop(v, f, f + 0x7C, lr_take + 0x68);
}

void str_chr(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x0005F57C, stack, f + 0x70);
    w16(v, f + 0x74, v.r32(f + 0x70));
    w16(v, f + 0x76, 0);
    finish(v, stack);
    if (v.r32(result) == f + 0x74) return;
    uint32_t n = str_chars(v, f, f + 0x74, 0x0005F5C0), data = v.r32(result);
    v.w32(result + 8, n);
    v.w32(result + 4, n);
    if (data | n) {
        data = v.call(f, 0x0005F5F4, func_0002AE4C, data, n << 1, 8);
        n = v.r32(result + 4);
        v.w32(result, data);
    }
    if (n) v.call(f, 0x0005F61C, func_00F297C8, data, f + 0x74, n << 1);
}

void str_asc(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    str_arg(v, f, 0x0005F6D4, stack, f + 0x70);
    finish(v, stack);
    v.w32(result, v.r16(text(v, f + 0x70)));
    drop(v, f, f + 0x70, 0x0005F724);
}

constexpr uint64_t k_repl_first[9] = {0x000601DC, 0x0006020C, 0x00060234, 0x00060364, 0x00060384,
                                      0x00060394, 0x000604A4, 0x000605FC, 0x000604C8};
constexpr uint64_t k_repl_next[9] = {0x0005FFE8, 0x00060020, 0x00060188, 0x00060170, 0x00060154,
                                     0x00060164, 0x00060070, 0x00060140, 0x00060130};

uint32_t str_find_caps(Vm& v, uint32_t f, uint32_t slot, const uint64_t (&lr)[9])
{
    const uint32_t p = text(v, f + 0x80);
    uint32_t data;
    uint32_t n = str_slot(v, f, slot, str_chars(v, f, p, lr[0]), lr[1], data);
    if (n) {
        v.call(f, lr[2], func_00F297C8, data, p, n << 1);
        n = v.r32(slot + 4);
    }
    str_upper_slot(v, slot, n);
    const uint32_t q = text(v, f + 0x8C);
    const uint32_t size = str_chars(v, f, q, lr[3]);
    const uint32_t match = str_clone(v, f, q, size, lr[4], lr[5]);
    str_map(v, match, size, str_upper);
    const uint32_t pos = v.call(f, lr[6], func_00F53A94, slot, size ? match : k_empty_text, 0, 0, (int64_t)-1);
    if (match) v.call(f, lr[7], func_0002ADF0, match);
    release(v, f, slot, lr[8]);
    return pos;
}

void str_repl_piece(Vm& v, uint32_t f, uint32_t result, uint32_t pos)
{
    const uint32_t count = str_left(v, f + 0x80, pos);
    const uint32_t src = text(v, f + 0x80);
    uint32_t data;
    uint32_t keep = str_slot(v, f, f + 0xA4, str_size(count), 0x0005FDF0, data);
    if (keep) {
        v.call(f, 0x0005FE14, func_0002B724, data, src, (int64_t)(int32_t)(count + 1));
        keep = v.r32(f + 0xA8);
    }
    uint32_t joined = 0, size = keep;
    if (const uint32_t with = v.r32(f + 0x9C)) {
        uint32_t at = keep, extra = with, buf = 0, max = 0, num = 0;
        if (keep) {
            extra--;
            at--;
        }
        auto fit = [&](uint64_t lr_slack, uint64_t lr_alloc) {
            if ((int32_t)num <= (int32_t)max) return;
            max = v.call(f, lr_slack, func_00010230, (int64_t)(int32_t)num, (int64_t)(int32_t)max, 2);
            if (buf | max) buf = v.call(f, lr_alloc, func_0002AE4C, buf, max << 1, 8);
        };
        if (keep + extra) {
            max = keep + extra;
            buf = v.call(f, 0x0005FE94, func_0002AE4C, 0, max << 1, 8);
        }
        if (keep) {
            num = keep;
            fit(0x0005FEDC, 0x0005FEFC);
            v.call(f, 0x0005FE80, func_00F297C8, buf, v.r32(f + 0xA4), num << 1);
        }
        num += extra;
        fit(0x0005FA7C, 0x0005FA9C);
        v.call(f, 0x0005FABC, func_00F297C8, buf + at * 2, v.r32(f + 0x98), v.r32(f + 0x9C) << 1);
        size = num;
        if (size) {
            joined = v.call(f, 0x0005FE30, func_0002AE4C, 0, size << 1, 8);
            v.call(f, 0x0005FE60, func_00F297C8, joined, buf, size << 1);
        }
        if (buf) v.call(f, 0x0005FEA8, func_0002ADF0, buf);
    } else if (size) {
        joined = v.call(f, 0x0005FE48, func_0002AE4C, 0, size << 1, 8);
        v.call(f, 0x0005FD50, func_00F297C8, joined, v.r32(f + 0xA4), size << 1);
    }
    if (size) {
        const uint32_t dst = str_extend(v, f, result, v.r32(result + 4), size, 0x0005FD94, 0x0005FDBC);
        v.call(f, 0x0005FDDC, func_00F297C8, dst, joined, size << 1);
    }
    if (joined) v.call(f, 0x0005FEB4, func_0002ADF0, joined);
    drop(v, f, f + 0xA4, 0x0005FEC8);
    uint32_t tail_src;
    const uint32_t rest = str_span(v, f + 0x80, pos + str_len(v, f + 0x8C), 0x7FFFFFFF, tail_src);
    uint32_t tail;
    if (str_slot(v, f, f + 0xA4, str_size(rest), 0x00060000, tail))
        v.call(f, 0x00060124, func_0002B724, tail, tail_src, (int64_t)(int32_t)(rest + 1));
    str_assign(v, f, f + 0x80, f + 0xA4, 0x0006003C, 0x0006010C);
    release(v, f, f + 0xA4, 0x00060194);
}

void str_repl(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x100;
    v.frame(sp0, 0x100, lr0);
    str_arg(v, f, 0x0005F7F8, stack, f + 0x80);
    str_arg(v, f, 0x0005F858, stack, f + 0x8C);
    str_arg(v, f, 0x0005F8B8, stack, f + 0x98);
    v.w32(f + 0x70, 0);
    str_optional(v);
    arg(v, f, 0x0005F920, stack, f + 0x70);
    const uint32_t exact = v.r32(f + 0x70);
    finish(v, stack);
    v.w32(f + 0x70, exact != 0);
    uint32_t pos = exact ? v.call(f, 0x0005F97C, func_00F53A94, f + 0x80, text(v, f + 0x8C), 0, 0, (int64_t)-1)
                         : str_find_caps(v, f, f + 0x74, k_repl_first);
    str_clear(v, f, result, 0x00060458, 0x0006047C);
    while (pos != ~0u) {
        str_repl_piece(v, f, result, pos);
        pos = v.r32(f + 0x70) ? v.call(f, 0x000600E8, func_00F53A94, f + 0x80, text(v, f + 0x8C), 0, 0, (int64_t)-1)
                              : str_find_caps(v, f, f + 0xA4, k_repl_next);
    }
    str_append(v, f, result, v.r32(result + 4), f + 0x80, 0x00060524, 0x0006054C, 0x00060570);
    release(v, f, f + 0x98, 0x00060588);
    release(v, f, f + 0x8C, 0x000605A4);
    drop(v, f, f + 0x80, 0x000605C0);
}

constexpr Exec StrEqual = str_compare<0x0005D1BC, func_00F2CB74, ieq>;
constexpr Exec StrNotEqual = str_compare<0x0005D398, func_00F2CB74, ine>;
constexpr Exec StrComplementEqual = str_compare<0x0005D574, func_008A8B50, ieq>;
constexpr Exec StrLess = str_compare<0x0005CA4C, func_00F2CB74, ilt>;
constexpr Exec StrGreater = str_compare<0x0005CC28, func_00F2CB74, igt>;
constexpr Exec StrLessEqual = str_compare<0x0005CE04, func_00F2CB74, ile>;
constexpr Exec StrGreaterEqual = str_compare<0x0005CFE0, func_00F2CB74, ige>;
constexpr Exec StrCaps = str_case<0x0005F090, 0x0005F0DC, 0x0005F1F4, str_upper>;
constexpr Exec StrLocs = str_case<0x0005F30C, 0x0005F358, 0x0005F474, str_lower>;

constexpr uint32_t k_interface_class = 0x019BA328;
constexpr uint32_t k_core_package = 0x019C277C;
constexpr uint32_t k_core_name = 0x01691F90;
constexpr uint32_t k_int_format = 0x016080DC;
constexpr uint32_t k_true_text = 0x019B1CE0;
constexpr uint32_t k_false_text = 0x019B1D60;
constexpr uint32_t k_comma = 0x01608918;
constexpr uint32_t k_dot = 0x01608380;
constexpr uint32_t k_bool_words[8] = {0x016080C0, 0x019B1CE0, 0x016080D0, 0x019B1BE0, 0x016080E4, 0x019B1D60, 0x0160B214, 0x019B1C60};

double frsp(double b)
{
    return b != b ? from_bits64(to_bits64(fp_quiet(b)) & ~((1ull << 29) - 1)) : (double)(float)b;
}

bool vnan(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
uint32_t vfix(uint32_t u) { return vnan(u) ? 0x7FC00000u : u; }

uint32_t vflush(Vm& v, uint32_t u)
{
    return (v.ctx->vscr & 0x10000u) && !(u & 0x7F800000u) ? u & 0x80000000u : u;
}

uint32_t vbin(Vm& v, uint32_t a, uint32_t b, bool sub)
{
    a = vflush(v, a);
    b = vflush(v, b);
    uint32_t r = vfix(to_bits(sub ? from_bits(a) - from_bits(b) : from_bits(a) + from_bits(b)));
    if (vnan(b)) r = b | 0x7FC00000u;
    if (vnan(a)) r = a | 0x7FC00000u;
    return vflush(v, r);
}

uint32_t vmadd(Vm& v, uint32_t a, uint32_t c, uint32_t b)
{
    return vflush(v, vfix(to_bits(std::fma(from_bits(vflush(v, a)), from_bits(vflush(v, c)), from_bits(vflush(v, b))))));
}

uint32_t vnmsub(Vm& v, uint32_t a, uint32_t c, uint32_t b)
{
    return vflush(v, vfix(to_bits(-std::fma(from_bits(vflush(v, a)), from_bits(vflush(v, c)), -from_bits(vflush(v, b))))));
}

uint32_t vrefp(Vm& v, uint32_t b)
{
    b = vflush(v, b);
    const uint32_t r = vfix(to_bits(1.0f / from_bits(b)));
    return vflush(v, vnan(b) ? b | 0x7FC00000u : r);
}

uint32_t vrsqrte(Vm& v, uint32_t b)
{
    b = vflush(v, b);
    if (vnan(b)) return b | 0x7FC00000u;
    return vflush(v, vfix(to_bits(1.0f / std::sqrt(from_bits(b)))));
}

uint32_t vcts(Vm& v, uint32_t a)
{
    const float x = from_bits(a);
    if (x != x) return 0;
    if (x >= 2147483648.0f) {
        v.ctx->vscr |= 1;
        return 0x7FFFFFFFu;
    }
    if (x < -2147483648.0f) {
        v.ctx->vscr |= 1;
        return 0x80000000u;
    }
    return (uint32_t)(int32_t)x;
}

uint32_t vcfsx(uint32_t i) { return to_bits((float)(int32_t)i); }


void store_string(Vm& v, uint32_t f, uint32_t tmp, uint32_t result, uint64_t lr_alloc, uint64_t lr_copy, uint64_t lr_free)
{
    if (result != tmp) {
        uint32_t n = v.r32(tmp + 4), data = v.r32(result), len = n;
        v.w32(result + 8, n);
        v.w32(result + 4, n);
        if (data | n) {
            data = v.call(f, lr_alloc, func_0002AE4C, data, n << 1, 8);
            len = v.r32(result + 4);
            v.w32(result, data);
            n = v.r32(tmp + 4);
        }
        if (len) v.call(f, lr_copy, func_00F297C8, data, n ? v.r32(tmp) : k_empty_text, len << 1);
    }
    drop(v, f, tmp, lr_free);
}

template <uint32_t size, uint32_t at, uint32_t clear, uint64_t lr, void (*op)(Vm&, uint32_t, uint32_t)>
void convert(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - size;
    v.frame(sp0, size, lr0);
    if (clear == 1) v.w8(f + at, 0);
    for (uint32_t i = 0; i < clear / 4; i++) v.w32(f + at + 4 * i, 0);
    arg(v, f, lr, stack, f + at);
    op(v, f, result);
}

void copy_word(Vm& v, uint32_t f, uint32_t result) { v.w32(result, v.r32(f + 0x70)); }
void interface_to_bool(Vm& v, uint32_t f, uint32_t result) { v.w32(result, v.r32(f + 0x70) && v.r32(f + 0x74)); }
void name_to_bool(Vm& v, uint32_t f, uint32_t result) { v.w32(result, v.r64(f + 0x70) != 0); }
void byte_to_int(Vm& v, uint32_t f, uint32_t result) { v.w32(result, v.r8(f + 0x70)); }
void byte_to_bool(Vm& v, uint32_t f, uint32_t result) { v.w32(result, v.r8(f + 0x70) != 0); }
void byte_to_float(Vm& v, uint32_t f, uint32_t result) { v.w32(result & ~3u, stfs((float)v.r8(f + 0x70))); }
void int_to_byte(Vm& v, uint32_t f, uint32_t result) { v.w8(result, (uint8_t)v.r32(f + 0x70)); }
void int_to_bool(Vm& v, uint32_t f, uint32_t result) { v.w32(result, v.r32(f + 0x70) != 0); }
void int_to_float(Vm& v, uint32_t f, uint32_t result) { v.w32(result & ~3u, stfs((float)(int32_t)v.r32(f + 0x70))); }
void bool_to_float(Vm& v, uint32_t f, uint32_t result) { v.w32(result & ~3u, stfs((float)(v.r32(f + 0x70) & 1))); }
void float_to_byte(Vm& v, uint32_t f, uint32_t result) { v.w8(result, (uint8_t)vcts(v, v.r32(f + 0x70))); }
void float_to_int(Vm& v, uint32_t f, uint32_t result) { v.w32(result & ~3u, vcts(v, v.r32(f + 0x70))); }
void float_to_bool(Vm& v, uint32_t f, uint32_t result) { v.w32(result, !(lfs(v.r32(f + 0x70)) == lfs(v.r32(k_float_zero)))); }

void vector_to_bool(Vm& v, uint32_t f, uint32_t result)
{
    const double zero = lfs(v.r32(k_float_zero));
    v.w32(result, !(lfs(v.r32(f + 0x70)) == zero) || !(lfs(v.r32(f + 0x74)) == zero) || !(lfs(v.r32(f + 0x78)) == zero));
}

void rotator_to_bool(Vm& v, uint32_t f, uint32_t result)
{
    v.w32(result, (v.r32(f + 0x70) & 0xFFFF) || (v.r32(f + 0x74) & 0xFFFF) || (v.r32(f + 0x78) & 0xFFFF));
}

void rotator_to_vector(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x0004C7B0, func_0008B878, f + 0x7C, f + 0x70);
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(lfs(v.r32(f + 0x7C + i))));
}

void vector_to_rotator(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x0004C5C4, func_00089B20, f + 0x7C, f + 0x70);
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, v.r32(f + 0x7C + i));
}

template <bool byte, uint64_t lr_parse, uint64_t lr_free>
void string_to_int(Vm& v, uint32_t f, uint32_t result)
{
    const uint32_t value = v.call(f, lr_parse, func_00F31124, text(v, f + 0x70), 0, 10);
    if (byte) v.w8(result, (uint8_t)value);
    else v.w32(result, value);
    drop(v, f, f + 0x70, lr_free);
}

void string_to_bool(Vm& v, uint32_t f, uint32_t result)
{
    uint32_t value = 2;
    for (uint32_t i = 0; i < 8 && value == 2; i++)
        if (!v.call(f, 0x00054450 + 0x30 * i, func_008A8B50, text(v, f + 0x70), k_bool_words[i])) value = i < 4;
    if (value == 2) value = v.call(f, 0x000545D0, func_00F31124, text(v, f + 0x70), 0, 10) != 0;
    v.w32(result, value);
    drop(v, f, f + 0x70, 0x00054608);
}

void string_to_float(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x000546F4, func_00F30974, text(v, f + 0x70), 0, 0);
    v.w32(result, stfs(frsp(v.ctx->fpr[1])));
    drop(v, f, f + 0x70, 0x0005471C);
}

void string_to_vector(Vm& v, uint32_t f, uint32_t result)
{
    const uint32_t s = text(v, f + 0x70);
    double y = lfs(v.r32(k_float_zero)), z = y;
    v.call(f, 0x0004C020, func_00F30974, s, 0, 0);
    const double x = frsp(v.ctx->fpr[1]);
    if (const uint32_t p = v.call(f, 0x0004C038, func_00F2CF74, s, k_comma)) {
        v.call(f, 0x0004C058, func_00F30974, p + 2, 0, 0);
        y = frsp(v.ctx->fpr[1]);
        if (const uint32_t q = v.call(f, 0x0004C068, func_00F2CF74, p + 2, k_comma)) {
            v.call(f, 0x0004C088, func_00F30974, q + 2, 0, 0);
            z = frsp(v.ctx->fpr[1]);
        }
    }
    v.w32(result, stfs(x));
    v.w32(result + 4, stfs(y));
    v.w32(result + 8, stfs(z));
    drop(v, f, f + 0x70, 0x0004C0B8);
}

void string_to_rotator(Vm& v, uint32_t f, uint32_t result)
{
    const uint32_t s = text(v, f + 0x70);
    const uint32_t a = v.call(f, 0x0004C1BC, func_00F31124, s, 0, 10);
    uint32_t b = 0, c = 0;
    if (const uint32_t p = v.call(f, 0x0004C1D4, func_00F2CF74, s, k_comma)) {
        b = v.call(f, 0x0004C200, func_00F31124, p + 2, 0, 10);
        if (const uint32_t q = v.call(f, 0x0004C210, func_00F2CF74, p + 2, k_comma)) c = v.call(f, 0x0004C234, func_00F31124, q + 2, 0, 10);
    }
    v.w32(result, a);
    v.w32(result + 4, b);
    v.w32(result + 8, c);
    drop(v, f, f + 0x70, 0x0004C260);
}

void string_to_name(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x00054144, func_000A4148, f + 0x70, text(v, f + 0x78), 1, 1);
    v.w32(result, v.r32(f + 0x70));
    v.w32(result + 4, v.r32(f + 0x74));
    drop(v, f, f + 0x78, 0x00054174);
}

void name_to_string(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x0005405C, func_000AEA60, f + 0x70, result);
}

void byte_to_string(Vm& v, uint32_t f, uint32_t result)
{
    const uint32_t b = v.r8(f + 0x84);
    if (const uint32_t prop = v.r32(k_property); prop && (v.r32(v.r32(prop + 0x34) + 0xC0) & 0x40))
        if (const uint32_t names = v.r32(prop + 0x64); names && (int32_t)b < (int32_t)v.r32(names + 0x44)) {
            const uint32_t name = v.r32(names + 0x40) + (b << 3);
            const uint32_t index = v.r32(name);
            v.w32(f + 0x70, index);
            v.w32(f + 0x74, v.r32(name + 4));
            v.call(f, 0x0004A1F0, func_000AEA60, f + 0x70, result);
            return;
        }
    v.call(f, 0x0004A0CC, func_0009E0B4, f + 0x78, k_int_format, b);
    store_string(v, f, f + 0x78, result, 0x0004A138, 0x0004A17C, 0x0004A0F0);
}

void int_to_string(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x0004A4D4, func_0009E0B4, f + 0x74, k_int_format, (int64_t)(int32_t)v.r32(f + 0x70));
    store_string(v, f, f + 0x74, result, 0x0004A540, 0x0004A584, 0x0004A4F8);
}

void float_to_string(Vm& v, uint32_t f, uint32_t result)
{
    const double x = lfs(v.r32(f + 0x70));
    v.w64(f + 0x40, to_bits64(x));
    v.ctx->fpr[1] = x;
    v.call(f, 0x0004AC3C, func_0009E0B4, f + 0x74, 0x0160B19C, v.r64(f + 0x40));
    store_string(v, f, f + 0x74, result, 0x0004ACA8, 0x0004ACEC, 0x0004AC60);
}

void vector_to_string(Vm& v, uint32_t f, uint32_t result)
{
    for (uint32_t i = 0; i < 3; i++) {
        const double x = lfs(v.r32(f + 0x7C + 4 * i));
        v.ctx->fpr[1 + i] = x;
        v.w64(f + 0x40 + 8 * i, to_bits64(x));
    }
    v.call(f, 0x0004C460, func_0009E0B4, f + 0x70, 0x0160B1A8, v.r64(f + 0x40), v.r64(f + 0x48), v.r64(f + 0x50));
    store_string(v, f, f + 0x70, result, 0x0004C4CC, 0x0004C510, 0x0004C484);
}

void rotator_to_string(Vm& v, uint32_t f, uint32_t result)
{
    v.call(f, 0x0004C8A0, func_0009E0B4, f + 0x70, 0x0160B1C8, (int64_t)(int32_t)v.r32(f + 0x7C), (int64_t)(int32_t)v.r32(f + 0x80),
           (int64_t)(int32_t)v.r32(f + 0x84));
    store_string(v, f, f + 0x70, result, 0x0004C90C, 0x0004C950, 0x0004C8C4);
}

void bool_to_string(Vm& v, uint32_t f, uint32_t result)
{
    const uint32_t b = v.r32(f + 0x70);
    v.w32(f + 0x70, b != 0);
    uint32_t data = v.r32(result);
    const uint32_t str = b ? k_true_text : k_false_text;
    if (data == str) return;
    uint32_t len = 0;
    if (v.r16(str)) {
        len = v.call(f, 0x0004A8B0, func_00F2CE40, str) + 1;
        data = v.r32(result);
    }
    v.w32(result + 8, len);
    v.w32(result + 4, len);
    if (data | len) {
        data = v.call(f, 0x0004A8E0, func_0002AE4C, data, len << 1, 8);
        len = v.r32(result + 4);
        v.w32(result, data);
    }
    if (len) v.call(f, 0x0004A908, func_00F297C8, data, str, len << 1);
}

void object_to_string(Vm& v, uint32_t f, uint32_t result)
{
    if (const uint32_t obj = v.r32(f + 0x70)) object_name(v, f, f + 0x74, obj, 0x00053580, 0x000534FC, 0x0005352C, 0x00053550);
    else literal(v, f, f + 0x74, k_null_owner_text, 0x000533B4, 0x000533E4, 0x00053408);
    store_string(v, f, f + 0x74, result, 0x00053478, 0x000534BC, 0x00053430);
}

void interface_to_string(Vm& v, uint32_t f, uint32_t result)
{
    const uint32_t obj = v.r32(f + 0x70);
    if (obj && v.r32(f + 0x74)) object_name(v, f, f + 0x78, obj, 0x00053810, 0x0005377C, 0x000537AC, 0x000537D0);
    else literal(v, f, f + 0x78, 0x0160B1FC, 0x000537F4, 0x00053664, 0x00053688);
    store_string(v, f, f + 0x78, result, 0x000536F8, 0x0005373C, 0x000536B0);
}

uint32_t interface_class(Vm& v, uint32_t f)
{
    uint32_t made = 0;
    if (const uint32_t mem = v.call(f, 0x00053100, func_0002AED0, 0x160, 8)) {
        const uint64_t vtable = v.r64(0x000140B0);
        v.w32(f + 0x80, v.r32(0x01607DA4));
        v.w32(f + 0x84, v.r32(0x01607DA8));
        v.w32(f + 0x88, v.r32(0x01607DAC));
        v.w32(f + 0x8C, v.r32(0x01607DB0));
        v.w64(f + 0x70, vtable);
        v.w64(f + 0x78, 0x0195C3B0);
        v.call(f, 0x000532B0, func_0003A134, mem, 0, 0x40, 0x4001, 0, 0x0160A8B2, k_core, 0x01691EA8);
        made = mem;
    }
    uint32_t package = v.r32(k_core_package);
    v.w32(k_interface_class, made);
    if (!package) {
        const uint32_t p = v.call(f, 0x00053130, func_000BC2E0, k_core_name);
        v.w32(k_core_package, p);
        v.call(f, 0x0005313C, func_000BC288, p);
        package = v.r32(k_core_package);
        made = v.r32(k_interface_class);
    }
    uint32_t outer = package;
    if (!package) {
        const uint32_t p = v.call(f, 0x00053160, func_000BC2E0, k_core_name);
        v.w32(k_core_package, p);
        v.call(f, 0x0005316C, func_000BC288, p);
        outer = v.r32(k_core_package);
    }
    v.call(f, 0x00053180, func_0003ADB0, package, made, outer);
    return v.r32(k_interface_class);
}

bool implements(Vm& v, uint32_t c, uint32_t cls)
{
    for (; c; c = v.r32(c + 0x40))
        for (int32_t i = 0; i < (int32_t)v.r32(c + 0x144); i++)
            for (uint32_t x = v.r32(v.r32(c + 0x140) + 8 * (uint32_t)i); x; x = v.r32(x + 0x40))
                if (x == cls) return true;
    return false;
}

void object_to_interface(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    const uint32_t code = v.r32(stack + 0x18);
    const uint32_t object = v.r32(stack + 0x14);
    uint32_t cls = 0;
    if (const uint32_t named = (uint32_t)v.r64(code)) {
        v.w32(stack + 0x18, code + 8);
        if (v.r32(v.r32(named + 0x34) + 0xC0) & 0x20) cls = named;
    }
    v.w32(f + 0x90, 0);
    v.w32(stack + 0x18, code + 9);
    step(v, f, 0x000530B8, object, stack, f + 0x90, v.r8(code + 8));
    if (const uint32_t obj = v.r32(f + 0x90)) {
        const uint32_t c = v.r32(obj + 0x34);
        if (cls && (v.r32(cls + 0xBC) & 0x4000)) {
            uint32_t iface = v.r32(k_interface_class);
            if (!iface) {
                iface = interface_class(v, f);
            }
            if (cls != iface && implements(v, c, cls)) {
                v.w32(result, obj);
                v.w32(result + 4, v.call(f, 0x000532E4, func_000D9BD0, obj, cls));
                return;
            }
        }
    }
    v.w32(result, 0);
    v.w32(result + 4, 0);
}

struct Text {
    uint32_t data, num;
};

void grow(Vm& v, uint32_t f, uint64_t lr_slack, uint64_t lr_alloc, uint32_t n, uint32_t& data, uint32_t& max)
{
    if ((int32_t)n <= (int32_t)max) return;
    max = v.call(f, lr_slack, func_00010230, (int64_t)(int32_t)n, (int64_t)(int32_t)max, 2);
    if (data | max) data = v.call(f, lr_alloc, func_0002AE4C, data, max << 1, 8);
}

Text duplicate(Vm& v, uint32_t f, uint64_t lr_alloc, uint64_t lr_copy, uint32_t src, uint32_t num)
{
    uint32_t data = 0;
    if (num) data = v.call(f, lr_alloc, func_0002AE4C, 0, num << 1, 8);
    if (num) v.call(f, lr_copy, func_00F297C8, data, src, num << 1);
    return {data, num};
}

Text concat(Vm& v, uint32_t f, const uint64_t* lr, uint32_t num, uint32_t extra, uint32_t base, uint32_t at, uint32_t src, uint32_t count)
{
    uint32_t data = 0, max = 0, used = 0;
    if (const uint32_t total = num + extra) {
        max = total;
        data = v.call(f, lr[0], func_0002AE4C, 0, max << 1, 8);
    }
    if (num) {
        used = num;
        grow(v, f, lr[1], lr[2], used, data, max);
        v.call(f, lr[3], func_00F297C8, data, base, used << 1);
    }
    const uint32_t size = used + extra;
    grow(v, f, lr[4], lr[5], size, data, max);
    v.call(f, lr[6], func_00F297C8, data + 2 * at, src, count << 1);
    const Text out = duplicate(v, f, lr[7], lr[8], data, size);
    if (data) v.call(f, lr[9], func_0002ADF0, data);
    return out;
}

constexpr uint64_t k_path_dot[10] = {0x00053AE4, 0x00053B10, 0x00053B30, 0x00053B44, 0x00053B60,
                                     0x00053B80, 0x00053B9C, 0x00053BBC, 0x00053BD8, 0x00053BE8};
constexpr uint64_t k_path_name[10] = {0x00053C40, 0x00053C68, 0x00053C88, 0x00053C9C, 0x00053CB8,
                                      0x00053CD8, 0x00053CF8, 0x00053D18, 0x00053D34, 0x00053D44};

void delegate_to_string(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xE0;
    v.frame(sp0, 0xE0, lr0);
    v.w32(f + 0x7C, 0);
    v.w32(f + 0x80, 0);
    v.w32(f + 0x84, 0);
    arg(v, f, 0x00053A50, stack, f + 0x7C);
    const uint32_t obj = v.r32(f + 0x7C);
    v.call(f, 0x00053A70, func_000C2DD4, f + 0x88, obj ? obj : self, 0);
    Text path;
    if (v.r16(k_dot)) {
        uint32_t at = v.r32(f + 0x8C);
        const uint32_t len = v.call(f, 0x00053A94, func_00F2CE40, k_dot);
        const uint32_t num = v.r32(f + 0x8C);
        uint32_t extra = len + 1;
        if (num) {
            extra = len;
            at--;
        }
        path = concat(v, f, k_path_dot, num, extra, v.r32(f + 0x88), at, k_dot, len + 1);
    } else {
        path = duplicate(v, f, 0x00053EB4, 0x00053ED0, v.r32(f + 0x88), v.r32(f + 0x8C));
    }
    v.call(f, 0x00053BF4, func_000AEB38, f + 0x94, f + 0x80);
    Text full;
    if (const uint32_t name = v.r32(f + 0x98)) {
        uint32_t extra = name, at = path.num;
        if (at) {
            extra--;
            at--;
        }
        full = concat(v, f, k_path_name, path.num, extra, path.data, at, v.r32(f + 0x94), v.r32(f + 0x98));
    } else {
        full = duplicate(v, f, 0x00053E70, 0x00053E8C, path.data, path.num);
    }
    release(v, f, f + 0x94, 0x00053D60);
    if (path.data) v.call(f, 0x00053D78, func_0002ADF0, path.data);
    release(v, f, f + 0x88, 0x00053D94);
    if (result != f + 0x70) {
        uint32_t data = v.r32(result), len = full.num;
        v.w32(result + 8, full.num);
        v.w32(result + 4, full.num);
        if (data | full.num) {
            data = v.call(f, 0x00053DD0, func_0002AE4C, data, full.num << 1, 8);
            len = v.r32(result + 4);
            v.w32(result, data);
        }
        if (len) v.call(f, 0x00053E04, func_00F297C8, data, full.num ? full.data : k_empty_text, len << 1);
    }
    if (full.data) v.call(f, 0x00053E14, func_0002ADF0, full.data);
}

constexpr Exec InterfaceToObject = convert<0x80, 0x70, 8, 0x00053974, copy_word>;
constexpr Exec InterfaceToString = convert<0xA0, 0x70, 8, 0x0005360C, interface_to_string>;
constexpr Exec InterfaceToBool = convert<0x80, 0x70, 8, 0x00053898, interface_to_bool>;
constexpr Exec RotatorToVector = convert<0xA0, 0x70, 12, 0x0004C7A4, rotator_to_vector>;
constexpr Exec ByteToInt = convert<0x80, 0x70, 1, 0x00049E4C, byte_to_int>;
constexpr Exec ByteToBool = convert<0x80, 0x70, 1, 0x00049F00, byte_to_bool>;
constexpr Exec ByteToFloat = convert<0x80, 0x70, 1, 0x00049FC0, byte_to_float>;
constexpr Exec IntToByte = convert<0x80, 0x70, 4, 0x0004A280, int_to_byte>;
constexpr Exec IntToBool = convert<0x80, 0x70, 4, 0x0004A334, int_to_bool>;
constexpr Exec IntToFloat = convert<0x80, 0x70, 4, 0x0004A3F4, int_to_float>;
constexpr Exec BoolToByte = convert<0x80, 0x70, 4, 0x0004A620, int_to_byte>;
constexpr Exec BoolToInt = convert<0x80, 0x70, 4, 0x0004A6D8, copy_word>;
constexpr Exec BoolToFloat = convert<0x80, 0x70, 4, 0x0004A790, bool_to_float>;
constexpr Exec FloatToByte = convert<0x80, 0x70, 4, 0x0004A9B8, float_to_byte>;
constexpr Exec FloatToInt = convert<0x80, 0x70, 4, 0x0004AA78, float_to_int>;
constexpr Exec FloatToBool = convert<0x90, 0x70, 4, 0x0004AB50, float_to_bool>;
constexpr Exec NameToBool = convert<0x80, 0x70, 8, 0x00053F70, name_to_bool>;
constexpr Exec StringToByte = convert<0x90, 0x70, 12, 0x00054228, string_to_int<true, 0x00054250, 0x00054274>>;
constexpr Exec StringToInt = convert<0x90, 0x70, 12, 0x00054328, string_to_int<false, 0x00054350, 0x00054374>>;
constexpr Exec StringToBool = convert<0x90, 0x70, 12, 0x00054428, string_to_bool>;
constexpr Exec StringToFloat = convert<0x90, 0x70, 12, 0x000546CC, string_to_float>;
constexpr Exec StringToVector = convert<0xB0, 0x70, 12, 0x0004BFE4, string_to_vector>;
constexpr Exec StringToRotator = convert<0xB0, 0x70, 12, 0x0004C190, string_to_rotator>;
constexpr Exec VectorToBool = convert<0x90, 0x70, 12, 0x0004C338, vector_to_bool>;
constexpr Exec VectorToRotator = convert<0xA0, 0x70, 12, 0x0004C5B8, vector_to_rotator>;
constexpr Exec RotatorToBool = convert<0x90, 0x70, 12, 0x0004C694, rotator_to_bool>;
constexpr Exec ByteToString = convert<0xA0, 0x84, 1, 0x0004A08C, byte_to_string>;
constexpr Exec IntToString = convert<0x90, 0x70, 4, 0x0004A4B8, int_to_string>;
constexpr Exec BoolToString = convert<0x90, 0x70, 4, 0x0004A85C, bool_to_string>;
constexpr Exec FloatToString = convert<0x90, 0x70, 4, 0x0004AC1C, float_to_string>;
constexpr Exec ObjectToString = convert<0x90, 0x70, 4, 0x00053378, object_to_string>;
constexpr Exec NameToString = convert<0x90, 0x70, 8, 0x00054050, name_to_string>;
constexpr Exec VectorToString = convert<0xA0, 0x7C, 0, 0x0004C428, vector_to_string>;
constexpr Exec RotatorToString = convert<0xA0, 0x7C, 0, 0x0004C874, rotator_to_string>;
constexpr Exec StringToName = convert<0x90, 0x78, 12, 0x00054114, string_to_name>;

constexpr uint32_t k_sine = 0x019EECDC;
constexpr uint32_t k_rand_seed = 0x019FED20;
constexpr uint32_t k_normal = 0x0004B380;
constexpr uint32_t k_rot_rand = 0x0004C96C;
constexpr uint32_t k_vrand = 0x0004EB88;
constexpr uint32_t k_vector_scale = 0x0005019C;
constexpr uint32_t k_rotate_sign = 0x00050EB4;
constexpr uint32_t k_neg_zero = 0x80000000u;

double fma_single(double a, double c, double b, bool neg_b, bool neg_r)
{
    if (a != a) return single(fp_quiet(a));
    if (b != b) return single(fp_quiet(b));
    if (c != c) return single(fp_quiet(c));
    const double r = std::fma(a, c, neg_b ? -b : b);
    if (r != r) return single(fp_result(r));
    return single(neg_r ? -r : r);
}

double fmsubs(double a, double c, double b) { return fma_single(a, c, b, true, false); }
double fnmadds(double a, double c, double b) { return fma_single(a, c, b, false, true); }
double fnmsubs(double a, double c, double b) { return fma_single(a, c, b, true, true); }

int64_t fctidz(double x)
{
    if (x != x) return INT64_MIN;
    const double r = std::trunc(x);
    if (r >= 9223372036854775807.0) return INT64_MAX;
    if (r <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)r;
}

uint32_t fctiwz(double x)
{
    if (x != x) return 0x80000000u;
    const double r = std::trunc(x);
    if (r >= 2147483647.0) return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

uint32_t flerp(uint32_t a, uint32_t b, uint32_t t) { return stfs(fmadds(lfs(t), fsubs(lfs(b), lfs(a)), lfs(a))); }

uint32_t lane(Vm& v, uint32_t ea, uint32_t i) { return (ea & 15) + i * 4 < 16 ? v.r32(ea + i * 4) : 0; }

uint32_t sine(Vm& v, uint32_t angle) { return v.r32(k_sine + (angle & 0xFFFC)); }

template <uint64_t lr_a, void (*op)(Vm&, uint32_t, uint32_t)>
void vector_unary(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg(v, f, lr_a, stack, f + 0x70);
    finish(v, stack);
    op(v, f + 0x70, result);
}

template <uint64_t lr_a, uint64_t lr_dirty, uint64_t lr_b, uint32_t (*op)(uint32_t, uint32_t)>
void vector_assign(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xB0;
    v.frame(sp0, 0xB0, lr0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x70);
    arg(v, f, lr_b, stack, f + 0x7C);
    finish(v, stack);
    uint32_t r[3];
    for (uint32_t i = 0; i < 3; i++) {
        r[i] = op(v.r32(at + i * 4), v.r32(f + 0x7C + i * 4));
        v.w32(at + i * 4, r[i]);
    }
    for (uint32_t i = 0; i < 3; i++) v.w32(result + i * 4, r[i]);
}

template <uint32_t size, uint64_t lr_a, uint64_t lr_dirty, uint64_t lr_b, uint64_t lr_log>
void vector_scale_by(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - size;
    v.frame(sp0, size, lr0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x74);
    arg0(v, f, lr_b, stack, f + 0x70);
    finish(v, stack);
    double s = lfs(v.r32(f + 0x70));
    if constexpr (lr_log != 0) {
        if (s == lfs(v.r32(k_vector_scale))) {
            v.call(f, lr_log, func_000AA46C, stack, 0x301, k_divide_zero);
            s = lfs(v.r32(f + 0x70));
        }
        s = fdivs(lfs(v.r32(k_vector_scale + 4)), s);
    }
    const double x = fmuls(lfs(v.r32(at)), s), y = fmuls(lfs(v.r32(at + 4)), s), z = fmuls(lfs(v.r32(at + 8)), s);
    v.w32(at, stfs(x));
    v.w32(at + 4, stfs(y));
    v.w32(at + 8, stfs(z));
    v.w32(result, stfs(x));
    v.w32(result + 4, stfs(y));
    v.w32(result + 8, stfs(z));
}

double vlen2(Vm& v, uint32_t a)
{
    const double x = lfs(v.r32(a)), y = lfs(v.r32(a + 4)), z = lfs(v.r32(a + 8));
    return fmadds(z, z, fmadds(x, x, fmuls(y, y)));
}

void vsize(Vm& v, uint32_t a, uint32_t result) { v.w32(result, stfs(single(std::sqrt(vlen2(v, a))))); }
void vsize_sq(Vm& v, uint32_t a, uint32_t result) { v.w32(result, stfs(vlen2(v, a))); }

void vnormal(Vm& v, uint32_t a, uint32_t result)
{
    double x = lfs(v.r32(a)), y = lfs(v.r32(a + 4)), z = lfs(v.r32(a + 8));
    const double s = vlen2(v, a), one = lfs(v.r32(k_normal));
    if (!(s == one)) {
        const double half = lfs(v.r32(k_normal + 0xC)), other = lfs(v.r32(k_normal + 8));
        const double d = fp_bin(s, lfs(v.r32(k_normal + 4)), s - lfs(v.r32(k_normal + 4)));
        double r = 1.0 / std::sqrt(s);
        for (int i = 0; i < 3; i++) r = fmadds(fnmsubs(fmuls(s, r), r, one), fmuls(half, r), r);
        x = d >= 0.0 ? fmuls(x, r) : other;
        y = d >= 0.0 ? fmuls(y, r) : other;
        z = d >= 0.0 ? fmuls(z, r) : other;
    }
    v.w32(result, stfs(x));
    v.w32(result + 4, stfs(y));
    v.w32(result + 8, stfs(z));
}

void vzero(Vm& v, uint32_t a, uint32_t result)
{
    const double zero = lfs(v.r32(k_float_zero));
    v.w32(result, lfs(v.r32(a)) == zero && lfs(v.r32(a + 4)) == zero && lfs(v.r32(a + 8)) == zero);
}

void vcross(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    const double ax = lfs(v.r32(a)), ay = lfs(v.r32(a + 4)), az = lfs(v.r32(a + 8));
    const double bx = lfs(v.r32(b)), by = lfs(v.r32(b + 4)), bz = lfs(v.r32(b + 8));
    v.w32(result, stfs(fmsubs(ay, bz, fmuls(az, by))));
    v.w32(result + 8, stfs(fmsubs(ax, by, fmuls(ay, bx))));
    v.w32(result + 4, stfs(fmsubs(az, bx, fmuls(ax, bz))));
}

void vmul(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, stfs(fmuls(lfs(v.r32(a + i)), lfs(v.r32(b + i)))));
}

void radd(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, v.r32(a + i) + v.r32(b + i));
}

void rsub(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    for (uint32_t i = 0; i < 12; i += 4) v.w32(result + i, v.r32(a + i) - v.r32(b + i));
}

bool same_rotator(Vm& v, uint32_t a, uint32_t b)
{
    return v.r32(a) == v.r32(b) && v.r32(a + 4) == v.r32(b + 4) && v.r32(a + 8) == v.r32(b + 8);
}

void requal(Vm& v, uint32_t a, uint32_t b, uint32_t result) { v.w32(result, same_rotator(v, a, b)); }
void runequal(Vm& v, uint32_t a, uint32_t b, uint32_t result) { v.w32(result, !same_rotator(v, a, b)); }

void rscale(Vm& v, uint32_t a, uint32_t, uint32_t result)
{
    const uint32_t out = result & ~3u;
    uint32_t r[3];
    for (uint32_t k = 0; k < 3; k++)
        for (uint32_t i = 0; i < 4; i++) {
            const uint32_t c = vcts(v, vmadd(v, vcfsx(lane(v, a + 4 + k * 4, i)), lane(v, a, i), k_neg_zero));
            if (!i) r[k] = c;
        }
    for (uint32_t k = 0; k < 3; k++) v.w32(out + k * 4, r[k]);
}

void vrotate(Vm& v, uint32_t a, uint32_t b, uint32_t result)
{
    const uint32_t n = k_neg_zero;
    const uint32_t pitch = v.r32(b), yaw = v.r32(b + 4), roll = v.r32(b + 8);
    const uint32_t sp = sine(v, pitch), cr = sine(v, roll + 0x4000), sr = sine(v, roll), sy = sine(v, yaw), cy = sine(v, yaw + 0x4000),
                   cp = sine(v, pitch + 0x4000), sign = v.r32(k_rotate_sign);
    const uint32_t crsp = vmadd(v, cr, sp, n), spsr = vmadd(v, sp, sr, n), crcy = vmadd(v, cr, cy, n);
    const uint32_t crsy = vmadd(v, cr, sy, n), srsy = vmadd(v, sr, sy, n), crspsy = vmadd(v, crsp, sy, n);
    const uint32_t m1 = vmadd(v, spsr, sy, crcy), cpsy = vmadd(v, cp, sy, n);
    const uint32_t m2 = vnmsub(v, spsr, cy, crsy) ^ sign, m3 = vmadd(v, crsp, cy, srsy) ^ sign;
    const uint32_t m4 = vnmsub(v, cy, sr, crspsy) ^ sign, cpsr = vmadd(v, cp, sr, n) ^ sign;
    const uint32_t cpcy = vmadd(v, cp, cy, n), cpcr = vmadd(v, cp, cr, n);
    const uint32_t x = v.r32(a), y = v.r32(a + 4), z = v.r32(a + 8), out = result & ~3u;
    const uint32_t ycpsy = vmadd(v, y, cpsy, n), ym1 = vmadd(v, y, m1, n), ym4 = vmadd(v, y, m4, n);
    const uint32_t rx = vmadd(v, x, cpcy, ycpsy), ry = vmadd(v, x, m2, ym1), rz = vmadd(v, x, m3, ym4);
    v.w32(out, vmadd(v, z, sp, rx));
    v.w32(out + 4, vmadd(v, z, cpsr, ry));
    v.w32(out + 8, vmadd(v, z, cpcr, rz));
}

void unrotate(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    arg(v, f, 0x00050D1C, stack, f + 0x70);
    arg(v, f, 0x00050D70, stack, f + 0x7C);
    finish(v, stack);
    const uint32_t pitch = v.r32(f + 0x7C), yaw = v.r32(f + 0x80), roll = v.r32(f + 0x84);
    const double sr = lfs(sine(v, roll)), sp = lfs(sine(v, pitch)), sy = lfs(sine(v, yaw));
    const double cp = lfs(sine(v, pitch + 0x4000)), cr = lfs(sine(v, roll + 0x4000)), cy = lfs(sine(v, yaw + 0x4000));
    const double x = lfs(v.r32(f + 0x70)), y = lfs(v.r32(f + 0x74)), z = lfs(v.r32(f + 0x78));
    const double spsr = fmuls(sp, sr), srsy = fmuls(sr, sy), cpsy = fmuls(cp, sy), crsp = fmuls(cr, sp);
    const double ycpsr = fmuls(fmuls(cp, sr), y), m1 = fmsubs(spsr, cy, fmuls(cr, sy)), crspsy = fmuls(crsp, sy);
    const double m2 = fmadds(spsr, sy, fmuls(cr, cy)), m3 = fnmadds(crsp, cy, srsy), m4 = fmsubs(cy, sr, crspsy);
    const double rx = fmadds(x, sp, -ycpsr), ry = fmadds(x, fmuls(cp, cy), fmuls(y, m1)), rz = fmadds(x, cpsy, fmuls(y, m2));
    v.w32(result + 8, stfs(fmadds(z, fmuls(cp, cr), rx)));
    v.w32(result, stfs(fmadds(z, m3, ry)));
    v.w32(result + 4, stfs(fmadds(z, m4, rz)));
}

void get_axes(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xD0;
    v.frame(sp0, 0xD0, lr0);
    arg(v, f, 0x0004D1C8, stack, f + 0x70);
    v.w32(k_prop_addr, 0);
    const uint32_t x = lvalue(v, f, 0x0004D22C, 0x0004D258, stack, f + 0x7C);
    v.w32(k_prop_addr, 0);
    const uint32_t y = lvalue(v, f, 0x0004D2CC, 0x0004D2F8, stack, f + 0x88);
    v.w32(k_prop_addr, 0);
    const uint32_t z = lvalue(v, f, 0x0004D36C, 0x0004D398, stack, f + 0x94);
    finish(v, stack);
    const uint32_t pitch = v.r32(f + 0x70), yaw = v.r32(f + 0x74), roll = v.r32(f + 0x78);
    const double cr = lfs(sine(v, roll + 0x4000)), sp = lfs(sine(v, pitch)), sr = lfs(sine(v, roll));
    const double sy = lfs(sine(v, yaw)), cp = lfs(sine(v, pitch + 0x4000)), cy = lfs(sine(v, yaw + 0x4000));
    const double crsp = fmuls(cr, sp), spsr = fmuls(sp, sr);
    v.w32(x + 8, stfs(sp));
    v.w32(x, stfs(fmuls(cp, cy)));
    v.w32(x + 4, stfs(fmuls(cp, sy)));
    v.w32(y, stfs(fmsubs(spsr, cy, fmuls(cr, sy))));
    v.w32(y + 8, stfs(-fmuls(cp, sr)));
    v.w32(y + 4, stfs(fmadds(spsr, sy, fmuls(cr, cy))));
    v.w32(z, stfs(fnmadds(crsp, cy, fmuls(sr, sy))));
    v.w32(z + 4, stfs(fmsubs(cy, sr, fmuls(crsp, sy))));
    v.w32(z + 8, stfs(fmuls(cp, cr)));
}

double rand_float(Vm& v, uint32_t f, uint64_t lr)
{
    v.w64(f + 0x78, (uint64_t)(int64_t)(int32_t)v.call(f, lr, func_00F2C010));
    return single((double)(int64_t)v.r64(f + 0x78));
}

void rot_rand(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xC0;
    v.frame(sp0, 0xC0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(k_runtime_flags, v.r32(k_runtime_flags) & ~2u);
    arg(v, f, 0x0004CA18, stack, f + 0x70);
    finish(v, stack);
    v.w32(f + 0x70, v.r32(f + 0x70) != 0);
    const double yaw = rand_float(v, f, 0x0004CA3C);
    const double scale = lfs(v.r32(k_rot_rand)), range = lfs(v.r32(k_rot_rand + 4));
    const double pitch = fmuls(fmuls(rand_float(v, f, 0x0004CA6C), scale), range);
    uint32_t roll = 0;
    if (v.r32(f + 0x70)) {
        v.w64(f + 0x78, (uint64_t)(int64_t)(int32_t)fctiwz(fmuls(fmuls(rand_float(v, f, 0x0004CA9C), scale), range)));
        roll = v.r32(f + 0x7C);
    }
    v.w32(result, fctiwz(pitch));
    v.w32(result + 4, fctiwz(fmuls(fmuls(yaw, scale), range)));
    v.w32(result + 8, roll);
}

double rand_unit(Vm& v, uint32_t at, uint32_t bits, double scale, double one)
{
    v.w32(at, bits);
    const double x = lfs(v.r32(at));
    return fmsubs(fsubs(x, single((double)fctidz(x))), scale, one);
}

void vrand(Vm& v, uint32_t sp0, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x40;
    finish(v, stack);
    const uint32_t mul = v.r32(k_vrand), one = v.r32(k_vrand + 4), two = v.r32(k_vrand + 8), add = v.r32(k_vrand + 0xC);
    const uint32_t fill = v.r32(k_vrand + 0x10), exp = fill << 23 | fill >> 9, mask = v.r32(k_vrand + 0x14);
    const double lo = lfs(one), scale = lfs(two);
    uint32_t s = v.r32(k_rand_seed), a, b;
    double len;
    do {
        a = s * mul + add;
        b = a * mul + add;
        s = b * mul + add;
        const double x = rand_unit(v, f + 0x38, (a & ~mask) | (exp & mask), scale, lo);
        const double y = rand_unit(v, f + 0x34, (b & ~mask) | (exp & mask), scale, lo);
        const double z = rand_unit(v, f + 0x30, (s & ~mask) | (exp & mask), scale, lo);
        len = fmadds(z, z, fmadds(x, x, fmuls(y, y)));
    } while (len > lo);
    v.w32(k_rand_seed, s);
    const uint32_t keep = v.r32(k_vrand + 0x18), half = v.r32(k_vrand + 0x1C), n = k_neg_zero;
    const auto unit = [&](uint32_t r) {
        const uint32_t u = (r & keep) | one;
        return vbin(v, vmadd(v, vbin(v, u, vcfsx(vcts(v, u)), true), two, n), one, true);
    };
    const uint32_t x = unit(a), y = unit(b), z = unit(s);
    const uint32_t q = vmadd(v, z, z, vmadd(v, x, x, vmadd(v, y, y, n)));
    uint32_t r = vrsqrte(v, q);
    for (int i = 0; i < 3; i++) r = vmadd(v, vnmsub(v, vmadd(v, q, r, n), r, one), vmadd(v, half, r, n), r);
    const uint32_t out = result & ~3u;
    v.w32(out, vmadd(v, x, r, n));
    v.w32(out + 4, vmadd(v, y, r, n));
    v.w32(out + 8, vmadd(v, z, r, n));
}

constexpr Exec VectorDivideBy = vector_scale_by<0xB0, 0x0005023C, 0x0005026C, 0x000502E0, 0x0005030C>;
constexpr Exec VectorMultiplyBy = vector_scale_by<0xA0, 0x000505EC, 0x0005061C, 0x00050684, 0>;
constexpr Exec RotatorAddTo = vector_assign<0x0004D74C, 0x0004D77C, 0x0004D7DC, iadd>;
constexpr Exec RotatorScale = vector_op<0x0004E430, 0x0004E48C, 0x74, 0x70, rscale>;
constexpr Exec FloatLerp = ternary<0x0005B7B0, 0x0005B80C, 0x0005B868, flerp>;

uint32_t sraw(uint32_t x, uint32_t n)
{
    return (uint32_t)((int32_t)x >> ((n & 0x20) ? 31 : (n & 0x1F)));
}

uint32_t lvlx(Vm& v, uint32_t ea, uint32_t i)
{
    const uint32_t at = ea + 4 * i, off = (ea & 15) + 4 * i;
    if (off + 4 <= 16) return v.r32(at);
    uint32_t w = 0;
    for (uint32_t k = 0; k < 4; k++) w = w << 8 | (off + k < 16 ? v.r8(at + k) : 0);
    return w;
}

void rand_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    arg0(v, f, 0x00058108, stack, f + 0x70);
    finish(v, stack);
    const uint32_t max = v.r32(f + 0x70);
    uint32_t r = 0;
    if ((int32_t)max > 0) {
        const uint32_t rnd = v.call(f, 0x00058128, func_00F2C010);
        v.w64(f + 0x78, (uint64_t)(int64_t)(int32_t)rnd);
        const double scale = lfs(v.r32(0x00058080));
        v.w64(f + 0x78, (uint64_t)(int64_t)(int32_t)max);
        const double x = fmuls(fmuls(single((double)(int32_t)rnd), scale), single((double)(int32_t)max));
        r = fctiwz(x);
        v.w64(f + 0x78, (uint64_t)(int64_t)(int32_t)r);
    }
    v.w32(result, r);
}

void frand_op(Vm& v, uint32_t, uint64_t, uint32_t, uint32_t stack, uint32_t result)
{
    constexpr uint32_t k = 0x0005B224, seed = 0x019FED20;
    const uint32_t s = v.r32(seed) * v.r32(k) + v.r32(k + 4);
    v.w32(seed, s);
    const uint32_t c = v.r32(k + 8), mask = v.r32(k + 0xC);
    finish(v, stack);
    const uint32_t x = (s & ~mask) | (((c << 23) | (c >> 9)) & mask);
    v.w32(result & ~3u, vbin(v, x, vcfsx(vcts(v, x)), true));
}

template <uint64_t lr_a, uint64_t lr_fn, void (*fn)(ppu_context*)>
void libm(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    finish(v, stack);
    v.ctx->fpr[1] = lfs(v.r32(f + 0x70));
    v.call(f, lr_fn, fn);
    v.w32(result, stfs(v.ctx->fpr[1]));
}

void round_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x0005AF78, stack, f + 0x70);
    finish(v, stack);
    v.ctx->fpr[1] = lfs(v.r32(f + 0x70));
    v.call(f, 0x0005AF8C, func_00F3E528);
    v.w32(result, fctiwz(v.ctx->fpr[1]));
}

void pow_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x000588CC, stack, f + 0x70);
    arg0(v, f, 0x00058928, stack, f + 0x74);
    finish(v, stack);
    v.ctx->fpr[1] = lfs(v.r32(f + 0x70));
    v.ctx->fpr[2] = lfs(v.r32(f + 0x74));
    v.call(f, 0x00058940, func_00F3FDBC);
    v.w32(result, stfs(v.ctx->fpr[1]));
}

void exp_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    constexpr uint32_t k = 0x0005A808;
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    arg0(v, f, 0x0005A8C0, stack, f + 0x70);
    finish(v, stack);
    const uint32_t x = lvlx(v, f + 0x70, 0), c0 = v.r32(k);
    uint32_t lane[4];
    for (uint32_t i = 0; i < 4; i++) lane[i] = vmadd(v, i ? lvlx(v, f + 0x70, i) : x, c0, 0x80000000u);
    const uint32_t t = lane[0];
    v.w32(f + 0x74, t);
    const uint32_t m = ~sraw(t, v.r32(k + 4) & 31) & v.r32(k + 8);
    for (uint32_t i = 0; i < 4; i++) lane[i] = vbin(v, lane[i], m, false);
    v.w32(f + 0x74, m);
    const uint32_t n = fctiwz(fmadds(lfs(x), lfs(c0), lfs(m)));
    const uint32_t i0 = vcts(v, lane[0]);
    for (uint32_t i = 1; i < 4; i++) vcts(v, lane[i]);
    v.w64(f + 0x80, (uint64_t)(int64_t)(int32_t)n);
    const uint32_t j = (i0 + v.r32(k + 0x10)) * v.r32(k + 0x14);
    const uint32_t e = vmadd(v, vbin(v, vcfsx(i0), t, true), v.r32(k + 0xC), 0x80000000u);
    const uint32_t e2 = vmadd(v, e, e, 0x80000000u);
    v.w32(f + 0x78, j);
    const uint32_t p1 = vmadd(v, e, v.r32(k + 0x18), v.r32(k + 0x1C));
    const uint32_t e4 = vmadd(v, e2, e2, 0x80000000u);
    const uint32_t p2 = vmadd(v, e, v.r32(k + 0x2C), v.r32(k + 0x30));
    const uint32_t q1 = vnmsub(v, e, p1, v.r32(k + 0x20));
    const uint32_t q2 = vnmsub(v, e, p2, v.r32(k + 0x34));
    const uint32_t sign = v.r32(k + 0x24);
    const uint32_t s1 = vmadd(v, e, q1 ^ sign, v.r32(k + 0x28));
    const uint32_t s2 = vmadd(v, e, q2 ^ sign, v.r32(k + 0x34));
    const uint32_t s = vmadd(v, e4, s1, s2);
    v.w32(f + 0x7C, s);
    const uint32_t scaled = n + ((s >> 23) & 0x1FF);
    double r = fmuls(lfs(s), lfs(j));
    if (scaled > 0xFF) {
        const uint32_t bits = (int32_t)scaled < 0 ? 0 : 0x7F800000u;
        v.w32(f + 0x7C, bits);
        r = lfs(bits);
    }
    v.w32(result, stfs(r));
}

void loge_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    constexpr uint32_t k = 0x0005AAC8;
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x0005AB80, stack, f + 0x70);
    finish(v, stack);
    const uint32_t x = lvlx(v, f + 0x70, 0);
    const uint32_t b = (((x << 9) | (x >> 23)) & v.r32(k)) - v.r32(k + 4);
    const uint32_t y = x - b * v.r32(k + 8);
    const uint32_t z = vbin(v, y, v.r32(k + 0xC), true);
    const uint32_t p = vmadd(v, z, v.r32(k + 0x10), v.r32(k + 0x14));
    const uint32_t z2 = vmadd(v, z, z, 0x80000000u);
    const uint32_t q1 = vnmsub(v, p, z, v.r32(k + 0x18));
    const uint32_t q2 = vnmsub(v, z, v.r32(k + 0x28), v.r32(k + 0x2C));
    const uint32_t z4 = vmadd(v, z2, z2, 0x80000000u);
    const uint32_t sign = v.r32(k + 0x1C);
    const uint32_t r1 = vmadd(v, q1 ^ sign, z, v.r32(k + 0x20));
    const uint32_t r2 = vmadd(v, q2 ^ sign, z, v.r32(k + 0x30));
    const uint32_t r3 = vnmsub(v, r1, z, v.r32(k + 0x24));
    const uint32_t r4 = vmadd(v, r2, z, 0x80000000u);
    const uint32_t s = vbin(v, vmadd(v, r3 ^ sign, z4, r4), vcfsx(b), false);
    v.w32(result & ~3u, vmadd(v, s, v.r32(k + 0x34), 0x80000000u));
}

void sqrt_op(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    const double zero = lfs(v.r32(k_float_zero));
    arg0(v, f, 0x0005ADA8, stack, f + 0x70);
    finish(v, stack);
    const double x = lfs(v.r32(f + 0x70));
    double r = zero;
    if (!(x < zero || x == zero)) r = single(std::sqrt(x));
    else if (!(x > zero || x == zero)) {
        v.call(f, 0x0005AE28, func_000AA46C, stack, 0x301, 0x0160B248);
    }
    v.w32(result, stfs(r));
}

template <uint64_t lr_a, uint64_t lr_b, uint64_t lr_log, uint32_t message, uint32_t (*op)(uint32_t, uint32_t)>
void divide_int(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, lr_a, stack, f + 0x70);
    arg0(v, f, lr_b, stack, f + 0x74);
    finish(v, stack);
    if (!v.r32(f + 0x74)) {
        v.call(f, lr_log, func_000AA46C, stack, 0x301, message);
    }
    const uint32_t b = v.r32(f + 0x74);
    v.w32(result, b ? op(v.r32(f + 0x70), b) : 0);
}

uint32_t idiv(uint32_t a, uint32_t b) { return a == 0x80000000u && b == ~0u ? 0 : (uint32_t)((int32_t)a / (int32_t)b); }
uint32_t irem(uint32_t a, uint32_t b) { return a - idiv(a, b) * b; }
uint32_t ishl(uint32_t a, uint32_t b) { return (b & 0x20) ? 0 : a << (b & 0x1F); }
uint32_t ishr(uint32_t a, uint32_t b) { return (b & 0x20) ? 0 : a >> (b & 0x1F); }
uint32_t isar(uint32_t a, uint32_t b) { return sraw(a, b & 0x3F); }
uint32_t inot(uint32_t a) { return ~a; }
uint32_t fsquare_bits(uint32_t a) { return stfs(fmuls(lfs(a), lfs(a))); }

void float_near(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x0005995C, stack, f + 0x70);
    arg0(v, f, 0x000599B8, stack, f + 0x74);
    const double d = std::fabs(fsubs(lfs(v.r32(f + 0x70)), lfs(v.r32(f + 0x74))));
    finish(v, stack);
    const double eps = from_bits64(v.r64(0x000598D0));
    v.w32(result, !(d > eps || d == eps));
}

void percent_float(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const double zero = lfs(v.r32(k_float_zero));
    arg0(v, f, 0x00058CF4, stack, f + 0x70);
    arg0(v, f, 0x00058D4C, stack, f + 0x74);
    finish(v, stack);
    double b = lfs(v.r32(f + 0x74));
    if (b == zero) {
        v.call(f, 0x00058D78, func_000AA46C, stack, 0x301, 0x0160B21C);
        b = lfs(v.r32(f + 0x74));
    }
    double r = zero;
    if (!(b == zero)) {
        const double m = std::fabs(b);
        const double q = fdivs(lfs(v.r32(f + 0x70)), m);
        const uint32_t bits = stfs(q);
        v.w32(f + 0x78, bits);
        const uint32_t s = 0x7F - ((bits >> 23) & 0xFF);
        const uint32_t keep = (int32_t)s > -31 ? sraw(0x7FFFFF, (0 - s) & 0x3F) : 0;
        const uint32_t sign = (uint32_t)((int32_t)(s - 1) >> 31);
        const uint32_t t = bits & ~(keep | ~sign);
        v.w32(f + 0x78, t);
        r = fmuls(m, fsubs(q, lfs(t)));
    }
    v.w32(result, stfs(r));
}

void divide_float_to(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const double zero = lfs(v.r32(k_float_zero));
    v.w32(f + 0x70, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, 0x00059C8C, 0x00059CBC, stack, f + 0x70);
    arg0(v, f, 0x00059D24, stack, f + 0x74);
    finish(v, stack);
    if (lfs(v.r32(f + 0x74)) == zero) {
        v.call(f, 0x00059D50, func_000AA46C, stack, 0x301, k_divide_zero);
    }
    const uint32_t r = stfs(fdivs(lfs(v.r32(at)), lfs(v.r32(f + 0x74))));
    v.w32(at, r);
    v.w32(result, r);
}

uint32_t int_float_lvalue(Vm& v, uint32_t f, uint32_t stack, uint64_t lr_a, uint64_t lr_dirty, uint64_t lr_b)
{
    v.w32(f + 0x70, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x70);
    arg0(v, f, lr_b, stack, f + 0x74);
    finish(v, stack);
    return at;
}

void int_mul_float(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const uint32_t at = int_float_lvalue(v, f, stack, 0x00057550, 0x00057580, 0x000575E8);
    uint32_t a[4], r = 0;
    for (uint32_t i = 0; i < 4; i++) a[i] = v.r32(at + 4 * i);
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t x = vcts(v, vmadd(v, vcfsx(a[i]), lvlx(v, f + 0x74, i), 0x80000000u));
        if (!i) r = x;
    }
    v.w32(at & ~3u, r);
    v.w32(result & ~3u, r);
}

void int_div_float(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const uint32_t at = int_float_lvalue(v, f, stack, 0x00057714, 0x00057744, 0x000577AC);
    const uint32_t b = lvlx(v, f + 0x74, 0);
    uint32_t r = 0;
    if (!(from_bits(b) == 0.0f)) {
        const uint32_t two = v.r32(0x0004DB24);
        uint32_t y = vrefp(v, b);
        const uint32_t a = vcfsx(v.r32(at));
        for (uint32_t i = 0; i < 3; i++) y = vmadd(v, y, vnmsub(v, y, b, two), 0x80000000u);
        r = vcts(v, vmadd(v, y, a, 0x80000000u));
    }
    v.w32(at & ~3u, r);
    v.w32(result & ~3u, r);
}

template <uint64_t lr_a, uint64_t lr_dirty, uint8_t delta, bool post>
void byte_crement(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    v.w8(f + 0x70, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x70);
    finish(v, stack);
    const uint8_t old = v.r8(at);
    v.w8(at, (uint8_t)(old + delta));
    v.w8(result, post ? old : (uint8_t)(old + delta));
}

uint32_t byte_lvalue(Vm& v, uint32_t f, uint32_t stack, uint64_t lr_a, uint64_t lr_dirty, uint64_t lr_b)
{
    v.w8(f + 0x70, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, lr_a, lr_dirty, stack, f + 0x70);
    v.w8(f + 0x71, 0);
    arg(v, f, lr_b, stack, f + 0x71);
    finish(v, stack);
    return at;
}

template <uint64_t lr_a, uint64_t lr_dirty, uint64_t lr_b, uint32_t (*op)(uint32_t, uint32_t)>
void byte_compound(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const uint32_t at = byte_lvalue(v, f, stack, lr_a, lr_dirty, lr_b);
    const uint8_t value = (uint8_t)op(v.r8(at), v.r8(f + 0x71));
    v.w8(at, value);
    v.w8(result, value);
}

void byte_divide(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    const uint32_t at = byte_lvalue(v, f, stack, 0x000553C4, 0x000553F4, 0x0005545C);
    const uint8_t b = v.r8(f + 0x71);
    uint8_t q = 0;
    if (b) {
        q = (uint8_t)(v.r8(at) / b);
        v.w8(at, q);
    }
    v.w8(result, q);
}

void byte_mul_float(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w8(f + 0x74, 0);
    v.w32(k_prop_addr, 0);
    const uint32_t at = lvalue(v, f, 0x00055208, 0x00055238, stack, f + 0x74);
    arg0(v, f, 0x000552A0, stack, f + 0x70);
    finish(v, stack);
    uint32_t a[4], r = 0;
    for (uint32_t i = 0; i < 4; i++) a[i] = lvlx(v, at, i) >> 24;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t x = vcts(v, vmadd(v, to_bits((float)a[i]), lvlx(v, f + 0x70, i), 0x80000000u));
        if (!i) r = x;
    }
    v.w8(at, (uint8_t)r);
    v.w8(result, (uint8_t)r);
}

void log_internal(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x78, 0);
    v.w32(f + 0x7C, 0);
    v.w32(f + 0x80, 0);
    arg(v, f, 0x0006536C, stack, f + 0x78);
    const uint32_t flags = v.r32(k_runtime_flags);
    v.w32(f + 0x70, 0x302);
    v.w32(f + 0x74, 0);
    v.w32(k_runtime_flags, flags & ~2u);
    arg(v, f, 0x000653D8, stack, f + 0x70);
    finish(v, stack);
    drop(v, f, f + 0x78, 0x00065404);
}

void warn_internal(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    v.w32(f + 0x78, 0);
    arg(v, f, 0x000654CC, stack, f + 0x70);
    finish(v, stack);
    v.call(f, 0x0006550C, func_0009DB34, stack, 0x0160B020, text(v, f + 0x70));
    drop(v, f, f + 0x70, 0x0006552C);
}

void state_warn(Vm& v, uint32_t f, uint32_t stack, uint32_t format, uint32_t a, uint32_t b, uint64_t lr_a, uint64_t lr_b, uint64_t lr_log,
                uint64_t lr_release, uint64_t lr_discard)
{
    v.call(f, lr_a, func_000AEB38, a, f + 0x70);
    const uint32_t state = text(v, a);
    v.call(f, lr_b, func_000AEB38, b, f + 0x78);
    v.call(f, lr_log, func_0009DB34, stack, format, state, text(v, b));
    release(v, f, b, lr_release);
    drop(v, f, a, lr_discard);
}

void goto_state(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0x100;
    v.frame(sp0, 0x100, lr0);
    uint32_t lo = 0, hi = 0;
    if (const uint32_t state = v.r32(self + 0x14)) {
        const uint32_t node = v.r32(state + 0x28);
        if (node != v.r32(self + 0x34)) {
            if ((int32_t)v.r32(node + 0x20) == -1) {
                v.call(f, 0x00066628, func_000A4148, f + 0x88, k_none_text, 1, 1);
                lo = v.r32(f + 0x88);
                hi = v.r32(f + 0x8C);
            } else {
                lo = v.r32(node + 0x2C);
                v.w32(f + 0x88, lo);
                hi = v.r32(node + 0x30);
                v.w32(f + 0x8C, hi);
            }
        }
    }
    uint32_t flags = v.r32(k_runtime_flags);
    v.w32(f + 0x70, lo);
    v.w32(f + 0x74, hi);
    v.w32(k_runtime_flags, flags & ~2u);
    arg(v, f, 0x000663F0, stack, f + 0x70);
    flags = v.r32(k_runtime_flags);
    v.w32(f + 0x78, 0);
    v.w32(f + 0x7C, 0);
    v.w32(k_runtime_flags, flags & ~2u);
    arg(v, f, 0x00066460, stack, f + 0x78);
    flags = v.r32(k_runtime_flags);
    v.w32(f + 0x80, 0);
    v.w32(k_runtime_flags, flags & ~2u);
    arg(v, f, 0x000664CC, stack, f + 0x80);
    const uint32_t force = v.r32(f + 0x80) != 0;
    flags = v.r32(k_runtime_flags);
    v.w32(f + 0x84, 0);
    v.w32(f + 0x80, force);
    v.w32(k_runtime_flags, flags & ~2u);
    arg(v, f, 0x00066544, stack, f + 0x84);
    const uint32_t keep = v.r32(f + 0x84) != 0;
    const bool same = v.r64(f + 0x70) == ((uint64_t)lo << 32 | hi);
    finish(v, stack);
    const uint32_t forced = v.r32(f + 0x80);
    v.w32(f + 0x84, keep);
    if (!same || forced) {
        v.w32(f + 0x38, v.r32(f + 0x70));
        v.w32(f + 0x3C, v.r32(f + 0x74));
        const uint32_t r = v.vcall(f, 0x00066780, self, 0x118, v.r64(f + 0x38), forced, keep);
        if (r != 1) {
            const uint64_t name = v.r64(f + 0x70);
            if (r || !name || name == v.r64(0x00066300)) return;
            return state_warn(v, f, stack, 0x0160B49C, f + 0xA8, f + 0xB4, 0x000667C8, 0x000667F0, 0x00066820, 0x00066840, 0x00066864);
        }
    }
    const bool label = v.r64(f + 0x78) != 0;
    v.w32(f + 0x38, label ? v.r32(f + 0x78) : 0x64);
    v.w32(f + 0x3C, label ? v.r32(f + 0x7C) : 0);
    if (v.vcall(f, 0x000665C4, self, 0x114, v.r64(f + 0x38)) || !v.r64(f + 0x78)) return;
    state_warn(v, f, stack, 0x0160B454, f + 0x90, f + 0x9C, 0x000666B4, 0x000666DC, 0x0006670C, 0x0006672C, 0x00066750);
}

void is_in_state(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x74, 0);
    v.w32(f + 0x78, 0);
    arg(v, f, 0x000687A8, stack, f + 0x74);
    const uint32_t flags = v.r32(k_runtime_flags);
    v.w32(f + 0x70, 0);
    v.w32(k_runtime_flags, flags & ~2u);
    arg(v, f, 0x00068814, stack, f + 0x70);
    v.w32(f + 0x38, v.r32(f + 0x74));
    const uint32_t test = v.r32(f + 0x70) != 0;
    v.w32(f + 0x3C, v.r32(f + 0x78));
    finish(v, stack);
    v.w32(f + 0x70, test);
    v.w32(result, v.call(f, 0x00068850, func_0003FF2C, self, v.r64(f + 0x38), test));
}

void get_state_name(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x80;
    v.frame(sp0, 0x80, lr0);
    finish(v, stack);
    uint32_t lo = 0, hi = 0;
    if (const uint32_t state = v.r32(self + 0x14))
        if (const uint32_t node = v.r32(state + 0x28)) {
            if ((int32_t)v.r32(node + 0x20) == -1) {
                v.call(f, 0x00068AF8, func_000A4148, f + 0x70, k_none_text, 1, 1);
                lo = v.r32(f + 0x70);
                hi = v.r32(f + 0x74);
            } else {
                lo = v.r32(node + 0x2C);
                v.w32(f + 0x70, lo);
                hi = v.r32(node + 0x30);
            }
        }
    v.w32(result, lo);
    v.w32(result + 4, hi);
}

void is_a(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    arg(v, f, 0x00066B70, stack, f + 0x70);
    finish(v, stack);
    uint32_t c = v.r32(self + 0x34);
    for (; c; c = v.r32(c + 0x40)) {
        uint32_t lo, hi;
        if ((int32_t)v.r32(c + 0x20) == -1) {
            v.call(f, 0x00066BB0, func_000A4148, f + 0x78, k_none_text, 1, 1);
            lo = v.r32(f + 0x78);
            hi = v.r32(f + 0x7C);
        } else {
            lo = v.r32(c + 0x2C);
            v.w32(f + 0x78, lo);
            hi = v.r32(c + 0x30);
            v.w32(f + 0x7C, hi);
        }
        if (((uint64_t)lo << 32 | hi) == v.r64(f + 0x70)) break;
    }
    v.w32(result, c != 0);
}

void class_is_child_of(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t, uint32_t stack, uint32_t result)
{
    const uint32_t f = sp0 - 0x90;
    v.frame(sp0, 0x90, lr0);
    arg0(v, f, 0x00065DE4, stack, f + 0x70);
    arg0(v, f, 0x00065E38, stack, f + 0x74);
    finish(v, stack);
    const uint32_t parent = v.r32(f + 0x74);
    v.w32(result, parent && child_of(v, v.r32(f + 0x70), parent));
}

uint32_t probe_state(Vm& v, uint32_t self, uint32_t f)
{
    const int32_t n = (int32_t)v.r32(f + 0x70);
    return n >= 0x12C && n < 0x14C ? v.r32(self + 0x14) : 0;
}

uint32_t probe_bit(Vm& v, uint32_t f)
{
    return ishl(1, v.r32(f + 0x70) - 300);
}

void probe_warn(Vm& v, uint32_t f, uint32_t stack, uint32_t format, uint64_t lr_name, uint64_t lr_log, uint64_t lr_free)
{
    v.call(f, lr_name, func_000AEB38, f + 0x78, f + 0x70);
    v.call(f, lr_log, func_0009DB34, stack, format, text(v, f + 0x78));
    drop(v, f, f + 0x78, lr_free);
}

void enable_probe(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    arg(v, f, 0x00066CC0, stack, f + 0x70);
    if (const uint32_t state = probe_state(v, self, f)) {
        const uint32_t mask = v.r32(v.r32(state + 0x28) + 0x74) | v.r32(v.r32(self + 0x34) + 0x74);
        v.w32(state + 0x2C, v.r32(state + 0x2C) | (mask & probe_bit(v, f)));
    } else {
        probe_warn(v, f, stack, 0x0160B4E4, 0x00066CF4, 0x00066D20, 0x00066D40);
    }
    finish(v, stack);
}

void disable_probe(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    const uint32_t f = sp0 - 0xA0;
    v.frame(sp0, 0xA0, lr0);
    v.w32(f + 0x70, 0);
    v.w32(f + 0x74, 0);
    arg(v, f, 0x00066E40, stack, f + 0x70);
    finish(v, stack);
    if (const uint32_t state = probe_state(v, self, f)) v.w32(state + 0x2C, v.r32(state + 0x2C) & ~probe_bit(v, f));
    else probe_warn(v, f, stack, 0x0160B530, 0x00066ED0, 0x00066EFC, 0x00066F1C);
}

void save_config(Vm& v, uint32_t sp0, uint64_t lr0, uint32_t self, uint32_t stack, uint32_t)
{
    v.frame(sp0, 0x70, lr0);
    finish(v, stack);
    v.call(sp0 - 0x70, 0x00067350, func_000D82AC, self, 0x4000, 0);
}

struct Native {
    uint32_t code;
    Exec exec;
};

constexpr Exec IntAdd = binary<0x0005630C, 0x00056368, iadd>;
constexpr Exec IntPostInc = crement<0x00057EB8, 0x00057EE8, 1, true>;
constexpr Exec IntAddTo = compound<0x0005792C, 0x0005795C, 0x000579C4, iadd>;
constexpr Exec IntSubFrom = compound<0x00057AC8, 0x00057AF8, 0x00057B60, isub>;
constexpr Exec FloatMulBy = compound<0x00059AD8, 0x00059B08, 0x00059B70, fmul_bits>;
constexpr Exec FloatAddTo = compound<0x00059E4C, 0x00059E7C, 0x00059EE4, fadd_bits>;
constexpr Exec FloatSubFrom = compound<0x00059FE8, 0x0005A018, 0x0005A080, fsub_bits>;
constexpr Exec FloatDivide = divide_float<0x00058B74, 0x00058BCC, 0x00058BF8>;
constexpr Exec VectorScale = vector_op<0x00051908, 0x00051964, 0x74, 0x70, vscale>;

const Native k_table[] = {
    {0x000413E4, local_variable},
    {0x00041480, instance_variable},
    {0x00041A0C, local_out_variable},
    {0x0004654C, jump_if_not},
    {0x00046BE0, let},
    {0x0004701C, let_bool},
    {0x000475B8, context},
    {0x000479D8, virtual_function},
    {0x00047A90, final_function},
    {0x00047AC4, global_function},
    {0x00045C6C, bool_variable},
    {0x000488C4, struct_member},
    {0x00042288, dyn_element},
    {0x00042A40, dyn_length},
    {0x00049CBC, primitive_cast},
    {0x00049530, dynamic_cast_op},
    {0x00046464, case_op},
    {0x000691C4, extended<0x0, 0x0006922C>},
    {0x00069254, extended<0x1, 0x000692BC>},
    {0x000692E4, extended<0x2, 0x0006934C>},
    {0x00069374, extended<0x3, 0x000693DC>},
    {0x00069404, extended<0x4, 0x0006946C>},
    {0x00069494, extended<0x5, 0x000694FC>},
    {0x00069524, extended<0x6, 0x0006958C>},
    {0x000695B4, extended<0x7, 0x0006961C>},
    {0x00069644, extended<0x8, 0x000696AC>},
    {0x000696D4, extended<0x9, 0x0006973C>},
    {0x00069764, extended<0xA, 0x000697CC>},
    {0x000697F4, extended<0xB, 0x0006985C>},
    {0x00069884, extended<0xC, 0x000698EC>},
    {0x00069914, extended<0xD, 0x0006997C>},
    {0x000699A4, extended<0xE, 0x00069A0C>},
    {0x00069A34, extended<0xF, 0x00069A9C>},
    {0x00046530, jump},
    {0x00045FE4, empty_parm},
    {0x00022F00, nothing},
    {0x00045D70, end_parms},
    {0x00046014, stop},
    {0x000475B0, self_const},
    {0x00048FDC, int_const},
    {0x00048FF4, float_const},
    {0x000493C8, object_const},
    {0x0004944C, name_const},
    {0x0004AD08, rotation_const},
    {0x0004AD48, vector_const},
    {0x000494A4, byte_const},
    {0x0004950C, int_const_byte},
    {0x00F570A8, store_const<0>},
    {0x000494E0, store_const<0>},
    {0x000494C8, store_const<1>},
    {0x00055E50, binary<0x00055ED4, 0x00055F30, imul>},
    {0x00056288, sited<func_00056288, IntAdd>},
    {0x000563D0, binary<0x00056454, 0x000564B0, isub>},
    {0x000568F0, binary<0x00056974, 0x000569D0, ilt>},
    {0x00056A44, binary<0x00056AC8, 0x00056B24, igt>},
    {0x00056B98, binary<0x00056C1C, 0x00056C78, ile>},
    {0x00056CEC, binary<0x00056D70, 0x00056DCC, ige>},
    {0x00056E40, binary<0x00056EC4, 0x00056F20, ieq>},
    {0x00056F90, binary<0x00057014, 0x00057070, ine>},
    {0x000570E4, binary<0x00057168, 0x000571C4, iand>},
    {0x0005722C, binary<0x000572B0, 0x0005730C, ixor>},
    {0x00057374, binary<0x000573F8, 0x00057454, ior>},
    {0x00055D88, unary<0x00055E08, ineg>},
    {0x00057BD0, crement<0x00057C60, 0x00057C90, 1, false>},
    {0x00057CFC, crement<0x00057D8C, 0x00057DBC, ~0u, false>},
    {0x00057E28, sited<func_00057E28, IntPostInc>},
    {0x00057F54, crement<0x00057FE4, 0x00058014, ~0u, true>},
    {0x00057898, sited<func_00057898, IntAddTo>},
    {0x00057A34, sited<func_00057A34, IntSubFrom>},
    {0x000581A4, binary<0x00058228, 0x0005827C, imin>},
    {0x000582F0, binary<0x00058374, 0x000583C8, imax>},
    {0x0005843C, ternary<0x000584BC, 0x00058518, 0x0005856C, iclamp>},
    {0x00054748, unary<0x000547C8, bnot>},
    {0x00054B1C, short_circuit<0x00054BA0, 0x00054C5C, false>},
    {0x00054E34, short_circuit<0x00054EB8, 0x00054F74, true>},
    {0x00054820, bool_compare<0x000548A4, 0x00054910, true>},
    {0x0005499C, bool_compare<0x00054A20, 0x00054A8C, false>},
    {0x00058990, binary<0x00058A14, 0x00058A70, fmul_bits>},
    {0x00058E3C, binary<0x00058EC0, 0x00058F1C, fadd_bits>},
    {0x00058F84, binary<0x00059008, 0x00059064, fsub_bits>},
    {0x00058780, unary<0x00058800, fneg>},
    {0x00058AE0, sited<func_00058AE0, FloatDivide>},
    {0x000590CC, binary<0x00059150, 0x000591AC, flt>},
    {0x00059224, binary<0x000592A8, 0x00059304, fgt>},
    {0x0005937C, binary<0x00059400, 0x0005945C, fle>},
    {0x000594D0, binary<0x00059554, 0x000595B0, fge>},
    {0x00059624, binary<0x000596A8, 0x00059704, feq>},
    {0x00059778, binary<0x000597FC, 0x00059858, fne>},
    {0x00059A44, sited<func_00059A44, FloatMulBy>},
    {0x00059DB8, sited<func_00059DB8, FloatAddTo>},
    {0x00059F54, sited<func_00059F54, FloatSubFrom>},
    {0x0005A0F0, unary<0x0005A170, fabs_bits>},
    {0x0005B2D0, binary<0x0005B354, 0x0005B3B0, fmin_bits>},
    {0x0005B41C, binary<0x0005B4A0, 0x0005B4FC, fmax_bits>},
    {0x0005B568, ternary<0x0005B5E8, 0x0005B644, 0x0005B6A0, fclamp_bits>},
    {0x00065030, name_compare<0x000650B8, 0x00065118, true>},
    {0x0006518C, name_compare<0x00065214, 0x00065274, false>},
    {0x000512E0, vector_op<0x0005135C, 0x000513B0, 0x70, 0x7C, vadd>},
    {0x00051188, vector_op<0x00051204, 0x00051258, 0x70, 0x7C, vsub>},
    {0x00051734, vector_op<0x000517B8, 0x0005180C, 0x70, 0x74, vscale>},
    {0x0005188C, sited<func_0005188C, VectorScale>},
    {0x000519E4, negate_vector},
    {0x00050878, vector_op<0x000508F4, 0x00050948, 0x70, 0x7C, vdot>},
    {0x00050B30, vector_op<0x00050BAC, 0x00050C00, 0x70, 0x7C, vequal>},
    {0x000509C8, vector_op<0x00050A44, 0x00050A98, 0x70, 0x7C, vunequal>},
    {0x00051440, divide_vector},
    {0x0004FFE4, sited<func_0004FFE4, add_vector_to>},
    {0x00045D90, end_of_script},
    {0x00049034, string_const},
    {0x00049258, unicode_string_const},
    {0x00045F30, iterator_pop},
    {0x00045E8C, return_nothing},
    {0x00067244, native_parm},
    {0x00047454, eat_return_value},
    {0x00041918, default_variable},
    {0x00041ACC, state_variable},
    {0x00041C2C, default_parm_value},
    {0x00065558, conditional},
    {0x00041B6C, interface_context},
    {0x00049D54, interface_cast},
    {0x000494F8, empty_delegate},
    {0x00046A84, goto_label},
    {0x000493E0, instance_delegate},
    {0x000472F8, let_delegate},
    {0x00047F40, delegate_property},
    {0x000413AC, undefined_op},
    {0x00041D54, array_element},
    {0x00046020, switch_op},
    {0x00046618, assert_op},
    {0x00065EDC, class_context},
    {0x00049868, meta_cast},
    {0x000656D0, new_op},
    {0x00047B7C, delegate_function},
    {0x000441AC, dyn_add},
    {0x00042B30, dyn_insert},
    {0x000431FC, dyn_remove},
    {0x000445E0, dyn_add_item},
    {0x00044CBC, dyn_remove_item},
    {0x000448F8, dyn_insert_item},
    {0x00045064, dyn_iterator},
    {0x00043A80, dyn_find},
    {0x00043D8C, dyn_find_struct},
    {0x0004551C, dyn_sort},
    {0x0004809C, struct_compare<k_struct_eq, true>},
    {0x000484AC, struct_compare<k_struct_ne, false>},
    {0x00048C4C, delegate_compare<0x00048CE0, 0x00048D3C, true>},
    {0x00048E10, delegate_compare<0x00048EA4, 0x00048F00, false>},
    {0x0005C29C, str_concat},
    {0x0005C5D4, str_at},
    {0x0005D130, StrEqual},
    {0x0005D30C, StrNotEqual},
    {0x0005D4E8, StrComplementEqual},
    {0x0005C9C0, StrLess},
    {0x0005CB9C, StrGreater},
    {0x0005CD78, StrLessEqual},
    {0x0005CF54, StrGreaterEqual},
    {0x0005D6C4, str_concat_to},
    {0x0005D994, str_at_to},
    {0x0005DD18, str_subtract_from},
    {0x0005E34C, str_length},
    {0x0005E448, str_instr},
    {0x0005E7D0, str_mid},
    {0x0005EAF4, str_left_op},
    {0x0005ED64, str_right},
    {0x0005F000, StrCaps},
    {0x0005F27C, StrLocs},
    {0x0005F4FC, str_chr},
    {0x0005F64C, str_asc},
    {0x0005F754, str_repl},
    {0x000538F0, InterfaceToObject},
    {0x00053584, InterfaceToString},
    {0x00053814, InterfaceToBool},
    {0x0004C718, RotatorToVector},
    {0x00049DCC, ByteToInt},
    {0x00049E80, ByteToBool},
    {0x00049F40, ByteToFloat},
    {0x0004A200, IntToByte},
    {0x0004A2B4, IntToBool},
    {0x0004A374, IntToFloat},
    {0x0004A5A0, BoolToByte},
    {0x0004A658, BoolToInt},
    {0x0004A710, BoolToFloat},
    {0x0004A938, FloatToByte},
    {0x0004A9F8, FloatToInt},
    {0x0004AAC0, FloatToBool},
    {0x00052FE8, object_to_interface},
    {0x00053EEC, NameToBool},
    {0x000541A0, StringToByte},
    {0x000542A0, StringToInt},
    {0x000543A0, StringToBool},
    {0x00054644, StringToFloat},
    {0x0004BF48, StringToVector},
    {0x0004C0F8, StringToRotator},
    {0x0004C2A0, VectorToBool},
    {0x0004C52C, VectorToRotator},
    {0x0004C60C, RotatorToBool},
    {0x0004A008, ByteToString},
    {0x0004A434, IntToString},
    {0x0004A7D8, BoolToString},
    {0x0004AB98, FloatToString},
    {0x000532F4, ObjectToString},
    {0x00053FC8, NameToString},
    {0x0004C3AC, VectorToString},
    {0x0004C7F8, RotatorToString},
    {0x000539A8, delegate_to_string},
    {0x0005408C, StringToName},
    {0x0004F998, vector_unary<0x0004FA10, vnormal>},
    {0x0004FAF4, vector_unary<0x0004FB6C, vsize>},
    {0x0004FC94, vector_unary<0x0004FD0C, vsize_sq>},
    {0x00050714, vector_op<0x00050790, 0x000507E4, 0x70, 0x7C, vcross>},
    {0x00050C94, unrotate},
    {0x00050EB8, vector_op<0x00050F34, 0x00050F88, 0x70, 0x7C, vrotate>},
    {0x000515DC, vector_op<0x00051658, 0x000516AC, 0x70, 0x7C, vmul>},
    {0x0004FE2C, vector_assign<0x0004FEBC, 0x0004FEEC, 0x0004FF4C, fsub_bits>},
    {0x000501A8, sited<func_000501A8, VectorDivideBy>},
    {0x0005055C, sited<func_0005055C, VectorMultiplyBy>},
    {0x0004D9CC, vector_op<0x0004DA48, 0x0004DA9C, 0x70, 0x7C, radd>},
    {0x0004D874, vector_op<0x0004D8F0, 0x0004D944, 0x70, 0x7C, rsub>},
    {0x0004D6BC, sited<func_0004D6BC, RotatorAddTo>},
    {0x0004E6C8, vector_op<0x0004E744, 0x0004E798, 0x70, 0x7C, requal>},
    {0x0004E560, vector_op<0x0004E5DC, 0x0004E630, 0x70, 0x7C, runequal>},
    {0x0004E3B4, sited<func_0004E3B4, RotatorScale>},
    {0x0005B730, sited<func_0005B730, FloatLerp>},
    {0x0004D14C, get_axes},
    {0x0004EBA8, vrand},
    {0x0004C978, rot_rand},
    {0x00069C40, vector_unary<0x00069CB8, vzero>},
    {0x00058088, rand_op},
    {0x0005B238, frand_op},
    {0x0005A1BC, libm<0x0005A23C, 0x0005A250, func_00F64730>},
    {0x0005A378, libm<0x0005A3F8, 0x0005A40C, func_00F64850>},
    {0x0005A530, libm<0x0005A5B0, 0x0005A5C4, func_00F3FC08>},
    {0x0005A5F8, libm<0x0005A678, 0x0005A68C, func_00F40130>},
    {0x0005A840, exp_op},
    {0x0005AB00, loge_op},
    {0x0005AD18, sqrt_op},
    {0x0005AE30, unary<0x0005AEB0, fsquare_bits>},
    {0x0005AEF8, round_op},
    {0x0005610C, divide_int<0x00056190, 0x000561EC, 0x00056218, 0x0160B21C, irem>},
    {0x00055F98, divide_int<0x0005601C, 0x00056078, 0x000560A4, k_divide_zero, idiv>},
    {0x00056518, binary<0x0005659C, 0x000565F8, ishl>},
    {0x00056660, binary<0x000566E4, 0x00056740, isar>},
    {0x000567A8, binary<0x0005682C, 0x00056888, ishr>},
    {0x00055CC0, unary<0x00055D40, inot>},
    {0x00058848, pow_op},
    {0x00059BE8, divide_float_to},
    {0x000574BC, int_mul_float},
    {0x00057680, int_div_float},
    {0x000598D8, float_near},
    {0x00058C60, percent_float},
    {0x00055A68, byte_crement<0x00055AF8, 0x00055B28, 1, true>},
    {0x00055B94, byte_crement<0x00055C24, 0x00055C54, 0xFF, true>},
    {0x00055810, byte_crement<0x000558A0, 0x000558D0, 1, false>},
    {0x0005593C, byte_crement<0x000559CC, 0x000559FC, 0xFF, false>},
    {0x000554D8, byte_compound<0x0005556C, 0x0005559C, 0x00055604, iadd>},
    {0x00055674, byte_compound<0x00055708, 0x00055738, 0x000557A0, isub>},
    {0x00054FD8, byte_compound<0x0005506C, 0x0005509C, 0x00055104, imul>},
    {0x00055330, byte_divide},
    {0x00055174, byte_mul_float},
    {0x00054CC0, bool_compare<0x00054D44, 0x00054DB0, false>},
    {0x000652E8, log_internal},
    {0x0006544C, warn_internal},
    {0x00066308, goto_state},
    {0x00068718, is_in_state},
    {0x00068A68, get_state_name},
    {0x00066AE4, is_a},
    {0x00065D60, class_is_child_of},
    {0x00066C34, enable_probe},
    {0x00066DB4, disable_probe},
    {0x0006732C, save_config},
};

std::atomic<uint64_t> g_slots[k_slots];
std::atomic<uint64_t> g_cast_slots[0x100];

uint32_t kind_of(uint32_t code)
{
    for (uint32_t i = 0; i < std::size(k_table); i++)
        if (k_table[i].code == code) return i + 1;
    return 0;
}

void dispatch(Vm& v, uint32_t sp, uint64_t lr, uint32_t self, uint32_t stack, uint32_t result, std::atomic<uint64_t>& slot, uint32_t fn)
{
    uint64_t s = slot.load(std::memory_order_relaxed);
    if ((uint32_t)(s >> 32) != fn || !(s & 1)) {
        s = (uint64_t)fn << 32 | kind_of(v.r32(fn)) << 1 | 1;
        slot.store(s, std::memory_order_relaxed);
    }
    const uint32_t kind = (uint32_t)s >> 1;
    if (!kind) {
        v.icall(sp, lr, v.r32(fn), self, stack, result);
        return;
    }
    k_table[kind - 1].exec(v, sp, lr, self, stack, result);
}

void call_index(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result, uint32_t index)
{
    const uint32_t entry = k_natives + 8 * index;
    uint32_t fn = v.r32(entry);
    const uint32_t self = obj + v.r32(entry + 4);
    if (fn & 1) fn = v.r32(v.r32(self) + fn - 1);
    dispatch(v, sp, lr, self, stack, result, g_slots[index], fn);
}

void call_cast(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result, uint32_t index)
{
    const uint32_t entry = k_casts + 8 * index;
    uint32_t fn = v.r32(entry);
    const uint32_t self = obj + v.r32(entry + 4);
    if (fn & 1) fn = v.r32(v.r32(self) + fn - 1);
    dispatch(v, sp, lr, self, stack, result, g_cast_slots[index], fn);
}

void step(Vm& v, uint32_t sp, uint64_t lr, uint32_t obj, uint32_t stack, uint32_t result, uint32_t op)
{
    call_index(v, sp, lr, obj, stack, result, op);
}

template <Exec exec>
void exec_entry(ppu_context* ctx)
{
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    Vm v(ctx);
    exec(v, (uint32_t)sp, lr, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void call_function_entry(ppu_context* ctx)
{
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    Vm v(ctx);
    call_function(v, (uint32_t)sp, lr, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void find_checked_entry(ppu_context* ctx)
{
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    Vm v(ctx);
    const uint32_t found = find_checked(v, (uint32_t)sp, lr, (uint32_t)ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = found;
}

}

void func_0004071C(ppu_context* ctx) { call_function_entry(ctx); }
void func_00040210(ppu_context* ctx) { exec_entry<process_internal>(ctx); }
void func_000D78A0(ppu_context* ctx) { find_checked_entry(ctx); }
void func_000413E4(ppu_context* ctx) { exec_entry<local_variable>(ctx); }
void func_00041480(ppu_context* ctx) { exec_entry<instance_variable>(ctx); }
void func_00041A0C(ppu_context* ctx) { exec_entry<local_out_variable>(ctx); }
void func_0004654C(ppu_context* ctx) { exec_entry<jump_if_not>(ctx); }
void func_00046BE0(ppu_context* ctx) { exec_entry<let>(ctx); }
void func_0004701C(ppu_context* ctx) { exec_entry<let_bool>(ctx); }
void func_000475B8(ppu_context* ctx) { exec_entry<context>(ctx); }
void func_000479D8(ppu_context* ctx) { exec_entry<virtual_function>(ctx); }
void func_00047A90(ppu_context* ctx) { exec_entry<final_function>(ctx); }
void func_00047AC4(ppu_context* ctx) { exec_entry<global_function>(ctx); }
void func_00045C6C(ppu_context* ctx) { exec_entry<bool_variable>(ctx); }
void func_000488C4(ppu_context* ctx) { exec_entry<struct_member>(ctx); }
void func_00042288(ppu_context* ctx) { exec_entry<dyn_element>(ctx); }
void func_00042A40(ppu_context* ctx) { exec_entry<dyn_length>(ctx); }
void func_00049CBC(ppu_context* ctx) { exec_entry<primitive_cast>(ctx); }
void func_00049530(ppu_context* ctx) { exec_entry<dynamic_cast_op>(ctx); }
void func_00046464(ppu_context* ctx) { exec_entry<case_op>(ctx); }
void func_000691C4(ppu_context* ctx) { exec_entry<extended<0x0, 0x0006922C>>(ctx); }
void func_00069254(ppu_context* ctx) { exec_entry<extended<0x1, 0x000692BC>>(ctx); }
void func_000692E4(ppu_context* ctx) { exec_entry<extended<0x2, 0x0006934C>>(ctx); }
void func_00069374(ppu_context* ctx) { exec_entry<extended<0x3, 0x000693DC>>(ctx); }
void func_00069404(ppu_context* ctx) { exec_entry<extended<0x4, 0x0006946C>>(ctx); }
void func_00069494(ppu_context* ctx) { exec_entry<extended<0x5, 0x000694FC>>(ctx); }
void func_00069524(ppu_context* ctx) { exec_entry<extended<0x6, 0x0006958C>>(ctx); }
void func_000695B4(ppu_context* ctx) { exec_entry<extended<0x7, 0x0006961C>>(ctx); }
void func_00069644(ppu_context* ctx) { exec_entry<extended<0x8, 0x000696AC>>(ctx); }
void func_000696D4(ppu_context* ctx) { exec_entry<extended<0x9, 0x0006973C>>(ctx); }
void func_00069764(ppu_context* ctx) { exec_entry<extended<0xA, 0x000697CC>>(ctx); }
void func_000697F4(ppu_context* ctx) { exec_entry<extended<0xB, 0x0006985C>>(ctx); }
void func_00069884(ppu_context* ctx) { exec_entry<extended<0xC, 0x000698EC>>(ctx); }
void func_00069914(ppu_context* ctx) { exec_entry<extended<0xD, 0x0006997C>>(ctx); }
void func_000699A4(ppu_context* ctx) { exec_entry<extended<0xE, 0x00069A0C>>(ctx); }
void func_00069A34(ppu_context* ctx) { exec_entry<extended<0xF, 0x00069A9C>>(ctx); }
void func_00055E50(ppu_context* ctx) { exec_entry<binary<0x00055ED4, 0x00055F30, imul>>(ctx); }
void func_000563D0(ppu_context* ctx) { exec_entry<binary<0x00056454, 0x000564B0, isub>>(ctx); }
void func_000568F0(ppu_context* ctx) { exec_entry<binary<0x00056974, 0x000569D0, ilt>>(ctx); }
void func_00056A44(ppu_context* ctx) { exec_entry<binary<0x00056AC8, 0x00056B24, igt>>(ctx); }
void func_00056B98(ppu_context* ctx) { exec_entry<binary<0x00056C1C, 0x00056C78, ile>>(ctx); }
void func_00056CEC(ppu_context* ctx) { exec_entry<binary<0x00056D70, 0x00056DCC, ige>>(ctx); }
void func_00056E40(ppu_context* ctx) { exec_entry<binary<0x00056EC4, 0x00056F20, ieq>>(ctx); }
void func_00056F90(ppu_context* ctx) { exec_entry<binary<0x00057014, 0x00057070, ine>>(ctx); }
void func_000570E4(ppu_context* ctx) { exec_entry<binary<0x00057168, 0x000571C4, iand>>(ctx); }
void func_0005722C(ppu_context* ctx) { exec_entry<binary<0x000572B0, 0x0005730C, ixor>>(ctx); }
void func_00057374(ppu_context* ctx) { exec_entry<binary<0x000573F8, 0x00057454, ior>>(ctx); }
void func_00055D88(ppu_context* ctx) { exec_entry<unary<0x00055E08, ineg>>(ctx); }
void func_00057BD0(ppu_context* ctx) { exec_entry<crement<0x00057C60, 0x00057C90, 1, false>>(ctx); }
void func_00057CFC(ppu_context* ctx) { exec_entry<crement<0x00057D8C, 0x00057DBC, ~0u, false>>(ctx); }
void func_00057F54(ppu_context* ctx) { exec_entry<crement<0x00057FE4, 0x00058014, ~0u, true>>(ctx); }
void func_000581A4(ppu_context* ctx) { exec_entry<binary<0x00058228, 0x0005827C, imin>>(ctx); }
void func_000582F0(ppu_context* ctx) { exec_entry<binary<0x00058374, 0x000583C8, imax>>(ctx); }
void func_0005843C(ppu_context* ctx) { exec_entry<ternary<0x000584BC, 0x00058518, 0x0005856C, iclamp>>(ctx); }
void func_00054748(ppu_context* ctx) { exec_entry<unary<0x000547C8, bnot>>(ctx); }
void func_00054B1C(ppu_context* ctx) { exec_entry<short_circuit<0x00054BA0, 0x00054C5C, false>>(ctx); }
void func_00054E34(ppu_context* ctx) { exec_entry<short_circuit<0x00054EB8, 0x00054F74, true>>(ctx); }
void func_00054820(ppu_context* ctx) { exec_entry<bool_compare<0x000548A4, 0x00054910, true>>(ctx); }
void func_0005499C(ppu_context* ctx) { exec_entry<bool_compare<0x00054A20, 0x00054A8C, false>>(ctx); }
void func_00058990(ppu_context* ctx) { exec_entry<binary<0x00058A14, 0x00058A70, fmul_bits>>(ctx); }
void func_00058E3C(ppu_context* ctx) { exec_entry<binary<0x00058EC0, 0x00058F1C, fadd_bits>>(ctx); }
void func_00058F84(ppu_context* ctx) { exec_entry<binary<0x00059008, 0x00059064, fsub_bits>>(ctx); }
void func_00058780(ppu_context* ctx) { exec_entry<unary<0x00058800, fneg>>(ctx); }
void func_000590CC(ppu_context* ctx) { exec_entry<binary<0x00059150, 0x000591AC, flt>>(ctx); }
void func_00059224(ppu_context* ctx) { exec_entry<binary<0x000592A8, 0x00059304, fgt>>(ctx); }
void func_0005937C(ppu_context* ctx) { exec_entry<binary<0x00059400, 0x0005945C, fle>>(ctx); }
void func_000594D0(ppu_context* ctx) { exec_entry<binary<0x00059554, 0x000595B0, fge>>(ctx); }
void func_00059624(ppu_context* ctx) { exec_entry<binary<0x000596A8, 0x00059704, feq>>(ctx); }
void func_00059778(ppu_context* ctx) { exec_entry<binary<0x000597FC, 0x00059858, fne>>(ctx); }
void func_0005A0F0(ppu_context* ctx) { exec_entry<unary<0x0005A170, fabs_bits>>(ctx); }
void func_0005B2D0(ppu_context* ctx) { exec_entry<binary<0x0005B354, 0x0005B3B0, fmin_bits>>(ctx); }
void func_0005B41C(ppu_context* ctx) { exec_entry<binary<0x0005B4A0, 0x0005B4FC, fmax_bits>>(ctx); }
void func_0005B568(ppu_context* ctx) { exec_entry<ternary<0x0005B5E8, 0x0005B644, 0x0005B6A0, fclamp_bits>>(ctx); }
void func_00065030(ppu_context* ctx) { exec_entry<name_compare<0x000650B8, 0x00065118, true>>(ctx); }
void func_0006518C(ppu_context* ctx) { exec_entry<name_compare<0x00065214, 0x00065274, false>>(ctx); }
void func_000512E0(ppu_context* ctx) { exec_entry<vector_op<0x0005135C, 0x000513B0, 0x70, 0x7C, vadd>>(ctx); }
void func_00051188(ppu_context* ctx) { exec_entry<vector_op<0x00051204, 0x00051258, 0x70, 0x7C, vsub>>(ctx); }
void func_00051734(ppu_context* ctx) { exec_entry<vector_op<0x000517B8, 0x0005180C, 0x70, 0x74, vscale>>(ctx); }
void func_000519E4(ppu_context* ctx) { exec_entry<negate_vector>(ctx); }
void func_00050878(ppu_context* ctx) { exec_entry<vector_op<0x000508F4, 0x00050948, 0x70, 0x7C, vdot>>(ctx); }
void func_00050B30(ppu_context* ctx) { exec_entry<vector_op<0x00050BAC, 0x00050C00, 0x70, 0x7C, vequal>>(ctx); }
void func_000509C8(ppu_context* ctx) { exec_entry<vector_op<0x00050A44, 0x00050A98, 0x70, 0x7C, vunequal>>(ctx); }
void func_00051440(ppu_context* ctx) { exec_entry<divide_vector>(ctx); }
void func_00045D90(ppu_context* ctx) { exec_entry<end_of_script>(ctx); }
void func_00049034(ppu_context* ctx) { exec_entry<string_const>(ctx); }
void func_00049258(ppu_context* ctx) { exec_entry<unicode_string_const>(ctx); }
void func_00045F30(ppu_context* ctx) { exec_entry<iterator_pop>(ctx); }
void func_00045E8C(ppu_context* ctx) { exec_entry<return_nothing>(ctx); }
void func_00067244(ppu_context* ctx) { exec_entry<native_parm>(ctx); }
void func_00047454(ppu_context* ctx) { exec_entry<eat_return_value>(ctx); }
void func_00041918(ppu_context* ctx) { exec_entry<default_variable>(ctx); }
void func_00041ACC(ppu_context* ctx) { exec_entry<state_variable>(ctx); }
void func_00041C2C(ppu_context* ctx) { exec_entry<default_parm_value>(ctx); }
void func_00065558(ppu_context* ctx) { exec_entry<conditional>(ctx); }
void func_00041B6C(ppu_context* ctx) { exec_entry<interface_context>(ctx); }
void func_00049D54(ppu_context* ctx) { exec_entry<interface_cast>(ctx); }
void func_00046A84(ppu_context* ctx) { exec_entry<goto_label>(ctx); }
void func_000472F8(ppu_context* ctx) { exec_entry<let_delegate>(ctx); }
void func_00047F40(ppu_context* ctx) { exec_entry<delegate_property>(ctx); }
void func_000413AC(ppu_context* ctx) { exec_entry<undefined_op>(ctx); }
void func_00041D54(ppu_context* ctx) { exec_entry<array_element>(ctx); }
void func_00046020(ppu_context* ctx) { exec_entry<switch_op>(ctx); }
void func_00046618(ppu_context* ctx) { exec_entry<assert_op>(ctx); }
void func_00065EDC(ppu_context* ctx) { exec_entry<class_context>(ctx); }
void func_00049868(ppu_context* ctx) { exec_entry<meta_cast>(ctx); }
void func_000656D0(ppu_context* ctx) { exec_entry<new_op>(ctx); }
void func_00047B7C(ppu_context* ctx) { exec_entry<delegate_function>(ctx); }
void func_000441AC(ppu_context* ctx) { exec_entry<dyn_add>(ctx); }
void func_00042B30(ppu_context* ctx) { exec_entry<dyn_insert>(ctx); }
void func_000431FC(ppu_context* ctx) { exec_entry<dyn_remove>(ctx); }
void func_000445E0(ppu_context* ctx) { exec_entry<dyn_add_item>(ctx); }
void func_00044CBC(ppu_context* ctx) { exec_entry<dyn_remove_item>(ctx); }
void func_000448F8(ppu_context* ctx) { exec_entry<dyn_insert_item>(ctx); }
void func_00045064(ppu_context* ctx) { exec_entry<dyn_iterator>(ctx); }
void func_00043A80(ppu_context* ctx) { exec_entry<dyn_find>(ctx); }
void func_00043D8C(ppu_context* ctx) { exec_entry<dyn_find_struct>(ctx); }
void func_0004551C(ppu_context* ctx) { exec_entry<dyn_sort>(ctx); }
void func_0004809C(ppu_context* ctx) { exec_entry<struct_compare<k_struct_eq, true>>(ctx); }
void func_000484AC(ppu_context* ctx) { exec_entry<struct_compare<k_struct_ne, false>>(ctx); }
void func_00048C4C(ppu_context* ctx) { exec_entry<delegate_compare<0x00048CE0, 0x00048D3C, true>>(ctx); }
void func_00048E10(ppu_context* ctx) { exec_entry<delegate_compare<0x00048EA4, 0x00048F00, false>>(ctx); }
void func_0005C29C(ppu_context* ctx) { exec_entry<str_concat>(ctx); }
void func_0005C5D4(ppu_context* ctx) { exec_entry<str_at>(ctx); }
void func_0005D130(ppu_context* ctx) { exec_entry<StrEqual>(ctx); }
void func_0005D30C(ppu_context* ctx) { exec_entry<StrNotEqual>(ctx); }
void func_0005D4E8(ppu_context* ctx) { exec_entry<StrComplementEqual>(ctx); }
void func_0005C9C0(ppu_context* ctx) { exec_entry<StrLess>(ctx); }
void func_0005CB9C(ppu_context* ctx) { exec_entry<StrGreater>(ctx); }
void func_0005CD78(ppu_context* ctx) { exec_entry<StrLessEqual>(ctx); }
void func_0005CF54(ppu_context* ctx) { exec_entry<StrGreaterEqual>(ctx); }
void func_0005D6C4(ppu_context* ctx) { exec_entry<str_concat_to>(ctx); }
void func_0005D994(ppu_context* ctx) { exec_entry<str_at_to>(ctx); }
void func_0005DD18(ppu_context* ctx) { exec_entry<str_subtract_from>(ctx); }
void func_0005E34C(ppu_context* ctx) { exec_entry<str_length>(ctx); }
void func_0005E448(ppu_context* ctx) { exec_entry<str_instr>(ctx); }
void func_0005E7D0(ppu_context* ctx) { exec_entry<str_mid>(ctx); }
void func_0005EAF4(ppu_context* ctx) { exec_entry<str_left_op>(ctx); }
void func_0005ED64(ppu_context* ctx) { exec_entry<str_right>(ctx); }
void func_0005F000(ppu_context* ctx) { exec_entry<StrCaps>(ctx); }
void func_0005F27C(ppu_context* ctx) { exec_entry<StrLocs>(ctx); }
void func_0005F4FC(ppu_context* ctx) { exec_entry<str_chr>(ctx); }
void func_0005F64C(ppu_context* ctx) { exec_entry<str_asc>(ctx); }
void func_0005F754(ppu_context* ctx) { exec_entry<str_repl>(ctx); }
void func_000538F0(ppu_context* ctx) { exec_entry<InterfaceToObject>(ctx); }
void func_00053584(ppu_context* ctx) { exec_entry<InterfaceToString>(ctx); }
void func_00053814(ppu_context* ctx) { exec_entry<InterfaceToBool>(ctx); }
void func_0004C718(ppu_context* ctx) { exec_entry<RotatorToVector>(ctx); }
void func_00049DCC(ppu_context* ctx) { exec_entry<ByteToInt>(ctx); }
void func_00049E80(ppu_context* ctx) { exec_entry<ByteToBool>(ctx); }
void func_00049F40(ppu_context* ctx) { exec_entry<ByteToFloat>(ctx); }
void func_0004A200(ppu_context* ctx) { exec_entry<IntToByte>(ctx); }
void func_0004A2B4(ppu_context* ctx) { exec_entry<IntToBool>(ctx); }
void func_0004A374(ppu_context* ctx) { exec_entry<IntToFloat>(ctx); }
void func_0004A5A0(ppu_context* ctx) { exec_entry<BoolToByte>(ctx); }
void func_0004A658(ppu_context* ctx) { exec_entry<BoolToInt>(ctx); }
void func_0004A710(ppu_context* ctx) { exec_entry<BoolToFloat>(ctx); }
void func_0004A938(ppu_context* ctx) { exec_entry<FloatToByte>(ctx); }
void func_0004A9F8(ppu_context* ctx) { exec_entry<FloatToInt>(ctx); }
void func_0004AAC0(ppu_context* ctx) { exec_entry<FloatToBool>(ctx); }
void func_00052FE8(ppu_context* ctx) { exec_entry<object_to_interface>(ctx); }
void func_00053EEC(ppu_context* ctx) { exec_entry<NameToBool>(ctx); }
void func_000541A0(ppu_context* ctx) { exec_entry<StringToByte>(ctx); }
void func_000542A0(ppu_context* ctx) { exec_entry<StringToInt>(ctx); }
void func_000543A0(ppu_context* ctx) { exec_entry<StringToBool>(ctx); }
void func_00054644(ppu_context* ctx) { exec_entry<StringToFloat>(ctx); }
void func_0004BF48(ppu_context* ctx) { exec_entry<StringToVector>(ctx); }
void func_0004C0F8(ppu_context* ctx) { exec_entry<StringToRotator>(ctx); }
void func_0004C2A0(ppu_context* ctx) { exec_entry<VectorToBool>(ctx); }
void func_0004C52C(ppu_context* ctx) { exec_entry<VectorToRotator>(ctx); }
void func_0004C60C(ppu_context* ctx) { exec_entry<RotatorToBool>(ctx); }
void func_0004A008(ppu_context* ctx) { exec_entry<ByteToString>(ctx); }
void func_0004A434(ppu_context* ctx) { exec_entry<IntToString>(ctx); }
void func_0004A7D8(ppu_context* ctx) { exec_entry<BoolToString>(ctx); }
void func_0004AB98(ppu_context* ctx) { exec_entry<FloatToString>(ctx); }
void func_000532F4(ppu_context* ctx) { exec_entry<ObjectToString>(ctx); }
void func_00053FC8(ppu_context* ctx) { exec_entry<NameToString>(ctx); }
void func_0004C3AC(ppu_context* ctx) { exec_entry<VectorToString>(ctx); }
void func_0004C7F8(ppu_context* ctx) { exec_entry<RotatorToString>(ctx); }
void func_000539A8(ppu_context* ctx) { exec_entry<delegate_to_string>(ctx); }
void func_0005408C(ppu_context* ctx) { exec_entry<StringToName>(ctx); }
void func_0004F998(ppu_context* ctx) { exec_entry<vector_unary<0x0004FA10, vnormal>>(ctx); }
void func_0004FAF4(ppu_context* ctx) { exec_entry<vector_unary<0x0004FB6C, vsize>>(ctx); }
void func_0004FC94(ppu_context* ctx) { exec_entry<vector_unary<0x0004FD0C, vsize_sq>>(ctx); }
void func_00050714(ppu_context* ctx) { exec_entry<vector_op<0x00050790, 0x000507E4, 0x70, 0x7C, vcross>>(ctx); }
void func_00050C94(ppu_context* ctx) { exec_entry<unrotate>(ctx); }
void func_00050EB8(ppu_context* ctx) { exec_entry<vector_op<0x00050F34, 0x00050F88, 0x70, 0x7C, vrotate>>(ctx); }
void func_000515DC(ppu_context* ctx) { exec_entry<vector_op<0x00051658, 0x000516AC, 0x70, 0x7C, vmul>>(ctx); }
void func_0004FE2C(ppu_context* ctx) { exec_entry<vector_assign<0x0004FEBC, 0x0004FEEC, 0x0004FF4C, fsub_bits>>(ctx); }
void func_0004D9CC(ppu_context* ctx) { exec_entry<vector_op<0x0004DA48, 0x0004DA9C, 0x70, 0x7C, radd>>(ctx); }
void func_0004D874(ppu_context* ctx) { exec_entry<vector_op<0x0004D8F0, 0x0004D944, 0x70, 0x7C, rsub>>(ctx); }
void func_0004E6C8(ppu_context* ctx) { exec_entry<vector_op<0x0004E744, 0x0004E798, 0x70, 0x7C, requal>>(ctx); }
void func_0004E560(ppu_context* ctx) { exec_entry<vector_op<0x0004E5DC, 0x0004E630, 0x70, 0x7C, runequal>>(ctx); }
void func_0004D14C(ppu_context* ctx) { exec_entry<get_axes>(ctx); }
void func_0004C978(ppu_context* ctx) { exec_entry<rot_rand>(ctx); }
void func_00069C40(ppu_context* ctx) { exec_entry<vector_unary<0x00069CB8, vzero>>(ctx); }
void func_00058088(ppu_context* ctx) { exec_entry<rand_op>(ctx); }
void func_0005A1BC(ppu_context* ctx) { exec_entry<libm<0x0005A23C, 0x0005A250, func_00F64730>>(ctx); }
void func_0005A378(ppu_context* ctx) { exec_entry<libm<0x0005A3F8, 0x0005A40C, func_00F64850>>(ctx); }
void func_0005A530(ppu_context* ctx) { exec_entry<libm<0x0005A5B0, 0x0005A5C4, func_00F3FC08>>(ctx); }
void func_0005A5F8(ppu_context* ctx) { exec_entry<libm<0x0005A678, 0x0005A68C, func_00F40130>>(ctx); }
void func_0005A840(ppu_context* ctx) { exec_entry<exp_op>(ctx); }
void func_0005AB00(ppu_context* ctx) { exec_entry<loge_op>(ctx); }
void func_0005AD18(ppu_context* ctx) { exec_entry<sqrt_op>(ctx); }
void func_0005AE30(ppu_context* ctx) { exec_entry<unary<0x0005AEB0, fsquare_bits>>(ctx); }
void func_0005AEF8(ppu_context* ctx) { exec_entry<round_op>(ctx); }
void func_0005610C(ppu_context* ctx) { exec_entry<divide_int<0x00056190, 0x000561EC, 0x00056218, 0x0160B21C, irem>>(ctx); }
void func_00055F98(ppu_context* ctx) { exec_entry<divide_int<0x0005601C, 0x00056078, 0x000560A4, k_divide_zero, idiv>>(ctx); }
void func_00056518(ppu_context* ctx) { exec_entry<binary<0x0005659C, 0x000565F8, ishl>>(ctx); }
void func_00056660(ppu_context* ctx) { exec_entry<binary<0x000566E4, 0x00056740, isar>>(ctx); }
void func_000567A8(ppu_context* ctx) { exec_entry<binary<0x0005682C, 0x00056888, ishr>>(ctx); }
void func_00055CC0(ppu_context* ctx) { exec_entry<unary<0x00055D40, inot>>(ctx); }
void func_00058848(ppu_context* ctx) { exec_entry<pow_op>(ctx); }
void func_00059BE8(ppu_context* ctx) { exec_entry<divide_float_to>(ctx); }
void func_000574BC(ppu_context* ctx) { exec_entry<int_mul_float>(ctx); }
void func_00057680(ppu_context* ctx) { exec_entry<int_div_float>(ctx); }
void func_000598D8(ppu_context* ctx) { exec_entry<float_near>(ctx); }
void func_00058C60(ppu_context* ctx) { exec_entry<percent_float>(ctx); }
void func_00055A68(ppu_context* ctx) { exec_entry<byte_crement<0x00055AF8, 0x00055B28, 1, true>>(ctx); }
void func_00055B94(ppu_context* ctx) { exec_entry<byte_crement<0x00055C24, 0x00055C54, 0xFF, true>>(ctx); }
void func_00055810(ppu_context* ctx) { exec_entry<byte_crement<0x000558A0, 0x000558D0, 1, false>>(ctx); }
void func_0005593C(ppu_context* ctx) { exec_entry<byte_crement<0x000559CC, 0x000559FC, 0xFF, false>>(ctx); }
void func_000554D8(ppu_context* ctx) { exec_entry<byte_compound<0x0005556C, 0x0005559C, 0x00055604, iadd>>(ctx); }
void func_00055674(ppu_context* ctx) { exec_entry<byte_compound<0x00055708, 0x00055738, 0x000557A0, isub>>(ctx); }
void func_00054FD8(ppu_context* ctx) { exec_entry<byte_compound<0x0005506C, 0x0005509C, 0x00055104, imul>>(ctx); }
void func_00055330(ppu_context* ctx) { exec_entry<byte_divide>(ctx); }
void func_00055174(ppu_context* ctx) { exec_entry<byte_mul_float>(ctx); }
void func_00054CC0(ppu_context* ctx) { exec_entry<bool_compare<0x00054D44, 0x00054DB0, false>>(ctx); }
void func_000652E8(ppu_context* ctx) { exec_entry<log_internal>(ctx); }
void func_0006544C(ppu_context* ctx) { exec_entry<warn_internal>(ctx); }
void func_00066308(ppu_context* ctx) { exec_entry<goto_state>(ctx); }
void func_00068718(ppu_context* ctx) { exec_entry<is_in_state>(ctx); }
void func_00068A68(ppu_context* ctx) { exec_entry<get_state_name>(ctx); }
void func_00066AE4(ppu_context* ctx) { exec_entry<is_a>(ctx); }
void func_00065D60(ppu_context* ctx) { exec_entry<class_is_child_of>(ctx); }
void func_00066C34(ppu_context* ctx) { exec_entry<enable_probe>(ctx); }
void func_00066DB4(ppu_context* ctx) { exec_entry<disable_probe>(ctx); }
void func_0006732C(ppu_context* ctx) { exec_entry<save_config>(ctx); }
