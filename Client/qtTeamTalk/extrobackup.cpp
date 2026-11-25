/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Unified cross-platform JSON Backup & Restore (v1 plain / v2 AES-256-GCM encrypted,
 * compatible with TeamTalk Extro for Android).
 */

#include "extrobackup.h"
#include "appinfo.h"
#include "common.h"
#include "extroshield.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QMessageBox>
#include <QVariant>

#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cwchar>
#endif

extern NonDefaultSettings* ttSettings;
extern TTInstance* ttInst;

namespace
{

struct ExtroBackupTr
{
    Q_DECLARE_TR_FUNCTIONS(ExtroBackup)
};

constexpr int kPbkdf2Iterations = 100000;
constexpr int kSaltLen = 16;
constexpr int kIvLen = 12;
constexpr int kTagLen = 16;
constexpr int kAesKeyLen = 32;

bool encryptJsonPayload(const QByteArray& plaintext,
                        const QString& password,
                        QJsonObject& outEnvelope,
                        QString& errorOut)
{
#if defined(Q_OS_WIN)
    const QByteArray passUtf8 = password.toUtf8();
    if (passUtf8.isEmpty())
    {
        errorOut = ExtroBackupTr::tr("Password cannot be empty for encrypted backup.");
        return false;
    }

    QByteArray salt(kSaltLen, '\0');
    QByteArray iv(kIvLen, '\0');
    std::vector<UCHAR> key(kAesKeyLen, 0);
    QByteArray tag(kTagLen, '\0');

    NTSTATUS st = BCryptGenRandom(nullptr,
                                  reinterpret_cast<PUCHAR>(salt.data()),
                                  static_cast<ULONG>(salt.size()),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st))
    {
        errorOut = ExtroBackupTr::tr("Windows CNG random generator failed (salt).");
        return false;
    }

    st = BCryptGenRandom(nullptr,
                         reinterpret_cast<PUCHAR>(iv.data()),
                         static_cast<ULONG>(iv.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st))
    {
        errorOut = ExtroBackupTr::tr("Windows CNG random generator failed (IV).");
        return false;
    }

    BCRYPT_ALG_HANDLE hPrf = nullptr;
    BCRYPT_ALG_HANDLE hAes = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<UCHAR> keyObj;
    bool ok = false;

    do
    {
        st = BCryptOpenAlgorithmProvider(&hPrf, BCRYPT_SHA256_ALGORITHM, nullptr,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to initialize SHA-256 HMAC provider.");
            break;
        }

        st = BCryptDeriveKeyPBKDF2(
            hPrf,
            reinterpret_cast<PUCHAR>(const_cast<char*>(passUtf8.constData())),
            static_cast<ULONG>(passUtf8.size()),
            reinterpret_cast<PUCHAR>(salt.data()),
            static_cast<ULONG>(salt.size()),
            static_cast<ULONGLONG>(kPbkdf2Iterations),
            key.data(),
            static_cast<ULONG>(key.size()),
            0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("PBKDF2 key derivation failed.");
            break;
        }

        st = BCryptOpenAlgorithmProvider(&hAes, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to initialize AES provider.");
            break;
        }

        st = BCryptSetProperty(
            hAes,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((std::wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)),
            0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to configure AES-GCM mode.");
            break;
        }

        ULONG cbKeyObj = 0;
        ULONG cbData = 0;
        st = BCryptGetProperty(hAes, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&cbKeyObj), sizeof(cbKeyObj), &cbData, 0);
        if (!BCRYPT_SUCCESS(st) || cbKeyObj == 0)
        {
            errorOut = ExtroBackupTr::tr("Failed to query AES key object length.");
            break;
        }
        keyObj.resize(cbKeyObj);

