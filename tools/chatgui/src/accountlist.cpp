#include "devgroupdialog.h"
#include <QPushButton>
#include <QCoreApplication>
#include "accountlist.h"
#include "addressprofiletext.h"
#include "accountdelegate.h"
#include "memberlistmodel.h"
#include "avatars/avatargallery.h"
#include "memberpropsdialog.h"
#include "renamedialog.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QVBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QDateTime>
#include <QFont>
#include <QSet>
#include <QMessageBox>
#include <QShowEvent>
#include <QHideEvent>
#include <QHostAddress>
#include <QScopeGuard>
#include <QMenu>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QTreeWidget>
#include <QHeaderView>
#include <QScrollBar>
#include <algorithm>
#include <cstring>
#include "../../../lib/platform_compat.h"

extern "C" {
    #include "../lib/socket_compat.h"
    #include "../lib/debug_config.h"
    #include "chat/chat_core.h"
    #include "dm/dm_core.h"
    #include "routing_layer/topo_node.h"
    #include "routing_layer/topo_node_sqlite.h"
    #include "member_files/protocol.h"
    #include "../../lib/mem.h"
}

static QList<QPointer<AccountList>> g_detailTargets; // только GUI; каждый ответ коррелируется своей панелью
static quint64 g_nextDetailRequest = 0; // только GUI-поток, не сбрасывается при пересоздании панели

static void memberDetailCb(const uint8_t* data, int len) {
    const auto targets = g_detailTargets;
    for (const auto& target : targets) if (target) target->onMemberDetailData(data, len);
}

QString AccountList::addrStr(const QByteArray& addr, int family) {
    if (!family) return "?";
    const int required = family == 4 ? 4 : family == 6 ? 16 : 0;
    if (!required || addr.size() < required) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail address invalid family=%d bytes=%lld required=%d",
                    family, (long long)addr.size(), required); return "?";
    }
    if (family == 4) {
        const auto* a = reinterpret_cast<const uint8_t*>(addr.constData());
        return QString("%1.%2.%3.%4").arg(a[0]).arg(a[1]).arg(a[2]).arg(a[3]);
    }
    Q_IPV6ADDR ip{}; memcpy(ip.c, addr.constData(), sizeof(ip.c));
    return '[' + QHostAddress(ip).toString() + ']';
}

QString AccountList::relativeTimeStr(qint64 timestamp) {
    if (timestamp <= 0) return "";
    qint64 diff = QDateTime::currentSecsSinceEpoch() - timestamp;
    if (diff < 0) diff = 0;
    if (diff < 60)  return QCoreApplication::translate("ChatGui", "%1s").arg(diff);
    if (diff < 3600) return QCoreApplication::translate("ChatGui", "%1m").arg(diff / 60);
    if (diff < 86400) return QCoreApplication::translate("ChatGui", "%1h").arg(diff / 3600);
    return QCoreApplication::translate("ChatGui", "%1d").arg(diff / 86400);
}

