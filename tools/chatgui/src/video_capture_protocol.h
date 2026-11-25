/* IPC GUI <-> video capture worker: QLocalSocket передаёт QDataStream Qt_5_15, пиксели лежат в VideoCaptureBuffer.
 * Пакет: quint32 длины тела, quint8 Type, quint64 поколения конфигурации, затем поля конкретного типа.
 * Worker пишет один RGB32-кадр и посылает Frame; до Ack его пиксели не меняются, в том числе при Configure.
 * Получатель копирует/отбрасывает кадр и подтверждает его поколение; shared mutex между процессами нет. */
#pragma once
#include <QDataStream>
#include <QLocalSocket>
#include <QByteArray>
#include <functional>

namespace VideoCaptureProtocol {
/* GUI -> worker: Configure(bool,QByteArray device,qint32 w/h/fps,quint8 kind,bool cursor), Quit(), Ack(),
 * Buffer(QString name,quint32 capacity), Refresh(). Worker -> GUI:
 * Devices(count,[name,id,count_modes,quint8 kind,QSize,modes(w,h,fps)]), Frame(qint64 pts/correction,qint32 w/h),
 * Stage/Error(QString), Heartbeat(). Usage call/stream выбирается аргументом запуска.
 * Buffer/Devices используют поколение 0; остальные — поколение конфигурации/подтверждаемого кадра. */
enum Type : quint8 { Configure = 1, Quit, Ack, Devices, Frame, Stage, Error, Buffer, Heartbeat, Refresh };
constexpr quint32 FrameBytes = 1920 * 1080 * 4; /* Буфер Call; Stream согласует буфер до MaxFrameBytes. */
constexpr quint32 MaxFrameBytes = 4096u * 4096u * 4u; /* Проверяемый верхний предел RGB32. */
constexpr quint32 MaxPacket = 65536;           /* Максимум тела IPC-пакета, без префикса длины. */

/* Чётные размеры от 16: длинная сторона <=1920, короткая <=1080; учитывает обе ориентации и YUV420. */
inline bool validSize(int width, int height) {
    return width >= 16 && height >= 16 && width <= 1920 && height <= 1920
        && (width <= 1080 || height <= 1080) && !(width % 2) && !(height % 2);
}

/* Полный диапазон трансляции; произведение безопасно вычисляется после этой проверки. */
inline bool validStreamSize(int width, int height) {
    return width >= 16 && height >= 16 && width <= 4096 && height <= 4096 && !(width % 2) && !(height % 2);
}

/* Сериализует пакет, синхронно вызывая write для его дополнительных полей; размер/содержимое полей не проверяет.
 * Вызывающий должен уложить тело в MaxPacket и обеспечить схему Type; пиксели сюда не включаются. */
inline QByteArray packet(Type type, quint64 generation, const std::function<void(QDataStream&)>& write = {}) {
    QByteArray result;
    QDataStream out(&result, QIODevice::WriteOnly); out.setVersion(QDataStream::Qt_5_15);
    out << quint32(0) << quint8(type) << generation;
    if (write) write(out);
    out.device()->seek(0); out << quint32(result.size() - 4);
    return result;
}

/* Один Reader на соединение, вызовы сериализуются в его потоке. После ошибки соединение закрыть,
 * для нового соединения создать/присвоить пустой Reader; автоматического восстановления framing нет. */
class Reader {
    QByteArray pending;  /* Неполный префикс длины либо неполное тело текущего пакета. */
    quint32 length = 0;  /* 0 — читается четырёхбайтовый префикс; иначе ожидаемая длина тела (9..MaxPacket). */
public:
    /* Читает доступные данные, сохраняя неполный пакет до следующего вызова. consume вызывается синхронно
     * после Type/generation и должен прочесть все остальные поля и вернуть true; stream заимствуется на вызов.
     * true — полные пакеты приняты либо нужны ещё байты; false — длина, consume, статус stream или лишние поля неверны. */
    bool read(QLocalSocket& socket, const std::function<bool(quint8, quint64, QDataStream&)>& consume) {
        while (socket.bytesAvailable()) {
            const qsizetype wanted = length ? length : 4;
            pending += socket.read(wanted - pending.size());
            if (pending.size() != wanted) return true;
            QDataStream in(pending); in.setVersion(QDataStream::Qt_5_15);
            if (!length) {
                in >> length; pending.clear();
                if (length < 9 || length > MaxPacket) return false;
            } else {
                quint8 type; quint64 generation; in >> type >> generation;
                bool valid = consume(type, generation, in) && in.status() == QDataStream::Ok && in.atEnd();
                pending.clear(); length = 0;
                if (!valid) return false;
            }
        }
        return true;
    }
};
}
