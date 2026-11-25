/* Общая очередь превью сообщений CHAT/DM/Kanban: видимые строки и один экран запаса.
 * Создавать в GUI-потоке после установки модели. view владеет загрузчиком; model заимствована через QPointer.
 * Не более двух worker-задач; деструктор ждёт I/O, поздние результаты проверяют view/model/path/type.
 * directory — каталог профиля либо пусто для системного CacheLocation. Исходники не читаются в GUI. */
#pragma once
#include "media_preview_cache.h"
#include "messagedelegate.h"
#include <QAbstractItemView>
#include <QStandardItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QEvent>
#include <algorithm>
#include <memory>
extern "C" {
#include "debug_config.h"
}
class MediaPreviewLoader final : public QObject {
public:
    MediaPreviewLoader(QAbstractItemView* view, QStandardItemModel* model, const QString& directory)
        : QObject(view), m_view(view), m_model(model) {
        const QString cacheBase = !directory.isEmpty() ? directory : QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        m_previewCache = std::make_shared<MediaPreviewCache::Cache>(QDir(cacheBase).filePath("media_previews/v1"));
        m_previewPool.setMaxThreadCount(2); m_previewTimer.setSingleShot(true);
        auto cleanup = [this] { auto cache = m_previewCache; m_previewPool.start([cache] { cache->prune(); }, -1); };
        m_cacheCleanupTimer.setInterval(60 * 60 * 1000); connect(&m_cacheCleanupTimer, &QTimer::timeout, this, cleanup);
        m_cacheCleanupTimer.start(); cleanup();
        m_view->viewport()->installEventFilter(this);
        connect(&m_previewTimer, &QTimer::timeout, this, &MediaPreviewLoader::prepareMediaPreviews);
        connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { m_previewTimer.start(0); });
        connect(m_model, &QAbstractItemModel::rowsInserted, this, [this] { m_previewTimer.start(0); });
        connect(m_model, &QAbstractItemModel::modelReset, this, [this] { m_previewTimer.start(0); });
        connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& first, const QModelIndex& last, const QVector<int>& roles) {
                const bool sourceChanged = roles.isEmpty() || roles.contains(MsgVoiceFileRole) || roles.contains(MsgContentTypeRole);
                const bool downloadChanged = roles.contains(MsgFileDownloadStateRole);
                if (!sourceChanged && !downloadChanged) return;
                for (int row = first.row(); row <= last.row(); ++row) if (auto* item = m_model->item(row)) {
                    if (sourceChanged) item->setData(QVariant(), MsgImagePreviewRole);
                    if (sourceChanged || item->data(MsgFileDownloadStateRole).toInt() == 2) item->setData(QVariant(), MsgImageErrorRole);
                }
                m_previewTimer.start(0);
            });
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (m_view && object == m_view->viewport() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
            m_previewTimer.start(0);
        return QObject::eventFilter(object, event);
    }
private:
    QPointer<QAbstractItemView> m_view; // История, которой принадлежит загрузчик.
    QPointer<QStandardItemModel> m_model; // Самостоятельные роли сообщений; может исчезнуть раньше view.
    std::shared_ptr<MediaPreviewCache::Cache> m_previewCache; // Workers удерживают кеш до завершения.
    QThreadPool m_previewPool; // Два worker, уничтожение ждёт завершения.
    QSet<QString> m_previewJobs; // Неповторяющиеся path+kind.
    QTimer m_previewTimer, m_cacheCleanupTimer; // Объединение обновлений и часовая очистка.
    void prepareMediaPreviews() {
        if (!m_view || !m_model) return;
        struct Candidate { QString path, job; MediaPreviewCache::Kind kind; int priority; };
        QList<Candidate> candidates; QSet<QString> seen;
        const QRect visible = m_view->viewport()->rect();
        const QRect nearby = visible.adjusted(0, -visible.height(), 0, visible.height());
        for (int row = 0; row < m_model->rowCount(); ++row) {
            auto* item = m_model->item(row);
            const QString type = item->data(MsgContentTypeRole).toString();
            if (!type.startsWith("image/") && !type.startsWith("video/")) continue;
            const QRect rectangle = m_view->visualRect(item->index());
            if (!rectangle.intersects(nearby)) {
                if (item->data(MsgImagePreviewRole).isValid()) item->setData(QVariant(), MsgImagePreviewRole);
                continue;
            }
            const QString path = item->data(MsgVoiceFileRole).toString();
            if (path.isEmpty() || item->data(MsgImagePreviewRole).isValid() || item->data(MsgImageErrorRole).toBool()) continue;
            const auto kind = type.startsWith("video/") ? MediaPreviewCache::Kind::Video : MediaPreviewCache::Kind::Image;
            const QString job = path + QChar(0) + QString::number(int(kind));
            if (m_previewJobs.contains(job) || seen.contains(job)) continue;
            seen.insert(job);
            int priority = qAbs(rectangle.center().y() - visible.center().y()) + (rectangle.intersects(visible) ? 0 : visible.height() * 3);
            candidates.append({path, job, kind, priority});
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) { return left.priority < right.priority; });
        for (const auto& candidate : candidates) {
            if (m_previewJobs.size() >= 2) break;
            m_previewJobs.insert(candidate.job); QPointer<MediaPreviewLoader> self(this); auto cache = m_previewCache;
            QObject* dispatcher = QCoreApplication::instance();
            m_previewPool.start([self, dispatcher, cache, candidate] {
                const auto result = cache->load(candidate.path, candidate.kind);
                if (!QMetaObject::invokeMethod(dispatcher, [self, candidate, result] {
                    if (!self || !self->m_view || !self->m_model) return;
                    self->m_previewJobs.remove(candidate.job);
                    const QRect nearby = self->m_view->viewport()->rect().adjusted(0, -self->m_view->viewport()->height(), 0, self->m_view->viewport()->height());
                    for (int r = 0; r < self->m_model->rowCount(); ++r) {
                        auto* current = self->m_model->item(r);
                        if (current->data(MsgVoiceFileRole).toString() != candidate.path) continue;
                        const QString type = current->data(MsgContentTypeRole).toString();
                        if ((candidate.kind == MediaPreviewCache::Kind::Video && !type.startsWith("video/"))
                            || (candidate.kind == MediaPreviewCache::Kind::Image && !type.startsWith("image/"))) continue;
                        if (!result.retryable && self->m_view->visualRect(current->index()).intersects(nearby)) {
                            current->setData(result.image, MsgImagePreviewRole); current->setData(result.image.isNull(), MsgImageErrorRole);
                        }
                    }
                    self->m_previewTimer.start(0);
                }, Qt::QueuedConnection)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview completion could not be queued source=%s", qPrintable(candidate.path));
            }, 1);
        }
    }

};
