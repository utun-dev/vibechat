// PNG для Linux-ярлыка из общего WebP: Qt сохраняет прозрачность, QSaveFile защищает готовый файл при ошибке.
#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QStringList>

// Запуск: INPUT.webp OUTPUT.png; возвращает 0 после сохранения или 1 с подробной диагностикой ошибки.
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 3) {
        qCritical().noquote() << "ERROR: Usage:" << arguments.value(0) << "INPUT.webp OUTPUT.png";
        return 1;
    }
    QImageReader reader(arguments[1], QByteArrayLiteral("webp"));
    const QImage image = reader.read();
    if (image.isNull()) {
        qCritical().noquote() << "ERROR: Cannot decode icon:" << arguments[1] << reader.errorString();
        return 1;
    }
    QSaveFile output(arguments[2]);
    if (!output.open(QIODevice::WriteOnly)) {
        qCritical().noquote() << "ERROR: Cannot open launcher PNG:" << arguments[2] << output.errorString();
        return 1;
    }
    QImageWriter writer(&output, QByteArrayLiteral("png"));
    writer.setCompression(9);
    if (!writer.write(image)) {
        qCritical().noquote() << "ERROR: Cannot encode launcher PNG:" << arguments[2] << writer.errorString();
        return 1;
    }
    if (!output.commit()) {
        qCritical().noquote() << "ERROR: Cannot save launcher PNG:" << arguments[2] << output.errorString();
        return 1;
    }
    qInfo().noquote() << "INFO: Launcher PNG ready:" << arguments[1] << "->" << arguments[2]
                     << "size=" << image.size() << "alpha=" << image.hasAlphaChannel();
    return 0;
}
