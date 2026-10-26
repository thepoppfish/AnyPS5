#include "prx/libSceRudp/include/Api.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace {
thread_local int allocationsUntilFailure = -1;
thread_local bool faultFired = false;
thread_local std::size_t failedBytes = 0;
int checks = 0;
int failures = 0;

void Require(bool value) {
    ++checks;
    if (!value) ++failures;
}
}

void* operator new(std::size_t bytes) {
    if (allocationsUntilFailure >= 0 && allocationsUntilFailure-- == 0) {
        allocationsUntilFailure = -1;
        faultFired = true;
        failedBytes = bytes;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

extern "C" [[noreturn]] void NotImplemented_nid_no_patch(const char*) { std::abort(); }

int main() {
    if (TimedWait::Coarse()) return 77;
    constexpr int cancelled = static_cast<int>(0x80770014u);
    alignas(16) std::array<unsigned char, 0x10000> pool{};
    Require(sceRudpInit_nid_postfix(pool.data(), static_cast<int>(pool.size())) == 0);
    int cases = 0;
    for (const auto timeout : {1000000ull, std::numeric_limits<std::uint64_t>::max()}) {
        for (const int failAfter : {0, 1}) {
            const int poll = sceRudpPollCreate(1);
            Require(poll > 0);
            std::promise<bool> reached;
            auto reachedFuture = reached.get_future();
            std::mutex releaseMutex;
            std::condition_variable releaseChanged;
            bool release = false;
            bool caught = false;
            std::size_t faultSize = 0;
            std::thread waiter([&] {
                std::array<RudpPollEvent, 1> events{};
                allocationsUntilFailure = failAfter;
                try {
                    static_cast<void>(sceRudpPollWait(poll, events.data(), events.size(), timeout));
                } catch (const std::bad_alloc&) {
                    caught = faultFired;
                    faultSize = failedBytes;
                } catch (...) {}
                allocationsUntilFailure = -1;
                reached.set_value(caught);
                std::unique_lock lock(releaseMutex);
                releaseChanged.wait(lock, [&] { return release; });
            });
            const bool entered = reachedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            Require(entered);
            if (!entered) {
                static_cast<void>(sceRudpPollDestroy(poll));
                {
                    std::lock_guard lock(releaseMutex);
                    release = true;
                }
                releaseChanged.notify_all();
                waiter.join();
                continue;
            }
            Require(reachedFuture.get());
            std::promise<int> completed;
            auto completedFuture = completed.get_future();
            std::thread cancel([&] { completed.set_value(sceRudpPollCancel(poll)); });
            const bool drained = completedFuture.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
            Require(drained);
            if (drained) {
                Require(completedFuture.get() == 0);
                std::array<RudpPollEvent, 1> events{};
                Require(sceRudpPollWait(poll, events.data(), events.size(), 0) == cancelled);
                Require(sceRudpPollWait(poll, events.data(), events.size(), 0) == 0);
            }
            Require(sceRudpPollDestroy(poll) == 0);
            if (!drained) Require(completedFuture.get() == 0);
            cancel.join();
            {
                std::lock_guard lock(releaseMutex);
                release = true;
            }
            releaseChanged.notify_all();
            waiter.join();
            ++cases;
            std::printf("{\"timeout\":%llu,\"fail_after\":%d,\"caught_bad_alloc\":%s,\"allocation_bytes\":%zu,\"cancel_finished_before_destroy\":%s}\n",
                static_cast<unsigned long long>(timeout), failAfter, caught ? "true" : "false", faultSize, drained ? "true" : "false");
        }
    }
    Require(sceRudpEnd() == 0);
    std::printf("{\"cases\":%d,\"checks\":%d,\"failures\":%d}\n", cases, checks, failures);
    return failures == 0 ? 0 : 1;
}
