#include "firmware.h"

#include "../ps3recomp/libs/filesystem/ps3_aes.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

extern "C" char* stbi_zlib_decode_malloc_guesssize_headerflag(const char* buffer, int len, int initial_size, int* outlen, int parse_header);

namespace {

constexpr const char* k_url = "http://dus01.ps3.update.playstation.net/update/ps3/image/us/2026_0318_a2b60b6ac1d2e49e230144345616927c/PS3UPDAT.PUP";
constexpr uint64_t k_size = 206197916;
constexpr const char* k_update_url_eu = "http://b0.ww.np.dl.playstation.net/tppkg/np/BLES01227/BLES01227_T3/0c670c22fd332f45/EP0102-BLES01227_00-PATCH02000000000-A0102-V0100-PE.pkg";
constexpr const char* k_update_url_us = "http://b0.ww.np.dl.playstation.net/tppkg/np/BLUS30721/BLUS30721_T3/9472a41e0f952e95/UP0102-BLUS30721_00-PATCH02000000000-A0102-V0100-PE.pkg";
constexpr uint64_t k_update_size = 34731232;
constexpr const char* k_files[] = { "sys/external/flashMP3.pic", "sys/external/flashATRAC.pic" };

const uint8_t k_erk[32] = { 0xA9, 0x78, 0x18, 0xBD, 0x19, 0x3A, 0x67, 0xA1, 0x6F, 0xE8, 0x3A, 0x85, 0x5E, 0x1B, 0xE9, 0xFB,
                            0x56, 0x40, 0x93, 0x8D, 0x4D, 0xBC, 0xB2, 0xCB, 0x52, 0xC5, 0xA2, 0xF8, 0xB0, 0x2B, 0x10, 0x31 };
const uint8_t k_riv[16] = { 0x4A, 0xCE, 0xF0, 0x12, 0x24, 0xFB, 0xEE, 0xDF, 0x82, 0x45, 0xF8, 0xFF, 0x10, 0x21, 0x1E, 0x6E };

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

struct tar_entry {
    std::string name;
    uint64_t size;
};

bool tar_header(const uint8_t* h, tar_entry& e)
{
    if (!h[0]) return false;
    const std::string prefix(reinterpret_cast<const char*>(h + 345), strnlen(reinterpret_cast<const char*>(h + 345), 155));
    const std::string name(reinterpret_cast<const char*>(h), strnlen(reinterpret_cast<const char*>(h), 100));
    e.name = prefix.empty() ? name : prefix + "/" + name;
    e.size = std::strtoull(std::string(reinterpret_cast<const char*>(h + 124), 12).c_str(), nullptr, 8);
    return true;
}

PS3_AES_TARGET bool unpack(const std::vector<uint8_t>& s, std::vector<uint8_t>& tar)
{
    if (s.size() < 0x20 || std::memcmp(s.data(), "SCE", 4)) return false;
    const uint64_t meta = 0x20 + (uint64_t)be32(s.data() + 0x0C), hlen = be64(s.data() + 0x10);
    if (hlen > s.size() || meta + 0x40 + 0x20 > hlen) return false;
    ps3_aes_key key;
    uint8_t info[0x40];
    ps3_aes256_init(&key, k_erk);
    ps3_aes_cbc_dec(&key, k_riv, s.data() + meta, info, sizeof info);
    std::vector<uint8_t> h(hlen - meta - 0x40);
    ps3_aes128_init(&key, info);
    ps3_aes_ctr(&key, info + 0x20, 0, s.data() + meta + 0x40, h.data(), h.size());
    const uint32_t sections = be32(h.data() + 0x0C), keys = be32(h.data() + 0x10);
    const uint64_t key_base = 0x20 + (uint64_t)sections * 0x30;
    if (sections < 3 || key_base + (uint64_t)keys * 16 > h.size()) return false;
    const uint8_t* sec = h.data() + 0x20 + 2 * 0x30;
    const uint64_t off = be64(sec), len = be64(sec + 8);
    const uint32_t enc = be32(sec + 0x20), key_idx = be32(sec + 0x24), iv_idx = be32(sec + 0x28), comp = be32(sec + 0x2C);
    if (off > s.size() || len > s.size() - off || len > (uint64_t)(INT32_MAX / 4) || (enc == 3 && (key_idx >= keys || iv_idx >= keys))) return false;
    std::vector<uint8_t> data(s.begin() + (ptrdiff_t)off, s.begin() + (ptrdiff_t)(off + len));
    if (enc == 3) {
        ps3_aes128_init(&key, h.data() + key_base + (uint64_t)key_idx * 16);
        ps3_aes_ctr(&key, h.data() + key_base + (uint64_t)iv_idx * 16, 0, data.data(), data.data(), data.size());
    }
    if (comp != 2) {
        tar = std::move(data);
        return true;
    }
    int out = 0;
    char* raw = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(data.data()), (int)data.size(), (int)data.size() * 4, &out, 1);
    if (!raw) return false;
    tar.assign(raw, raw + out);
    std::free(raw);
    return true;
}

}

bool firmware_installed(const std::filesystem::path& flash)
{
    std::error_code ec;
    for (const char* f : k_files)
        if (!std::filesystem::is_regular_file(flash / f, ec) || ec) return false;
    return true;
}

