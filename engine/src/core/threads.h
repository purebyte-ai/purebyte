// Persistent worker threads and the synchronisation of a TEAM of threads that runs one window together.
//
// A session owns one ThreadPool for its lifetime (threads are created once, not per call). A batch of windows is run
// by splitting the pool into teams: a team of one thread runs whole windows; a larger team runs each window SPMD-style
// (every member executes the same forward, takes its share of each stage and meets the others at a barrier).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pb {

// Reusable barrier for a fixed number of threads. Waiters spin briefly (most stages end within microseconds of each
// other) and then sleep until the last one arrives. Sleeping, not yielding: a thread that yields can hand its core to
// any other runnable thread, lower priority included, and wait up to a scheduler tick to get it back, which on a busy
// machine made every barrier of a window cost milliseconds.
class Barrier {
public:
    explicit Barrier(int count) : count_(count) {}
    void wait();

private:
    const int count_;
    std::atomic<int> arrived_{0};
    std::atomic<unsigned> generation_{0};
    std::atomic<int> sleepers_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
};

// The view one thread has of its team.
struct Team {
    int member = 0;  // 0 is the leader
    int size = 1;
    Barrier* barrier = nullptr;  // shared by the members; unused when size == 1

    void sync() const {
        if (size > 1) barrier->wait();
    }
    bool leader() const { return member == 0; }
    // This member's share [begin, end) of n items; inner boundaries fall on multiples of `align`.
    std::pair<int, int> share(int n, int align = 1) const;
    // The same for a count that may not fit an int (heads x positions of a long window), computed in 64 bits.
    std::pair<int64_t, int64_t> share64(int64_t n, int64_t align = 1) const;
};

class ThreadPool {
public:
    // Tests only: called before worker `index` is created; returning false makes that creation fail as a thread or
    // memory limit of the system would.
    using CreationCheck = bool (*)(int index);

    // threads - 1 workers are started; the calling thread is thread 0. When the system cannot start a worker (a limit
    // on threads or memory), the workers already started are stopped and joined and Failure(PB_ERR_NO_MEMORY) is
    // thrown: the process is never terminated.
    explicit ThreadPool(int threads, CreationCheck check = nullptr);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int size() const { return size_; }
    // Runs task(i) on threads i = 0 .. size() - 1 at once (i = 0 on the caller) and returns when all have finished.
    // The first exception thrown by any of them is rethrown here.
    void run(const std::function<void(int)>& task);

private:
    void worker(int index);
    void stop_workers();  // asks every started worker to return, and joins it

    const int size_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    const std::function<void(int)>* task_ = nullptr;
    unsigned generation_ = 0;
    int pending_ = 0;
    bool stop_ = false;
    std::exception_ptr error_;
};

}  // namespace pb