AccountList::AccountList(DbManager* db, QWidget *parent)
    : QWidget(parent)
    , m_listView(new QListView(this))
    , m_model(new MemberListModel(this))
    , m_db(db)
    , m_header(nullptr)
    , m_detailPanel(nullptr)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_header = new QLabel(QCoreApplication::translate("ChatGui", "Members"), this);
    m_header->setAlignment(Qt::AlignCenter);
    m_header->setStyleSheet("font-weight: bold; padding: 8px; background: palette(window);");
    layout->addWidget(m_header);

    m_dmDevices = new QTreeWidget(this); m_dmDevices->setObjectName("dmDevices");
    m_dmDevices->setStyleSheet("QTreeWidget#dmDevices { border: none; border-top: 1px solid palette(mid); }");
    m_dmDevices->setColumnCount(2); m_dmDevices->setHeaderHidden(true); m_dmDevices->setRootIsDecorated(false);
    m_dmDevices->setItemsExpandable(false); m_dmDevices->setIndentation(12);
    m_dmDevices->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_dmDevices->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_dmDevices->hide(); layout->addWidget(m_dmDevices, 1);
    m_dmTimer = new QTimer(this); m_dmTimer->setInterval(2000);
    connect(m_dmTimer, &QTimer::timeout, this, &AccountList::refresh);

    m_grouped = new GroupedMemberModel(m_model, this);
    m_listView->setModel(m_grouped);
    m_listView->setObjectName(QStringLiteral("memberItems"));
    m_listView->setStyleSheet("QListView#memberItems { border: none; border-top: 1px solid palette(mid); }");
    auto* delegate = new AccountDelegate(m_listView);
    auto* devicesDelegate = new AccountDelegate(this);
    m_listView->setItemDelegate(delegate);
    connect(delegate, &AccountDelegate::groupToggleRequested, this, [this](const QModelIndex& index) {
        const int row = index.row();
        if (m_grouped->toggle(row)) onMemberClicked(m_grouped->index(row));
    });
    for (auto* actions : {delegate, devicesDelegate}) {
        connect(actions, &AccountDelegate::assignmentRequested, this, &AccountList::assignmentRequested);
        connect(actions, &AccountDelegate::radioMuteRequested, this, [this](const QModelIndex& index) {
            const auto peer = QByteArray::fromHex(index.data(MemberListModel::Ed25519Role).toByteArray());
            if (peer.size() != 32) return;
            if (gui_bridge_toggle_member_radio_mute(m_channelId.toUtf8().constData(), reinterpret_cast<const uint8_t*>(peer.constData())) < 0)
                QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Error"),
                    QCoreApplication::translate("ChatGui", "Cannot change radio listening setting"));
        });
        connect(actions, &AccountDelegate::avatarRequested, this, [this](const QModelIndex& index) {
            showAvatarGallery(m_channelId, index.data(MemberListModel::AvatarEdRole).toByteArray());
        });
        connect(actions, &AccountDelegate::radioCallRequested, this, [this](const QModelIndex& index) {
            const QByteArray peer = QByteArray::fromHex(index.data(MemberListModel::Ed25519Role).toByteArray());
            const auto member = m_model->member(peer);
            const bool devices = index.data(GroupedMemberModel::GroupHeaderRole).toBool();
            if (!member || (!devices && member->is_self) || !member->online) {
                DEBUG_WARN(DEBUG_CATEGORY_RADIO, "member radio call ignored: peer=%s online=%d",
                           peer.toHex().constData(), member && member->online); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "member radio call clicked ch=%s peer=%s", qPrintable(m_channelId), peer.toHex().constData());
            emit radioCallRequested(m_channelId, peer, devices);
        });
        connect(actions, &AccountDelegate::callRequested, this, [this](const QModelIndex& index) {
            const QByteArray peer = QByteArray::fromHex(index.data(MemberListModel::Ed25519Role).toByteArray());
            const auto member = m_model->member(peer);
            const bool devices = index.data(GroupedMemberModel::GroupHeaderRole).toBool();
            if (!member || (!devices && member->is_self) || !member->online) {
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "member call ignored: peer=%s online=%d",
                           peer.toHex().constData(), member && member->online); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "member call clicked ch=%s peer=%s", qPrintable(m_channelId), peer.toHex().constData());
            emit callRequested(m_channelId, peer, devices ? index.data(Qt::DisplayRole).toString() : QString::fromUtf8(member->name), devices);
        });
    }
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setMouseTracking(true);
    m_listView->setIconSize(QSize(MemberListModel::kAvatarSize, MemberListModel::kAvatarSize));
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_detailPanel = new QWidget(this);
    auto* detailLayout = new QVBoxLayout(m_detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(0);

    m_detailHeader = new QLabel(QCoreApplication::translate("ChatGui", "Select a member"), m_detailPanel);
    m_detailHeader->setObjectName("memberDetailHeader");
    m_detailHeader->setWordWrap(true);
    m_detailHeader->setStyleSheet("font-weight: bold; padding: 4px 8px; background: palette(window);");
    detailLayout->addWidget(m_detailHeader);

    m_detailText = new QTextEdit(m_detailPanel);
    m_detailText->setObjectName("memberDetailText");
    m_detailText->setReadOnly(true);
    m_detailText->setFont(QFont("monospace", 9));
    m_detailText->setStyleSheet("QTextEdit { border: none; padding: 4px; background: palette(base); }");
    detailLayout->addWidget(m_detailText, 1);

    auto *memberSplitter = new QSplitter(Qt::Vertical, this);
    memberSplitter->setObjectName(QStringLiteral("memberSections"));
    memberSplitter->setHandleWidth(1);
    memberSplitter->setStyleSheet("QSplitter#memberSections::handle:vertical { background: palette(mid); }");
    memberSplitter->addWidget(m_listView);
    memberSplitter->addWidget(m_detailPanel);
    memberSplitter->setStretchFactor(0, 3);
    memberSplitter->setStretchFactor(1, 2);
    memberSplitter->setSizes({360, 240});
    layout->addWidget(memberSplitter, 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "Member panel: single 1px separators, list side frames removed, detail divider=1px");

    m_groupDevicesModel = new QSortFilterProxyModel(this); m_groupDevicesModel->setSourceModel(m_model);
    m_groupDevicesModel->setFilterRole(MemberListModel::ActiveDevgroupRole); m_groupDevicesModel->setSortRole(Qt::UserRole + 2);
    m_groupDevicesModel->sort(0, Qt::DescendingOrder);
    m_groupDevices = new QListView(m_detailPanel); m_groupDevices->setObjectName("devgroupDevices");
    m_groupDevices->setFrameShape(QFrame::NoFrame);
    m_groupDevices->setModel(m_groupDevicesModel); m_groupDevices->setItemDelegate(devicesDelegate);
    m_groupDevices->setEditTriggers(QAbstractItemView::NoEditTriggers); m_groupDevices->setMouseTracking(true);
    m_groupDevices->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_groupDevices->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_groupDevices->setContextMenuPolicy(Qt::CustomContextMenu); m_groupDevices->hide(); detailLayout->addWidget(m_groupDevices, 1);
    connect(m_groupDevices, &QListView::clicked, this, &AccountList::onMemberClicked);
    connect(m_groupDevices, &QListView::doubleClicked, this, &AccountList::onMemberDoubleClicked);
    connect(m_groupDevices, &QWidget::customContextMenuRequested, this, &AccountList::onMemberContextMenu);

    connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
            m_listView->viewport(), qOverload<>(&QWidget::update));
    connect(m_listView, &QListView::clicked, this, &AccountList::onMemberClicked);
    connect(m_listView, &QListView::doubleClicked, this, &AccountList::onMemberDoubleClicked);

    connect(m_listView, &QWidget::customContextMenuRequested,
            this, &AccountList::onMemberContextMenu);

    m_detailClock.start();
    m_detailTimer = new QTimer(this);
    m_detailTimer->setInterval(1000);
    connect(m_detailTimer, &QTimer::timeout, this, &AccountList::onDetailTimerTick);
    connect(m_model, &MemberListModel::snapshotChanged, this, &AccountList::onSnapshotChanged);

    if (g_detailTargets.isEmpty()) {
        gui_bridge_set_radio_mute_result_cb([](const uint8_t* data, int len) {
            if (!data || len != CHAT_RADIO_MUTE_RESULT_SIZE) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: invalid GUI mute result bytes=%d", len); return; }
            if (data[64]) return;
            const auto targets = g_detailTargets;
            for (auto it = targets.crbegin(); it != targets.crend(); ++it) {
                if (!*it || !(*it)->isVisible() || (*it)->m_channelId != QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data), 32).toHex())) continue;
                QMessageBox::warning(*it, QCoreApplication::translate("ChatGui", "Error"),
                    QCoreApplication::translate("ChatGui", "Cannot change radio listening setting"));
                return;
            }
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio mute failed with no visible member panel group=%s member=%s",
                        QByteArray(reinterpret_cast<const char*>(data), 32).toHex().constData(), QByteArray(reinterpret_cast<const char*>(data + 32), 32).toHex().constData());
        });
        gui_bridge_set_member_detail_cb(memberDetailCb);
    }
    g_detailTargets.append(this);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member detail panel registered count=%lld", (long long)g_detailTargets.size());

}

AccountList::~AccountList() {
    clearMemberDetail();
    g_detailTargets.erase(std::remove_if(g_detailTargets.begin(), g_detailTargets.end(), [this](const auto& target) {
        return !target || target.data() == this;
    }), g_detailTargets.end());
    if (g_detailTargets.isEmpty()) { gui_bridge_set_radio_mute_result_cb(nullptr); gui_bridge_set_member_detail_cb(nullptr); }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member detail panel removed remaining=%lld", (long long)g_detailTargets.size());
}

