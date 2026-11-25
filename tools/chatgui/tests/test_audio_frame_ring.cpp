/* Целые кадры, переполнение, ошибочные аргументы и конкурентные producers/consumer. */
#include "../src/audio_frame_ring.h"
#include <QtTest>
#include <QScopeGuard>
#include <atomic>
#include <thread>

class AudioFrameRingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_enable_console(1); debug_set_level(DEBUG_LEVEL_WARN); }

    void invalidGeometry() {
        for (int capacity : {-1, 0, 1}) for (int slot : {-1, 0, 1}) {
            if (capacity > 0 && slot > 0) continue;
            QVERIFY_EXCEPTION_THROWN((void)AudioFrameRing(capacity, slot), std::invalid_argument);
        }
    }

    void overflowDropsOnlyOldest() {
        AudioFrameRing ring(2, 1);
        for (uint8_t value : {1, 2, 3}) QVERIFY(ring.push(&value, 1));
        QCOMPARE(ring.size(), 2); QCOMPARE(ring.capacity(), 2);
        uint8_t value = 0;
        QCOMPARE(ring.pop(&value, 1), 1); QCOMPARE(value, uint8_t(2));
        QCOMPARE(ring.pop(&value, 1), 1); QCOMPARE(value, uint8_t(3));
        QCOMPARE(ring.pop(nullptr, 0), 0);
    }

    void rejectsArgumentsWithoutConsumingFrame() {
        AudioFrameRing ring(3, 4);
        uint8_t data[] = {1, 2, 3, 4};
        QVERIFY(ring.push(data, 4));
        QVERIFY(!ring.push(nullptr, 1)); QVERIFY(!ring.push(data, 0));
        QVERIFY(!ring.push(data, -1)); QVERIFY(!ring.push(data, 5));
        QCOMPARE(ring.pop(nullptr, 4), -1);
        uint8_t output[] = {9, 9, 9, 9};
        QCOMPARE(ring.pop(output, 3), -1); QCOMPARE(output[0], uint8_t(9));
        QCOMPARE(ring.size(), 1);
        QCOMPARE(ring.pop(output, 4), 4); QCOMPARE(memcmp(output, data, 4), 0);
    }

    void preservesLengthBeyondUint16() {
        AudioFrameRing ring(2, 70000);
        const std::vector<uint8_t> data(65536, 0x5a);
        std::vector<uint8_t> output(data.size() + 1, 0xa5);
        QVERIFY(ring.push(data.data(), int(data.size())));
        QCOMPARE(ring.pop(output.data(), int(data.size())), int(data.size()));
        QCOMPARE(memcmp(data.data(), output.data(), data.size()), 0);
        QCOMPARE(output.back(), uint8_t(0xa5));
    }

    void clearAfterWrappedQueue() {
        AudioFrameRing ring(3, 1);
        uint8_t value = 7;
        for (int i = 0; i < 8; ++i) QVERIFY(ring.push(&value, 1));
        ring.clear(); QCOMPARE(ring.size(), 0); QCOMPARE(ring.pop(&value, 1), 0);
        value = 9; QVERIFY(ring.push(&value, 1)); value = 0;
        QCOMPARE(ring.pop(&value, 1), 1); QCOMPARE(value, uint8_t(9));
    }

    void concurrentFrameIntegrity() {
        constexpr int producers = 4, perProducer = 1000, total = producers * perProducer;
        AudioFrameRing ring(total, sizeof(uint32_t));
        std::atomic<int> done{0};
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;
        const auto join = qScopeGuard([&] { for (auto& thread : threads) if (thread.joinable()) thread.join(); });
        for (int producer = 0; producer < producers; ++producer) threads.emplace_back([&, producer] {
            for (uint32_t i = 0; i < perProducer; ++i) {
                const uint32_t value = producer * perProducer + i;
                if (!ring.push(reinterpret_cast<const uint8_t*>(&value), sizeof(value))) failed = true;
            }
            ++done;
        });
        std::vector<bool> seen(total, false);
        int received = 0;
        while (done.load() < producers || ring.size()) {
            uint32_t value = 0;
            const int length = ring.pop(reinterpret_cast<uint8_t*>(&value), sizeof(value));
            if (!length) { std::this_thread::yield(); continue; }
            QCOMPARE(length, int(sizeof(value))); QVERIFY(value < total); QVERIFY(!seen[value]);
            seen[value] = true; ++received;
        }
        QVERIFY(!failed.load()); QCOMPARE(received, total);
    }
};

QTEST_GUILESS_MAIN(AudioFrameRingTest)
#include "test_audio_frame_ring.moc"
