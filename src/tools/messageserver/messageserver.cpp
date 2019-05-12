/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Messaging Framework.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "messageserver.h"
#include "servicehandler.h"
#include "mailmessageclient.h"
#include <qmailfolder.h>
#include <qmailmessage.h>
#include <qmailstore.h>
#include <QDataStream>
#include <QTimer>
#include <QCoreApplication>
#include <qmaillog.h>
#include <qmailipc.h>
#include <newcountnotifier.h>
#include <private/qcopserver_p.h>
#include <qmailmessageserverplugin.h>

extern "C" {
#ifndef Q_OS_WIN
#include <sys/socket.h>
#endif
#include <signal.h>
}

#if defined(Q_OS_UNIX)
#include <unistd.h>
int MessageServer::sighupFd[2];
int MessageServer::sigtermFd[2];
int MessageServer::sigintFd[2];
#endif

MessageServer::MessageServer(QObject *parent)
    : QObject(parent),
      handler(Q_NULLPTR),
      client(new MailMessageClient(this)),
      messageCountUpdate("QPE/Messages/MessageCountUpdated"),
      newMessageTotal(0),
      completionAttempted(false)
{
    qMailLog(Messaging) << "MessageServer ctor begin";
    new QCopServer(this);

#if defined(Q_OS_UNIX)
    // Unix signal handlers. We use the trick described here: http://doc.qt.io/qt-5/unix-signals.html
    // Looks shocking but the trick has certain reasons stated in Steven's book: http://cr.yp.to/docs/selfpipe.html
    // Use a socket and notifier because signal handlers can't call Qt code

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sighupFd))
        qFatal("Couldn't create HUP socketpair");
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigtermFd))
        qFatal("Couldn't create TERM socketpair");
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigintFd))
        qFatal("Couldn't create TERM socketpair");

    snHup = new QSocketNotifier(sighupFd[1], QSocketNotifier::Read, this);
    connect(snHup, SIGNAL(activated(int)), this, SLOT(handleSigHup()));
    snTerm = new QSocketNotifier(sigtermFd[1], QSocketNotifier::Read, this);
    connect(snTerm, SIGNAL(activated(int)), this, SLOT(handleSigTerm()));
    snInt = new QSocketNotifier(sigintFd[1], QSocketNotifier::Read, this);
    connect(snInt, SIGNAL(activated(int)), this, SLOT(handleSigInt()));

    struct sigaction action;
    action.sa_handler = MessageServer::hupSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_flags |= SA_RESTART;

    if (sigaction(SIGHUP, &action, Q_NULLPTR) > 0)
        qFatal("Couldn't register HUP handler");

    action.sa_handler = MessageServer::termSignalHandler;
    if (sigaction(SIGTERM, &action, Q_NULLPTR) > 0)
        qFatal("Couldn't register TERM handler");

    action.sa_handler = MessageServer::intSignalHandler;
    if (sigaction(SIGINT, &action, Q_NULLPTR) > 0)
        qFatal("Couldn't register INT handler");

