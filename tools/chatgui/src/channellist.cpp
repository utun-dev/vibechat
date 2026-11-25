#include <QCoreApplication>
#include "channellist.h"
#include "channeldelegate.h"
#include "clipboard_exchange.h"
#include "outline_icon.h"
#include "avatars/groupavatarmanager.h"
#include "../db/db_manager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QPixmap>
#include <QMouseEvent>
#include <QMenu>
#include <QStyle>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QSignalBlocker>
#include <QToolTip>
#include <QScrollBar>

extern "C" {
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include "../transport/gui_bridge.h"
#include "dm/dm_core.h"
}

/* Секундное среднее CPU: фиксированный градиент, заполнение по проценту. */
class CpuLoadIndicator : public QWidget {
public:
    explicit CpuLoadIndicator(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("cpuLoadIndicator"));
        setFixedSize(60, 24);
        setToolTip(QCoreApplication::translate("ChatGui", "Network core CPU load (1-second average)"));
        setAccessibleName(toolTip()); setAccessibleDescription(QStringLiteral("0%"));
    }
    void setValue(int percent) {
        percent = qBound(0, percent, 100);
        if (m_percent == percent) return;
        m_percent = percent; setAccessibleDescription(QString::number(percent) + '%'); update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path; path.addRoundedRect(QRectF(rect()), 5, 5);
        painter.setClipPath(path);
        painter.fillRect(rect(), QColor("#E4E9E6"));
        QLinearGradient gradient(0, 0, width(), 0);
        gradient.setColorAt(0, QColor("#66BB6A"));
        gradient.setColorAt(0.5, QColor("#FFCA28"));
        gradient.setColorAt(1, QColor("#EF5350"));
        painter.fillRect(QRectF(0, 0, width() * m_percent / 100.0, height()), gradient);
        painter.setPen(QColor("#172019"));
        QFont f = font(); f.setBold(true); painter.setFont(f);
        painter.drawText(rect(), Qt::AlignCenter, QString::number(m_percent) + '%');
    }
private:
    int m_percent = 0;
};

/* Пустое место и заголовки секций не сбрасывают выбор канала. */
class ChannelListView : public QListView {
public:
    using QListView::QListView;

protected:
    void mousePressEvent(QMouseEvent* e) override {
        QModelIndex idx = indexAt(e->pos());
        if (!idx.isValid()) return;
        if (idx.data(ChannelIsSectionRole).toBool()) return;
        QListView::mousePressEvent(e);
    }
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex& idx, const QEvent* e) const override {
        if (idx.isValid() && idx.data(ChannelIsSectionRole).toBool())
            return QItemSelectionModel::NoUpdate;
        if (idx.isValid())
            return QItemSelectionModel::ClearAndSelect;
        return QItemSelectionModel::NoUpdate;
    }
    void selectionChanged(const QItemSelection& sel, const QItemSelection& desel) override {
        QListView::selectionChanged(sel, desel);
        if (selectionModel() && !selectionModel()->hasSelection() && currentIndex().isValid()
            && currentIndex().row() < model()->rowCount())
            selectionModel()->select(currentIndex(), QItemSelectionModel::ClearAndSelect);
    }

};

static QIcon makeChannelIcon(const QColor &color, const QString &letter) {
    QPixmap pix(32, 32);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, 32, 32, 8, 8);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setBold(true);
    f.setPointSize(14);
    p.setFont(f);
    p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, letter);
    p.end();
    return QIcon(pix);
}

/* Создание группы: два человека и плюс; присоединение: стрелка входа в открытую дверь. */
static QIcon channelActionIcon(bool create, const QColor& color) {
    QPainterPath path;
    if (create) {
        path.addEllipse(QRectF(4.5, 3.5, 6, 6)); path.addEllipse(QRectF(14, 4.5, 5, 5));
        path.moveTo(2, 20); path.lineTo(2, 17); path.cubicTo(2, 14, 4.5, 12, 7.5, 12);
        path.cubicTo(10.5, 12, 13, 14, 13, 17); path.lineTo(13, 20); path.closeSubpath();
        path.moveTo(14.5, 12); path.cubicTo(18, 11.5, 21, 13, 21, 16);
        path.moveTo(19, 17); path.lineTo(19, 22); path.moveTo(16.5, 19.5); path.lineTo(21.5, 19.5);
    } else {
        path.moveTo(12.5, 3); path.lineTo(21, 3); path.lineTo(21, 21); path.lineTo(12.5, 21);
        path.moveTo(3, 12); path.lineTo(16, 12); path.moveTo(11, 7); path.lineTo(16, 12); path.lineTo(11, 17);
    }
    return outlineIcon(path, color, 28);
}

/* Два звена цепочки обозначают открытие ссылки в общем контурном стиле. */
static QIcon openLinkIcon(const QColor& color) {
    QPainterPath path;
    path.moveTo(10, 7); path.lineTo(13, 4); path.cubicTo(18, -1, 25, 6, 20, 11); path.lineTo(17, 14);
    path.moveTo(7, 10); path.lineTo(4, 13); path.cubicTo(-1, 18, 6, 25, 11, 20); path.lineTo(14, 17);
    path.moveTo(8, 16); path.lineTo(16, 8);
    return outlineIcon(path, color, 28);
}