void AccountList::setChannel(const QString& channelId, quint64 dmGeneration) {
    const QByteArray channel = channelId.toUtf8();
    if (channel.size() >= int(sizeof(chat_member_list_req::ch_id)) || channel.contains('\0')) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member panel channel rejected bytes=%lld embedded_nul=%d",
                    (long long)channel.size(), channel.contains('\0')); return;
    }
    if (!dmGeneration && channelId.startsWith("dm:") && m_db && m_db->isOpen())
        for (const auto& conv : m_db->getDmConversations())
            if (conv.convId == channelId.mid(3) && conv.mode == DM_MODE_GROUPS) { dmGeneration = conv.generation; break; }
    if (m_channelId != channelId || m_dmGeneration != dmGeneration) {
        clearMemberDetail(); m_dmDevices->clear(); m_dmSnapshot.clear();
    }
    m_channelId = channelId;
    m_dmGeneration = channelId.startsWith("dm:") ? dmGeneration : 0;
    m_dmDevices->setVisible(m_dmGeneration != 0);
    m_listView->parentWidget()->setVisible(!m_dmGeneration); // Общий splitter списка/карточки не занимает место в режиме PM.
    m_listView->setVisible(!m_dmGeneration); m_detailPanel->setVisible(!m_dmGeneration);
    if (m_dmGeneration && isVisible()) m_dmTimer->start(); else m_dmTimer->stop();

    bool isOwner = false;
    if (m_db && m_db->isOpen()) {
        const auto chs = m_db->getChannels();
        for (const auto& ch : chs) {
            if (ch.channelId == channelId) { isOwner = (ch.ed25519_privkey.size() == 32); break; }
        }
    }

    m_model->setChannel(channelId.startsWith("dm:") ? QString() : channelId, isOwner);
    if (m_dmGeneration) refresh();

    m_header->setText(isOwner ? QCoreApplication::translate("ChatGui", "Members 🔑") : QCoreApplication::translate("ChatGui", "Members"));
    m_header->setToolTip(isOwner ? QCoreApplication::translate("ChatGui", "Group owner (has private key)") : QString());
}

void AccountList::refresh() {
    if (!m_dmGeneration) { m_model->refresh(); return; }
    if (!gui_bridge_is_uasync_ready()) return;
    auto* req = static_cast<dm_messages_req*>(u_calloc(1, sizeof(dm_messages_req)));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: request allocation failed"); return; }
    req->inst = gui_bridge_get_inst(); req->generation = m_dmGeneration;
    snprintf(req->conv_id, sizeof(req->conv_id), "%s", m_channelId.mid(3).toUtf8().constData());
    if (gui_bridge_post_uasync_owned_fn(dm_members_trampoline, req) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM members: request rejected conv=%s", req->conv_id); u_free(req);
    }
}

void AccountList::onDmMembers(const uint8_t* data, int len) {
    if (!data || len < 12 || !data[0] || data[0] > 63 || len < 11 + data[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: invalid header bytes=%d", len); return;
    }
    const int cl = data[0], count = data[9 + cl] | (unsigned(data[10 + cl]) << 8);
    const QString channel = "dm:" + QString::fromUtf8(reinterpret_cast<const char*>(data + 1), cl);
    if (!m_dmGeneration || m_channelId != channel || mf_get64(data + 1 + cl) != m_dmGeneration) return;
    if (count > DM_TARGET_MAX || len != 11 + cl + count * DM_MEMBER_WIRE_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: invalid count=%d bytes=%d", count, len); return;
    }
    const QByteArray snapshot(reinterpret_cast<const char*>(data), len);
    if (snapshot == m_dmSnapshot) return;
    QVector<QPair<int, chat_member_display>> members; QSet<QByteArray> nodes;
    for (int i = 0; i < count; i++) {
        const auto* row = data + 11 + cl + i * DM_MEMBER_WIRE_SIZE; chat_member_display member{};
        if (row[0] > 1 || chat_core_deserialize_member(row + 1, &member) || mf_zero(member.ed25519, 32)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: invalid/duplicate row=%d", i); return;
        }
        const QByteArray ed(reinterpret_cast<const char*>(member.ed25519), 32);
        if (nodes.contains(ed)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: duplicate peer=%s", ed.toHex().constData()); return; }
        nodes.insert(ed); members.append({row[0], member});
    }
    std::stable_sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        if (a.second.is_self != b.second.is_self) return a.second.is_self > b.second.is_self;
        if (a.second.online != b.second.online) return a.second.online > b.second.online;
        return memcmp(a.second.ed25519, b.second.ed25519, 32) < 0;
    });
    const QByteArray selected = m_dmDevices->currentItem() ? m_dmDevices->currentItem()->data(0, Qt::UserRole).toByteArray() : QByteArray();
    const int scroll = m_dmDevices->verticalScrollBar()->value();
    m_dmDevices->clear(); QTreeWidgetItem* sections[2];
    for (int side = 0; side < 2; side++) {
        sections[side] = new QTreeWidgetItem(m_dmDevices, {QCoreApplication::translate("ChatGui", side ? "Peer devices" : "My devices")});
        sections[side]->setFlags(Qt::ItemIsEnabled); QFont font = sections[side]->font(0); font.setBold(true);
        sections[side]->setFont(0, font); sections[side]->setExpanded(true);
    }
    for (const auto& entry : members) {
        const auto& member = entry.second;
        auto* row = new QTreeWidgetItem(sections[entry.first], {QString::fromUtf8(member.name),
            QCoreApplication::translate("ChatGui", member.online ? "online" : "offline")});
        const QByteArray ed(reinterpret_cast<const char*>(member.ed25519), 32);
        row->setData(0, Qt::UserRole, ed);
        row->setToolTip(0, QString::fromLatin1(ed.toHex()));
        if (member.is_self) { QFont font = row->font(0); font.setBold(true); row->setFont(0, font); }
        if (ed == selected) m_dmDevices->setCurrentItem(row);
    }
    m_dmDevices->verticalScrollBar()->setValue(scroll); m_dmSnapshot = snapshot;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM members: panel updated channel=%s count=%d", qPrintable(channel), count);
}

void AccountList::onMemberClicked(const QModelIndex& index) {
    if (!m_detailsEnabled) return;
    if (!index.isValid()) return;
    if (index.data(GroupedMemberModel::GroupHeaderRole).toBool()) {
        showGroupDetail(index.data(MemberListModel::DevgroupRole).toString());
        return;
    }
    showMemberDetail(index.data(MemberListModel::AvatarEdRole).toByteArray());
}

void AccountList::setDetailsEnabled(bool enabled) {
    m_detailsEnabled = enabled;
    clearMemberDetail(); m_detailPanel->setVisible(enabled);
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "member details enabled=%d channel=%s", enabled, qPrintable(m_channelId));
}

void AccountList::clearMemberDetail() {
    if (!m_detailEd.isEmpty() || m_detailRequest) DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION,
        "member detail cleared channel=%s peer=%s request=%llu", qPrintable(m_channelId),
        m_detailEd.toHex().constData(), (unsigned long long)m_detailRequest);
    m_detailTimer->stop(); m_detailEd.clear(); m_detailRequest = 0; m_detailDirty = false;
    m_detailHops.clear(); m_snapLinks.clear(); m_snapSocks.clear(); m_detailAddrs.clear();
    m_detailLastSeen = 0; m_detailCreated = 0; m_detailConnectionOrder.clear();
    m_detailHeader->setText(QCoreApplication::translate("ChatGui", "Select a member")); m_detailText->clear();
    m_detailGroup.clear(); m_groupDevices->hide(); m_detailHeader->show(); m_detailText->show();
}

QByteArray AccountList::ownGroupNode(const QString& group) const {
    if (group.isEmpty()) return {};
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const auto index = m_model->index(row);
        if (index.data(Qt::UserRole + 2).toBool() && index.data(MemberListModel::DevgroupRole).toString() == group &&
            !(index.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_DELETED)) return index.data(MemberListModel::AvatarEdRole).toByteArray();
    }
    return {};
}

