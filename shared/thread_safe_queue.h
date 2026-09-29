#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace wizard {

// Blocking, thread-safe FIFO queue. push() from any thread; pop()/pop_for()
// block until an item arrives (or times out). Keeps mutex/condition_variable
// logic in one place instead of scattered across callers.
//
//   ThreadSafeQueue<int> q;
//   q.push(42);
//   auto v = q.pop();                              // blocks
//   auto v2 = q.pop_for(std::chrono::seconds(1));   // times out
//
// close() signals "no more items will ever arrive": wakes every blocked
// pop(), and all later pop() calls return nullopt once drained.
template <typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    // Blocks until an item arrives or the queue closes.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
        return take_front_locked();
    }

    // Like pop(), but gives up after 'timeout'. nullopt on timeout or close.
    template <typename Rep, typename Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
        return take_front_locked();
    }

    // Drops whatever's queued right now (e.g. a stale response).
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }

    // Wakes every blocked pop(); future pops return nullopt once drained.
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    // Caller must hold mutex_.
    std::optional<T> take_front_locked() {
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

}  // namespace wizard