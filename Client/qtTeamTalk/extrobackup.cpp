/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Unified cross-platform JSON Backup & Restore (compatible with Android).
 */

#include "extrobackup.h"
#include "appinfo.h"
#include "common.h"
#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageBox>
#include <QObject>
#include <QVariant>

extern NonDefaultSettings* ttSettings;
extern TTInstance* ttInst;

namespace ExtroBackup
{

void applyStoredExtroSettings()
{
    if (!ttSettings || !ttInst)
        return;

    VoiceChangerSettings vcs = {};
    vcs.nMode = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_MODE,
                                  SETTINGS_SOUND_VOICECHANGER_MODE_DEFAULT).toInt();
    vcs.bEcho = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_ECHO,
                                  SETTINGS_SOUND_VOICECHANGER_ECHO_DEFAULT).toBool() ? TRUE : FALSE;
    vcs.nEchoLevel = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_ECHO_LEVEL,
                                       SETTINGS_SOUND_VOICECHANGER_ECHO_LEVEL_DEFAULT).toInt();
    vcs.bReverb = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_REVERB,
                                    SETTINGS_SOUND_VOICECHANGER_REVERB_DEFAULT).toBool() ? TRUE : FALSE;
    vcs.nReverbLevel = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_REVERB_LEVEL,
                                         SETTINGS_SOUND_VOICECHANGER_REVERB_LEVEL_DEFAULT).toInt();
    vcs.nRoomSize = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_ROOM_SIZE,
                                      SETTINGS_SOUND_VOICECHANGER_ROOM_SIZE_DEFAULT).toInt();
    TT_SetVoiceChangerSettings(ttInst, &vcs);

    int scale = ttSettings->value(SETTINGS_SOUND_VOICECHANGER_AUTOTUNE_SCALE,
                                  SETTINGS_SOUND_VOICECHANGER_AUTOTUNE_SCALE_DEFAULT).toInt();
    TT_SetVoiceChangerAutoTuneScale(ttInst, scale);

    int karaokeVol = ttSettings->value(SETTINGS_SOUND_KARAOKE_VOLUME,
                                       SETTINGS_SOUND_KARAOKE_VOLUME_DEFAULT).toInt();
    TT_SetKaraokeVolume(ttInst, karaokeVol);

    bool ducking = ttSettings->value(SETTINGS_SOUND_KARAOKE_DUCKING,
                                     SETTINGS_SOUND_KARAOKE_DUCKING_DEFAULT).toBool();
    TT_SetKaraokeDucking(ttInst, ducking ? TRUE : FALSE);

    int pitch = ttSettings->value(SETTINGS_SOUND_KARAOKE_PITCH,
                                  SETTINGS_SOUND_KARAOKE_PITCH_DEFAULT).toInt();
    TT_SetKaraokePitch(ttInst, pitch);
}

