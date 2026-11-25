#include <QCoreApplication>
#include "soundsettingspage.h"
#include "sound_manager.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "../../lib/mem.h"
extern "C" {
#include "chat/chat_core.h"
#include "debug_config.h"
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QToolTip>
#include <QVector>

SoundSettingsPage::SoundSettingsPage(DbManager* db, QWidget* parent)
    : QWidget(parent)
    , m_db(db)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(14);

    m_enabledCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Enable sounds"), this);
    m_enabledCheck->setStyleSheet("font-size: 13px;");
    layout->addWidget(m_enabledCheck);

    auto* volLayout = new QHBoxLayout();
    auto* volLabel = new QLabel(QCoreApplication::translate("ChatGui", "Volume:"), this);
    volLabel->setStyleSheet("font-size: 13px;");
    volLayout->addWidget(volLabel);

    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(200);
    m_volumeSlider->setStyleSheet(
        "QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: palette(mid); }"
        "QSlider::handle:horizontal { width: 14px; height: 14px; margin: -4px 0; border-radius: 7px; background: palette(highlight); }");
    volLayout->addWidget(m_volumeSlider);

    m_volumeLabel = new QLabel("80%", this);
    m_volumeLabel->setFixedWidth(36);
    m_volumeLabel->setStyleSheet("font-size: 13px;");
    volLayout->addWidget(m_volumeLabel);
    volLayout->addStretch();
    layout->addLayout(volLayout);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("QFrame { color: palette(mid); }");
    layout->addWidget(sep);

    auto* header = new QLabel(QCoreApplication::translate("ChatGui", "Events"), this);
    header->setStyleSheet("font-size: 13px; font-weight: bold;");
    layout->addWidget(header);

    m_events[0] = {"msg_incoming", QT_TRANSLATE_NOOP("ChatGui", "Incoming message"), nullptr, nullptr, nullptr};
    m_events[1] = {"msg_outgoing", QT_TRANSLATE_NOOP("ChatGui", "Outgoing message"), nullptr, nullptr, nullptr};
    m_events[2] = {"connect",     QT_TRANSLATE_NOOP("ChatGui", "Peer connected"),  nullptr, nullptr, nullptr};
    m_events[3] = {"disconnect",  QT_TRANSLATE_NOOP("ChatGui", "Peer disconnected"), nullptr, nullptr, nullptr};
    m_events[4] = {"call_incoming", QT_TRANSLATE_NOOP("ChatGui", "Incoming call"), nullptr, nullptr, nullptr};
    m_events[5] = {"call_connecting", QT_TRANSLATE_NOOP("ChatGui", "Call: connecting"), nullptr, nullptr, nullptr};
    m_events[6] = {"call_ringing", QT_TRANSLATE_NOOP("ChatGui", "Call: ringback"), nullptr, nullptr, nullptr};
    m_events[7] = {"radio_incoming", QT_TRANSLATE_NOOP("ChatGui", "Radio call"), nullptr, nullptr, nullptr};

    for (int i = 0; i < 8; i++) {
        auto& e = m_events[i];
        auto* row = new QHBoxLayout();
        row->setSpacing(8);

        e.check = new QCheckBox(QCoreApplication::translate("ChatGui", e.label), this);
        e.check->setMinimumWidth(175); row->addWidget(e.check);
        e.slider = new QSlider(Qt::Horizontal, this);
        e.slider->setRange(0, 100);
        e.slider->setMinimumWidth(100); e.slider->setMaximumWidth(220);
        e.slider->setStyleSheet(
            "QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: palette(mid); }"
            "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -3px 0; border-radius: 5px; background: palette(highlight); }");
        row->addWidget(e.slider, 1);

        e.volLabel = new QLabel("80%", this);
        e.volLabel->setFixedWidth(40);
        e.volLabel->setStyleSheet("font-size: 12px;");
        row->addWidget(e.volLabel);

        auto* preview = new QPushButton(QCoreApplication::translate("ChatGui", "Listen"), this);
        preview->setObjectName(QString("previewSound%1").arg(i)); preview->setFixedWidth(120); row->addWidget(preview);
        connect(preview, &QPushButton::clicked, this, [this, i, preview] {
            const float volume = m_volumeSlider->value() * m_events[i].slider->value() / 10000.0f;
            if (!SoundManager::instance()->preview(m_events[i].key, volume))
                QToolTip::showText(preview->mapToGlobal(QPoint(0, preview->height())),
                    QCoreApplication::translate("ChatGui", "Sound playback is unavailable."), preview);
        });
        auto update = [this, i] {
            m_events[i].check->setEnabled(m_enabledCheck->isChecked());
            const bool enabled = m_enabledCheck->isChecked() && m_events[i].check->isChecked();
            m_events[i].slider->setEnabled(enabled); m_events[i].volLabel->setEnabled(enabled);
        };
        connect(m_enabledCheck, &QCheckBox::toggled, this, update);
        connect(e.check, &QCheckBox::toggled, this, update); update();
        layout->addLayout(row);

        connect(e.slider, &QSlider::valueChanged, this, [this, i](int val) {
            m_events[i].volLabel->setText(QString::number(val) + "%");
        });
    }

    layout->addStretch();

    connect(m_enabledCheck, &QCheckBox::stateChanged, this, [this](int state) {
        bool on = (state == Qt::Checked);
        m_volumeSlider->setEnabled(on);
        m_volumeLabel->setEnabled(on);
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int val) {
        m_volumeLabel->setText(QString::number(val) + "%");
    });
}

