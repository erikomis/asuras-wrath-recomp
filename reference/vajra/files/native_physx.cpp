/*
 * Native C++ PPU overrides for hot PhysX / UE3 P1 main & P40 PPU PhysX functions.
 *
 * Overrides:
 *   - func_00AFB230 (P40 PPU PhysX joint/constraint update dispatch)
 *   - func_00B4A738 (P40 PPU PhysX kinematic target velocity solver)
 *   - func_00531F20 (P1 main USkeletalMeshComponent::ComposeSkeleton)
 *   - func_00392F08 (P1 main UWorld::MultiLineCheck wrapper)
 */
#include "guest.h"

#include <cstring>
#include <immintrin.h>
#include <math.h>
#include <stdint.h>

extern "C" uint64_t ppu_timebase_now(void);

namespace {

static inline float rd_f32(uint32_t ea) {
    return from_bits(vm_read32(ea));
}

static inline void wr_f32(uint32_t ea, float f) {
    vm_write32(ea, to_bits(f));
}

static inline void call_vfn_toc(ppu_context* ctx, uint32_t obj_ea, uint32_t vtbl_off, uint32_t lr_val) {
    uint32_t vtbl = vm_read32(obj_ea);
    uint32_t opd  = vm_read32(vtbl + vtbl_off);
    ctx->ctr    = vm_read32(opd);
    ctx->gpr[2] = vm_read32(opd + 4);
    ctx->lr     = lr_val;
    guest_icall(ctx);
    ctx->gpr[2] = 0x019AF8F8ULL;
}

static inline uint32_t fast32(const ppu_mm& m, uint32_t a) {
    if (__builtin_expect(vm_mm_ok(&m, a, 4), 1)) {
        uint32_t x;
        std::memcpy(&x, m.vb + a, 4);
        return __builtin_bswap32(x);
    }
    return vm_read32_mm(&m, a);
}

static inline uint8_t fast8(const ppu_mm& m, uint32_t a) {
    if (__builtin_expect(vm_mm_ok(&m, a, 1), 1)) return m.vb[a];
    return vm_read8_mm(&m, a);
}

static inline void fast_w32(const ppu_mm& m, uint32_t a, uint32_t v) {
    if (__builtin_expect(plain_store(m, a, 4), 1)) {
        const uint32_t be = __builtin_bswap32(v);
        std::memcpy(m.vb + a, &be, 4);
        vm_track_write(a, 4);
        return;
    }
    vm_write32_mm(&m, a, v);
}

static inline float rd_f32_mm(const ppu_mm& m, uint32_t ea) {
    return from_bits(fast32(m, ea));
}

static inline void wr_f32_mm(const ppu_mm& m, uint32_t ea, float f) {
    fast_w32(m, ea, to_bits(f));
}

static inline void call_vfn_toc_mm(ppu_context* ctx, const ppu_mm& m, uint32_t obj_ea, uint32_t vtbl_off, uint32_t lr_val) {
    uint32_t vtbl = fast32(m, obj_ea);
    uint32_t opd  = fast32(m, vtbl + vtbl_off);
    ctx->ctr    = fast32(m, opd);
    ctx->gpr[2] = fast32(m, opd + 4);
    ctx->lr     = lr_val;
    guest_icall(ctx);
    ctx->gpr[2] = 0x019AF8F8ULL;
}

static inline void copy_bytes_mm(const ppu_mm& m, uint32_t dst, uint32_t src, uint32_t n) {
    if (__builtin_expect(vm_mm_ok(&m, src, n) && plain_store(m, dst, n), 1)) {
        std::memcpy(m.vb + dst, m.vb + src, n);
        vm_track_write(dst, n);
    } else {
        ppu_store_block(dst, m.vb + src, n);
    }
}

static inline __m128i bswap16(__m128i v) {
    return _mm_shuffle_epi8(v, _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}

static inline __m128i ld_vmx_rev(const ppu_mm& m, uint32_t ea) {
    return bswap16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(m.vb + (ea & ~0xFu))));
}

static inline void st_vmx_rev_fast(const ppu_mm& m, uint32_t ea, __m128i v) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(m.vb + (ea & ~0xFu)), bswap16(v));
}

static inline void st_vmx_rev(const ppu_mm& m, uint32_t ea, __m128i v) {
    ea &= ~0xFu;
    __m128i be = bswap16(v);
    if (__builtin_expect(plain_store(m, ea, 16), 1)) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(m.vb + ea), be);
        vm_track_write(ea, 16);
    } else {
        uint8_t buf[16];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(buf), be);
        ppu_store_block(ea, buf, 16);
    }
}

static inline void copy32_aligned(const ppu_mm& m, uint32_t dst_ea, uint32_t src_ea) {
    dst_ea &= ~0xFu;
    src_ea &= ~0xFu;
    if (__builtin_expect(plain_store(m, dst_ea, 32), 1)) {
        std::memcpy(m.vb + dst_ea, m.vb + src_ea, 32);
        vm_track_write(dst_ea, 32);
    } else {
        ppu_store_block(dst_ea, m.vb + src_ea, 32);
    }
}

static inline __m128i vmx_perm_mask_from_rev(__m128i idx_rev) {
    return _mm_xor_si128(_mm_and_si128(idx_rev, _mm_set1_epi8(15)), _mm_set1_epi8(15));
}

static inline __m128 vmx_perm_self(__m128 v, __m128i pshuf) {
    return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(v), pshuf));
}

static inline void rot_vec_by_quat(float qx, float qy, float qz, float qw,
                                   float vx, float vy, float vz,
                                   float& ox, float& oy, float& oz) {
    const float w2 = qw * qw - 0.5f;
    const float cx = qy * vz - qz * vy;
    const float cy = qz * vx - qx * vz;
    const float cz = qx * vy - qy * vx;
    const float d  = qx * vx + qy * vy + qz * vz;
    const float rx = w2 * vx + qw * cx + qx * d;
    const float ry = w2 * vy + qw * cy + qy * d;
    const float rz = w2 * vz + qw * cz + qz * d;
    ox = rx + rx;
    oy = ry + ry;
    oz = rz + rz;
}

static inline void init_check_result(uint8_t* p) {
    std::memset(p + 4, 0, 0x48);
    const uint32_t neg1 = 0xFFFFFFFFu;
    std::memcpy(p + 0x24, &neg1, 4);
    std::memcpy(p + 0x44, &neg1, 4);
}

} // namespace

/* ============================================================================
 * func_00AFB230 (ppu_recomp_104.cpp:4917)
 * P40 PPU PhysX: joint/constraint update dispatch (10.8% of P40 PPU PhysX).
 * ============================================================================ */
void func_00AFB230(ppu_context* ctx) {
    const ppu_mm m     = ppu_mm_take();
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const double dt    = ctx->fpr[1];

    uint32_t b0 = fast32(m, obj + 0x128);
    if (!(b0 != 0 && (fast32(m, b0 + 0x148) & 0x80u) == 0)) {
        uint32_t b1 = fast32(m, obj + 0x12C);
        if (b1 == 0) {
            ctx->gpr[3] = obj;
            call_vfn_toc_mm(ctx, m, obj, 0x1E0, 0x00AFB328);
            return;
        }
        ctx->gpr[3] = b1;
        call_vfn_toc_mm(ctx, m, b1, 0x78, 0x00AFB2F8);
        if ((ctx->gpr[3] & 0x80u) != 0) {
            ctx->gpr[3] = obj;
            call_vfn_toc_mm(ctx, m, obj, 0x1E0, 0x00AFB328);
            return;
        }
    }

    if (((fast32(m, obj + 0x20) >> 3) & 3u) == 2u) {
        ctx->gpr[3] = obj;
        call_vfn_toc_mm(ctx, m, obj, 0x1E0, 0x00AFB328);
        return;
    }

    int dirty = 0;
    uint32_t r9 = fast32(m, obj + 0x128);
    if (r9 != 0 && fast32(m, r9 + 0x168) != fast32(m, obj + 0x10C)) {
        dirty = 1;
        ctx->gpr[3] = obj;
        ctx->gpr[4] = 0;
        ctx->lr = 0x00AFB2C0;
        run(func_00AF9F58, ctx);
    }

    r9 = fast32(m, obj + 0x12C);
    if (r9 != 0 && fast32(m, r9 + 0x168) != fast32(m, obj + 0x110)) {
        dirty++;
        ctx->gpr[3] = obj;
        ctx->gpr[4] = 1;
        ctx->lr = 0x00AFB38C;
        run(func_00AF9F58, ctx);
    }

    if (dirty != 0) {
        ctx->gpr[3] = obj;
        call_vfn_toc_mm(ctx, m, obj, 0x1DC, 0x00AFB460);
    }

    uint32_t flags = fast32(m, obj + 0x20);
    fast_w32(m, obj + 0x20, (flags & ~0x18u) | 0x8u);
    if (__builtin_expect(plain_store(m, obj + 0x114, 12), 1)) {
        std::memset(m.vb + obj + 0x114, 0, 12);
        vm_track_write(obj + 0x114, 12);
    } else {
        vm_write32_mm(&m, obj + 0x114, 0);
        vm_write32_mm(&m, obj + 0x118, 0);
        vm_write32_mm(&m, obj + 0x11C, 0);
    }

    if (fast32(m, obj + 0x14) != 0) {
        ctx->gpr[3] = obj;
        ctx->fpr[1] = dt;
        call_vfn_toc_mm(ctx, m, obj, 0x1D4, 0x00AFB3F0);
    }

    ctx->gpr[3] = obj;
    ctx->fpr[1] = dt;
    call_vfn_toc_mm(ctx, m, obj, 0x1CC, 0x00AFB418);
}

