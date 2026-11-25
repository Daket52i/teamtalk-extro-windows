/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Unified cross-platform JSON Backup & Restore (v1 plain / v2 AES-256-GCM encrypted,
 * compatible with TeamTalk Extro for Android).
 */

#ifndef EXTROBACKUP_H
#define EXTROBACKUP_H

#include <QString>

class QWidget;

namespace ExtroBackup
{

bool exportToFile(const QString& filepath,
                  QString& summaryOrError,
                  const QString& password = QString());
bool importFromFile(const QString& filepath,
                    QString& summaryOrError,
                    const QString& password = QString());

bool exportInteractive(QWidget* parent);
bool importInteractive(QWidget* parent);

void applyStoredExtroSettings();

} // namespace ExtroBackup

#endif