void SoundSettingsPage::loadFromDb() {
    if (!m_db) return;

    bool enabled = m_db->getUiStateInt("sound_enabled", 1);
    m_enabledCheck->setChecked(enabled);

    int vol = m_db->getUiStateInt("sound_volume", 80);
    m_volumeSlider->setValue(vol);
    m_volumeSlider->setEnabled(enabled);
    m_volumeLabel->setEnabled(enabled);
    m_volumeLabel->setText(QString::number(vol) + "%");

    for (int i = 0; i < 8; i++) {
        auto& e = m_events[i];
        QString onKey  = QStringLiteral("sound_%1_on").arg(e.key);
        QString volKey = QStringLiteral("sound_%1_vol").arg(e.key);
        bool on = m_db->getUiStateInt(onKey, (i == 0 || i >= 4) ? 1 : 0);  // msg_incoming и звонковые on by default
        int ev = m_db->getUiStateInt(volKey, 80);
        e.check->setChecked(on);
        e.slider->setValue(ev);
        e.volLabel->setText(QString::number(ev) + "%");
    }
}

void SoundSettingsPage::applyAndSave() {
    SoundManager* sm = SoundManager::instance();

    bool enabled = m_enabledCheck->isChecked();
    float masterVol = m_volumeSlider->value() / 100.0f;
    sm->setEnabled(enabled);
    sm->setVolume(masterVol);

    QVector<QPair<QByteArray, QByteArray>> values;
    values.reserve(18);
    for (int i = 0; i < 8; i++) {
        auto& e = m_events[i];
        bool on = e.check->isChecked();
        float ev = e.slider->value() / 100.0f;
        sm->setEvent(e.key, on, ev);

        const QByteArray prefix = QByteArray("sound_") + e.key;
        values.append({prefix + "_on", on ? QByteArray("1") : QByteArray("0")});
        values.append({prefix + "_vol", QByteArray::number(e.slider->value())});
    }
    values.append({"sound_enabled", enabled ? QByteArray("1") : QByteArray("0")});
    values.append({"sound_volume", QByteArray::number(m_volumeSlider->value())});

    /* Один путь передачи владения для всех key\0value\0; принятый запрос освобождает callback либо мост при отмене. */
    unsigned queued = 0, failed = 0;
    for (const auto& value : values) {
        const auto& key = value.first;
        const size_t klen = key.size(), vlen = value.second.size(), total = klen + 1 + vlen + 1;
        if (total > sizeof(save_ui_state_arg::data)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Sound settings: key=%s key/value too large bytes=%zu", key.constData(), total);
            ++failed; continue;
        }
        auto* arg = static_cast<save_ui_state_arg*>(u_malloc(sizeof(save_ui_state_arg)));
        if (!arg) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Sound settings: key=%s request allocation failed", key.constData());
            ++failed; continue;
        }
        arg->inst = gui_bridge_get_inst();
        memcpy(arg->data, key.constData(), klen + 1);
        memcpy(arg->data + klen + 1, value.second.constData(), vlen + 1);
        if (gui_bridge_post_uasync_owned_fn(chat_core_save_ui_state_trampoline, arg) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Sound settings: key=%s save rejected by GUI bridge", key.constData());
            u_free(arg); ++failed; continue;
        }
        ++queued;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Sound settings applied locally: enabled=%d volume=%d queued=%u failed=%u",
                enabled, m_volumeSlider->value(), queued, failed);
}