#endif // defined(Q_OS_UNIX)

    QMailMessageCountMap::iterator it = messageCounts.begin(), end = messageCounts.end();
    for ( ; it != end; ++it)
        it.value() = 0;

    QMailStore *store = QMailStore::instance();
    if (store->initializationState() != QMailStore::Initialized) {
        qFatal("Messaging DB Invalid: Messaging cannot operate due to database incompatibilty!");
        // Do not close, however, or QPE will start another instance.
    } else {
        handler = new ServiceHandler(this);

        connect(store, SIGNAL(messagesAdded(QMailMessageIdList)),
                this, SLOT(messagesAdded(QMailMessageIdList)));
        connect(store, SIGNAL(messagesUpdated(QMailMessageIdList)),
                this, SLOT(messagesUpdated(QMailMessageIdList)));
        connect(store, SIGNAL(messagesRemoved(QMailMessageIdList)),
                this, SLOT(messagesRemoved(QMailMessageIdList)));

        // Propagate email handler signals to the client
        connect(handler, SIGNAL(actionStarted(QMailActionData)),
                client, SIGNAL(actionStarted(QMailActionData)));
        connect(handler, SIGNAL(activityChanged(quint64, QMailServiceAction::Activity)),
                client, SIGNAL(activityChanged(quint64, QMailServiceAction::Activity)));
        connect(handler, SIGNAL(connectivityChanged(quint64, QMailServiceAction::Connectivity)),
                client, SIGNAL(connectivityChanged(quint64, QMailServiceAction::Connectivity)));
        connect(handler, SIGNAL(statusChanged(quint64, const QMailServiceAction::Status)),
                client, SIGNAL(statusChanged(quint64, const QMailServiceAction::Status)));
        connect(handler, SIGNAL(progressChanged(quint64, uint, uint)),
                client, SIGNAL(progressChanged(quint64, uint, uint)));
        connect(handler, SIGNAL(messagesDeleted(quint64, QMailMessageIdList)), 
                client, SIGNAL(messagesDeleted(quint64, QMailMessageIdList)));
        connect(handler, SIGNAL(messagesCopied(quint64, QMailMessageIdList)), 
                client, SIGNAL(messagesCopied(quint64, QMailMessageIdList)));
        connect(handler, SIGNAL(messagesMoved(quint64, QMailMessageIdList)), 
                client, SIGNAL(messagesMoved(quint64, QMailMessageIdList)));
        connect(handler, SIGNAL(messagesFlagged(quint64, QMailMessageIdList)), 
                client, SIGNAL(messagesFlagged(quint64, QMailMessageIdList)));
        connect(handler, SIGNAL(folderCreated(quint64, QMailFolderId)),
                client, SIGNAL(folderCreated(quint64, QMailFolderId)));
        connect(handler, SIGNAL(folderRenamed(quint64, QMailFolderId)),
                client, SIGNAL(folderRenamed(quint64, QMailFolderId)));
        connect(handler, SIGNAL(folderDeleted(quint64, QMailFolderId)),
                client, SIGNAL(folderDeleted(quint64, QMailFolderId)));
        connect(handler, SIGNAL(folderMoved(quint64, QMailFolderId)),
                client, SIGNAL(folderMoved(quint64, QMailFolderId)));
        connect(handler, SIGNAL(storageActionCompleted(quint64)),
                client, SIGNAL(storageActionCompleted(quint64)));
        connect(handler, SIGNAL(matchingMessageIds(quint64, QMailMessageIdList)), 
                client, SIGNAL(matchingMessageIds(quint64, QMailMessageIdList)));
        connect(handler, SIGNAL(remainingMessagesCount(quint64, uint)), 
                client, SIGNAL(remainingMessagesCount(quint64, uint)));
        connect(handler, SIGNAL(messagesCount(quint64, uint)), 
                client, SIGNAL(messagesCount(quint64, uint)));
        connect(handler, SIGNAL(searchCompleted(quint64)),
                client, SIGNAL(searchCompleted(quint64)));
        connect(handler, SIGNAL(actionsListed(QMailActionDataList)),
                client, SIGNAL(actionsListed(QMailActionDataList)));
        connect(handler, SIGNAL(protocolResponse(quint64, QString, QVariant)), 
                client, SIGNAL(protocolResponse(quint64, QString, QVariant)));
        connect(handler, SIGNAL(protocolRequestCompleted(quint64)),
                client, SIGNAL(protocolRequestCompleted(quint64)));
        connect(handler, SIGNAL(messagesTransmitted(quint64, QMailMessageIdList)), 
                client, SIGNAL(messagesTransmitted(quint64, QMailMessageIdList)));
        connect(handler, SIGNAL(messagesFailedTransmission(quint64, QMailMessageIdList, QMailServiceAction::Status::ErrorCode)), 
                client, SIGNAL(messagesFailedTransmission(quint64, QMailMessageIdList, QMailServiceAction::Status::ErrorCode)));

        connect(handler, SIGNAL(transmissionCompleted(quint64)), 
                this, SLOT(transmissionCompleted(quint64)));
        connect(handler, SIGNAL(retrievalCompleted(quint64)),
                this, SLOT(retrievalCompleted(quint64)));

        // The email handler should handle the email client signals
        connect(client, SIGNAL(transmitMessages(quint64, QMailAccountId)),
                handler, SLOT(transmitMessages(quint64, QMailAccountId)));
        connect(client, SIGNAL(transmitMessage(quint64, QMailMessageId)),
                handler, SLOT(transmitMessage(quint64, QMailMessageId)));
        connect(client, SIGNAL(retrieveFolderList(quint64, QMailAccountId, QMailFolderId, bool)),
                handler, SLOT(retrieveFolderList(quint64, QMailAccountId, QMailFolderId, bool)));
        connect(client, SIGNAL(retrieveMessageList(quint64, QMailAccountId, QMailFolderId, uint, QMailMessageSortKey)),
                handler, SLOT(retrieveMessageList(quint64, QMailAccountId, QMailFolderId, uint, QMailMessageSortKey)));
        connect(client, SIGNAL(retrieveMessageLists(quint64, QMailAccountId, QMailFolderIdList, uint, QMailMessageSortKey)),
                handler, SLOT(retrieveMessageLists(quint64, QMailAccountId, QMailFolderIdList, uint, QMailMessageSortKey)));
        connect(client, SIGNAL(retrieveNewMessages(quint64, QMailAccountId, QMailFolderIdList)),
                handler, SLOT(retrieveNewMessages(quint64, QMailAccountId, QMailFolderIdList)));
        connect(client, SIGNAL(createStandardFolders(quint64, QMailAccountId)),
                handler, SLOT(createStandardFolders(quint64, QMailAccountId)));
        connect(client, SIGNAL(retrieveMessages(quint64, QMailMessageIdList, QMailRetrievalAction::RetrievalSpecification)),
                handler, SLOT(retrieveMessages(quint64, QMailMessageIdList, QMailRetrievalAction::RetrievalSpecification)));
        connect(client, SIGNAL(retrieveMessagePart(quint64, QMailMessagePart::Location)),
                handler, SLOT(retrieveMessagePart(quint64, QMailMessagePart::Location)));
        connect(client, SIGNAL(retrieveMessageRange(quint64, QMailMessageId, uint)),
                handler, SLOT(retrieveMessageRange(quint64, QMailMessageId, uint)));
        connect(client, SIGNAL(retrieveMessagePartRange(quint64, QMailMessagePart::Location, uint)),
                handler, SLOT(retrieveMessagePartRange(quint64, QMailMessagePart::Location, uint)));
        connect(client, SIGNAL(retrieveAll(quint64, QMailAccountId)),
                handler, SLOT(retrieveAll(quint64, QMailAccountId)));
        connect(client, SIGNAL(exportUpdates(quint64, QMailAccountId)),
                handler, SLOT(exportUpdates(quint64, QMailAccountId)));
        connect(client, SIGNAL(synchronize(quint64, QMailAccountId)),
                handler, SLOT(synchronize(quint64, QMailAccountId)));
        connect(client, SIGNAL(onlineDeleteMessages(quint64, QMailMessageIdList, QMailStore::MessageRemovalOption)),
                handler, SLOT(onlineDeleteMessages(quint64, QMailMessageIdList, QMailStore::MessageRemovalOption)));
        connect(client, SIGNAL(onlineCopyMessages(quint64, QMailMessageIdList, QMailFolderId)),
                handler, SLOT(onlineCopyMessages(quint64, QMailMessageIdList, QMailFolderId)));
        connect(client, SIGNAL(onlineMoveMessages(quint64, QMailMessageIdList, QMailFolderId)),
                handler, SLOT(onlineMoveMessages(quint64, QMailMessageIdList, QMailFolderId)));
        connect(client, SIGNAL(onlineFlagMessagesAndMoveToStandardFolder(quint64, QMailMessageIdList, quint64, quint64)),
                handler, SLOT(onlineFlagMessagesAndMoveToStandardFolder(quint64, QMailMessageIdList, quint64, quint64)));
        connect(client, SIGNAL(addMessages(quint64, QString)),
                handler, SLOT(addMessages(quint64, QString)));
        connect(client, SIGNAL(updateMessages(quint64, QString)),
                handler, SLOT(updateMessages(quint64, QString)));
        connect(client, SIGNAL(addMessages(quint64, QMailMessageMetaDataList)),
                handler, SLOT(addMessages(quint64, QMailMessageMetaDataList)));
        connect(client, SIGNAL(updateMessages(quint64, QMailMessageMetaDataList)),
                handler, SLOT(updateMessages(quint64, QMailMessageMetaDataList)));
        connect(client, SIGNAL(deleteMessages(quint64, QMailMessageIdList)),
                handler, SLOT(deleteMessages(quint64, QMailMessageIdList)));
        connect(client, SIGNAL(rollBackUpdates(quint64, QMailAccountId)),
                handler, SLOT(rollBackUpdates(quint64, QMailAccountId)));
        connect(client, SIGNAL(moveToStandardFolder(quint64, QMailMessageIdList, quint64)),
                handler, SLOT(moveToStandardFolder(quint64, QMailMessageIdList, quint64)));
        connect(client, SIGNAL(moveToFolder(quint64, QMailMessageIdList, QMailFolderId)),
                handler, SLOT(moveToFolder(quint64, QMailMessageIdList, QMailFolderId)));
        connect(client, SIGNAL(flagMessages(quint64, QMailMessageIdList, quint64, quint64)),
                handler, SLOT(flagMessages(quint64, QMailMessageIdList, quint64, quint64)));
        connect(client, SIGNAL(restoreToPreviousFolder(quint64, QMailMessageKey)),
                handler, SLOT(restoreToPreviousFolder(quint64, QMailMessageKey)));
        connect(client, SIGNAL(onlineCreateFolder(quint64, QString, QMailAccountId, QMailFolderId)),
                handler, SLOT(onlineCreateFolder(quint64, QString, QMailAccountId, QMailFolderId)));
        connect(client, SIGNAL(onlineRenameFolder(quint64, QMailFolderId, QString)),
                handler, SLOT(onlineRenameFolder(quint64, QMailFolderId, QString)));
        connect(client, SIGNAL(onlineDeleteFolder(quint64, QMailFolderId)),
                handler, SLOT(onlineDeleteFolder(quint64, QMailFolderId)));
        connect(client, SIGNAL(onlineMoveFolder(quint64, QMailFolderId, QMailFolderId)),
                handler, SLOT(onlineMoveFolder(quint64, QMailFolderId, QMailFolderId)));
        connect(client, SIGNAL(cancelTransfer(quint64)),
                handler, SLOT(cancelTransfer(quint64)));
        connect(client, SIGNAL(protocolRequest(quint64, QMailAccountId, QString, QVariant)),
                handler, SLOT(protocolRequest(quint64, QMailAccountId, QString, QVariant)));
        connect(client, SIGNAL(searchMessages(quint64, QMailMessageKey, QString, QMailSearchAction::SearchSpecification, QMailMessageSortKey)),
                handler, SLOT(searchMessages(quint64, QMailMessageKey, QString, QMailSearchAction::SearchSpecification, QMailMessageSortKey)));
        connect(client, SIGNAL(searchMessages(quint64, QMailMessageKey, QString, QMailSearchAction::SearchSpecification, quint64, QMailMessageSortKey)),
                handler, SLOT(searchMessages(quint64, QMailMessageKey, QString, QMailSearchAction::SearchSpecification, quint64, QMailMessageSortKey)));
        connect(client, SIGNAL(countMessages(quint64, QMailMessageKey, QString)),
                handler, SLOT(countMessages(quint64, QMailMessageKey, QString)));
        connect(client, SIGNAL(shutdown()),
                handler, SLOT(shutdown()));
        connect(client, SIGNAL(listActions()),
                handler, SLOT(listActions()));
       connect(handler, SIGNAL(newMessagesAvailable()),
                this, SLOT(reportNewCounts()));
        connect(client, SIGNAL(acknowledgeNewMessages(QMailMessageTypeList)),
                this, SLOT(acknowledgeNewMessages(QMailMessageTypeList)));

        QCopAdaptor::connect(this, SIGNAL(messageCountUpdated()),
             &messageCountUpdate, MESSAGE(changeValue()));

        //clean up any temporary messages that were not cleaned up by clients
        QTimer::singleShot(0, this, SLOT(cleanupTemporaryMessages()));

        emit client->actionsListed(QMailActionDataList());
    }

