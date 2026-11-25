/* Регулятор потолка QP по объёму выхода encoder; один video worker, без синхронизации и обращения к encoder.
 * Перед update вызвать reset при смене настроек/encoder. now — неубывающее монотонное время в микросекундах.
 * update получает только новые encoded bytes, включая KEY/config, независимо от принятия кадров publisher.
 * Каждые >=2с измеряет битрейт; после первой выборки усредняет новое измерение с прежним средним пополам.
 * При включённой адаптации потолок меняется не более чем на 1 за выборку: рост при измерении И среднем >108% цели,
 * снижение при среднем <90%. Возврат ограничен выбранным base, рост — 51; -1 отключает адаптацию потолка.
 * Здесь нет валидации/логов: caller передаёт допустимые настройки и применяет/логирует изменение QP. */
#pragma once
#include <cstdint>

class StreamQpControl {
    int m_base = -1, m_limit = -1, m_target = 0; // Выбранный/текущий потолок QP и целевой битрейт в бит/с.
    bool m_adaptive = false, m_sampled = false; // Разрешена адаптация; есть первая завершённая выборка.
    int64_t m_at = 0; // Начало текущего измерения, микросекунды монотонных часов.
    uint64_t m_bytes = 0, m_bitrate = 0; // Накопленные байты интервала и сглаженный битрейт.
public:
    // Сбросить измерение и вернуть потолок к base: -1 либо 0..51; target — цель в бит/с (>0 для адаптации).
    void reset(int base, int target, bool adaptive, int64_t now) {
        m_base = m_limit = base; m_target = target; m_adaptive = adaptive;
        m_at = now; m_bytes = m_bitrate = 0; m_sampled = false;
    }
    // Текущий потолок; -1 до reset либо при выборе настроек encoder.
    int limit() const { return m_limit; }
    // Сглаженный выходной битрейт в бит/с; 0 до первой выборки, считается и при отключённой адаптации.
    uint64_t bitrate() const { return m_bitrate; }
    // Накопить bytes (0 допустим); при истечении >=2с обновить среднее/потолок и вернуть текущий limit.
    // Объём должен помещаться в расчёт m_bytes*8000000 без переполнения uint64_t.
    int update(uint64_t bytes, int64_t now) {
        m_bytes += bytes;
        const int64_t elapsed = now - m_at;
        if (elapsed < 2000000) return m_limit;
        const uint64_t measured = m_bytes * 8000000 / elapsed;
        m_bitrate = m_sampled ? (m_bitrate + measured) / 2 : measured;
        m_sampled = true; m_bytes = 0; m_at = now;
        if (!m_adaptive || m_base < 0 || m_target <= 0) return m_limit;
        if (measured > uint64_t(m_target) * 108 / 100 && m_bitrate > uint64_t(m_target) * 108 / 100 && m_limit < 51) ++m_limit;
        else if (m_bitrate < uint64_t(m_target) * 90 / 100 && m_limit > m_base) --m_limit;
        return m_limit;
    }
};
