#include "guest.h"
#include "pc_options.h"
#include "vk/rsx_vk.h"
#include "../ps3recomp/libs/input/pad_bindings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr uint32_t k_delta_seconds = 0x019B8690;
constexpr uint32_t k_current_time = 0x019FED10;
constexpr uint32_t k_gnatives = 0x01A01824;
constexpr uint32_t k_gnames = 0x01A0B014;
constexpr uint32_t k_gprop_addr = 0x019B1BD4;
constexpr uint32_t k_gprop_state[] = {0x019B1BB4, k_gprop_addr, 0x019B1BD8};
constexpr uint32_t k_runtime_flags = 0x019EEC88;
constexpr uint32_t k_skipped_optional = 2;
constexpr uint8_t k_empty_parm = 0x4A;
constexpr uint32_t k_frame = 0x100;
constexpr uint32_t k_slot_bits = 11;
constexpr uint32_t k_slot_mask = (1u << k_slot_bits) - 1;
constexpr uint32_t k_timers = 0xA0;
constexpr uint32_t k_timer_size = 0x1C;
constexpr double k_tick = 1.0 / 30;

constexpr double k_aim_stick_per_unit = 3.14159265358979 / 32768 / 1.2;

enum class Mode : uint8_t { Add, Ratio, Press, Lerp, LerpRev, Homing, SmaReset, SmaRate, Scale, Approach, LerpScale, LerpDt, Ramp, Clamp, TimerTick, Look, LerpLook, Aim };

struct Site {
    const char* path;
    uint32_t offset;
    Mode mode;
    uint32_t extra;
};

const Site k_sites[] = {
#include "frame_sites.inc"
};

struct Shape {
    uint32_t count;
    bool integer;
    bool assigns;
    bool returns_new;
};

constexpr Shape k_float = {1, false, true, true};
constexpr Shape k_vector = {3, false, true, true};
constexpr Shape k_rotator = {3, true, true, true};
constexpr Shape k_int = {1, true, true, true};
constexpr Shape k_int_sum = {1, true, false, true};
constexpr Shape k_post_increment = {1, true, true, false};

std::atomic<uint64_t> g_keys[1u << k_slot_bits];
std::atomic<uint64_t> g_owners[1u << k_slot_bits];
std::atomic<uint32_t> g_version{0};
std::mutex g_writer;
uint32_t g_site_code[std::size(k_sites)];
thread_local bool t_rotation_site;
thread_local bool t_timer_site;
thread_local bool t_mouse_turned;
std::atomic<bool> g_mouse_camera{false};
std::atomic<uint32_t> g_can_assist{0};
thread_local std::unordered_map<uint32_t, std::deque<float>> t_frame_times;

double read_f64(uint32_t ea)
{
    return from_bits64(vm_read64(ea));
}

}

double tick_scale()
{
    return read_f64(k_delta_seconds) * 30.0;
}