bool firmware_extract(const std::filesystem::path& pup, const std::filesystem::path& flash, std::atomic<float>& progress, std::string& error,
                      const std::atomic<bool>* cancel)
{
    std::ifstream in(pup, std::ios::binary);
    uint8_t head[0x30];
    if (!in.read(reinterpret_cast<char*>(head), sizeof head) || std::memcmp(head, "SCEUF", 5)) {
        error = "Not a PS3UPDAT.PUP";
        return false;
    }
    uint64_t start = 0, length = 0;
    for (uint64_t i = 0, n = be64(head + 0x18); i < n && in; i++) {
        uint8_t e[0x20];
        if (!in.read(reinterpret_cast<char*>(e), sizeof e)) break;
        if (be64(e) == 0x300) {
            start = be64(e + 8);
            length = be64(e + 0x10);
        }
    }
    if (!in || !length) {
        error = "Damaged file";
        return false;
    }
    size_t found = 0;
    in.seekg((std::streamoff)start);
    std::error_code ec;
    for (uint64_t pos = 0; in && found < std::size(k_files) && pos + 512 <= length;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            error = "Cancelled";
            return false;
        }
        uint8_t hdr[512];
        tar_entry e;
        if (!in.read(reinterpret_cast<char*>(hdr), sizeof hdr) || !tar_header(hdr, e)) break;
        const uint64_t padded = (e.size + 511) & ~511ull;
        if (e.size > length - pos - 512 || padded > length - pos - 512) {
            error = "Damaged file";
            return false;
        }
        pos += 512 + padded;
        progress = (float)pos / (float)length;
        if (e.name.rfind("dev_flash_", 0) != 0) {
            in.seekg((std::streamoff)padded, std::ios::cur);
            continue;
        }
        if (e.size > (256ull << 20)) {
            error = "Damaged file";
            return false;
        }
        std::vector<uint8_t> pkg((size_t)e.size), tar;
        if (!in.read(reinterpret_cast<char*>(pkg.data()), (std::streamsize)e.size)) {
            error = "Damaged file";
            return false;
        }
        in.seekg((std::streamoff)(padded - e.size), std::ios::cur);
        if (!unpack(pkg, tar)) {
            error = "Damaged file";
            return false;
        }
        for (size_t at = 0; at + 512 <= tar.size();) {
            tar_entry f;
            if (!tar_header(tar.data() + at, f) || f.size > tar.size() - at - 512) break;
            for (const char* want : k_files)
                if (f.name == std::string("dev_flash/") + want) {
                    const std::filesystem::path dst = flash / want;
                    std::filesystem::create_directories(dst.parent_path(), ec);
                    std::ofstream out(dst, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(tar.data() + at + 512), (std::streamsize)f.size);
                    if (!out) {
                        error = "Write failed";
                        return false;
                    }
                    found++;
                }
            at += 512 + ((f.size + 511) & ~511ull);
        }
    }
    if (found < std::size(k_files)) {
        error = "Required files not found";
        return false;
    }
    progress = 1.f;
    return true;
}

namespace {

bool download_file(const char* url, uint64_t expected_size, const std::filesystem::path& dst, std::atomic<float>& progress, std::string& error,
                   const std::atomic<bool>* cancel)
{
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::remove(dst, ec);
    bool cancelled = false;
#ifdef _WIN32
    std::wstring cmd = L"curl.exe -fsSL --retry 3 -o \"" + dst.wstring() + L"\" " + std::filesystem::path(url).wstring();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = "curl not found";
        return false;
    }
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            cancelled = true;
            break;
        }
        ec.clear();
        const auto sz = std::filesystem::file_size(dst, ec);
        if (!ec) progress = std::clamp((float)sz / (float)expected_size, 0.f, 1.f);
    }
    DWORD code = 1;
    if (!cancelled) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    const std::string out = dst.string();
    const char* argv[] = { "curl", "-fsSL", "--retry", "3", "-o", out.c_str(), url, nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, "curl", nullptr, nullptr, const_cast<char* const*>(argv), environ)) {
        error = "curl not found";
        return false;
    }
    int status = 0;
    int code = 1;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r > 0) {
            code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            break;
        }
        if (r < 0 && errno != EINTR) break;
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            cancelled = true;
            break;
        }
        ec.clear();
        const auto sz = std::filesystem::file_size(dst, ec);
        if (!ec) progress = std::clamp((float)sz / (float)expected_size, 0.f, 1.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
    if (cancelled || code) {
        std::filesystem::remove(dst, ec);
        error = cancelled ? "Cancelled" : "Download failed";
        return false;
    }
    progress = 1.f;
    return true;
}

}

bool firmware_download(const std::filesystem::path& pup, std::atomic<float>& progress, std::string& error, const std::atomic<bool>* cancel)
{
    return download_file(k_url, k_size, pup, progress, error, cancel);
}

bool update_download(const std::filesystem::path& pkg, std::atomic<float>& progress, std::string& error,
                     const std::atomic<bool>* cancel, const std::string& title_id)
{
    const char* url = title_id == "BLUS30721" ? k_update_url_us : k_update_url_eu;
    return download_file(url, k_update_size, pkg, progress, error, cancel);
}