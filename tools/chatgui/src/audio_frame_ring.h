#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "../../../lib/debug_config.h"

/* Кольцевой буфер непрозрачных кадров переменной длины под mutex.
 *
 * push/pop/clear/size допускаются из разных потоков, включая нескольких producers/consumers.
 * Данные копируются: буферы вызывающего нужны только до возврата вызова.
 * Два вектора (данные и длины) выделяются конструктором, без аллокаций на кадр.
 * Переполнение штатно заменяет старейший кадр. Инициализация/clear логируются в MEDIA/DEBUG,
 * неверные аргументы — MEDIA/ERROR. Перед уничтожением остановить всех пользователей.
 */
class AudioFrameRing {
public:
    // capacity/slotSize > 0; неверные размеры дают invalid_argument, переполнение — length_error.
    // Ошибка выделения памяти даёт стандартное исключение vector; все отказы логируются.
    AudioFrameRing(int capacity, int slotSize) try
        : cap_(capacity), slotSize_(slotSize),
          buf_(buffer_size(capacity, slotSize), 0), lens_((size_t)capacity, 0) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "AudioFrameRing: initialized capacity=%d slot_size=%d bytes=%zu", capacity, slotSize, buf_.size());
    } catch (const std::exception& error) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "AudioFrameRing: initialization failed capacity=%d slot_size=%d: %s",
                    capacity, slotSize, error.what());
        throw;
    }

    /* Скопировать data[0..len), 1<=len<=slotSize; false при NULL/неверной длине, очередь не меняется.
     * При переполнении дропает старейший кадр (true). */
    bool push(const uint8_t* data, int len) {
        if (!data || len <= 0 || len > slotSize_) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "AudioFrameRing: push rejected data_present=%d len=%d slot_size=%d", data != nullptr, len, slotSize_);
            return false;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (count_ == cap_) {
            head_ = (head_ + 1) % cap_;   /* drop oldest */
            count_--;
        }
        std::memcpy(buf_.data() + (size_t)tail_ * slotSize_, data, (size_t)len);
        lens_[(size_t)tail_] = len;
        tail_ = (tail_ + 1) % cap_;
        count_++;
        return true;
    }

    /* Скопировать старейший кадр и удалить его, вернуть положительную длину; 0 — пусто.
     * NULL out/недостаточный outCap дают -1, кадр остаётся для повторной попытки. */
    int pop(uint8_t* out, int outCap) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (count_ == 0) return 0;
        int len = (int)lens_[(size_t)head_];
        if (!out || outCap < len) {
            lk.unlock();
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "AudioFrameRing: pop rejected output_present=%d capacity=%d required=%d", out != nullptr, outCap, len);
            return -1;
        }
        std::memcpy(out, buf_.data() + (size_t)head_ * slotSize_, (size_t)len);
        head_ = (head_ + 1) % cap_;
        count_--;
        return len;
    }

    void clear() { // удалить все кадры; память сохраняется, следующий push начинает новую очередь
        int removed;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            removed = count_;
            head_ = tail_ = count_ = 0;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "AudioFrameRing: cleared frames=%d capacity=%d", removed, cap_);
    }

    int size() const { // моментальный снимок количества; между size и pop возможны другие операции
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }

    int capacity() const { return cap_; } // неизменная ёмкость в кадрах

private:
    static size_t buffer_size(int capacity, int slotSize) {
        if (capacity <= 0 || slotSize <= 0) throw std::invalid_argument("capacity and slot size must be positive");
        if ((size_t)capacity > std::vector<uint8_t>().max_size() / (size_t)slotSize)
            throw std::length_error("frame buffer size exceeds vector limit");
        return (size_t)capacity * (size_t)slotSize;
    }

    const int cap_, slotSize_; // ёмкость в кадрах, максимум байтов на кадр
    int head_ = 0, tail_ = 0, count_ = 0; // индексы чтения/записи и заполнение, под mtx_
    std::vector<uint8_t> buf_; // cap_*slotSize_ байтов, слот выбирается индексом кадра
    std::vector<int> lens_; // фактическая длина каждого занятого слота без сужения uint16_t
    mutable std::mutex mtx_; // сериализует копирование и изменение индексов
};