#ifdef MESSAGESERVER_PLUGINS
    qMailLog(Messaging) << "Initiating messageserver plugins.";
    QStringList availablePlugins = QMailMessageServerPluginFactory::keys();

    for (int i = 0; i < availablePlugins.size(); i++) {
        QMailMessageServerPlugin *plugin = QMailMessageServerPluginFactory::createService(availablePlugins.at(i));
        plugin->exec();
    }
#endif
}

MessageServer::~MessageServer()
{
}

void MessageServer::retrievalCompleted(quint64 action)
{
    // Ensure the client receives any resulting events before a notification
    QMailStore::instance()->flushIpcNotifications();

    if (!completionList.isEmpty()) {
        if (!completionAttempted) {
            // Complete the messages that we selected for immediate completion
            completionAttempted = true;
            handler->retrieveMessages(action, completionList.toList(), QMailRetrievalAction::Content);
            return;
        } else {
            completionList.clear();
        }
    }

    completionAttempted = false;
    emit client->retrievalCompleted(action);
}

QMap<QMailMessage::MessageType, QString> typeSignatureInit()
{
    QMap<QMailMessage::MessageType, QString> map;

    map.insert(QMailMessage::Sms, "newSmsCount(int)");
    map.insert(QMailMessage::Mms, "newMmsCount(int)");
    map.insert(QMailMessage::Email, "newEmailCount(int)");
    map.insert(QMailMessage::Instant, "newInstantCount(int)");
    map.insert(QMailMessage::System, "newSystemCount(int)");

    return map;
}

