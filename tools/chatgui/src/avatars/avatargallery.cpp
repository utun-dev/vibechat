#include "avatargallery.h"
#include "avatarmanager.h"
#include "../imageviewer_window.h"
#include <QDialog>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace {
QString text(const char* source) { return QCoreApplication::translate("ChatGui",source); }
class Gallery : public QDialog {
public:
    explicit Gallery(AvatarManager* source) : manager(source) {
        setAttribute(Qt::WA_DeleteOnClose); resize(850,720); setWindowTitle(text("Avatars"));
        auto* layout = new QVBoxLayout(this); viewer = new ImageViewerWindow(this); layout->addWidget(viewer,1);
        viewer->setPlaceholder(text("Loading original…"));
        location = new QLabel(this); location->setTextFormat(Qt::PlainText); layout->addWidget(location);
        status = new QLabel(this); status->setAlignment(Qt::AlignCenter); layout->addWidget(status);
        retry = new QPushButton(text("Retry"),this); retry->hide(); layout->addWidget(retry);
        connect(retry,&QPushButton::clicked,this,[this] { retry->hide(); if (manager) manager->retryGallery(); });
        auto* strip = new QHBoxLayout; strip->addStretch(); previous = new QPushButton("←",this); strip->addWidget(previous);
        previous->setObjectName("avatarPrevious");
        for (int i=0;i<5;i++) {
            previews[i] = new QPushButton(this); previews[i]->setFixedSize(64,64); previews[i]->setIconSize({56,56});
            previews[i]->setObjectName(QString("avatarPreview%1").arg(i));
            if (i==2) previews[i]->setStyleSheet("border: 1px solid #77aaff;");
            strip->addWidget(previews[i]); connect(previews[i], &QPushButton::clicked, this, [this,i] { move(i-2); });
        }
        next = new QPushButton("→",this); strip->addWidget(next); strip->addStretch(); layout->addLayout(strip);
        next->setObjectName("avatarNext");
        connect(previous, &QPushButton::clicked, this, [this] { move(-1); });
        connect(next, &QPushButton::clicked, this, [this] { move(1); });
        connect(new QShortcut(QKeySequence(Qt::Key_Left),this), &QShortcut::activated, this, [this] { move(-1); });
        connect(new QShortcut(QKeySequence(Qt::Key_Right),this), &QShortcut::activated, this, [this] { move(1); });
        connect(viewer, &ImageViewerWindow::closed, this, &QDialog::close);
        connect(manager, &AvatarManager::galleryChanged, this, [this] { refresh(); });
        connect(manager, &AvatarManager::galleryError, this, [this](const QString& error) {
            status->setText(error); viewer->setPlaceholder(error); retry->setVisible(manager && !manager->galleryCurrent().isEmpty());
        });
        connect(manager, &QObject::destroyed, this, &QDialog::close);
        auto* timer = new QTimer(this); timer->setInterval(2000); connect(timer,&QTimer::timeout,this,[this] { refresh(); }); timer->start();
    }
    ~Gallery() override { if (manager) manager->closeGallery(); }
    void move(int delta) {
        if (!manager) return;
        const auto files=manager->gallery(); const int index=files.indexOf(selected)+delta;
        if (index<0 || index>=files.size()) return;
        selected=files[index]; displayed=0; retry->hide(); viewer->setPlaceholder(text("Loading original…")); refresh();
    }
    void refresh() {
        if (!manager) return;
        location->setText(manager->galleryStorageLocation());
        const auto files=manager->gallery(); if (selected.isEmpty()) selected=manager->galleryCurrent();
        const int index=files.indexOf(selected); previous->setEnabled(index>0); next->setEnabled(index>=0 && index+1<files.size());
        if (index<0) return;
        QImage image=manager->galleryImage(selected,true);
        const bool full=!image.isNull(); if (!full) image=manager->galleryImage(selected);
        if (image.cacheKey()!=displayed) { viewer->loadImage(image); displayed=image.cacheKey(); }
        if (full) status->setText(QString("%1 / %2").arg(index+1).arg(files.size()));
        else if (!retry->isVisible()) status->setText(text("Loading original…"));
        for (int i=0;i<5;i++) {
            const int item=index+i-2; const bool valid=item>=0 && item<files.size(); previews[i]->setEnabled(valid);
            const QImage thumbnail=valid ? manager->galleryImage(files[item]) : QImage();
            previews[i]->setIcon(QIcon(QPixmap::fromImage(thumbnail)));
            previews[i]->setText(valid && thumbnail.isNull() ? QStringLiteral("…") : QString());
        }
    }
private:
    QPointer<AvatarManager> manager; ImageViewerWindow* viewer; QLabel *status, *location;
    QPushButton *previews[5], *previous, *next, *retry; QString selected; qint64 displayed=0;
};
QPointer<Gallery> active;
}

void showAvatarGallery(const QString& channel, const QByteArray& owner) {
    auto* manager=AvatarManager::instance(); if (!manager) return;
    if (active) { delete active.data(); active=nullptr; }
    active=new Gallery(manager); active->show(); manager->openGallery(channel,owner);
}
