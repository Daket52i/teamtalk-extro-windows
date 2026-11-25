/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Server Peek ("Who's Online without joining a channel").
 */

#ifndef SERVERPEEKDLG_H
#define SERVERPEEKDLG_H

#include "common.h"

#include <QDialog>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QThread>
#include <QVector>

class QLabel;
class QPushButton;
class QTreeWidget;

struct PeekChannelInfo
{
    int id = 0;
    int parentId = 0;
    QString name;
    QString topic;
};

struct PeekUserInfo
{
    int id = 0;
    int channelId = 0;
    QString nickname;
    QString username;
    QString statusMsg;
};

struct ServerPeekResult
{
    bool ok = false;
    QString errorMessage;
    QString serverName;
    int myUserId = 0;
    QMap<int, PeekChannelInfo> channels;
    QMap<int, PeekUserInfo> users;
};

Q_DECLARE_METATYPE(ServerPeekResult)

class ServerPeekWorker : public QThread
{
    Q_OBJECT
public:
    explicit ServerPeekWorker(const HostEntry& host, QObject* parent = nullptr);

signals:
    void peekFinished(const ServerPeekResult& result);

protected:
    void run() override;

private:
    HostEntry m_host;
};

class ServerPeekDlg : public QDialog
{
    Q_OBJECT
public:
    explicit ServerPeekDlg(const HostEntry& host, QWidget* parent = nullptr);
    ~ServerPeekDlg() override;

    bool shouldConnect() const { return m_shouldConnect; }

private slots:
    void startPeek();
    void onPeekFinished(const ServerPeekResult& result);
    void onConnectClicked();

private:
    void stopWorker();

    HostEntry m_host;
    ServerPeekWorker* m_worker = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTreeWidget* m_tree = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    bool m_shouldConnect = false;
};

#endif
