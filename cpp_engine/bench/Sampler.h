//
// A minimal sampling profiler for Windows, where perf does not exist and
// MinGW's gprof records call counts but no time samples.
//
// A background thread suspends the profiled thread at ~10 kHz, reads its
// instruction pointer, and resumes it. Samples go into a preallocated buffer:
// the sampler must never allocate, because the suspended thread may be
// holding the heap lock. Afterwards the histogram of addresses is written as
// "address count" lines; bench/symbolize.py maps them to functions with
// addr2line. Link the profiled binary with -Wl,--disable-dynamicbase so
// runtime addresses equal the addresses in the debug info.
//

#ifndef LIMIT_ORDER_BOOK_SAMPLER_H
#define LIMIT_ORDER_BOOK_SAMPLER_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

class Sampler {
public:
    // Samples the calling thread until stop().
    explicit Sampler(const std::size_t capacity = 4'000'000) : samples(capacity) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        worker = std::thread([this] { loop(); });
    }

    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;

    ~Sampler() {
        stop();
        CloseHandle(target);
    }

    void stop() {
        running.store(false);
        if (worker.joinable()) worker.join();
    }

    [[nodiscard]] std::size_t size() const { return count; }

    // "hex-address count" per line, most-sampled first.
    void write(const std::string &path) {
        std::vector<std::uint64_t> sorted(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(count));
        std::sort(sorted.begin(), sorted.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> histogram; // (count, address)
        for (std::size_t i = 0; i < sorted.size();) {
            std::size_t j = i;
            while (j < sorted.size() && sorted[j] == sorted[i]) ++j;
            histogram.emplace_back(j - i, sorted[i]);
            i = j;
        }
        std::sort(histogram.rbegin(), histogram.rend());
        std::ofstream out(path);
        for (const auto &[n, address]: histogram) out << std::hex << "0x" << address << std::dec << " " << n << "\n";
    }

private:
    void loop() {
        LARGE_INTEGER freq, now, next;
        QueryPerformanceFrequency(&freq);
        const LONGLONG period = freq.QuadPart / 10'000; // 100 us
        QueryPerformanceCounter(&next);
        while (running.load() && count < samples.size()) {
            next.QuadPart += period;
            do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart); // spin: Sleep is 1 ms at best
            if (SuspendThread(target) == static_cast<DWORD>(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(target, &ctx)) samples[count++] = ctx.Rip;
            ResumeThread(target);
        }
    }

    std::vector<std::uint64_t> samples;
    std::size_t count = 0;
    HANDLE target = nullptr;
    std::atomic<bool> running{true};
    std::thread worker;
};

#endif // _WIN32

#endif //LIMIT_ORDER_BOOK_SAMPLER_H