void func_00B8CA04(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint32_t r29 = (uint32_t)ctx->gpr[3];
    const uint32_t kind = (uint32_t)ctx->gpr[4];
    const uint32_t dst = (uint32_t)ctx->gpr[5];
    vm_write64_mm(&m, sp - 0x90, sp);
    ctx->gpr[1] = sp - 0x90;
    ctx->lr = 0x00B8CA2C;
    run(func_00B876DC, ctx);
    const uint32_t mgr = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = mgr;
    ctx->gpr[4] = r29;
    call_vfn_toc_mm(ctx, m, mgr, 0x38, 0x00B8CA58);
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    switch (kind) {
    case 0:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = dst + 4u;
        ctx->gpr[5] = dst;
        call_vfn_toc_mm(ctx, m, obj, 0x28, 0x00B8CADC);
        break;
    case 2:
        ctx->gpr[3] = obj;
        call_vfn_toc_mm(ctx, m, obj, 0x08, 0x00B8CC20);
        copy_bytes_mm(m, dst, (uint32_t)ctx->gpr[3], 24u);
        break;
    case 3:
        ctx->gpr[3] = obj;
        call_vfn_toc_mm(ctx, m, obj, 0x10, 0x00B8CC20);
        copy_bytes_mm(m, dst, (uint32_t)ctx->gpr[3], 24u);
        break;
    case 4:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = dst;
        ctx->gpr[5] = dst + 4u;
        call_vfn_toc_mm(ctx, m, obj, 0x18, 0x00B8CADC);
        break;
    case 5:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = dst;
        ctx->gpr[5] = dst + 4u;
        call_vfn_toc_mm(ctx, m, obj, 0x20, 0x00B8CADC);
        break;
    case 10:
        ctx->gpr[3] = obj;
        call_vfn_toc_mm(ctx, m, obj, 0x00, 0x00B8CB20);
        copy_bytes_mm(m, dst, (uint32_t)ctx->gpr[3], 28u);
        ctx->gpr[3] = obj;
        call_vfn_toc_mm(ctx, m, obj, 0x08, 0x00B8CB80);
        copy_bytes_mm(m, dst + 0x1Cu, (uint32_t)ctx->gpr[3], 24u);
        break;
    default:
        break;
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_00B8CC88(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint32_t r29 = (uint32_t)ctx->gpr[3];
    const uint32_t kind = (uint32_t)ctx->gpr[4];
    const uint32_t src = (uint32_t)ctx->gpr[5];
    vm_write64_mm(&m, sp - 0x90, sp);
    ctx->gpr[1] = sp - 0x90;
    ctx->lr = 0x00B8CCB0;
    run(func_00B876DC, ctx);
    const uint32_t mgr = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = mgr;
    ctx->gpr[4] = r29;
    call_vfn_toc_mm(ctx, m, mgr, 0x38, 0x00B8CCDC);
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    switch (kind) {
    case 0:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = src + 4u;
        ctx->fpr[1] = rd_f32_mm(m, src);
        call_vfn_toc_mm(ctx, m, obj, 0x2C, 0x00B8CDAC);
        break;
    case 2:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = src;
        call_vfn_toc_mm(ctx, m, obj, 0x0C, 0x00B8CE54);
        break;
    case 3:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = src;
        call_vfn_toc_mm(ctx, m, obj, 0x14, 0x00B8CE54);
        break;
    case 4:
        ctx->gpr[3] = obj;
        ctx->fpr[1] = rd_f32_mm(m, src);
        ctx->fpr[2] = rd_f32_mm(m, src + 4u);
        call_vfn_toc_mm(ctx, m, obj, 0x1C, 0x00B8CD5C);
        break;
    case 5:
        ctx->gpr[3] = obj;
        ctx->fpr[1] = rd_f32_mm(m, src);
        ctx->fpr[2] = rd_f32_mm(m, src + 4u);
        call_vfn_toc_mm(ctx, m, obj, 0x24, 0x00B8CD5C);
        break;
    case 10:
        ctx->gpr[3] = obj;
        ctx->gpr[4] = src;
        call_vfn_toc_mm(ctx, m, obj, 0x04, 0x00B8CDDC);
        ctx->gpr[3] = obj;
        ctx->gpr[4] = src + 0x1Cu;
        call_vfn_toc_mm(ctx, m, obj, 0x0C, 0x00B8CE08);
        break;
    default:
        break;
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

/* ============================================================================
 * func_00B4A738 (ppu_recomp_106.cpp:13440)
 * P40 PPU PhysX: kinematic target velocity solver (4.4% of P40 PPU PhysX).
 * ============================================================================ */
void func_00B4A738(ppu_context* ctx) {
    const ppu_mm m     = ppu_mm_take();
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const float inv_dt = (float)ctx->fpr[2];

    if ((vm_read32_mm(&m, obj + 0x148) & 0x80u) == 0)
        return;

    const uint32_t scene = vm_read32_mm(&m, obj + 0x144);
    if (vm_read32_mm(&m, scene + 0x90) != 0)
        return;

    uint32_t kin = vm_read32_mm(&m, obj + 0x260);
    uint32_t kflags = vm_read32_mm(&m, kin + 0x0C);

    if (kflags == 0) {
        uint32_t f14c = vm_read32_mm(&m, obj + 0x14C);
        if (f14c & 0x20000u) {
            f14c &= ~0x20000u;
            vm_write32_mm(&m, obj + 0x14C, f14c);
        } else if (vm_read8_mm(&m, obj + 0x54) != 0) {
            ctx->gpr[3] = obj + 4;
            ctx->gpr[4] = 0;
            ctx->lr = 0x00B4AB20;
            run(func_00B90F90, ctx);
            f14c = vm_read32_mm(&m, obj + 0x14C);
        }
        wr_f32_mm(m, obj + 0x198, 0.0f);
        if ((f14c & 0x8000u) == 0) {
            ctx->gpr[3] = vm_read32_mm(&m, obj + 0x144);
            ctx->gpr[4] = obj;
            ctx->lr = 0x00B4AC20;
            run(func_00ACB050, ctx);
        }
        if (__builtin_expect(plain_store(m, obj + 0xCC, 25), 1)) {
            std::memset(m.vb + obj + 0xCC, 0, 24);
            m.vb[obj + 0xE4] = 1;
            vm_track_write(obj + 0xCC, 25);
        } else {
            vm_write8_mm(&m, obj + 0xE4, 1);
            wr_f32_mm(m, obj + 0xD8, 0.0f);
            wr_f32_mm(m, obj + 0xDC, 0.0f);
            wr_f32_mm(m, obj + 0xE0, 0.0f);
            wr_f32_mm(m, obj + 0xCC, 0.0f);
            wr_f32_mm(m, obj + 0xD0, 0.0f);
            wr_f32_mm(m, obj + 0xD4, 0.0f);
        }
        return;
    }

    const float scale = rd_f32_mm(m, scene + 0x84);
    float lin_x = 0.0f, lin_y = 0.0f, lin_z = 0.0f;

    if (kflags & 0x2u) {
        const float lx = rd_f32_mm(m, obj + 0x108);
        const float ly = rd_f32_mm(m, obj + 0x10C);
        const float lz = rd_f32_mm(m, obj + 0x110);
        const float qx = rd_f32_mm(m, kin + 0x10);
        const float qy = rd_f32_mm(m, kin + 0x14);
        const float qz = rd_f32_mm(m, kin + 0x18);
        const float qw = rd_f32_mm(m, kin + 0x1C);
        float rx, ry, rz;
        rot_vec_by_quat(qx, qy, qz, qw, lx, ly, lz, rx, ry, rz);

        float tx, ty, tz;
        if (kflags & 0x1u) {
            tx = rx + rd_f32_mm(m, kin + 0x00);
            ty = ry + rd_f32_mm(m, kin + 0x04);
            tz = rz + rd_f32_mm(m, kin + 0x08);
        } else {
            tx = rx + rd_f32_mm(m, obj + 0x1B8);
            ty = ry + rd_f32_mm(m, obj + 0x1BC);
            tz = rz + rd_f32_mm(m, obj + 0x1C0);
        }
        const float k = scale * inv_dt;
        lin_x = (tx - rd_f32_mm(m, obj + 0xB0)) * k;
        lin_y = (ty - rd_f32_mm(m, obj + 0xB4)) * k;
        lin_z = (tz - rd_f32_mm(m, obj + 0xB8)) * k;
    } else if (kflags & 0x1u) {
        const float lx = -rd_f32_mm(m, obj + 0x108);
        const float ly = -rd_f32_mm(m, obj + 0x10C);
        const float lz = -rd_f32_mm(m, obj + 0x110);
        const float qx = rd_f32_mm(m, obj + 0xBC);
        const float qy = rd_f32_mm(m, obj + 0xC0);
        const float qz = rd_f32_mm(m, obj + 0xC4);
        const float qw = rd_f32_mm(m, obj + 0xC8);
        float rx, ry, rz;
        rot_vec_by_quat(qx, qy, qz, qw, lx, ly, lz, rx, ry, rz);
        const float k = scale * inv_dt;
        lin_x = (rd_f32_mm(m, kin + 0x00) - (rx + rd_f32_mm(m, obj + 0xB0))) * k;
        lin_y = (rd_f32_mm(m, kin + 0x04) - (ry + rd_f32_mm(m, obj + 0xB4))) * k;
        lin_z = (rd_f32_mm(m, kin + 0x08) - (rz + rd_f32_mm(m, obj + 0xB8))) * k;
    }

    float ang_x = 0.0f, ang_y = 0.0f, ang_z = 0.0f;
    if (kflags & 0x2u) {
        const float ax = -rd_f32_mm(m, obj + 0x1C4);
        const float ay = -rd_f32_mm(m, obj + 0x1C8);
        const float az = -rd_f32_mm(m, obj + 0x1CC);
        const float aw =  rd_f32_mm(m, obj + 0x1D0);

        const float bx = rd_f32_mm(m, kin + 0x10);
        const float by = rd_f32_mm(m, kin + 0x14);
        const float bz = rd_f32_mm(m, kin + 0x18);
        const float bw = rd_f32_mm(m, kin + 0x1C);

        float rx = aw * bx + ax * bw + az * by - ay * bz;
        float ry = aw * by + ay * bw + ax * bz - az * bx;
        float rz = aw * bz + az * bw + ay * bx - ax * by;
        float rw = aw * bw - ax * bx - ay * by - az * bz;

        if (rw < 0.0f) {
            rx = -rx; ry = -ry; rz = -rz; rw = -rw;
        }
        const float len = sqrtf(rx * rx + ry * ry + rz * rz + rw * rw);
        if (len != 0.0f) {
            const float inv = 1.0f / len;
            rx *= inv; ry *= inv; rz *= inv; rw *= inv;
        }

        float angle, ax_x, ax_y, ax_z;
        {
            GuestFrame frame(ctx, 0xA0);
            const uint32_t sp = frame.base;
            wr_f32_mm(m, sp + 0x80, rx);
            wr_f32_mm(m, sp + 0x84, ry);
            wr_f32_mm(m, sp + 0x88, rz);
            wr_f32_mm(m, sp + 0x8C, rw);
            ctx->gpr[3] = sp + 0x80;
            ctx->gpr[4] = sp + 0x74;
            ctx->gpr[5] = sp + 0x70;
            ctx->lr = 0x00B4A990;
            run(func_00B49830, ctx);
            angle = rd_f32_mm(m, sp + 0x70);
            ax_x  = rd_f32_mm(m, sp + 0x74);
            ax_y  = rd_f32_mm(m, sp + 0x78);
            ax_z  = rd_f32_mm(m, sp + 0x7C);
        }

        kin = vm_read32_mm(&m, obj + 0x260);
        const float k = scale * inv_dt;
        ang_x = (angle * ax_x) * k;
        ang_y = (angle * ax_y) * k;
        ang_z = (angle * ax_z) * k;
    }

    vm_write32_mm(&m, kin + 0x0C, 0);
    if (vm_read8_mm(&m, obj + 0x54) == 0) {
        ctx->gpr[3] = obj + 4;
        ctx->gpr[4] = 1;
        ctx->lr = 0x00B4AC44;
        run(func_00B90F90, ctx);
    }

    uint32_t f14c = vm_read32_mm(&m, obj + 0x14C) | 0x20000u;
    vm_write32_mm(&m, obj + 0x14C, f14c);
    if ((f14c & 0x8000u) == 0) {
        ctx->gpr[3] = vm_read32_mm(&m, obj + 0x144);
        ctx->gpr[4] = obj;
        ctx->lr = 0x00B4AC34;
        run(func_00ACB050, ctx);
    }

    if (__builtin_expect(plain_store(m, obj + 0xCC, 25), 1)) {
        const uint32_t vels[6] = {
            __builtin_bswap32(to_bits(lin_x)), __builtin_bswap32(to_bits(lin_y)), __builtin_bswap32(to_bits(lin_z)),
            __builtin_bswap32(to_bits(ang_x)), __builtin_bswap32(to_bits(ang_y)), __builtin_bswap32(to_bits(ang_z))
        };
        std::memcpy(m.vb + obj + 0xCC, vels, 24);
        m.vb[obj + 0xE4] = 1;
        vm_track_write(obj + 0xCC, 25);
    } else {
        vm_write8_mm(&m, obj + 0xE4, 1);
        wr_f32_mm(m, obj + 0xD8, ang_x);
        wr_f32_mm(m, obj + 0xDC, ang_y);
        wr_f32_mm(m, obj + 0xE0, ang_z);
        wr_f32_mm(m, obj + 0xCC, lin_x);
        wr_f32_mm(m, obj + 0xD0, lin_y);
        wr_f32_mm(m, obj + 0xD4, lin_z);
    }
}

/* ============================================================================
 * func_00531F20 (ppu_recomp_050.cpp:6396)
 * P1 main: USkeletalMeshComponent::ComposeSkeleton (native SSE/FMA bone loop).
 * ============================================================================ */
void func_00531F20(ppu_context* ctx) {
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    if (vm_read32(obj + 0x1E4) == 0)
        return;

    GuestFrame frame(ctx, 0x1D0);
    const uint32_t sp = frame.base;

    if ((vm_read32(obj + 0x3BC) >> 17) & 1u) {
        ctx->gpr[3] = obj;
        ctx->lr = 0x00531F88;
        run(func_00531394, ctx);
        vm_write32(obj + 0x3BC, vm_read32(obj + 0x3BC) & ~(1u << 17));
    }

    uint32_t anim_node = vm_read32(obj + 0x1F0);
    uint32_t r30 = 0;
    if (anim_node != 0) {
        uint32_t cls = vm_read32(0x019C4094u);
        if (cls == 0) {
            ctx->gpr[3] = 0x01691EA8u;
            ctx->lr = 0x00531FC8;
            run(func_002735A8, ctx);
            vm_write32(0x019C4094u, (uint32_t)ctx->gpr[3]);
            ctx->lr = 0x00531FD4;
            run(func_00273468, ctx);
            cls = vm_read32(0x019C4094u);
        }
        if (cls == 0) {
            r30 = anim_node;
        } else {
            uint32_t c = vm_read32(anim_node + 0x34);
            while (c != 0) {
                if (c == cls) { r30 = anim_node; break; }
                c = vm_read32(c + 0x40);
            }
        }
    }

    const uint32_t buf_ea = sp + 0x70;
    ctx->gpr[3] = buf_ea;
    ctx->gpr[4] = 0;
    ctx->gpr[5] = vm_read32(obj + 0x260);
    ctx->lr = 0x00532014;
    run(func_00F35E2C, ctx);

    ctx->gpr[3] = vm_read32(0x019CCE80u);
    ctx->gpr[4] = 0;
    ctx->lr = 0x00532024;
    run(func_00582F38, ctx);

    const ppu_mm m = ppu_mm_take();
    const uint32_t r3a8 = vm_read32_mm(&m, obj + 0x3A8);
    const uint32_t r234 = vm_read32_mm(&m, obj + 0x234);
    const uint32_t r384 = vm_read32_mm(&m, obj + 0x384);
    const uint32_t local_atoms = vm_read32_mm(&m, obj + 0x25C);
    const uint32_t space_bases = vm_read32_mm(&m, obj + 0x250);
    const uint32_t g_flag = vm_read32_mm(&m, 0x019FED5Cu);

    uint32_t r29 = 0;
    if (g_flag != 0 && ((vm_read32_mm(&m, obj + 0x3BC) >> 19) & 1u))
        r29 = 1;

    uint32_t r3_flag = 0;
    if (g_flag != 0 && ((r3a8 >> 25) & 1u) != 0 && r29 != 0)
        r3_flag = 1;

    const uint32_t r28 = (r234 == 0) ? 1u : 0u;
    uint32_t r27 = 0;
    if (r30 != 0 && (r3_flag == 0 || r384 == 0))
        r27 = 1;

    // Preload read-only VMX constants once outside the bone loop
    const __m128  c_quat_conj = _mm_castsi128_ps(ld_vmx_rev(m, 0x01A63860u));
    const __m128  c_s1        = _mm_castsi128_ps(ld_vmx_rev(m, 0x01A63870u));
    const __m128  c_s2        = _mm_castsi128_ps(ld_vmx_rev(m, 0x01A63880u));
    const __m128  c_s3        = _mm_castsi128_ps(ld_vmx_rev(m, 0x01A63890u));
    const __m128i c_w_mask    = ld_vmx_rev(m, 0x01AA3810u);
    const __m128i p_perm1     = vmx_perm_mask_from_rev(ld_vmx_rev(m, 0x01AA46A0u));
    const __m128i p_perm2     = vmx_perm_mask_from_rev(ld_vmx_rev(m, 0x01AA4370u));
    const __m128i p_perm3     = vmx_perm_mask_from_rev(ld_vmx_rev(m, 0x01AA3D40u));

    const int32_t req_count = (int32_t)vm_read32_mm(&m, obj + 0x294);
    const uint32_t bone_cap = vm_read32_mm(&m, obj + 0x260);
    uint32_t req_arr = vm_read32_mm(&m, obj + 0x290);
    uint32_t vis_arr = vm_read32_mm(&m, obj + 0x5A8);
    uint32_t skel    = vm_read32_mm(&m, vm_read32_mm(&m, obj + 0x1E4) + 0x88);
    const bool buf_plain = bone_cap > 0 && plain_store(m, buf_ea, bone_cap);
    const bool sb_plain  = bone_cap > 0 && plain_store(m, space_bases & ~0xFu, bone_cap << 5);
    const bool la_plain  = bone_cap > 0 && plain_store(m, local_atoms & ~0xFu, bone_cap << 5);
    for (int32_t i = 0; i < req_count; ++i) {
        const uint32_t bone_idx = vm_read8_mm(&m, req_arr + (uint32_t)i);
        const uint32_t byte_off = bone_idx << 5;
        const uint32_t dst_ea   = space_bases + byte_off;

        if (__builtin_expect(buf_plain && bone_idx < bone_cap, 1))
            m.vb[buf_ea + bone_idx] = 1;
        else
            vm_write8_mm(&m, buf_ea + bone_idx, 1);

        const uint8_t vis = vm_read8_mm(&m, vis_arr + bone_idx);
        if (vis != 1) {
            const uint32_t la_ea = local_atoms + byte_off + 0x10u;
            __m128i v3 = ld_vmx_rev(m, la_ea);
            if (__builtin_expect(la_plain && bone_idx < bone_cap, 1))
                st_vmx_rev_fast(m, la_ea, _mm_andnot_si128(c_w_mask, v3));
            else
                st_vmx_rev(m, la_ea, _mm_andnot_si128(c_w_mask, v3));
        }

        if (bone_idx == 0) {
            copy32_aligned(m, space_bases, local_atoms);
        } else {
            const uint32_t parent_idx = vm_read32_mm(&m, skel + bone_idx * 0x50u + 0x40u);
            const uint32_t parent_ea  = space_bases + (parent_idx << 5);
            const uint32_t local_ea   = local_atoms + byte_off;

            const __m128 v2 = _mm_castsi128_ps(ld_vmx_rev(m, local_ea));
            const __m128 v3 = _mm_castsi128_ps(ld_vmx_rev(m, parent_ea));
            const __m128 v4 = _mm_castsi128_ps(ld_vmx_rev(m, local_ea + 0x10u));
            const __m128 v5 = _mm_castsi128_ps(ld_vmx_rev(m, parent_ea + 0x10u));

            const __m128 v3_w = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v3), 0x00));
            const __m128 v3_x = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v3), 0xFF));
            const __m128 v3_y = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v3), 0xAA));
            const __m128 v3_z = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v3), 0x55));

            const __m128 v5_w = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v5), 0x00));

            // Compose quaternion: q_out = parent_q * local_q
            __m128 q_out = _mm_mul_ps(v3_w, v2);
            const __m128 q_conj = _mm_mul_ps(c_quat_conj, v3);
            q_out = _mm_fmadd_ps(_mm_mul_ps(v3_x, vmx_perm_self(v2, p_perm1)), c_s1, q_out);
            q_out = _mm_fmadd_ps(_mm_mul_ps(v3_y, vmx_perm_self(v2, p_perm2)), c_s2, q_out);
            q_out = _mm_fmadd_ps(_mm_mul_ps(v3_z, vmx_perm_self(v2, p_perm3)), c_s3, q_out);

            // Rotate scaled local translation by parent_q + parent_t
            const __m128 s_t   = _mm_mul_ps(_mm_castsi128_ps(_mm_andnot_si128(c_w_mask, _mm_castps_si128(v5_w))), v4);
            const __m128 w_out = _mm_mul_ps(v4, v5);

            __m128 r_mid = _mm_mul_ps(v3_w, s_t);
            r_mid = _mm_fmadd_ps(_mm_mul_ps(v3_x, vmx_perm_self(s_t, p_perm1)), c_s1, r_mid);
            r_mid = _mm_fmadd_ps(_mm_mul_ps(v3_y, vmx_perm_self(s_t, p_perm2)), c_s2, r_mid);
            r_mid = _mm_fmadd_ps(_mm_mul_ps(v3_z, vmx_perm_self(s_t, p_perm3)), c_s3, r_mid);

            const __m128 rm_w = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(r_mid), 0x00));
            const __m128 rm_x = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(r_mid), 0xFF));
            const __m128 rm_y = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(r_mid), 0xAA));
            const __m128 rm_z = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(r_mid), 0x55));

            __m128 t_out = _mm_mul_ps(rm_w, q_conj);
            t_out = _mm_fmadd_ps(_mm_mul_ps(rm_x, vmx_perm_self(q_conj, p_perm1)), c_s1, t_out);
            t_out = _mm_fmadd_ps(_mm_mul_ps(rm_y, vmx_perm_self(q_conj, p_perm2)), c_s2, t_out);
            t_out = _mm_fmadd_ps(_mm_mul_ps(rm_z, vmx_perm_self(q_conj, p_perm3)), c_s3, t_out);
            t_out = _mm_add_ps(t_out, v5);

            __m128i final_t = _mm_or_si128(_mm_andnot_si128(c_w_mask, _mm_castps_si128(t_out)),
                                           _mm_and_si128(c_w_mask, _mm_castps_si128(w_out)));
            if (__builtin_expect(sb_plain && bone_idx < bone_cap, 1)) {
                st_vmx_rev_fast(m, dst_ea, _mm_castps_si128(q_out));
                st_vmx_rev_fast(m, dst_ea + 0x10u, final_t);
            } else {
                st_vmx_rev(m, dst_ea, _mm_castps_si128(q_out));
                st_vmx_rev(m, dst_ea + 0x10u, final_t);
            }
        }

        if (r27 != 0) {
            ctx->gpr[3] = obj;
            ctx->gpr[4] = bone_idx;
            ctx->gpr[5] = 1;
            ctx->gpr[6] = r28;
            ctx->gpr[7] = r30;
            ctx->gpr[8] = r29;
            ctx->gpr[9] = buf_ea;
            ctx->lr = 0x00532300;
            run(func_00530180, ctx);
        }
    }
    if (buf_plain && req_count > 0) vm_track_write(buf_ea, bone_cap);
    if (sb_plain && req_count > 0)  vm_track_write(space_bases & ~0xFu, bone_cap << 5);
    if (la_plain && req_count > 0)  vm_track_write(local_atoms & ~0xFu, bone_cap << 5);

    const int32_t vis_count = (int32_t)vm_read32_mm(&m, obj + 0x5AC);
    if (vis_count > 0) {
        vis_arr = vm_read32_mm(&m, obj + 0x5A8);
        const uint32_t la_base = vm_read32_mm(&m, obj + 0x25C);
        const uint32_t sb_base = vm_read32_mm(&m, obj + 0x250);
        for (int32_t j = 0; j < vis_count; ++j) {
            if (vm_read8_mm(&m, vis_arr + (uint32_t)j) == 1)
                continue;
            const uint32_t off32 = (uint32_t)j << 5;
            if (j == 0) {
                const uint32_t la = la_base + off32 + 0x10u;
                st_vmx_rev(m, la, _mm_andnot_si128(c_w_mask, ld_vmx_rev(m, la)));
                const uint32_t sb = sb_base + off32 + 0x10u;
                st_vmx_rev(m, sb, _mm_andnot_si128(c_w_mask, ld_vmx_rev(m, sb)));
            } else {
                const uint32_t parent_off = vm_read32_mm(&m, skel + (uint32_t)j * 0x50u + 0x40u) << 5;
                copy32_aligned(m, la_base + off32, la_base + parent_off);
                st_vmx_rev(m, la_base + off32 + 0x10u, _mm_andnot_si128(c_w_mask, ld_vmx_rev(m, la_base + off32 + 0x10u)));
                copy32_aligned(m, sb_base + off32, sb_base + parent_off);
                st_vmx_rev(m, sb_base + off32 + 0x10u, _mm_andnot_si128(c_w_mask, ld_vmx_rev(m, sb_base + off32 + 0x10u)));
            }
        }
    }
}