        st = BCryptGenerateSymmetricKey(hAes, &hKey, keyObj.data(), cbKeyObj,
                                        key.data(), static_cast<ULONG>(key.size()), 0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to generate AES-256 symmetric key.");
            break;
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = reinterpret_cast<PUCHAR>(iv.data());
        authInfo.cbNonce = static_cast<ULONG>(iv.size());
        authInfo.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        authInfo.cbTag = static_cast<ULONG>(tag.size());

        QByteArray ciphertext(plaintext.size(), '\0');
        ULONG cbCipher = 0;
        st = BCryptEncrypt(
            hKey,
            reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.constData())),
            static_cast<ULONG>(plaintext.size()),
            &authInfo,
            nullptr, 0,
            reinterpret_cast<PUCHAR>(ciphertext.data()),
            static_cast<ULONG>(ciphertext.size()),
            &cbCipher,
            0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("AES-256-GCM encryption failed.");
            break;
        }
        ciphertext.resize(static_cast<int>(cbCipher));

        outEnvelope = QJsonObject();
        outEnvelope.insert(QStringLiteral("format"), QStringLiteral("teamtalk-extro-backup-encrypted"));
        outEnvelope.insert(QStringLiteral("version"), 2);
        outEnvelope.insert(QStringLiteral("kdf"), QStringLiteral("PBKDF2-HMAC-SHA256"));
        outEnvelope.insert(QStringLiteral("iterations"), kPbkdf2Iterations);
        outEnvelope.insert(QStringLiteral("cipher"), QStringLiteral("AES-256-GCM"));
        outEnvelope.insert(QStringLiteral("salt_b64"), QString::fromLatin1(salt.toBase64()));
        outEnvelope.insert(QStringLiteral("iv_b64"), QString::fromLatin1(iv.toBase64()));
        outEnvelope.insert(QStringLiteral("tag_b64"), QString::fromLatin1(tag.toBase64()));
        outEnvelope.insert(QStringLiteral("ciphertext_b64"), QString::fromLatin1(ciphertext.toBase64()));
        ok = true;
    } while (false);

    SecureZeroMemory(key.data(), key.size());
    if (hKey)
        BCryptDestroyKey(hKey);
    if (hAes)
        BCryptCloseAlgorithmProvider(hAes, 0);
    if (hPrf)
        BCryptCloseAlgorithmProvider(hPrf, 0);

    return ok;
#else
    Q_UNUSED(plaintext);
    Q_UNUSED(password);
    Q_UNUSED(outEnvelope);
    errorOut = ExtroBackupTr::tr("Encrypted backups require Windows CNG.");
    return false;
#endif
}