/* Шестерёнка с восемью зубцами и центральным отверстием в общем контурном стиле. */
static QIcon settingsIcon(const QColor& color) {
    QPainterPath path;
    path.moveTo(10.1, 4.2);
    for (int tooth = 0; tooth < 8; tooth++) {
        QTransform rotation; rotation.translate(12, 12); rotation.rotate(tooth * 45); rotation.translate(-12, -12);
        for (const QPointF& point : {QPointF(10.1, 4.2), QPointF(10.5, 2), QPointF(13.5, 2), QPointF(13.9, 4.2)}) {
            path.lineTo(rotation.map(point));
        }
    }
    path.closeSubpath(); path.addEllipse(QRectF(9, 9, 6, 6));
    return outlineIcon(path, color, 24);
}

/* График с осями повторяет смысл кнопки Diagnostics в окне звонка. */
static QIcon connectionMonitorIcon(const QColor& color) {
    QPainterPath path;
    path.moveTo(3, 3); path.lineTo(3, 21); path.lineTo(21, 21);
    path.moveTo(5, 12); path.lineTo(9, 12); path.lineTo(12, 6); path.lineTo(15, 18); path.lineTo(20, 8);
    return outlineIcon(path, color, 24);
}

/* Три ряда обозначают inflight/TX/RX в отдельном окне трафика. */
static QIcon networkTrafficIcon(const QColor& color) {
    QPainterPath path;
    path.moveTo(3, 2); path.lineTo(3, 22);
    for (int y : {6, 13, 20}) {
        path.moveTo(5, y); path.lineTo(9, y); path.lineTo(12, y - 3); path.lineTo(16, y + 1); path.lineTo(21, y - 2);
    }
    return outlineIcon(path, color, 24);
}

void ChannelList::setActiveRadioChannel(const QString& channel) {
    if (m_activeRadioChannel == channel) return;
    releaseRadioPtt();
    m_activeRadioChannel = channel;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        auto* item = m_model->item(row);
        const bool active = !channel.isEmpty() && item->data(ChannelChannelIdRole).toString() == channel;
        item->setData(active, ChannelRadioActiveRole);
        item->setData(active && m_radioTransmitting, ChannelRadioTransmittingRole);
    }
    updateRadioPttGeometry();
}

void ChannelList::releaseRadioPtt() {
    if (!m_radioPtt || !m_radioPtt->isDown()) return;
    m_radioPtt->setDown(false);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "ChannelList PTT: release hidden hold channel=%s", qPrintable(m_activeRadioChannel));
    emit radioPttReleased();
}

void ChannelList::setRadioPttState(bool ready, bool transmitting, bool latched) {
    if (!ready) releaseRadioPtt();
    m_radioPtt->setEnabled(ready);
    m_radioPtt->setToolTip(latched ? QCoreApplication::translate("ChatGui", "PTT locked. Click to release.")
        : QCoreApplication::translate("ChatGui", "Hold to talk, or double-click to lock PTT. Click again to release. Global shortcuts also work."));
    m_radioPtt->setAccessibleName(latched ? QCoreApplication::translate("ChatGui", "PTT locked. Click to release.")
        : QCoreApplication::translate("ChatGui", "Hold to talk"));
    if (m_radioTransmitting == transmitting) return;
    m_radioTransmitting = transmitting;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        auto* item = m_model->item(row);
        item->setData(item->data(ChannelRadioActiveRole).toBool() && transmitting, ChannelRadioTransmittingRole);
    }
}

void ChannelList::updateRadioPttGeometry() {
    if (!m_radioPtt) return;
    QRect target;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const auto index = m_model->index(row, 0);
        if (!index.data(ChannelRadioActiveRole).toBool()) continue;
        QStyleOptionViewItem option; option.rect = m_listView->visualRect(index);
        const QRect image = ChannelDelegate::radioRect(option, index);
        if (!image.isEmpty()) target = QRect(image.left() - 3, option.rect.top(), 30, option.rect.height());
        break;
    }
    if (target.isEmpty() || !m_listView->viewport()->rect().contains(target)) {
        releaseRadioPtt(); m_radioPtt->hide(); return;
    }
    m_radioPtt->setGeometry(target); m_radioPtt->show(); m_radioPtt->raise();
}