/* ============================================================================
 * func_00392F08 (ppu_recomp_033.cpp:38672)
 * P1 main: UWorld::MultiLineCheck wrapper (eliminates 1,152-write init loop).
 * ============================================================================ */
void func_00392F08(ppu_context* ctx) {
    const uint32_t world     = (uint32_t)ctx->gpr[3];
    const uint32_t mem_stack = (uint32_t)ctx->gpr[4];
    const uint32_t end_ea    = (uint32_t)ctx->gpr[5];
    const uint32_t start_ea  = (uint32_t)ctx->gpr[6];
    const uint32_t extent_ea = (uint32_t)ctx->gpr[7];
    const uint32_t trace_flg = (uint32_t)ctx->gpr[8];
    const uint32_t src_actor = (uint32_t)ctx->gpr[9];
    const uint32_t extra_arg = (uint32_t)ctx->gpr[10];

    GuestFrame frame(ctx, 0x13F0);
    const uint32_t sp = frame.base;
    const uint32_t hits_ea = sp + 0x7C;

    // Fast init of 64 x 0x4C-byte FCheckResult structs on guest stack
    std::memset(vm_base + hits_ea, 0, 64u * 0x4Cu);
    for (uint32_t i = 0; i < 64u; ++i) {
        uint8_t* p = vm_base + hits_ea + i * 0x4Cu;
        const uint32_t neg1 = 0xFFFFFFFFu;
        std::memcpy(p + 0x24, &neg1, 4);
        std::memcpy(p + 0x44, &neg1, 4);
    }

    int32_t num_hits = 0;
    float best_time = 1.0f;
    wr_f32(sp + 0x70, rd_f32(end_ea + 0x0));
    wr_f32(sp + 0x74, rd_f32(end_ea + 0x4));
    wr_f32(sp + 0x78, rd_f32(end_ea + 0x8));

    if ((trace_flg & 0x4u) != 0) {
        ctx->gpr[3] = world;
        ctx->gpr[4] = hits_ea;
        ctx->gpr[5] = 0;
        ctx->gpr[6] = end_ea;
        ctx->gpr[7] = start_ea;
        ctx->gpr[8] = extent_ea;
        ctx->gpr[9] = trace_flg;
        ctx->lr = 0x00393328;
        run(func_00392B78, ctx);
        if ((int32_t)ctx->gpr[3] == 0) {
            ctx->gpr[3] = world;
            ctx->gpr[4] = 0;
            ctx->lr = 0x0039333C;
            run(func_00582F38, ctx);
            vm_write32(sp + 0x80, (uint32_t)ctx->gpr[3]);

            const float sx = rd_f32(start_ea + 0x0);
            const float sy = rd_f32(start_ea + 0x4);
            const float sz = rd_f32(start_ea + 0x8);
            const float dx = rd_f32(sp + 0x84) - sx;
            const float dy = rd_f32(sp + 0x88) - sy;
            const float dz = rd_f32(sp + 0x8C) - sz;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            const float hit_t = rd_f32(sp + 0x9C);
            const float scaled_t = (hit_t * (dist + 5.0f)) / (dist + 9.999999747378752e-05f);
            if (best_time - scaled_t >= 0.0f)
                best_time = scaled_t;

            const float ex = rd_f32(end_ea + 0x0) - sx;
            const float ey = rd_f32(end_ea + 0x4) - sy;
            const float ez = rd_f32(end_ea + 0x8) - sz;
            wr_f32(sp + 0x70, best_time * ex + sx);
            wr_f32(sp + 0x74, best_time * ey + sy);
            wr_f32(sp + 0x78, best_time * ez + sz);
            num_hits = 1;
        }
    }

    const bool skip_hash = ((double)best_time <= 1e-8) ||
                           (num_hits != 0 && (trace_flg & 0x200u) != 0);
    if (!skip_hash && (trace_flg & 0x209Bu) != 0) {
        const uint32_t hash = vm_read32(world + 0x128);
        if (hash != 0) {
            const uint32_t vtbl = vm_read32(hash);
            const uint32_t opd  = vm_read32(vtbl + 0x14);
            ctx->gpr[3]  = hash;
            ctx->gpr[4]  = mem_stack;
            ctx->gpr[5]  = sp + 0x70;
            ctx->gpr[6]  = start_ea;
            ctx->gpr[7]  = extent_ea;
            ctx->gpr[8]  = trace_flg;
            ctx->gpr[9]  = src_actor;
            ctx->gpr[10] = extra_arg;
            ctx->lr      = 0x00393248;
            guest_call(ctx, opd);

            uint32_t node = (uint32_t)ctx->gpr[3];
            while (node != 0 && (uint32_t)num_hits < 64u) {
                const float t = rd_f32(node + 0x20) * best_time;
                wr_f32(node + 0x20, t);
                std::memcpy(vm_base + hits_ea + (uint32_t)num_hits * 0x4Cu, vm_base + node, 0x4C);
                num_hits++;
                node = vm_read32(node);
            }
        }
    }

    if (num_hits == 0) {
        ctx->gpr[3] = 0;
        return;
    }

    // Sort hits with qsort callback 0x01973610
    ctx->gpr[3] = hits_ea;
    ctx->gpr[4] = (uint32_t)num_hits;
    ctx->gpr[5] = 0x4C;
    ctx->gpr[6] = 0x01973610u;
    ctx->lr = 0x00393054;
    run(func_0002BAE0, ctx);

    const uint32_t total_bytes = (uint32_t)num_hits * 0x4Cu;
    uint32_t top = (vm_read32(mem_stack + 0x0) + 7u) & ~7u;
    uint32_t end = vm_read32(mem_stack + 0x4);
    uint32_t next_top = top + total_bytes;
    if (next_top > end) {
        ctx->gpr[3] = mem_stack;
        ctx->gpr[4] = (int64_t)(int32_t)(total_bytes + 8u);
        ctx->lr = 0x00393098;
        run(func_000B902C, ctx);
        top = (vm_read32(mem_stack + 0x0) + 7u) & ~7u;
        next_top = top + total_bytes;
    }
    vm_write32(mem_stack + 0x0, next_top);

    uint32_t out_head = 0;
    if (top != 0) {
        init_check_result(vm_base + top);
        out_head = top;
    }

    if (num_hits > 0) {
        uint32_t dst = out_head;
        for (int32_t i = 0; i < num_hits; ++i) {
            std::memcpy(vm_base + dst, vm_base + hits_ea + (uint32_t)i * 0x4Cu, 0x4C);
            const uint32_t next_ptr = (i + 1 < num_hits) ? (dst + 0x4Cu) : 0u;
            vm_write32(dst, next_ptr);
            dst += 0x4Cu;
        }
    }

    ctx->gpr[3] = out_head;
}

