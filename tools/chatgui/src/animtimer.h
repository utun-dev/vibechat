/* Общий GUI-таймер анимированных emoji: регистрирует LottieIcon и выдаёт ticked.
 * Все операции в GUI-потоке с живым QCoreApplication. Singleton владеет только QTimer;
 * значки заимствованы: до их удаления вызвать unregisterIcon или shutdown.
 * Один общий tick каждые 33 мс двигает каждый значок на кадр независимо от metadata FPS.
 * play запускает минимум PlaybackMs; после дедлайна каждый значок останавливается
 * отдельно на кадре 0, затем таймер останавливается. shutdown не удаляет значки. */
#pragma once

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QElapsedTimer>

class LottieIcon;

class AnimTimer : public QObject {
    Q_OBJECT
public:
    static AnimTimer *instance(); // статический объект на весь процесс, вызывающий не удаляет

    void registerIcon(const QString &key, LottieIcon *icon); // только валидный icon; существующий key заменяет указатель без delete
    void unregisterIcon(const QString &key); // отсутствующий key допустим, объект не удаляется
    LottieIcon *icon(const QString &key) const; // заимствованный до замены/удаления владельцем; NULL при отсутствии
    void logRegisteredIcons() const; // накопленные parser WARN и DEBUG-метаданные в general

    // Наведение запускает минимум две секунды воспроизведения, затем каждый значок завершает свой цикл.
    static constexpr int PlaybackMs = 2000;
    void play(); // каждый вызов заново отсчитывает PlaybackMs, без сброса текущих кадров; пустой реестр даёт WARN
    static void shutdown(); // stop, сброс дедлайна/реестра; следующий registerIcon/play допустим

signals:
    void ticked(); // после изменения кадров, включая последний tick с остановкой всех на 0

private slots:
    void onTimer();

private:
    AnimTimer();
    QTimer m_timer; // период 33 мс; закончивший цикл icon после дедлайна больше не продвигается
    QHash<QString, LottieIcon *> m_icons; // не владеет значками, доступ только в GUI
    QElapsedTimer m_playback; // время с последнего play, invalid после shutdown/завершения
};
