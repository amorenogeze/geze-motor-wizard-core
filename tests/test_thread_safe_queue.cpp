#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "thread_safe_queue.h"

using namespace wizard;

TEST(ThreadSafeQueue, PushThenPopReturnsItem) {
    ThreadSafeQueue<int> q;
    q.push(42);
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(ThreadSafeQueue, PopIsFifo) {
    ThreadSafeQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    EXPECT_EQ(*q.pop(), 1);
    EXPECT_EQ(*q.pop(), 2);
    EXPECT_EQ(*q.pop(), 3);
}

TEST(ThreadSafeQueue, PopBlocksUntilPush) {
    ThreadSafeQueue<int> q;
    std::thread producer([&q] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        q.push(7);
    });
    auto v = q.pop();  // should block ~50ms, not return immediately
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 7);
    producer.join();
}

TEST(ThreadSafeQueue, PopForTimesOutWhenEmpty) {
    ThreadSafeQueue<int> q;
    auto v = q.pop_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(v.has_value());
}

TEST(ThreadSafeQueue, PopForReturnsItemBeforeTimeout) {
    ThreadSafeQueue<int> q;
    q.push(9);
    auto v = q.pop_for(std::chrono::seconds(1));
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 9);
}

TEST(ThreadSafeQueue, ClearDiscardsQueuedItems) {
    ThreadSafeQueue<int> q;
    q.push(1);
    q.push(2);
    q.clear();
    auto v = q.pop_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(v.has_value());
}

TEST(ThreadSafeQueue, CloseWakesBlockedPop) {
    ThreadSafeQueue<int> q;
    std::thread closer([&q] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        q.close();
    });
    auto v = q.pop();  // should wake up on close(), not hang forever
    EXPECT_FALSE(v.has_value());
    EXPECT_TRUE(q.is_closed());
    closer.join();
}

TEST(ThreadSafeQueue, PopAfterCloseDrainsRemainingItemsFirst) {
    ThreadSafeQueue<int> q;
    q.push(1);
    q.close();
    // Items already queued before close() are still delivered...
    auto v1 = q.pop();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1);
    // ...then pop() returns nullopt once drained.
    auto v2 = q.pop();
    EXPECT_FALSE(v2.has_value());
}