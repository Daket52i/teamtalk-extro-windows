/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * Server Peek ("Who's Online without joining a channel").
 */

#include "serverpeekdlg.h"
#include "appinfo.h"

#include <QDir>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTreeWidget>
#include <QVBoxLayout>

extern TTInstance* ttInst;

namespace
{

bool setupPeekEncryption(TTInstance* inst, const HostEntry& host)
{
    if (!host.encrypted)
        return true;

    QTemporaryFile cafile, certfile, keyfile;
    if (!cafile.open() || !certfile.open() || !keyfile.open())
        return false;

    QString cafilename, certfilename, keyfilename;
    if (!host.encryption.cacertdata.isEmpty())
    {
        cafile.write(host.encryption.cacertdata.toUtf8());
        cafilename = QDir::toNativeSeparators(cafile.fileName());
    }
    if (!host.encryption.certdata.isEmpty())
    {
        certfile.write(host.encryption.certdata.toUtf8());
        certfilename = QDir::toNativeSeparators(certfile.fileName());
    }
    if (!host.encryption.privkeydata.isEmpty())
    {
        keyfile.write(host.encryption.privkeydata.toUtf8());
        keyfilename = QDir::toNativeSeparators(keyfile.fileName());
    }
    cafile.close();
    certfile.close();
    keyfile.close();

    EncryptionContext ctx = {};
    ctx.bVerifyPeer = host.encryption.verifypeer ? TRUE : FALSE;
    if (!ctx.bVerifyPeer)
        ctx.nVerifyDepth = -1;
    COPY_TTSTR(ctx.szCAFile, cafilename);
    COPY_TTSTR(ctx.szCertificateFile, certfilename);
    COPY_TTSTR(ctx.szPrivateKeyFile, keyfilename);

    return TT_SetEncryptionContext(inst, &ctx) != FALSE;
}

void mergeNativeSnapshots(TTInstance* inst, ServerPeekResult& res)
{
    if (!inst)
        return;

    ServerProperties sp = {};
    if (TT_GetServerProperties(inst, &sp))
    {
        const QString srvName = _Q(sp.szServerName);
        if (!srvName.isEmpty())
            res.serverName = srvName;
    }

    INT32 chanCount = 0;
    if (TT_GetServerChannels(inst, nullptr, &chanCount) && chanCount > 0)
    {
        QVector<Channel> chans(chanCount);
        if (TT_GetServerChannels(inst, chans.data(), &chanCount))
        {
            for (INT32 i = 0; i < chanCount; ++i)
            {
                const Channel& c = chans[i];
                PeekChannelInfo ch;
                ch.id = c.nChannelID;
                ch.parentId = c.nParentID;
                ch.name = _Q(c.szName);
                ch.topic = _Q(c.szTopic);
                res.channels.insert(ch.id, ch);
            }
        }
    }

    INT32 userCount = 0;
    if (TT_GetServerUsers(inst, nullptr, &userCount) && userCount > 0)
    {
        QVector<User> users(userCount);
        if (TT_GetServerUsers(inst, users.data(), &userCount))
        {
            for (INT32 i = 0; i < userCount; ++i)
            {
                const User& u = users[i];
                PeekUserInfo info;
                info.id = u.nUserID;
                info.channelId = u.nChannelID;
                info.nickname = _Q(u.szNickname);
                info.username = _Q(u.szUsername);
                info.statusMsg = _Q(u.szStatusMsg);
                res.users.insert(info.id, info);
            }
        }
    }
}

} // namespace

ServerPeekWorker::ServerPeekWorker(const HostEntry& host, QObject* parent)
    : QThread(parent)
    , m_host(host)
{
}

