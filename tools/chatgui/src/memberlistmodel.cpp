#include <QCoreApplication>
#include "memberlistmodel.h"
#include "debug_ui.h"
#include "flagpainter.h"
#include "avatars/avatarmanager.h"
#include "../transport/gui_bridge.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QColor>
#include <QIcon>
#include <algorithm>
#include <utility>
#include <cstring>

extern "C" {
#include "../../../src/chat/chat_core.h"
#include "../../../src/chat/chat_member.h"
#include "../../../src/routing_layer/topo_node.h"
#include "../../../lib/mem.h"
}

static bool deserialize_member(const uint8_t* src, MemberListItem& item);

static void applyPhoto(MemberListItem& item, const QString& channel) {
    auto* manager = AvatarManager::instance(); if (!manager) return;
    const QImage image = manager->image(channel, QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32)); if (image.isNull()) return;
    const int size = MemberListModel::kAvatarSize;
    QPixmap pixmap(size, size); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip; clip.addEllipse(QRectF(0, 0, size, size)); painter.setClipPath(clip);
    painter.drawImage(QRect(0, 0, size, size), image); painter.setClipping(false);
    if (item.snapshot.flags & CHAT_MEMBER_FLAG_SUPERNODE) FlagPainter::drawServer(&painter, 0, 0, size / 3);
    if (item.snapshot.storage) FlagPainter::drawStorage(&painter, size * 2 / 3, 0, size / 3);
    const QColor statusColor = item.snapshot.online ? QColor("#4CAF50") : QColor("#9E9E9E");
    painter.setPen(QPen(statusColor, 1)); painter.setBrush(statusColor);
    painter.drawEllipse(QRect(size - 9, size - 9, 8, 8)); painter.end(); item.avatar = pixmap;
}

MemberListModel::MemberListModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_rttTimer(new QTimer(this))
{
    if (auto* manager = AvatarManager::instance()) connect(manager, &AvatarManager::changed, this,
        [this](const QString& channel, const QByteArray& owner) {
            if (channel != m_channelId) return;
            for (int row = 0; row < m_members.size(); ++row) if (owner.size() == 32 && !memcmp(m_members[row].snapshot.ed25519, owner.constData(), 32)) {
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; chat_core_serialize_member(&m_members[row].snapshot, wire);
                if (!deserialize_member(wire, m_members[row])) return;
                applyPhoto(m_members[row], channel); emit dataChanged(index(row), index(row), {Qt::DecorationRole});
            }
        });
    m_rttTimer->setInterval(2000);
    connect(m_rttTimer, &QTimer::timeout, this, &MemberListModel::refreshRtt);
    m_rttTimer->start();
}

int MemberListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_members.size());
}

QVariant MemberListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.model() != this || index.column() != 0 || index.row() < 0 || index.row() >= m_members.size())
        return {};

    const auto& item = m_members.at(index.row());
    if (role == ProtocolWarningRole) return item.snapshot.protocol_state == 2 ?
        QCoreApplication::translate("ChatGui", "Incompatible group protocol: local %1, peer %2")
            .arg(item.snapshot.protocol_local).arg(item.snapshot.protocol_remote) : QString();
    if (role == Qt::DisplayRole) return item.name;
    if (role == Qt::DecorationRole) return QVariant::fromValue(QIcon(item.avatar));
    if (role == Qt::UserRole + 1) return routeText(item.snapshot, false);
    if (role == Qt::ToolTipRole)
        return QCoreApplication::translate("ChatGui", item.snapshot.online ? "online" : "offline") + " | 0x" +
               QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32).toHex()) +
               ((item.snapshot.flags & CHAT_MEMBER_FLAG_RADIO) ? QCoreApplication::translate("ChatGui", " · Radio enabled") : QString());
    if (role == Qt::UserRole + 2) return item.snapshot.is_self;
    if (role == Qt::UserRole + 3) return item.snapshot.flags;
    if (role == Qt::UserRole + 4) return bool(item.snapshot.storage);
    if (role == RadioSpeakingRole) return item.snapshot.is_self ? m_selfTransmitting : m_radioTalkers.contains(QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32));
    if (role == OnlineRole) return item.snapshot.online;
    if (role == Ed25519Role) return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32).toHex());
    if (role == AvatarEdRole) return QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32);
    if (role == ActiveDevgroupRole && (item.snapshot.flags & CHAT_MEMBER_FLAG_DELETED)) return QString();
    if (role == DevgroupRole || role == ActiveDevgroupRole) return item.snapshot.devgroup_name[0] ?
        QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(item.snapshot.devgroup), 32).toHex()) : QString();
    return {};
}