/* ============================================================================
 * func_005CCB00 (ppu_recomp_055.cpp:26086)
 * P1 main: UPhysicsAsset::CalcAABB (eliminates 13 FPR + 10 GPR spills,
 * 3x3 Newton-Raphson rsqrt loops, and temp TArray heap alloc/free).
 * ============================================================================ */
void func_005CCB00(ppu_context* ctx) {
    const uint32_t out_box    = (uint32_t)ctx->gpr[3];
    const uint32_t phys_asset = (uint32_t)ctx->gpr[4];
    const uint32_t skel_comp  = (uint32_t)ctx->gpr[5];

    GuestFrame frame(ctx, 0x1F0);
    const uint32_t sp = frame.base;

    float sx = rd_f32(skel_comp + 0x1BC) * rd_f32(skel_comp + 0x1B8);
    float sy = rd_f32(skel_comp + 0x1C0) * rd_f32(skel_comp + 0x1B8);
    float sz = rd_f32(skel_comp + 0x1C4) * rd_f32(skel_comp + 0x1B8);

    const uint32_t owner = vm_read32(skel_comp + 0x4C);
    if (owner != 0) {
        const float os = rd_f32(owner + 0x6C);
        sx *= rd_f32(owner + 0x70) * os;
        sy *= rd_f32(owner + 0x74) * os;
        sz *= rd_f32(owner + 0x78) * os;
    }
    wr_f32(sp + 0xB0, sx);
    wr_f32(sp + 0xB4, sy);
    wr_f32(sp + 0xB8, sz);

    const float eps        = rd_f32(0x005CCAECu);
    const float min_len_sq = rd_f32(0x005CCAF0u);
    const float one_val    = rd_f32(0x005CCAF4u);

    float bmin_x = 0.0f, bmin_y = 0.0f, bmin_z = 0.0f;
    float bmax_x = 0.0f, bmax_y = 0.0f, bmax_z = 0.0f;
    uint8_t bvalid = 0;

    if (fabsf(sx - sy) < eps && fabsf(sy - sz) < eps) {
        const bool all_bodies = ((vm_read32(skel_comp + 0x3A8) >> 27) & 1u) != 0;
        const int32_t count = all_bodies ? (int32_t)vm_read32(phys_asset + 0x40)
                                         : (int32_t)vm_read32(phys_asset + 0x88);
        const uint32_t idx_arr  = all_bodies ? 0u : vm_read32(phys_asset + 0x84);
        const uint32_t body_arr = vm_read32(phys_asset + 0x3C);

        for (int32_t i = 0; i < count; ++i) {
            const uint32_t body_idx = all_bodies ? (uint32_t)i : vm_read32(idx_arr + (uint32_t)i * 4u);
            const uint32_t body = vm_read32(body_arr + body_idx * 4u);

            const uint64_t fname = ((uint64_t)vm_read32(body + 0x84) << 32) | vm_read32(body + 0x88);
            ctx->gpr[3] = skel_comp;
            ctx->gpr[4] = fname;
            ctx->lr = 0x005CCDC0;
            run(func_0052A4D0, ctx);
            if ((int32_t)ctx->gpr[3] == -1)
                continue;

            ctx->gpr[5] = (uint32_t)ctx->gpr[3];
            ctx->gpr[3] = sp + 0x70;
            ctx->gpr[4] = skel_comp;
            ctx->lr = 0x005CCDD8;
            run(func_00529DC0, ctx);

            float m00 = rd_f32(sp + 0x70), m01 = rd_f32(sp + 0x74), m02 = rd_f32(sp + 0x78);
            float m10 = rd_f32(sp + 0x80), m11 = rd_f32(sp + 0x84), m12 = rd_f32(sp + 0x88);
            float m20 = rd_f32(sp + 0x90), m21 = rd_f32(sp + 0x94), m22 = rd_f32(sp + 0x98);

            const float c0 = m11 * m22 - m12 * m21;
            const float c1 = m01 * m22 - m02 * m21;
            const float c2 = m01 * m12 - m02 * m11;
            const float det = m00 * c0 - m10 * c1 + m20 * c2;
            if (fabsf(det) <= eps)
                continue;

            const float l0 = m00 * m00 + m01 * m01 + m02 * m02;
            const float l1 = m10 * m10 + m11 * m11 + m12 * m12;
            const float l2 = m20 * m20 + m21 * m21 + m22 * m22;
            const float s0 = (l0 >= min_len_sq) ? (1.0f / sqrtf(l0)) : one_val;
            const float s1 = (l1 >= min_len_sq) ? (1.0f / sqrtf(l1)) : one_val;
            const float s2 = (l2 >= min_len_sq) ? (1.0f / sqrtf(l2)) : one_val;

            wr_f32(sp + 0x70, m00 * s0); wr_f32(sp + 0x74, m01 * s0); wr_f32(sp + 0x78, m02 * s0);
            wr_f32(sp + 0x80, m10 * s1); wr_f32(sp + 0x84, m11 * s1); wr_f32(sp + 0x88, m12 * s1);
            wr_f32(sp + 0x90, m20 * s2); wr_f32(sp + 0x94, m21 * s2); wr_f32(sp + 0x98, m22 * s2);

            ctx->gpr[3] = sp + 0x100;
            ctx->gpr[4] = body + 0x48;
            ctx->gpr[5] = sp + 0x70;
            ctx->gpr[6] = sp + 0xB0;
            ctx->lr = 0x005CCF74;
            run(func_005B9480, ctx);

            const uint8_t nv = vm_read8(sp + 0x118);
            if (!nv)
                continue;

            const float nx0 = rd_f32(sp + 0x100), ny0 = rd_f32(sp + 0x104), nz0 = rd_f32(sp + 0x108);
            const float nx1 = rd_f32(sp + 0x10C), ny1 = rd_f32(sp + 0x110), nz1 = rd_f32(sp + 0x114);
            if (!bvalid) {
                bmin_x = nx0; bmin_y = ny0; bmin_z = nz0;
                bmax_x = nx1; bmax_y = ny1; bmax_z = nz1;
                bvalid = nv;
            } else {
                if (nx0 < bmin_x) bmin_x = nx0;
                if (ny0 < bmin_y) bmin_y = ny0;
                if (nz0 < bmin_z) bmin_z = nz0;
                if (nx1 > bmax_x) bmax_x = nx1;
                if (ny1 > bmax_y) bmax_y = ny1;
                if (nz1 > bmax_z) bmax_z = nz1;
            }
        }
    }

    if (!bvalid) {
        bmin_x = bmax_x = rd_f32(skel_comp + 0xC0);
        bmin_y = bmax_y = rd_f32(skel_comp + 0xC4);
        bmin_z = bmax_z = rd_f32(skel_comp + 0xC8);
        bvalid = 1;
    }

    wr_f32(out_box + 0x00, bmin_x);
    wr_f32(out_box + 0x04, bmin_y);
    wr_f32(out_box + 0x08, bmin_z);
    wr_f32(out_box + 0x0C, bmax_x);
    wr_f32(out_box + 0x10, bmax_y);
    wr_f32(out_box + 0x14, bmax_z);
    vm_write32(out_box + 0x18, (uint32_t)bvalid << 24);
}

