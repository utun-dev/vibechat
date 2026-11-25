#include <QCoreApplication>
// databasesettingspage.cpp — Database maintenance page in settings
#include "databasesettingspage.h"
#include "../db/db_manager.h"
#include "../../../lib/debug_config.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QFrame>
#include <QFileInfo>
#include <QTimer>

DatabaseSettingsPage::DatabaseSettingsPage(DbManager* db, QWidget* parent)
    : QWidget(parent)
    , m_db(db)
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Database Maintenance"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    // --- DB info ---
    auto* infoLayout = new QFormLayout();
    infoLayout->setSpacing(4);

    QString dbPath = "—";
    QString dbSize = "—";
    QString walSize = "—";
    if (m_db && m_db->m_db) {
        const char* fname = sqlite3_db_filename(m_db->m_db, "main");
        if (fname && fname[0]) {
            dbPath = QString::fromUtf8(fname);
            QFileInfo fi(dbPath);
            if (fi.exists()) {
                qint64 sz = fi.size();
                if (sz < 1024) dbSize = QCoreApplication::translate("ChatGui", "%1 B").arg(sz);
                else if (sz < 1024 * 1024) dbSize = QCoreApplication::translate("ChatGui", "%1 KB").arg(sz / 1024);
                else dbSize = QCoreApplication::translate("ChatGui", "%1 MB").arg(sz / (1024.0 * 1024.0), 0, 'f', 2);
            }
            QFileInfo walFi(dbPath + "-wal");
            if (walFi.exists()) {
                qint64 wsz = walFi.size();
                if (wsz < 1024) walSize = QCoreApplication::translate("ChatGui", "%1 B").arg(wsz);
                else if (wsz < 1024 * 1024) walSize = QCoreApplication::translate("ChatGui", "%1 KB").arg(wsz / 1024);
                else walSize = QCoreApplication::translate("ChatGui", "%1 MB").arg(wsz / (1024.0 * 1024.0), 0, 'f', 2);
            } else {
                walSize = QCoreApplication::translate("ChatGui", "n/a");
            }
        }
    }
    m_filePathLabel = new QLabel(dbPath, this);
    m_filePathLabel->setStyleSheet("font-size: 11px; color: palette(placeholder-text);");
    m_filePathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLayout->addRow(QCoreApplication::translate("ChatGui", "File:"), m_filePathLabel);

    m_fileSizeLabel = new QLabel(dbSize, this);
    m_fileSizeLabel->setStyleSheet("font-size: 11px;");
    infoLayout->addRow(QCoreApplication::translate("ChatGui", "Size:"), m_fileSizeLabel);

    m_walSizeLabel = new QLabel(walSize, this);
    m_walSizeLabel->setStyleSheet("font-size: 11px;");
    infoLayout->addRow("WAL:", m_walSizeLabel);

    layout->addLayout(infoLayout);

    // --- Separator ---
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    // --- Buttons ---
    auto* btnLayout = new QVBoxLayout();
    btnLayout->setSpacing(6);

    auto* quickRow = new QHBoxLayout();
    m_quickBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Quick Check"), this);
    m_quickBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 5px 14px; font-size: 12px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(m_quickBtn, &QPushButton::clicked, this, &DatabaseSettingsPage::onQuickCheck);
    quickRow->addWidget(m_quickBtn);
    auto* quickDesc = new QLabel(QCoreApplication::translate("ChatGui", "Scans structure without verifying indexes (fast)"), this);
    quickDesc->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    quickRow->addWidget(quickDesc, 1);
    btnLayout->addLayout(quickRow);

    auto* fullRow = new QHBoxLayout();
    m_fullBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Full Check"), this);
    m_fullBtn->setStyleSheet(m_quickBtn->styleSheet());
    connect(m_fullBtn, &QPushButton::clicked, this, &DatabaseSettingsPage::onFullCheck);
    fullRow->addWidget(m_fullBtn);
    auto* fullDesc = new QLabel(QCoreApplication::translate("ChatGui", "Verifies all data structures and indexes (thorough)"), this);
    fullDesc->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    fullRow->addWidget(fullDesc, 1);
    btnLayout->addLayout(fullRow);

    auto* optRow = new QHBoxLayout();
    m_optimizeBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Optimize"), this);
    m_optimizeBtn->setStyleSheet(m_quickBtn->styleSheet());
    connect(m_optimizeBtn, &QPushButton::clicked, this, &DatabaseSettingsPage::onOptimize);
    optRow->addWidget(m_optimizeBtn);
    auto* optDesc = new QLabel(QCoreApplication::translate("ChatGui", "Update query planner statistics for better performance"), this);
    optDesc->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    optRow->addWidget(optDesc, 1);
    btnLayout->addLayout(optRow);

    layout->addLayout(btnLayout);

    // --- Status ---
    m_statusLabel = new QLabel("", this);
    m_statusLabel->setStyleSheet("font-size: 12px; margin-top: 4px;");
    layout->addWidget(m_statusLabel);

    // --- Results ---
    m_resultText = new QPlainTextEdit(this);
    m_resultText->setReadOnly(true);
    m_resultText->setMaximumHeight(120);
    m_resultText->setStyleSheet(
        "QPlainTextEdit { font-family: monospace; font-size: 11px;"
        " background: palette(base); border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 4px; }");
    m_resultText->setPlaceholderText(QCoreApplication::translate("ChatGui", "Run a check to see results..."));
    layout->addWidget(m_resultText);

    layout->addStretch();

    if (!m_db || !m_db->m_db) {
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Database not connected"));
        m_statusLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: palette(bright-text);");
        m_quickBtn->setEnabled(false);
        m_fullBtn->setEnabled(false);
        m_optimizeBtn->setEnabled(false);
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "DB maintenance page: manager/database unavailable");
    }
    if (m_db) {
        connect(m_db, &DbManager::databaseChanged, this, [this] { m_dbGeneration++; refreshStatus(); });
        connect(m_db, &QObject::destroyed, this, [this] { m_db = nullptr; m_dbGeneration++; refreshStatus(); });
    }
}