void ServerPeekWorker::run()
{
    ServerPeekResult res;
    res.serverName = m_host.name.isEmpty() ? m_host.generateEntryName() : m_host.name;

    QString stage = ServerPeekDlg::tr("initializing temporary client");
    TTInstance* peekInst = TT_InitTeamTalkPoll();
    if (!peekInst)
    {
        res.errorMessage = ServerPeekDlg::tr("[%1] Failed to initialize temporary TeamTalk instance.").arg(stage);
        emit peekFinished(res);
        return;
    }

    if (m_host.encrypted)
    {
        stage = ServerPeekDlg::tr("configuring TLS encryption");
        if (!setupPeekEncryption(peekInst, m_host))
        {
            TT_CloseTeamTalk(peekInst);
            res.errorMessage = ServerPeekDlg::tr("[%1] Failed to initialize TLS encryption context.").arg(stage);
            emit peekFinished(res);
            return;
        }
    }

    const int tcpPort = m_host.tcpport > 0 ? m_host.tcpport : DEFAULT_TCPPORT;
    const int udpPort = m_host.udpport > 0 ? m_host.udpport : DEFAULT_UDPPORT;
    const QString hostAddr = m_host.ipaddr.trimmed();

    stage = ServerPeekDlg::tr("connecting to %1:%2").arg(hostAddr).arg(tcpPort);
    if (!TT_Connect(peekInst, _W(hostAddr), tcpPort, udpPort, 0, 0,
                    m_host.encrypted ? TRUE : FALSE))
    {
        TT_CloseTeamTalk(peekInst);
        res.errorMessage = ServerPeekDlg::tr("[%1] Failed to start connection.").arg(stage);
        emit peekFinished(res);
        return;
    }

    QElapsedTimer timer;
    timer.start();
    bool loggedIn = false;
    bool loginCmdSucceeded = false;
    int loginCmdId = -1;
    qint64 loginEventAtMs = 0;
    qint64 loginCmdDoneAtMs = 0;

    constexpr qint64 kMaxTotalMs = 12000;
    constexpr qint64 kSettleGraceMs = 250;
    constexpr qint64 kFallbackAfterLoggedInMs = 1500;

    while (!isInterruptionRequested() && timer.elapsed() < kMaxTotalMs)
    {
        if (loginCmdSucceeded && (timer.elapsed() - loginCmdDoneAtMs) >= kSettleGraceMs)
            break;
        if (loggedIn && !loginCmdSucceeded && (timer.elapsed() - loginEventAtMs) >= kFallbackAfterLoggedInMs)
            break;

        TTMessage msg = {};
        INT32 waitMs = 50;
        if (!TT_GetMessage(peekInst, &msg, &waitMs))
            continue;

        switch (msg.nClientEvent)
        {
        case CLIENTEVENT_CON_SUCCESS:
        {
            stage = ServerPeekDlg::tr("logging in");
            const QString peekNick = QStringLiteral("Extro-Peek");
            const QString user = m_host.username;
            const QString pass = m_host.password;
            const QString clientName = QStringLiteral(APPNAME_SHORT);
            loginCmdId = TT_DoLoginEx(peekInst, _W(peekNick), _W(user), _W(pass), _W(clientName));
            if (loginCmdId <= 0)
            {
                res.errorMessage = ServerPeekDlg::tr("[%1] TT_DoLoginEx failed (code %2).").arg(stage).arg(loginCmdId);
                goto done;
            }
            break;
        }
        case CLIENTEVENT_CON_CRYPT_ERROR:
        {
            stage = ServerPeekDlg::tr("TLS handshake");
            const QString errStr = _Q(msg.clienterrormsg.szErrorMsg);
            res.errorMessage = ServerPeekDlg::tr("[%1] TLS error 0x%2: %3")
                                   .arg(stage)
                                   .arg(msg.nSource, 0, 16)
                                   .arg(errStr.isEmpty() ? ServerPeekDlg::tr("Unknown TLS error") : errStr);
            goto done;
        }
        case CLIENTEVENT_CON_FAILED:
            res.errorMessage = ServerPeekDlg::tr("[%1] Connection failed.").arg(stage);
            goto done;
        case CLIENTEVENT_CON_LOST:
            if (!res.ok)
                res.errorMessage = ServerPeekDlg::tr("[%1] Connection lost before data was received.").arg(stage);
            goto done;
        case CLIENTEVENT_CMD_ERROR:
        {
            if (!res.ok || (loginCmdId > 0 && msg.nSource == loginCmdId))
            {
                QString errStr = _Q(msg.clienterrormsg.szErrorMsg);
                if (errStr.isEmpty())
                    errStr = ServerPeekDlg::tr("Server error");
                res.errorMessage = ServerPeekDlg::tr("[%1] %2 (error #%3)")
                                       .arg(stage, errStr)
                                       .arg(msg.clienterrormsg.nErrorNo);
                goto done;
            }
            break;
        }
        case CLIENTEVENT_CMD_SUCCESS:
            if (loginCmdId > 0 && msg.nSource == loginCmdId)
            {
                loginCmdSucceeded = true;
                res.ok = true;
                loginCmdDoneAtMs = timer.elapsed();
                stage = ServerPeekDlg::tr("collecting channels and users");
            }
            break;
        case CLIENTEVENT_CMD_PROCESSING:
            if (loginCmdId > 0 && msg.nSource == loginCmdId && !msg.bActive && loggedIn && !loginCmdSucceeded)
            {
                loginCmdSucceeded = true;
                res.ok = true;
                loginCmdDoneAtMs = timer.elapsed();
            }
            break;
        case CLIENTEVENT_CMD_SERVER_UPDATE:
        {
            const QString srvName = _Q(msg.serverproperties.szServerName);
            if (!srvName.isEmpty())
                res.serverName = srvName;
            break;
        }
        case CLIENTEVENT_CMD_CHANNEL_NEW:
        case CLIENTEVENT_CMD_CHANNEL_UPDATE:
        {
            PeekChannelInfo ch;
            ch.id = msg.channel.nChannelID;
            ch.parentId = msg.channel.nParentID;
            ch.name = _Q(msg.channel.szName);
            ch.topic = _Q(msg.channel.szTopic);
            res.channels.insert(ch.id, ch);
            break;
        }
        case CLIENTEVENT_CMD_MYSELF_LOGGEDIN:
        {
            res.myUserId = msg.user.nUserID;
            if (res.myUserId <= 0)
                res.myUserId = TT_GetMyUserID(peekInst);
            loggedIn = true;
            res.ok = true;
            loginEventAtMs = timer.elapsed();
            stage = ServerPeekDlg::tr("collecting channels and users");
            break;
        }
        case CLIENTEVENT_CMD_USER_LOGGEDIN:
        case CLIENTEVENT_CMD_USER_JOINED:
        case CLIENTEVENT_CMD_USER_UPDATE:
        {
            PeekUserInfo u;
            u.id = msg.user.nUserID;
            u.channelId = msg.user.nChannelID;
            u.nickname = _Q(msg.user.szNickname);
            u.username = _Q(msg.user.szUsername);
            u.statusMsg = _Q(msg.user.szStatusMsg);
            res.users.insert(u.id, u);
            break;
        }
        case CLIENTEVENT_CMD_USER_LOGGEDOUT:
            res.users.remove(msg.user.nUserID);
            break;
        default:
            break;
        }
    }

done:
    if (res.ok)
    {
        const int liveMyId = TT_GetMyUserID(peekInst);
        if (liveMyId > 0)
            res.myUserId = liveMyId;

        // Merge native TeamTalk snapshots so no channels/users are ever missed
        mergeNativeSnapshots(peekInst, res);

        if (res.myUserId > 0)
            res.users.remove(res.myUserId);
    }

    if (!res.ok && res.errorMessage.isEmpty())
    {
        res.errorMessage = ServerPeekDlg::tr("[%1] Timed out waiting for server response (12 s).").arg(stage);
    }

    TT_Disconnect(peekInst);
    TT_CloseTeamTalk(peekInst);

    emit peekFinished(res);
}