QString MemberListModel::routeText(const chat_member_display& peer, bool detailed) const {
    if (!peer.online) return QCoreApplication::translate("ChatGui", "offline");
    if (peer.is_self) return QStringLiteral("SELF");
    QString route;
    if (peer.conn_up & NCONN_DIRECT) {
        route = QStringLiteral("DIR");
        if (detailed) {
            uint8_t localNat = 255;
            for (const auto& item : m_members) if (item.snapshot.is_self) { localNat = item.snapshot.nat_type; break; }
            auto nat = [](uint8_t type) {
                static const char* names[] = {"NET", "NONE", "EIM", "STRICT", "NAT?", "CONTACT", "EIM?"};
                return QString::fromLatin1(type < sizeof(names) / sizeof(names[0]) ? names[type] : "?");
            };
            route += QStringLiteral("(%1-%2)").arg(nat(localNat), nat(peer.nat_type));
        }
    } else if (peer.conn_up & NCONN_INDIRECT) route = QStringLiteral("IND");
    else route = detailed ? QStringLiteral("BGP(%1 hop)").arg(peer.intermediaries) : QStringLiteral("B%1").arg(peer.intermediaries);
    if (peer.rtt != UINT16_MAX) {
        QString rtt = QString::number(peer.rtt / 10.0, 'f', peer.rtt % 10 ? 1 : 0);
        route += (detailed ? QStringLiteral(" RTT=%1ms") : QStringLiteral(", RTT=%1ms")).arg(rtt);
    }
    return route;
}

void MemberListModel::setChannel(const QString& channelId, bool isOwner) {
    const QByteArray channel = channelId.toUtf8();
    if (channel.size() >= int(sizeof(chat_member_list_req::ch_id)) || channel.contains('\0')) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member channel rejected: bytes=%lld embedded_nul=%d limit=64",
                    (long long)channel.size(), channel.contains('\0')); return;
    }
    const bool changed = m_channelId != channelId;
    if (changed || (m_isOwner && !isOwner)) {
        beginResetModel();
        if (changed) {
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio members: channel %s -> %s cleared talkers=%d self_tx=%d",
                        qPrintable(m_channelId), qPrintable(channelId), int(m_radioTalkers.size()), m_selfTransmitting);
            m_channelId = channelId;
            m_radioTalkers.clear(); m_radioActive = false; m_selfTransmitting = false;
            m_members.clear();
        } else {
            m_members.erase(std::remove_if(m_members.begin(), m_members.end(), [](const MemberListItem& item) {
                return item.snapshot.flags & CHAT_MEMBER_FLAG_DELETED;
            }), m_members.end());
        }
        m_isOwner = isOwner;
        endResetModel();
        emit snapshotChanged({});
    } else m_isOwner = isOwner;
    refresh();
}

void MemberListModel::refresh() {
    if (!m_channelId.isEmpty() && gui_bridge_is_uasync_ready()) {
        struct chat_member_list_req* req = (struct chat_member_list_req*)u_calloc(1, sizeof(*req));
        if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member list request allocation failed"); return; }
        req->inst = gui_bridge_get_inst();
        strncpy(req->ch_id, m_channelId.toUtf8().constData(), sizeof(req->ch_id) - 1);
        if (gui_bridge_post_uasync_owned_fn(chat_core_request_member_list_trampoline, req) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member list request rejected ch=%s", qPrintable(m_channelId)); u_free(req);
        }
    }
}

void MemberListModel::refreshRtt() {
    if (m_channelId.isEmpty() || !gui_bridge_is_uasync_ready()) return;
    struct chat_member_list_req* req = (struct chat_member_list_req*)u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member RTT request allocation failed"); return; }
    req->inst = gui_bridge_get_inst();
    strncpy(req->ch_id, m_channelId.toUtf8().constData(), sizeof(req->ch_id) - 1);
    if (gui_bridge_post_uasync_owned_fn(chat_core_request_member_rtt_trampoline, req) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member RTT request rejected ch=%s", qPrintable(m_channelId)); u_free(req);
    }
}