void MessageServer::acknowledgeNewMessages(const QMailMessageTypeList& types)
{
    foreach (QMailMessage::MessageType type, types) {
        // No messages of this type are new any longer
        QMailMessageKey newMessages(QMailMessageKey::messageType(type));
        newMessages &= QMailMessageKey(QMailMessageKey::status(QMailMessage::New, QMailDataComparator::Includes));
        QMailStore::instance()->updateMessagesMetaData(newMessages, QMailMessage::New, false);

        if (messageCounts[type] != 0) {
            newMessageTotal -= messageCounts[type];

            messageCounts[type] = 0;
            NewCountNotifier::notify(type, 0);
        }
    }
}

static QMap<QMailMessage::MessageType, QString> typeServiceInit()
{
    QMap<QMailMessage::MessageType, QString> map;

    map.insert(QMailMessage::Sms, "NewSmsArrival");
    map.insert(QMailMessage::Mms, "NewMmsArrival");
    map.insert(QMailMessage::Email, "NewEmailArrival");
    map.insert(QMailMessage::Instant, "NewInstantMessageArrival");
    map.insert(QMailMessage::System, "NewSystemMessageArrival");

    return map;
}

QString serviceForType(QMailMessage::MessageType type)
{
    static QMap<QMailMessage::MessageType, QString> typeService(typeServiceInit());
    return typeService[type];
}