void AccountList::showGroupDetail(QString group) {
    clearMemberDetail();
    if (group.isEmpty()) return;
    m_detailGroup = group; m_groupDevicesModel->setFilterFixedString(group); updateGroupDetail();
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "devgroup details selected group=%s devices=%d own=%d", qPrintable(group),
        m_groupDevicesModel->rowCount(), !ownGroupNode(group).isEmpty());
}

void AccountList::updateGroupDetail() {
    const auto first = m_model->member(m_groupDevicesModel->index(0, 0).data(MemberListModel::AvatarEdRole).toByteArray());
    if (!first) { clearMemberDetail(); return; }
    m_detailHeader->clear(); m_detailHeader->hide();
    for (int row = 0; row < m_grouped->rowCount(); ++row) {
        const auto index = m_grouped->index(row);
        if (index.data(GroupedMemberModel::GroupHeaderRole).toBool() &&
            index.data(MemberListModel::DevgroupRole).toString() == m_detailGroup) {
            m_listView->setCurrentIndex(index); break;
        }
    }
    m_detailText->hide(); m_groupDevices->show();
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "devgroup details updated group=%s members=%d", qPrintable(m_detailGroup),
        m_groupDevicesModel->rowCount());
}

void AccountList::renameGroup(QString group) {
    const QByteArray own = ownGroupNode(group); const auto member = m_model->member(own);
    if (!member) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup rename ignored: group is no longer ours"); return; }
    const QString channel = m_channelId; QPointer<AccountList> alive(this);
    QPointer<QInputDialog> dialog = new QInputDialog(this);
    const auto release = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    dialog->setObjectName("devgroupRenameDialog"); dialog->setWindowTitle(QCoreApplication::translate("ChatGui", "Rename"));
    dialog->setLabelText(QCoreApplication::translate("ChatGui", "Device group name"));
    dialog->setTextValue(QString::fromUtf8(member->devgroup_name));
    if (auto* edit = dialog->findChild<QLineEdit*>()) edit->setMaxLength(63);
    connect(dialog, &QInputDialog::textValueChanged, dialog, [dialog](const QString& name) {
        const auto bytes = name.trimmed().toUtf8();
        if (auto* box = dialog->findChild<QDialogButtonBox*>())
            box->button(QDialogButtonBox::Ok)->setEnabled(!bytes.isEmpty() && bytes.size() <= 63 && !bytes.contains('\0'));
    });
    const int result = dialog->exec();
    if (!alive || !dialog || result != QDialog::Accepted) return;
    if (channel != m_channelId || ownGroupNode(group) != own) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup rename cancelled: membership/channel changed"); return;
    }
    const QString name = dialog->textValue().trimmed();
    if (name != QString::fromUtf8(member->devgroup_name)) renameMyDeviceGroup(this, name);
}

void AccountList::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_dmGeneration) { m_dmTimer->start(); refresh(); }
    if (!m_detailEd.isEmpty()) { m_detailTimer->start(); requestMemberDetail(); }
}

void AccountList::hideEvent(QHideEvent* event) {
    m_dmTimer->stop();
    m_detailTimer->stop(); m_detailRequest = 0; m_detailHops.clear();
    QWidget::hideEvent(event);
}

void AccountList::onSnapshotChanged(const QByteArray& ed) {
    if (!m_detailGroup.isEmpty()) { updateGroupDetail(); return; }
    if (m_detailEd.isEmpty()) return;
    if (!ed.isEmpty() && ed != m_detailEd) {
        const auto changed = m_model->member(ed);
        if (changed && changed->is_self) updateDetailDisplay();
        return;
    }
    if (!m_model->member(m_detailEd)) { clearMemberDetail(); return; }
    m_detailDirty = true; m_detailHops.clear();
    for (int row = 0; row < m_grouped->rowCount(); row++) {
        const auto index = m_grouped->index(row);
        if (!index.data(GroupedMemberModel::GroupHeaderRole).toBool() &&
            index.data(MemberListModel::AvatarEdRole).toByteArray() == m_detailEd) {
            m_listView->setCurrentIndex(index); break;
        }
    }
    updateDetailDisplay();
    requestMemberDetail();
}

void AccountList::showMemberDetail(const QByteArray& ed) {
    if (!m_model->member(ed)) return;
    clearMemberDetail(); m_detailEd = ed;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member detail selected channel=%s peer=%s", qPrintable(m_channelId),
        ed.toHex().constData());
    readDetailMetadata(); updateDetailDisplay();
    if (isVisible()) { m_detailTimer->start(); requestMemberDetail(); }
}

/* Только статические сведения БД; имя и live-состояние принадлежат модели участников. */
void AccountList::readDetailMetadata() {
    m_detailAddrs.clear(); m_detailLastSeen = 0; m_detailCreated = 0; m_detailConnectionOrder.clear();
    if (!m_db || !m_db->m_db || m_detailEd.isEmpty()) return;
    sqlite3* d = m_db->m_db;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(d, "SELECT last_seen_at,created_at,x25519_pubkey FROM nodes WHERE ed25519_pubkey=?", -1, &st, nullptr) == SQLITE_OK) {
        int rc = sqlite3_bind_blob(st, 1, m_detailEd.constData(), m_detailEd.size(), SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            m_detailLastSeen = sqlite3_column_int64(st, 0); m_detailCreated = sqlite3_column_int64(st, 1);
            char order[256];
            const auto* x = static_cast<const uint8_t*>(sqlite3_column_blob(st, 2));
            if (x && sqlite3_column_bytes(st, 2) == 32 && topo_node_sqlite_get_connection_order(d, x, order) == 0)
                m_detailConnectionOrder = QString::fromLatin1(order);
        }
        else if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail dates read: %s", sqlite3_errmsg(d));
    } else DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail dates prepare: %s", sqlite3_errmsg(d));
    sqlite3_finalize(st);

    sqlite3_stmt* na = nullptr;
    if (sqlite3_prepare_v2(d,
        "SELECT family, protocol, address, port, socket_id, addr_type, nat_mapping, nat_inbound, nat_source"
        " FROM node_addresses WHERE node_x IN (SELECT x25519_pubkey FROM nodes WHERE ed25519_pubkey=?)", -1, &na, nullptr) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail addresses prepare: %s", sqlite3_errmsg(d)); return;
    }
    if (na) {
        int rc = sqlite3_bind_blob(na, 1, m_detailEd.constData(), m_detailEd.size(), SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(na);
        for (; rc == SQLITE_ROW; rc = sqlite3_step(na)) {
            DetailAddress a;
            a.family   = sqlite3_column_int(na, 0);
            a.protocol = sqlite3_column_int(na, 1);
            const void* adr = sqlite3_column_blob(na, 2);
            int sz = sqlite3_column_bytes(na, 2);
            if (!adr || !((a.family == 4 && sz >= 4) || (a.family == 6 && sz >= 16))) {
                DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "member detail invalid stored address peer=%s family=%d bytes=%d",
                           m_detailEd.toHex().constData(), a.family, sz); continue;
            }
            if (adr && sz > 0) a.addr = QByteArray((const char*)adr, sz);
            a.port      = sqlite3_column_int(na, 3);
            a.socket_id = sqlite3_column_int(na, 4);
            a.addr_type = sqlite3_column_int(na, 5);
            a.nat_mapping = sqlite3_column_int(na, 6);
            a.nat_inbound = sqlite3_column_int(na, 7);
            a.nat_source = sqlite3_column_int(na, 8);
            if (!a.addr.isEmpty()) m_detailAddrs.append(a);
        }
        if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail addresses read: %s", sqlite3_errmsg(d));
        sqlite3_finalize(na);
    }

    std::sort(m_detailAddrs.begin(), m_detailAddrs.end(),
        [](const DetailAddress& a, const DetailAddress& b) {
            if (a.socket_id != b.socket_id) return a.socket_id < b.socket_id;
            if (a.addr_type != b.addr_type) return a.addr_type < b.addr_type;
            if (a.addr != b.addr) return a.addr < b.addr;
            if (a.protocol != b.protocol) return a.protocol < b.protocol;
            return a.port < b.port;
        });

}

