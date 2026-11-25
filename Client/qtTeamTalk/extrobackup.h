/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Unified cross-platform JSON Backup & Restore (compatible with Android).
 */

#ifndef EXTROBACKUP_H
#define EXTROBACKUP_H

#include <QString>

class QWidget;

namespace ExtroBackup
{

bool exportToFile(const QString& filepath, QString& summaryOrError);
bool importFromFile(const QString& filepath, QString& summaryOrError);

bool exportInteractive(QWidget* parent);
bool importInteractive(QWidget* parent);

void applyStoredExtroSettings();

} // namespace ExtroBackup

#endif