int MessageServer::newMessageCount(QMailMessage::MessageType type) const
{
    QMailMessageKey newMessageKey(QMailMessageKey::status(QMailMessage::New, QMailDataComparator::Includes));
    if (type != QMailMessage::AnyType) {
        newMessageKey &= QMailMessageKey::messageType(type);
    }

    return QMailStore::instance()->countMessages(newMessageKey);
}

void MessageServer::reportNewCounts()
{
    static QMap<QMailMessage::MessageType, QString> typeSignature(typeSignatureInit());

    QMailMessageCountMap newCounts;
    foreach (const QMailMessage::MessageType &type, typeSignature.keys()) {
        newCounts[type] = newMessageCount(type);
    }

    newMessageTotal = newMessageCount(QMailMessage::AnyType);

    if (newMessageTotal) {
        // Inform QPE of changes to the new message counts
        foreach (const QMailMessage::MessageType &type, typeSignature.keys()) {
            if ((newCounts[type] > 0) && (newCounts[type] != messageCounts[type]))
               NewCountNotifier::notify(type, newCounts[type]);
        }

        // Request handling of the new message events
        QMailMessageCountMap::const_iterator it = newCounts.begin(), end = newCounts.end();
        for ( ; it != end; ++it) {
            QMailMessage::MessageType type(it.key());
            if (it.value() != messageCounts[type]) {
                // This type's count has changed since last reported

                if( NewCountNotifier* action = new NewCountNotifier(type,it.value())) {
                    actionType[action] = type;

                    connect(action, SIGNAL(response(bool)), this, SLOT(response(bool)));
                    connect(action, SIGNAL(error(QString)), this, SLOT(error(QString)));

                    // Ensure the client receives any generated events before the arrival notification
                    QMailStore::instance()->flushIpcNotifications();
                    if (!action->notify())
                        qWarning() << "Unable to invoke service:" << serviceForType(type);
                }
            }
        }
    }

    messageCounts = newCounts;
}

void MessageServer::response(bool handled)
{
    if (NewCountNotifier* action = static_cast<NewCountNotifier*>(sender())) {
        if (handled) {
            acknowledgeNewMessages(QMailMessageTypeList() << actionType[action]);
        }
        actionType.remove(action);
        action->deleteLater();
        if (actionType.isEmpty()) {
            // All outstanding handler events have been processed
            emit messageCountUpdated();
        }
    }
}