void AccountList::requestMemberDetail() {
    if (!m_detailsEnabled) return;
    if (!!m_detailEd.isEmpty() || !isVisible()) return;
    if (!gui_bridge_is_uasync_ready()) {
        if (m_detailRequest) DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member detail request abandoned: core unavailable request=%llu",
                                       (unsigned long long)m_detailRequest);
        m_detailRequest = 0; return;
    }
    const qint64 now = m_detailClock.elapsed();
    if (m_detailRequest && now - m_detailRequestedAt < 5000) return;
    if (m_detailRequest) DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "member detail timeout channel=%s peer=%s request=%llu",
        qPrintable(m_channelId), m_detailEd.toHex().constData(), (unsigned long long)m_detailRequest);
    auto* req = (chat_member_detail_req*)u_calloc(1, sizeof(chat_member_detail_req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail request allocation failed"); m_detailRequest = 0; return; }
    req->inst = gui_bridge_get_inst(); sc_hex_to_binary(m_channelId.toLatin1().constData(), req->group, sizeof(req->group)); memcpy(req->ed, m_detailEd.constData(), sizeof(req->ed));
    if (++g_nextDetailRequest == 0) ++g_nextDetailRequest;
    req->request_id = g_nextDetailRequest;
    m_detailRequest = req->request_id; m_detailRequestedAt = now; m_detailDirty = false;
    DEBUG_TRACE(DEBUG_CATEGORY_CONNECTION, "member detail requested channel=%s peer=%s request=%llu", qPrintable(m_channelId),
        m_detailEd.toHex().constData(), (unsigned long long)m_detailRequest);
    if (gui_bridge_post_uasync_owned_fn(chat_core_collect_member_detail_trampoline, req) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "member detail request rejected channel=%s peer=%s request=%llu",
            qPrintable(m_channelId), m_detailEd.toHex().constData(), (unsigned long long)m_detailRequest);
        u_free(req); m_detailRequest = 0;
    }
}

void AccountList::onMemberDetailData(const uint8_t* data, int len) {
    chat_member_detail_view view{};
    if (len < 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member detail negative packet length=%d", len); return; }
    if (chat_core_parse_member_detail(data, (size_t)len, &view) < 0) return;
    if (!m_detailRequest || view.request_id != m_detailRequest || m_channelId != QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(view.group), 32).toHex()) ||
        m_detailEd != QByteArray(reinterpret_cast<const char*>(view.ed), sizeof(view.ed))) {
        DEBUG_TRACE(DEBUG_CATEGORY_CONNECTION, "member detail ignored stale request=%llu group=%s peer=%s",
            (unsigned long long)view.request_id, QByteArray(reinterpret_cast<const char*>(view.group), 32).toHex().constData(), QByteArray(reinterpret_cast<const char*>(view.ed), sizeof(view.ed)).toHex().constData()); return;
    }
    m_detailRequest = 0;
    if (view.result != CHAT_MEMBER_DETAIL_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "member detail failed channel=%s peer=%s request=%llu result=%u",
            qPrintable(m_channelId), m_detailEd.toHex().constData(), (unsigned long long)view.request_id, view.result);
        m_detailHops.clear(); m_snapLinks.clear(); m_snapSocks.clear(); updateDetailDisplay(); return;
    }
    if (m_detailDirty) { requestMemberDetail(); return; }
    QVector<MemberLinkSnap> links;
    for (int i = 0; i < view.link_count; i++) {
        const uint8_t* p = view.links + i * CHAT_MEMBER_DETAIL_LINK_SIZE;
        MemberLinkSnap lk{};
        lk.direction = p[0]; lk.linkState = p[1]; lk.linkStatus = p[2]; lk.hasIp = p[3] != 0;
        lk.localFamily = p[4]; lk.localAddr = QByteArray((const char*)p + 5, 16);
        lk.localPort = (uint16_t)p[21] | ((uint16_t)p[22] << 8);
        lk.family = p[23]; lk.addr = QByteArray((const char*)p + 24, 16);
        lk.port = (uint16_t)p[40] | ((uint16_t)p[41] << 8);
        memcpy(lk.sockName, p + 42, 20); lk.protocol = QString::fromUtf8((const char*)p + 62); links.append(lk);
    }
    QVector<MemberSockSnap> socks;
    for (int i = 0; i < view.sock_count; i++) {
        const uint8_t* p = view.socks + i * CHAT_MEMBER_DETAIL_SOCK_SIZE;
        MemberSockSnap s{};
        uint32_t id = 0; for (int j = 0; j < 4; j++) id |= (uint32_t)p[j] << (8 * j);
        s.sockId = id; s.port = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
        s.linksUp = (uint16_t)p[6] | ((uint16_t)p[7] << 8); s.linksDown = (uint16_t)p[8] | ((uint16_t)p[9] << 8);
        s.ifname = QString::fromUtf8((const char*)p + 10); socks.append(s);
    }
    QVector<QByteArray> hops;
    for (int i = 0; i < view.hop_count; i++) hops.append(QByteArray(reinterpret_cast<const char*>(view.hops + i * 32), 32));
    m_snapLinks = links; m_snapSocks = socks; m_detailHops = hops;
    readDetailMetadata();
    /* Снимок сокетов локальный; socket_id другого узла относится к другому пространству ID. */
    const auto member = m_model->member(m_detailEd);
    const bool isSelf = member && member->is_self;
    int namedAddresses = 0;
    if (isSelf) {
        for (auto& a : m_detailAddrs) {
            for (const auto& s : m_snapSocks) {
                if (s.sockId == a.socket_id) { a.ifname = s.ifname; namedAddresses += !a.ifname.isEmpty(); break; }
            }
        }
    }
    DEBUG_TRACE(DEBUG_CATEGORY_CONNECTION, "member detail interfaces: peer=%s self=%d local_sockets=%d addresses=%d named=%d",
        m_detailEd.toHex().constData(), isSelf, (int)m_snapSocks.size(), (int)m_detailAddrs.size(), namedAddresses);
    updateDetailDisplay();
}