/* ============================================================================
 * func_00AD0C68 (ppu_recomp_103.cpp:722)
 * P1 main: NxScene::fetchResults (replaces 26-op per-body pose/vel float
 * copies with 2 contiguous memcpys and eliminates 14 GPR stack spills).
 * ============================================================================ */
static inline void copy_body_state(const ppu_mm& m, uint32_t body) {
    // +0x124..0x13F (28B: pos + quat) -> +0x1B8..0x1D3
    // +0xCC..0xE3  (24B: linVel + angVel) -> +0x19C..0x1B3
    if (__builtin_expect(plain_store(m, body + 0x19C, 0x38), 1)) {
        std::memcpy(m.vb + body + 0x1B8, m.vb + body + 0x124, 28);
        std::memcpy(m.vb + body + 0x19C, m.vb + body + 0xCC, 24);
    } else {
        ppu_store_block(body + 0x1B8, m.vb + body + 0x124, 28);
        ppu_store_block(body + 0x19C, m.vb + body + 0xCC, 24);
    }
    vm_write32_mm(&m, body + 0x140, vm_read32_mm(&m, body + 0x140) + 1u);
    vm_write8_mm(&m, body + 0x1B4, vm_read8_mm(&m, body + 0x54) ^ 1u);
}

void func_00AD0C68(ppu_context* ctx) {
    const uint32_t scene  = (uint32_t)ctx->gpr[3];
    const uint32_t err_ea = (uint32_t)ctx->gpr[4];

    GuestFrame frame(ctx, 0x100);
    const uint32_t sp = frame.base;

    const uint32_t lock_ea = scene + 0x180u;
    ctx->gpr[3] = lock_ea;
    ctx->lr = 0x00AD0CD4;
    run(func_00A670C0, ctx);

    const uint32_t prof_mgr = vm_read32(scene + 0x340);
    ctx->gpr[3] = vm_read32(prof_mgr + 0x34);
    ctx->lr = 0x00AD0CE4;
    run(func_00A669A0, ctx);
    uint32_t prof = (uint32_t)ctx->gpr[3];
    if (prof == 0) {
        ctx->gpr[3] = prof_mgr;
        ctx->lr = 0x00AD1470;
        run(func_00A73EC4, ctx);
        prof = (uint32_t)ctx->gpr[3];
    }

    // Start profiler zone 0x15 if enabled
    {
        const int32_t diff = (int32_t)(vm_read32(prof + 0x8) - vm_read32(prof + 0x4)) >> 3;
        const uint32_t cnt = (uint32_t)(diff * (int32_t)0xB6DB6DB7u) - 1u;
        if (cnt > 0x15u) {
            const uint32_t z = vm_read32(prof + 0x30) + 0x498u;
            if (vm_read32(z) != 0) {
                uint32_t stk = vm_read32(prof + 0x34);
                if (vm_read32(z + 0x8) != 0) {
                    if (vm_read32(z + 0x4) != vm_read32(stk))
                        vm_write32(z + 0x4, 0xFFFFFFFFu);
                } else {
                    const uint32_t id = vm_read32(prof + 0x3C);
                    vm_write32(z + 0x30, id);
                    vm_write32(prof + 0x3C, id + 1u);
                    vm_write32(z + 0x20, vm_read32(prof + 0x38));
                    vm_write32(z + 0x4, vm_read32(stk));
                }
                stk += 4u;
                vm_write32(prof + 0x34, stk);
                vm_write32(prof + 0x38, vm_read32(prof + 0x38) + 1u);
                vm_write32(stk, 0x15u);
                uint64_t tb;
                do { tb = ppu_timebase_now(); } while ((int32_t)tb == 0);
                vm_write64(z + 0x28, tb);
            }
        }
    }

    if (err_ea != 0)
        vm_write32(err_ea, vm_read32(scene + 0x418));

    // Loop 1: copy active rigid body state
    const ppu_mm m = ppu_mm_take();
    {
        const uint32_t list = vm_read32_mm(&m, scene + 0x3B8);
        const uint32_t cnt  = vm_read32_mm(&m, list + 0x10);
        const uint32_t arr  = vm_read32_mm(&m, list + 0x4);
        for (uint32_t i = 0; i < cnt; ++i) {
            const uint32_t item = vm_read32_mm(&m, arr + i * 4u);
            if (vm_read32_mm(&m, item + 0x8) == 0) {
                const uint32_t body = (item != 0) ? (item - 4u) : 0u;
                copy_body_state(m, body);
            }
        }
    }

    // Loop 2: sleeping/deactivated bodies at scene + 0x2E4
    {
        const uint32_t beg = vm_read32(scene + 0x2E4);
        const uint32_t end = vm_read32(scene + 0x2E8);
        if ((uint32_t)((end - beg) + 3u) > 6u) {
            uint32_t idx = 0;
            for (;;) {
                const uint32_t cur_beg = vm_read32(scene + 0x2E4);
                const uint32_t cur_end = vm_read32(scene + 0x2E8);
                const uint32_t n = (uint32_t)((int32_t)(cur_end - cur_beg) >> 2);
                if (idx >= n) break;
                const uint32_t node = vm_read32(cur_beg + idx * 4u);
                const uint32_t body = vm_read32(node + 0x20);
                idx++;
                if (vm_read8(body + 0x54) != 0) continue;

                std::memset(vm_base + sp + 0x74, 0, 0x18);
                ctx->gpr[3] = vm_read32(body + 0xE8);
                ctx->gpr[4] = 2;
                ctx->gpr[5] = sp + 0x74;
                ctx->gpr[6] = 0x18;
                ctx->lr = 0x00AD0F8C;
                run(func_00B8CC88, ctx);
                copy_body_state(m, body);
            }
        }
    }

    // Optional subsystem at scene + 0x150
    const uint32_t sub150 = vm_read32(scene + 0x150);
    if (sub150 != 0) {
        ctx->gpr[3] = sub150;
        ctx->lr = 0x00AD107C;
        run(func_00BB4B50, ctx);
    }

    // Loop 3: sub-scenes / compartments at scene + 0xFC
    {
        const uint32_t beg = vm_read32(scene + 0xFC);
        const uint32_t end = vm_read32(scene + 0x100);
        if ((uint32_t)((end - beg) + 3u) > 6u) {
            uint32_t idx = 0;
            for (;;) {
                const uint32_t cur_beg = vm_read32(scene + 0xFC);
                const uint32_t cur_end = vm_read32(scene + 0x100);
                const uint32_t n = (uint32_t)((int32_t)(cur_end - cur_beg) >> 2);
                if (idx >= n) break;
                vm_write32(sp + 0x70, 0);
                ctx->gpr[3] = vm_read32(cur_beg + idx * 4u);
                ctx->gpr[4] = sp + 0x70;
                ctx->lr = (err_ea == 0) ? 0x00AD10D4 : 0x00AD14A8;
                run(func_00AA6870, ctx);
                if (err_ea != 0 && vm_read32(err_ea) == 0) {
                    const uint32_t e = vm_read32(sp + 0x70);
                    if (e != 0) vm_write32(err_ea, e);
                }
                idx++;
            }
        }
    }

    // Loop 4: active transforms / dirty bodies at scene + 0x14
    {
        uint32_t count = vm_read32(scene + 0x18);
        const uint32_t arr = vm_read32(scene + 0x1C);
        uint32_t i = 0;
        while (i < count) {
            const uint32_t body = vm_read32(arr + i * 4u);
            const uint32_t b104 = body + 0x104u;
            const uint16_t slot = vm_read16(b104 + 0x20);
            if (slot != 0xFFFFu) {
                const uint32_t f8 = vm_read32(b104 + 0x8) & ~0x2u;
                vm_write32(b104 + 0x8, f8);
                const uint32_t idx8 = vm_read8(b104 + 0x23);
                const uint32_t mgr = vm_read32(scene + 0x160u + idx8 * 4u + 4u);
                ctx->gpr[3] = mgr;
                ctx->gpr[4] = b104;
                call_vfn_toc(ctx, mgr, 0x14, 0x00AD1208);

                const uint16_t slot2 = vm_read16(b104 + 0x20);
                if (slot2 != 0xFFFFu) {
                    const uint32_t f8b = vm_read32(b104 + 0x8);
                    if ((f8b & 0x2u) == 0) {
                        vm_write32(b104 + 0x8, f8b | 0x2u);
                        const uint32_t cb_ptr = vm_read32((uint32_t)(0x019AF8F8u - 0x5194u));
                        const uint32_t cb_opd = vm_read32(cb_ptr);
                        if (cb_opd != 0) {
                            const uint32_t b18 = vm_read32(b104 + 0x18);
                            const uint32_t base10 = vm_read32(b18 + 0x10);
                            ctx->gpr[3] = vm_read32(b104 + 0x4);
                            ctx->gpr[4] = base10 + (uint32_t)slot2 * 24u;
                            ctx->ctr    = vm_read32(cb_opd);
                            ctx->gpr[2] = vm_read32(cb_opd + 4);
                            ctx->lr     = 0x00AD127C;
                            guest_icall(ctx);
                            ctx->gpr[2] = 0x019AF8F8ULL;
                        }
                    }
                }
            }

            const uint32_t f14c = vm_read32(body + 0x14C);
            const bool keep = (f14c & 0x4u) != 0;
            vm_write32(body + 0x14C, f14c & ~0x8u);
            if (!keep) {
                count--;
                const uint32_t new_n = vm_read32(scene + 0x18) - 1u;
                const uint32_t cur_arr = vm_read32(scene + 0x1C);
                vm_write32(scene + 0x18, new_n);
                vm_write32(cur_arr + i * 4u, vm_read32(cur_arr + new_n * 4u));
                vm_write32(body + 0x14C, vm_read32(body + 0x14C) & ~0x2u);
            } else {
                i++;
            }
        }
    }

    ctx->gpr[3] = scene + 0x160u;
    ctx->lr = 0x00AD12C0;
    run(func_00BA2CB8, ctx);

    // Loop 5: scene + 0xB0 callbacks
    {
        uint32_t p = vm_read32(scene + 0xB0);
        while (p != vm_read32(scene + 0xB4)) {
            ctx->gpr[3] = vm_read32(p);
            p += 4u;
            ctx->lr = 0x00AD12E8;
            run(func_00BE77F4, ctx);
        }
    }

    // End profiler zone 0x15
    ctx->gpr[3] = vm_read32(prof_mgr + 0x34);
    ctx->lr = 0x00AD1300;
    run(func_00A669A0, ctx);
    prof = (uint32_t)ctx->gpr[3];
    if (prof == 0) {
        ctx->gpr[3] = prof_mgr;
        ctx->lr = 0x00AD1460;
        run(func_00A73EC4, ctx);
        prof = (uint32_t)ctx->gpr[3];
    }
    {
        const int32_t diff = (int32_t)(vm_read32(prof + 0x8) - vm_read32(prof + 0x4)) >> 3;
        const uint32_t cnt = (uint32_t)(diff * (int32_t)0xB6DB6DB7u) - 1u;
        if (cnt > 0x15u) {
            const uint32_t base30 = vm_read32(prof + 0x30);
            const uint32_t z = base30 + 0x498u;
            if (vm_read32(z) != 0) {
                uint64_t tb;
                do { tb = ppu_timebase_now(); } while ((int32_t)tb == 0);
                const uint64_t dt = tb - vm_read64(base30 + 0x4C0u);
                vm_write64(base30 + 0x4C0u, dt);
                vm_write32(prof + 0x38, vm_read32(prof + 0x38) - 1u);
                vm_write32(z + 0x8, vm_read32(z + 0x8) + 1u);
                vm_write64(z + 0x10, vm_read64(z + 0x10) + vm_read64(z + 0x28));
                const uint32_t stk = vm_read32(prof + 0x34) - 4u;
                vm_write32(prof + 0x34, stk);
                const uint32_t parent_id = vm_read32(stk);
                const uint32_t pz = base30 + parent_id * 56u;
                vm_write64(pz + 0x18, vm_read64(pz + 0x18) + vm_read64(z + 0x28));
            }
        }
    }

    ctx->gpr[3] = lock_ea;
    ctx->lr = 0x00AD13DC;
    run(func_00A67040, ctx);
}

