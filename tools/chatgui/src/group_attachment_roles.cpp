#include "group_attachment_roles.h"
#include "messagedelegate.h"
#include "../db/db_manager.h"
#include <QStandardItem>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
extern "C" {
#include "media_async/encrypted_file.h"
}

void setGroupAttachmentRoles(QStandardItem* item, const QByteArray& data,
                                   const QString& contentType, const QString& mediaDirBase,
                                   const QString& channelId, const QByteArray& localAttrs,
                                   DbManager* db, const QByteArray& messageId) {
    QByteArray body = data;
    if (body.startsWith('*')) {
        int pipePos = body.indexOf('|');
        if (pipePos > 1) {
            QByteArray b64 = body.mid(1, pipePos - 1);
            b64.replace('-', '+');
            b64.replace('_', '/');
            body = QByteArray::fromBase64(b64) + body.mid(pipePos);
        }
    }
    QString ct = contentType;
    if (ct.isEmpty()) ct = "text/plain";
    item->setData(ct, MsgContentTypeRole);
    item->setData(channelId, MsgChannelIdRole);

    if (ct == QLatin1String(ENCRYPTED_ATTACHMENT_TYPE)) {
        item->setData(true, MsgEncryptedRole);
        item->setData(QStringLiteral("application/octet-stream"), MsgContentTypeRole);
        item->setData(QCoreApplication::translate("ChatGui", "Encrypted attachment"), MsgFileDisplayNameRole);
        item->setData(QString(), MsgVoiceFileRole);
        item->setData(0, MsgFileDownloadStateRole);
        const auto parts = QString::fromUtf8(body).split('|');
        if (parts.size() > 3) { item->setData(parts[1].toLongLong(), MsgFileSizeRole); item->setData(parts[3].toInt(), MsgMediaNumBlocksRole); }
        const AttachmentRow snapshot = db ? db->getAttachment(channelId, messageId) : AttachmentRow();
        if (snapshot.found) {
            const bool ready = snapshot.ready, downloaded = snapshot.downloaded, decrypting = snapshot.decrypting;
            const bool error = snapshot.error, key = snapshot.hasKey;
            const int state = snapshot.requestState;
            const QString path = snapshot.path;
            struct attachment_info info = {};
            const bool available = ready && QFile::exists(path) &&
                !attachment_decode((const uint8_t*)snapshot.info.constData(), snapshot.info.size(), &info);
            if (available) {
                item->setData(snapshot.contentType, MsgContentTypeRole);
                item->setData(QString::fromUtf8(info.name), MsgFileDisplayNameRole);
                item->setData(QFileInfo(path).size(), MsgFileSizeRole); item->setData(QFileInfo(path).size(), MsgVoiceFileSizeRole);
                item->setData(path, MsgVoiceFileRole); item->setData(2, MsgFileDownloadStateRole);
                item->setData(QCoreApplication::translate("ChatGui", "Encrypted"), MsgDeliveryStateRole);
                item->setData(QString::number(info.duration_ms / 1000.0, 'f', 1) + "s", MsgVoiceDurationRole);
                item->setData(QString::fromLatin1(info.waveform), MsgVoiceWaveformRole);
                item->setData(info.duration_ms, MsgVideoDurationMsRole);
                item->setData(info.width, MsgVideoWidthRole); item->setData(info.height, MsgVideoHeightRole);
                item->setData(info.width, MsgImageWidthRole); item->setData(info.height, MsgImageHeightRole);
            } else {
                QString status;
                if (error) status = QCoreApplication::translate("ChatGui", "Decryption failed; click to retry");
                else if (state == 3) status = QCoreApplication::translate("ChatGui", "Access denied by author");
                else if (decrypting || (downloaded && key)) status = QCoreApplication::translate("ChatGui", "Decrypting…");
                else if (state == 1) status = QCoreApplication::translate("ChatGui", "Awaiting author's approval");
                else if (state == 0) status = QCoreApplication::translate("ChatGui", "Sending key request…");
                else if (downloaded) status = QCoreApplication::translate("ChatGui", "Requesting decryption…");
                else status = QCoreApplication::translate("ChatGui", "Download encrypted attachment");
                item->setData(status, MsgDeliveryStateRole);
                item->setData(downloaded && !error ? 1 : 0, MsgFileDownloadStateRole);
            }
        }
        return;
    }

    if (ct == "audio/opus") {
        QString raw = QString::fromUtf8(body);
        if (raw.startsWith('{')) return;

        /* unified format: wf={base64};dur={ms};|{file_size}|{block_size}|{num_blocks}|{sigs} */
        QString wfData;
        int durMs = 0;

        if (raw.startsWith("wf=")) {
            int durPos = raw.indexOf(";dur=", 3);
            int durEnd = raw.indexOf(';', durPos + 5);
            if (durPos > 0 && durEnd > 0) {
                wfData = raw.mid(3, durPos - 3);  /* 100 base64 chars */
                durMs  = raw.mid(durPos + 5, durEnd - durPos - 5).toInt();
            }
        }
        item->setData(QString::number(durMs / 1000.0, 'f', 1) + "s", MsgVoiceDurationRole);

        /* block metadata after last ; */
        int pipePos = raw.indexOf('|');
        if (pipePos > 0) {
            QString meta = raw.mid(pipePos + 1);
            QStringList parts = meta.split('|');
            if (parts.size() >= 4) {
                item->setData(parts[0].toULongLong(), MsgVoiceFileSizeRole);
                item->setData(parts[1].toULongLong(), MsgVoiceBlockSizeRole);
                item->setData(parts[2].toUInt(),    MsgVoiceNumBlocksRole);
                item->setData(parts[3],             MsgVoiceSigsRole);
                item->setData(parts[2].toUInt(),    MsgMediaNumBlocksRole);
            }
        }

        /* file path from local_attrs */
        QJsonObject la = QJsonDocument::fromJson(localAttrs).object();
        QString fp = la.value("fp").toString();
        QString st = la.value("st").toString();
        int dlState = (st == "fl" && !fp.isEmpty()) ? 2 : 0;
        item->setData(dlState, MsgFileDownloadStateRole);
        if (!fp.isEmpty()) {
            QString fullPath = mediaDirBase + "/" + channelId + "/" + fp;
            item->setData(fullPath, MsgVoiceFileRole);
        }

        /* decode base64 waveform to normalized 0..1 float list */
        if (!wfData.isEmpty()) {
            QList<QVariant> wf;
            for (int i = 0; i < wfData.size(); i++) {
                QChar c = wfData[i];
                int idx = 0;
                if (c >= 'A' && c <= 'Z') idx = c.unicode() - 'A';
                else if (c >= 'a' && c <= 'z') idx = c.unicode() - 'a' + 26;
                else if (c >= '0' && c <= '9') idx = c.unicode() - '0' + 52;
                else if (c == '+') idx = 62;
                else if (c == '/') idx = 63;
                wf.append((float)idx / 63.0f);
            }
            item->setData(wf, MsgVoiceWaveformRole);
        }
    }
    if (ct == "application/octet-stream") {
        QString raw = QString::fromUtf8(body);
        int pipePos = raw.indexOf('|');
        QString fname = (pipePos > 0) ? raw.left(pipePos) : raw;
        item->setData(fname, MsgFileDisplayNameRole);

        if (pipePos > 0) {
            QString meta = raw.mid(pipePos + 1);
            QStringList parts = meta.split('|');
            if (parts.size() >= 1)
                item->setData(parts[0].toULongLong(), MsgFileSizeRole);
            if (parts.size() >= 3)
                item->setData(parts[2].toUInt(), MsgMediaNumBlocksRole);
        }

        /* parse download state from local_attrs */
        QJsonObject la = QJsonDocument::fromJson(localAttrs).object();
        QString st = la.value("st").toString();
        QString fp = la.value("fp").toString();
        int dlState = 0; /* 0=not started, 2=downloaded */
        if (st == "fl" && !fp.isEmpty()) {
            dlState = 2; /* downloaded */
            QString fullPath = mediaDirBase + "/" + channelId + "/" + fp;
            item->setData(fullPath, MsgVoiceFileRole);
        }
        item->setData(dlState, MsgFileDownloadStateRole);
    }
    if (ct.startsWith("video/")) {
        QString raw = QString::fromUtf8(body);
        int pipePos = raw.indexOf('|');
        QString fname = (pipePos > 0) ? raw.left(pipePos) : raw;
        item->setData(fname, MsgFileDisplayNameRole);

        if (pipePos > 0) {
            QString meta = raw.mid(pipePos + 1);
            QStringList parts = meta.split('|');
            if (parts.size() >= 1)
                item->setData(parts[0].toULongLong(), MsgFileSizeRole);
            if (parts.size() >= 3)
                item->setData(parts[2].toUInt(), MsgMediaNumBlocksRole);
            /* dur/w/h в хвосте: |dur=<ms>|w=<w>|h=<h> */
            for (const QString& p : parts) {
                if (p.startsWith("dur=")) {
                    qint64 durMs = p.mid(4).toLongLong();
                    if (durMs > 0) {
                        qint64 s = durMs / 1000;
                        QString d = QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
                        item->setData(d, MsgVoiceDurationRole);
                    }
                    item->setData((qlonglong)durMs, MsgVideoDurationMsRole);
                } else if (p.startsWith("w=")) {
                    item->setData(p.mid(2).toInt(), MsgVideoWidthRole);
                } else if (p.startsWith("h=")) {
                    item->setData(p.mid(2).toInt(), MsgVideoHeightRole);
                }
            }
        }

        QJsonObject la = QJsonDocument::fromJson(localAttrs).object();
        QString st = la.value("st").toString();
        QString fp = la.value("fp").toString();
        int dlState = 0;
        if (st == "fl" && !fp.isEmpty()) {
            dlState = 2;
            QString fullPath = mediaDirBase + "/" + channelId + "/" + fp;
            item->setData(fullPath, MsgVoiceFileRole);
        }
        item->setData(dlState, MsgFileDownloadStateRole);
    }
    if (ct.startsWith("image/")) {
        QString raw = QString::fromUtf8(body);
        int pipePos = raw.indexOf('|');
        QString fname = (pipePos > 0) ? raw.left(pipePos) : raw;
        item->setData(fname, MsgFileDisplayNameRole);

        if (pipePos > 0) {
            QString meta = raw.mid(pipePos + 1);
            QStringList parts = meta.split('|');
            if (parts.size() >= 1)
                item->setData(parts[0].toULongLong(), MsgFileSizeRole);
            if (parts.size() >= 3)
                item->setData(parts[2].toUInt(), MsgMediaNumBlocksRole);
            /* хвост: |w=<w>|h=<h> */
            for (const QString& p : parts) {
                if (p.startsWith("w=")) item->setData(p.mid(2).toInt(), MsgImageWidthRole);
                else if (p.startsWith("h=")) item->setData(p.mid(2).toInt(), MsgImageHeightRole);
            }
        }

        QJsonObject la = QJsonDocument::fromJson(localAttrs).object();
        QString st = la.value("st").toString();
        QString fp = la.value("fp").toString();
        int dlState = 0;
        if (st == "fl" && !fp.isEmpty()) {
            dlState = 2;
            QString fullPath = mediaDirBase + "/" + channelId + "/" + fp;
            item->setData(fullPath, MsgVoiceFileRole);
        }
        item->setData(dlState, MsgFileDownloadStateRole);
    }
}