void AccountList::updateDetailDisplay() {
    const auto member = m_model->member(m_detailEd);
    if (!member) return;
    const bool isSelf = member->is_self != 0;
    QString nodeHex = QString::fromLatin1(m_detailEd.toHex());

    /* ── Header ── */
    QString dotColor = member->online ? "#4CAF50" : "#9E9E9E";
    QStringList hdr;
    hdr.append(QString("<span style='color:%1;'>●</span> %2  %3")
        .arg(dotColor, QString::fromUtf8(member->name).toHtmlEscaped(), nodeHex));
    if (m_detailLastSeen > 0)
        hdr.append(QCoreApplication::translate("ChatGui", "seen %1").arg(relativeTimeStr(m_detailLastSeen)));
    if (m_detailCreated > 0)
        hdr.append(QCoreApplication::translate("ChatGui", "created %1").arg(relativeTimeStr(m_detailCreated)));
    m_detailHeader->setText(hdr.join("  "));

    /* ── Body ── */
    QStringList lines;

    auto protoName = [](int protocol) -> QString {
        if (protocol & 0x04) return (protocol & 0x02) ? "TCP+REALITY" : "REALITY";
        switch (protocol) { case 1: return "UDP"; case 2: return "TCP"; default: return "?"; }
    };

    QString policy = QCoreApplication::translate("ChatGui", "unknown");
    if (!m_detailConnectionOrder.isEmpty()) {
        QStringList protocols;
        int i = 0;
        for (; i < m_detailConnectionOrder.size() && m_detailConnectionOrder[i].isLetter(); ++i)
            protocols.append(m_detailConnectionOrder[i] == 'T' ? "TCP" : m_detailConnectionOrder[i] == 'U' ? "UDP" : "REA");
        const QChar mode = m_detailConnectionOrder[i];
        QString behavior = mode == '1' ? "first" : mode == '2' ? "seq" : "par";
        if (mode == '2') behavior += " " + QString::number(m_detailConnectionOrder.mid(i + 2).toInt()) + "s";
        policy = protocols.join("->") + " " + behavior;
    }
    lines.append(policy);

    if (!m_detailAddrs.isEmpty()) {
        lines.append(QCoreApplication::translate("ChatGui", "── Addresses ──"));
        for (const auto& a : m_detailAddrs) {
            lines.append(QString("  [sock%1] %2  %3  %4:%5%6")
                .arg(a.socket_id)
                .arg(protoName(a.protocol))
                .arg(a.ifname.toHtmlEscaped())
                .arg(addrStr(a.addr, a.family))
                .arg(a.port)
                .arg(a.family == 4 ? "  " + addressProfileText(a.nat_mapping, a.nat_inbound, a.nat_source) : QString()));
            DEBUG_TRACE(DEBUG_CATEGORY_CONNECTION, "member address: peer=%s family=%d socket=%d nat_profile=%d",
                m_detailEd.toHex().constData(), a.family, a.socket_id, a.family == 4);
        }
    }

    if (isSelf) {
        lines.append(QCoreApplication::translate("ChatGui", "── Listen ──"));
        for (const auto& s : m_snapSocks) {
            const DetailAddress* a = nullptr;
            for (const auto& da : m_detailAddrs) {
                if (da.socket_id == s.sockId) { a = &da; break; }
            }
            QString proto = a ? protoName(a->protocol) : "?";
            QString ip = a ? addrStr(a->addr, a->family) : "?";
            lines.append(QString("  [%1/%2]  %3:%4  %5")
                .arg(s.linksUp).arg(s.linksDown)
                .arg(ip).arg(s.port)
                .arg(proto));
        }
    } else {
        if (!m_snapLinks.isEmpty()) {
            lines.append(QCoreApplication::translate("ChatGui", "── Connections ──"));
            for (const auto& lk : m_snapLinks) {
                bool up = (lk.linkState == 3 && lk.linkStatus == 1);
                QString c = up ? "#4CAF50" : "#E53935";
                QString arrow = lk.direction == 2 ? "◀" : lk.direction == 1 ? "▶" : "";
                QString description = lk.protocol.toHtmlEscaped();
                if (lk.hasIp) description += QString("  %1:%2  %3  %4:%5")
                    .arg(addrStr(lk.localAddr, lk.localFamily)).arg(lk.localPort)
                    .arg(arrow).arg(addrStr(lk.addr, lk.family)).arg(lk.port);
                else if (!arrow.isEmpty()) description += "  " + arrow;
                lines.append(QString("<span style='color:%1;'>●</span> %2").arg(c, description));
                if (lk.sockName[0])
                    lines.append(QString("       %1").arg(QString::fromUtf8(lk.sockName).toHtmlEscaped()));
            }
        }
        lines.prepend(m_model->routeText(*member, true));
        if (!m_detailHops.isEmpty() && member->online) {
            QStringList hops;
            for (auto it = m_detailHops.crbegin(); it != m_detailHops.crend(); ++it) {
                QString label = QString::fromLatin1(it->toHex());
                const auto hop = m_model->member(*it);
                if (hop) label += ' ' + QString::fromUtf8(hop->name).toHtmlEscaped();
                hops.append(label);
            }
            lines.append(QString("BGP: ") + hops.join(" → "));
        }
    }

    if (lines.isEmpty())
        m_detailText->clear();
    else
        m_detailText->setHtml(QString("<pre style='margin:0;'>") + lines.join("\n") + "</pre>");
}

void AccountList::onDetailTimerTick() {
    if (!m_detailEd.isEmpty()) requestMemberDetail();
}

/* Диалог может пережить закрытие панели/БД: QObject-владение и QPointer исключают повторное удаление. */
void AccountList::editMember(const QByteArray& ed, bool properties) {
    const auto member = m_model->member(ed);
    if (!member || !m_db || !m_db->isOpen()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "member edit unavailable channel=%s ed=%s database=%d",
                   qPrintable(m_channelId), ed.toHex().constData(), m_db && m_db->isOpen());
        return;
    }
    const QString channel = m_channelId, name = QString::fromUtf8(member->name);
    QPointer<AccountList> alive(this);
    QPointer<QDialog> dialog;
    if (properties) dialog = new MemberPropsDialog(m_db, channel, ed, name, member->flags, member->is_self, this);
    else dialog = new RenameDialog(m_db, channel, ed, name, this);
    const auto release = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    connect(m_db.data(), &QObject::destroyed, dialog.data(), &QDialog::reject);
    const int result = dialog->exec();
    if (!alive || result != QDialog::Accepted || channel != m_channelId) return;
    m_model->refresh();
    emit memberRenamed();
}