namespace {

static inline void quat_mul(float ax, float ay, float az, float aw,
                            float bx, float by, float bz, float bw,
                            float& ox, float& oy, float& oz, float& ow) {
    ox = aw * bx + ax * bw + ay * bz - az * by;
    oy = aw * by + ay * bw + az * bx - ax * bz;
    oz = aw * bz + az * bw + ax * by - ay * bx;
    ow = aw * bw - ax * bx - ay * by - az * bz;
}

static inline void obb_to_aabb(float px, float py, float pz,
                               float qx, float qy, float qz, float qw,
                               float lcx, float lcy, float lcz,
                               float lex, float ley, float lez,
                               float out_c[3], float out_e[3]) {
    const float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    const float xx = qx * x2, yy = qy * y2, zz = qz * z2;
    const float xy = qx * y2, xz = qx * z2, yz = qy * z2;
    const float wx = qw * x2, wy = qw * y2, wz = qw * z2;
    const float r00 = 1.0f - (yy + zz), r10 = xy + wz, r20 = xz - wy;
    const float r01 = xy - wz, r11 = 1.0f - (xx + zz), r21 = yz + wx;
    const float r02 = xz + wy, r12 = yz - wx, r22 = 1.0f - (xx + yy);
    out_e[0] = fabsf(r00) * lex + fabsf(r01) * ley + fabsf(r02) * lez;
    out_e[1] = fabsf(r10) * lex + fabsf(r11) * ley + fabsf(r12) * lez;
    out_e[2] = fabsf(r20) * lex + fabsf(r21) * ley + fabsf(r22) * lez;
    if (lcx == 0.0f && lcy == 0.0f && lcz == 0.0f) {
        out_c[0] = px; out_c[1] = py; out_c[2] = pz;
    } else {
        float rx, ry, rz;
        rot_vec_by_quat(qx, qy, qz, qw, lcx, lcy, lcz, rx, ry, rz);
        out_c[0] = px + rx; out_c[1] = py + ry; out_c[2] = pz + rz;
    }
}

static inline bool compute_shape_bounds_fast(const ppu_mm& m, uint32_t geom,
                                             float px, float py, float pz,
                                             float qx, float qy, float qz, float qw,
                                             float out_c[3], float out_e[3]) {
    const uint32_t type = vm_read32_mm(&m, geom + 0x14);
    switch (type) {
    case 1: { // Sphere
        const float r = rd_f32_mm(m, geom + 0x1C);
        out_c[0] = px; out_c[1] = py; out_c[2] = pz;
        out_e[0] = r;  out_e[1] = r;  out_e[2] = r;
        return true;
    }
    case 3: { // Capsule (local X axis)
        const float radius = rd_f32_mm(m, geom + 0x1C);
        const float half_h = rd_f32_mm(m, geom + 0x20);
        const float ax = 1.0f - 2.0f * (qy * qy + qz * qz);
        const float ay = 2.0f * (qy * qx + qz * qw);
        const float az = 2.0f * (qz * qx - qy * qw);
        out_c[0] = px; out_c[1] = py; out_c[2] = pz;
        out_e[0] = fabsf(ax) * half_h + radius;
        out_e[1] = fabsf(ay) * half_h + radius;
        out_e[2] = fabsf(az) * half_h + radius;
        return true;
    }
    case 5: { // Box
        const float lex = rd_f32_mm(m, geom + 0x1C);
        const float ley = rd_f32_mm(m, geom + 0x20);
        const float lez = rd_f32_mm(m, geom + 0x24);
        obb_to_aabb(px, py, pz, qx, qy, qz, qw, 0.0f, 0.0f, 0.0f, lex, ley, lez, out_c, out_e);
        return true;
    }
    case 6:
    case 7: { // ConvexMesh / TriangleMesh
        const uint32_t b = vm_read32_mm(&m, geom + 0x1C);
        const float min_x = rd_f32_mm(m, b + 0x0),  min_y = rd_f32_mm(m, b + 0x4),  min_z = rd_f32_mm(m, b + 0x8);
        const float max_x = rd_f32_mm(m, b + 0xC),  max_y = rd_f32_mm(m, b + 0x10), max_z = rd_f32_mm(m, b + 0x14);
        const float lex = (max_x - min_x) * 0.5f;
        const float ley = (max_y - min_y) * 0.5f;
        const float lez = (max_z - min_z) * 0.5f;
        obb_to_aabb(px, py, pz, qx, qy, qz, qw, min_x + lex, min_y + ley, min_z + lez, lex, ley, lez, out_c, out_e);
        return true;
    }
    default:
        return false;
    }
}

} // namespace

