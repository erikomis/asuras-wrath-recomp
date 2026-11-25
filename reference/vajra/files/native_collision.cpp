#include "guest.h"

#include <cmath>
#include <cstring>

namespace {

inline float r_f32(const ppu_mm& m, uint32_t ea)
{
    if (__builtin_expect(vm_mm_ok(&m, ea, 4), 1)) {
        uint32_t x;
        std::memcpy(&x, m.vb + ea, 4);
        return from_bits(__builtin_bswap32(x));
    }
    return from_bits(vm_read32_mm(&m, ea));
}

inline uint32_t r_u32(const ppu_mm& m, uint32_t ea)
{
    if (__builtin_expect(vm_mm_ok(&m, ea, 4), 1)) {
        uint32_t x;
        std::memcpy(&x, m.vb + ea, 4);
        return __builtin_bswap32(x);
    }
    return vm_read32_mm(&m, ea);
}

inline double r_f64(const ppu_mm& m, uint32_t ea)
{
    uint64_t u;
    if (__builtin_expect(vm_mm_ok(&m, ea, 8), 1)) {
        std::memcpy(&u, m.vb + ea, 8);
        u = __builtin_bswap64(u);
    } else {
        u = vm_read64_mm(&m, ea);
    }
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

inline void w_f32(const ppu_mm& m, uint32_t ea, float v)
{
    vm_write32_mm(&m, ea, to_bits(v));
}

inline float dot3(float ax, float ay, float az, const ppu_mm& m, uint32_t ea)
{
    const float x = r_f32(m, ea);
    const float y = r_f32(m, ea + 4u);
    const float z = r_f32(m, ea + 8u);
    return std::fmaf(az, z, std::fmaf(ax, x, ay * y));
}

inline float dot3_scaled(float ax, float ay, float az, const ppu_mm& m, uint32_t ea, float s)
{
    const float x = r_f32(m, ea) * s;
    const float y = r_f32(m, ea + 4u) * s;
    const float z = r_f32(m, ea + 8u) * s;
    return std::fmaf(az, z, std::fmaf(ax, x, ay * y));
}

inline void write_hit(const ppu_mm& m, uint32_t ea, float nx, float ny, float nz)
{
    vm_write32_mm(&m, ea, 1u);
    w_f32(m, ea + 4u, nx);
    w_f32(m, ea + 8u, ny);
    w_f32(m, ea + 12u, nz);
}

inline void write_vec3(const ppu_mm& m, uint32_t ea, float x, float y, float z)
{
    w_f32(m, ea, x);
    w_f32(m, ea + 4u, y);
    w_f32(m, ea + 8u, z);
}

}

void func_01271980(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t hit_out = (uint32_t)ctx->gpr[3];
    const uint32_t box = (uint32_t)ctx->gpr[4];
    const uint32_t other = (uint32_t)ctx->gpr[5];
    const uint32_t axis = (uint32_t)ctx->gpr[6];
    const uint32_t start = (uint32_t)ctx->gpr[7];
    const uint32_t end = (uint32_t)ctx->gpr[8];
    const uint32_t p_t0 = (uint32_t)ctx->gpr[9];
    const uint32_t p_t1 = (uint32_t)ctx->gpr[10];
    const uint32_t out_n0 = vm_read32_mm(&m, sp + 0x74u);
    const uint32_t out_n1 = vm_read32_mm(&m, sp + 0x7Cu);

    const float ax = r_f32(m, axis);
    const float ay = r_f32(m, axis + 4u);
    const float az = r_f32(m, axis + 8u);

    const float o30 = r_f32(m, other + 0x30u);
    const float o34 = r_f32(m, other + 0x34u);
    const float o38 = r_f32(m, other + 0x38u);

    const float c_proj = dot3(ax, ay, az, m, other);
    const float u_proj = dot3_scaled(ax, ay, az, m, other + 0x0Cu, o30);
    const float v_proj = dot3_scaled(ax, ay, az, m, other + 0x18u, o34);
    const float w_proj = dot3_scaled(ax, ay, az, m, other + 0x24u, o38);

    const float c0 = r_f32(m, 0x01271968u);
    const float tbl[2] = { r_f32(m, 0x016A4B00u), r_f32(m, 0x016A4B04u) };

    float min_v = c0, max_v = c0;
    bool first = true;
    for (int i = 0; i < 2; ++i) {
        const float ti = std::fmaf(tbl[i], u_proj, c_proj);
        for (int j = 0; j < 2; ++j) {
            const float tj = std::fmaf(tbl[j], v_proj, ti);
            for (int k = 0; k < 2; ++k) {
                const float val = std::fmaf(tbl[k], w_proj, tj);
                if (first) {
                    min_v = val;
                    max_v = val;
                    first = false;
                } else {
                    min_v = ((double)val - (double)min_v >= 0.0) ? min_v : val;
                    if (val > max_v) max_v = val;
                }
            }
        }
    }

    const float bx = std::fabs(dot3(ax, ay, az, m, box + 0x0Cu));
    const float by = std::fabs(dot3(ax, ay, az, m, box + 0x18u));
    const float bz = std::fabs(dot3(ax, ay, az, m, box + 0x24u));
    const float rad = std::fmaf(r_f32(m, box + 0x38u), bz,
                                std::fmaf(r_f32(m, box + 0x30u), bx, r_f32(m, box + 0x34u) * by));

    min_v -= rad;
    max_v += rad;

    const float eps = r_f32(m, 0x0127196Cu);
    const float p0 = dot3(ax, ay, az, m, start);
    const float p1 = dot3(ax, ay, az, m, end);
    const float dp = p1 - p0;
    const float adp = std::fabs(dp);

    if (!(adp >= eps)) {
        if (p0 < min_v && !(p0 <= min_v - eps))
            write_hit(m, hit_out, -ax, -ay, -az);
        if (!(p0 <= max_v) && p0 < max_v + eps)
            write_hit(m, hit_out, ax, ay, az);
        const double eps_d = r_f64(m, 0x01271970u);
        if (!((double)adp >= eps_d)) {
            ctx->gpr[3] = (p0 < min_v || p0 > max_v) ? 0u : 1u;
            return;
        }
    }

    const float inv_dp = r_f32(m, 0x01271978u) / dp;
    const float d0 = min_v - p0;
    const float d1 = max_v - p0;
    float t_enter, t_exit, nx, ny, nz;
    if (dp > c0) {
        t_enter = d0 * inv_dp;
        t_exit = d1 * inv_dp;
        nx = -ax; ny = -ay; nz = -az;
    } else {
        t_enter = d1 * inv_dp;
        t_exit = d0 * inv_dp;
        nx = ax; ny = ay; nz = az;
    }

    if (!(t_enter <= r_f32(m, p_t0))) {
        w_f32(m, p_t0, t_enter);
        write_vec3(m, out_n0, nx, ny, nz);
    }
    float cur_t1 = r_f32(m, p_t1);
    if (t_exit < cur_t1) {
        w_f32(m, p_t1, t_exit);
        write_vec3(m, out_n1, -nx, -ny, -nz);
        cur_t1 = t_exit;
    }
    const float cur_t0 = r_f32(m, p_t0);
    ctx->gpr[3] = (cur_t1 >= cur_t0 && !(cur_t1 < c0)) ? 1u : 0u;
}

void func_01272748(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t hit_out = (uint32_t)ctx->gpr[3];
    const uint32_t verts_arr = (uint32_t)ctx->gpr[4];
    const uint32_t axis = (uint32_t)ctx->gpr[6];
    const uint32_t start = (uint32_t)ctx->gpr[7];
    const uint32_t end = (uint32_t)ctx->gpr[8];
    const uint32_t box_ax = (uint32_t)ctx->gpr[9];
    const uint32_t box_ay = (uint32_t)ctx->gpr[10];
    const uint32_t box_az = r_u32(m, sp + 0x74u);
    const uint32_t box_ext = r_u32(m, sp + 0x7Cu);
    const uint32_t p_t0 = r_u32(m, sp + 0x84u);
    const uint32_t p_t1 = r_u32(m, sp + 0x8Cu);
    const uint32_t out_n0 = r_u32(m, sp + 0x94u);
    const uint32_t out_n1 = r_u32(m, sp + 0x9Cu);

    const float ax = r_f32(m, axis);
    const float ay = r_f32(m, axis + 4u);
    const float az = r_f32(m, axis + 8u);
    const float len2 = std::fmaf(az, az, std::fmaf(ax, ax, ay * ay));
    if (!((double)len2 >= r_f64(m, 0x01272720u))) {
        ctx->gpr[3] = 1u;
        return;
    }

    const float bx = std::fabs(dot3(ax, ay, az, m, box_ax));
    const float by = std::fabs(dot3(ax, ay, az, m, box_ay));
    const float bz = std::fabs(dot3(ax, ay, az, m, box_az));
    const float rad = std::fmaf(r_f32(m, box_ext + 8u), bz,
                                std::fmaf(r_f32(m, box_ext), bx, r_f32(m, box_ext + 4u) * by));

    const float p0 = dot3(ax, ay, az, m, start);
    const float p1 = dot3(ax, ay, az, m, end);

    float min_v = r_f32(m, 0x01272728u);
    float max_v = r_f32(m, 0x0127272Cu);
    const int32_t count = (int32_t)r_u32(m, verts_arr + 4u);
    uint32_t vp = r_u32(m, verts_arr);
    if (__builtin_expect(count > 0 && vm_mm_ok(&m, vp, (uint32_t)count * 12u), 1)) {
        const uint8_t* ptr = m.vb + vp;
        for (int32_t i = 0; i < count; ++i, ptr += 12) {
            uint32_t ux, uy, uz;
            std::memcpy(&ux, ptr, 4);
            std::memcpy(&uy, ptr + 4, 4);
            std::memcpy(&uz, ptr + 8, 4);
            const float x = from_bits(__builtin_bswap32(ux));
            const float y = from_bits(__builtin_bswap32(uy));
            const float z = from_bits(__builtin_bswap32(uz));
            const float d = std::fmaf(az, z, std::fmaf(ax, x, ay * y));
            min_v = (d < min_v) ? d : min_v;
            max_v = (d > max_v) ? d : max_v;
        }
    } else {
        for (int32_t i = 0; i < count; ++i, vp += 12u) {
            const float d = dot3(ax, ay, az, m, vp);
            min_v = (d - min_v >= 0.0f) ? min_v : d;
            max_v = (d - max_v >= 0.0f) ? d : max_v;
        }
    }

    min_v -= rad;
    max_v += rad;

    const float eps = r_f32(m, 0x01272730u);
    const float dp = p1 - p0;
    const float adp = std::fabs(dp);

    if (!(adp >= eps)) {
        if (min_v > p0 && p0 > min_v - eps)
            write_hit(m, hit_out, -ax, -ay, -az);
        if (max_v < p0 && p0 < max_v + eps)
            write_hit(m, hit_out, ax, ay, az);
        if (!((double)adp >= r_f64(m, 0x01272738u))) {
            ctx->gpr[3] = (min_v > p0 || max_v < p0) ? 0u : 1u;
            return;
        }
    }

    const float inv_dp = r_f32(m, 0x01272740u) / dp;
    const float c0 = r_f32(m, 0x01272734u);
    const float t_a = (min_v - p0) * inv_dp;
    const float t_b = (max_v - p0) * inv_dp;
    float t_enter, t_exit, nx, ny, nz;
    if (dp <= c0) {
        t_enter = t_b;
        t_exit = t_a;
        nx = ax; ny = ay; nz = az;
    } else {
        t_enter = t_a;
        t_exit = t_b;
        nx = -ax; ny = -ay; nz = -az;
    }

    if (!(t_enter <= r_f32(m, p_t0))) {
        w_f32(m, p_t0, t_enter);
        write_vec3(m, out_n0, nx, ny, nz);
    }
    float cur_t1 = r_f32(m, p_t1);
    if (t_exit < cur_t1) {
        w_f32(m, p_t1, t_exit);
        write_vec3(m, out_n1, -nx, -ny, -nz);
        cur_t1 = t_exit;
    }
    const float cur_t0 = r_f32(m, p_t0);
    ctx->gpr[3] = (cur_t1 >= cur_t0 && !(cur_t1 < c0)) ? 1u : 0u;
}
