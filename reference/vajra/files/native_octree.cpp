#include "guest.h"

#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace {

constexpr uint32_t k_tag = 0x019C2E9C;
constexpr uint32_t k_zero = 0x001CBBD0;
constexpr uint32_t k_root_bounds = 0x01A61F10;
constexpr uint32_t k_result_size = 0x4C;
constexpr uint32_t k_chunk_request = 0x54;

struct Variant {
    uint32_t half;
    uint32_t alloc_return;
    uint32_t recheck_return;
    bool by_center;
};
constexpr Variant k_box{0x001CBBD4, 0x00447D10, 0, false};
constexpr Variant k_center{0x001123C0, 0x004477BC, 0x004476B4, true};

alignas(16) constexpr float k_signs[8][4] = {{-1, -1, -1, 0}, {-1, -1, 1, 0}, {-1, 1, -1, 0}, {-1, 1, 1, 0},
                                              {1, -1, -1, 0},  {1, -1, 1, 0},  {1, 1, -1, 0},  {1, 1, 1, 0}};

float fmadds(float t, float acc) { return (float)std::fma((double)t, (double)t, (double)acc); }

__m128 vmaddfp(__m128 a, __m128 c, __m128 b)
{
    return _mm_fmadd_ps(a, c, b);
}

struct Search {
    ppu_mm m;
    ppu_context* ctx;
    const Variant& v;
    uint32_t octree;
    uint32_t call_sp;
    __m128 point;
    float r2;
    uint32_t tag = 0, zero = 0, stack = 0;
    __m128 lo{}, hi{}, center{}, half_scale{};
    float center_r2 = 0;
    bool called = false;

    uint32_t r32(uint32_t a) const { return vm_read32_mm(&m, a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(&m, a); }
    uint32_t fast32(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 4), 1)) {
            uint32_t x;
            memcpy(&x, m.vb + a, 4);
            return __builtin_bswap32(x);
        }
        return vm_read32_mm(&m, a);
    }
    uint64_t fast64(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 8), 1)) {
            uint64_t x;
            memcpy(&x, m.vb + a, 8);
            return __builtin_bswap64(x);
        }
        return vm_read64_mm(&m, a);
    }
    void w32(uint32_t a, uint32_t value) { vm_write32_mm(&m, a, value); }
    void fast_w32(uint32_t a, uint32_t value)
    {
        if (__builtin_expect(plain_store(m, a, 4), 1)) {
            const uint32_t be = __builtin_bswap32(value);
            memcpy(m.vb + a, &be, 4);
            vm_track_write(a, 4);
            return;
        }
        vm_write32_mm(&m, a, value);
    }

    __m128 vec3(uint32_t a) const
    {
        if (vm_mm_ok(&m, a, 16))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), 0));
    }

    uint32_t peek(uint32_t a) const
    {
        uint32_t x = 0;
        if (vm_mm_ok(&m, a, 4)) memcpy(&x, m.vb + a, 4);
        return __builtin_bswap32(x);
    }

    void touch(uint32_t a) const { _mm_prefetch((const char*)m.vb + a, _MM_HINT_T0); }

    void reload()
    {
        tag = r32(k_tag);
        stack = r32(octree + 0x10);
        lo = vec3(octree + 0xA4);
        hi = vec3(octree + 0xB0);
        half_scale = _mm_set1_ps(from_bits(r32(v.half)));
        if (v.by_center) {
            center = vec3(octree + 0x20);
            center_r2 = from_bits(r32(octree + 0x60));
        } else {
            zero = r32(k_zero);
        }
    }

    void call(uint32_t lr, void (*fn)(ppu_context*))
    {
        const uint64_t sp = ctx->gpr[1];
        ctx->gpr[1] = call_sp;
        ctx->lr = lr;
        run(fn, ctx);
        ctx->gpr[1] = sp;
        reload();
        called = true;
    }

    bool box_hit(uint32_t prim) const
    {
        const __m128 c = vec3(prim + 0x5C), e = vec3(prim + 0x68);
        const __m128 lo_p = _mm_sub_ps(c, e), hi_p = _mm_add_ps(c, e);
        const __m128 ge = _mm_cmpge_ps(point, lo_p);
        const int out = (~_mm_movemask_ps(ge) | _mm_movemask_ps(_mm_cmpgt_ps(point, hi_p))) & 7;
        float d = from_bits(zero);
        if (!out) return !(d > r2);
        alignas(16) float t[4];
        _mm_store_ps(t, _mm_sub_ps(point, _mm_blendv_ps(lo_p, hi_p, ge)));
        if (out & 1) d = t[0] * t[0];
        if (out & 2) d = fmadds(t[1], d);
        if (out & 4) d = fmadds(t[2], d);
        return !(d > r2);
    }

    bool center_hit(uint32_t prim) const
    {
        alignas(16) float t[4];
        _mm_store_ps(t, _mm_sub_ps(vec3(prim + 0x5C), center));
        const float d = fmadds(t[2], fmadds(t[0], t[1] * t[1]));
        return !(d >= center_r2);
    }

    bool recheck(uint32_t actor, uint32_t prim)
    {
        ctx->gpr[3] = actor;
        ctx->gpr[4] = prim;
        ctx->ctr = r32(r32(r32(actor) + 0x328));
        call(v.recheck_return, guest_icall);
        return (int32_t)ctx->gpr[3] != 0;
    }

    uint32_t allocate(uint32_t s)
    {
        const uint64_t top_end = r64(s);
        const uint32_t result = ((uint32_t)(top_end >> 32) + 7u) & ~7u;
        if (result + k_result_size <= (uint32_t)top_end) return result;
        ctx->gpr[3] = s;
        ctx->gpr[4] = k_chunk_request;
        call(v.alloc_return, func_000B902C);
        return (r32(s) + 7u) & ~7u;
    }

    void add(uint32_t actor, uint32_t prim)
    {
        const uint32_t fill = v.by_center ? 0 : zero, s = stack;
        const uint32_t result = allocate(s);
        w32(s, result + k_result_size);
        if (result && vm_mm_st_ok(&m, result, 4) && vm_mm_st_ok(&m, result + k_result_size - 4, 4)) {
            const uint32_t f = __builtin_bswap32(fill);
            const uint32_t record[k_result_size / 4] = {__builtin_bswap32(r32(octree + 0xC)), __builtin_bswap32(actor), f, f, f, f, f, f, f, ~0u, 0, 0,
                                                        __builtin_bswap32(prim), 0, 0, 0, 0, ~0u, 0};
            memcpy(m.vb + result, record, k_result_size);
            vm_track_write(result, k_result_size);
            w32(octree + 0xC, result);
            w32(actor + 0x18C, tag);
            return;
        }
        if (result) {
            w32(result + 4, 0);
            for (uint32_t k = 0x08; k <= 0x20; k += 4) w32(result + k, fill);
            w32(result + 0x24, 0xFFFFFFFFu);
            for (uint32_t k = 0x28; k <= 0x40; k += 4) w32(result + k, 0);
            w32(result + 0x44, 0xFFFFFFFFu);
            w32(result + 0x48, 0);
        }
        w32(result + 4, actor);
        w32(result + 0x30, prim);
        w32(result, r32(octree + 0xC));
        w32(octree + 0xC, result);
        w32(actor + 0x18C, tag);
    }

    void primitive(uint32_t prim)
    {
        if (fast32(prim + 0x58) == tag) return;
        w32(prim + 0x58, tag);
        const uint32_t actor = fast32(prim + 0x4C);
        if (!actor) return;
        if (fast32(actor + 0x18C) == tag && (!v.by_center || !recheck(actor, prim))) return;
        if (v.by_center ? center_hit(prim) : box_hit(prim)) add(actor, prim);
    }

    void prefetch(uint32_t list, int32_t i, int32_t count) const
    {
        if (i + 8 < count) {
            const uint32_t prim = peek(list + 4u * (uint32_t)(i + 8));
            touch(prim + 0x4C);
            touch(prim + 0x73);
        }
        if (i + 4 < count) touch(peek(peek(list + 4u * (uint32_t)(i + 4)) + 0x4C) + 0x18C);
    }

    uint64_t header(uint32_t n)
    {
        called = false;
        return fast64(n);
    }

    void node(uint32_t n, __m128 b)
    {
        if (const uint32_t kids = peek(n + 0xC)) {
            touch(kids);
            touch(kids + 0x40);
        }
        uint64_t head = header(n);
        for (int32_t i = 0; i < (int32_t)(uint32_t)head; i++) {
            const uint32_t list = (uint32_t)(head >> 32);
            prefetch(list, i, (int32_t)(uint32_t)head);
            primitive(fast32(list + 4u * (uint32_t)i));
            if (called) head = header(n);
        }
        uint32_t kids = fast32(n + 0xC);
        if (!kids) return;
        const int max_nle = _mm_movemask_ps(_mm_cmpnle_ps(hi, b)), max_gt = _mm_movemask_ps(_mm_cmpgt_ps(hi, b));
        const int min_ngt = _mm_movemask_ps(_mm_cmpngt_ps(lo, b)), min_le = _mm_movemask_ps(_mm_cmple_ps(lo, b));
        uint32_t sel = 0;
        if (max_nle & 1) {
            if (max_nle & 2) sel |= (max_nle & 4 ? 0x80u : 0) | (min_ngt & 4 ? 0x40u : 0);
            if (min_ngt & 2) sel |= (max_nle & 4 ? 0x20u : 0) | (min_le & 4 ? 0x10u : 0);
        }
        if (min_ngt & 1) {
            if (max_nle & 2) sel |= (max_gt & 4 ? 0x08u : 0) | (min_le & 4 ? 0x04u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x02u : 0) | (min_ngt & 4 ? 0x01u : 0);
        }
        if (!sel) return;
        const __m128 half = vmaddfp(_mm_shuffle_ps(b, b, 0xFF), half_scale, _mm_set1_ps(-0.0f));
        while (sel) {
            const uint32_t h = 31u - (uint32_t)__builtin_clz(sel);
            sel ^= 1u << h;
            node(kids + 16u * h, _mm_blend_ps(vmaddfp(_mm_load_ps(k_signs[h]), half, b), half, 8));
            if (called) kids = r32(n + 0xC);
        }
    }
};