ServerPeekDlg::ServerPeekDlg(const HostEntry& host, QWidget* parent)
    : QDialog(parent, QT_DEFAULT_DIALOG_HINTS)
    , m_host(host)
{
    qRegisterMetaType<ServerPeekResult>("ServerPeekResult");

    setWindowIcon(QIcon(APPICON));
    setWindowTitle(tr("Who's Online (Peek) - %1")
                       .arg(host.name.isEmpty() ? host.generateEntryName() : host.name));
    setAccessibleDescription(tr("Preview channels and online users without joining a channel"));
    resize(560, 440);

    auto* layout = new QVBoxLayout(this);

    m_statusLabel = new QLabel(tr("Connecting to %1:%2...")
                                   .arg(host.ipaddr)
                                   .arg(host.tcpport > 0 ? host.tcpport : DEFAULT_TCPPORT),
                               this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(m_statusLabel);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Channel / User"), tr("Details")});
    m_tree->setAccessibleName(tr("Online Channels and Users"));
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree);

    auto* btnRow = new QHBoxLayout();
    m_refreshBtn = new QPushButton(tr("&Refresh"), this);
    m_connectBtn = new QPushButton(tr("&Connect"), this);
    m_closeBtn = new QPushButton(tr("C&lose"), this);
    btnRow->addWidget(m_refreshBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_connectBtn);
    btnRow->addWidget(m_closeBtn);
    layout->addLayout(btnRow);

    connect(m_refreshBtn, &QPushButton::clicked, this, &ServerPeekDlg::startPeek);
    connect(m_connectBtn, &QPushButton::clicked, this, &ServerPeekDlg::onConnectClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    startPeek();
}

ServerPeekDlg::~ServerPeekDlg()
{
    stopWorker();
}

void ServerPeekDlg::stopWorker()
{
    if (m_worker)
    {
        m_worker->requestInterruption();
        m_worker->wait(2000);
        delete m_worker;
        m_worker = nullptr;
    }
}

bool ServerPeekDlg::tryPeekFromLiveConnection(ServerPeekResult& outResult) const
{
    if (!ttInst || (TT_GetFlags(ttInst) & CLIENT_AUTHORIZED) == 0)
        return false;

    const HostEntry cur = getCurrentConnectedHost();
    const QString curAddr = cur.ipaddr.trimmed();
    const QString tgtAddr = m_host.ipaddr.trimmed();
    if (curAddr.isEmpty() || tgtAddr.isEmpty())
        return false;

    const int curPort = cur.tcpport > 0 ? cur.tcpport : DEFAULT_TCPPORT;
    const int tgtPort = m_host.tcpport > 0 ? m_host.tcpport : DEFAULT_TCPPORT;
    if (curAddr.compare(tgtAddr, Qt::CaseInsensitive) != 0 || curPort != tgtPort)
        return false;

    outResult = ServerPeekResult();
    outResult.serverName = m_host.name.isEmpty() ? m_host.generateEntryName() : m_host.name;
    outResult.myUserId = TT_GetMyUserID(ttInst);
    mergeNativeSnapshots(ttInst, outResult);
    outResult.ok = true;
    outResult.fromLiveConnection = true;
    return true;
}

void ServerPeekDlg::startPeek()
{
    stopWorker();

    m_tree->clear();

    // 0ms fast path if main client is already connected & authorized to this server
    ServerPeekResult liveRes;
    if (tryPeekFromLiveConnection(liveRes))
    {
        onPeekFinished(liveRes);
        return;
    }

    m_refreshBtn->setEnabled(false);
    m_statusLabel->setText(tr("Peeking %1:%2...")
                               .arg(m_host.ipaddr)
                               .arg(m_host.tcpport > 0 ? m_host.tcpport : DEFAULT_TCPPORT));

    m_worker = new ServerPeekWorker(m_host, this);
    connect(m_worker, &ServerPeekWorker::peekFinished,
            this, &ServerPeekDlg::onPeekFinished, Qt::QueuedConnection);
    m_worker->start();
}

void ServerPeekDlg::onPeekFinished(const ServerPeekResult& result)
{
    m_refreshBtn->setEnabled(true);
    m_tree->clear();

    if (!result.ok)
    {
        m_statusLabel->setText(tr("Peek failed: %1").arg(result.errorMessage));
        return;
    }

    const int userCount = result.users.size();
    const int chanCount = result.channels.size();
    if (result.fromLiveConnection)
    {
        m_statusLabel->setText(
            tr("Server: %1 (active connection) - %2 user(s) online across %3 channel(s)")
                .arg(result.serverName)
                .arg(userCount)
                .arg(chanCount));
    }
    else
    {
        m_statusLabel->setText(
            tr("Server: %1 - %2 user(s) online across %3 channel(s)")
                .arg(result.serverName)
                .arg(userCount)
                .arg(chanCount));
    }

    // Group users by channel ID
    QMap<int, QVector<PeekUserInfo>> usersByChan;
    for (auto it = result.users.constBegin(); it != result.users.constEnd(); ++it)
        usersByChan[it.value().channelId].append(it.value());

    // Build channel items
    QMap<int, QTreeWidgetItem*> chanItems;
    for (auto it = result.channels.constBegin(); it != result.channels.constEnd(); ++it)
    {
        const PeekChannelInfo& ch = it.value();
        const int uCnt = usersByChan.value(ch.id).size();
        const QString chTitle = ch.name.isEmpty() ? tr("/") : ch.name;
        auto* item = new QTreeWidgetItem();
        item->setText(0, QString("%1 (%2)").arg(chTitle).arg(uCnt));
        item->setText(1, ch.topic);
        item->setData(0, Qt::AccessibleTextRole,
                      tr("Channel %1, %2 user(s)%3")
                          .arg(chTitle)
                          .arg(uCnt)
                          .arg(ch.topic.isEmpty() ? QString() : tr(", Topic: %1").arg(ch.topic)));
        chanItems.insert(ch.id, item);
    }

    // Attach channels in hierarchy
    for (auto it = result.channels.constBegin(); it != result.channels.constEnd(); ++it)
    {
        const PeekChannelInfo& ch = it.value();
        QTreeWidgetItem* item = chanItems.value(ch.id);
        QTreeWidgetItem* parentItem = (ch.parentId > 0 && ch.parentId != ch.id)
                                          ? chanItems.value(ch.parentId, nullptr)
                                          : nullptr;
        if (parentItem)
            parentItem->addChild(item);
        else
            m_tree->addTopLevelItem(item);
    }

    // Add users under their channels (or Lobby if channelId == 0 / unknown)
    QTreeWidgetItem* lobbyItem = nullptr;
    for (auto it = usersByChan.constBegin(); it != usersByChan.constEnd(); ++it)
    {
        QTreeWidgetItem* parentChan = chanItems.value(it.key(), nullptr);
        if (!parentChan)
        {
            if (!lobbyItem)
            {
                lobbyItem = new QTreeWidgetItem();
                lobbyItem->setText(0, tr("Lobby / Not in channel (%1)").arg(it.value().size()));
                lobbyItem->setData(0, Qt::AccessibleTextRole,
                                   tr("Lobby, %1 user(s)").arg(it.value().size()));
                m_tree->addTopLevelItem(lobbyItem);
            }
            parentChan = lobbyItem;
        }

        for (const PeekUserInfo& u : it.value())
        {
            auto* uItem = new QTreeWidgetItem(parentChan);
            const QString display = u.nickname.isEmpty() ? u.username : u.nickname;
            uItem->setText(0, display);
            QString details;
            if (!u.username.isEmpty() && u.username != display)
                details = QString("@%1").arg(u.username);
            if (!u.statusMsg.isEmpty())
            {
                if (!details.isEmpty())
                    details += QStringLiteral(" - ");
                details += u.statusMsg;
            }
            uItem->setText(1, details);
            uItem->setData(0, Qt::AccessibleTextRole,
                           tr("User %1%2")
                               .arg(display)
                               .arg(details.isEmpty() ? QString() : QStringLiteral(", ") + details));
        }
    }

    m_tree->expandAll();
    if (m_tree->topLevelItemCount() > 0)
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    m_tree->setFocus();
}

void ServerPeekDlg::onConnectClicked()
{
    m_shouldConnect = true;
    accept();
}
