/* Представление профиля endpoint в GUI: mapping, доступность входа и источник измерения.
 * Использовать для полей enum из address_profile.h; происхождение самого адреса (interface/NAT)
 * не подменяет измерение. Функция форматирует строку, не выполняет сетевых проверок. */
#pragma once

#include <QCoreApplication>
#include <QString>
#include "../../../src/routing_layer/address_profile.h"

/* mapping/inbound/source — enum address_mapping/address_inbound/address_profile_source.
 * Возвращает QString с переводом текущего ChatGui; при source=NONE — NAT not determined.
 * Известный inbound сохраняется при любом mapping; STUN помечается отдельно. UNKNOWN mapping → NAT?,
 * EIM с неизвестным inbound → EIM?. Допустимость полей проверяет вызывающий. */
inline QString addressProfileText(int mapping, int inbound, int source) {
    if (source == ADDRESS_SOURCE_NONE) return QCoreApplication::translate("ChatGui", "NAT not determined");
    QString text;
    switch (mapping) {
    case ADDRESS_MAP_REAL_IP: text = QCoreApplication::translate("ChatGui", "No NAT"); break;
    case ADDRESS_MAP_EIM: text = inbound == ADDRESS_IN_UNKNOWN ? "EIM?" : "EIM"; break;
    case ADDRESS_MAP_DEPENDENT: text = "DEPENDENT"; break;
    default: text = "NAT?"; break;
    }
    if (inbound == ADDRESS_IN_OPEN) text += QCoreApplication::translate("ChatGui", "; inbound open");
    else if (inbound == ADDRESS_IN_CONTACT) text += QCoreApplication::translate("ChatGui", "; inbound filtered");
    else if (mapping == ADDRESS_MAP_REAL_IP) text += QCoreApplication::translate("ChatGui", "; inbound untested");
    if (source == ADDRESS_SOURCE_STUN) text += " (STUN)";
    return text;
}