void search(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t octree = (uint32_t)ctx->gpr[3], stack = (uint32_t)ctx->gpr[4], point = (uint32_t)ctx->gpr[5];
    const bool by_box = (int32_t)ctx->gpr[7] != 0;
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const double radius = ctx->fpr[1];
    vm_write64_mm(&m, sp + 0x10, lr);
    vm_write64_mm(&m, sp - 0xC0, sp);
    vm_write64_mm(&m, sp - 0x1E0, sp - 0xC0);
    const uint32_t tag = vm_read32_mm(&m, k_tag) + 1;
    vm_write32_mm(&m, octree + 0xC, 0);
    vm_write32_mm(&m, octree + 0x10, stack);
    vm_write32_mm(&m, k_tag, tag);
    const double x = from_bits(vm_read32_mm(&m, point)), y = from_bits(vm_read32_mm(&m, point + 4)), z = from_bits(vm_read32_mm(&m, point + 8));
    const uint32_t box[6] = {stfs(single(x - radius)), stfs(single(y - radius)), stfs(single(z - radius)),
                             stfs(single(x + radius)), stfs(single(y + radius)), stfs(single(z + radius))};
    for (int i = 0; i < 6; i++) vm_write32_mm(&m, octree + 0xA4 + 4 * i, box[i]);
    vm_write8_mm(&m, octree + 0xBC, 1);
    const double r2 = single(radius * radius);
    const uint32_t qx = stfs(x), qy = stfs(y), qz = stfs(z), qr2 = stfs(r2);
    if (!by_box) {
        vm_write32_mm(&m, octree + 0x20, qx);
        vm_write32_mm(&m, octree + 0x24, qy);
        vm_write32_mm(&m, octree + 0x60, qr2);
        vm_write32_mm(&m, octree + 0x28, qz);
    }
    Search s{m, ctx, by_box ? k_box : k_center, octree, (uint32_t)sp - 0x1E0u,
             _mm_setr_ps(from_bits(qx), from_bits(qy), from_bits(qz), 0.0f), from_bits(qr2)};
    s.reload();
    s.node(vm_read32_mm(&m, octree + 4),
           _mm_setr_ps(from_bits(vm_read32_mm(&m, k_root_bounds)), from_bits(vm_read32_mm(&m, k_root_bounds + 4)),
                       from_bits(vm_read32_mm(&m, k_root_bounds + 8)), from_bits(vm_read32_mm(&m, k_root_bounds + 12))));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = vm_read32_mm(&m, octree + 0xC);
}

}

void func_0044C160(ppu_context* ctx)
{
    search(ctx);
}

namespace {

struct LineSearch {
    ppu_mm m;
    ppu_context* ctx;
    uint32_t query;
    uint32_t call_sp;
    uint32_t tag = 0, stack = 0, ignore = 0, flags = 0;
    __m128 lo{}, hi{}, q_start{}, q_inv{}, q_ext{}, q_start_lo{}, q_start_hi{};
    bool called = false;

