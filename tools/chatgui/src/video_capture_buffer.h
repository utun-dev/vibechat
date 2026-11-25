/* Однокадровый mapping согласованной ёмкости до MaxFrameBytes: GUI создаёт, capture worker подключается по name().
 * Объект единолично владеет локальным mapping/handle, не копируется; вызовы не синхронизированы.
 * Доступ к общим пикселям согласуется через Frame/Ack, а не через этот класс. close не ждёт другой процесс. */
#pragma once
#include "video_capture_protocol.h"
#include "debug_config.h"
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <QTemporaryFile>
#include <memory>
#endif

class VideoCaptureBuffer {
    QString m_name, m_error; /* Имя mapping/путь файла и последняя ошибка; close и успех не очищают эти строки. */
    uchar* m_data = nullptr; /* Заимствованный адрес локального отображения, nullptr вне успешного create/attach. */
    quint32 m_capacity = VideoCaptureProtocol::FrameBytes; /* Согласованная ёмкость; проверяется до mapping. */
#ifdef Q_OS_WIN
    HANDLE m_mapping = nullptr; /* Windows file mapping, закрывается этим объектом. */
#else
    std::unique_ptr<QFile> m_file; /* Создатель владеет QTemporaryFile с автоудалением; attach — обычным QFile. */
#endif
    /* Запоминает причину, освобождает локальные ресурсы и возвращает false. */
    bool fail(const QString& error) { m_error = error; close(); return false; }
    /* Отображает согласованную ёмкость handle/файла; false освобождает ресурсы через fail. */
    bool map() {
#ifdef Q_OS_WIN
        m_data = static_cast<uchar*>(MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, m_capacity));
        if (!m_data) return fail(QStringLiteral("MapViewOfFile error=%1").arg(GetLastError()));
#else
        m_data = m_file->map(0, m_capacity);
        if (!m_data) return fail(m_file->errorString());
#endif
        return true;
    }
public:
    /* Пустой буфер без mapping; копирование запрещено. */
    VideoCaptureBuffer() = default;
    VideoCaptureBuffer(const VideoCaptureBuffer&) = delete;
    VideoCaptureBuffer& operator=(const VideoCaptureBuffer&) = delete;
    ~VideoCaptureBuffer() { close(); }
    /* Закрывает прежний mapping и создаёт новый с уникальным именем/временным файлом.
     * true — data/name готовы; false — ресурсов нет, причина в errorString. Пиксели не инициализируются классом. */
    bool create(quint32 capacity = VideoCaptureProtocol::FrameBytes) {
        close();
        if (!capacity || capacity > VideoCaptureProtocol::MaxFrameBytes) return fail(QStringLiteral("Invalid capture buffer capacity"));
        m_capacity = capacity;
#ifdef Q_OS_WIN
        m_name = QStringLiteral("Local\\vibechat-camera-") + QUuid::createUuid().toString(QUuid::Id128);
        m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, m_capacity,
            reinterpret_cast<const wchar_t*>(m_name.utf16()));
        const DWORD error = GetLastError();
        if (!m_mapping || error == ERROR_ALREADY_EXISTS) return fail(QStringLiteral("CreateFileMapping error=%1").arg(error));
#else
        auto file = std::make_unique<QTemporaryFile>();
        if (!file->open() || !file->resize(m_capacity)) return fail(file->errorString());
        m_name = file->fileName(); m_file = std::move(file);
#endif
        return map();
    }
    /* Закрывает прежний mapping и подключается к существующему name на запись/чтение.
     * На Unix проверяет точный размер файла; файл peer не удаляет. Результат/ошибка — как у create. */
    bool attach(const QString& name, quint32 capacity = VideoCaptureProtocol::FrameBytes) {
        close(); m_name = name;
        if (!capacity || capacity > VideoCaptureProtocol::MaxFrameBytes) return fail(QStringLiteral("Invalid capture buffer capacity"));
        m_capacity = capacity;
#ifdef Q_OS_WIN
        m_mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
        if (!m_mapping) return fail(QStringLiteral("OpenFileMapping error=%1").arg(GetLastError()));
#else
        m_file = std::make_unique<QFile>(name);
        if (!m_file->open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) return fail(m_file->errorString());
        if (m_file->size() != m_capacity) return fail(QStringLiteral("Invalid capture buffer size"));
#endif
        return map();
    }
    /* Освобождает только местное отображение/handle; повтор безопасен. У создателя Unix удаляет временный файл,
     * уже подключённый peer сохраняет своё отображение. Ранее выданный этим объектом data становится недействительным. */
    void close() {
#ifdef Q_OS_WIN
        if (m_data && !UnmapViewOfFile(m_data))
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "capture buffer unmap error=%lu", (unsigned long)GetLastError());
        if (m_mapping && !CloseHandle(m_mapping))
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "capture buffer close error=%lu", (unsigned long)GetLastError());
        m_mapping = nullptr;
#else
        m_file.reset();
#endif
        m_data = nullptr;
    }
    uchar* data() const { return m_data; }        /* Адрес до close/create/attach; владение пикселями задаёт Frame/Ack. */
    QString name() const { return m_name; }       /* Имя для attach, действительно для подключения при живом mapping. */
    QString errorString() const { return m_error; } /* Последняя ошибка, может остаться после последующего успеха. */
};
