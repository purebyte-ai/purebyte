#include "core/threads.h"

#include <algorithm>
#include <string>
#include <system_error>

#include "core/failure.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace pb {

namespace {

// A hint to the core that this thread is spinning (it lets an SMT sibling run and saves power).
inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#endif
}

constexpr int kSpins = 4000;  // about 0.1-0.5 ms of spinning before sleeping

}  // namespace

void Barrier::wait() {
    const unsigned generation = generation_.load(std::memory_order_acquire);
    if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == count_) {
        arrived_.store(0, std::memory_order_relaxed);
        {
            // Under the mutex, so that a waiter between its check and its sleep cannot miss the change.
            std::lock_guard<std::mutex> lock(mutex_);
            generation_.fetch_add(1, std::memory_order_release);
        }
        if (sleepers_.load(std::memory_order_acquire) > 0) wake_.notify_all();
        return;
    }
    for (int spins = 0; spins < kSpins; ++spins) {
        if (generation_.load(std::memory_order_acquire) != generation) return;
        cpu_relax();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    sleepers_.fetch_add(1, std::memory_order_acq_rel);
    wake_.wait(lock, [&] { return generation_.load(std::memory_order_acquire) != generation; });
    sleepers_.fetch_sub(1, std::memory_order_acq_rel);
}

std::pair<int, int> Team::share(int n, int align) const {
    const std::pair<int64_t, int64_t> s = share64(n, align);  // within [0, n]: fits an int
    return {static_cast<int>(s.first), static_cast<int>(s.second)};
}

std::pair<int64_t, int64_t> Team::share64(int64_t n, int64_t align) const {
    if (size <= 1) return {0, n};
    const int64_t blocks = (n + align - 1) / align;
    const int64_t per = blocks / size, extra = blocks % size;
    const int64_t first = member * per + std::min<int64_t>(member, extra);
    const int64_t count = per + (member < extra ? 1 : 0);
    return {std::min(n, first * align), std::min(n, (first + count) * align)};
}

ThreadPool::ThreadPool(int threads, CreationCheck check) : size_(std::max(1, threads)) {
    workers_.reserve(static_cast<size_t>(size_ - 1));  // no worker is started yet if this throws
    for (int i = 1; i < size_; ++i) {
        // A thread that cannot be created throws std::system_error. The members this constructor already built must
        // not be destroyed with joinable threads waiting on them (std::terminate): stop and join those first.
        std::string reason;
        try {
            if (check && !check(i))
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                        "refused by the test");
            workers_.emplace_back(&ThreadPool::worker, this, i);
            continue;
        } catch (const std::exception& e) {
            reason = e.what();
        } catch (...) {
            reason = "unknown error";
        }
        stop_workers();
        fail(PB_ERR_NO_MEMORY, format("could not start worker thread %d of %d (%s): the system is out of threads or "
                                      "memory; use fewer threads",
                                      i, size_ - 1, reason.c_str()));
    }
}

ThreadPool::~ThreadPool() { stop_workers(); }

void ThreadPool::stop_workers() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    start_.notify_all();
    for (std::thread& t : workers_) t.join();
    workers_.clear();
}

void ThreadPool::run(const std::function<void(int)>& task) {
    if (size_ == 1) {
        task(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_ = &task;
        pending_ = size_ - 1;
        error_ = nullptr;
        ++generation_;
    }
    start_.notify_all();
    std::exception_ptr own;
    try {
        task(0);
    } catch (...) {
        own = std::current_exception();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return pending_ == 0; });
    task_ = nullptr;
    if (own) std::rethrow_exception(own);
    if (error_) std::rethrow_exception(error_);
}

void ThreadPool::worker(int index) {
    unsigned seen = 0;
    for (;;) {
        const std::function<void(int)>* task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            start_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            task = task_;
        }
        std::exception_ptr failure;
        try {
            (*task)(index);
        } catch (...) {
            failure = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure && !error_) error_ = failure;
        if (--pending_ == 0) done_.notify_one();
    }
}

}  // namespace pb