    uint32_t r32(uint32_t a) const { return vm_read32_mm(&m, a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(&m, a); }
    uint32_t fast32(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 4), 1)) {
            uint32_t x;
            memcpy(&x, m.vb + a, 4);
            return __builtin_bswap32(x);
        }
        return vm_read32_mm(&m, a);
    }
    uint64_t fast64(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 8), 1)) {
            uint64_t x;
            memcpy(&x, m.vb + a, 8);
            return __builtin_bswap64(x);
        }
        return vm_read64_mm(&m, a);
    }
    void w32(uint32_t a, uint32_t value) { vm_write32_mm(&m, a, value); }
    void fast_w32(uint32_t a, uint32_t value)
    {
        if (__builtin_expect(plain_store(m, a, 4), 1)) {
            const uint32_t be = __builtin_bswap32(value);
            memcpy(m.vb + a, &be, 4);
            vm_track_write(a, 4);
            return;
        }
        vm_write32_mm(&m, a, value);
    }

    static inline __m128i bswap32_mask()
    {
        return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    }

    __m128 vec4(uint32_t a) const
    {
        if (vm_mm_ok(&m, a, 16))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask()));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), (int)r32(a + 12)));
    }

    __m128 vec3(uint32_t a) const
    {
        if (vm_mm_ok(&m, a, 16))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask()));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), 0));
    }

    uint32_t peek(uint32_t a) const
    {
        uint32_t x = 0;
        if (vm_mm_ok(&m, a, 4)) memcpy(&x, m.vb + a, 4);
        return __builtin_bswap32(x);
    }

    void touch(uint32_t a) const { _mm_prefetch((const char*)m.vb + a, _MM_HINT_T0); }

    void reload()
    {
        tag = r32(k_tag);
        stack = r32(query + 0x10);
        ignore = r32(query + 0x3C);
        flags = r32(query + 0x2C);
        lo = vec3(query + 0xA4);
        hi = vec3(query + 0xB0);
        q_start = vec3(query + 0x20);
        q_inv = vec3(query + 0x30);
        q_ext = vec3(query + 0x40);
        q_start_lo = _mm_sub_ps(q_start, q_ext);
        q_start_hi = _mm_add_ps(q_start, q_ext);
    }

    void call(uint32_t lr, void (*fn)(ppu_context*))
    {
        const uint64_t sp = ctx->gpr[1];
        ctx->gpr[1] = call_sp;
        ctx->lr = lr;
        run(fn, ctx);
        ctx->gpr[1] = sp;
        reload();
        called = true;
    }

    inline bool slab_hit(__m128 center, __m128 ext) const
    {
        const __m128 bmin = _mm_sub_ps(_mm_sub_ps(center, ext), q_start_hi);
        const __m128 bmax = _mm_sub_ps(_mm_add_ps(center, ext), q_start_lo);
        const __m128 t1 = _mm_mul_ps(bmin, q_inv);
        const __m128 t2 = _mm_mul_ps(bmax, q_inv);
        const __m128 lo_t = _mm_blend_ps(_mm_min_ps(t1, t2), _mm_setzero_ps(), 8);
        const __m128 hi_t = _mm_blend_ps(_mm_max_ps(t1, t2), _mm_set1_ps(1.0f), 8);
        const __m128 m0 = _mm_max_ps(lo_t, _mm_shuffle_ps(lo_t, lo_t, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128 hmax = _mm_max_ps(m0, _mm_shuffle_ps(m0, m0, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m128 n0 = _mm_min_ps(hi_t, _mm_shuffle_ps(hi_t, hi_t, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128 hmin = _mm_min_ps(n0, _mm_shuffle_ps(n0, n0, _MM_SHUFFLE(2, 3, 0, 1)));
        return (_mm_movemask_ps(_mm_cmpge_ss(hmin, hmax)) & 1) != 0;
    }

    uint32_t allocate(uint32_t s)
    {
        const uint64_t top_end = r64(s);
        const uint32_t result = ((uint32_t)(top_end >> 32) + 7u) & ~7u;
        if (result + k_result_size <= (uint32_t)top_end) return result;
        ctx->gpr[3] = s;
        ctx->gpr[4] = k_chunk_request;
        call(0x0044620C, func_000B902C);
        return (r32(s) + 7u) & ~7u;
    }

    bool test_prim(uint32_t prim)
    {
        if (fast32(prim + 0x58) == tag) return false;
        fast_w32(prim + 0x58, tag);
        const uint32_t actor = fast32(prim + 0x4C);
        if (!actor || actor == ignore) return false;
        __m128 p_center, p_ext;
        if (__builtin_expect(vm_mm_ok(&m, prim + 0x5C, 28), 1)) {
            const __m128i sh = bswap32_mask();
            p_center = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + prim + 0x5C)), sh));
            p_ext = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + prim + 0x68)), sh));
        } else {
            p_center = vec3(prim + 0x5C);
            p_ext = vec3(prim + 0x68);
        }
        if (!slab_hit(p_center, p_ext)) return false;
        if ((fast32(prim + 0x12C) & 0x44000000u) != 0x44000000u) return false;
        if ((fast32(actor + 0xB0) & 0x00000010u) == 0) return false;
        for (uint32_t a = ignore; a; a = fast32(a + 0x98))
            if (a == actor) return false;
        for (uint32_t a = actor; a; a = fast32(a + 0x98))
            if (a == ignore) return false;
        ctx->gpr[3] = actor;
        ctx->gpr[4] = prim;
        ctx->gpr[5] = ignore;
        ctx->gpr[6] = flags;
        ctx->ctr = fast32(fast32(fast32(actor) + 0x1B0));
        call(0x004460B4, guest_icall);
        if ((int32_t)ctx->gpr[3] == 0) return false;

        const uint32_t hit_ea = call_sp + 0xA0;
        if (vm_mm_st_ok(&m, hit_ea, k_result_size)) {
            uint32_t init[k_result_size / 4] = {};
            init[9] = ~0u;
            init[17] = ~0u;
            memcpy(m.vb + hit_ea, init, k_result_size);
            vm_track_write(hit_ea, k_result_size);
        } else {
            for (uint32_t off = 0; off < k_result_size; off += 4)
                w32(hit_ea + off, (off == 0x24 || off == 0x44) ? 0xFFFFFFFFu : 0u);
        }
        ctx->gpr[3] = prim;
        ctx->gpr[4] = hit_ea;
        ctx->gpr[5] = query + 0x7C;
        ctx->gpr[6] = query + 0x20;
        ctx->gpr[7] = query + 0x40;
        ctx->gpr[8] = flags;
        ctx->ctr = fast32(fast32(fast32(prim) + 0x190));
        call(0x00446138, guest_icall);
        if ((int32_t)ctx->gpr[3] != 0) return false;

        const uint32_t s = stack;
        const uint32_t res = allocate(s);
        w32(s, res + k_result_size);
        if (res) {
            if (vm_mm_ok(&m, hit_ea, k_result_size) && vm_mm_st_ok(&m, res, k_result_size)) {
                memcpy(m.vb + res, m.vb + hit_ea, k_result_size);
                vm_track_write(res, k_result_size);
            } else {
                for (uint32_t off = 0; off < k_result_size; off += 4) w32(res + off, r32(hit_ea + off));
            }
        }
        w32(res, r32(query + 0xC));
        w32(query + 0xC, res);
        return (flags & 0x200u) != 0;
    }

    bool node(uint32_t n, __m128 b)
    {
        if (const uint32_t kids = peek(n + 0xC)) {
            touch(kids);
            touch(kids + 0x40);
        }
        called = false;
        uint64_t head = fast64(n);
        uint32_t list = (uint32_t)(head >> 32);
        const int32_t count = (int32_t)(uint32_t)head;
        bool list_fast = count > 0 && vm_mm_ok(&m, list, 4u * (uint32_t)count);
        for (int32_t i = 0; i < count; i++) {
            if (i + 6 < count) {
                uint32_t p;
                if (list_fast) {
                    memcpy(&p, m.vb + list + 4u * (uint32_t)(i + 6), 4);
                    p = __builtin_bswap32(p);
                } else {
                    p = peek(list + 4u * (uint32_t)(i + 6));
                }
                touch(p + 0x4C);
                touch(p + 0x5C);
            }
            uint32_t prim;
            if (__builtin_expect(list_fast, 1)) {
                memcpy(&prim, m.vb + list + 4u * (uint32_t)i, 4);
                prim = __builtin_bswap32(prim);
            } else {
                prim = fast32(list + 4u * (uint32_t)i);
            }
            if (test_prim(prim)) return true;
            if (__builtin_expect(called, 0)) {
                called = false;
                head = fast64(n);
                list = (uint32_t)(head >> 32);
                list_fast = count > 0 && vm_mm_ok(&m, list, 4u * (uint32_t)count);
            }
        }
        uint32_t kids = fast32(n + 0xC);
        if (!kids) return false;
        const int max_nle = _mm_movemask_ps(_mm_cmpnle_ps(hi, b)), max_gt = _mm_movemask_ps(_mm_cmpgt_ps(hi, b));
        const int min_ngt = _mm_movemask_ps(_mm_cmpngt_ps(lo, b)), min_le = _mm_movemask_ps(_mm_cmple_ps(lo, b));
        uint32_t sel = 0;
        if (max_nle & 1) {
            if (max_nle & 2) sel |= (max_nle & 4 ? 0x80u : 0) | (min_ngt & 4 ? 0x40u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x20u : 0) | (min_le & 4 ? 0x10u : 0);
        }
        if (min_ngt & 1) {
            if (max_nle & 2) sel |= (max_gt & 4 ? 0x08u : 0) | (min_le & 4 ? 0x04u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x02u : 0) | (min_le & 4 ? 0x01u : 0);
        }
        if (!sel) return false;
        const __m128 half = _mm_mul_ps(_mm_shuffle_ps(b, b, 0xFF), _mm_set1_ps(0.5f));
        while (sel) {
            const uint32_t h = 31u - (uint32_t)__builtin_clz(sel);
            sel ^= 1u << h;
            const __m128 child_b = _mm_blend_ps(_mm_fmadd_ps(_mm_load_ps(k_signs[h]), half, b), half, 8);
            if (slab_hit(child_b, half)) {
                if (node(kids + 16u * h, child_b)) return true;
                if (fast32(query + 0xC) != 0 && (flags & 0x200u)) return true;
                if (__builtin_expect(called, 0)) {
                    called = false;
                    kids = fast32(n + 0xC);
                }
            }
        }
        return false;
    }
};

}