QByteArray MemberListModel::memberKeyAt(int row) const {
    return index(row).data(AvatarEdRole).toByteArray();
}

bool MemberListModel::isOnlineAt(int row) const {
    return row >= 0 && row < m_members.size() ? m_members.at(row).snapshot.online : false;
}

std::optional<chat_member_display> MemberListModel::member(const QByteArray& ed) const {
    if (ed.size() == 32)
        for (const auto& item : m_members) if (!memcmp(item.snapshot.ed25519, ed.constData(), 32)) return item.snapshot;
    return std::nullopt;
}

static bool deserialize_member(const uint8_t* src, MemberListItem& item) {
    if (chat_core_deserialize_member(src, &item.snapshot) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot decode failed: missing data"); return false;
    }
    item.name = QString::fromUtf8(item.snapshot.name);

    QColor c = item.snapshot.online ? QColor("#4CAF50") : QColor("#9E9E9E");
    const int size = MemberListModel::kAvatarSize;
    QPixmap pm(size, size); pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const bool supernode = item.snapshot.flags & CHAT_MEMBER_FLAG_SUPERNODE;
    const bool storage = item.snapshot.storage;
    if (supernode || storage) {
        if (supernode) FlagPainter::drawServer(&p, 0, 0, storage ? size * 4 / 5 : size);
        if (storage) {
            const int storageSize = supernode ? size * 2 / 3 : size;
            FlagPainter::drawStorage(&p, size - storageSize, size - storageSize, storageSize);
        }
        p.setPen(QPen(c, 1)); p.setBrush(c);
        p.drawEllipse(QRect(size - 9, size - 9, 8, 8));
    } else {
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawEllipse(0, 0, size, size);
        p.setPen(Qt::white);
        QFont f; f.setBold(true); f.setPixelSize(size / 2);
        p.setFont(f);
        p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, item.name.left(1));
    }
    p.end();
    item.avatar = pm;
    return true;
}

void MemberListModel::onMemberList(const QString& chId, int count, const uint8_t* data, int dataLen) {
    if (chId != m_channelId) return;
    if (count < 0 || dataLen < 0 || count != dataLen / CHAT_MEMBER_DISPLAY_SIZE ||
        dataLen % CHAT_MEMBER_DISPLAY_SIZE || (count && !data)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member list: invalid count=%d bytes=%d", count, dataLen); return;
    }

    QVector<MemberListItem> members;
    members.reserve(count);
    for (int i = 0; i < count; i++) {
        MemberListItem item;
        if (!deserialize_member(data + i * CHAT_MEMBER_DISPLAY_SIZE, item)) return;
        applyPhoto(item, chId);
        if ((item.snapshot.flags & CHAT_MEMBER_FLAG_DELETED) && !m_isOwner) continue;
        members.append(item);
    }
    beginResetModel();
    m_members = std::move(members);
    endResetModel();
    const int roleAvatars = std::count_if(m_members.begin(), m_members.end(), [](const MemberListItem& item) {
        return (item.snapshot.flags & CHAT_MEMBER_FLAG_SUPERNODE) || item.snapshot.storage;
    });
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member list applied ch=%s count=%d visible=%d owner=%d role_avatars=%d",
                qPrintable(chId), count, int(m_members.size()), m_isOwner, roleAvatars);
    emit snapshotChanged({});
}