ChannelList::ChannelList(DbManager* db, QWidget *parent)
    : QWidget(parent)
    , m_listView(new ChannelListView(this))
    , m_model(new QStandardItemModel(this))
    , m_db(db)
{
    if (auto* avatars = GroupAvatarManager::instance()) connect(avatars, &GroupAvatarManager::changed, this,
        [this, avatars](const QString& channel) {
            const QImage image = avatars->currentImage(channel); if (image.isNull()) return;
            QPixmap pixmap(64, 64); pixmap.fill(Qt::transparent); QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing); QPainterPath path; path.addEllipse(QRectF(0, 0, 64, 64));
            painter.setClipPath(path); painter.drawImage(QRect(0, 0, 64, 64), image); painter.end();
            for (int row = 0; row < m_model->rowCount(); row++) if (m_model->item(row)->data(ChannelChannelIdRole).toString() == channel)
                m_model->item(row)->setData(QIcon(pixmap), ChannelIconRole);
        });
    m_dmPresenceTimer = new QTimer(this);
    m_dmPresenceTimer->setSingleShot(true);
    m_dmPresenceTimer->setInterval(50);
    connect(m_dmPresenceTimer, &QTimer::timeout, this, [this]() {
        auto* inst = gui_bridge_get_inst();
        if (!inst || !m_db || !gui_bridge_is_uasync_ready()) { clearDmPresence(); return; }
        int posted = 0;
        for (const auto& id : m_visibleDm) {
            const QByteArray conv = id.toUtf8();
            if (conv.isEmpty() || conv.size() >= 64 || conv.contains('\0')) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM presence: invalid conversation ID bytes=%d", int(conv.size())); continue;
            }
            auto* req = (struct dm_messages_req*)u_calloc(1, sizeof(struct dm_messages_req));
            if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM presence: allocation failed"); return; }
            req->inst = inst;
            memcpy(req->conv_id, conv.constData(), size_t(conv.size()));
            if (gui_bridge_post_uasync_owned_fn(dm_call_route_trampoline, req) < 0) {
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "PM presence: request rejected conv=%s", conv.constData());
                u_free(req); clearDmPresence(); return;
            }
            posted++;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "PM presence: queued route snapshots=%d", posted);
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QLabel(QCoreApplication::translate("ChatGui", "Groups"), this);
    m_header = header;
    header->setObjectName("channelTitle"); header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->setAlignment(Qt::AlignCenter); header->setMargin(8);
    header->setStyleSheet("QLabel#channelTitle { font-weight: bold; background: palette(window); }"); header->installEventFilter(this);
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 8, 0);
    headerRow->addWidget(header, 1);
    m_clipboard = new ClipboardExchange(this); headerRow->addWidget(m_clipboard);
    m_clipboard->setNameResolver([this](const QString& group, const QByteArray& peer) {
        return m_db && m_db->isOpen() ? m_db->getDisplayName(peer, group) : QString();
    });
    connect(m_clipboard, &ClipboardExchange::memberClipboardChanged, this, [this](const QString& group, const QString& sender, int type) {
        for (int row = 0; row < m_model->rowCount(); ++row) {
            auto* item = m_model->item(row); if (item->data(ChannelChannelIdRole).toString() != group) continue;
            item->setData(sender, ChannelClipboardSenderRole);
            item->setData(type ? ClipboardExchange::contentIcon(type, QColor("#FFC107")) : QIcon(), ChannelClipboardIconRole);
        }
    });
    m_cpuLoad = new CpuLoadIndicator(this);
    m_cpuLoad->setProperty("instantTooltip", true); m_cpuLoad->installEventFilter(this);
    headerRow->addWidget(m_cpuLoad);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "CPU indicator initialized: tooltip=network-core-load average=1s tooltip_delay_ms=0");
    layout->addLayout(headerRow);

    m_listView->setModel(m_model);
    m_listView->setObjectName(QStringLiteral("channelItems"));
    m_listView->setStyleSheet("QListView#channelItems { border: none; border-top: 1px solid palette(mid);"
                             " border-bottom: 1px solid palette(mid); }");
    auto* delegate = new ChannelDelegate(this); m_listView->setItemDelegate(delegate);
    m_radioPtt = new QPushButton(m_listView->viewport());
    m_radioPtt->setObjectName("channelRadioPtt");
    m_radioPtt->setStyleSheet("QPushButton { background: transparent; border: none; }");
    m_radioPtt->setCursor(Qt::PointingHandCursor); m_radioPtt->setEnabled(false); m_radioPtt->hide();
    m_radioPtt->installEventFilter(this); m_listView->viewport()->installEventFilter(this);
    connect(m_radioPtt, &QPushButton::pressed, this, [this] {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "ChannelList PTT: press channel=%s", qPrintable(m_activeRadioChannel));
        emit radioPttPressed();
    });
    connect(m_radioPtt, &QPushButton::released, this, &ChannelList::radioPttReleased);
    connect(m_listView->verticalScrollBar(), &QScrollBar::valueChanged, this, &ChannelList::updateRadioPttGeometry);
    connect(m_listView->horizontalScrollBar(), &QScrollBar::valueChanged, this, &ChannelList::updateRadioPttGeometry);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ChannelList::updateRadioPttGeometry);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "ChannelList PTT: right button, hold/double-click/unlock, grey idle / green TX");
    connect(delegate, &ChannelDelegate::clipboardClicked, this, [this](const QString& group) { m_clipboard->openMember(group); });
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setMouseTracking(true);
    layout->addWidget(m_listView);

    auto *footer = new QWidget(this);
    m_footer = footer; m_footer->setObjectName("channelFooter");
    footer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *fl = new QHBoxLayout(footer);
    fl->setSizeConstraint(QLayout::SetNoConstraint);
    fl->setContentsMargins(6, 4, 6, 4);
    fl->setSpacing(6);

    const QString actionStyle = QStringLiteral(
        "QPushButton { border: none; border-radius: 8px; background: transparent; }"
        "QPushButton:hover { background: palette(light); }");
    auto *gearBtn = new QPushButton(footer);
    gearBtn->setObjectName(QStringLiteral("settingsButton"));
    gearBtn->setIcon(settingsIcon(palette().color(QPalette::ButtonText))); gearBtn->setIconSize(QSize(24, 24));
    gearBtn->setFixedSize(34, 34); gearBtn->setCursor(Qt::PointingHandCursor); gearBtn->setProperty("instantTooltip", true);
    gearBtn->setToolTip(QCoreApplication::translate("ChatGui", "Settings"));
    gearBtn->setAccessibleName(gearBtn->toolTip());
    gearBtn->setStyleSheet(actionStyle);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "settings button initialized icon=gear teeth=8 icon_px=24 button=34x34 tooltip_delay_ms=0");
    fl->addWidget(gearBtn);

    connect(gearBtn, &QPushButton::clicked, this, [this] { emit settingsRequested(); });

    auto *monitorBtn = new QPushButton(footer);
    monitorBtn->setObjectName(QStringLiteral("connectionMonitorButton"));
    monitorBtn->setIcon(connectionMonitorIcon(palette().color(QPalette::ButtonText))); monitorBtn->setIconSize(QSize(24, 24));
    monitorBtn->setFixedSize(34, 34); monitorBtn->setCursor(Qt::PointingHandCursor); monitorBtn->setProperty("instantTooltip", true);
    monitorBtn->setToolTip(QCoreApplication::translate("ChatGui", "Connection Monitor"));
    monitorBtn->setAccessibleName(monitorBtn->toolTip());
    monitorBtn->setStyleSheet(actionStyle);
    fl->addWidget(monitorBtn);
    connect(monitorBtn, &QPushButton::clicked, this, [this] { emit connMonitorRequested(); });

    auto* trafficBtn = new QPushButton(footer);
    trafficBtn->setObjectName(QStringLiteral("networkTrafficButton"));
    trafficBtn->setIcon(networkTrafficIcon(palette().color(QPalette::ButtonText))); trafficBtn->setIconSize(QSize(24, 24));
    trafficBtn->setFixedSize(34, 34); trafficBtn->setCursor(Qt::PointingHandCursor); trafficBtn->setProperty("instantTooltip", true);
    trafficBtn->setToolTip(QCoreApplication::translate("ChatGui", "Network Traffic"));
    trafficBtn->setAccessibleName(trafficBtn->toolTip()); trafficBtn->setStyleSheet(actionStyle); fl->addWidget(trafficBtn);
    connect(trafficBtn, &QPushButton::clicked, this, &ChannelList::networkTrafficRequested);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "network traffic button initialized icon=three-graphs icon_px=24 button=34x34");

    fl->addStretch();

    auto* versionLabel = new QLabel(QStringLiteral("v%1").arg(QCoreApplication::applicationVersion()), footer);
    versionLabel->setObjectName("channelVersion");
    versionLabel->setStyleSheet("font-size: 10px; color: palette(placeholder-text);");
    fl->addWidget(versionLabel);

    auto *createBtn = new QPushButton(footer);
    createBtn->setObjectName(QStringLiteral("createGroupButton"));
    createBtn->setIcon(channelActionIcon(true, palette().color(QPalette::ButtonText))); createBtn->setIconSize(QSize(28, 28));
    createBtn->setFixedSize(40, 38); createBtn->setCursor(Qt::PointingHandCursor); createBtn->setProperty("instantTooltip", true);
    createBtn->setToolTip(QCoreApplication::translate("ChatGui", "Create group"));
    createBtn->setAccessibleName(createBtn->toolTip());
    createBtn->setStyleSheet(actionStyle);
    connect(createBtn, &QPushButton::clicked, this, [this] { emit createGroupRequested(); });
    fl->addWidget(createBtn);

    auto *plusBtn = new QPushButton(footer);
    plusBtn->setObjectName(QStringLiteral("joinGroupButton"));
    plusBtn->setIcon(channelActionIcon(false, palette().color(QPalette::ButtonText))); plusBtn->setIconSize(QSize(28, 28));
    plusBtn->setFixedSize(40, 38); plusBtn->setCursor(Qt::PointingHandCursor); plusBtn->setProperty("instantTooltip", true);
    plusBtn->setToolTip(QCoreApplication::translate("ChatGui", "Join"));
    plusBtn->setAccessibleName(plusBtn->toolTip());
    plusBtn->setStyleSheet(actionStyle);
    connect(plusBtn, &QPushButton::clicked, this, [this] { emit joinRequested(); });
    fl->addWidget(plusBtn);
    auto* linkBtn = new QPushButton(footer);
    linkBtn->setObjectName(QStringLiteral("openLinkButton"));
    linkBtn->setIcon(openLinkIcon(palette().color(QPalette::ButtonText))); linkBtn->setIconSize(QSize(28, 28));
    linkBtn->setFixedSize(40, 38); linkBtn->setCursor(Qt::PointingHandCursor); linkBtn->setProperty("instantTooltip", true);
    linkBtn->setToolTip(QCoreApplication::translate("ChatGui", "Open link…"));
    linkBtn->setAccessibleName(linkBtn->toolTip()); linkBtn->setStyleSheet(actionStyle);
    connect(linkBtn, &QPushButton::clicked, this, &ChannelList::openLinkRequested);
    fl->addWidget(linkBtn);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "open link button initialized location=channel-footer icon=chain icon_px=28 button=40x38");
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT,
        "channel actions initialized icons=group-plus/enter icon_px=28 button=40x38 background=transparent tooltip_delay_ms=0");
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "connection monitor button initialized icon=diagnostics icon_px=24 button=34x34 tooltip_delay_ms=0");

    m_footerOptional = {versionLabel, trafficBtn, monitorBtn};
    const auto margins = fl->contentsMargins();
    footer->setMinimumWidth(margins.left() + margins.right() + gearBtn->width() + createBtn->width() +
                            plusBtn->width() + linkBtn->width() + 3 * fl->spacing());
    layout->addWidget(footer);
    auto targets = footer->findChildren<QWidget*>(); targets.append(footer);
    for (auto* target : targets) { target->setAcceptDrops(true); target->installEventFilter(this); }
    m_dropHint = new QLabel(footer); m_dropHint->setObjectName("ownFilesDropHint");
    m_dropHint->setAttribute(Qt::WA_TransparentForMouseEvents); m_dropHint->setAlignment(Qt::AlignCenter);
    m_dropHint->setStyleSheet("QLabel { border: 1px dashed palette(highlight); background: palette(window); color: palette(text); }");
    m_dropHint->hide();
    updateFooterVisibility();

    connect(m_listView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &current, const QModelIndex &) {
                if (current.isValid() && !current.data(ChannelIsSectionRole).toBool()) {
                    m_selectAfterLoad.clear();
                    QString cid = current.data(ChannelChannelIdRole).toString();
                    if (!cid.isEmpty()) emit channelSelected(cid);
                }
            });
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_listView, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto index = m_listView->indexAt(point);
        QString channel = index.data(ChannelChannelIdRole).toString();
        if (!index.isValid() || channel.isEmpty()) return;
        const bool dm = channel.startsWith(QStringLiteral("dm:"));
        const QString conv = channel.mid(3), name = index.data(ChannelNameRole).toString();
        const quint64 generation = index.data(ChannelDmGenerationRole).toULongLong();
        QPointer<ChannelList> guard(this);
        QMenu menu;
        connect(this, &QObject::destroyed, &menu, &QMenu::close);
        if (m_db) connect(m_db, &QObject::destroyed, &menu, &QMenu::close);
        auto* action = menu.addAction(QCoreApplication::translate("ChatGui", dm ? "Delete conversation" : "Group files"));
        if (dm) {
            action->setObjectName(QStringLiteral("deleteDmAction"));
            action->setEnabled(generation && m_dmDeleting.value(conv) != generation);
        }
        if (menu.exec(m_listView->viewport()->mapToGlobal(point)) != action || !guard) return;
        for (int row = 0; row < m_model->rowCount(); row++) {
            auto* item = m_model->item(row);
            if (item->data(ChannelChannelIdRole).toString() != channel) continue;
            if (dm && (item->data(ChannelDmGenerationRole).toULongLong() != generation || m_dmDeleting.value(conv) == generation)) break;
            if (dm) emit dmDeleteRequested(conv, generation, name);
            else emit filesRequested(channel);
            return;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ChannelList: stale menu action skipped channel=%s generation=%llu",
                    qPrintable(channel), (unsigned long long)generation);
    });
}