void func_00445B7C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t root = (uint32_t)ctx->gpr[3], query = (uint32_t)ctx->gpr[4], bounds_ea = (uint32_t)ctx->gpr[5];
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    vm_write64_mm(&m, sp + 0x10, lr);
    vm_write64_mm(&m, sp - 0x150, sp);
    LineSearch s{m, ctx, query, (uint32_t)sp - 0x150u};
    s.reload();
    s.node(root, s.vec4(bounds_ea));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

namespace {

struct SingleLineSearch {
    ppu_mm m;
    ppu_context* ctx;
    uint32_t query;
    uint64_t sp;
    uint64_t lr;
    uint32_t call_sp;
    uint32_t tag = 0, stack = 0, ignore = 0, flags = 0, mirror = 0, world = 0;
    __m128 q_start{}, q_inv{}, q_ext{}, q_start_lo{}, q_start_hi{};
    __m128i zero_mask{};
    __m128 ray_origin{};
    bool framed = false;
    bool called = false;

    uint32_t r32(uint32_t a) const { return vm_read32_mm(&m, a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(&m, a); }
    uint32_t fast32(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 4), 1)) {
            uint32_t x;
            memcpy(&x, m.vb + a, 4);
            return __builtin_bswap32(x);
        }
        return vm_read32_mm(&m, a);
    }
    uint64_t fast64(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 8), 1)) {
            uint64_t x;
            memcpy(&x, m.vb + a, 8);
            return __builtin_bswap64(x);
        }
        return vm_read64_mm(&m, a);
    }
    void w32(uint32_t a, uint32_t value) { vm_write32_mm(&m, a, value); }
    void fast_w32(uint32_t a, uint32_t value)
    {
        if (__builtin_expect(plain_store(m, a, 4), 1)) {
            const uint32_t be = __builtin_bswap32(value);
            memcpy(m.vb + a, &be, 4);
            vm_track_write(a, 4);
            return;
        }
        vm_write32_mm(&m, a, value);
    }

    static inline __m128i bswap32_mask()
    {
        return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    }

    __m128 vec4(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 16), 1))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask()));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), (int)r32(a + 12)));
    }

    __m128i vec4i(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 16), 1))
            return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask());
        return _mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), (int)r32(a + 12));
    }

    __m128 vec3(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 16), 1))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask()));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), 0));
    }

    void touch(uint32_t a) const { _mm_prefetch((const char*)m.vb + a, _MM_HINT_T0); }

    void reload()
    {
        tag = fast32(k_tag);
        stack = fast32(query + 0x10);
        ignore = fast32(query + 0x3C);
        flags = fast32(query + 0x2C);
        mirror = fast32(query + 0xD8);
        world = fast32(query + 0xA0);
        q_start = vec3(query + 0x20);
        q_inv = vec3(query + 0x30);
        q_ext = vec3(query + 0x40);
        zero_mask = vec4i(query + 0x50);
        ray_origin = vec4(query + 0x70);
        q_start_lo = _mm_sub_ps(q_start, q_ext);
        q_start_hi = _mm_add_ps(q_start, q_ext);
    }

    void ensure_frame()
    {
        if (!framed) {
            vm_write64_mm(&m, sp + 0x10, lr);
            vm_write64_mm(&m, sp - 0x170, sp);
            framed = true;
        }
    }

    void call(uint32_t ret_lr, void (*fn)(ppu_context*))
    {
        ensure_frame();
        ctx->gpr[1] = call_sp;
        ctx->lr = ret_lr;
        run(fn, ctx);
        ctx->gpr[1] = sp;
        reload();
        called = true;
    }

    inline bool slab_hit(__m128 center, __m128 ext, float best_t) const
    {
        const __m128 bmin = _mm_sub_ps(_mm_sub_ps(center, ext), q_start_hi);
        const __m128 bmax = _mm_sub_ps(_mm_add_ps(center, ext), q_start_lo);
        const __m128 t1 = _mm_mul_ps(bmin, q_inv);
        const __m128 t2 = _mm_mul_ps(bmax, q_inv);
        const __m128 lo_t = _mm_blend_ps(_mm_min_ps(t1, t2), _mm_setzero_ps(), 8);
        const __m128 hi_t = _mm_blend_ps(_mm_max_ps(t1, t2), _mm_set1_ps(best_t), 8);
        const __m128 m0 = _mm_max_ps(lo_t, _mm_shuffle_ps(lo_t, lo_t, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128 hmax = _mm_max_ps(m0, _mm_shuffle_ps(m0, m0, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m128 n0 = _mm_min_ps(hi_t, _mm_shuffle_ps(hi_t, hi_t, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128 hmin = _mm_min_ps(n0, _mm_shuffle_ps(n0, n0, _MM_SHUFFLE(2, 3, 0, 1)));
        return (_mm_movemask_ps(_mm_cmpge_ss(hmin, hmax)) & 1) != 0;
    }

    uint32_t allocate(uint32_t s)
    {
        const uint64_t top_end = fast64(s);
        const uint32_t result = ((uint32_t)(top_end >> 32) + 7u) & ~7u;
        if (result + k_result_size <= (uint32_t)top_end) return result;
        ctx->gpr[3] = s;
        ctx->gpr[4] = k_chunk_request;
        call(0x00446784, func_000B902C);
        return (fast32(s) + 7u) & ~7u;
    }

    bool test_prim(uint32_t prim, float& best_t)
    {
        if (fast32(prim + 0x58) == tag) return false;
        fast_w32(prim + 0x58, tag);
        const uint32_t actor = fast32(prim + 0x4C);
        if (!actor || actor == ignore) return false;
        if (__builtin_expect((flags & 0x100u) != 0, 0)) {
            const uint32_t f128 = fast32(prim + 0x128);
            if (((f128 >> 12) & 1u) == 0 || ((f128 >> 1) & 1u) == 0) return false;
            ctx->gpr[3] = world;
            ctx->gpr[4] = prim;
            ctx->gpr[5] = 0;
            call(0x0044676C, func_00160B80);
            if ((int32_t)ctx->gpr[3] == 0) return false;
        } else {
            const uint32_t f12c = fast32(prim + 0x12C);
            if ((f12c & 0x48000000u) != 0x48000000u) return false;
            if ((fast32(actor + 0xB0) & 0x00000010u) == 0) return false;
        }
        __m128 p_center, p_ext;
        if (__builtin_expect(vm_mm_ok(&m, prim + 0x5C, 28), 1)) {
            const __m128i sh = bswap32_mask();
            p_center = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + prim + 0x5C)), sh));
            p_ext = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + prim + 0x68)), sh));
        } else {
            p_center = vec3(prim + 0x5C);
            p_ext = vec3(prim + 0x68);
        }
        if (!slab_hit(p_center, p_ext, best_t)) return false;
        for (uint32_t a = ignore; a; a = fast32(a + 0x98))
            if (a == actor) return false;
        for (uint32_t a = actor; a; a = fast32(a + 0x98))
            if (a == ignore) return false;
        ctx->gpr[3] = actor;
        ctx->gpr[4] = prim;
        ctx->gpr[5] = ignore;
        ctx->gpr[6] = flags;
        ctx->ctr = fast32(fast32(fast32(actor) + 0x1B0));
        call(0x004465BC, guest_icall);
        if ((int32_t)ctx->gpr[3] == 0) return false;

        ensure_frame();
        const uint32_t hit_ea = call_sp + 0xC0;
        if (__builtin_expect(plain_store(m, hit_ea, k_result_size), 1)) {
            uint32_t init[k_result_size / 4] = {};
            init[9] = ~0u;
            init[17] = ~0u;
            memcpy(m.vb + hit_ea, init, k_result_size);
            vm_track_write(hit_ea, k_result_size);
        } else {
            for (uint32_t off = 0; off < k_result_size; off += 4)
                w32(hit_ea + off, (off == 0x24 || off == 0x44) ? 0xFFFFFFFFu : 0u);
        }
        ctx->gpr[3] = prim;
        ctx->gpr[4] = hit_ea;
        ctx->gpr[5] = query + 0x7C;
        ctx->gpr[6] = query + 0x20;
        ctx->gpr[7] = query + 0x40;
        ctx->gpr[8] = flags;
        ctx->ctr = fast32(fast32(fast32(prim) + 0x190));
        call(0x00446640, guest_icall);
        if ((int32_t)ctx->gpr[3] != 0) return false;

        const uint32_t s = stack;
        const uint32_t res = allocate(s);
        fast_w32(s, res + k_result_size);
        if (res) {
            if (__builtin_expect(vm_mm_ok(&m, hit_ea, k_result_size) && plain_store(m, res, k_result_size), 1)) {
                memcpy(m.vb + res, m.vb + hit_ea, k_result_size);
                vm_track_write(res, k_result_size);
            } else {
                for (uint32_t off = 0; off < k_result_size; off += 4) w32(res + off, r32(hit_ea + off));
            }
        }
        fast_w32(res, fast32(query + 0xC));
        fast_w32(query + 0xC, res);
        const uint32_t prev_best = fast32(query + 0xD4);
        const float hit_time = from_bits(fast32(res + 0x20));
        if (!prev_best || hit_time < from_bits(fast32(prev_best + 0x20))) {
            fast_w32(query + 0xD4, res);
            if (flags & 0x400u) best_t = hit_time;
        }
        return (flags & 0x200u) != 0;
    }

    bool node(uint32_t n, __m128 t0, __m128 t1, __m128 b, float& best_t)
    {
        called = false;
        uint64_t head = fast64(n);
        uint32_t list = (uint32_t)(head >> 32);
        const int32_t count = (int32_t)(uint32_t)head;
        uint32_t kids = fast32(n + 0xC);
        bool list_fast = count > 0 && vm_mm_ok(&m, list, 4u * (uint32_t)count);
        const int32_t pref = count < 6 ? count : 6;
        for (int32_t i = 0; i < pref; i++) {
            uint32_t p;
            if (__builtin_expect(list_fast, 1)) {
                memcpy(&p, m.vb + list + 4u * (uint32_t)i, 4);
                p = __builtin_bswap32(p);
            } else {
                p = fast32(list + 4u * (uint32_t)i);
            }
            touch(p + 0x58);
        }

        __m128 tm{};
        uint32_t oct = 8;
        if (kids) {
            touch(kids);
            const __m128 tm_norm = _mm_mul_ps(_mm_add_ps(t0, t1), _mm_set1_ps(0.5f));
            const __m128 gt = _mm_cmpgt_ps(b, ray_origin);
            const __m128 tm_zero = _mm_blendv_ps(vec4(0x019DBFA0), vec4(0x019DBF90), gt);
            tm = _mm_blendv_ps(tm_norm, tm_zero, _mm_castsi128_ps(zero_mask));
            alignas(16) float t0a[4], tma[4];
            _mm_store_ps(t0a, t0);
            _mm_store_ps(tma, tm);
            oct = 0;
            if (t0a[0] > t0a[1] && t0a[0] > t0a[2]) {
                if (t0a[0] > tma[1]) oct |= 2u;
                if (t0a[0] > tma[2]) oct |= 1u;
            } else if (t0a[1] > t0a[2]) {
                if (t0a[1] > tma[0]) oct |= 4u;
                if (t0a[1] > tma[2]) oct |= 1u;
            } else {
                if (t0a[2] > tma[0]) oct |= 4u;
                if (t0a[2] > tma[1]) oct |= 2u;
            }
        }

        for (int32_t i = 0; i < count; i++) {
            uint32_t prim;
            if (__builtin_expect(list_fast, 1)) {
                memcpy(&prim, m.vb + list + 4u * (uint32_t)i, 4);
                prim = __builtin_bswap32(prim);
            } else {
                prim = fast32(list + 4u * (uint32_t)i);
            }
            if (test_prim(prim, best_t)) return true;
            if (__builtin_expect(called, 0)) {
                called = false;
                head = fast64(n);
                list = (uint32_t)(head >> 32);
                list_fast = count > 0 && vm_mm_ok(&m, list, 4u * (uint32_t)count);
            }
        }

        kids = fast32(n + 0xC);
        if (!kids) return false;

        static constexpr uint8_t k_next[8][3] = {
            {4, 2, 1}, {5, 3, 8}, {6, 8, 3}, {7, 8, 8},
            {8, 6, 5}, {8, 7, 8}, {8, 8, 7}, {8, 8, 8},
        };
        const __m128 half = _mm_mul_ps(_mm_shuffle_ps(b, b, 0xFF), _mm_set1_ps(0.5f));
        const __m128 zero = _mm_setzero_ps();
        while (oct <= 7u) {
            const __m128 mask = _mm_castsi128_ps(_mm_setr_epi32(
                (oct & 4u) ? -1 : 0, (oct & 2u) ? -1 : 0, (oct & 1u) ? -1 : 0, 0));
            const __m128 ct0 = _mm_blendv_ps(t0, tm, mask);
            const __m128 ct1 = _mm_blendv_ps(tm, t1, mask);
            alignas(16) float c1a[4];
            _mm_store_ps(c1a, ct1);
            const uint32_t exit_axis = (c1a[0] < c1a[1]) ? ((c1a[0] < c1a[2]) ? 0u : 2u) : ((c1a[1] < c1a[2]) ? 1u : 2u);
            const uint32_t next_oct = k_next[oct][exit_axis];
            const __m128 best_v = _mm_set1_ps(best_t);
            const int bad_hi = _mm_movemask_ps(_mm_cmpgt_ps(ct0, best_v)) & 7;
            const int bad_lo = _mm_movemask_ps(_mm_cmpgt_ps(zero, ct1)) & 7;
            if (!bad_hi && !bad_lo) {
                const uint32_t h = oct ^ mirror;
                const __m128 child_b = _mm_blend_ps(_mm_fmadd_ps(_mm_load_ps(k_signs[h]), half, b), half, 8);
                if (node(kids + 16u * h, ct0, ct1, child_b, best_t)) return true;
                if (__builtin_expect(called, 0)) {
                    called = false;
                    kids = fast32(n + 0xC);
                }
            }
            oct = next_oct;
        }
        return false;
    }
};

}

