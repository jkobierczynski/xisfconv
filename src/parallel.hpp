// Work spread over the cores of the machine: one pool of threads for the whole library.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstddef>
#include <functional>

namespace xisfconv {

// The cores of the machine, as the system tells them (at least 1).
unsigned hardwareThreads();

// The threads the work of this thread is spread over, the thread itself among them (at least
// 1). It is set for a call (ThreadScope: a call of the C API sets that of its context); 0 there
// is as many as the machine has cores. Work that runs on the pool spreads no further: inside it
// this is 1.
unsigned threadLimit();

class ThreadScope {
public:
    explicit ThreadScope(unsigned threads);   // 0: the cores of the machine
    ~ThreadScope();
    ThreadScope(const ThreadScope&) = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;

private:
    unsigned previous_;
};

// fn(index, slot) for each index of [0, count), on up to threadLimit() threads at once, the
// calling one among them; slot (below threadLimit()) is the one of the thread that runs it, for
// state of its own (a compressor), which no other task uses at the same time. Returns when all
// are done. What runs does not report progress or warn (that is the calling thread's): it
// computes. If one throws, the indices not yet begun are left, and the exception of the lowest
// index that threw is thrown here, so that the same input fails the same way at every number of
// threads.
using ParallelTask = std::function<void(size_t index, unsigned slot)>;
void parallelFor(size_t count, const ParallelTask& fn);

// `here` on the calling thread (which may spread its work over the other threads but one), and
// `beside` on a thread of the pool at the same time. Returns when both are done. With one thread,
// `beside` is done first, then `here`. If both throw, the exception of `beside` is the one that
// comes out (a checksum that does not match is what a reader is told first). `beside` does not
// report progress or warn.
void runBeside(const std::function<void()>& here, const std::function<void()>& beside);

// fn(begin, end) for the pieces of [0, items), side by side: for work on each item by itself,
// where it does not matter how the items are cut. A piece has at least `minimum` items.
void parallelPieces(size_t items, size_t minimum, const std::function<void(size_t begin, size_t end)>& fn);

// Pieces of at least `minimum` items for `items` items, about as many as there are threads to
// do them (1 if the threads are 1, or the items too few).
size_t pieceCount(size_t items, size_t minimum);

}  // namespace xisfconv