bool decryptJsonPayload(const QJsonObject& envelope,
                        const QString& password,
                        QByteArray& outPlaintext,
                        QString& errorOut)
{
#if defined(Q_OS_WIN)
    const QByteArray passUtf8 = password.toUtf8();
    if (passUtf8.isEmpty())
    {
        errorOut = ExtroBackupTr::tr("Password is required to decrypt this backup.");
        return false;
    }

    const int iterations = envelope.value(QStringLiteral("iterations")).toInt(kPbkdf2Iterations);
    if (iterations < 1000 || iterations > 5000000)
    {
        errorOut = ExtroBackupTr::tr("Invalid KDF iteration count in encrypted backup.");
        return false;
    }

    QByteArray salt = QByteArray::fromBase64(envelope.value(QStringLiteral("salt_b64")).toString().toLatin1());
    QByteArray iv = QByteArray::fromBase64(envelope.value(QStringLiteral("iv_b64")).toString().toLatin1());
    QByteArray tag = QByteArray::fromBase64(envelope.value(QStringLiteral("tag_b64")).toString().toLatin1());
    QByteArray ciphertext = QByteArray::fromBase64(envelope.value(QStringLiteral("ciphertext_b64")).toString().toLatin1());

    if (salt.isEmpty() || iv.size() != kIvLen || tag.size() != kTagLen)
    {
        errorOut = ExtroBackupTr::tr("Encrypted backup metadata is corrupted or incomplete.");
        return false;
    }

    std::vector<UCHAR> key(kAesKeyLen, 0);
    BCRYPT_ALG_HANDLE hPrf = nullptr;
    BCRYPT_ALG_HANDLE hAes = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<UCHAR> keyObj;
    bool ok = false;

    do
    {
        NTSTATUS st = BCryptOpenAlgorithmProvider(&hPrf, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                  BCRYPT_ALG_HANDLE_HMAC_FLAG);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to initialize SHA-256 HMAC provider.");
            break;
        }

        st = BCryptDeriveKeyPBKDF2(
            hPrf,
            reinterpret_cast<PUCHAR>(const_cast<char*>(passUtf8.constData())),
            static_cast<ULONG>(passUtf8.size()),
            reinterpret_cast<PUCHAR>(salt.data()),
            static_cast<ULONG>(salt.size()),
            static_cast<ULONGLONG>(iterations),
            key.data(),
            static_cast<ULONG>(key.size()),
            0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("PBKDF2 key derivation failed.");
            break;
        }

        st = BCryptOpenAlgorithmProvider(&hAes, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to initialize AES provider.");
            break;
        }

        st = BCryptSetProperty(
            hAes,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((std::wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)),
            0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to configure AES-GCM mode.");
            break;
        }

        ULONG cbKeyObj = 0;
        ULONG cbData = 0;
        st = BCryptGetProperty(hAes, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&cbKeyObj), sizeof(cbKeyObj), &cbData, 0);
        if (!BCRYPT_SUCCESS(st) || cbKeyObj == 0)
        {
            errorOut = ExtroBackupTr::tr("Failed to query AES key object length.");
            break;
        }
        keyObj.resize(cbKeyObj);

        st = BCryptGenerateSymmetricKey(hAes, &hKey, keyObj.data(), cbKeyObj,
                                        key.data(), static_cast<ULONG>(key.size()), 0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Failed to generate AES-256 symmetric key.");
            break;
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = reinterpret_cast<PUCHAR>(iv.data());
        authInfo.cbNonce = static_cast<ULONG>(iv.size());
        authInfo.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        authInfo.cbTag = static_cast<ULONG>(tag.size());

        outPlaintext.resize(ciphertext.size());
        ULONG cbPlain = 0;
        st = BCryptDecrypt(
            hKey,
            reinterpret_cast<PUCHAR>(ciphertext.data()),
            static_cast<ULONG>(ciphertext.size()),
            &authInfo,
            nullptr, 0,
            reinterpret_cast<PUCHAR>(outPlaintext.data()),
            static_cast<ULONG>(outPlaintext.size()),
            &cbPlain,
            0);
        if (!BCRYPT_SUCCESS(st))
        {
            errorOut = ExtroBackupTr::tr("Decryption failed: incorrect password or damaged backup file.");
            break;
        }
        outPlaintext.resize(static_cast<int>(cbPlain));
        ok = true;
    } while (false);

    SecureZeroMemory(key.data(), key.size());
    if (hKey)
        BCryptDestroyKey(hKey);
    if (hAes)
        BCryptCloseAlgorithmProvider(hAes, 0);
    if (hPrf)
        BCryptCloseAlgorithmProvider(hPrf, 0);

    return ok;
#else
    Q_UNUSED(envelope);
    Q_UNUSED(password);
    Q_UNUSED(outPlaintext);
    errorOut = ExtroBackupTr::tr("Encrypted backups require Windows CNG.");
    return false;
#endif
}

} // namespace

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

