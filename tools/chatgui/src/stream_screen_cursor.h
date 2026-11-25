/* Наложение системного курсора на кадр выбранного экрана до масштабирования/кодирования.
 * X11/XFixes: создание, draw и разрушение в GUI-потоке. Windows: создать в GUI, draw последовательно в image worker;
 * сохранённый HMONITOR позволяет не обращаться к QScreen из worker. Экземпляр не синхронизирует параллельные вызовы.
 * Кадр задаётся в физических пикселях исходного монитора; position/hotspot/bitmap используют ту же систему координат.
 * При рисовании devicePixelRatio кадра сбрасывается в 1. Wayland получает курсор через portal, этот класс там не используется. */
#pragma once
#include <QImage>
#include <QPointer>
#include <QScreen>
#ifndef Q_OS_WIN
#include <QWindow>
struct _XDisplay;
#endif
class StreamScreenCursor {
public:
    // screen — выбранный живой монитор, владение не передаётся; ошибка открытия логируется, draw вернёт false.
    explicit StreamScreenCursor(QScreen* screen);
    // Освободить служебные ресурсы после завершения draw; X11 содержит QWindow и требует GUI-поток.
    ~StreamScreenCursor();
    // Изменить paintable frame в исходном размере экрана; Windows также проверяет точное совпадение размеров.
    // true — обработано, включая отсутствие видимого курсора в кадре; false — ошибка с логом video.
    // Ранний успех без рисования сохраняет frame, включая его devicePixelRatio.
    bool draw(QImage& frame);
private:
#ifdef Q_OS_WIN
    void* m_monitor = nullptr; // Заимствованный HMONITOR; GetMonitorInfo проверяет доступность перед draw.
    QString m_screenName; // Снимок имени для диагностики, без чтения QScreen из worker.
#else
    QPointer<QScreen> m_screen; // Выбранный монитор; удаление приводит к отказу draw.
    QWindow m_origin; // Невидимое окно в начале монитора для перевода координат X11 в физические пиксели.
    struct _XDisplay* m_display = nullptr; // Заимствованный Qt X11 display, не закрывается этим объектом.
#endif
    bool m_ready = false, m_reported = false; // Открыт backend; успешное наложение уже записано в INFO.
};
