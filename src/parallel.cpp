// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "parallel.hpp"

#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace xisfconv {

namespace {

constexpr unsigned kMostThreads = 256;

// 0: not set for this thread (the cores of the machine).
thread_local unsigned t_limit = 0;

// One parallelFor: its tasks are taken by index, by the calling thread and by the threads of
// the pool that join in.
struct Batch {
    size_t count = 0;
    const ParallelTask* fn = nullptr;
    unsigned helpersWanted = 0;          // threads of the pool it can use
    unsigned helpers = 0;                // ... that joined (under the pool's lock)
    unsigned inside = 0;                 // ... and are still at it (under the pool's lock)
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> abandon{false};    // runBeside: the call was stopped while the other thing ran
    std::mutex failure;
    size_t failedIndex = std::numeric_limits<size_t>::max();
    std::exception_ptr error;
};

// Runs tasks of the batch until there are none left.
void work(Batch& b, unsigned slot) {
    const unsigned outer = t_limit;
    t_limit = 1;   // (what a task spreads, it does itself)
    const AbandonScope abandon(&b.abandon);
    while (!b.stop.load(std::memory_order_relaxed)) {
        const size_t i = b.next.fetch_add(1);
        if (i >= b.count) break;
        try {
            (*b.fn)(i, slot);
        } catch (...) {
            std::lock_guard<std::mutex> lock(b.failure);
            if (i < b.failedIndex) {
                b.failedIndex = i;
                b.error = std::current_exception();
            }
            b.stop.store(true);
        }
    }
    t_limit = outer;
}

// The threads that help. They are made as they are needed, and live as long as the process: a
// library is unloaded, and a process ends, with them waiting for work, never in it.
struct Pool {
    std::mutex lock;
    std::condition_variable wake;    // work has come
    std::condition_variable left;    // a helper has left a batch
    std::deque<Batch*> batches;
    unsigned threads = 0;
    bool cannotGrow = false;         // the system made no more threads

    void grow(unsigned wanted) {     // (under the lock)
        while (threads < wanted && !cannotGrow) {
            try {
                std::thread([this] { helper(); }).detach();
                ++threads;
            } catch (...) {
                cannotGrow = true;   // fewer helpers, then; the calling thread does what is left
            }
        }
    }

    void helper() {
        std::unique_lock<std::mutex> guard(lock);
        for (;;) {
            Batch* b = nullptr;
            for (Batch* candidate : batches) {
                if (candidate->helpers < candidate->helpersWanted && candidate->next.load() < candidate->count &&
                    !candidate->stop.load()) {
                    b = candidate;
                    break;
                }
            }
            if (!b) {
                wake.wait(guard);
                continue;
            }
            const unsigned slot = ++b->helpers;
            ++b->inside;
            guard.unlock();
            work(*b, slot);
            guard.lock();
            if (--b->inside == 0) left.notify_all();
        }
    }
};

std::once_flag g_once;
Pool* g_pool = nullptr;

#if !defined(_WIN32)
// A child of fork() has none of the threads, and may have the lock as one of them held it:
// it starts a pool of its own. (The old one is left as it is: its memory is not the child's
// to free.)
void forkedChild() {
    if (g_pool) g_pool = new Pool;
}
#endif

Pool& pool() {
    std::call_once(g_once, [] {
        g_pool = new Pool;
#if !defined(_WIN32)
        pthread_atfork(nullptr, nullptr, forkedChild);
#endif
    });
    return *g_pool;
}

}  // namespace

unsigned hardwareThreads() {
    static const unsigned n = [] {
        const unsigned cores = std::thread::hardware_concurrency();
        return std::max(1u, std::min(cores, kMostThreads));
    }();
    return n;
}

unsigned threadLimit() { return t_limit == 0 ? hardwareThreads() : t_limit; }

ThreadScope::ThreadScope(unsigned threads) : previous_(t_limit) { t_limit = std::min(threads, kMostThreads); }
ThreadScope::~ThreadScope() { t_limit = previous_; }

void parallelFor(size_t count, const ParallelTask& fn) {
    if (count == 0) return;
    const unsigned threads = static_cast<unsigned>(std::min<size_t>(threadLimit(), count));
    Batch b;
    b.count = count;
    b.fn = &fn;
    if (threads > 1) {
        b.helpersWanted = threads - 1;
        Pool& p = pool();
        {
            std::lock_guard<std::mutex> guard(p.lock);
            p.grow(std::max(p.threads, b.helpersWanted));
            p.batches.push_back(&b);
        }
        p.wake.notify_all();
        work(b, 0);
        std::unique_lock<std::mutex> guard(p.lock);
        p.batches.erase(std::find(p.batches.begin(), p.batches.end(), &b));
        p.left.wait(guard, [&] { return b.inside == 0; });
    } else {
        work(b, 0);
    }
    if (b.error) std::rethrow_exception(b.error);
}

void runBeside(const std::function<void()>& here, const std::function<void()>& beside) {
    const unsigned threads = threadLimit();
    if (threads <= 1) {
        beside();
        here();
        return;
    }
    const ParallelTask task = [&](size_t, unsigned) { beside(); };
    Batch b;
    b.count = 1;
    b.fn = &task;
    b.helpersWanted = 1;
    Pool& p = pool();
    {
        std::lock_guard<std::mutex> guard(p.lock);
        p.grow(std::max(p.threads, 1u));
        p.batches.push_back(&b);
    }
    p.wake.notify_all();
    std::exception_ptr mine;
    try {
        const ThreadScope rest(threads - 1);   // (one thread does the other thing)
        here();
    } catch (const Error& e) {
        mine = std::current_exception();
        // A call that was stopped stops the other thing too (the request is used up here: the
        // handler does not ask a second time).
        if (e.kind == ErrorKind::Cancelled) b.abandon.store(true);
    } catch (...) {
        mine = std::current_exception();
    }
    work(b, 0);   // (what no thread of the pool took yet, the calling thread does now)
    {
        // Waiting for the other thing, the calling thread gives signs of life, where the call may
        // be stopped; the other thing then stops at its next one.
        std::unique_lock<std::mutex> guard(p.lock);
        p.batches.erase(std::find(p.batches.begin(), p.batches.end(), &b));
        while (!p.left.wait_for(guard, std::chrono::milliseconds(250), [&] { return b.inside == 0; })) {
            if (b.abandon.load()) continue;
            guard.unlock();
            try {
                progressAgain();
            } catch (...) {
                if (!mine) mine = std::current_exception();
                b.abandon.store(true);
            }
            guard.lock();
        }
    }
    if (b.abandon.load() && mine) std::rethrow_exception(mine);   // (stopped: whatever the other thing came to)
    if (b.error) std::rethrow_exception(b.error);
    if (mine) std::rethrow_exception(mine);
}

void parallelPieces(size_t items, size_t minimum, const std::function<void(size_t begin, size_t end)>& fn) {
    const size_t pieces = pieceCount(items, minimum);
    if (pieces <= 1) {
        if (items) fn(0, items);
        return;
    }
    parallelFor(pieces, [&](size_t k, unsigned) { fn(items / pieces * k + std::min(k, items % pieces),
                                                     items / pieces * (k + 1) + std::min(k + 1, items % pieces)); });
}

size_t pieceCount(size_t items, size_t minimum) {
    const size_t threads = threadLimit();
    if (threads <= 1 || items < 2 * std::max<size_t>(minimum, 1)) return 1;
    return std::min(threads, items / std::max<size_t>(minimum, 1));
}

}  // namespace xisfconv
