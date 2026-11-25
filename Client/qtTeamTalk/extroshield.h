/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Extro Shield: Safe Link Guard, Anti-Flood & Anti-Zalgo / Unicode Spoof Guard,
 * Security Audit Log, and Local User Notes & Tags.
 */

#ifndef EXTROSHIELD_H
#define EXTROSHIELD_H

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

class QWidget;

namespace ExtroShield
{

struct AuditEntry
{
    QString timestamp;
    QString category;
    QString details;
};

struct MessageCheckResult
{
    bool blockFlood = false;
    bool firstFloodNotice = false;
    bool hadRloSpoof = false;
    bool hadZalgo = false;
    QString sanitizedText;
    QStringList linkWarnings;
};

// Settings getters/setters
bool isSafeLinkEnabled();
void setSafeLinkEnabled(bool enabled);

bool isAntiFloodEnabled();
void setAntiFloodEnabled(bool enabled);

bool isWarnNoTlsEnabled();
void setWarnNoTlsEnabled(bool enabled);

// Message inspection (Safe Link + Anti-Flood + Anti-Zalgo / RLO Spoof)
QStringList scanMessageForSuspiciousLinks(const QString& text);
MessageCheckResult inspectIncomingMessage(int senderId,
                                          const QString& senderName,
                                          const QString& rawText);
void resetFloodTracker();

// Security Audit Log (keeps last 200 events)
void logEvent(const QString& category, const QString& details);
QVector<AuditEntry> getAuditLog();
void clearAuditLog();
QString formatAuditLogText();
void showAuditLogDialog(QWidget* parent);

// Local User Notes & Tags
QString noteKeyForUser(const QString& username, const QString& nickname);
QMap<QString, QString> loadUserNotes();
void saveUserNotes(const QMap<QString, QString>& notes);
QString getUserNote(const QString& username, const QString& nickname);
void setUserNote(const QString& key, const QString& note);
bool showEditUserNoteDialog(QWidget* parent,
                            const QString& key,
                            const QString& displayName);
void showManageUserNotesDialog(QWidget* parent);

// Cross-platform JSON Backup helpers
QJsonObject exportShieldSettingsJson();
void importShieldSettingsJson(const QJsonObject& obj);
QJsonObject exportUserNotesJson();
void importUserNotesJson(const QJsonObject& obj);

} // namespace ExtroShield

#endif
