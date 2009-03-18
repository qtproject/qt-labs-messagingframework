/****************************************************************************
**
** This file is part of the $PACKAGE_NAME$.
**
** Copyright (C) $THISYEAR$ $COMPANY_NAME$.
**
** $QT_EXTENDED_DUAL_LICENSE$
**
****************************************************************************/

#ifndef IMAPCLIENT_H
#define IMAPCLIENT_H

#include "imapprotocol.h"
#include <qstring.h>
#include <qstringlist.h>
#include <qobject.h>
#include <qlist.h>
#include <qtimer.h>
#include <qmailaccountconfiguration.h>
#include <qmailfolder.h>
#include <qmailmessage.h>
#include <qmailmessageclassifier.h>
#include <qmailmessageserver.h>


class ImapStrategy;
class ImapStrategyContext;

class ImapClient : public QObject
{
    Q_OBJECT

public:
    ImapClient(QObject* parent);
    ~ImapClient();

    void setAccount(const QMailAccountId& accountId);
    QMailAccountId account() const;

    void newConnection();
    void cancelTransfer();
    void closeConnection();

    ImapStrategyContext *strategyContext();

    ImapStrategy *strategy() const;
    void setStrategy(ImapStrategy *strategy);

    QMailFolderId mailboxId(const QString &path) const;
    QMailFolderIdList mailboxIds() const;

    QStringList serverUids(const QMailFolderId &folderId) const;
    QStringList serverUids(const QMailFolderId &folderId, quint64 messageStatusFilter, bool set = true) const;

    QStringList deletedMessages(const QMailFolderId &folderId) const;

signals:
    void errorOccurred(int, const QString &);
    void errorOccurred(QMailServiceAction::Status::ErrorCode, const QString &);
    void updateStatus(const QString &);

    void progressChanged(uint, uint);
    void retrievalCompleted();

    void messageActionCompleted(const QString &uid);

    void allMessagesReceived();
    void idleChangeNotification();

public slots:
    void transportError(int, const QString &msg);
    void transportError(QMailServiceAction::Status::ErrorCode, const QString &msg);

    void idleTransportError();
    void idleErrorRecovery();

    void mailboxListed(QString &, QString &, QString &);
    void messageFetched(QMailMessage& mail);
    void dataFetched(const QString &uid, const QString &section, const QString &fileName, int size, bool partial);
    void nonexistentUid(const QString &uid);
    void messageStored(const QString &);
    void messageCopied(const QString &, const QString &);
    void downloadSize(const QString &uid, int);

protected slots:
    void connectionInactive();
    void commandCompleted(const ImapCommand, const OperationStatus);
    void checkCommandResponse(const ImapCommand, const OperationStatus);
    void commandTransition(const ImapCommand, const OperationStatus);
    void transportStatus(const QString& status);

    void idleContinuation(ImapCommand, const QString &);
    void idleCommandTransition(ImapCommand, OperationStatus);
    void idleTimeOut();

private:
    friend class ImapStrategyContextBase;

    void deactivateConnection();
    void retrieveOperationCompleted();

    void operationFailed(int code, const QString &text);
    void operationFailed(QMailServiceAction::Status::ErrorCode code, const QString &text);

    void updateFolderCountStatus(QMailFolder *folder);

    QStringList serverUids(QMailMessageKey key) const;
    QMailMessageKey messagesKey(const QMailFolderId &folderId) const;
    QMailMessageKey trashKey(const QMailFolderId &folderId) const;

private:
    QMailAccountConfiguration _config;

    ImapProtocol _protocol;
    QTimer _inactiveTimer;

    ImapProtocol _idleProtocol;
    QMailFolder _idleFolder;
    QTimer _idleTimer; // Send a DONE command every 29 minutes
    QTimer _idleRecoveryTimer; // Check command hasn't hung
    int _idleRetryDelay; // Try to restablish IDLE state
    enum IdleRetryDelay { InitialIdleRetryDelay = 30 }; //seconds
    bool _waitingForIdle;

    QMailMessageClassifier _classifier;
    ImapStrategyContext *_strategyContext;
    QMap<QString, uint> partialLength;
};

#endif