void ChannelList::updateFooterVisibility() {
    if (m_footerOptional.isEmpty()) return;
    auto* row = static_cast<QHBoxLayout*>(m_footer->layout());
    const auto margins = row->contentsMargins();
    auto itemWidth = [](QWidget* widget) {
        return widget->sizeHint().expandedTo(widget->minimumSize()).boundedTo(widget->maximumSize()).width();
    };
    int needed = margins.left() + margins.right(), count = 0;
    for (int i = 0; i < row->count(); i++) if (auto* widget = row->itemAt(i)->widget()) { needed += itemWidth(widget); count++; }
    needed += qMax(0, count - 1) * row->spacing();
    for (auto* widget : m_footerOptional) {
        bool hide = needed > width();
        if (widget->isHidden() != hide) {
            widget->setVisible(!hide);
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "channel footer: width=%d item=%s visible=%d", width(), qPrintable(widget->objectName()), !hide);
        }
        if (hide) needed -= itemWidth(widget) + row->spacing();
    }
}

bool ChannelList::eventFilter(QObject* target, QEvent* event) {
    if (target == m_radioPtt) {
        if (event->type() == QEvent::MouseButtonDblClick && m_radioPtt->isEnabled() &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "ChannelList PTT: double-click lock channel=%s", qPrintable(m_activeRadioChannel));
            emit radioPttLockRequested(); return true;
        }
        if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) releaseRadioPtt();
        return QWidget::eventFilter(target, event);
    }
    if (target == m_listView->viewport()) {
        if (event->type() == QEvent::Paint || event->type() == QEvent::Resize) updateRadioPttGeometry();
        return QWidget::eventFilter(target, event);
    }
    if (target == m_header && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange)) {
        const QString title = QCoreApplication::translate("ChatGui", "Groups");
        m_header->setText(m_header->fontMetrics().horizontalAdvance(title) <= m_header->width() - 2 * m_header->margin() ? title : QString());
    }
    if (target == m_footer && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) updateFooterVisibility();
    if (target->property("instantTooltip").toBool()) {
        auto* button = static_cast<QWidget*>(target);
        if (event->type() == QEvent::Enter && button->isEnabled()) {
            QToolTip::showText(button->mapToGlobal(QPoint(button->width() / 2, button->height())), button->toolTip(), button);
            DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "channel action tooltip shown button=%s", qPrintable(button->objectName()));
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Hide) {
            QToolTip::hideText();
        }
    }
    if (event->type() == QEvent::DragLeave) { m_dropHint->hide(); return true; }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove && event->type() != QEvent::Drop)
        return QWidget::eventFilter(target, event);
    auto* drop = static_cast<QDropEvent*>(event);
    QString channel = m_listView->currentIndex().data(ChannelChannelIdRole).toString();
    QString reason;
    if (channel.size() != 64) reason = QCoreApplication::translate("ChatGui", "Select a group to upload files");
    else if (!m_db || !m_db->isOpen() || !gui_bridge_is_uasync_ready())
        reason = QCoreApplication::translate("ChatGui", "Upload unavailable");
    else if (!drop->mimeData()->hasUrls() || drop->mimeData()->urls().isEmpty() || !(drop->possibleActions() & Qt::CopyAction))
        reason = QCoreApplication::translate("ChatGui", "Drop local files to copy");
    QStringList paths;
    if (reason.isEmpty()) for (const auto& url : drop->mimeData()->urls()) {
        if (!url.isLocalFile() || url.toLocalFile().isEmpty()) {
            reason = QCoreApplication::translate("ChatGui", "Only local files can be uploaded"); break;
        }
        paths.append(url.toLocalFile());
    }
    if (event->type() == QEvent::Drop) m_dropHint->hide();
    else {
        m_dropHint->setGeometry(m_footer->rect());
        m_dropHint->setText(reason.isEmpty() ? QCoreApplication::translate("ChatGui", "Copy to my files") : reason);
        m_dropHint->show(); m_dropHint->raise();
    }
    if (!reason.isEmpty()) {
        if (event->type() != QEvent::DragMove)
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file drop: rejected channel=%s reason=%s", qPrintable(channel), qPrintable(reason));
        drop->ignore();
        // После отклонённого DragEnter Qt не гарантирует DragLeave.
        m_dropHint->hide(); return true;
    }
    drop->setDropAction(Qt::CopyAction); drop->accept();
    if (event->type() == QEvent::Drop) {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file drop: accepted channel=%s files=%d", qPrintable(channel), int(paths.size()));
        QTimer::singleShot(0, this, [this, channel, paths] { emit ownFilesDropped(channel, paths); });
    }
    return true;
}

