/* MPSC: резерв управляющих сообщений, публикация слота и порядок каждого producer. */
#include "../src/audio_event_queue.h"
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "queue:%d %s\n", __LINE__, #x); abort(); } } while (0)

static std::atomic<bool> entered{false}, releaseCopy{false};
struct Item {
    int producer = 0, sequence = 0;
    bool block = false;
    Item& operator=(const Item& other) {
        if (other.block) { entered = true; while (!releaseCopy) std::this_thread::yield(); }
        producer = other.producer; sequence = other.sequence; block = other.block; return *this;
    }
};
int main() {
    AudioEventQueue<Item, 16> queue;
    Item item{};
    for (int i = 0; i < 8; ++i) REQUIRE(queue.push(item));
    REQUIRE(!queue.push(item));
    for (int i = 0; i < 8; ++i) REQUIRE(queue.push(item, true));
    REQUIRE(!queue.push(item, true));
    for (int i = 0; i < 16; ++i) REQUIRE(queue.pop(item));
    REQUIRE(!queue.pop(item));
    std::thread first([&] { REQUIRE(queue.push({0, 123, true})); });
    while (!entered) std::this_thread::yield();
    REQUIRE(queue.push({1, 456, false}, true));
    REQUIRE(!queue.pop(item)); // Второй слот опубликован, первый ещё принадлежит producer.
    releaseCopy = true; first.join();
    REQUIRE(queue.pop(item) && item.sequence == 123);
    REQUIRE(queue.pop(item) && item.sequence == 456);
    queue.reset();
    constexpr int producers = 4, count = 20000;
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) threads.emplace_back([&, p] {
        for (int i = 0; i < count; ++i) while (!queue.push({p, i, false}, i % 7 == 0)) std::this_thread::yield();
    });
    int expected[producers]{};
    for (int total = 0; total < producers * count;) {
        if (!queue.pop(item)) { std::this_thread::yield(); continue; }
        REQUIRE(queue.size() <= 16);
        REQUIRE(item.producer >= 0 && item.producer < producers);
        REQUIRE(item.sequence == expected[item.producer]++); ++total;
    }
    for (auto& thread : threads) thread.join();
    REQUIRE(queue.size() == 0);
    puts("audio queue: control reserve, unpublished slot, 80000 ordered messages PASS");
}
