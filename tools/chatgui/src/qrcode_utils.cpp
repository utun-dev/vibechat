#include "qrcode_utils.h"
#include "ZXingQt.h"

QImage generateQRCode(const QString& text, int moduleSize)
{
    using namespace ZXingQt;
    auto barcode = Barcode::fromText(text, BarcodeFormat::QRCode);
    if (!barcode.isValid())
        return {};
    WriterOptions opts;
    if (moduleSize > 0)
        opts.scale(moduleSize);
    return barcode.toImage(opts);
}

QStringList readQRCodes(const QImage& image)
{
    using namespace ZXingQt;
    auto results = ReadBarcodes(image);
    QStringList texts;
    for (const auto& b : results) {
        if (b.isValid())
            texts.append(b.text());
    }
    return texts;
}
