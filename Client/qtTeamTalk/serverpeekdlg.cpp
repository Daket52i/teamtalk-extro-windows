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

    TTInstance* peekInst = TT_InitTeamTalkPoll();
    if (!peekInst)
    {
        res.errorMessage = tr("Failed to initialize temporary TeamTalk instance.");
        emit peekFinished(res);
        return;
    }

    if (m_host.encrypted)
        setupPeekEncryption(peekInst, m_host);

    const int tcpPort = m_host.tcpport > 0 ? m_host.tcpport : DEFAULT_TCPPORT;
    const int udpPort = m_host.udpport > 0 ? m_host.udpport : DEFAULT_UDPPORT;
    const QString hostAddr = m_host.ipaddr.trimmed();

    if (!TT_Connect(peekInst, _W(hostAddr), tcpPort, udpPort, 0, 0,
                    m_host.encrypted ? TRUE : FALSE))
    {
        TT_CloseTeamTalk(peekInst);
        res.errorMessage = tr("Failed to connect to %1:%2.").arg(hostAddr).arg(tcpPort);
        emit peekFinished(res);
        return;
    }

    QElapsedTimer timer;
    timer.start();
    bool loggedIn = false;
    qint64 loggedInAtMs = 0;
    const qint64 kMaxTotalMs = 4500;
    const qint64 kAfterLoginCollectMs = 650;

    while (!isInterruptionRequested() && timer.elapsed() < kMaxTotalMs)
    {
        if (loggedIn && (timer.elapsed() - loggedInAtMs) >= kAfterLoginCollectMs)
            break;

        TTMessage msg = {};
        INT32 waitMs = 50;
        if (!TT_GetMessage(peekInst, &msg, &waitMs))
            continue;

        switch (msg.nClientEvent)
        {
        case CLIENTEVENT_CON_SUCCESS:
        {
            const QString peekNick = QStringLiteral("Extro-Peek");
            const QString user = m_host.username;
            const QString pass = m_host.password;
            TT_DoLogin(peekInst, _W(peekNick), _W(user), _W(pass));
            break;
        }
        case CLIENTEVENT_CON_FAILED:
            res.errorMessage = tr("Connection failed to %1:%2.").arg(hostAddr).arg(tcpPort);
            goto done;
        case CLIENTEVENT_CON_LOST:
            if (!loggedIn)
                res.errorMessage = tr("Connection lost before login completed.");
            goto done;
        case CLIENTEVENT_CMD_ERROR:
            if (!loggedIn)
            {
                QString errStr = _Q(msg.clienterrormsg.szErrorMsg);
                if (errStr.isEmpty())
                    errStr = tr("Server returned error #%1.").arg(msg.clienterrormsg.nErrorNo);
                res.errorMessage = errStr;
                goto done;
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
            res.myUserId = msg.user.nUserID;
            loggedIn = true;
            res.ok = true;
            loggedInAtMs = timer.elapsed();
            break;
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
    if (res.myUserId > 0)
        res.users.remove(res.myUserId);

    if (!res.ok && res.errorMessage.isEmpty())
        res.errorMessage = tr("Timed out waiting for server response.");

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

void ServerPeekDlg::startPeek()
{
    stopWorker();

    m_tree->clear();
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
    m_statusLabel->setText(
        tr("Server: %1 - %2 user(s) online across %3 channel(s)")
            .arg(result.serverName)
            .arg(userCount)
            .arg(chanCount));

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