void AccountList::onMemberDoubleClicked(const QModelIndex& index) {
    if (index.isValid() && !index.data(GroupedMemberModel::GroupHeaderRole).toBool())
        editMember(QByteArray::fromHex(index.data(MemberListModel::Ed25519Role).toByteArray()), true);
}

bool AccountList::canLinkMyDevice(const QByteArray& ed) const {
    const auto member = m_model->member(ed);
    static const uint8_t noGroup[32] = {};
    return member && memcmp(member->ed25519, noGroup, 32) && !member->is_self && member->online && (member->conn_up & NCONN_BGP)
        && !(member->flags & CHAT_MEMBER_FLAG_DELETED) && !memcmp(member->devgroup, noGroup, sizeof(noGroup));
}

void AccountList::confirmMyDeviceLink(const QByteArray& ed) {
    if (!canLinkMyDevice(ed) || !gui_bridge_is_uasync_ready()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "device link: target unavailable channel=%s peer=%s", qPrintable(m_channelId),
            ed.toHex().constData()); return;
    }
    const QString channel = m_channelId; QPointer<AccountList> alive(this);
    const auto member = m_model->member(ed);
    QPointer<QMessageBox> dialog = new QMessageBox(QMessageBox::Information,
        QCoreApplication::translate("ChatGui", "This is my device"),
        QCoreApplication::translate("ChatGui", "Link %1 to your device subgroup. The device will receive the subgroup's shared keys.\n\n"
            "Enter the first PIN on that device, then enter its return PIN here. On the other device, open Finish pairing in your own member menu. Complete both steps within two minutes.")
            .arg(QString::fromUtf8(member->name)), QMessageBox::Cancel, this);
    const auto release = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    dialog->setObjectName("linkMyDeviceConfirmation"); dialog->setTextFormat(Qt::PlainText);
    auto* link = dialog->addButton(QCoreApplication::translate("ChatGui", "Start linking"), QMessageBox::AcceptRole);
    dialog->setDefaultButton(QMessageBox::Cancel);
    connect(m_model, &MemberListModel::snapshotChanged, dialog, [this, channel, ed, link] {
        link->setEnabled(channel == m_channelId && canLinkMyDevice(ed) && gui_bridge_is_uasync_ready());
    });
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "device link: explanation opened channel=%s peer=%s", qPrintable(channel),
        ed.toHex().constData());
    dialog->exec();
    if (!alive || !dialog || dialog->clickedButton() != link) return;
    if (channel != m_channelId || !canLinkMyDevice(ed) || !gui_bridge_is_uasync_ready()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "device link: cancelled after target changed peer=%s", ed.toHex().constData()); return;
    }
    inviteMyDevice(this, channel, ed);
}