void MemberListModel::onMemberUpdated(const QString& chId, const uint8_t* data) {
    if (chId != m_channelId) return;

    MemberListItem item;
    if (!deserialize_member(data, item)) return;
    applyPhoto(item, chId);

    /* Строка определяется публичным ключом. */
    int row = -1;
    for (int i = 0; i < m_members.size(); ++i) {
        if (!memcmp(m_members[i].snapshot.ed25519, item.snapshot.ed25519, 32)) { row = i; break; }
    }

    if ((item.snapshot.flags & CHAT_MEMBER_FLAG_DELETED) && !m_isOwner) {
        /* deleted member — remove from non-owner's list */
        onMemberRemoved(chId, QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32));
        return;
    }

    char peer[65];
    if (row < 0 || ((m_members[row].snapshot.flags ^ item.snapshot.flags) & CHAT_MEMBER_FLAG_SUPERNODE)
        || m_members[row].snapshot.storage != item.snapshot.storage)
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member avatar updated ch=%s peer=%s supernode=%d storage=%u",
            qPrintable(chId), sc_public_key_hex(item.snapshot.ed25519, peer), bool(item.snapshot.flags & CHAT_MEMBER_FLAG_SUPERNODE),
            item.snapshot.storage);
    if (row >= 0) {
        const auto& previous = m_members[row].snapshot;
        if (previous.online != item.snapshot.online || previous.conn_up != item.snapshot.conn_up
            || previous.intermediaries != item.snapshot.intermediaries || previous.nat_type != item.snapshot.nat_type)
            DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member route updated ch=%s peer=%s online=%u up=%u intermediaries=%u nat=%u display=%s",
                qPrintable(chId), sc_public_key_hex(item.snapshot.ed25519, peer), item.snapshot.online, item.snapshot.conn_up,
                item.snapshot.intermediaries, item.snapshot.nat_type, qPrintable(routeText(item.snapshot, false)));
        m_members[row] = item;
        emit dataChanged(index(row), index(row));
    } else {
        /* new member — add at end */
        row = int(m_members.size());
        beginInsertRows(QModelIndex(), row, row);
        m_members.append(item);
        endInsertRows();
    }
    emit snapshotChanged(QByteArray(reinterpret_cast<const char*>(item.snapshot.ed25519), 32));
}

void MemberListModel::onMemberRemoved(const QString& chId, const QByteArray& ed) {
    if (chId != m_channelId || ed.size() != 32) return;
    for (int row = 0; row < m_members.size(); ++row) {
        if (!memcmp(m_members[row].snapshot.ed25519, ed.constData(), 32)) {
            m_radioTalkers.remove(ed);
            beginRemoveRows(QModelIndex(), row, row);
            m_members.removeAt(row);
            endRemoveRows();
            emit snapshotChanged(ed);
            return;
        }
    }
}

/* Состояние собственного TX берём из аудио-движка: оно включает PTT и VAD. */
void MemberListModel::setRadioState(const QString& groupId, bool active, bool transmitting) {
    active = active && !groupId.isEmpty() && groupId == m_channelId;
    transmitting = active && transmitting;
    if (m_radioActive == active && m_selfTransmitting == transmitting) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio members: state ch=%s group=%s active=%d self_tx=%d talkers=%d",
                qPrintable(m_channelId), qPrintable(groupId), active, transmitting, int(m_radioTalkers.size()));
    m_radioActive = active; m_selfTransmitting = transmitting;
    if (!active) m_radioTalkers.clear();
    if (!m_members.isEmpty()) emit dataChanged(index(0), index(m_members.size() - 1), {RadioSpeakingRole});
}

/* START может прийти до списка участников; STOP снимает только указанный источник. */
void MemberListModel::onRadioTalk(const QString& groupId, const QByteArray& peer, bool on) {
    if (groupId.isEmpty() || peer.size() != 32 || peer == QByteArray(32, '\0')) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio members: invalid talk group=%016llx peer=%s on=%d",
                   qPrintable(groupId), peer.toHex().constData(), on);
        return;
    }
    if (groupId != m_channelId || (on && !m_radioActive)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio members: ignored talk ch=%s group=%016llx peer=%s on=%d active=%d",
                    qPrintable(m_channelId), qPrintable(groupId), peer.toHex().constData(), on, m_radioActive);
        return;
    }
    if (m_radioTalkers.contains(peer) == on) return;
    if (on) m_radioTalkers.insert(peer);
    else m_radioTalkers.remove(peer);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio members: talk ch=%s peer=%s on=%d talkers=%d",
                qPrintable(m_channelId), peer.toHex().constData(), on, int(m_radioTalkers.size()));
    for (int row = 0; row < m_members.size(); ++row) if (!memcmp(m_members[row].snapshot.ed25519, peer.constData(), 32)) {
        emit dataChanged(index(row), index(row), {RadioSpeakingRole});
        return;
    }
}