void MessageServer::error(const QString &message)
{
    if (NewCountNotifier* action = static_cast<NewCountNotifier*>(sender())) {
        qWarning() << "Unable to complete service:" << serviceForType(actionType[action]) << "-" << message;
        actionType.remove(action);
        action->deleteLater();
    }

    if (actionType.isEmpty()) {
        // No outstanding handler events remain
        emit messageCountUpdated();
    }
}

void MessageServer::transmissionCompleted(quint64 action)
{
    // Ensure the client receives any resulting events before the completion notification
    QMailStore::instance()->flushIpcNotifications();

    emit client->transmissionCompleted(action);
}

void MessageServer::messagesAdded(const QMailMessageIdList &ids)
{
    if (!QMailStore::instance()->asynchronousEmission()) {
        // Added in our process - from retrieval
        foreach (const QMailMessageId &id, ids) {
            QMailMessageMetaData message(id);

            bool complete(false);
            if (!(message.status() & QMailMessage::ContentAvailable)) {
                // Automatically download voicemail messages
                if (message.content() == QMailMessage::VoicemailContent ||
                    message.content() == QMailMessage::VideomailContent) {
                    complete = true;
                }
            }

            if (complete)
                completionList.insert(message.id());
        }
    }
}

void MessageServer::messagesUpdated(const QMailMessageIdList &ids)
{
    if (QMailStore::instance()->asynchronousEmission()) {
        // Only need to check message counts if the update occurred in another process
        updateNewMessageCounts();
    } else {
        // If we're updating, check whether the messages have been marked as Removed
        foreach (const QMailMessageId &id, ids) {
            if (completionList.contains(id)) {
                QMailMessageMetaData message(id);
                if ((message.status() & QMailMessage::ContentAvailable) || (message.status() & QMailMessage::Removed)) {
                    // This message has been completed (or removed)
                    completionList.remove(id);
                }
            }
        }
    }
}

void MessageServer::messagesRemoved(const QMailMessageIdList &ids)
{
    foreach (const QMailMessageId &id, ids) {
        // No need to complete deleted messages
        completionList.remove(id);
    }

    updateNewMessageCounts();
}

void MessageServer::updateNewMessageCounts()
{
    int newTotal = newMessageCount(QMailMessage::AnyType);
    if (newTotal != newMessageTotal) {
        // The number of messages marked as new has changed, but not via a message arrival event
        static QMap<QMailMessage::MessageType, QString> typeSignature(typeSignatureInit());

        // Update the individual counts
        foreach (const QMailMessage::MessageType &type, typeSignature.keys()) {
            int count(newMessageCount(type));
            if (count != messageCounts[type]) {
                messageCounts[type] = count;
                NewCountNotifier::notify(type, count);
            }
        }

        emit messageCountUpdated();
    }
}

void MessageServer::cleanupTemporaryMessages()
{
    QMailStore::instance()->removeMessages(QMailMessageKey::status(QMailMessage::Temporary), QMailStore::NoRemovalRecord);
}

#if defined(Q_OS_UNIX)
void MessageServer::hupSignalHandler(int)
{
    // Can't call Qt code. Write to the socket and the notifier will fire from the Qt event loop
    char a = 1;
    ::write(sighupFd[0], &a, sizeof(a));
}

void MessageServer::handleSigHup()
{
    snHup->setEnabled(false);
    char tmp;
    ::read(sighupFd[1], &tmp, sizeof(tmp));

    // This is ~/.config/QtProject/Messageserver.conf
    qMailLoggersRecreate("QtProject", "Messageserver", "Msgsrv");

    snHup->setEnabled(true);
}

void MessageServer::termSignalHandler(int)
{
    char a = 1;
    ::write(sigtermFd[0], &a, sizeof(a));
}

void MessageServer::handleSigTerm()
{
    snTerm->setEnabled(false);
    char tmp;
    ::read(sigtermFd[1], &tmp, sizeof(tmp));

    qMailLog(Messaging) << "Received SIGTERM, shutting down.";
    QCoreApplication::exit();

    snTerm->setEnabled(true);
}

void MessageServer::intSignalHandler(int)
{
    char a = 1;
    ::write(sigintFd[0], &a, sizeof(a));
}

void MessageServer::handleSigInt()
{
    snInt->setEnabled(false);
    char tmp;
    ::read(sigintFd[1], &tmp, sizeof(tmp));

    qMailLog(Messaging) << "Received SIGINT, shutting down.";
    QCoreApplication::exit();

    snInt->setEnabled(true);
}

#endif // defined(Q_OS_UNIX)