bool exportToFile(const QString& filepath, QString& summaryOrError)
{
    if (!ttSettings)
    {
        summaryOrError = QObject::tr("Settings store is not initialized.");
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("teamtalk-extro-backup"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("platform"), QStringLiteral("windows"));
    root.insert(QStringLiteral("app_version"), QStringLiteral(APPVERSION_EXTRO));
    root.insert(QStringLiteral("exported_at"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    // 1. Saved servers (plus fallback to latest hosts if no saved servers exist)
    QJsonArray serversArr;
    QVector<HostEntry> collected;
    HostEntry entry;
    int idx = 0;
    while (getServerEntry(idx++, entry, false))
    {
        collected.append(entry);
        entry = HostEntry();
    }
    if (collected.isEmpty())
    {
        idx = 0;
        while (getServerEntry(idx++, entry, true))
        {
            collected.append(entry);
            entry = HostEntry();
        }
    }

    for (const HostEntry& h : collected)
    {
        QJsonObject s;
        s.insert(QStringLiteral("name"), h.name.isEmpty() ? h.generateEntryName() : h.name);
        s.insert(QStringLiteral("host"), h.ipaddr);
        s.insert(QStringLiteral("tcp_port"), h.tcpport > 0 ? h.tcpport : DEFAULT_TCPPORT);
        s.insert(QStringLiteral("udp_port"), h.udpport > 0 ? h.udpport : DEFAULT_UDPPORT);
        s.insert(QStringLiteral("encrypted"), h.encrypted);
        s.insert(QStringLiteral("username"), h.username);
        s.insert(QStringLiteral("password"), h.password);
        s.insert(QStringLiteral("nickname"), h.nickname);
        s.insert(QStringLiteral("channel"), h.channel);
        s.insert(QStringLiteral("channel_password"), h.chanpasswd);
        serversArr.append(s);
    }
    root.insert(QStringLiteral("servers"), serversArr);

    // 2. Extro cross-platform settings
    QJsonObject extroObj;
    extroObj.insert(QStringLiteral("voice_changer_mode"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_MODE,
                                      SETTINGS_SOUND_VOICECHANGER_MODE_DEFAULT).toInt());
    extroObj.insert(QStringLiteral("echo_enabled"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_ECHO,
                                      SETTINGS_SOUND_VOICECHANGER_ECHO_DEFAULT).toBool());
    extroObj.insert(QStringLiteral("echo_level"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_ECHO_LEVEL,
                                      SETTINGS_SOUND_VOICECHANGER_ECHO_LEVEL_DEFAULT).toInt());
    extroObj.insert(QStringLiteral("reverb_enabled"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_REVERB,
                                      SETTINGS_SOUND_VOICECHANGER_REVERB_DEFAULT).toBool());
    extroObj.insert(QStringLiteral("reverb_level"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_REVERB_LEVEL,
                                      SETTINGS_SOUND_VOICECHANGER_REVERB_LEVEL_DEFAULT).toInt());
    extroObj.insert(QStringLiteral("reverb_room_size"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_ROOM_SIZE,
                                      SETTINGS_SOUND_VOICECHANGER_ROOM_SIZE_DEFAULT).toInt());
    extroObj.insert(QStringLiteral("autotune_scale"),
                    ttSettings->value(SETTINGS_SOUND_VOICECHANGER_AUTOTUNE_SCALE,
                                      SETTINGS_SOUND_VOICECHANGER_AUTOTUNE_SCALE_DEFAULT).toInt());
    extroObj.insert(QStringLiteral("karaoke_volume"),
                    ttSettings->value(SETTINGS_SOUND_KARAOKE_VOLUME,
                                      SETTINGS_SOUND_KARAOKE_VOLUME_DEFAULT).toInt());
    extroObj.insert(QStringLiteral("karaoke_ducking"),
                    ttSettings->value(SETTINGS_SOUND_KARAOKE_DUCKING,
                                      SETTINGS_SOUND_KARAOKE_DUCKING_DEFAULT).toBool());
    root.insert(QStringLiteral("extro"), extroObj);

    // 3. Complete Windows QSettings snapshot
    QJsonObject winSettingsObj;
    const QStringList keys = ttSettings->allKeys();
    for (const QString& key : keys)
    {
        const QVariant val = ttSettings->value(key);
        const int typeId = val.typeId();
        if (typeId == QMetaType::Bool)
        {
            winSettingsObj.insert(key, val.toBool());
        }
        else if (typeId == QMetaType::Int || typeId == QMetaType::LongLong ||
                 typeId == QMetaType::UInt || typeId == QMetaType::ULongLong)
        {
            winSettingsObj.insert(key, static_cast<qint64>(val.toLongLong()));
        }
        else if (typeId == QMetaType::Double || typeId == QMetaType::Float)
        {
            winSettingsObj.insert(key, val.toDouble());
        }
        else if (typeId == QMetaType::QString)
        {
            winSettingsObj.insert(key, val.toString());
        }
        else if (typeId == QMetaType::QStringList)
        {
            QJsonArray arr;
            for (const QString& s : val.toStringList())
                arr.append(s);
            winSettingsObj.insert(key, arr);
        }
        else if (typeId == QMetaType::QByteArray)
        {
            QJsonObject wrap;
            wrap.insert(QStringLiteral("__type"), QStringLiteral("base64"));
            wrap.insert(QStringLiteral("data"), QString::fromLatin1(val.toByteArray().toBase64()));
            winSettingsObj.insert(key, wrap);
        }
        else
        {
            winSettingsObj.insert(key, val.toString());
        }
    }
    root.insert(QStringLiteral("windows_settings"), winSettingsObj);

    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        summaryOrError = QObject::tr("Cannot open file for writing: %1").arg(filepath);
        return false;
    }

    QJsonDocument doc(root);
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size())
    {
        summaryOrError = QObject::tr("Failed writing backup file.");
        return false;
    }
    file.close();

    summaryOrError = QObject::tr("Exported %1 server(s) and %2 setting(s) to:\n%3")
                         .arg(serversArr.size())
                         .arg(winSettingsObj.size())
                         .arg(QDir::toNativeSeparators(filepath));
    return true;
}

bool importFromFile(const QString& filepath, QString& summaryOrError)
{
    if (!ttSettings)
    {
        summaryOrError = QObject::tr("Settings store is not initialized.");
        return false;
    }

    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
    {
        summaryOrError = QObject::tr("Cannot open backup file: %1").arg(filepath);
        return false;
    }

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    file.close();

    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
    {
        summaryOrError = QObject::tr("Invalid JSON backup file: %1").arg(parseErr.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("teamtalk-extro-backup"))
    {
        summaryOrError = QObject::tr("Unsupported backup format (expected 'teamtalk-extro-backup').");
        return false;
    }

    int restoredWinKeys = 0;
    if (root.contains(QStringLiteral("windows_settings")) &&
        root.value(QStringLiteral("windows_settings")).isObject())
    {
        const QJsonObject winObj = root.value(QStringLiteral("windows_settings")).toObject();
        for (auto it = winObj.constBegin(); it != winObj.constEnd(); ++it)
        {
            const QString key = it.key();
            // Skip serverentries/* here; servers are merged cleanly below
            if (key.startsWith(QStringLiteral("serverentries/")))
                continue;

            const QJsonValue v = it.value();
            if (v.isBool())
            {
                ttSettings->setValue(key, v.toBool());
                ++restoredWinKeys;
            }
            else if (v.isDouble())
            {
                double d = v.toDouble();
                qint64 asInt = static_cast<qint64>(d);
                if (static_cast<double>(asInt) == d)
                    ttSettings->setValue(key, asInt);
                else
                    ttSettings->setValue(key, d);
                ++restoredWinKeys;
            }
            else if (v.isString())
            {
                ttSettings->setValue(key, v.toString());
                ++restoredWinKeys;
            }
            else if (v.isArray())
            {
                QStringList list;
                for (const QJsonValue& item : v.toArray())
                    list.append(item.toString());
                ttSettings->setValue(key, list);
                ++restoredWinKeys;
            }
            else if (v.isObject())
            {
                const QJsonObject obj = v.toObject();
                if (obj.value(QStringLiteral("__type")).toString() == QStringLiteral("base64"))
                {
                    QByteArray raw = QByteArray::fromBase64(obj.value(QStringLiteral("data")).toString().toLatin1());
                    ttSettings->setValue(key, raw);
                    ++restoredWinKeys;
                }
            }
        }
    }

    // Load existing saved hosts to avoid duplicates
    QVector<HostEntry> existing;
    {
        HostEntry tmp;
        int idx = 0;
        while (getServerEntry(idx++, tmp, false))
        {
            existing.append(tmp);
            tmp = HostEntry();
        }
    }

    int importedServers = 0;
    if (root.contains(QStringLiteral("servers")) && root.value(QStringLiteral("servers")).isArray())
    {
        const QJsonArray arr = root.value(QStringLiteral("servers")).toArray();
        for (const QJsonValue& val : arr)
        {
            if (!val.isObject())
                continue;
            const QJsonObject s = val.toObject();
            HostEntry h;
            h.latesthost = false;
            h.ipaddr = s.value(QStringLiteral("host")).toString().trimmed();
            if (h.ipaddr.isEmpty())
                continue;
            h.tcpport = s.value(QStringLiteral("tcp_port")).toInt(DEFAULT_TCPPORT);
            if (h.tcpport <= 0)
                h.tcpport = DEFAULT_TCPPORT;
            h.udpport = s.value(QStringLiteral("udp_port")).toInt(DEFAULT_UDPPORT);
            if (h.udpport <= 0)
                h.udpport = DEFAULT_UDPPORT;
            h.encrypted = s.value(QStringLiteral("encrypted")).toBool(false);
            h.username = s.value(QStringLiteral("username")).toString();
            h.password = s.value(QStringLiteral("password")).toString();
            h.nickname = s.value(QStringLiteral("nickname")).toString();
            h.channel = s.value(QStringLiteral("channel")).toString();
            h.chanpasswd = s.value(QStringLiteral("channel_password")).toString();
            h.name = s.value(QStringLiteral("name")).toString().trimmed();
            if (h.name.isEmpty())
                h.name = h.generateEntryName();

            bool duplicate = false;
            for (const HostEntry& ex : existing)
            {
                if (ex.sameHostEntry(h))
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                addServerEntry(h);
                existing.append(h);
                ++importedServers;
            }
        }
    }

    // Import cross-platform Extro settings
    if (root.contains(QStringLiteral("extro")) && root.value(QStringLiteral("extro")).isObject())
    {
        const QJsonObject e = root.value(QStringLiteral("extro")).toObject();
        if (e.contains(QStringLiteral("voice_changer_mode")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_MODE, e.value(QStringLiteral("voice_changer_mode")).toInt(0));
        if (e.contains(QStringLiteral("echo_enabled")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_ECHO, e.value(QStringLiteral("echo_enabled")).toBool(true));
        if (e.contains(QStringLiteral("echo_level")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_ECHO_LEVEL, e.value(QStringLiteral("echo_level")).toInt(35));
        if (e.contains(QStringLiteral("reverb_enabled")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_REVERB, e.value(QStringLiteral("reverb_enabled")).toBool(true));
        if (e.contains(QStringLiteral("reverb_level")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_REVERB_LEVEL, e.value(QStringLiteral("reverb_level")).toInt(50));
        if (e.contains(QStringLiteral("reverb_room_size")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_ROOM_SIZE, e.value(QStringLiteral("reverb_room_size")).toInt(60));
        if (e.contains(QStringLiteral("autotune_scale")))
            ttSettings->setValue(SETTINGS_SOUND_VOICECHANGER_AUTOTUNE_SCALE, e.value(QStringLiteral("autotune_scale")).toInt(0));
        if (e.contains(QStringLiteral("karaoke_volume")))
            ttSettings->setValue(SETTINGS_SOUND_KARAOKE_VOLUME, e.value(QStringLiteral("karaoke_volume")).toInt(75));
        if (e.contains(QStringLiteral("karaoke_ducking")))
            ttSettings->setValue(SETTINGS_SOUND_KARAOKE_DUCKING, e.value(QStringLiteral("karaoke_ducking")).toBool(false));
    }

    ttSettings->sync();
    applyStoredExtroSettings();

    const QString srcPlatform = root.value(QStringLiteral("platform")).toString(QStringLiteral("unknown"));
    summaryOrError = QObject::tr("Backup imported (%1).\nNew servers added: %2\nWindows settings restored: %3")
                         .arg(srcPlatform)
                         .arg(importedServers)
                         .arg(restoredWinKeys);
    return true;
}

bool exportInteractive(QWidget* parent)
{
    QString startDir = ttSettings ? ttSettings->value(SETTINGS_LAST_DIRECTORY, QDir::homePath()).toString() : QDir::homePath();
    QString defaultName = startDir + QDir::separator() +
                          QStringLiteral("TeamTalkExtro_Backup_") +
                          QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) +
                          QStringLiteral(".json");
    QString filepath = QFileDialog::getSaveFileName(
        parent,
        QObject::tr("Export Settings & Servers Backup (JSON)"),
        defaultName,
        QObject::tr("TeamTalk Extro Backup (*.json);;All Files (*.*)"));
    if (filepath.isEmpty())
        return false;

    if (ttSettings)
        ttSettings->setValue(SETTINGS_LAST_DIRECTORY, QFileInfo(filepath).absolutePath());

    QString msg;
    if (exportToFile(filepath, msg))
    {
        QMessageBox::information(parent, QObject::tr("Export Backup"), msg);
        return true;
    }
    QMessageBox::critical(parent, QObject::tr("Export Backup"), msg);
    return false;
}

bool importInteractive(QWidget* parent)
{
    QString startDir = ttSettings ? ttSettings->value(SETTINGS_LAST_DIRECTORY, QDir::homePath()).toString() : QDir::homePath();
    QString filepath = QFileDialog::getOpenFileName(
        parent,
        QObject::tr("Import Settings & Servers Backup (JSON)"),
        startDir,
        QObject::tr("TeamTalk Extro Backup (*.json);;All Files (*.*)"));
    if (filepath.isEmpty())
        return false;

    if (ttSettings)
        ttSettings->setValue(SETTINGS_LAST_DIRECTORY, QFileInfo(filepath).absolutePath());

    QString msg;
    if (importFromFile(filepath, msg))
    {
        QMessageBox::information(parent, QObject::tr("Import Backup"), msg);
        return true;
    }
    QMessageBox::critical(parent, QObject::tr("Import Backup"), msg);
    return false;
}

} // namespace ExtroBackup