void ChannelList::setDmDeleting(const QString& convId, quint64 generation, bool deleting) {
    if (deleting) m_dmDeleting.insert(convId, generation);
    else if (m_dmDeleting.value(convId) == generation) m_dmDeleting.remove(convId);
}

void ChannelList::refreshDmPresence() {
    if (!m_dmPresenceTimer->isActive()) m_dmPresenceTimer->start();
}

void ChannelList::clearDmPresence() {
    m_dmPresenceTimer->stop();
    m_dmOnline.clear();
    for (int i = 0; i < m_model->rowCount(); i++) {
        auto* item = m_model->item(i);
        if (item->data(ChannelChannelIdRole).toString().startsWith(QStringLiteral("dm:")))
            item->setData(false, ChannelDmOnlineRole);
    }
}

void ChannelList::onDmCallRoute(const uint8_t* data, int len) {
    if (!data || len < 66 || !data[0] || len != 65 + data[0]) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "PM presence snapshot: invalid size=%d", len); return;
    }
    QString conv = QString::fromUtf8((const char*)data + 1, data[0]);
    const QByteArray group(reinterpret_cast<const char*>(data + 33 + data[0]), 32);
    const QByteArray peer(reinterpret_cast<const char*>(data + 1 + data[0]), 32);
    const QByteArray empty(32, '\0');
    const bool online = group != empty && peer != empty;
    if (m_dmOnline.value(conv) != online)
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "PM presence: conv=%s online=%d", conv.toUtf8().constData(), online);
    m_dmOnline.insert(conv, online);
    for (int i = 0; i < m_model->rowCount(); i++) {
        auto* item = m_model->item(i);
        if (item->data(ChannelChannelIdRole).toString() == QStringLiteral("dm:") + conv)
            item->setData(online, ChannelDmOnlineRole);
    }
}

