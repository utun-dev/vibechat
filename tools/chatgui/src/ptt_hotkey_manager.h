/* Глобальный PTT: Windows/X11 опрашивают физические клавиши каждые 10 мс, Wayland использует системный портал.
 * Все методы/сигналы — GUI-поток, singleton принадлежит qApp; init вызывается после открытия БД.
 * DbManager заимствуется. Одно сочетание читается из ui_state radio_ptt_hotkey; сохранение выполняет вызывающий.
 * Удержание передаёт; radio_ptt_double_press (по умолчанию 1) разрешает фиксацию двойным нажатием до следующего нажатия.
 * Wayland-комбинациями владеет портал. Общим замком кнопки/хоткея владеет RadioAudioEngine; VAD от замка не зависит. */
#pragma once

#include "ptt_key_chord.h"
#include "ptt_hotkey_state.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class DbManager;
class PttPortal;
class QWidget;

class PttHotkeyManager : public QObject {
    Q_OBJECT
public:
    /* Заимствованный singleton на время жизни qApp; первый вызов должен быть из GUI-потока. */
    static PttHotkeyManager* instance();
    /* Запоминает db; запускает портал либо reload/опрос клавиатуры. Наличие поддержки не гарантирует доступ к backend. */
    void init(DbManager* db);
    /* Активное физическое сочетание в формате pttEncodeChord; пустая строка — не назначено. */
    QString hotkey() const;
    /* Применяет одно физическое сочетание в памяти, снимая удержание/фиксацию; невалидное значение отключает хоткей.
     * В режиме портала ничего не делает; БД не меняет. */
    void setHotkey(const QString& chord);
    /* Включает двойное нажатие только для хоткея; изменение снимает общую фиксацию/удержание хоткея, БД не меняет. */
    void setDoublePressEnabled(bool enabled);
    /* Загружает radio_ptt_hotkey и radio_ptt_double_press; без БД — пустое сочетание и разрешённая фиксация. */
    void reload();
    /* Поддержка платформы: Windows, Linux/xcb либо выбранный portal; не проверяет доступность сервиса/устройства. */
    static bool isSupported();
    /* UTUN_PTT_PORTAL и Wayland по XDG_SESSION_TYPE либо Qt platformName; иначе false. */
    static bool usesPortal();
    /* Открывает системное назначение при наличии portal; parentWindow заимствуется на вызов для X11 window ID. */
    void configurePortal(QWidget* parentWindow);
    /* Описание единственного сочетания от портала; без portal пустая строка. */
    QString portalDescription() const;
    /* Локализованный статус portal либо пустая строка. */
    QString portalStatus() const;
    /* Идёт инициализация/запрос назначения; не включает открытое системное окно ConfigureShortcuts. */
    bool portalBusy() const;
    /* При true снимает portal-PTT и игнорирует новые Activated; false не восстанавливает прежнее удержание. */
    void inhibitPortal(bool inhibited);
    /* Читаемые имена кодов в отсортированном порядке, пустой chord — «Not set»; сочетание не изменяет. */
    QString describe(const PttKeyChord& chord) const;
    /* Начинает запись только при работающем физическом опросе и отсутствии другой записи; false при отказе.
     * Снимает hotkey-PTT и ждёт полного отпускания исходных клавиш перед keysChanged. Для Wayland не используется. */
    bool beginCapture();
    /* Завершает запись без сохранения; удерживаемое сочетание нужно отпустить перед новым PTT. */
    void endCapture();

signals:
    /* Изменилось описание или статус portal; перечитать portalDescription/portalStatus/portalBusy. */
    void portalChanged();
    /* Полный снимок физических клавиш только при записи, после её вооружения и при изменении снимка. */
    void keysChanged(const PttKeyChord& held);

private:
    PttHotkeyManager();
    ~PttHotkeyManager() override;
    void pollKeys(); /* Обновляет hotkey-PTT/запись; ошибка чтения отключает PTT и останавливает таймер. */
    bool readKeys(PttKeyChord& held) const; /* Добавляет нажатые коды в изначально пустой held; false при отказе backend. */
    QString keyName(int key) const; /* Имя кода ОС либо локализованный запасной вариант Key N. */
    void updatePressed(bool pressed); // Фронты Windows/X11/Wayland: удержание, установка/снятие общего замка.
    void resetHotkey(const char* reason); // Снимает общую фиксацию, удержание хоткея и историю кликов с причиной в логе.

#ifdef UTUN_PTT_PORTAL
    PttPortal* m_portal = nullptr; /* Дочерняя portal-сессия, создаётся только для выбранного Wayland-режима. */
#endif
    DbManager* m_db = nullptr; // Заимствованная БД, должна быть доступна при reload.
    QTimer m_timer; // Физический опрос, 10 мс; portal использует сигналы без этого таймера.
    void* m_display = nullptr; // собственное X11-соединение, только GUI-поток
    PttKeyChord m_chord; // Единственное активное физическое сочетание; пустое — отключено.
    PttKeyChord m_held; // Последний снимок ОС, не конфигурация сочетания.
    bool m_capturing = false; // Идёт ручная запись вместо вычисления hotkey-PTT.
    bool m_captureArmed = false; // Исходные клавиши полностью отпущены, можно выдавать keysChanged.
    PttHotkeyState m_activation; // Физическое состояние и история двойного нажатия; собственной фиксации нет.
    QElapsedTimer m_clock; // Монотонные мс для двойного нажатия, запускаются в конструкторе.
    bool m_doublePressEnabled = true; // radio_ptt_double_press: кнопка PTT от этой настройки не зависит.
};