void DatabaseSettingsPage::refreshStatus() {
    if (!m_db || !m_db->m_db) {
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Database not connected"));
        m_statusLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: palette(bright-text);");
        m_quickBtn->setEnabled(false);
        m_fullBtn->setEnabled(false);
        m_optimizeBtn->setEnabled(false);
        return;
    }
    m_quickBtn->setEnabled(!m_busy);
    m_fullBtn->setEnabled(!m_busy);
    m_optimizeBtn->setEnabled(!m_busy);
    const char* fname = sqlite3_db_filename(m_db->m_db, "main");
    if (fname && fname[0]) {
        QString dbPath = QString::fromUtf8(fname);
        m_filePathLabel->setText(dbPath);
        QFileInfo fi(dbPath);
        if (fi.exists()) {
            qint64 sz = fi.size();
            if (sz < 1024) m_fileSizeLabel->setText(QCoreApplication::translate("ChatGui", "%1 B").arg(sz));
            else if (sz < 1024 * 1024) m_fileSizeLabel->setText(QCoreApplication::translate("ChatGui", "%1 KB").arg(sz / 1024));
            else m_fileSizeLabel->setText(QCoreApplication::translate("ChatGui", "%1 MB").arg(sz / (1024.0 * 1024.0), 0, 'f', 2));
            QFileInfo walFi(dbPath + "-wal");
            if (walFi.exists()) {
                qint64 wsz = walFi.size();
                if (wsz < 1024) m_walSizeLabel->setText(QCoreApplication::translate("ChatGui", "%1 B").arg(wsz));
                else if (wsz < 1024 * 1024) m_walSizeLabel->setText(QCoreApplication::translate("ChatGui", "%1 KB").arg(wsz / 1024));
                else m_walSizeLabel->setText(QCoreApplication::translate("ChatGui", "%1 MB").arg(wsz / (1024.0 * 1024.0), 0, 'f', 2));
            } else { m_walSizeLabel->setText(QCoreApplication::translate("ChatGui", "n/a")); }
        }
    }
}

void DatabaseSettingsPage::setBusy(bool busy) {
    m_busy = busy;
    const bool ready = !busy && m_db && m_db->isOpen();
    m_quickBtn->setEnabled(ready);
    m_fullBtn->setEnabled(ready);
    m_optimizeBtn->setEnabled(ready);
    if (busy) {
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Checking..."));
        m_statusLabel->setStyleSheet("font-size: 12px; margin-top: 4px; color: palette(placeholder-text);");
    }
}

void DatabaseSettingsPage::onQuickCheck() {
    if (!m_db || !m_db->m_db) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "DB quick check unavailable: manager/database missing");
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Database not open"));
        m_statusLabel->setStyleSheet("font-size: 12px; color: palette(bright-text);");
        return;
    }
    setBusy(true);
    auto complete = [this](IntegrityResult result) { showResult(QCoreApplication::translate("ChatGui", "Quick Check"), result); };
    if (!m_db->integrityCheckAsync(false, this, complete)) {
        IntegrityResult result; result.message = QCoreApplication::translate("ChatGui", "Database check could not be started"); complete(result);
    }
}

void DatabaseSettingsPage::onFullCheck() {
    if (!m_db || !m_db->m_db) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "DB full check unavailable: manager/database missing");
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Database not open"));
        m_statusLabel->setStyleSheet("font-size: 12px; color: palette(bright-text);");
        return;
    }
    setBusy(true);
    auto complete = [this](IntegrityResult result) { showResult(QCoreApplication::translate("ChatGui", "Full Check"), result); };
    if (!m_db->integrityCheckAsync(true, this, complete)) {
        IntegrityResult result; result.message = QCoreApplication::translate("ChatGui", "Database check could not be started"); complete(result);
    }
}

void DatabaseSettingsPage::onOptimize() {
    if (!m_db || !m_db->m_db) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "DB optimize unavailable: manager/database missing");
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Database not open"));
        m_statusLabel->setStyleSheet("font-size: 12px; color: palette(bright-text);");
        return;
    }
    setBusy(true);
    const auto generation = m_dbGeneration;
    QTimer::singleShot(50, this, [this, generation]() {
        if (!m_db || !m_db->isOpen() || generation != m_dbGeneration) {
            IntegrityResult result; result.sqliteCode = SQLITE_ABORT; result.message = QStringLiteral("Database changed or stopped");
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "DB optimize cancelled: manager/database changed before execution");
            showResult(QCoreApplication::translate("ChatGui", "Optimize"), result); return;
        }
        IntegrityResult r = m_db->integrityOptimize();
        showResult(QCoreApplication::translate("ChatGui", "Optimize"), r);
    });
}

void DatabaseSettingsPage::showResult(const QString& checkName, const IntegrityResult& result) {
    setBusy(false);

    if (result.ok) {
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "%1: OK  (%2 ms)")
                               .arg(checkName).arg(result.elapsedMs));
        m_statusLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: palette(text);");
    } else {
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "%1: FAILED  (%2 ms)")
                               .arg(checkName).arg(result.elapsedMs));
        m_statusLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: palette(bright-text);");
    }

    if (result.errors.isEmpty()) {
        m_resultText->setPlainText(result.message.isEmpty() ? "ok" : result.message);
    } else {
        QStringList lines;
        lines.append(result.message);
        lines.append(result.errors);
        m_resultText->setPlainText(lines.join('\n'));
    }
}