static QByteArray memberGroup(const MemberListModel* source, int row) {
    const auto member = source->member(source->index(row, 0).data(MemberListModel::AvatarEdRole).toByteArray());
    // Служебный ключ отличается по длине от любого 32-байтового devgroup.
    if (member && (member->flags & CHAT_MEMBER_FLAG_DELETED)) return QByteArrayLiteral("deleted");
    if (!member || !member->devgroup_name[0]) return {};
    return QByteArray(reinterpret_cast<const char*>(member->devgroup), 32);
}
GroupedMemberModel::GroupedMemberModel(MemberListModel* source, QObject* parent) : QAbstractListModel(parent), m_source(source) {
    connect(source, &QAbstractItemModel::modelReset, this, [this] { rebuild(true); });
    connect(source, &QAbstractItemModel::rowsInserted, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::rowsRemoved, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::dataChanged, this, [this] { rebuild(); });
    rebuild();
}
void GroupedMemberModel::rebuild(bool forceReset) {
    QVector<int> rows; QSet<QByteArray> seen;
    QVector<int> order; order.reserve(m_source->rowCount());
    for (int i = 0; i < m_source->rowCount(); ++i) order.append(i);
    std::stable_partition(order.begin(), order.end(), [this](int row) { return m_source->index(row).data(Qt::UserRole + 2).toBool(); });
    const auto deleted = std::stable_partition(order.begin(), order.end(), [this](int row) {
        return !(m_source->index(row).data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_DELETED);
    });
    for (int i : order) {
        const auto group = memberGroup(m_source, i);
        if (group.isEmpty()) { rows.append(i); continue; }
        if (seen.contains(group)) continue;
        seen.insert(group); rows.append(-i-1);
        if (m_expanded.contains(group)) for (int j : order)
            if (memberGroup(m_source, j) == group) rows.append(j);
    }
    if (rows != m_rows) GUI_DEBUG("Member order: self_first=%d rows=%d first_peer=%s deleted=%d expanded=%d",
        !order.isEmpty() && m_source->index(order[0]).data(Qt::UserRole + 2).toBool(), int(rows.size()),
        m_source->memberKeyAt(order.value(0, -1)).toHex().constData(), int(order.end() - deleted),
        m_expanded.contains(QByteArrayLiteral("deleted")));
    if (forceReset || rows != m_rows) { beginResetModel(); m_rows = rows; endResetModel(); }
    else if (!rows.isEmpty()) emit dataChanged(index(0), index(rows.size()-1));
}
int GroupedMemberModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : m_rows.size(); }
int GroupedMemberModel::sourceRow(int row) const { return row >= 0 && row < m_rows.size() ? m_rows[row] : -1; }
bool GroupedMemberModel::toggle(int row) {
    if (row < 0 || row >= m_rows.size() || m_rows[row] >= 0) return false;
    const auto group = memberGroup(m_source, -m_rows[row]-1);
    if (!m_expanded.remove(group)) m_expanded.insert(group);
    GUI_DEBUG("Member group: toggled group=%s expanded=%d", group.toHex().constData(), m_expanded.contains(group));
    rebuild(); return true;
}
int GroupedMemberModel::avatarSourceRow(int row) const {
    if (row < 0 || row >= m_rows.size()) return -1;
    if (m_rows[row] >= 0) return m_rows[row];
    const int first = -m_rows[row]-1;
    const auto group = memberGroup(m_source, first);
    if (group == QByteArrayLiteral("deleted")) return -1;
    auto hasAvatar = [this](int i) {
        const auto member = m_source->member(m_source->index(i, 0).data(MemberListModel::AvatarEdRole).toByteArray());
        return member && member->avatar_file[0];
    };
    if (hasAvatar(first)) return first;
    for (int i = 0; i < m_source->rowCount(); ++i)
        if (memberGroup(m_source, i) == group && hasAvatar(i)) return i;
    return first;
}
int GroupedMemberModel::actionSourceRow(int row) const {
    if (row < 0 || row >= m_rows.size()) return -1;
    if (m_rows[row] >= 0) return m_rows[row];
    const int first = -m_rows[row]-1;
    const auto group = memberGroup(m_source, first);
    if (group == QByteArrayLiteral("deleted")) return -1;
    if (m_source->isOnlineAt(first) && !(m_source->index(first).data(Qt::UserRole + 3).toInt() & MEMBER_FLAG_DELETED)) return first;
    for (int i = 0; i < m_source->rowCount(); ++i)
        if (memberGroup(m_source, i) == group && m_source->isOnlineAt(i) &&
            !(m_source->index(i).data(Qt::UserRole + 3).toInt() & MEMBER_FLAG_DELETED)) return i;
    return -1;
}
QVariant GroupedMemberModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.model() != this || idx.row() < 0 || idx.row() >= m_rows.size()) return {};
    const int row = m_rows[idx.row()], target = actionSourceRow(idx.row());
    const bool deletedHeader = row < 0 && memberGroup(m_source, -row-1) == QByteArrayLiteral("deleted");
    if (role == DeletedGroupRole) return deletedHeader;
    if (deletedHeader) {
        if (role == GroupHeaderRole) return true;
        if (role == GroupExpandedRole) return m_expanded.contains(QByteArrayLiteral("deleted"));
        if (role == Qt::DisplayRole) return QCoreApplication::translate("ChatGui", "Deleted accounts");
        return {};
    }
    if (role == CallAvailableRole) {
        if (row >= 0) return m_source->isOnlineAt(row) && !m_source->index(row).data(Qt::UserRole + 2).toBool();
        const auto group = memberGroup(m_source, -row-1);
        for (int i = 0; i < m_source->rowCount(); ++i)
            if (memberGroup(m_source, i) == group && m_source->isOnlineAt(i) &&
                !m_source->index(i).data(Qt::UserRole + 2).toBool() &&
                !(m_source->index(i).data(Qt::UserRole + 3).toInt() & MEMBER_FLAG_DELETED)) return true;
        return false;
    }
    if (role == GroupHeaderRole) return row < 0;
    if (role == GroupBranchRole) {
        const auto group = row < 0 ? QByteArray() : memberGroup(m_source, row);
        if (group.isEmpty()) return 0;
        const int next = idx.row() + 1;
        return next < m_rows.size() && m_rows[next] >= 0 && memberGroup(m_source, m_rows[next]) == group ? 1 : 2;
    }
    if (role == ActionPeerRole) return m_source->index(target).data(MemberListModel::AvatarEdRole);
    if (role == MemberListModel::Ed25519Role) return m_source->index(target).data(role);
    if (row >= 0) return m_source->data(m_source->index(row), role);
    const int avatar = avatarSourceRow(idx.row()), first = -row-1;
    const auto group = memberGroup(m_source, first);
    const auto member = m_source->member(m_source->index(first, 0).data(MemberListModel::AvatarEdRole).toByteArray()); if (!member) return {};
    if (role == Qt::DecorationRole) DEBUG_TRACE(DEBUG_CATEGORY_CHAT_SYNC, "device group avatar: first=%s avatar=%s",
        m_source->memberKeyAt(first).toHex().constData(), m_source->memberKeyAt(avatar).toHex().constData());
    if (role == GroupExpandedRole) return m_expanded.contains(group);
    if (role == Qt::DisplayRole) return QString::fromUtf8(member->devgroup_name);
    if (role == Qt::DecorationRole) return m_source->data(m_source->index(avatar), role);
    if (role == MemberListModel::AvatarEdRole || role == MemberListModel::DevgroupRole)
        return m_source->data(m_source->index(avatar), role);
    if (role == MemberListModel::OnlineRole) return target >= 0;
    if (role == Qt::UserRole + 3) {
        const int shared = CHAT_MEMBER_FLAG_RADIO | CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY;
        int flags = target < 0 ? 0 : m_source->index(target).data(role).toInt() & ~shared;
        for (int i = 0; i < m_source->rowCount(); ++i) if (memberGroup(m_source, i) == group) {
            const int deviceFlags = m_source->index(i).data(role).toInt();
            if (!(deviceFlags & MEMBER_FLAG_DELETED)) flags |= deviceFlags & shared;
        }
        return flags;
    }
    if (role == Qt::UserRole + 1) return target < 0 ? QCoreApplication::translate("ChatGui", "offline") :
        m_source->data(m_source->index(target), Qt::DisplayRole).toString() + " · " + m_source->data(m_source->index(target), role).toString();
    return target < 0 ? QVariant() : m_source->data(m_source->index(target), role);
}
