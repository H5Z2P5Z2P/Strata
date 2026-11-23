#pragma once
// Host memory lock (Linux; ported from nicejunjie/Strata-rdna3 86de9791 + 1097d7fa + 8b01b0c8).
// Included by the HIP shim (cuda_runtime.h) after HIP's __noinline__ macro cleanup so GCC 16 <format>/<chrono> build cleanly.
//
// Every block made through hip_compat::host_alloc / malloc_host (and verify.cpp's coherent hipHostMalloc) is GPU-mapped
// host memory (a KFD userptr BO). When Linux reclaim pages one out or kcompactd migrates it, KFD stops every queue of
// the process until fault-back completes. mlock'ing these blocks (plus sweeping ROCr's own shared-anon runtime blocks
// at READY and every 10 s) eliminates multi-second queue stalls under RAM/swap pressure.
// - The expert arena is NOT locked (mmap + register, >4 GiB).
// - Soft RLIMIT_MEMLOCK is raised to hard limit on first lock; if hard limit < 1 GiB nothing is locked and one line is printed.
// - A failed mlock never fails an allocation.
#include <hip/hip_runtime.h>
#include <hip/hip_version.h>
#ifdef __noinline__
#undef __noinline__
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/resource.h>
#endif

namespace strata::hip_compat {

struct HostLock {
    std::mutex mu;
    std::unordered_map<void*, size_t> blocks;   // locked block -> bytes (forgotten on free; unmap drops the lock)
    std::unordered_map<void*, size_t> failed;   // blocks whose mlock failed (counted once; sweep leaves them)
    uint64_t bytes = 0;
    uint64_t fail_bytes = 0;
    int fails = 0;
    int state = -1;                             // -1 not decided, 0 off, 1 on
    unsigned long long hard = 0;
};
inline HostLock& host_lock_state() { static HostLock s; return s; }

inline void host_lock_decide(HostLock& s) {   // under s.mu
    if (s.state >= 0) return;
#if !defined(_WIN32)
    rlimit rl{};
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0) { s.state = 0; return; }
    s.hard = rl.rlim_max == RLIM_INFINITY ? ~0ull : (unsigned long long) rl.rlim_max;
    if (s.hard < (1ull << 30)) {
        s.state = 0;
        std::fprintf(stderr, "strata: host memory lock: nothing locked (the RLIMIT_MEMLOCK hard limit is %llu KiB, under 1 GiB)\n",
                     s.hard >> 10);
        return;
    }
    if (rl.rlim_cur != rl.rlim_max) { rl.rlim_cur = rl.rlim_max; (void) setrlimit(RLIMIT_MEMLOCK, &rl); }
    s.state = 1;
#else
    s.state = 0;
#endif
}

inline void host_lock(void* p, size_t bytes) {
#if !defined(_WIN32)
    if (p == nullptr || bytes == 0) return;
    HostLock& s = host_lock_state();
    std::lock_guard<std::mutex> g(s.mu);
    host_lock_decide(s);
    if (s.state != 1) return;
    if (mlock(p, bytes) != 0) {
        const int e = errno;
        if (s.fails++ < 8)
            std::fprintf(stderr, "strata: host memory lock: a %zu KiB block stays unlocked (mlock: %s)\n", bytes >> 10,
                         std::strerror(e));
        s.fail_bytes += bytes;
        s.failed[p] = bytes;
        return;
    }
    s.blocks[p] = bytes;
    s.bytes += bytes;
#else
    (void) p; (void) bytes;
#endif
}

inline void host_lock_forget(void* p) {
    if (p == nullptr) return;
    HostLock& s = host_lock_state();
    std::lock_guard<std::mutex> g(s.mu);
    s.failed.erase(p);
    auto it = s.blocks.find(p);
    if (it == s.blocks.end()) return;
    s.bytes -= it->second;
    s.blocks.erase(it);
}

