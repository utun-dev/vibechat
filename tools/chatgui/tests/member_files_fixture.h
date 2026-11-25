#ifndef CHATGUI_TEST_MEMBER_FILES_FIXTURE_H
#define CHATGUI_TEST_MEMBER_FILES_FIXTURE_H

#include <QByteArray>
#include <cstring>

#define TEST_FILE_GROUP "2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a"
#define TEST_FILE_OTHER_GROUP "2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b"

#define TEST_FILE_ED_11 "7380a4b28d0b49b46e7ca1e4369949c8eb733ff90b69b3530d7e83f073900979"
#define TEST_FILE_ED_22 "1e269fd26a80bc4eac0d827fcc91945e71165af5c635e94674b7125806ddb013"
#define TEST_FILE_ED_33 "c745e46b13e26a10c3c2b63f3fde24c80e69fa61b494ef2f05838922d3a72973"

static inline QByteArray fileKey(unsigned fixture) {
    switch (fixture) {
    case 11: return QByteArray::fromHex(TEST_FILE_ED_11);
    case 22: return QByteArray::fromHex(TEST_FILE_ED_22);
    case 33: return QByteArray::fromHex(TEST_FILE_ED_33);
    default: return QByteArray(32, '\0');
    }
}

static inline const uint8_t* fileKeyData(const QByteArray& key) {
    return reinterpret_cast<const uint8_t*>(key.constData());
}

static inline QByteArray fileKeyValue(const uint8_t key[32]) {
    return QByteArray(reinterpret_cast<const char*>(key), 32);
}

#endif