// PhysX BoxShape::getWorldBounds (P1 main, 100% pure leaf)
void func_00B44FA8(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint32_t shape = (uint32_t)ctx->gpr[3];
    const uint32_t out   = (uint32_t)ctx->gpr[4];
    const uint32_t body  = vm_read32_mm(&m, shape + 0x128);
    if (body != 0) {
        const uint32_t ts = vm_read32_mm(&m, body + 0x140);
        if (ts != vm_read32_mm(&m, shape + 0x2C)) {
            const float bx = rd_f32_mm(m, body + 0x130), by = rd_f32_mm(m, body + 0x134),
                        bz = rd_f32_mm(m, body + 0x138), bw = rd_f32_mm(m, body + 0x13C);
            const float lx = rd_f32_mm(m, shape + 0xA0), ly = rd_f32_mm(m, shape + 0xA4),
                        lz = rd_f32_mm(m, shape + 0xA8), lw = rd_f32_mm(m, shape + 0xAC);
            const float lpx = rd_f32_mm(m, shape + 0x94), lpy = rd_f32_mm(m, shape + 0x98), lpz = rd_f32_mm(m, shape + 0x9C);
            float wx, wy, wz, ww;
            quat_mul(bx, by, bz, bw, lx, ly, lz, lw, wx, wy, wz, ww);
            const float xx = wx * wx, yy = wy * wy, zz = wz * wz;
            const float xy = wx * wy, xz = wx * wz, yz = wy * wz;
            const float xw = wx * ww, yw = wy * ww, zw = wz * ww;
            const float r[9] = {
                1.0f - 2.0f * (yy + zz), 2.0f * (xy - zw),        2.0f * (xz + yw),
                2.0f * (xy + zw),        1.0f - 2.0f * (zz + xx), 2.0f * (yz - xw),
                2.0f * (xz - yw),        2.0f * (yz + xw),        1.0f - 2.0f * (yy + xx),
            };
            float rx, ry, rz;
            rot_vec_by_quat(bx, by, bz, bw, lpx, lpy, lpz, rx, ry, rz);
            const float px = rx + rd_f32_mm(m, body + 0x124);
            const float py = ry + rd_f32_mm(m, body + 0x128);
            const float pz = rz + rd_f32_mm(m, body + 0x12C);
            for (int i = 0; i < 9; ++i) wr_f32_mm(m, shape + 0x30u + 4u * (uint32_t)i, r[i]);
            wr_f32_mm(m, shape + 0x54, px);
            wr_f32_mm(m, shape + 0x58, py);
            wr_f32_mm(m, shape + 0x5C, pz);
            vm_write32_mm(&m, shape + 0x2C, ts);
        }
    }
    const float ex = rd_f32_mm(m, shape + 0x16C);
    const float ey = rd_f32_mm(m, shape + 0x170);
    const float ez = rd_f32_mm(m, shape + 0x174);
    const float r00 = rd_f32_mm(m, shape + 0x30), r01 = rd_f32_mm(m, shape + 0x34), r02 = rd_f32_mm(m, shape + 0x38);
    const float r10 = rd_f32_mm(m, shape + 0x3C), r11 = rd_f32_mm(m, shape + 0x40), r12 = rd_f32_mm(m, shape + 0x44);
    const float r20 = rd_f32_mm(m, shape + 0x48), r21 = rd_f32_mm(m, shape + 0x4C), r22 = rd_f32_mm(m, shape + 0x50);
    const float cx = rd_f32_mm(m, shape + 0x54), cy = rd_f32_mm(m, shape + 0x58), cz = rd_f32_mm(m, shape + 0x5C);
    const float hx = fabsf(ex * r00) + fabsf(ey * r01) + fabsf(ez * r02);
    const float hy = fabsf(ex * r10) + fabsf(ey * r11) + fabsf(ez * r12);
    const float hz = fabsf(ex * r20) + fabsf(ey * r21) + fabsf(ez * r22);
    wr_f32_mm(m, out + 0x0,  cx - hx);
    wr_f32_mm(m, out + 0x4,  cy - hy);
    wr_f32_mm(m, out + 0x8,  cz - hz);
    wr_f32_mm(m, out + 0xC,  cx + hx);
    wr_f32_mm(m, out + 0x10, cy + hy);
    wr_f32_mm(m, out + 0x14, cz + hz);
}

