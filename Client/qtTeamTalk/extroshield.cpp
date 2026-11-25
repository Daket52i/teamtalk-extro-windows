/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Extro Shield: Safe Link Guard, Anti-Flood & Anti-Zalgo / Unicode Spoof Guard,
 * Security Audit Log, and Local User Notes & Tags.
 */

#include "extroshield.h"
#include "appinfo.h"
#include "settings.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

extern NonDefaultSettings* ttSettings;

namespace
{

struct ExtroShieldTr
{
    Q_DECLARE_TR_FUNCTIONS(ExtroShield)
};

constexpr int kMaxAuditEntries = 200;
constexpr qint64 kFloodWindowMs = 3000;
constexpr int kFloodMaxMessages = 6;
constexpr qint64 kFloodNoticeCooldownMs = 5000;

QMutex g_shieldMutex;
QVector<ExtroShield::AuditEntry> g_auditLog;
QMap<int, QVector<qint64>> g_senderTimestamps;
QMap<int, qint64> g_senderLastFloodNotice;

bool isBidiSpoofChar(ushort code)
{
    // RLO (U+202E), LRO (U+202D), LRE/RLE/PDF (U+202A..U+202C),
    // LRI/RLI/FSI/PDI (U+2066..U+2069)
    return (code >= 0x202A && code <= 0x202E) ||
           (code >= 0x2066 && code <= 0x2069);
}

bool hostMatchesDomain(const QString& hostLower, const QString& domain)
{
    return hostLower == domain || hostLower.endsWith(QLatin1Char('.') + domain);
}

} // namespace