void func_004462E4(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t root = (uint32_t)ctx->gpr[3], query = (uint32_t)ctx->gpr[4];
    const uint32_t t_ea = (uint32_t)ctx->gpr[5], bounds_ea = (uint32_t)ctx->gpr[6];
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    SingleLineSearch s{m, ctx, query, sp, lr, (uint32_t)sp - 0x170u};
    s.reload();
    const __m128 t0 = s.vec4(t_ea);
    const __m128 t1 = s.vec4(t_ea + 0x20u);
    const __m128 b = s.vec4(bounds_ea);
    float best_t = from_bits(s.fast32(t_ea + 0xCu));
    s.node(root, t0, t1, b, best_t);
    s.fast_w32(t_ea + 0xCu, to_bits(best_t));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

namespace {

struct OverlapSearch {
    ppu_mm m;
    ppu_context* ctx;
    uint32_t query;
    uint64_t sp;
    uint64_t lr;
    uint32_t call_sp;
    uint32_t tag = 0, stack = 0, ignore = 0, flags = 0, extra = 0;
    __m128 lo{}, hi{};
    bool framed = false;
    bool called = false;

    uint32_t r32(uint32_t a) const { return vm_read32_mm(&m, a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(&m, a); }
    uint32_t fast32(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 4), 1)) {
            uint32_t x;
            std::memcpy(&x, m.vb + a, 4);
            return __builtin_bswap32(x);
        }
        return vm_read32_mm(&m, a);
    }
    uint64_t fast64(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 8), 1)) {
            uint64_t x;
            std::memcpy(&x, m.vb + a, 8);
            return __builtin_bswap64(x);
        }
        return vm_read64_mm(&m, a);
    }
    uint8_t fast8(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 1), 1)) return m.vb[a];
        return vm_read8_mm(&m, a);
    }
    void w32(uint32_t a, uint32_t value) { vm_write32_mm(&m, a, value); }
    void fast_w32(uint32_t a, uint32_t value)
    {
        if (__builtin_expect(plain_store(m, a, 4), 1)) {
            const uint32_t be = __builtin_bswap32(value);
            memcpy(m.vb + a, &be, 4);
            vm_track_write(a, 4);
            return;
        }
        vm_write32_mm(&m, a, value);
    }

    static inline __m128i bswap32_mask()
    {
        return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    }

    __m128 vec4(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 16), 1))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask()));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), (int)r32(a + 12)));
    }

    __m128 vec3(uint32_t a) const
    {
        if (__builtin_expect(vm_mm_ok(&m, a, 16), 1))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), bswap32_mask()));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), 0));
    }

    uint32_t peek(uint32_t a) const
    {
        uint32_t x = 0;
        if (vm_mm_ok(&m, a, 4)) std::memcpy(&x, m.vb + a, 4);
        return __builtin_bswap32(x);
    }
    void touch(uint32_t a) const { _mm_prefetch((const char*)m.vb + a, _MM_HINT_T0); }

    void reload()
    {
        tag = fast32(k_tag);
        stack = fast32(query + 0x10);
        ignore = fast32(query + 0x3C);
        flags = fast32(query + 0x2C);
        extra = fast32(query + 0x4C);
        lo = vec3(query + 0xA4);
        hi = vec3(query + 0xB0);
    }

    void ensure_frame()
    {
        if (!framed) {
            vm_write64_mm(&m, sp + 0x10, lr);
            vm_write64_mm(&m, call_sp, sp);
            framed = true;
        }
    }

    void call(uint32_t ret_lr, void (*fn)(ppu_context*))
    {
        ensure_frame();
        ctx->gpr[1] = call_sp;
        ctx->lr = ret_lr;
        run(fn, ctx);
        ctx->gpr[1] = sp;
        reload();
        called = true;
    }

    uint32_t allocate(uint32_t s)
    {
        const uint64_t top_end = r64(s);
        const uint32_t result = ((uint32_t)(top_end >> 32) + 7u) & ~7u;
        if (result + k_result_size <= (uint32_t)top_end) return result;
        ctx->gpr[3] = s;
        ctx->gpr[4] = k_chunk_request;
        call(0x00446D24, func_000B902C);
        return (r32(s) + 7u) & ~7u;
    }

    void test_prim(uint32_t prim)
    {
        const uint32_t actor = fast32(prim + 0x4C);
        if (!actor || fast32(prim + 0x58) == tag || fast32(actor + 0x18C) == tag) return;
        w32(prim + 0x58, tag);
        __m128 p_center, p_ext;
        if (__builtin_expect(vm_mm_ok(&m, prim + 0x5C, 28), 1)) {
            const __m128i sh = bswap32_mask();
            p_center = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + prim + 0x5C)), sh));
            p_ext = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + prim + 0x68)), sh));
        } else {
            p_center = vec3(prim + 0x5C);
            p_ext = vec3(prim + 0x68);
        }
        const __m128 p_lo = _mm_sub_ps(p_center, p_ext);
        const __m128 p_hi = _mm_add_ps(p_center, p_ext);
        const int sep = (_mm_movemask_ps(_mm_cmpgt_ps(p_lo, hi)) | _mm_movemask_ps(_mm_cmpgt_ps(lo, p_hi))) & 7;
        if (sep) return;
        // Inlined func_001B7848(prim): (prim->Flags & 0x40000000) && (actor->Flags & 0x10)
        if ((fast32(prim + 0x12C) & 0x40000000u) == 0) return;
        if ((fast32(actor + 0xB0) & 0x00000010u) == 0) return;
        for (uint32_t a = actor; a; a = fast32(a + 0x9C))
            if (a == ignore) return;

        ctx->gpr[3] = actor;
        ctx->gpr[4] = prim;
        ctx->gpr[5] = ignore;
        ctx->gpr[6] = flags;
        ctx->ctr = fast32(fast32(fast32(actor) + 0x1B0));
        call(0x00446BB0, guest_icall);
        if ((int32_t)ctx->gpr[3] == 0) return;

        if (fast8(ignore + 0x90) == 7 && ((fast32(actor + 0xAC) >> 24) & 1u) != 0) return;

        const uint32_t hit_ea = call_sp + 0xA0;
        if (vm_mm_st_ok(&m, hit_ea, k_result_size)) {
            uint32_t init[k_result_size / 4] = {};
            init[8] = __builtin_bswap32(0x3F800000u); // Time = 1.0f at +0x20
            init[9] = ~0u;
            init[17] = ~0u;
            std::memcpy(m.vb + hit_ea, init, k_result_size);
            vm_track_write(hit_ea, k_result_size);
        } else {
            for (uint32_t off = 0; off < k_result_size; off += 4)
                w32(hit_ea + off, off == 0x20 ? 0x3F800000u : (off == 0x24 || off == 0x44) ? 0xFFFFFFFFu : 0u);
        }

        ctx->gpr[3] = ignore;
        ctx->gpr[4] = actor;
        ctx->gpr[5] = hit_ea;
        ctx->gpr[6] = prim;
        ctx->gpr[7] = extra;
        ctx->ctr = fast32(fast32(fast32(ignore) + 0x1B4));
        call(0x00446C54, guest_icall);
        if ((int32_t)ctx->gpr[3] == 0) return;

        w32(hit_ea + 4, actor);
        const uint32_t s = stack;
        const uint32_t res = allocate(s);
        w32(s, res + k_result_size);
        if (res) {
            if (vm_mm_ok(&m, hit_ea, k_result_size) && vm_mm_st_ok(&m, res, k_result_size)) {
                std::memcpy(m.vb + res, m.vb + hit_ea, k_result_size);
                vm_track_write(res, k_result_size);
            } else {
                for (uint32_t off = 0; off < k_result_size; off += 4) w32(res + off, r32(hit_ea + off));
            }
        }
        w32(res, r32(query + 0xC));
        w32(query + 0xC, res);
        w32(actor + 0x18C, tag);
    }

    void node(uint32_t n, __m128 b)
    {
        if (const uint32_t kids = peek(n + 0xC)) {
            touch(kids);
            touch(kids + 0x40);
        }
        called = false;
        uint64_t head = fast64(n);
        uint32_t list = (uint32_t)(head >> 32);
        const int32_t count = (int32_t)(uint32_t)head;
        for (int32_t i = 0; i < count; i++) {
            if (i + 6 < count) {
                const uint32_t p = peek(list + 4u * (uint32_t)(i + 6));
                touch(p + 0x4C);
                touch(p + 0x5C);
            }
            test_prim(fast32(list + 4u * (uint32_t)i));
            if (__builtin_expect(called, 0)) {
                called = false;
                head = fast64(n);
                list = (uint32_t)(head >> 32);
            }
        }
        uint32_t kids = fast32(n + 0xC);
        if (!kids) return;
        const int max_gt = _mm_movemask_ps(_mm_cmpgt_ps(hi, b));
        const int min_le = _mm_movemask_ps(_mm_cmple_ps(lo, b));
        const int min_ngt = _mm_movemask_ps(_mm_cmpngt_ps(lo, b));
        uint32_t sel = 0;
        if (max_gt & 1) {
            if (max_gt & 2) sel |= (max_gt & 4 ? 0x80u : 0) | (min_ngt & 4 ? 0x40u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x20u : 0) | (min_le & 4 ? 0x10u : 0);
        }
        if (min_ngt & 1) {
            if (max_gt & 2) sel |= (max_gt & 4 ? 0x08u : 0) | (min_le & 4 ? 0x04u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x02u : 0) | (min_ngt & 4 ? 0x01u : 0);
        }
        if (!sel) return;
        const __m128 half = _mm_mul_ps(_mm_shuffle_ps(b, b, 0xFF), _mm_set1_ps(0.5f));
        while (sel) {
            const uint32_t h = 31u - (uint32_t)__builtin_clz(sel);
            sel ^= 1u << h;
            const __m128 child_b = _mm_blend_ps(_mm_fmadd_ps(_mm_load_ps(k_signs[h]), half, b), half, 8);
            node(kids + 16u * h, child_b);
            if (__builtin_expect(called, 0)) {
                called = false;
                kids = fast32(n + 0xC);
            }
        }
    }
};

}

