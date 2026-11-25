#include "invite_link.h"
#define InviteData CoreInviteData
#define InviteAddr CoreInviteAddr
#define InviteReality CoreInviteReality
extern "C" {
#include "../../../src/chat/invite_link.h"
}
#undef InviteData
#undef InviteAddr
#undef InviteReality

InviteData decodeInviteLink(const QString& link) {
    InviteData result;
    CoreInviteData data; char error[128];
    const QByteArray bytes = link.toUtf8();
    if (invite_link_decode(bytes.constData(), (size_t)bytes.size(), &data, error, sizeof(error))) {
        result.error = QString::fromUtf8(error); return result;
    }
    result.kind = data.kind; result.joinKey = data.join_key;
    result.pubkey = QByteArray((const char*)data.pubkey, 32);
    result.reality.has = data.reality.has;
    if (data.reality.has) {
        result.reality.shortId = QByteArray((const char*)data.reality.short_id, 8);
        result.reality.serverPubkey = QByteArray((const char*)data.reality.server_pubkey, 32);
        result.reality.version = QByteArray((const char*)data.reality.version, 3);
        result.reality.serverName = QString::fromUtf8(data.reality.server_name);
    }
    for (unsigned i = 0; i < data.addrCount; i++) {
        const auto& a = data.addrs[i];
        result.addrs.append({a.family, QByteArray((const char*)a.address, a.family == 6 ? 16 : 4), a.port, a.socketId, a.proto});
    }
    return result;
}