namespace ExtroShield
{

bool isSafeLinkEnabled()
{
    if (!ttSettings)
        return SETTINGS_SHIELD_SAFELINK_DEFAULT;
    return ttSettings->value(SETTINGS_SHIELD_SAFELINK, SETTINGS_SHIELD_SAFELINK_DEFAULT).toBool();
}

void setSafeLinkEnabled(bool enabled)
{
    if (ttSettings)
        ttSettings->setValueOrClear(SETTINGS_SHIELD_SAFELINK, enabled, SETTINGS_SHIELD_SAFELINK_DEFAULT);
}

bool isAntiFloodEnabled()
{
    if (!ttSettings)
        return SETTINGS_SHIELD_ANTIFLOOD_DEFAULT;
    return ttSettings->value(SETTINGS_SHIELD_ANTIFLOOD, SETTINGS_SHIELD_ANTIFLOOD_DEFAULT).toBool();
}

void setAntiFloodEnabled(bool enabled)
{
    if (ttSettings)
        ttSettings->setValueOrClear(SETTINGS_SHIELD_ANTIFLOOD, enabled, SETTINGS_SHIELD_ANTIFLOOD_DEFAULT);
}

bool isWarnNoTlsEnabled()
{
    if (!ttSettings)
        return SETTINGS_SHIELD_WARN_NO_TLS_DEFAULT;
    return ttSettings->value(SETTINGS_SHIELD_WARN_NO_TLS, SETTINGS_SHIELD_WARN_NO_TLS_DEFAULT).toBool();
}

void setWarnNoTlsEnabled(bool enabled)
{
    if (ttSettings)
        ttSettings->setValueOrClear(SETTINGS_SHIELD_WARN_NO_TLS, enabled, SETTINGS_SHIELD_WARN_NO_TLS_DEFAULT);
}

void logEvent(const QString& category, const QString& details)
{
    QMutexLocker locker(&g_shieldMutex);
    AuditEntry entry;
    entry.timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    entry.category = category;
    entry.details = details;
    g_auditLog.append(entry);
    while (g_auditLog.size() > kMaxAuditEntries)
        g_auditLog.removeFirst();
}

QVector<AuditEntry> getAuditLog()
{
    QMutexLocker locker(&g_shieldMutex);
    return g_auditLog;
}

void clearAuditLog()
{
    QMutexLocker locker(&g_shieldMutex);
    g_auditLog.clear();
}

QString formatAuditLogText()
{
    const QVector<AuditEntry> entries = getAuditLog();
    if (entries.isEmpty())
        return ExtroShieldTr::tr("No security events recorded yet.");

    QStringList lines;
    lines.reserve(entries.size());
    for (const AuditEntry& e : entries)
    {
        lines.append(QStringLiteral("[%1] [%2] %3").arg(e.timestamp, e.category, e.details));
    }
    return lines.join(QLatin1Char('\n'));
}

QStringList scanMessageForSuspiciousLinks(const QString& text)
{
    QStringList warnings;
    if (text.isEmpty())
        return warnings;

    static const QStringList kIpLoggerSubstrings = {
        QStringLiteral("grabify."),
        QStringLiteral("iplogger."),
        QStringLiteral("2no.co"),
        QStringLiteral("yip.su"),
        QStringLiteral("blasze."),
        QStringLiteral("ps3cfw."),
        QStringLiteral("stopify.co"),
        QStringLiteral("leancoding.co"),
        QStringLiteral("grabify.link")
    };

    static const QStringList kShorteners = {
        QStringLiteral("bit.ly"),
        QStringLiteral("tinyurl.com"),
        QStringLiteral("clck.ru"),
        QStringLiteral("t.co"),
        QStringLiteral("goo.gl"),
        QStringLiteral("cutt.ly"),
        QStringLiteral("is.gd"),
        QStringLiteral("rb.gy"),
        QStringLiteral("shorturl.at"),
        QStringLiteral("vk.cc")
    };

    static const QStringList kExecExts = {
        QStringLiteral(".exe"),
        QStringLiteral(".scr"),
        QStringLiteral(".bat"),
        QStringLiteral(".cmd"),
        QStringLiteral(".vbs"),
        QStringLiteral(".msi"),
        QStringLiteral(".apk"),
        QStringLiteral(".pif"),
        QStringLiteral(".ps1"),
        QStringLiteral(".hta")
    };

    static const QRegularExpression urlRx(
        QStringLiteral("((?:https?://|www\\.)[^\\s<>()\"'\\[\\]]+)"),
        QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression ipv4Rx(
        QStringLiteral("^\\d{1,3}(?:\\.\\d{1,3}){3}$"));

    auto addUnique = [&warnings](const QString& w) {
        if (!warnings.contains(w))
            warnings.append(w);
    };

    // Check explicit URLs first
    QRegularExpressionMatchIterator it = urlRx.globalMatch(text);
    while (it.hasNext())
    {
        QString rawUrl = it.next().captured(1);
        while (!rawUrl.isEmpty() && QStringLiteral(".,;:!?").contains(rawUrl.back()))
            rawUrl.chop(1);

        QString parseStr = rawUrl;
        if (parseStr.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
            parseStr.prepend(QStringLiteral("http://"));

        QUrl url(parseStr);
        QString host = url.host().toLower();
        // Also inspect raw authority in case punycode was decoded by QUrl
        QString rawLower = rawUrl.toLower();

        for (const QString& bad : kIpLoggerSubstrings)
        {
            if (host.contains(bad) || rawLower.contains(bad))
                addUnique(ExtroShieldTr::tr("IP-logger link (%1)").arg(host.isEmpty() ? rawUrl : host));
        }

        if (rawLower.contains(QStringLiteral("xn--")) || host.contains(QStringLiteral("xn--")))
            addUnique(ExtroShieldTr::tr("Punycode / homograph link (%1)").arg(host.isEmpty() ? rawUrl : host));

        for (const QString& sh : kShorteners)
        {
            if (hostMatchesDomain(host, sh))
                addUnique(ExtroShieldTr::tr("URL shortener (%1)").arg(host));
        }

        if (ipv4Rx.match(host).hasMatch())
            addUnique(ExtroShieldTr::tr("Direct IP link (%1)").arg(host));

        const QString pathLower = url.path().toLower();
        for (const QString& ext : kExecExts)
        {
            if (pathLower.endsWith(ext))
                addUnique(ExtroShieldTr::tr("Direct executable file link (%1)").arg(ext));
        }
    }

    // Also catch bare IP-logger domains without http:// prefix
    const QString textLower = text.toLower();
    for (const QString& bad : kIpLoggerSubstrings)
    {
        if (textLower.contains(bad))
            addUnique(ExtroShieldTr::tr("IP-logger domain (%1)").arg(bad));
    }

    return warnings;
}

MessageCheckResult inspectIncomingMessage(int senderId,
                                          const QString& senderName,
                                          const QString& rawText)
{
    MessageCheckResult result;
    result.sanitizedText = rawText;

    const QString safeSender = senderName.isEmpty()
                                   ? ExtroShieldTr::tr("User #%1").arg(senderId)
                                   : senderName;

    if (isAntiFloodEnabled())
    {
        // 1. Burst flood detection (>6 messages in 3 seconds per sender)
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        bool flooded = false;
        bool emitNotice = false;
        {
            QMutexLocker locker(&g_shieldMutex);
            QVector<qint64>& times = g_senderTimestamps[senderId];
            int writeIdx = 0;
            for (int i = 0; i < times.size(); ++i)
            {
                if (nowMs - times[i] <= kFloodWindowMs)
                    times[writeIdx++] = times[i];
            }
            times.resize(writeIdx);
            times.append(nowMs);

            if (times.size() > kFloodMaxMessages)
            {
                flooded = true;
                qint64 lastNotice = g_senderLastFloodNotice.value(senderId, 0);
                if (nowMs - lastNotice >= kFloodNoticeCooldownMs)
                {
                    g_senderLastFloodNotice[senderId] = nowMs;
                    emitNotice = true;
                }
            }
        }

        if (flooded)
        {
            result.blockFlood = true;
            result.firstFloodNotice = emitNotice;
            if (emitNotice)
            {
                logEvent(ExtroShieldTr::tr("Anti-Flood"),
                         ExtroShieldTr::tr("Suppressed message flood burst from %1 (>6 messages in 3s)")
                             .arg(safeSender));
            }
            return result;
        }

        // 2. Sanitize RLO / Bidi spoof characters and Zalgo combining mark floods
        QString cleaned;
        cleaned.reserve(rawText.size());
        int consecutiveMarks = 0;
        for (int i = 0; i < rawText.size(); ++i)
        {
            const QChar ch = rawText.at(i);
            if (isBidiSpoofChar(ch.unicode()))
            {
                result.hadRloSpoof = true;
                continue;
            }
            if (ch.isMark())
            {
                ++consecutiveMarks;
                if (consecutiveMarks > 6)
                {
                    result.hadZalgo = true;
                    continue;
                }
                if (consecutiveMarks <= 2)
                    cleaned.append(ch);
            }
            else
            {
                consecutiveMarks = 0;
                cleaned.append(ch);
            }
        }

        if (result.hadRloSpoof || result.hadZalgo)
            result.sanitizedText = cleaned;

        if (result.hadRloSpoof)
        {
            logEvent(ExtroShieldTr::tr("Unicode Spoof"),
                     ExtroShieldTr::tr("Removed bidi/RLO spoof characters in message from %1")
                         .arg(safeSender));
        }
        if (result.hadZalgo)
        {
            logEvent(ExtroShieldTr::tr("Anti-Zalgo"),
                     ExtroShieldTr::tr("Neutralized Zalgo combining-mark spam in message from %1")
                         .arg(safeSender));
        }
    }

    if (isSafeLinkEnabled())
    {
        result.linkWarnings = scanMessageForSuspiciousLinks(result.sanitizedText);
        if (!result.linkWarnings.isEmpty())
        {
            logEvent(ExtroShieldTr::tr("Safe Link Guard"),
                     ExtroShieldTr::tr("Suspicious link from %1: %2")
                         .arg(safeSender, result.linkWarnings.join(QStringLiteral(", "))));
        }
    }

    return result;
}

void resetFloodTracker()
{
    QMutexLocker locker(&g_shieldMutex);
    g_senderTimestamps.clear();
    g_senderLastFloodNotice.clear();
}

void showAuditLogDialog(QWidget* parent)
{
    QDialog dlg(parent, QT_DEFAULT_DIALOG_HINTS);
    dlg.setWindowIcon(QIcon(APPICON));
    dlg.setWindowTitle(ExtroShieldTr::tr("Extro Shield - Security Audit Log"));
    dlg.setAccessibleDescription(ExtroShieldTr::tr("Recent security events detected by Extro Shield"));
    dlg.resize(640, 440);

    auto* layout = new QVBoxLayout(&dlg);
    auto* infoLabel = new QLabel(
        ExtroShieldTr::tr("Last %1 security events (flood blocks, suspicious links, Zalgo/spoof protection, TLS status, kicks/bans):").arg(kMaxAuditEntries),
        &dlg);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto* textEdit = new QPlainTextEdit(&dlg);
    textEdit->setReadOnly(true);
    textEdit->setAccessibleName(ExtroShieldTr::tr("Security Audit Log"));
    textEdit->setPlainText(formatAuditLogText());
    layout->addWidget(textEdit);

    auto* btnRow = new QHBoxLayout();
    auto* copyBtn = new QPushButton(ExtroShieldTr::tr("&Copy"), &dlg);
    auto* exportBtn = new QPushButton(ExtroShieldTr::tr("&Export..."), &dlg);
    auto* clearBtn = new QPushButton(ExtroShieldTr::tr("C&lear"), &dlg);
    auto* closeBtn = new QPushButton(ExtroShieldTr::tr("Cl&ose"), &dlg);
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(exportBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    QObject::connect(copyBtn, &QPushButton::clicked, &dlg, [textEdit]() {
        if (QClipboard* cb = QApplication::clipboard())
            cb->setText(textEdit->toPlainText());
    });

    QObject::connect(exportBtn, &QPushButton::clicked, &dlg, [&dlg, textEdit]() {
        QString startDir = ttSettings ? ttSettings->value(SETTINGS_LAST_DIRECTORY, QDir::homePath()).toString() : QDir::homePath();
        QString defName = startDir + QDir::separator() +
                          QStringLiteral("ExtroShield_AuditLog_") +
                          QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) +
                          QStringLiteral(".txt");
        QString path = QFileDialog::getSaveFileName(
            &dlg,
            ExtroShieldTr::tr("Export Security Audit Log"),
            defName,
            ExtroShieldTr::tr("Text Files (*.txt);;All Files (*.*)"));
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        {
            QMessageBox::critical(&dlg, ExtroShieldTr::tr("Export Security Audit Log"),
                                  ExtroShieldTr::tr("Failed to write file: %1").arg(path));
            return;
        }
        f.write(textEdit->toPlainText().toUtf8());
        f.close();
        QMessageBox::information(&dlg, ExtroShieldTr::tr("Export Security Audit Log"),
                                 ExtroShieldTr::tr("Security audit log saved to:\n%1").arg(QDir::toNativeSeparators(path)));
    });

    QObject::connect(clearBtn, &QPushButton::clicked, &dlg, [textEdit]() {
        clearAuditLog();
        textEdit->setPlainText(formatAuditLogText());
    });

    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

// Local User Notes & Tags
QString noteKeyForUser(const QString& username, const QString& nickname)
{
    QString u = username.trimmed();
    if (!u.isEmpty())
        return u;
    return nickname.trimmed();
}

QMap<QString, QString> loadUserNotes()
{
    QMap<QString, QString> notes;
    if (!ttSettings)
        return notes;

    const QString raw = ttSettings->value(SETTINGS_USER_NOTES_JSON, QString()).toString();
    if (raw.isEmpty())
        return notes;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return notes;

    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
    {
        const QString k = it.key().trimmed();
        const QString v = it.value().toString().trimmed();
        if (!k.isEmpty() && !v.isEmpty())
            notes.insert(k, v);
    }
    return notes;
}

void saveUserNotes(const QMap<QString, QString>& notes)
{
    if (!ttSettings)
        return;

    QJsonObject obj;
    for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
    {
        const QString k = it.key().trimmed();
        const QString v = it.value().trimmed();
        if (!k.isEmpty() && !v.isEmpty())
            obj.insert(k, v);
    }

    if (obj.isEmpty())
    {
        ttSettings->remove(SETTINGS_USER_NOTES_JSON);
    }
    else
    {
        const QString jsonStr = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        ttSettings->setValue(SETTINGS_USER_NOTES_JSON, jsonStr);
    }
}

QString getUserNote(const QString& username, const QString& nickname)
{
    const QMap<QString, QString> notes = loadUserNotes();
    if (notes.isEmpty())
        return QString();

    const QString u = username.trimmed();
    if (!u.isEmpty())
    {
        if (notes.contains(u))
            return notes.value(u);
        for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
        {
            if (it.key().compare(u, Qt::CaseInsensitive) == 0)
                return it.value();
        }
    }

    const QString n = nickname.trimmed();
    if (!n.isEmpty())
    {
        if (notes.contains(n))
            return notes.value(n);
        for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
        {
            if (it.key().compare(n, Qt::CaseInsensitive) == 0)
                return it.value();
        }
    }

    return QString();
}

void setUserNote(const QString& key, const QString& note)
{
    const QString k = key.trimmed();
    if (k.isEmpty())
        return;

    QMap<QString, QString> notes = loadUserNotes();
    const QString v = note.trimmed();
    // Remove case-insensitive duplicates
    QStringList toRemove;
    for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
    {
        if (it.key().compare(k, Qt::CaseInsensitive) == 0)
            toRemove.append(it.key());
    }
    for (const QString& r : toRemove)
        notes.remove(r);

    if (!v.isEmpty())
        notes.insert(k, v);

    saveUserNotes(notes);
}

bool showEditUserNoteDialog(QWidget* parent,
                            const QString& key,
                            const QString& displayName)
{
    const QString k = key.trimmed();
    if (k.isEmpty())
        return false;

    const QString current = getUserNote(k, displayName);
    bool ok = false;
    const QString newNote = QInputDialog::getText(
        parent,
        ExtroShieldTr::tr("Set User Note"),
        ExtroShieldTr::tr("Local note for %1 (leave empty to remove):").arg(displayName.isEmpty() ? k : displayName),
        QLineEdit::Normal,
        current,
        &ok);
    if (!ok)
        return false;

    setUserNote(k, newNote);
    return true;
}

void showManageUserNotesDialog(QWidget* parent)
{
    QDialog dlg(parent, QT_DEFAULT_DIALOG_HINTS);
    dlg.setWindowIcon(QIcon(APPICON));
    dlg.setWindowTitle(ExtroShieldTr::tr("Manage User Notes & Tags"));
    dlg.setAccessibleDescription(ExtroShieldTr::tr("View, add, edit, or delete local notes about users"));
    dlg.resize(560, 400);

    auto* layout = new QVBoxLayout(&dlg);
    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({ExtroShieldTr::tr("User (Username / Nickname)"), ExtroShieldTr::tr("Note")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->setAccessibleName(ExtroShieldTr::tr("User Notes List"));
    layout->addWidget(table);

    auto populateTable = [table]() {
        table->setRowCount(0);
        const QMap<QString, QString> notes = loadUserNotes();
        int row = 0;
        for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
        {
            table->insertRow(row);
            auto* kItem = new QTableWidgetItem(it.key());
            auto* vItem = new QTableWidgetItem(it.value());
            kItem->setData(Qt::AccessibleTextRole,
                           ExtroShieldTr::tr("%1: %2").arg(it.key(), it.value()));
            table->setItem(row, 0, kItem);
            table->setItem(row, 1, vItem);
            ++row;
        }
    };

    populateTable();

    auto* btnRow = new QHBoxLayout();
    auto* addBtn = new QPushButton(ExtroShieldTr::tr("&Add..."), &dlg);
    auto* editBtn = new QPushButton(ExtroShieldTr::tr("&Edit..."), &dlg);
    auto* delBtn = new QPushButton(ExtroShieldTr::tr("&Delete"), &dlg);
    auto* closeBtn = new QPushButton(ExtroShieldTr::tr("Cl&ose"), &dlg);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(editBtn);
    btnRow->addWidget(delBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    QObject::connect(addBtn, &QPushButton::clicked, &dlg, [&dlg, populateTable]() {
        bool ok = false;
        QString userKey = QInputDialog::getText(
            &dlg,
            ExtroShieldTr::tr("Add User Note"),
            ExtroShieldTr::tr("Username or nickname:"),
            QLineEdit::Normal,
            QString(),
            &ok).trimmed();
        if (!ok || userKey.isEmpty())
            return;
        if (showEditUserNoteDialog(&dlg, userKey, userKey))
            populateTable();
    });

    auto editSelected = [&dlg, table, populateTable]() {
        int row = table->currentRow();
        if (row < 0 || !table->item(row, 0))
            return;
        QString userKey = table->item(row, 0)->text();
        if (showEditUserNoteDialog(&dlg, userKey, userKey))
            populateTable();
    };

    QObject::connect(editBtn, &QPushButton::clicked, &dlg, editSelected);
    QObject::connect(table, &QTableWidget::cellDoubleClicked, &dlg, [editSelected](int, int) {
        editSelected();
    });

    QObject::connect(delBtn, &QPushButton::clicked, &dlg, [table, populateTable]() {
        int row = table->currentRow();
        if (row < 0 || !table->item(row, 0))
            return;
        QString userKey = table->item(row, 0)->text();
        setUserNote(userKey, QString());
        populateTable();
    });

    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

QJsonObject exportShieldSettingsJson()
{
    QJsonObject obj;
    obj.insert(QStringLiteral("safe_link_guard"), isSafeLinkEnabled());
    obj.insert(QStringLiteral("anti_flood_guard"), isAntiFloodEnabled());
    obj.insert(QStringLiteral("warn_no_tls"), isWarnNoTlsEnabled());
    return obj;
}

void importShieldSettingsJson(const QJsonObject& obj)
{
    if (obj.contains(QStringLiteral("safe_link_guard")))
        setSafeLinkEnabled(obj.value(QStringLiteral("safe_link_guard")).toBool(true));
    if (obj.contains(QStringLiteral("anti_flood_guard")))
        setAntiFloodEnabled(obj.value(QStringLiteral("anti_flood_guard")).toBool(true));
    if (obj.contains(QStringLiteral("warn_no_tls")))
        setWarnNoTlsEnabled(obj.value(QStringLiteral("warn_no_tls")).toBool(true));
}

QJsonObject exportUserNotesJson()
{
    QJsonObject obj;
    const QMap<QString, QString> notes = loadUserNotes();
    for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    return obj;
}

void importUserNotesJson(const QJsonObject& obj)
{
    if (obj.isEmpty())
        return;
    QMap<QString, QString> notes = loadUserNotes();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
    {
        const QString k = it.key().trimmed();
        const QString v = it.value().toString().trimmed();
        if (!k.isEmpty() && !v.isEmpty())
            notes.insert(k, v);
    }
    saveUserNotes(notes);
}

} // namespace ExtroShield
