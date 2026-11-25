#include <QCoreApplication>
#include "emoji.h"

QHash<QString, QString> g_animatedEmojiMap;

EmojiData EmojiData::unicodeEmoji(const QString &u, const QString &s) {
    EmojiData d;
    d.type = EmojiType::Unicode;
    d.unicode = u;
    d.shortcode = s;
    return d;
}

EmojiData EmojiData::animatedEmoji(const QString &u, const QString &s, const QString &animPath) {
    EmojiData d;
    d.type = EmojiType::Animation;
    d.unicode = u;
    d.shortcode = s;
    d.animPath = animPath;
    g_animatedEmojiMap[u] = animPath;
    return d;
}

static QVector<EmojiData> smileys() {
    return {
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x80", ":grinning:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x83", ":smiley:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x84", ":smile:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x81", ":grin:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x86", ":laughing:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x85", ":sweat_smile:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x82", ":joy:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\xA3", ":rofl:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x8A", ":blush:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x87", ":innocent:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x99\x82", ":slight_smile:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x89", ":wink:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x8D", ":heart_eyes:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA5\xB0", ":smiling_face_3_hearts:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x98", ":kissing_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x97", ":kissing:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x99", ":kissing_smiling_eyes:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x9B", ":stuck_out_tongue:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x9D", ":stuck_out_tongue_closed_eyes:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x9C", ":stuck_out_tongue_wink:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA5\xB4", ":woozy_face:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\xAA", ":zany_face:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x8E", ":sunglasses:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\xA9", ":star_struck:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x97", ":hugging:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x94", ":thinking:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\xAB", ":shushing:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x90", ":zipper_mouth:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x90", ":neutral_face:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\x91", ":expressionless:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x99\x84", ":rolling_eyes:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xAF", ":hushed:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xB3", ":flushed:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA5\xB5", ":hot_face:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA5\xB6", ":cold_face:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xA8", ":fearful:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xB1", ":scream:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xA2", ":cry:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xAD", ":sob:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xA4", ":triumph:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x98\xA0", ":angry:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\xA9", ":poop:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\xBB", ":ghost:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x80", ":skull:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\xBD", ":alien:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x96", ":robot:"),
    };
}

static QVector<EmojiData> gestures() {
    return {
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x8D", ":+1:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x8E", ":-1:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x8F", ":clap:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x99\x8C", ":raised_hands:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x99\x8F", ":pray:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x9D", ":handshake:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\xAA", ":muscle:"),
        EmojiData::unicodeEmoji("\xE2\x9C\x8A", ":fist:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x8B", ":wave:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x8C", ":ok_hand:"),
        EmojiData::unicodeEmoji("\xE2\x98\x9D\xEF\xB8\x8F", ":point_up:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x86", ":point_up_2:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x87", ":point_down:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x88", ":point_left:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x91\x89", ":point_right:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x98", ":metal:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x9E", ":fingers_crossed:"),
        EmojiData::unicodeEmoji("\xE2\x9C\x8C\xEF\xB8\x8F", ":v:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x9F", ":love_you_gesture:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x8C", ":pinched_fingers:"),
    };
}

static QVector<EmojiData> hearts() {
    return {
        EmojiData::unicodeEmoji("\xE2\x9D\xA4\xEF\xB8\x8F", ":heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA7\xA1", ":orange_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x9B", ":yellow_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x9A", ":green_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x99", ":blue_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x9C", ":purple_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x8E", ":brown_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x96\xA4", ":black_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\xA4\x8D", ":white_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x94", ":broken_heart:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x95", ":two_hearts:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x98", ":cupid:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\x97", ":heartbeat:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x94\xA5", ":fire:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\xAF", ":100:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\xA5", ":boom:"),
        EmojiData::unicodeEmoji("\xE2\xAD\x90", ":star:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x8C\x9F", ":glowing_star:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\xAB", ":dizzy:"),
        EmojiData::unicodeEmoji("\xF0\x9F\x92\xA4", ":zzz:"),
    };
}

static QVector<EmojiData> animations() {
    return {
        EmojiData::animatedEmoji("\xE2\x9C\xA8", ":sparkles:",          ":/animations/sparkles_emoji.tgs"),
        EmojiData::animatedEmoji("\xF0\x9F\x8F\xB3\xEF\xB8\x8F", ":flag:", ":/animations/white_flag_emoji.tgs"),
        EmojiData::animatedEmoji("\xF0\x9F\x91\x8B", ":wave:",           ":/animations/greeting.tgs"),
        EmojiData::animatedEmoji("\xE2\x9C\x8B", ":raised_hand:",        ":/animations/stop.tgs"),
        EmojiData::animatedEmoji("\xF0\x9F\xA4\x96", ":robot:",          ":/animations/robot.tgs"),
    };
}

QVector<EmojiCategory> builtinEmojiSet() {
    return {
        {QCoreApplication::translate("ChatGui", "Smileys & People"), smileys()},
        {QCoreApplication::translate("ChatGui", "Gestures"),        gestures()},
        {QCoreApplication::translate("ChatGui", "Hearts & Symbols"), hearts()},
        {QCoreApplication::translate("ChatGui", "Animations"),       animations()},
    };
}

#include <QSet>

static QSet<QString> buildAllEmojiSet() {
    QSet<QString> s;
    for (const auto &cat : builtinEmojiSet())
        for (const auto &e : cat.items)
            s.insert(e.unicode);
    return s;
}

int detectEmojiOnly(const QString &text) {
    static const QSet<QString> emojiSet = buildAllEmojiSet();

    QString t = text.trimmed();
    if (t.isEmpty()) return 0;

    int pos = 0;
    int count = 0;
    while (pos < t.length()) {
        const QChar *ch = t.constData() + pos;
        if (ch->isSpace()) { ++pos; continue; }

        bool matched = false;
        for (const QString &e : emojiSet) {
            if (e.length() <= t.length() - pos
                && QStringView(t).mid(pos, e.length()) == e)
            {
                pos += e.length();
                ++count;
                matched = true;
                break;
            }
        }
        if (!matched) return 0;
        if (count > 3) return 0;
    }
    return (count >= 1 && count <= 3) ? count : 0;
}