void ChannelList::setCpuLoad(int percent) { m_cpuLoad->setValue(percent); }

void ChannelList::loadChannels() {
    if (!m_db || !m_db->isOpen()) {
        m_loadGeneration++; m_loadPending = m_reloadPending = false; m_selectAfterLoad.clear();
        clearDmPresence(); applyNavigation({}); return;
    }
    if (m_loadPending) { m_reloadPending = true; return; }
    if (!m_db->hasAsyncReader()) {
        const char* path = sqlite3_db_filename(m_db->m_db, "main");
        if (path && *path) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "channel list: file database requires asynchronous reader"); return; }
        DbReadView view(m_db->m_db, m_db->dbPath());
        const auto snapshot = view.getNavigation();
        if (view.sqlError() != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "channel list: RAM snapshot failed rc=%d", view.sqlError()); return;
        }
        applyNavigation(snapshot); return;
    }
    m_loadPending = true; m_reloadPending = false; const auto generation = ++m_loadGeneration;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "channel list: READ queued generation=%llu", (unsigned long long)generation);
    const bool accepted = m_db->readAsync(this, [](DbReadView& view, QVariant* value) {
        *value = QVariant::fromValue(view.getNavigation()); return SQLITE_OK;
    }, [this, generation](const db_async_result& status, const QVariant& value) {
        if (generation != m_loadGeneration) return;
        if (!m_db || !m_db->isOpen()) { m_loadPending = false; loadChannels(); return; }
        if (status.status != DB_ASYNC_OK || !value.canConvert<NavigationSnapshot>()) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "channel list: READ failed generation=%llu status=%d rc=%d: %s",
                (unsigned long long)generation, status.status, status.sqlite_code, status.message);
            m_loadPending = false; if (m_reloadPending) loadChannels(); return;
        }
        const auto snapshot = value.value<NavigationSnapshot>();
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "channel list: READ applied generation=%llu groups=%d conversations=%d",
            (unsigned long long)generation, int(snapshot.channels.size()), int(snapshot.conversations.size()));
        const bool reload = m_reloadPending; m_reloadPending = false;
        const QPointer<ChannelList> self(this); applyNavigation(snapshot, !reload);
        if (!self || generation != m_loadGeneration) return;
        m_loadPending = false; if (reload || m_reloadPending) loadChannels();
    });
    if (!accepted) {
        m_loadPending = false;
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "channel list: READ scheduling rejected generation=%llu", (unsigned long long)generation);
    }
}