inline void host_lock_sweep_locked(HostLock& s, std::unordered_map<uintptr_t, std::pair<size_t, unsigned long long>>& seen, uint64_t& swept_bytes) {
#if !defined(_WIN32)
    if (s.state != 1) return;
    std::FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return;
    char line[512];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        unsigned long long a = 0, b = 0, off = 0, inode = 0; char perm[8] = {0}, dev[16] = {0};
        if (std::sscanf(line, "%llx-%llx %7s %llx %15s %llu", &a, &b, perm, &off, dev, &inode) != 6) continue;
        if (std::strcmp(perm, "rw-s") != 0 || std::strstr(line, "/dev/zero (deleted)") == nullptr) continue;
        const size_t bytes = (size_t) (b - a);
        if (bytes == 0 || bytes > ((size_t) 4 << 30)) continue;
        const std::pair<size_t, unsigned long long> id(bytes, inode);
        auto it = seen.find((uintptr_t) a);
        if (it != seen.end() && it->second == id) continue;
        bool failed_ours = false;
        for (auto& kv : s.failed)
            if ((uintptr_t) kv.first >= (uintptr_t) a && (uintptr_t) kv.first < (uintptr_t) b) { failed_ours = true; break; }
        if (failed_ours) { seen[(uintptr_t) a] = id; continue; }
        if (mlock((void*) (uintptr_t) a, bytes) != 0) {
            const int e = errno;
            if (s.fails++ < 8)
                std::fprintf(stderr, "strata: host memory lock: a %zu KiB runtime block stays unlocked (mlock: %s)\n",
                             bytes >> 10, std::strerror(e));
            s.fail_bytes += bytes;
            seen[(uintptr_t) a] = id;
            continue;
        }
        bool ours = false;
        for (auto& kv : s.blocks)
            if ((uintptr_t) kv.first >= (uintptr_t) a && (uintptr_t) kv.first < (uintptr_t) b) { ours = true; break; }
        seen[(uintptr_t) a] = id;
        if (!ours) swept_bytes += bytes;
    }
    std::fclose(f);
#else
    (void) s; (void) seen; (void) swept_bytes;
#endif
}

struct HostLockSweep { std::unordered_map<uintptr_t, std::pair<size_t, unsigned long long>> seen; uint64_t bytes = 0; bool thread = false; };
inline HostLockSweep& host_lock_sweep_state() { static HostLockSweep w; return w; }

inline void host_lock_report(const char* when) {
#if !defined(_WIN32)
    HostLock& s = host_lock_state();
    HostLockSweep& w = host_lock_sweep_state();
    std::lock_guard<std::mutex> g(s.mu);
    host_lock_decide(s);
    if (s.state != 1) return;
    host_lock_sweep_locked(s, w.seen, w.bytes);
    unsigned long long vmlck_kib = 0;
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f) != nullptr)
            if (std::strncmp(line, "VmLck:", 6) == 0) { vmlck_kib = std::strtoull(line + 6, nullptr, 10); break; }
        std::fclose(f);
    }
    char lim[32];
    if (s.hard == ~0ull) std::snprintf(lim, sizeof lim, "unlimited");
    else std::snprintf(lim, sizeof lim, "%.2f GiB", s.hard / 1073741824.0);
    std::fprintf(stderr,
                 "strata: host memory lock: %zu blocks, %.2f GiB locked, %.2f GiB runtime blocks, %d failures (%.2f GiB unlocked), "
                 "VmLck %.2f GiB, limit %s (%s)\n",
                 s.blocks.size(), s.bytes / 1073741824.0, w.bytes / 1073741824.0, s.fails, s.fail_bytes / 1073741824.0,
                 vmlck_kib / 1048576.0, lim, when);
    if (!w.thread) {
        w.thread = true;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                HostLock& s2 = host_lock_state();
                HostLockSweep& w2 = host_lock_sweep_state();
                std::lock_guard<std::mutex> g2(s2.mu);
                host_lock_sweep_locked(s2, w2.seen, w2.bytes);
            }
        }).detach();
    }
#else
    (void) when;
#endif
}

inline hipError_t host_free(void* p) {
    host_lock_forget(p);
#if HIP_VERSION_MAJOR < 7
    return hipHostFree(p);
#else
    return hipFreeHost(p);
#endif
}

}  // namespace strata::hip_compat