// PhysX Body::updateBody2World (P40 PPU PhysX)
void func_00B49908(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint32_t body = (uint32_t)ctx->gpr[3];
    const float c2b_x = -rd_f32_mm(m, body + 0x114), c2b_y = -rd_f32_mm(m, body + 0x118),
                c2b_z = -rd_f32_mm(m, body + 0x11C), c2b_w =  rd_f32_mm(m, body + 0x120);
    const float b2w_x =  rd_f32_mm(m, body + 0xBC),  b2w_y =  rd_f32_mm(m, body + 0xC0),
                b2w_z =  rd_f32_mm(m, body + 0xC4),  b2w_w =  rd_f32_mm(m, body + 0xC8);
    float qx, qy, qz, qw;
    quat_mul(b2w_x, b2w_y, b2w_z, b2w_w, c2b_x, c2b_y, c2b_z, c2b_w, qx, qy, qz, qw);
    wr_f32_mm(m, body + 0x130, qx);
    wr_f32_mm(m, body + 0x134, qy);
    wr_f32_mm(m, body + 0x138, qz);
    wr_f32_mm(m, body + 0x13C, qw);
    float rx, ry, rz;
    rot_vec_by_quat(qx, qy, qz, qw,
                    rd_f32_mm(m, body + 0x108), rd_f32_mm(m, body + 0x10C), rd_f32_mm(m, body + 0x110),
                    rx, ry, rz);
    wr_f32_mm(m, body + 0x124, rd_f32_mm(m, body + 0xB0) - rx);
    wr_f32_mm(m, body + 0x128, rd_f32_mm(m, body + 0xB4) - ry);
    wr_f32_mm(m, body + 0x12C, rd_f32_mm(m, body + 0xB8) - rz);

    const uint32_t f14c = vm_read32_mm(&m, body + 0x14C);
    vm_write32_mm(&m, body + 0x140, vm_read32_mm(&m, body + 0x140) + 1u);

    const uint32_t beg = vm_read32_mm(&m, body + 0x48);
    const uint32_t end = vm_read32_mm(&m, body + 0x4C);
    if (beg == end) return;

    const uint64_t saved_lr = ctx->lr;
    if (f14c & 0x2000u) {
        GuestFrame frame(ctx, 0xA0);
        for (uint32_t p = beg; p != end; p += 4u) {
            const uint32_t s = vm_read32(p);
            if (vm_read32(s + 8u) != 0u) continue;
            const uint32_t obj = s - 4u;
            if (obj == 0u) break;
            ctx->gpr[3] = obj;
            call_vfn_toc(ctx, obj, 0x1F0u, 0x00B49AD8u);
        }
        ctx->lr = saved_lr;
        return;
    }
    if (vm_read8_mm(&m, body + 0x54) != 0u) return;
    uint32_t scene = vm_read32_mm(&m, body + 0x144);
    const uint32_t b_flag = vm_read32_mm(&m, vm_read32_mm(&m, scene + 0x3B8) + 0xC8);
    const bool notify_bp = ((b_flag & 1u) != 0u) || ((f14c & 0x80u) != 0u);
    GuestFrame frame(ctx, 0xA0);
    for (uint32_t p = beg; p != end; p += 4u) {
        const uint32_t s = vm_read32(p);
        if (vm_read32(s + 8u) != 0u) continue;
        const uint32_t obj = s - 4u;
        if (obj == 0u) break;
        if (notify_bp) {
            ctx->gpr[3] = vm_read32(scene + 0x15C);
            ctx->gpr[4] = obj;
            ctx->gpr[5] = 0;
            ctx->lr = 0x00B49C4Cu;
            run(func_00B41078, ctx);
            scene = vm_read32(body + 0x144);
        }
        ctx->gpr[3] = scene;
        ctx->gpr[4] = obj;
        ctx->gpr[5] = 0;
        ctx->lr = notify_bp ? 0x00B49C60u : 0x00B49BB0u;
        run(func_00AC6534, ctx);
        scene = vm_read32(body + 0x144);
    }
    ctx->lr = saved_lr;
}

// PhysX Shape::computeWorldBounds (P40 PPU PhysX)
void func_00C91AD8(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint32_t geom  = (uint32_t)ctx->gpr[3];
    const uint32_t pose  = (uint32_t)ctx->gpr[4];
    const uint32_t out_c = (uint32_t)ctx->gpr[5];
    const uint32_t out_e = (uint32_t)ctx->gpr[6];
    float c[3], e[3];
    if (compute_shape_bounds_fast(m, geom,
                                  rd_f32_mm(m, pose + 0x0), rd_f32_mm(m, pose + 0x4), rd_f32_mm(m, pose + 0x8),
                                  rd_f32_mm(m, pose + 0xC), rd_f32_mm(m, pose + 0x10),
                                  rd_f32_mm(m, pose + 0x14), rd_f32_mm(m, pose + 0x18), c, e)) {
        wr_f32_mm(m, out_c + 0x0, c[0]); wr_f32_mm(m, out_c + 0x4, c[1]); wr_f32_mm(m, out_c + 0x8, c[2]);
        wr_f32_mm(m, out_e + 0x0, e[0]); wr_f32_mm(m, out_e + 0x4, e[1]); wr_f32_mm(m, out_e + 0x8, e[2]);
        return;
    }
    func_00C91AD8_lifted(ctx);
}

// PhysX BroadPhase::updateBounds (P40 PPU PhysX)
void func_00C4BDFC(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t dirty = vm_read32_mm(&m, self + 0x8) + 0x20B4u;
    const uint32_t nwords = vm_read32_mm(&m, dirty + 0x4);
    if (nwords == 0u) return;
    const uint32_t words_ea = vm_read32_mm(&m, dirty + 0x0);
    const uint32_t pool = self + 0xCu;
    const uint32_t pool_mask  = vm_read32_mm(&m, pool + 0x0) - 1u;
    const uint32_t pool_shift = vm_read32_mm(&m, pool + 0xC) & 31u;
    const uint32_t pool_pages = vm_read32_mm(&m, pool + 0x18);
    const uint64_t saved_lr = ctx->lr;
    bool framed = false;
    uint32_t pose_ea = 0;

    for (uint32_t wi = 0u; wi < nwords; ++wi) {
        uint32_t w = vm_read32_mm(&m, words_ea + wi * 4u);
        while (w != 0u) {
            const uint32_t b = (uint32_t)__builtin_clz(w);
            w &= ~(0x80000000u >> b);
            const uint32_t id = (wi << 5) + b;
            if (id == 0xFFFFFFFFu) goto done;
            const uint32_t page = vm_read32_mm(&m, pool_pages + 4u * (id >> pool_shift));
            const uint32_t entry = page + (id & pool_mask) * 0x34u;
            const uint32_t shape = vm_read32_mm(&m, entry + 0x10);
            if (shape == 0u) continue;
            const uint32_t body = vm_read32_mm(&m, shape + 0xC);
            float px, py, pz, qx, qy, qz, qw;
            if (body != 0u) {
                const float bx = rd_f32_mm(m, body + 0x64), by = rd_f32_mm(m, body + 0x68),
                            bz = rd_f32_mm(m, body + 0x6C), bw = rd_f32_mm(m, body + 0x70);
                const float lpx = rd_f32_mm(m, shape + 0x1C), lpy = rd_f32_mm(m, shape + 0x20), lpz = rd_f32_mm(m, shape + 0x24);
                const float lx = rd_f32_mm(m, shape + 0x28), ly = rd_f32_mm(m, shape + 0x2C),
                            lz = rd_f32_mm(m, shape + 0x30), lw = rd_f32_mm(m, shape + 0x34);
                float rx, ry, rz;
                rot_vec_by_quat(bx, by, bz, bw, lpx, lpy, lpz, rx, ry, rz);
                px = rx + rd_f32_mm(m, body + 0x58);
                py = ry + rd_f32_mm(m, body + 0x5C);
                pz = rz + rd_f32_mm(m, body + 0x60);
                quat_mul(bx, by, bz, bw, lx, ly, lz, lw, qx, qy, qz, qw);
            } else {
                px = rd_f32_mm(m, shape + 0x1C); py = rd_f32_mm(m, shape + 0x20); pz = rd_f32_mm(m, shape + 0x24);
                qx = rd_f32_mm(m, shape + 0x28); qy = rd_f32_mm(m, shape + 0x2C);
                qz = rd_f32_mm(m, shape + 0x30); qw = rd_f32_mm(m, shape + 0x34);
            }
            const uint32_t geom = vm_read32_mm(&m, shape + 0x10);
            float c[3], e[3];
            if (compute_shape_bounds_fast(m, geom, px, py, pz, qx, qy, qz, qw, c, e)) {
                wr_f32_mm(m, entry + 0x1C, c[0]);
                wr_f32_mm(m, entry + 0x20, c[1]);
                wr_f32_mm(m, entry + 0x24, c[2]);
                wr_f32_mm(m, entry + 0x28, e[0] >= 0.0f ? e[0] : 0.0f);
                wr_f32_mm(m, entry + 0x2C, e[1] >= 0.0f ? e[1] : 0.0f);
                wr_f32_mm(m, entry + 0x30, e[2] >= 0.0f ? e[2] : 0.0f);
            } else {
                if (!framed) {
                    const uint64_t sp = ctx->gpr[1];
                    vm_write64(sp - 0x120u, sp);
                    ctx->gpr[1] = sp - 0x120u;
                    pose_ea = (uint32_t)ctx->gpr[1] + 0x70u;
                    framed = true;
                }
                wr_f32(pose_ea + 0x0, px); wr_f32(pose_ea + 0x4, py); wr_f32(pose_ea + 0x8, pz);
                wr_f32(pose_ea + 0xC, qx); wr_f32(pose_ea + 0x10, qy); wr_f32(pose_ea + 0x14, qz); wr_f32(pose_ea + 0x18, qw);
                ctx->gpr[3] = geom;
                ctx->gpr[4] = pose_ea;
                ctx->gpr[5] = entry + 0x1Cu;
                ctx->gpr[6] = entry + 0x28u;
                ctx->lr = 0x00C4C0ACu;
                run(func_00C91AD8_lifted, ctx);
                const float ex = rd_f32(entry + 0x28), ey = rd_f32(entry + 0x2C), ez = rd_f32(entry + 0x30);
                wr_f32(entry + 0x28, ex >= 0.0f ? ex : 0.0f);
                wr_f32(entry + 0x2C, ey >= 0.0f ? ey : 0.0f);
                wr_f32(entry + 0x30, ez >= 0.0f ? ez : 0.0f);
            }
        }
    }
done:
    if (framed) ctx->gpr[1] += 0x120u;
    ctx->lr = saved_lr;
}

void func_005C5010(ppu_context* ctx) {
    const ppu_mm m = ppu_mm_take();
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t l2w  = (uint32_t)ctx->gpr[4];
    for (uint32_t i = 0; i < 16u; ++i) {
        if ((fast32(m, l2w + i * 4u) & 0x7F800000u) == 0x7F800000u) return;
    }
    if (fast32(m, self + 0x378u) == 0u) {
        if (fast32(m, self + 0x230u) == 0u) return;
        const uint32_t pai = fast32(m, self + 0x234u);
        if (pai == 0u || fast32(m, self + 0x1E4u) == 0u) return;
        const int32_t n = (int32_t)fast32(m, pai + 0x48u);
        if (n <= 0) return;
        const uint32_t bodies = fast32(m, pai + 0x44u);
        bool any = false;
        for (int32_t i = 0; i < n; ++i) {
            const uint32_t inst = fast32(m, bodies + (uint32_t)i * 4u);
            if (fast32(m, inst + 0x60u) != 0u) { any = true; break; }
        }
        if (!any) return;
    }
    func_005C5010_lifted(ctx);
}