/* Только готовые значения: отрисовка, декодирование JPEG и presence не обращаются к SQLite. */
void ChannelList::applyNavigation(const NavigationSnapshot& snapshot, bool settled) {
    m_visibleDm.clear();
    for (const auto& dm : snapshot.conversations) m_visibleDm.append(dm.convId);
    refreshDmPresence();
    const QString requested = m_selectAfterLoad;
    QHash<QString, int> peersOnline;
    for (int row = 0; row < m_model->rowCount(); row++) {
        auto* item = m_model->item(row);
        peersOnline.insert(item->data(ChannelChannelIdRole).toString(), item->data(ChannelPeersOnlineRole).toInt());
    }
    QString selectedCid;
    if (m_listView->currentIndex().isValid())
        selectedCid = m_listView->currentIndex().data(ChannelChannelIdRole).toString();

    auto* sel = m_listView->selectionModel();
    QSignalBlocker selectionSignals(sel);
    m_model->clear();
    {
        const auto& channels = snapshot.channels;
        static const QColor colors[] = {
            QColor(74,144,226), QColor(126,211,33), QColor(245,166,35),
            QColor(208,2,27),   QColor(144,19,254), QColor(0,184,148)
        };
        int ci = 0;

        for (const auto& ch : channels) {
            auto letter = ch.name.mid(0, 1).toUpper();
            if (letter.isEmpty()) letter = "#";
            auto *item = new QStandardItem();
            item->setData(ch.groupType == QStringLiteral("kanban") ? QStringLiteral("▦ ") + ch.name : ch.name, ChannelNameRole);
            item->setData(ch.channelId,   ChannelChannelIdRole);
            item->setData(ch.channelId == m_activeRadioChannel, ChannelRadioActiveRole);
            item->setData(ch.channelId == m_activeRadioChannel && m_radioTransmitting, ChannelRadioTransmittingRole);
            const int clipboardType = m_clipboard->memberType(ch.channelId);
            item->setData(m_clipboard->memberSender(ch.channelId), ChannelClipboardSenderRole);
            if (clipboardType) item->setData(ClipboardExchange::contentIcon(clipboardType, QColor("#FFC107")), ChannelClipboardIconRole);
            item->setData(snapshot.previews.value(ch.channelId), ChannelLastMessageRole);
            item->setData(ch.unreadCount, ChannelUnreadCountRole);
            item->setData(ch.lastMsgAt,   ChannelLastMsgAtRole);
            item->setData(peersOnline.value(ch.channelId), ChannelPeersOnlineRole);
            item->setData(m_autoConnectActive, ChannelAutoConnectActiveRole);
            item->setData(makeChannelIcon(colors[ci % 6], letter), ChannelIconRole);
            if (auto* avatars = GroupAvatarManager::instance()) {
                const QByteArray bytes = snapshot.avatars.value(ch.channelId);
                const QImage image = avatars->image(ch.channelId, ch.avatarVersion, ch.avatarHash, &bytes);
                if (!image.isNull()) {
                    QPixmap pixmap(64, 64); pixmap.fill(Qt::transparent); QPainter painter(&pixmap);
                    painter.setRenderHint(QPainter::Antialiasing); QPainterPath clip; clip.addEllipse(QRectF(0, 0, 64, 64));
                    painter.setClipPath(clip); painter.drawImage(QRect(0, 0, 64, 64), image); painter.end();
                    item->setData(QIcon(pixmap), ChannelIconRole);
                }
            }
            m_model->appendRow(item);
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ChannelList::loadChannels row=%d cid='%s' name='%s' unread=%d lastMsgAt=%lld",
                       m_model->rowCount()-1, qPrintable(ch.channelId), qPrintable(ch.name), ch.unreadCount,
                       (long long)ch.lastMsgAt);
            ci++;
        }

        /* ── секция «Личные» (DM-беседы) ── */
        const auto& dms = snapshot.conversations;
        if (!dms.isEmpty()) {
            auto *section = new QStandardItem();
            section->setData(QCoreApplication::translate("ChatGui", "Direct messages"), ChannelNameRole);
            section->setData(true, ChannelIsSectionRole);
            section->setFlags(Qt::NoItemFlags);
            m_model->appendRow(section);

            static const QColor dmColor(0x8E, 0x44, 0xAD);
            for (const auto& dm : dms) {
                QString name = dm.peerName;
                if (name.isEmpty()) name = QString::fromLatin1(dm.peerEd25519.toHex());
                auto letter = name.mid(0, 1).toUpper();
                if (letter.isEmpty()) letter = "@";
                auto *item = new QStandardItem();
                item->setData(name, ChannelNameRole);
                item->setData(QStringLiteral("dm:") + dm.convId, ChannelChannelIdRole);
                item->setData(QVariant::fromValue(dm.generation), ChannelDmGenerationRole);
                item->setData(m_dmOnline.value(dm.convId), ChannelDmOnlineRole);
                item->setData(QString(), ChannelLastMessageRole);
                item->setData(0, ChannelUnreadCountRole);
                item->setData(dm.lastTs, ChannelLastMsgAtRole);
                item->setData(makeChannelIcon(dmColor, letter), ChannelIconRole);
                m_model->appendRow(item);
                DEBUG_DEBUG(DEBUG_CATEGORY_DM, "ChannelList::loadChannels dm row=%d conv='%s' peer='%s'",
                           m_model->rowCount()-1, qPrintable(dm.convId), qPrintable(name));
            }
        }
    }
    auto select = [this, sel](const QString& id) {
        if (id.isEmpty()) return false;
        for (int row = 0; row < m_model->rowCount(); row++) {
            const auto index = m_model->index(row, 0);
            if (index.data(ChannelChannelIdRole).toString() != id) continue;
            sel->select(index, QItemSelectionModel::ClearAndSelect); sel->setCurrentIndex(index, QItemSelectionModel::NoUpdate); return true;
        }
        return false;
    };
    if (select(requested) || settled) m_selectAfterLoad.clear();
    if (!m_listView->currentIndex().isValid()) select(selectedCid);
    if (!m_listView->currentIndex().isValid()) {
        for (int row = 0; row < m_model->rowCount(); ++row) {
            QModelIndex idx = m_model->index(row, 0);
            if (idx.data(ChannelIsSectionRole).toBool()) continue;
            sel->select(idx, QItemSelectionModel::ClearAndSelect);
            sel->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
            break;
        }
    }
    const QString chosen = m_listView->currentIndex().data(ChannelChannelIdRole).toString();
    selectionSignals.unblock();
    if (!chosen.isEmpty()) emit channelSelected(chosen);
}