bool exportToFile(const QString& filepath, QString& summaryOrError, const QString& password)
{
    if (!ttSettings)
    {
        summaryOrError = ExtroBackupTr::tr("Settings store is not initialized.");
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

    // 3. Extro Shield settings & User Notes (cross-platform)
    root.insert(QStringLiteral("shield_settings"), ExtroShield::exportShieldSettingsJson());
    root.insert(QStringLiteral("user_notes"), ExtroShield::exportUserNotesJson());

    // 4. Complete Windows QSettings snapshot
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

    QByteArray outputBytes;
    const bool encrypted = !password.isEmpty();
    if (encrypted)
    {
        const QByteArray plainJson = QJsonDocument(root).toJson(QJsonDocument::Compact);
        QJsonObject envelope;
        QString encErr;
        if (!encryptJsonPayload(plainJson, password, envelope, encErr))
        {
            summaryOrError = encErr;
            return false;
        }
        outputBytes = QJsonDocument(envelope).toJson(QJsonDocument::Indented);
    }
    else
    {
        outputBytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        summaryOrError = ExtroBackupTr::tr("Cannot open file for writing: %1").arg(filepath);
        return false;
    }

    if (file.write(outputBytes) != outputBytes.size())
    {
        summaryOrError = ExtroBackupTr::tr("Failed writing backup file.");
        return false;
    }
    file.close();

    if (encrypted)
    {
        summaryOrError = ExtroBackupTr::tr("Encrypted backup (AES-256-GCM) exported: %1 server(s) and %2 setting(s) to:\n%3")
                             .arg(serversArr.size())
                             .arg(winSettingsObj.size())
                             .arg(QDir::toNativeSeparators(filepath));
    }
    else
    {
        summaryOrError = ExtroBackupTr::tr("Exported %1 server(s) and %2 setting(s) to:\n%3")
                             .arg(serversArr.size())
                             .arg(winSettingsObj.size())
                             .arg(QDir::toNativeSeparators(filepath));
    }
    return true;
}

bool importFromFile(const QString& filepath, QString& summaryOrError, const QString& password)
{
    if (!ttSettings)
    {
        summaryOrError = ExtroBackupTr::tr("Settings store is not initialized.");
        return false;
    }

    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
    {
        summaryOrError = ExtroBackupTr::tr("Cannot open backup file: %1").arg(filepath);
        return false;
    }

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    file.close();

    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
    {
        summaryOrError = ExtroBackupTr::tr("Invalid JSON backup file: %1").arg(parseErr.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    const QString fmt = root.value(QStringLiteral("format")).toString();
    bool wasEncrypted = false;

    if (fmt == QStringLiteral("teamtalk-extro-backup-encrypted"))
    {
        QByteArray decrypted;
        QString decErr;
        if (!decryptJsonPayload(root, password, decrypted, decErr))
        {
            summaryOrError = decErr;
            return false;
        }
        QJsonParseError innerErr;
        QJsonDocument innerDoc = QJsonDocument::fromJson(decrypted, &innerErr);
        if (innerErr.error != QJsonParseError::NoError || !innerDoc.isObject())
        {
            summaryOrError = ExtroBackupTr::tr("Decrypted payload is not valid JSON: %1").arg(innerErr.errorString());
            return false;
        }
        root = innerDoc.object();
        wasEncrypted = true;
    }

    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("teamtalk-extro-backup"))
    {
        summaryOrError = ExtroBackupTr::tr("Unsupported backup format (expected 'teamtalk-extro-backup').");
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

    // Import Extro Shield settings & User Notes
    if (root.contains(QStringLiteral("shield_settings")) &&
        root.value(QStringLiteral("shield_settings")).isObject())
    {
        ExtroShield::importShieldSettingsJson(root.value(QStringLiteral("shield_settings")).toObject());
    }
    if (root.contains(QStringLiteral("user_notes")) &&
        root.value(QStringLiteral("user_notes")).isObject())
    {
        ExtroShield::importUserNotesJson(root.value(QStringLiteral("user_notes")).toObject());
    }

    ttSettings->sync();
    applyStoredExtroSettings();

    const QString srcPlatform = root.value(QStringLiteral("platform")).toString(QStringLiteral("unknown"));
    if (wasEncrypted)
    {
        summaryOrError = ExtroBackupTr::tr("Encrypted backup imported (%1).\nNew servers added: %2\nWindows settings restored: %3")
                             .arg(srcPlatform)
                             .arg(importedServers)
                             .arg(restoredWinKeys);
    }
    else
    {
        summaryOrError = ExtroBackupTr::tr("Backup imported (%1).\nNew servers added: %2\nWindows settings restored: %3")
                             .arg(srcPlatform)
                             .arg(importedServers)
                             .arg(restoredWinKeys);
    }
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
        ExtroBackupTr::tr("Export Settings & Servers Backup (JSON)"),
        defaultName,
        ExtroBackupTr::tr("TeamTalk Extro Backup (*.json);;All Files (*.*)"));
    if (filepath.isEmpty())
        return false;

    if (ttSettings)
        ttSettings->setValue(SETTINGS_LAST_DIRECTORY, QFileInfo(filepath).absolutePath());

    bool okPass = false;
    QString password = QInputDialog::getText(
        parent,
        ExtroBackupTr::tr("Backup Encryption (Optional)"),
        ExtroBackupTr::tr("Enter a password to encrypt the backup with AES-256-GCM (leave empty for standard unencrypted JSON):"),
        QLineEdit::Password,
        QString(),
        &okPass);
    if (!okPass)
        return false;

    if (!password.isEmpty())
    {
        bool okConfirm = false;
        QString confirm = QInputDialog::getText(
            parent,
            ExtroBackupTr::tr("Confirm Backup Password"),
            ExtroBackupTr::tr("Re-enter the password to confirm:"),
            QLineEdit::Password,
            QString(),
            &okConfirm);
        if (!okConfirm)
            return false;
        if (confirm != password)
        {
            QMessageBox::warning(parent, ExtroBackupTr::tr("Export Backup"),
                                 ExtroBackupTr::tr("Passwords do not match. Export cancelled."));
            return false;
        }
    }

    QString msg;
    if (exportToFile(filepath, msg, password))
    {
        QMessageBox::information(parent, ExtroBackupTr::tr("Export Backup"), msg);
        return true;
    }
    QMessageBox::critical(parent, ExtroBackupTr::tr("Export Backup"), msg);
    return false;
}

bool importInteractive(QWidget* parent)
{
    QString startDir = ttSettings ? ttSettings->value(SETTINGS_LAST_DIRECTORY, QDir::homePath()).toString() : QDir::homePath();
    QString filepath = QFileDialog::getOpenFileName(
        parent,
        ExtroBackupTr::tr("Import Settings & Servers Backup (JSON)"),
        startDir,
        ExtroBackupTr::tr("TeamTalk Extro Backup (*.json);;All Files (*.*)"));
    if (filepath.isEmpty())
        return false;

    if (ttSettings)
        ttSettings->setValue(SETTINGS_LAST_DIRECTORY, QFileInfo(filepath).absolutePath());

    // Auto-detect encrypted backup v2
    QString password;
    {
        QFile f(filepath);
        if (f.open(QIODevice::ReadOnly))
        {
            QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            f.close();
            if (doc.isObject() &&
                doc.object().value(QStringLiteral("format")).toString() ==
                    QStringLiteral("teamtalk-extro-backup-encrypted"))
            {
                bool okPass = false;
                password = QInputDialog::getText(
                    parent,
                    ExtroBackupTr::tr("Encrypted Backup Password"),
                    ExtroBackupTr::tr("This backup is encrypted (AES-256-GCM). Enter password:"),
                    QLineEdit::Password,
                    QString(),
                    &okPass);
                if (!okPass)
                    return false;
            }
        }
    }

    QString msg;
    if (importFromFile(filepath, msg, password))
    {
        QMessageBox::information(parent, ExtroBackupTr::tr("Import Backup"), msg);
        return true;
    }
    QMessageBox::critical(parent, ExtroBackupTr::tr("Import Backup"), msg);
    return false;
}

} // namespace ExtroBackup