namespace {

float read_f32(uint32_t ea)
{
    return from_bits(vm_read32(ea));
}

void write_f32(uint32_t ea, float v)
{
    vm_write32(ea, to_bits(v));
}

uint32_t script(uint32_t stack)
{
    return vm_read32(vm_read32(stack + 0x10) + 0x4C);
}

void step(ppu_context* ctx, uint32_t stack, uint32_t out)
{
    const uint32_t code = vm_read32(stack + 0x18);
    vm_write32(stack + 0x18, code + 1);
    const uint32_t entry = k_gnatives + vm_read8(code) * 8u;
    const uint32_t self = vm_read32(stack + 0x14) + vm_read32(entry + 4);
    uint32_t function = vm_read32(entry);
    if (function & 1)
        function = vm_read32(vm_read32(self) + function - 1);
    ctx->gpr[3] = self;
    ctx->gpr[4] = stack;
    ctx->gpr[5] = out;
    guest_call(ctx, function);
}

uint32_t probe(ppu_context* ctx, uint32_t stack, uint32_t code, uint32_t out)
{
    const uint32_t resume = vm_read32(stack + 0x18);
    uint32_t state[std::size(k_gprop_state)];
    for (size_t i = 0; i < std::size(k_gprop_state); ++i)
        state[i] = vm_read32(k_gprop_state[i]);
    vm_write32(stack + 0x18, code);
    vm_write32(k_gprop_addr, 0);
    step(ctx, stack, out);
    const uint32_t lvalue = vm_read32(k_gprop_addr);
    vm_write32(stack + 0x18, resume);
    for (size_t i = 0; i < std::size(k_gprop_state); ++i)
        vm_write32(k_gprop_state[i], state[i]);
    return lvalue;
}

uint32_t home(uint32_t code)
{
    return (code * 0x9E3779B1u) >> (32 - k_slot_bits);
}

const Site* lookup(uint32_t stack)
{
    const uint32_t code = vm_read32(stack + 0x18);
    for (;;) {
        const uint32_t version = g_version.load(std::memory_order_acquire);
        const Site* found = nullptr;
        for (uint32_t i = home(code); const uint64_t key = g_keys[i].load(std::memory_order_relaxed); i = (i + 1) & k_slot_mask) {
            if ((uint32_t)(key >> 32) != code)
                continue;
            const uint64_t owner = g_owners[i].load(std::memory_order_relaxed);
            const uint32_t node = vm_read32(stack + 0x10);
            if ((uint32_t)(owner >> 32) == node && vm_read32(node + 0x4C) == (uint32_t)owner) {
                found = &k_sites[(uint32_t)key];
                break;
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!(version & 1) && g_version.load(std::memory_order_relaxed) == version)
            return found;
    }
}

void erase(uint64_t key)
{
    uint32_t i = home((uint32_t)(key >> 32));
    while (g_keys[i].load(std::memory_order_relaxed) != key)
        i = (i + 1) & k_slot_mask;
    for (uint32_t j = (i + 1) & k_slot_mask; const uint64_t moved = g_keys[j].load(std::memory_order_relaxed); j = (j + 1) & k_slot_mask) {
        if (((j - home((uint32_t)(moved >> 32))) & k_slot_mask) < ((j - i) & k_slot_mask))
            continue;
        g_owners[i].store(g_owners[j].load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_keys[i].store(moved, std::memory_order_relaxed);
        i = j;
    }
    g_keys[i].store(0, std::memory_order_relaxed);
}

void enroll(const Site* first, const Site* last, uint32_t function)
{
    const uint32_t data = vm_read32(function + 0x4C);
    const std::lock_guard<std::mutex> lock(g_writer);
    const uint32_t version = g_version.load(std::memory_order_relaxed);
    g_version.store(version + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (const Site* site = first; site != last; ++site) {
        const uint32_t index = (uint32_t)(site - k_sites), code = data + site->offset;
        if (g_site_code[index])
            erase((uint64_t)g_site_code[index] << 32 | index);
        uint32_t i = home(code);
        while (g_keys[i].load(std::memory_order_relaxed))
            i = (i + 1) & k_slot_mask;
        g_owners[i].store((uint64_t)function << 32 | data, std::memory_order_relaxed);
        g_keys[i].store((uint64_t)code << 32 | index, std::memory_order_relaxed);
        g_site_code[index] = code;
    }
    g_version.store(version + 2, std::memory_order_release);
}

bool object_path(uint32_t object, std::string& path)
{
    const uint32_t outer = vm_read32(object + 0x28);
    if (outer) {
        if (!object_path(outer, path))
            return false;
        path += '.';
    }
    if (vm_read32(object + 0x30))
        return false;
    const uint32_t entry = vm_read32(vm_read32(k_gnames) + vm_read32(object + 0x2C) * 4);
    if (vm_read32(entry) & 1)
        return false;
    for (uint32_t at = entry + 8; const char c = (char)vm_read8(at); ++at)
        path += c;
    return true;
}

float corrected_alpha(Mode mode, float alpha, double k)
{
    if (mode == Mode::LerpLook && t_mouse_turned)
        return 1;
    if (mode == Mode::Homing || mode == Mode::Scale)
        return (float)(alpha * k);
    const double a = mode == Mode::LerpDt ? alpha / k : alpha;
    if (!(a > 0 && a < 1))
        return alpha;
    return (float)(mode == Mode::LerpRev ? std::pow(a, k) : 1 - std::pow(1 - a, k));
}

uint32_t float_step(Mode mode, uint32_t before_bits, uint32_t after_bits, double k, double press)
{
    const double b = from_bits(before_bits), a = from_bits(after_bits), c = a / b;
    return to_bits((float)(mode == Mode::Ratio ? (b != 0 && c >= 0 ? b * std::pow(c, k) : a)
                           : mode == Mode::LerpScale ? (c > 0 && c < 1 ? b * (1 - std::pow(1 - c, k)) : a)
                           : mode == Mode::Press ? a + press * (1 - k)
                                                 : b + (a - b) * k));
}

double whole_delta(const Site* site, int32_t delta, double k, double press)
{
    if (site->mode == Mode::Ramp)
        return delta * k + press * k * (1 - k) / 2;
    if (site->mode == Mode::Clamp && std::abs(delta) < (int32_t)site->extra)
        return std::copysign(std::min<double>(std::abs(delta), site->extra * k), delta);
    return delta * k;
}

uint32_t whole_step(uint64_t key, uint32_t before, double delta)
{
    static thread_local std::unordered_map<uint64_t, double> carry;
    if (carry.size() >= 8192)
        carry.clear();
    double& acc = carry[key];
    acc += delta;
    const double whole = std::floor(acc);
    acc -= whole;
    return before + (uint32_t)(int64_t)whole;
}

std::atomic<uint64_t> g_mouse_owner_tick{0};
std::atomic<uint64_t> g_rotation_tick{0};

uint64_t tick_now()
{
    return vm_read64(k_current_time);
}

bool ran_recently(const std::atomic<uint64_t>& tick)
{
    const uint64_t bits = tick.load(std::memory_order_relaxed);
    return bits && read_f64(k_current_time) <= from_bits64(bits) + read_f64(k_delta_seconds);
}

void take_mouse(float* dx, float* dy)
{
    rsx_vk_mouse_motion(dx, dy);
    g_mouse_owner_tick.store(tick_now(), std::memory_order_relaxed);
}

void look(void (*body)(ppu_context*), ppu_context* ctx, uint32_t stack)
{
    const uint64_t self = ctx->gpr[3], result = ctx->gpr[5];
    const GuestFrame frame(ctx, k_frame);
    const uint32_t lvalue = probe(ctx, stack, vm_read32(stack + 0x18), frame.base + 0x70);
    const uint64_t before = lvalue ? vm_read64(lvalue) : 0;
    ctx->gpr[3] = self;
    ctx->gpr[4] = stack;
    ctx->gpr[5] = result;
    run(body, ctx);
    float dx, dy;
    take_mouse(&dx, &dy);
    t_mouse_turned = dx != 0 || dy != 0;
    if (!lvalue)
        return;
    if (vm_read64(lvalue) != before)
        g_mouse_camera.store(false, std::memory_order_relaxed);
    if (!t_mouse_turned)
        return;
    g_mouse_camera.store(true, std::memory_order_relaxed);
    static thread_local float carry[2];
    float yaw, pitch;
    pc_options_mouse_scale(&yaw, &pitch);
    const float turn[2] = {-dy * pitch + carry[0], dx * yaw + carry[1]};
    for (uint32_t i = 0; i < 2; ++i) {
        const float whole = std::round(turn[i]);
        carry[i] = turn[i] - whole;
        const uint32_t value = vm_read32(lvalue + 4 * i) + (uint32_t)(int32_t)whole;
        vm_write32(lvalue + 4 * i, value);
        vm_write32((uint32_t)result + 4 * i, value);
    }
}

void compound(void (*body)(ppu_context*), ppu_context* ctx, const Shape& shape)
{
    const uint32_t stack = (uint32_t)ctx->gpr[4];
    const Site* site = lookup(stack);
    if (!site)
        return run(body, ctx);
    if (site->mode == Mode::SmaReset) {
        t_frame_times.erase(vm_read32(stack + 0x14));
        return run(body, ctx);
    }
    if (site->mode == Mode::Look)
        return look(body, ctx, stack);
    const uint64_t self = ctx->gpr[3], result = ctx->gpr[5];
    const GuestFrame frame(ctx, k_frame);
    const uint32_t scratch = frame.base + 0x70;
    const uint32_t lvalue = probe(ctx, stack, vm_read32(stack + 0x18), scratch);
    const uint32_t old_at = lvalue ? lvalue : scratch;
    uint32_t before[3];
    for (uint32_t i = 0; i < shape.count; ++i)
        before[i] = vm_read32(old_at + 4 * i);
    ctx->gpr[3] = self;
    ctx->gpr[4] = stack;
    ctx->gpr[5] = result;
    run(body, ctx);
    const double k = tick_scale();
    double press = 0;
    if (site->mode == Mode::Press || site->mode == Mode::Ramp) {
        probe(ctx, stack, script(stack) + site->extra, scratch);
        press = shape.integer ? (double)(int32_t)vm_read32(scratch) : read_f32(scratch);
    }
    const uint64_t key = (uint64_t)vm_read32(stack + 0x14) << 32 | (uint32_t)(site - k_sites) << 2;
    const uint32_t new_at = shape.returns_new ? (uint32_t)result : lvalue;
    for (uint32_t i = 0; i < shape.count; ++i) {
        const uint32_t after = vm_read32(new_at + 4 * i);
        const uint32_t value = shape.integer
            ? whole_step(key | i, before[i], whole_delta(site, (int32_t)(after - before[i]), k, press))
            : float_step(site->mode, before[i], after, k, press);
        if (shape.assigns && lvalue)
            vm_write32(lvalue + 4 * i, value);
        if (shape.returns_new)
            vm_write32((uint32_t)result + 4 * i, value);
    }
}

uint32_t param_size(char param)
{
    return param == 'v' || param == 'r' ? 12 : 4;
}

void advance(float* y, const float* d, float c)
{
    float s = 0;
    for (int i = 0; i < 3; ++i) {
        y[i] += d[i] * c;
        s += y[i] * y[i];
    }
    if (s == 1)
        return;
    const float scale = s < 1e-8f ? 0 : 1 / std::sqrt(s);
    for (int i = 0; i < 3; ++i)
        y[i] *= scale;
}

void homing(ppu_context* ctx, uint32_t stack, const Site* site, uint32_t direction, float c, double k, uint32_t result, uint32_t scratch)
{
    probe(ctx, stack, script(stack) + site->extra, scratch);
    float x[3], d[3], y[3];
    for (int i = 0; i < 3; ++i) {
        x[i] = y[i] = read_f32(scratch + 4 * i);
        d[i] = read_f32(direction + 4 * i);
    }
    const double whole = std::floor(k);
    for (double n = 0; n < whole; ++n)
        advance(y, d, c);
    advance(y, d, (float)(c * (k - whole)));
    for (int i = 0; i < 3; ++i)
        write_f32(result + 4 * i, y[i] - x[i]);
}

float approach(ppu_context* ctx, uint32_t stack, const Site* site, float step, double k, uint32_t scratch)
{
    probe(ctx, stack, script(stack) + site->extra, scratch);
    const double distance = std::hypot(read_f32(scratch), read_f32(scratch + 4)), c = step / distance;
    return c > 0 && c < 1 ? (float)(distance * (1 - std::pow(1 - c, k))) : step;
}

void lerp_op(void (*body)(ppu_context*), ppu_context* ctx, const char* signature)
{
    const uint32_t stack = (uint32_t)ctx->gpr[4];
    const Site* site = lookup(stack);
    if (!site)
        return run(body, ctx);
    const uint64_t self = ctx->gpr[3], result = ctx->gpr[5];
    const GuestFrame frame(ctx, k_frame);
    const uint32_t values = frame.base + 0x70, stream = values + 0x20, proxy = stream + 0x30;
    bool empty = false;
    uint32_t alpha = 0, at = values;
    for (const char* p = signature; *p; at += param_size(*p++)) {
        const uint32_t code = vm_read32(stack + 0x18);
        if (*p == 'b' && vm_read8(code) == k_empty_parm) {
            empty = true;
            vm_write32(stack + 0x18, code + 1);
            continue;
        }
        if (*p == 'a')
            alpha = at;
        step(ctx, stack, at);
    }
    vm_write32(stack + 0x18, vm_read32(stack + 0x18) + 1);
    const double k = tick_scale();
    if (site->mode == Mode::Homing && k > 1)
        return homing(ctx, stack, site, values, read_f32(alpha), k, (uint32_t)result, stream);
    write_f32(alpha, site->mode == Mode::Approach ? approach(ctx, stack, site, read_f32(alpha), k, stream)
                                                  : corrected_alpha(site->mode, read_f32(alpha), k));
    uint32_t out = stream;
    at = values;
    for (const char* p = signature; *p; at += param_size(*p++)) {
        if (*p == 'b') {
            vm_write8(out++, empty ? k_empty_parm : vm_read32(at) ? 0x27 : 0x28);
            continue;
        }
        vm_write8(out++, *p == 'v' ? 0x23 : *p == 'r' ? 0x22 : 0x1E);
        for (uint32_t i = 0; i < param_size(*p); i += 4, out += 4)
            vm_write32(out, vm_read32(at + i));
    }
    vm_write8(out, 0x16);
    for (uint32_t i = 0; i < 0x28; i += 4)
        vm_write32(proxy + i, vm_read32(stack + i));
    vm_write32(proxy + 0x18, stream);
    ctx->gpr[3] = self;
    ctx->gpr[4] = proxy;
    ctx->gpr[5] = result;
    run(body, ctx);
}

void rotate(void (*body)(ppu_context*), ppu_context* ctx, bool per_tick)
{
    const bool prev = t_rotation_site;
    t_rotation_site = per_tick;
    run(body, ctx);
    t_rotation_site = prev;
}

void velocity_decay(void (*body)(ppu_context*), ppu_context* ctx, uint32_t factor_at)
{
    const uint32_t pawn = vm_read32((uint32_t)ctx->gpr[3] + 0xE4);
    if (!pawn)
        return run(body, ctx);
    float before[3];
    for (int i = 0; i < 3; ++i)
        before[i] = read_f32(pawn + 0x134 + 4 * i);
    run(body, ctx);
    const double factor = read_f32(factor_at), scaled = std::pow(factor, tick_scale());
    for (int i = 0; i < 3; ++i) {
        const uint32_t at = pawn + 0x134 + 4 * i;
        if (vm_read32(at) == to_bits((float)(before[i] * factor)))
            write_f32(at, (float)(before[i] * scaled));
    }
}

float window_frame_time(uint32_t owner, uint32_t length, float dt)
{
    if (!length)
        return dt;
    if (t_frame_times.size() > 8192 && !t_frame_times.count(owner))
        t_frame_times.clear();
    std::deque<float>& times = t_frame_times[owner];
    times.push_back(dt);
    while (times.size() > length)
        times.pop_front();
    while (times.size() < length)
        times.push_front(times.front());
    double sum = 0, weight = 0;
    for (const float time : times)
        sum += time * ++weight;
    return (float)(2 * sum / (weight * (weight + 1)));
}

void aim(ppu_context* ctx, uint32_t stack, uint32_t extra)
{
    const uint64_t self = ctx->gpr[3], result = ctx->gpr[5];
    const GuestFrame frame(ctx, k_frame);
    const uint32_t scratch = frame.base + 0x70;
    const uint32_t stick = probe(ctx, stack, script(stack) + (extra >> 16), scratch);
    probe(ctx, stack, script(stack) + (extra & 0xFFFF), scratch);
    const double dt = read_f32(scratch);
    ctx->gpr[3] = self;
    ctx->gpr[4] = stack;
    ctx->gpr[5] = result;
    run(func_00058AE0_lifted, ctx);
    if (!stick || !(dt > 0))
        return;
    float dx, dy, yaw, pitch;
    take_mouse(&dx, &dy);
    pc_options_mouse_scale(&yaw, &pitch);
    const double k = k_aim_stick_per_unit / dt;
    write_f32(stick, (float)(read_f32(stick) - dy * pitch * k));
    write_f32(stick + 4, (float)(read_f32(stick + 4) + dx * yaw * k));
}

void mash_meter(void (*body)(ppu_context*), ppu_context* ctx, uint32_t drain_field, uint32_t press_field)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const float drain = read_f32(self + drain_field), press = read_f32(self + press_field);
    const double k = tick_scale();
    write_f32(self + drain_field, (float)(drain * k));
    write_f32(self + press_field, (float)(press + drain * (1.0 - k)));
    run(body, ctx);
    write_f32(self + drain_field, drain);
    write_f32(self + press_field, press);
}

struct Timer {
    double due;
    double armed;
    double dilation = 1;
    bool chained;
};

double timer_rate(uint32_t actor, uint64_t name, uint32_t object, double rate)
{
    static thread_local std::unordered_map<uint64_t, Timer> timers;
    if (timers.size() >= 8192)
        timers.clear();
    float armed_rate = 0, count = 0;
    for (uint32_t at = vm_read32(actor + k_timers), end = at + vm_read32(actor + k_timers + 4) * k_timer_size; at != end; at += k_timer_size)
        if (vm_read64(at + 4) == name && vm_read32(at + 0x18) == object) {
            armed_rate = read_f32(at + 0xC);
            count = read_f32(at + 0x10);
        }
    const double now = read_f64(k_current_time);
    const bool fired = armed_rate > 0 && count > armed_rate;
    Timer& timer = timers[(uint64_t)actor << 32 | (uint32_t)(name >> 32)];
    const bool measured = fired && timer.chained && now > timer.armed;
    if (measured)
        timer.dilation = count / (now - timer.armed);
    timer.due = (fired && now > timer.armed ? std::max(timer.due, now - k_tick) : now) + (std::floor(rate / k_tick / timer.dilation) + 1) * k_tick;
    timer.armed = now;
    timer.chained = fired;
    return std::max((timer.due - now - (measured ? k_tick / 4 : k_tick * 3 / 4)) * timer.dilation, 1e-6);
}

}

bool frame_site(uint32_t stack)
{
    return lookup(stack) != nullptr;
}

bool camera_assist_blocked(uint32_t function)
{
    return function == g_can_assist.load(std::memory_order_relaxed) && g_mouse_camera.load(std::memory_order_relaxed);
}

void note_stick_rotation()
{
    g_rotation_tick.store(tick_now(), std::memory_order_relaxed);
}

int mouse_route()
{
    return ran_recently(g_mouse_owner_tick) ? PAD_MOUSE_CAMERA : ran_recently(g_rotation_tick) ? PAD_MOUSE_LEFT_STICK : PAD_MOUSE_RIGHT_STICK;
}

void func_00078E88(ppu_context* ctx)
{
    const uint32_t function = (uint32_t)ctx->gpr[3];
    run(func_00078E88_lifted, ctx);
    std::string path;
    if (!object_path(function, path))
        return;
    if (path == "BCGame.BCCamMode_Default.CanAssistFunction")
        g_can_assist.store(function, std::memory_order_relaxed);
    pc_options_link(function, path);
    const Site* first = std::lower_bound(std::begin(k_sites), std::end(k_sites), path,
                                         [](const Site& site, const std::string& key) { return key.compare(site.path) > 0; });
    const Site* last = first;
    while (last != std::end(k_sites) && path == last->path)
        ++last;
    if (first != last)
        enroll(first, last, function);
}

void func_00059DB8(ppu_context* ctx)
{
    compound(func_00059DB8_lifted, ctx, k_float);
}

void func_00059F54(ppu_context* ctx)
{
    compound(func_00059F54_lifted, ctx, k_float);
}

void func_00059A44(ppu_context* ctx)
{
    compound(func_00059A44_lifted, ctx, k_float);
}

void func_0004FFE4(ppu_context* ctx)
{
    compound(func_0004FFE4_lifted, ctx, k_vector);
}

void func_0005055C(ppu_context* ctx)
{
    compound(func_0005055C_lifted, ctx, k_vector);
}

void func_000501A8(ppu_context* ctx)
{
    compound(func_000501A8_lifted, ctx, k_vector);
}

void func_0004D6BC(ppu_context* ctx)
{
    compound(func_0004D6BC_lifted, ctx, k_rotator);
}

void func_00057898(ppu_context* ctx)
{
    compound(func_00057898_lifted, ctx, k_int);
}

void func_00056288(ppu_context* ctx)
{
    compound(func_00056288_lifted, ctx, k_int_sum);
}

void func_00057E28(ppu_context* ctx)
{
    compound(func_00057E28_lifted, ctx, k_post_increment);
}

void func_00057A34(ppu_context* ctx)
{
    compound(func_00057A34_lifted, ctx, k_int);
}

void func_0005B730(ppu_context* ctx)
{
    lerp_op(func_0005B730_lifted, ctx, "ffa");
}

void func_0004F1D8(ppu_context* ctx)
{
    lerp_op(func_0004F1D8_lifted, ctx, "vva");
}

void func_00051E18(ppu_context* ctx)
{
    lerp_op(func_00051E18_lifted, ctx, "rrab");
}

void func_0005188C(ppu_context* ctx)
{
    lerp_op(func_0005188C_lifted, ctx, "va");
}

void func_0004E3B4(ppu_context* ctx)
{
    lerp_op(func_0004E3B4_lifted, ctx, "ra");
}

void func_00062E98(ppu_context* ctx)
{
    const uint32_t stack = (uint32_t)ctx->gpr[4];
    const Site* site = lookup(stack);
    if (!site)
        return run(func_00062E98_lifted, ctx);
    const uint32_t result = (uint32_t)ctx->gpr[5];
    const GuestFrame frame(ctx, k_frame);
    const uint32_t a = frame.base + 0x70, b = a + 0x10, alpha = b + 0x10, shortest = alpha + 4, out = a + 0x30;
    step(ctx, stack, a);
    step(ctx, stack, b);
    step(ctx, stack, alpha);
    vm_write32(shortest, 1);
    vm_write32(k_runtime_flags, vm_read32(k_runtime_flags) & ~k_skipped_optional);
    step(ctx, stack, shortest);
    vm_write32(stack + 0x18, vm_read32(stack + 0x18) + 1);
    ctx->gpr[3] = out;
    ctx->gpr[4] = a;
    ctx->gpr[5] = b;
    ctx->fpr[1] = corrected_alpha(site->mode, read_f32(alpha), tick_scale());
    run(vm_read32(shortest) ? func_00086450 : func_000865C8, ctx);
    for (uint32_t i = 0; i < 16; i += 4)
        vm_write32(result + i, vm_read32(out + i));
}

void func_00058AE0(ppu_context* ctx)
{
    const uint32_t stack = (uint32_t)ctx->gpr[4];
    const Site* site = lookup(stack);
    if (!site)
        return run(func_00058AE0_lifted, ctx);
    if (site->mode == Mode::Aim)
        return aim(ctx, stack, site->extra);
    const uint32_t result = (uint32_t)ctx->gpr[5], owner = vm_read32(stack + 0x14);
    const GuestFrame frame(ctx, k_frame);
    const uint32_t a = frame.base + 0x70, b = a + 4;
    step(ctx, stack, a);
    step(ctx, stack, b);
    vm_write32(stack + 0x18, vm_read32(stack + 0x18) + 1);
    write_f32(result, read_f32(a) / window_frame_time(owner, vm_read32(owner + site->extra + 4), read_f32(b)));
}

void func_014F1F64(ppu_context* ctx)
{
    rotate(func_014F1F64_lifted, ctx, lookup((uint32_t)ctx->gpr[4]) != nullptr);
}

void func_014F2218(ppu_context* ctx)
{
    rotate(func_014F2218_lifted, ctx, lookup((uint32_t)ctx->gpr[4]) != nullptr);
}

void func_008F55B0(ppu_context* ctx)
{
    rotate(func_008F55B0_lifted, ctx, true);
}

void func_008F5740(ppu_context* ctx)
{
    rotate(func_008F5740_lifted, ctx, true);
}

void func_008F58C0(ppu_context* ctx)
{
    rotate(func_008F58C0_lifted, ctx, true);
}

void func_008F5A30(ppu_context* ctx)
{
    rotate(func_008F5A30_lifted, ctx, true);
}

void func_008F5BA8(ppu_context* ctx)
{
    rotate(func_008F5BA8_lifted, ctx, true);
}

void func_008F6288(ppu_context* ctx)
{
    rotate(func_008F6288_lifted, ctx, true);
}

void func_008F65C8(ppu_context* ctx)
{
    rotate(func_008F65C8_lifted, ctx, true);
}

void func_008F6980(ppu_context* ctx)
{
    rotate(func_008F6980_lifted, ctx, true);
}

void func_008F21A0(ppu_context* ctx)
{
    if (t_rotation_site) {
        t_rotation_site = false;
        ctx->fpr[1] = corrected_alpha(Mode::Lerp, (float)ctx->fpr[1], tick_scale());
    }
    run(func_008F21A0_lifted, ctx);
}

void func_00934904(ppu_context* ctx)
{
    const uint32_t rate = (uint32_t)ctx->gpr[3] + 0xB28;
    const uint32_t original = vm_read32(rate);
    write_f32(rate, corrected_alpha(Mode::Lerp, read_f32(rate), tick_scale()));
    run(func_00934904_lifted, ctx);
    vm_write32(rate, original);
}

void func_008CC0B0(ppu_context* ctx)
{
    velocity_decay(func_008CC0B0_lifted, ctx, 0x008CC0A8);
}

void func_008D1048(ppu_context* ctx)
{
    velocity_decay(func_008D1048_lifted, ctx, 0x008D1040);
}

void func_011368C4(ppu_context* ctx)
{
    const bool prev = t_timer_site;
    t_timer_site = lookup((uint32_t)ctx->gpr[4]);
    run(func_011368C4_lifted, ctx);
    t_timer_site = prev;
}

void func_00498718(ppu_context* ctx)
{
    if (t_timer_site) {
        t_timer_site = false;
        const uint32_t actor = (uint32_t)ctx->gpr[3];
        ctx->fpr[1] = (float)timer_rate(actor, ctx->gpr[6], ctx->gpr[7] ? (uint32_t)ctx->gpr[7] : actor, ctx->fpr[1]);
    }
    run(func_00498718_lifted, ctx);
}

void func_008C9B20(ppu_context* ctx)
{
    mash_meter(func_008C9B20_lifted, ctx, 0xEC, 0xF0);
}

void func_008CAABC(ppu_context* ctx)
{
    mash_meter(func_008CAABC_lifted, ctx, 0xD0, 0xD4);
}