void func_00446A38(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t root = (uint32_t)ctx->gpr[3], query = (uint32_t)ctx->gpr[4], bounds_ea = (uint32_t)ctx->gpr[5];
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    OverlapSearch s{m, ctx, query, sp, lr, (uint32_t)sp - 0x150u};
    s.reload();
    s.node(root, s.vec4(bounds_ea));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_005B0EB0(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t self = (uint32_t)ctx->gpr[3], result = (uint32_t)ctx->gpr[4];
    const uint32_t end = (uint32_t)ctx->gpr[6], start = (uint32_t)ctx->gpr[7], extent = (uint32_t)ctx->gpr[8];
    if (vm_read32_mm(&m, self + 0x1C) == 0) {
        ctx->gpr[3] = 1;
        return;
    }
    const int32_t num_planes = (int32_t)vm_read32_mm(&m, self + 0x40);
    if (num_planes == 0) {
        ctx->gpr[3] = 1;
        return;
    }
    if (from_bits(vm_read32_mm(&m, extent)) != 0.0f || from_bits(vm_read32_mm(&m, extent + 4)) != 0.0f ||
        from_bits(vm_read32_mm(&m, extent + 8)) != 0.0f) {
        return func_005B0EB0_lifted(ctx);
    }
    const float sx = from_bits(vm_read32_mm(&m, start)), sy = from_bits(vm_read32_mm(&m, start + 4)),
                sz = from_bits(vm_read32_mm(&m, start + 8));
    const float dx = from_bits(vm_read32_mm(&m, end)) - sx;
    const float dy = from_bits(vm_read32_mm(&m, end + 4)) - sy;
    const float dz = from_bits(vm_read32_mm(&m, end + 8)) - sz;
    float t0 = -3.3999999521443642e+38f;
    float t1 = 3.3999999521443642e+38f;
    float nx_hit = 0.0f, ny_hit = 0.0f, nz_hit = 0.0f;
    constexpr double k_eps = 1e-8;
    const uint32_t planes = vm_read32_mm(&m, self + 0x3C);
    const __m128i shuf = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i shuf2 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                           3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    auto step_plane = [&](const float pl[4]) -> bool {
        const float start_dist = pl[0] * sx + pl[1] * sy + pl[2] * sz - pl[3];
        const float dir_dot = pl[0] * dx + pl[1] * dy + pl[2] * dz;
        if (std::fabs((double)dir_dot) < k_eps) return start_dist <= 0.0f;
        const float t = -start_dist / dir_dot;
        if (dir_dot > 0.0f) {
            if (t < t1) t1 = t;
        } else if (t > t0) {
            t0 = t;
            nx_hit = pl[0];
            ny_hit = pl[1];
            nz_hit = pl[2];
        }
        return !(t1 < 0.0f || t1 < t0);
    };
    const __m128 vsx = _mm_set1_ps(sx), vsy = _mm_set1_ps(sy), vsz = _mm_set1_ps(sz);
    const __m128 vdx = _mm_set1_ps(dx), vdy = _mm_set1_ps(dy), vdz = _mm_set1_ps(dz);
    const __m128 vabs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 veps = _mm_set1_ps((float)k_eps);
    const __m128 vzero = _mm_setzero_ps();
    int32_t i = 0;
    for (; i + 3 < num_planes; i += 4) {
        const uint32_t p = planes + 16u * (uint32_t)i;
        if (__builtin_expect(vm_mm_ok(&m, p, 64), 1)) {
            const __m256 p01 = _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(m.vb + p)), shuf2));
            const __m256 p23 = _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(m.vb + p + 32)), shuf2));
            __m128 r0 = _mm256_castps256_ps128(p01);
            __m128 r1 = _mm256_extractf128_ps(p01, 1);
            __m128 r2 = _mm256_castps256_ps128(p23);
            __m128 r3 = _mm256_extractf128_ps(p23, 1);
            _MM_TRANSPOSE4_PS(r0, r1, r2, r3); // r0=px, r1=py, r2=pz, r3=pw
            const __m128 sdist = _mm_sub_ps(_mm_fmadd_ps(r0, vsx, _mm_fmadd_ps(r1, vsy, _mm_mul_ps(r2, vsz))), r3);
            const __m128 ddot = _mm_fmadd_ps(r0, vdx, _mm_fmadd_ps(r1, vdy, _mm_mul_ps(r2, vdz)));
            const __m128 par = _mm_cmplt_ps(_mm_and_ps(ddot, vabs_mask), veps);
            const int par_mask = _mm_movemask_ps(par);
            if (__builtin_expect(par_mask != 0, 0)) {
                const int out_mask = _mm_movemask_ps(_mm_cmpgt_ps(sdist, vzero));
                if (par_mask & out_mask) {
                    ctx->gpr[3] = 1;
                    return;
                }
            }
            const __m128 vt = _mm_div_ps(_mm_sub_ps(vzero, sdist), ddot);
            const __m128 pos = _mm_andnot_ps(par, _mm_cmpgt_ps(ddot, vzero));
            const __m128 neg = _mm_andnot_ps(par, _mm_cmple_ps(ddot, vzero));
            const __m128 thi = _mm_blendv_ps(_mm_set1_ps(t1), vt, pos);
            const __m128 m1 = _mm_min_ps(thi, _mm_shuffle_ps(thi, thi, _MM_SHUFFLE(1, 0, 3, 2)));
            t1 = _mm_cvtss_f32(_mm_min_ps(m1, _mm_shuffle_ps(m1, m1, _MM_SHUFFLE(2, 3, 0, 1))));
            const __m128 vt0 = _mm_set1_ps(t0);
            const __m128 tlo = _mm_blendv_ps(vt0, vt, neg);
            const __m128 n1 = _mm_max_ps(tlo, _mm_shuffle_ps(tlo, tlo, _MM_SHUFFLE(1, 0, 3, 2)));
            const __m128 vmax_lo = _mm_max_ps(n1, _mm_shuffle_ps(n1, n1, _MM_SHUFFLE(2, 3, 0, 1)));
            const float new_t0 = _mm_cvtss_f32(vmax_lo);
            if (__builtin_expect(new_t0 > t0, 0)) {
                const int wm = _mm_movemask_ps(_mm_and_ps(neg, _mm_cmpeq_ps(vt, vmax_lo)));
                if (wm) {
                    const int lane = __builtin_ctz((unsigned)wm);
                    t0 = new_t0;
                    alignas(16) float nx4[4], ny4[4], nz4[4];
                    _mm_store_ps(nx4, r0);
                    _mm_store_ps(ny4, r1);
                    _mm_store_ps(nz4, r2);
                    nx_hit = nx4[lane];
                    ny_hit = ny4[lane];
                    nz_hit = nz4[lane];
                }
            }
            if (t1 < 0.0f || t1 < t0) {
                ctx->gpr[3] = 1;
                return;
            }
            continue;
        }
        for (int k = 0; k < 4; ++k) {
            const uint32_t pk = p + 16u * (uint32_t)k;
            const float q[4] = {from_bits(vm_read32_mm(&m, pk)), from_bits(vm_read32_mm(&m, pk + 4)),
                                from_bits(vm_read32_mm(&m, pk + 8)), from_bits(vm_read32_mm(&m, pk + 12))};
            if (!step_plane(q)) {
                ctx->gpr[3] = 1;
                return;
            }
        }
    }
    for (; i + 1 < num_planes; i += 2) {
        const uint32_t p = planes + 16u * (uint32_t)i;
        alignas(32) float pl[8];
        if (vm_mm_ok(&m, p, 32)) {
            _mm256_store_ps(pl, _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(m.vb + p)), shuf2)));
            if (!step_plane(pl) || !step_plane(pl + 4)) {
                ctx->gpr[3] = 1;
                return;
            }
            continue;
        }
        for (int k = 0; k < 2; ++k) {
            const uint32_t pk = p + 16u * (uint32_t)k;
            const float q[4] = {from_bits(vm_read32_mm(&m, pk)), from_bits(vm_read32_mm(&m, pk + 4)),
                                from_bits(vm_read32_mm(&m, pk + 8)), from_bits(vm_read32_mm(&m, pk + 12))};
            if (!step_plane(q)) {
                ctx->gpr[3] = 1;
                return;
            }
        }
    }
    if (i < num_planes) {
        const uint32_t p = planes + 16u * (uint32_t)i;
        alignas(16) float pl[4];
        if (vm_mm_ok(&m, p, 16)) {
            _mm_store_ps(pl, _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + p)), shuf)));
        } else {
            pl[0] = from_bits(vm_read32_mm(&m, p));
            pl[1] = from_bits(vm_read32_mm(&m, p + 4));
            pl[2] = from_bits(vm_read32_mm(&m, p + 8));
            pl[3] = from_bits(vm_read32_mm(&m, p + 12));
        }
        if (!step_plane(pl)) {
            ctx->gpr[3] = 1;
            return;
        }
    }
    const float best = from_bits(vm_read32_mm(&m, result + 0x20));
    if (t0 > best) {
        ctx->gpr[3] = 1;
        return;
    }
    if (t0 < 0.0f) {
        vm_write32_mm(&m, result + 0x20, 0);
        const float len2 = dx * dx + dy * dy + dz * dz;
        float ix = dx, iy = dy, iz = dz;
        if (len2 > 0.0f) {
            const float inv = 1.0f / std::sqrt(len2);
            ix = dx * inv;
            iy = dy * inv;
            iz = dz * inv;
        }
        if ((double)(ix * ix + iy * iy + iz * iz) >= k_eps) {
            vm_write32_mm(&m, result + 0x14, to_bits(-ix));
            vm_write32_mm(&m, result + 0x18, to_bits(-iy));
            vm_write32_mm(&m, result + 0x1C, to_bits(-iz));
        } else {
            vm_write32_mm(&m, result + 0x14, 0);
            vm_write32_mm(&m, result + 0x18, 0);
            vm_write32_mm(&m, result + 0x1C, 0x3F800000u);
        }
    } else {
        vm_write32_mm(&m, result + 0x20, to_bits(t0));
        vm_write32_mm(&m, result + 0x14, to_bits(nx_hit));
        vm_write32_mm(&m, result + 0x18, to_bits(ny_hit));
        vm_write32_mm(&m, result + 0x1C, to_bits(nz_hit));
    }
    ctx->gpr[3] = 0;
}
