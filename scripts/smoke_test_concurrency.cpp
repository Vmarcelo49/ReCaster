// Smoke test for concurrency.hpp: BlockingQueue with jthread + stop_token.
//
// Compile and run (native, no cross-compile needed):
//   g++ -std=c++23 -pthread -Isrc/common scripts/smoke_test_concurrency.cpp -o /tmp/smoke_concurrency && /tmp/smoke_concurrency
//
// Tests:
//   1. Basic push / try_pop ordering (FIFO)
//   2. try_pop returns false on empty
//   3. wait_and_pop blocks until item arrives
//   4. wait_and_pop returns false on stop_token (clean shutdown)
//   5. Producer/consumer: 1 producer + 1 consumer jthread, 1000 items
//   6. Multiple producers (2) + 1 consumer
//   7. clear() empties the queue
//   8. No deadlock when stopping with empty queue

#include "concurrency.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "  OK   " : "  FAIL ", label);
    if (!ok) ++failures;
}

int main() {
    using caster::common::concurrency::BlockingQueue;

    // ---- Test 1: Basic FIFO ordering ----
    {
        BlockingQueue<int> q;
        q.push(1);
        q.push(2);
        q.push(3);
        int a = 0, b = 0, c = 0;
        check(q.try_pop(a), "FIFO: pop 1");
        check(q.try_pop(b), "FIFO: pop 2");
        check(q.try_pop(c), "FIFO: pop 3");
        check(a == 1 && b == 2 && c == 3, "FIFO: order preserved");
        check(!q.try_pop(a), "FIFO: empty after drain");
    }

    // ---- Test 2: try_pop returns false on empty ----
    {
        BlockingQueue<int> q;
        int out;
        check(!q.try_pop(out), "try_pop empty: returns false");
        check(q.try_pop() == std::nullopt, "try_pop() optional: nullopt on empty");
    }

    // ---- Test 3: wait_and_pop blocks until item arrives ----
    {
        BlockingQueue<int> q;
        std::jthread producer([&q] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            q.push(42);
        });
        int out = 0;
        std::stop_token st;
        // Use a dummy stop_token that's never stopped
        bool ok = q.wait_and_pop(out, st);
        check(ok, "wait_and_pop: returns true when item arrives");
        check(out == 42, "wait_and_pop: correct value");
    }

    // ---- Test 4: wait_and_pop returns false on stop_token ----
    {
        BlockingQueue<int> q;
        std::jthread worker([&q](std::stop_token st) {
            int out;
            bool ok = q.wait_and_pop(out, st);
            check(!ok, "wait_and_pop: returns false on stop");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        worker.request_stop();
        // worker joins on destruction
    }

    // ---- Test 5: No deadlock when stopping with empty queue ----
    {
        BlockingQueue<int> q;
        std::jthread worker([&q](std::stop_token st) {
            int out;
            // This should return immediately on stop, not deadlock
            q.wait_and_pop(out, st);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        worker.request_stop();
        // If we reach here, no deadlock
        check(true, "no deadlock on stop with empty queue");
    }

    // ---- Test 6: Producer/consumer, 1000 items ----
    {
        BlockingQueue<int> q;
        std::atomic<int> sum{0};
        std::atomic<int> count{0};

        std::jthread consumer([&](std::stop_token st) {
            int item;
            while (q.wait_and_pop(item, st)) {
                sum += item;
                ++count;
            }
            // Drain any remaining items after stop
            while (q.try_pop(item)) {
                sum += item;
                ++count;
            }
        });

        constexpr int N = 1000;
        for (int i = 1; i <= N; ++i) {
            q.push(i);
        }

        // Wait for consumer to process all items
        while (count.load() < N) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        consumer.request_stop();
        // consumer joins on destruction

        int expected = N * (N + 1) / 2;
        check(count.load() == N, "producer/consumer: all items consumed");
        check(sum.load() == expected, "producer/consumer: sum correct");
    }

    // ---- Test 7: Multiple producers, 1 consumer ----
    {
        BlockingQueue<int> q;
        std::atomic<int> total{0};

        std::jthread consumer([&](std::stop_token st) {
            int item;
            while (q.wait_and_pop(item, st)) {
                total += item;
            }
            while (q.try_pop(item)) {
                total += item;
            }
        });

        constexpr int PER_PRODUCER = 500;
        std::jthread p1([&q] {
            for (int i = 1; i <= PER_PRODUCER; ++i) q.push(i);
        });
        std::jthread p2([&q] {
            for (int i = 1; i <= PER_PRODUCER; ++i) q.push(i * 10);
        });
        p1.join();
        p2.join();

        int expected = (PER_PRODUCER * (PER_PRODUCER + 1) / 2)  // p1: 1..500
                     + (10 * PER_PRODUCER * (PER_PRODUCER + 1) / 2);  // p2: 10..5000

        // Wait for consumer to finish
        while (total.load() < expected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        consumer.request_stop();

        check(total.load() == expected, "multi-producer: total correct");
    }

    // ---- Test 8: clear() empties the queue ----
    {
        BlockingQueue<int> q;
        q.push(1);
        q.push(2);
        q.push(3);
        check(q.size() == 3, "clear: size before");
        q.clear();
        check(q.empty(), "clear: empty after");
        check(q.size() == 0, "clear: size 0 after");
    }

    // ---- Test 9: Move semantics (non-copyable items) ----
    {
        BlockingQueue<std::unique_ptr<int>> q;
        q.push(std::make_unique<int>(99));
        std::unique_ptr<int> out;
        check(q.try_pop(out), "move: pop unique_ptr");
        check(out && *out == 99, "move: value correct");
    }

    // ---- Summary ----
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
