/* Владелец rlottie-анимации TGS, все операции в GUI-потоке, без одновременного render.
 * Конструктор копирует данные и принимает один полный gzip с JSON до MaxJsonBytes;
 * усечённый/повреждённый/слишком большой вход или NUL внутри JSON дают isValid()==false с диагностикой.
 * Объект не копируется; деструктор освобождает handle. AnimTimer хранит только заимствованный
 * указатель: перед уничтожением зарегистрированного icon вызвать unregisterIcon/shutdown. */
#pragma once

#include <QImage>
#include <QByteArray>
#include <QSize>
#include <QStringList>
#include <cstdint>

class LottieIcon {
public:
    explicit LottieIcon(const QByteArray &tgsData, const QString &key);
    ~LottieIcon();
    LottieIcon(const LottieIcon&) = delete;
    LottieIcon& operator=(const LottieIcon&) = delete;

    static constexpr int MaxJsonBytes = 16 * 1024 * 1024; // бюджет распакованного JSON, не ограничение формата TGS

    bool isValid() const { return m_valid; }
    QSize originalSize() const { return m_valid ? QSize((int)m_w, (int)m_h) : QSize(0, 0); } // canvas, либо 0x0 для невалидного
    int frameCount() const { return m_totalFrames; } // положительный при isValid()
    double frameRate() const { return m_frameRate; } // метаданные; AnimTimer использует общий период 33 мс
    int currentFrame() const { return m_currentFrame; }
    int tgsSize() const { return m_tgsSize; }
    void setTgsSize(int sz) { m_tgsSize = sz; }
    void logWarnings() const; // вывести сохранённые предупреждения парсера после настройки logger

    // Независимый ARGB32 premultiplied, номер циклический, включая отрицательные; пустой QImage при ошибке/размере <=0.
    QImage renderFrame(int frameNum, QSize targetSize) const;
    void advanceFrame(); // следующий кадр; невалидный объект остаётся на кадре 0 и выдаёт WARN

private:
    bool m_valid = false;
    struct Lottie_Animation_S *m_anim = nullptr; // собственный handle, включая невалидные метаданные после разбора
    int m_currentFrame = 0;
    int m_totalFrames = 0;
    int m_tgsSize = 0;
    double m_frameRate = 60.0;
    size_t m_w = 0, m_h = 0;
    QByteArray m_jsonData; // JSON живёт до освобождения handle
    QString m_key; // ключ rlottie-кеша: вызывающий задаёт уникальное имя ресурса/содержимого
    QStringList m_loadWarnings; // warning callback доступен только во встроенной сборке с соответствующим API
};