/* Вложенный event loop меню допускает смену канала, снимка и уничтожение панели. */
void AccountList::onMemberContextMenu(const QPoint& pos) {
    auto* view = qobject_cast<QListView*>(sender()); if (!view) view = m_listView;
    const QModelIndex index = view->indexAt(pos);
    if (!index.isValid()) return;
    view->setCurrentIndex(index);
    onMemberClicked(index);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member context selected row=%d peer=%s group=%s", index.row(),
        index.data(MemberListModel::AvatarEdRole).toByteArray().toHex().constData(),
        qPrintable(index.data(MemberListModel::DevgroupRole).toString()));
    if (index.data(GroupedMemberModel::GroupHeaderRole).toBool()) {
        const QString group = index.data(MemberListModel::DevgroupRole).toString(), channel = m_channelId;
        const bool own = !ownGroupNode(group).isEmpty();
        QByteArray target; QString name;
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const auto item = m_model->index(row);
            if (item.data(MemberListModel::DevgroupRole).toString() != group ||
                (item.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_DELETED)) continue;
            const auto member = m_model->member(QByteArray::fromHex(item.data(MemberListModel::Ed25519Role).toByteArray()));
            if (member && !member->is_self) {
                target = QByteArray(reinterpret_cast<const char*>(member->ed25519), 32);
                name = QString::fromUtf8(member->devgroup_name); break;
            }
        }
        QPointer<AccountList> alive(this); QMenu menu;
        connect(this, &QObject::destroyed, &menu, &QMenu::close);
        auto* message = !own && !target.isEmpty() ? menu.addAction(QCoreApplication::translate("ChatGui", "Send message")) : nullptr;
        auto* rename = own ? menu.addAction(QCoreApplication::translate("ChatGui", "Rename")) : nullptr;
        if (rename) rename->setEnabled(gui_bridge_is_uasync_ready());
        auto* finish = own && hasPendingDevicePairing() ? menu.addAction(QCoreApplication::translate("ChatGui", "Finish pairing")) : nullptr;
        auto* chosen = menu.exec(view->viewport()->mapToGlobal(pos));
        if (!chosen || !alive || channel != m_channelId) return;
        if (message && chosen == message) {
            const auto current = m_model->member(target);
            if (current && !(current->flags & CHAT_MEMBER_FLAG_DELETED) &&
                QByteArray(reinterpret_cast<const char*>(current->devgroup), 32).toHex() == group.toLatin1())
                emit pmRequested(channel, target, name, true);
            return;
        }
        if (ownGroupNode(group).isEmpty()) return;
        if (chosen == rename) renameGroup(group);
        else if (finish && chosen == finish) finishDevicePairing(this);
        return;
    }
    const QByteArray ed = QByteArray::fromHex(index.data(MemberListModel::Ed25519Role).toByteArray());
    const auto member = m_model->member(ed);
    if (!member) return;
    const QString channel = m_channelId;
    const bool self = member->is_self != 0, editable = m_db && m_db->isOpen();

    QPointer<AccountList> alive(this);
    QMenu menu;
    connect(this, &QObject::destroyed, &menu, &QMenu::close);
    auto* assignmentDelegate = qobject_cast<AccountDelegate*>(view->itemDelegate());
    const QString memberEd = QString::fromLatin1(ed.toHex());
    const int assignment = assignmentDelegate && assignmentDelegate->assignmentState && !(member->flags & CHAT_MEMBER_FLAG_DELETED) ?
        assignmentDelegate->assignmentState(memberEd) : -1;
    QAction* assignAction = assignment >= 0 ? menu.addAction(QCoreApplication::translate("ChatGui",
        assignment ? "Remove assignee" : "Assign to card")) : nullptr;
    if (assignAction) { assignAction->setCheckable(true); assignAction->setChecked(assignment == 1); menu.addSeparator(); }
    QAction* pmAction = menu.addAction(QCoreApplication::translate("ChatGui", "Send message"));
    pmAction->setEnabled(!self);
    QAction* inviteAction = menu.addAction(QCoreApplication::translate("ChatGui", "Invite to group"));
    inviteAction->setEnabled(!self);
    QAction* callAction = menu.addAction(QCoreApplication::translate("ChatGui", "Call peer"));
    callAction->setEnabled(!self && member->online);
    QAction* radioCallAction = menu.addAction(QCoreApplication::translate("ChatGui", "Call by radio"));
    radioCallAction->setEnabled(!self && member->online);
    QAction* connectAction = menu.addAction(QCoreApplication::translate("ChatGui", "Connect"));
    connectAction->setEnabled(!self);
    QAction* renameAction = menu.addAction(QCoreApplication::translate("ChatGui", "Rename"));
    renameAction->setEnabled(!self && editable);
    QAction* editAction = menu.addAction(QCoreApplication::translate("ChatGui", "Edit"));
    editAction->setEnabled(editable);
    QAction* filesAction = menu.addAction(QCoreApplication::translate("ChatGui", "Member files"));
    QAction* clipboardAction = channel.size() == 64 && editable && m_db->clipboardAllowed(channel) ?
        menu.addAction(QCoreApplication::translate("ChatGui", "Send clipboard")) : nullptr;
    if (clipboardAction) clipboardAction->setEnabled(!self && member->online && !(member->flags & CHAT_MEMBER_FLAG_DELETED));
    QAction* myDeviceAction = canLinkMyDevice(ed) ? menu.addAction(QCoreApplication::translate("ChatGui", "This is my device")) : nullptr;
    if (myDeviceAction) myDeviceAction->setEnabled(gui_bridge_is_uasync_ready());
    QAction* finishAction = self && hasPendingDevicePairing() ?
        menu.addAction(QCoreApplication::translate("ChatGui", "Finish pairing")) : nullptr;
    QAction* chosen = menu.exec(view->viewport()->mapToGlobal(pos));
    if (!alive || !chosen) return;
    const auto current = m_model->member(ed);
    if (channel != m_channelId || !current) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member menu cancelled after snapshot/channel change peer=%s",
                    ed.toHex().constData());
        return;
    }
    if (assignAction && chosen == assignAction) {
        if (!(current->flags & CHAT_MEMBER_FLAG_DELETED) && assignmentDelegate->assignmentState(memberEd) == assignment) {
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "member menu assignment peer=%s assigned=%d", ed.toHex().constData(), assignment);
            emit assignmentRequested(memberEd);
        } else DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "member menu assignment changed while open peer=%s", ed.toHex().constData());
        return;
    }
    if (current->is_self && chosen != editAction && chosen != filesAction && chosen != finishAction) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member menu action cancelled: target became self peer=%s", ed.toHex().constData());
        return;
    }
    if ((chosen == callAction || chosen == radioCallAction) && !current->online) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member menu call cancelled: peer went offline peer=%s", ed.toHex().constData());
        return;
    }
    const QString name = QString::fromUtf8(current->name);
    if (clipboardAction && chosen == clipboardAction) {
        if (!current->is_self && current->online && !(current->flags & CHAT_MEMBER_FLAG_DELETED) &&
            m_db && m_db->isOpen() && m_db->clipboardAllowed(channel)) emit clipboardRequested(channel, ed, name);
        else DEBUG_WARN(DEBUG_CATEGORY_CHAT, "member clipboard action revoked group=%s peer=%s", qPrintable(channel), ed.toHex().constData());
        return;
    }
    if (finishAction && chosen == finishAction && current->is_self) { finishDevicePairing(this); return; }
    if (myDeviceAction && chosen == myDeviceAction) { confirmMyDeviceLink(ed); return; }
    if (chosen == inviteAction) { emit inviteRequested(channel, ed, name); return; }
    if (chosen == radioCallAction) { emit radioCallRequested(channel, ed); return; }
    if (chosen == filesAction) { emit filesRequested(channel, ed); return; }
    if (chosen == pmAction) {
        bool ownGroup = false, groups = false; const uint8_t noGroup[32] = {0};
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const auto item = m_model->index(row);
            if (item.data(Qt::UserRole + 2).toBool() && !item.data(MemberListModel::DevgroupRole).toString().isEmpty() &&
                !(item.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_DELETED)) ownGroup = true;
        }
        if (ownGroup && !memcmp(current->devgroup, noGroup, 32)) {
            QMessageBox choice(QMessageBox::Question, QCoreApplication::translate("ChatGui", "Send message"),
                QCoreApplication::translate("ChatGui", "Choose which devices participate in this conversation."), QMessageBox::Cancel, this);
            auto* withGroups = choice.addButton(QCoreApplication::translate("ChatGui", "With device groups"), QMessageBox::AcceptRole);
            auto* devices = choice.addButton(QCoreApplication::translate("ChatGui", "Only these devices"), QMessageBox::AcceptRole);
            choice.exec(); if (!alive || channel != m_channelId) return;
            if (choice.clickedButton() != withGroups && choice.clickedButton() != devices) return;
            groups = choice.clickedButton() == withGroups;
            const auto latest = m_model->member(ed);
            if (!latest || latest->is_self || memcmp(latest->devgroup, current->devgroup, 32)) return;
        }
        emit pmRequested(channel, ed, name, groups); return;
    }
    if (chosen == callAction) { emit callRequested(channel, ed, name); return; }
    if (chosen == connectAction) {
        auto* request = static_cast<chat_connect_node_req*>(u_calloc(1, sizeof(chat_connect_node_req)));
        if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "member connect request allocation failed"); return; }
        request->inst = gui_bridge_get_inst(); memcpy(request->ed, ed.constData(), sizeof(request->ed));
        strncpy(request->ch_id, channel.toUtf8().constData(), sizeof(request->ch_id) - 1);
        if (gui_bridge_post_uasync_owned_fn(chat_core_connect_node_trampoline, request) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "member connect request rejected channel=%s peer=%s",
                       qPrintable(channel), ed.toHex().constData());
            u_free(request);
        }
        return;
    }
    if (chosen == renameAction || chosen == editAction) editMember(ed, chosen == editAction);
}
void AccountList::onMemberList(const QString& chId, int count, const uint8_t* data, int dataLen) {
    m_model->onMemberList(chId, count, data, dataLen);
}

void AccountList::onMemberUpdated(const QString& chId, const uint8_t* data) {
    m_model->onMemberUpdated(chId, data);
}

void AccountList::onMemberRemoved(const QString& chId, const QByteArray& ed) {
    m_model->onMemberRemoved(chId, ed);
}

void AccountList::setAssignmentState(std::function<int(const QString&)> state) {
    for (auto* view : {m_listView, m_groupDevices}) {
        if (auto* delegate = qobject_cast<AccountDelegate*>(view->itemDelegate())) delegate->assignmentState = state;
    }
    refreshAssignments();
}
void AccountList::refreshAssignments() {
    m_listView->viewport()->update(); m_groupDevices->viewport()->update();
}