void ChannelList::selectChannel(const QString& channelId) {
    if (channelId.isEmpty()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "ChannelList: empty selection ID ignored"); return;
    }
    m_selectAfterLoad.clear();
    for (int row = 0; row < m_model->rowCount(); row++) {
        if (m_model->item(row)->data(ChannelChannelIdRole).toString() == channelId) {
            QModelIndex idx = m_model->index(row, 0);
            m_listView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
            m_listView->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
            return;
        }
    }
    if (m_loadPending) m_selectAfterLoad = channelId;
}

void ChannelList::setChannelPeersOnline(const QString& channelId, int count) {
    if (count < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "ChannelList: invalid online count=%d channel=%s", count, qPrintable(channelId)); return;
    }
    int found = 0;
    for (int row = 0; row < m_model->rowCount(); row++) {
        QStandardItem* item = m_model->item(row);
        if (item) {
            QString itemCid = item->data(ChannelChannelIdRole).toString();
            int match = (itemCid == channelId) ? 1 : 0;
            if (match) {
                item->setData(count, ChannelPeersOnlineRole);
                item->setData(m_autoConnectActive ? 1 : 0, ChannelAutoConnectActiveRole);
                found = 1;
                break;
            }
        }
    }
    if (!found)
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ChannelList: channel=%s not found in model", qPrintable(channelId));
}

void ChannelList::setAutoConnectActive(bool active) {
    m_autoConnectActive = active;
    for (int row = 0; row < m_model->rowCount(); row++) {
        QStandardItem* item = m_model->item(row);
        if (item && !item->data(ChannelIsSectionRole).toBool()
            && !item->data(ChannelChannelIdRole).toString().startsWith(QStringLiteral("dm:")))
            item->setData(active, ChannelAutoConnectActiveRole);
    }
}

void ChannelList::setChannelUnreadCount(const QString& channelId, int count) {
    if (count < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "ChannelList: invalid unread count=%d channel=%s", count, qPrintable(channelId)); return;
    }
    for (int row = 0; row < m_model->rowCount(); row++) {
        QStandardItem* item = m_model->item(row);
        if (item && item->data(ChannelChannelIdRole).toString() == channelId) {
            int old = item->data(ChannelUnreadCountRole).toInt();
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ChannelList::setChannelUnreadCount ch=%s %d->%d",
                       qPrintable(channelId), old, count);
            item->setData(count, ChannelUnreadCountRole);
            return;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ChannelList::setChannelUnreadCount ch=%s not found", qPrintable(channelId));
}

int ChannelList::getChannelUnreadCount(const QString& channelId) const {
    for (int row = 0; row < m_model->rowCount(); row++) {
        QStandardItem* item = m_model->item(row);
        if (item && item->data(ChannelChannelIdRole).toString() == channelId)
            return item->data(ChannelUnreadCountRole).toInt();
    }
    return 0;
}

void ChannelList::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange) return;
    const QColor color = palette().color(QPalette::ButtonText);
    const struct { const char* name; QIcon icon; } icons[] = {
        {"settingsButton", settingsIcon(color)}, {"connectionMonitorButton", connectionMonitorIcon(color)},
        {"networkTrafficButton", networkTrafficIcon(color)}, {"createGroupButton", channelActionIcon(true, color)},
        {"joinGroupButton", channelActionIcon(false, color)}, {"openLinkButton", openLinkIcon(color)}
    };
    for (const auto& entry : icons)
        if (auto* button = findChild<QPushButton*>(QLatin1String(entry.name))) button->setIcon(entry.icon);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "Channel actions: palette changed, icon color=%s", qPrintable(color.name()));
}
