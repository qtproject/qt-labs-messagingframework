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

#include "imapstrategy.h"
#include "imapclient.h"
#include "imapconfiguration.h"
#include <private/longstream_p.h>
#include <qobject.h>
#include <qmaillog.h>
#include <qmailaccount.h>
#include <qmailcrypto.h>
#include <qmailstore.h>
#include <qmailaccountconfiguration.h>
#include <qmailmessage.h>
#include <qmailnamespace.h>
#include <qmaildisconnected.h>
#include <qmailcodec.h>
#include <limits.h>
#include <QDir>


namespace {
const int MetaDataFetchFlags = F_Uid | F_Date | F_Rfc822_Size | F_Rfc822_Header | F_BodyStructure;
const int ContentFetchFlags = F_Uid | F_Rfc822_Size | F_Rfc822;

QString stripFolderPrefix(const QString &str)
{
    QString result;
    int index;
    if ((index = str.lastIndexOf(UID_SEPARATOR)) != -1)
        return str.mid(index + 1);
    return str;
}

QStringList stripFolderPrefix(const QStringList &list)
{
    QStringList result;
    foreach(const QString &uid, list) {
        result.append(stripFolderPrefix(uid));
    }
    return result;
}

QStringList inFirstAndSecond(const QStringList &first, const QStringList &second)
{
    // TODO: this may be more efficient if we convert both inputs to sets and perform set operations
    QStringList result;

    foreach (const QString &value, first)
        if (second.contains(value))
            result.append(value);

    return result;
}

QStringList inFirstButNotSecond(const QStringList &first, const QStringList &second)
{
    QStringList result;

    foreach (const QString &value, first)
        if (!second.contains(value))
            result.append(value);

    return result;
}

bool messageSelectorLessThan(const MessageSelector &lhs, const MessageSelector &rhs)
{
    // Any full message sorts before any message element
    if (lhs._properties.isEmpty() && !rhs._properties.isEmpty()) {
        return true;
    } else if (rhs._properties.isEmpty() && !lhs._properties.isEmpty()) {
        return false;
    }

    // Any UID value sorts before a non-UID value
    if ((lhs._uid != 0) && (rhs._uid == 0)) {
        return true;
    } else if ((rhs._uid != 0) && (lhs._uid == 0)) {
        return false;
    } 

    if (lhs._uid != 0) {
        if (lhs._uid != rhs._uid) {
            return (lhs._uid < rhs._uid);
        }
    } else {
        if (lhs._messageId.toULongLong() != rhs._messageId.toULongLong()) {
            return (lhs._messageId.toULongLong() < rhs._messageId.toULongLong());
        }
    }

    // Messages are the same - fall back to lexicographic comparison of the location
    // but ensure headers are seen first.
    if (!(lhs._properties._location == rhs._properties._location))
        return (lhs._properties._location.toString(false) < rhs._properties._location.toString(false));
    else
        return lhs._properties._minimum == SectionProperties::HeadersOnly
            && rhs._properties._minimum != SectionProperties::HeadersOnly ? true : false;
}

bool purge(ImapStrategyContextBase *context, const QMailMessageKey &removedKey)
{
    bool result(true);
    QStringList vanishedIds;
    foreach (const QMailMessageMetaData& r, QMailStore::instance()->messagesMetaData(removedKey, QMailMessageKey::ServerUid))  {
        const QString &uid(r.serverUid()); 
        // We might have a deletion record for this UID
        vanishedIds << uid;
    }
    if (!vanishedIds.isEmpty() && // guard to protect against deleting all removal records when vanishedIds is empty!
        !QMailStore::instance()->purgeMessageRemovalRecords(context->config().id(), vanishedIds)) {
        result = false;
        qWarning() << "Unable to purge message records for account:" << context->config().id();
    }
    if (!QMailStore::instance()->removeMessages(removedKey, QMailStore::NoRemovalRecord)) {
        result = false;
        qWarning() << "Unable to update folder after uidvalidity changed:" << QMailFolder(context->mailbox().id).displayName();
    }
    return result;
}

bool updateMessagesMetaData(ImapStrategyContextBase *context,
                                   const QMailMessageKey &storedKey, 
                                   const QMailMessageKey &unseenKey, 
                                   const QMailMessageKey &seenKey,
                                   const QMailMessageKey &flaggedAsDeleted,
                                   const QMailMessageKey &flaggedKey,
                                   const QMailMessageKey &unreadElsewhereKey,
                                   const QMailMessageKey &importantElsewhereKey,
                                   const QMailMessageKey &unavailableKey)
{
    bool result = true;
    QMailMessageKey reportedKey((seenKey | unseenKey) & ~flaggedAsDeleted);
    QMailMessageKey unflaggedKey(reportedKey & ~flaggedKey);

    // Mark as deleted any messages that the server does not report
    QMailMessageKey nonexistentKey(storedKey & ~reportedKey);
    QMailMessageIdList ids(QMailStore::instance()->queryMessages(nonexistentKey));
    
    if (!purge(context, nonexistentKey)) {
        result = false;
        qWarning() << "Unable to purge messages for account:" << context->config().id();
    }
    
    // Restore any messages thought to be unavailable that the server now reports
    QMailMessageKey reexistentKey(unavailableKey & reportedKey);
    if (!QMailStore::instance()->updateMessagesMetaData(reexistentKey, QMailMessage::Removed, false)) {
        result = false;
        qWarning() << "Unable to update un-removed message metadata for account:" << context->config().id();
    }

    foreach (const QMailMessageMetaData& r, QMailStore::instance()->messagesMetaData(nonexistentKey, QMailMessageKey::ServerUid))  {
        const QString &uid(r.serverUid()); 
        // We might have a deletion record for this UID
        if (!QMailStore::instance()->purgeMessageRemovalRecords(context->config().id(), QStringList() << uid)) {
            result = false;
            qWarning() << "Unable to purge message records for account:" << context->config().id();
        }
        context->completedMessageAction(uid);
    }

    // Update any messages that are reported as read elsewhere, that previously were not read elsewhere
    if (!QMailStore::instance()->updateMessagesMetaData(seenKey & unreadElsewhereKey, QMailMessage::Read, true)
        || !QMailStore::instance()->updateMessagesMetaData(seenKey & unreadElsewhereKey, QMailMessage::ReadElsewhere, true)) {
        result = false;
        qWarning() << "Unable to update read message metadata for account:" << context->config().id();
    }

    // Update any messages that are reported as unread elsewhere, that previously were read elsewhere
    if (!QMailStore::instance()->updateMessagesMetaData(unseenKey & ~unreadElsewhereKey, QMailMessage::Read, false)
        || !QMailStore::instance()->updateMessagesMetaData(unseenKey & ~unreadElsewhereKey, QMailMessage::ReadElsewhere, false)) {
        result = false;
        qWarning() << "Unable to update unread message metadata for account:" << context->config().id();
    }
    
    // Update any messages that are reported as important elsewhere, that previously were not important elsewhere
    if (!QMailStore::instance()->updateMessagesMetaData(flaggedKey & ~importantElsewhereKey, QMailMessage::Important, true)
        || !QMailStore::instance()->updateMessagesMetaData(flaggedKey & ~importantElsewhereKey, QMailMessage::ImportantElsewhere, true)) {
        result = false;
        qWarning() << "Unable to update important status flag message metadata for account:" << context->config().id();
    }

    // Update any messages that are reported as not important elsewhere, that previously were important elsewhere
    if (!QMailStore::instance()->updateMessagesMetaData(unflaggedKey & importantElsewhereKey, QMailMessage::Important, false)
        || !QMailStore::instance()->updateMessagesMetaData(unflaggedKey & importantElsewhereKey, QMailMessage::ImportantElsewhere, false)) {
        result = false;
        qWarning() << "Unable to update not important status flag message metadata for account:" << context->config().id();
    }
    
    return result;
}

bool findFetchContinuationStart(const QMailMessage &message, const QMailMessagePart::Location &location, int *start)
{
    if (message.id().isValid()) {
        *start = 0;
        if (location.isValid() && message.contains(location)) {
            const QMailMessagePart &part(message.partAt(location));
            if (part.hasBody()) {
                *start = part.body().length();
            }
        } else {
            if (message.hasBody()) {
                *start = message.body().length();
            }
        }
        return true;
    }

    return false;
}

QString numericUidSequence(const QStringList &uids)
{
    QStringList numericUids;
    foreach (const QString &uid, uids) {
        numericUids.append(ImapProtocol::uid(uid));
    }

    return IntegerRegion(numericUids).toString();
}

bool transferPartBodies(QMailMessagePartContainer &destination, const QMailMessagePartContainer &source)
{
    if (destination.partCount() != source.partCount()) {
        qWarning() << "transferPartBodies detected copy failure, aborting transfer. Part count, destination" << destination.partCount() << "source" << source.partCount();
        return false; // copy failed, abort entire move
    }

    if (source.hasBody()) {
        // If the content of the source part is not fully available
        // flag the copy with the same status
        if (!source.contentAvailable()) {
            // Incomplete parts are always saved encoded
            destination.setBody(source.body(), QMailMessageBody::Encoded);
            destination.setHeaderField("X-qmf-internal-partial-content", "true");
       } else {
            destination.setBody(source.body());
       }
    } else if (source.partCount() > 0) {
        for (uint i = 0; i < source.partCount(); ++i) {
            const QMailMessagePart &sourcePart = source.partAt(i);
            QMailMessagePart &destinationPart = destination.partAt(i);

            bool result = transferPartBodies(destinationPart, sourcePart);
            if (!result)
                return false;
        }
    }
    return true;
}

bool transferMessageData(QMailMessage &message, const QMailMessage &source)
{
    // TODO: Perform sanity checking to ensure message and source message refer to the same message
    // e.g. same subject, rfcid, date, content-type, no of parts

    if (!transferPartBodies(message, source))
        return false;

    if (!message.customField("qmf-detached-filename").isEmpty()) {
        // We have modified the content, so the detached file data is no longer sufficient
        QFile::remove(message.customField("qmf-detached-filename"));
        message.removeCustomField("qmf-detached-filename");
    }

    if (source.status() & QMailMessage::ContentAvailable) {
        message.setStatus(QMailMessage::ContentAvailable, true);
    }

    if (source.status() & QMailMessage::PartialContentAvailable) {
        message.setStatus(QMailMessage::PartialContentAvailable, true);
    }
    return true;
}

void updateAccountLastSynchronized(ImapStrategyContextBase *context)
{
    QMailAccount account(context->config().id());
    account.setLastSynchronized(QMailTimeStamp::currentDateTime());
    if (!QMailStore::instance()->updateAccount(&account))
        qWarning() << "Unable to update account" << account.id() << "to set lastSynchronized";
}

QSet<QMailFolderId> foldersApplicableTo(QMailMessageKey const& messagekey, QSet<QMailFolderId> const& total)
{
    typedef QPair< QSet<QMailFolderId>, QSet<QMailFolderId> >  IncludedExcludedPair;

    struct L {
        static IncludedExcludedPair extractFolders(QMailMessageKey const& key)
        {
            bool isNegated(key.isNegated());
            IncludedExcludedPair r;

            QSet<QMailFolderId> & included = isNegated ? r.second : r.first;
            QSet<QMailFolderId> & excluded = isNegated ? r.first : r.second;

            foreach(QMailMessageKey::ArgumentType const& arg, key.arguments())
            {
                switch (arg.property)
                {
                case QMailMessageKey::ParentFolderId:
                    if (arg.op == QMailKey::Equal || arg.op == QMailKey::Includes) {
                        Q_ASSERT(arg.valueList.count() == 1);
                        Q_ASSERT(arg.valueList[0].canConvert<QMailFolderId>());
                        included.insert(arg.valueList[0].value<QMailFolderId>());
                    } else if (arg.op == QMailKey::NotEqual || arg.op == QMailKey::Excludes) {
                        Q_ASSERT(arg.valueList.count() == 1);
                        Q_ASSERT(arg.valueList[0].canConvert<QMailFolderId>());
                        excluded.insert(arg.valueList[0].value<QMailFolderId>());
                    } else {
                        Q_ASSERT(false);
                    }
                    break;
                case QMailMessageKey::AncestorFolderIds:
                    if (arg.op == QMailKey::Equal || arg.op == QMailKey::Includes) {
                        Q_ASSERT(arg.valueList.count() == 1);
                        Q_ASSERT(arg.valueList[0].canConvert<QMailFolderId>());
                        included.unite(QMailStore::instance()->queryFolders(
                                QMailFolderKey::ancestorFolderIds(arg.valueList[0].value<QMailFolderId>())).toSet());
                    } else if (arg.op == QMailKey::NotEqual || arg.op == QMailKey::Excludes) {
                        Q_ASSERT(arg.valueList.count() == 1);
                        Q_ASSERT(arg.valueList[0].canConvert<QMailFolderId>());
                        excluded.unite(QMailStore::instance()->queryFolders(
                                QMailFolderKey::ancestorFolderIds(arg.valueList[0].value<QMailFolderId>())).toSet());
                    } else {
                        Q_ASSERT(false);
                    }
                    break;

                case QMailMessageKey::ParentAccountId:
                    if (arg.op == QMailKey::Equal || arg.op == QMailKey::Includes) {
                        Q_ASSERT(arg.valueList.count() == 1);
                        Q_ASSERT(arg.valueList[0].canConvert<QMailAccountId>());
                        included.unite(QMailStore::instance()->queryFolders(
                                QMailFolderKey::parentAccountId(arg.valueList[0].value<QMailAccountId>())).toSet());
                    } else if (arg.op == QMailKey::NotEqual || arg.op == QMailKey::Excludes) {
                        Q_ASSERT(arg.valueList.count() == 1);
                        Q_ASSERT(arg.valueList[0].canConvert<QMailAccountId>());
                        excluded.unite(QMailStore::instance()->queryFolders(
                                QMailFolderKey::parentAccountId(arg.valueList[0].value<QMailAccountId>())).toSet());
                    } else {
                        Q_ASSERT(false);
                    }
                default:
                    break;
                }
            }

            if (key.combiner() == QMailKey::None) {
                Q_ASSERT(key.subKeys().size() == 0);
                Q_ASSERT(key.arguments().size() <= 1);
            } else if (key.combiner() == QMailKey::Or) {
                foreach (QMailMessageKey const& k, key.subKeys()) {
                    IncludedExcludedPair v(extractFolders(k));
                    included.unite(v.first);
                    excluded.unite(v.second);
                }
            } else if (key.combiner() == QMailKey::And) {
                bool filled(included.size() == 0 && excluded.size() == 0 ? false : true);

                for (QList<QMailMessageKey>::const_iterator it(key.subKeys().begin()) ; it != key.subKeys().end() ; ++it) {
                    IncludedExcludedPair next(extractFolders(*it));
                    if (next.first.size() != 0 || next.second.size() != 0) {
                        if (filled) {
                            included.intersect(next.first);
                            excluded.intersect(next.second);
                        } else {
                            filled = true;
                            included.unite(next.first);
                            excluded.unite(next.second);
                        }
                    }
                }
            } else {
                Q_ASSERT(false);
            }
            return r;
        }
    };

    IncludedExcludedPair result(L::extractFolders(messagekey));
    if (result.first.isEmpty()) {
        return total - result.second;
    } else {
        return (total & result.first) - result.second;
    }
}

QStringList flaggedAsDeletedUids(ImapStrategyContextBase *context)
{
    QStringList flaggedAsDeleted;
    const ImapMailboxProperties &properties(context->mailbox());
    foreach (FlagChange change, properties.flagChanges) {
        QString uidStr(stripFolderPrefix(change.first));
        MessageFlags flags(change.second);
        if (!uidStr.isEmpty()) {
            if (flags & MFlag_Deleted) {
                flaggedAsDeleted.append(uidStr);
            }
        }
    }
    return flaggedAsDeleted;
}

}


ImapClient *ImapStrategyContextBase::client() 
{ 
    return _client; 
}

ImapProtocol &ImapStrategyContextBase::protocol() 
{ 
    return _client->_protocol; 
}

const ImapMailboxProperties &ImapStrategyContextBase::mailbox() 
{ 
    return _client->_protocol.mailbox(); 
}

const QMailAccountConfiguration &ImapStrategyContextBase::config() 
{ 
    return _client->_config; 
}

void ImapStrategyContextBase::updateStatus(const QString &text) 
{ 
    emit _client->updateStatus(text);
}

void ImapStrategyContextBase::progressChanged(uint progress, uint total) 
{ 
    emit _client->progressChanged(progress, total);
}

void ImapStrategyContextBase::completedMessageAction(const QString &text) 
{ 
    emit _client->messageActionCompleted(text);
}

void ImapStrategyContextBase::completedMessageCopy(QMailMessage &message, const QMailMessage &original) 
{ 
    emit _client->messageCopyCompleted(message, original);
}

void ImapStrategyContextBase::operationCompleted()
{ 
    // Flush any pending messages now so that _modifiedFolders is up to date
    QMailMessageBuffer::instance()->flush();

    // Update the status on any folders we modified
    for (QSet<QMailFolderId>::iterator it(_modifiedFolders.begin()) ; it != _modifiedFolders.end(); it = _modifiedFolders.erase(it))
    {
        QMailFolder folder(*it);
        _client->updateFolderCountStatus(&folder);

        if (!QMailStore::instance()->updateFolder(&folder)) {
            qWarning() << "Unable to update folder " << *it << " for account:" << _client->_config.id();
        }
    }

    _client->retrieveOperationCompleted();
}

void ImapStrategyContextBase::matchingMessageIds(const QMailMessageIdList &msgs)
{
    emit _client->matchingMessageIds(msgs);
}

void ImapStrategyContextBase::remainingMessagesCount(uint count)
{
    emit _client->remainingMessagesCount(count);
}

void ImapStrategyContextBase::messagesCount(uint count)
{
    emit _client->messagesCount(count);
}

/* A basic strategy to achieve an authenticated state with the server,
   and to provide default responses to IMAP command completion notifications,
*/
void ImapStrategy::newConnection(ImapStrategyContextBase *context)
{
    _transferState = Init;

    ImapConfiguration imapCfg(context->config());
    _baseFolder = imapCfg.baseFolder();

    initialAction(context);
}

void ImapStrategy::initialAction(ImapStrategyContextBase *context)
{
    if (context->protocol().loggingOut()) {
        context->protocol().close();
    }

    if (context->protocol().inUse()) {
        // We have effectively just completed authenticating
        transition(context, IMAP_Login, OpOk);
    } else {
        ImapConfiguration imapCfg(context->config());
        context->protocol().open(imapCfg);
    }
}

void ImapStrategy::mailboxListed(ImapStrategyContextBase *c, QMailFolder& folder, const QString &flags)
{
    Q_UNUSED(c)
    Q_UNUSED(folder)
    Q_UNUSED(flags)
}

bool ImapStrategy::messageFetched(ImapStrategyContextBase * context, QMailMessage &message)
{
    _folder[message.serverUid()] = false;
    // Store this message to the mail store
    if (message.id().isValid()) {
        if (!QMailMessageBuffer::instance()->updateMessage(&message)) {
            _error = true;
            qWarning() << "Unable to update message for account:" << message.parentAccountId() << "UID:" << message.serverUid();
            return false;
        }
   } else {
        QMailMessageKey duplicateKey(QMailMessageKey::serverUid(message.serverUid()) & QMailMessageKey::parentAccountId(message.parentAccountId()));
        QMailMessageIdList ids(QMailStore::instance()->queryMessages(duplicateKey));
        if (!ids.isEmpty()) {
            QMailMessageId existingId(ids.takeFirst());
            if (!ids.isEmpty() && !QMailStore::instance()->removeMessages(QMailMessageKey::id(ids))) {
                _error = true;
                qWarning() << "Unable to remove duplicate message(s) for account:" << message.parentAccountId() << "UID:" << message.serverUid();
                return true;
            }
            QMailMessage lv(existingId);
            messageFlushed(context, lv);
            return true; // flushing complete
        } else {
            if (!QMailMessageBuffer::instance()->addMessage(&message)) {
                _error = true;
                qWarning() << "Unable to add message for account:" << message.parentAccountId() << "UID:" << message.serverUid();
                return false;
            }
        }

        _folder[message.serverUid()] = true;
    }
    return false;
}

void ImapStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    bool folder = _folder.take(message.serverUid());
    if (_error) return;

    if (folder) {
        context->folderModified(QMailDisconnected::sourceFolderId(message));
    }

    context->completedMessageAction(message.serverUid());
}

void ImapStrategy::dataFetched(ImapStrategyContextBase * /*context*/, QMailMessage &message, const QString &/*uid*/, const QString &/*section*/)
{
    // Store the updated message
    if (!QMailMessageBuffer::instance()->updateMessage(&message)) {
        _error = true;
        qWarning() << "Unable to update message for account:" << message.parentAccountId() << "UID:" << message.serverUid();
        return;
    }
}

void ImapStrategy::dataFlushed(ImapStrategyContextBase *context, QMailMessage &, const QString &uid, const QString &/*section*/)
{
    if (_error) return;

    context->completedMessageAction(uid);
}

void ImapStrategy::nonexistentUid(ImapStrategyContextBase *context, const QString &uid)
{
    // Mark this message as deleted
    QMailMessage message(uid, context->config().id());
    if (message.id().isValid()) {
        if (!purge(context, QMailMessageKey::id(message.id()))) {
            _error = true;
        }
    }

    context->completedMessageAction(uid);
}

void ImapStrategy::messageStored(ImapStrategyContextBase *context, const QString &uid)
{
    context->completedMessageAction(uid);
}

void ImapStrategy::messageCopied(ImapStrategyContextBase *context, const QString &copiedUid, const QString &createdUid)
{
    // A copy operation should be reported as completed by the subsequent fetch, not the copy itself
    //context->completedMessageAction(copiedUid);

    Q_UNUSED(context)
    Q_UNUSED(copiedUid)
    Q_UNUSED(createdUid)
}

void ImapStrategy::messageCreated(ImapStrategyContextBase *context, const QMailMessageId &id, const QString &uid)
{
    Q_UNUSED(context)
    Q_UNUSED(id)
    Q_UNUSED(uid)
}

void ImapStrategy::downloadSize(ImapStrategyContextBase *context, const QString &uid, int length)
{
    Q_UNUSED(context)
    Q_UNUSED(uid)
    Q_UNUSED(length)
}

void ImapStrategy::urlAuthorized(ImapStrategyContextBase *context, const QString &url)
{
    Q_UNUSED(context)
    Q_UNUSED(url)
}

void ImapStrategy::folderCreated(ImapStrategyContextBase *context, const QString &folder, bool success)
{
    Q_UNUSED(context)
    Q_UNUSED(folder)
    Q_UNUSED(success)
}

void ImapStrategy::folderDeleted(ImapStrategyContextBase *context, const QMailFolder &folder, bool success)
{
    Q_UNUSED(context)
    Q_UNUSED(folder)
    Q_UNUSED(success)
}

void ImapStrategy::folderRenamed(ImapStrategyContextBase *context, const QMailFolder &folder, const QString &newPath, bool success)
{
    Q_UNUSED(context)
    Q_UNUSED(folder)
    Q_UNUSED(newPath)
    Q_UNUSED(success)
}

void ImapStrategy::folderMoved(ImapStrategyContextBase *context, const QMailFolder &folder,
                               const QString &newPath, const QMailFolderId &newParentId, bool success)
{
    Q_UNUSED(context)
    Q_UNUSED(folder)
    Q_UNUSED(newPath)
    Q_UNUSED(newParentId)
    Q_UNUSED(success)
}

void ImapStrategy::selectFolder(ImapStrategyContextBase *context, const QMailFolder &folder)
{
    context->protocol().sendSelect(folder);
}

/* A strategy to create a folder */
void ImapCreateFolderStrategy::transition(ImapStrategyContextBase* context, const ImapCommand cmd, const OperationStatus op)
{
    if(op != OpOk)
        qWarning() << "IMAP Response to cmd:" << cmd << " is not ok: " << op;

    switch(cmd)
    {
    case IMAP_Login:
        handleLogin(context);
        break;
    case IMAP_Create:
        handleCreate(context);
        break;
    default:
        qWarning() << "Unhandled IMAP response:" << cmd;
    }
}

void ImapCreateFolderStrategy::createFolder(const QMailFolderId &folderParent, const QString &name, bool matchFoldersRequired)
{
    _matchFoldersRequired = matchFoldersRequired;
    _folders.append(qMakePair(folderParent, name));
}

void ImapCreateFolderStrategy::handleLogin(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapCreateFolderStrategy::handleCreate(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapCreateFolderStrategy::process(ImapStrategyContextBase *context)
{
    while(_folders.count() > 0) {
        QPair<QMailFolderId, QString> folder = _folders.takeFirst();
        _inProgress++;
        context->protocol().sendCreate(folder.first, folder.second);
    }
}

void ImapCreateFolderStrategy::folderCreated(ImapStrategyContextBase *context, const QString &folder, bool success)
{
    if (_inProgress > 0) {
        _inProgress--;
    }
    if (!success) {
        _inProgress = 0; // in case of error, subsequent responses may not be received
        qWarning() << "IMAP folder creation failed";
        return; // don't call context->operationCompleted in case of error
    }
    if (_inProgress == 0) {
        if (_matchFoldersRequired) {
            QMailAccountId accountId = context->config().id();
            QMail::detectStandardFolders(accountId);
        }
        context->operationCompleted();
    }
    Q_UNUSED(folder)
}


/* A strategy to delete a folder */
void ImapDeleteFolderStrategy::transition(ImapStrategyContextBase* context, const ImapCommand cmd, const OperationStatus op)
{
    if(op != OpOk)
        qWarning() << "IMAP Response to cmd:" << cmd << " is not ok: " << op;

    switch(cmd)
    {
    case IMAP_Login:
        handleLogin(context);
        break;
    case IMAP_Delete:
        handleDelete(context);
        break;
    default:
        qWarning() << "Unhandled IMAP response:" << cmd;
    }
}

void ImapDeleteFolderStrategy::deleteFolder(const QMailFolderId& folderId)
{
    _folderIds.append(folderId);
}

void ImapDeleteFolderStrategy::handleLogin(ImapStrategyContextBase* context)
{
    process(context);
}

void ImapDeleteFolderStrategy::handleDelete(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapDeleteFolderStrategy::process(ImapStrategyContextBase *context)
{
    while(_folderIds.count() > 0) {
        deleteFolder(_folderIds.takeFirst(), context);
    }
}

void ImapDeleteFolderStrategy::deleteFolder(const QMailFolderId &folderId, ImapStrategyContextBase *context)
{
    // Assume no pending disconnected moves/copies exist for this folder
    QMailFolderKey childKey(QMailFolderKey::parentFolderId(folderId));
    QMailFolderIdList childrenList = QMailStore::instance()->queryFolders(childKey);

    foreach(QMailFolderId fid, childrenList) {
        deleteFolder(fid, context);
    }

    //now the parent is safe to delete
    _inProgress++;
    context->protocol().sendDelete(QMailFolder(folderId));
}

void ImapDeleteFolderStrategy::folderDeleted(ImapStrategyContextBase *context, const QMailFolder &folder, bool success)
{
    if (_inProgress > 0) {
        _inProgress--;
    }
    if (!success) {
        _inProgress = 0; // in case of error, subsequent responses may not be received
        qWarning() << "IMAP folder deletion failed";
        return; // don't call context->operationCompleted in case of error
    }
    if (!QMailStore::instance()->removeFolder(folder.id()))
        qWarning() << "Unable to remove folder id: " << folder.id();

    if (_inProgress == 0)
       context->operationCompleted();
}

/* A strategy to rename a folder */
void ImapRenameFolderStrategy::transition(ImapStrategyContextBase* context, const ImapCommand cmd, const OperationStatus op)
{
    if(op != OpOk)
        qWarning() << "IMAP Response to cmd:" << cmd << " is not ok: " << op;

    switch(cmd)
    {
    case IMAP_Login:
        handleLogin(context);
        break;
    case IMAP_Rename:
        handleRename(context);
        break;
    default:
        qWarning() << "Unhandled IMAP response:" << cmd;
    }
}

void ImapRenameFolderStrategy::renameFolder(const QMailFolderId &folderId, const QString &newName)
{
    _folderNewNames.append(qMakePair(folderId, newName));
}

void ImapRenameFolderStrategy::handleLogin(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapRenameFolderStrategy::handleRename(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapRenameFolderStrategy::process(ImapStrategyContextBase *context)
{
    while(_folderNewNames.count() > 0) {
        const QPair<QMailFolderId, QString> &folderId_name =  _folderNewNames.takeFirst();
        _inProgress++;
        context->protocol().sendRename(QMailFolder(folderId_name.first), folderId_name.second);
    }
}

void ImapRenameFolderStrategy::folderRenamed(ImapStrategyContextBase *context, const QMailFolder &folder,
                                             const QString &newPath, bool success)
{
    QString name;
    if (_inProgress > 0) {
        _inProgress--;
    }
    if (!success) {
        _inProgress = 0; // in case of error, subsequent responses may not be received
        qWarning() << "IMAP folder rename failed";
        return; // don't call context->operationCompleted in case of error
    }
    if(!context->protocol().delimiter().isNull()) {
        //only update if we're dealing with a hierarchical system
        QChar delimiter = context->protocol().delimiter();
        if(folder.path().count(delimiter) == 0) {
            name = newPath;
        } else {
            name = newPath.section(delimiter, -1, -1);
        }
        QMailFolderKey affectedFolderKey(QMailFolderKey::ancestorFolderIds(folder.id()));
        QMailFolderIdList affectedFolders = QMailStore::instance()->queryFolders(affectedFolderKey);

        while (!affectedFolders.isEmpty()) {
            QMailFolder childFolder(affectedFolders.takeFirst());
            QString path = childFolder.path();
            path.replace(0, folder.path().length(), newPath);
            childFolder.setPath(path);
            if (!QMailStore::instance()->updateFolder(&childFolder))
                qWarning() << "Unable to locally change path of a subfolder";
        }

    } else {
        name = newPath;
    }

    QMailFolder newFolder = folder;
    newFolder.setPath(newPath);
    newFolder.setDisplayName(QMailCodec::decodeModifiedUtf7(name));

    if(!QMailStore::instance()->updateFolder(&newFolder))
        qWarning() << "Unable to locally rename folder";
    if (_inProgress == 0)
        context->operationCompleted();
}

/* A strategy to move a folder */
void ImapMoveFolderStrategy::transition(ImapStrategyContextBase* context, const ImapCommand cmd, const OperationStatus op)
{
    if (op != OpOk)
        qWarning() << "IMAP Response to cmd:" << cmd << " is not ok: " << op;

    switch (cmd)
    {
    case IMAP_Login:
        handleLogin(context);
        break;
    case IMAP_Move:
        handleMove(context);
        break;
    default:
        qWarning() << "Unhandled IMAP response:" << cmd;
    }
}

void ImapMoveFolderStrategy::moveFolder(const QMailFolderId &folderId, const QMailFolderId &newParentId)
{
    _folderNewParents.append(qMakePair(folderId, newParentId));
}

void ImapMoveFolderStrategy::handleLogin(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapMoveFolderStrategy::handleMove(ImapStrategyContextBase *context)
{
    process(context);
}

void ImapMoveFolderStrategy::process(ImapStrategyContextBase *context)
{
    while (_folderNewParents.count() > 0) {
        const QPair<QMailFolderId, QMailFolderId> &folderId_parent = _folderNewParents.takeFirst();
        _inProgress++;
        context->protocol().sendMove(QMailFolder(folderId_parent.first), folderId_parent.second);
    }
}

void ImapMoveFolderStrategy::folderMoved(ImapStrategyContextBase *context, const QMailFolder &folder,
                                         const QString &newPath, const QMailFolderId &newParentId, bool success)
{
    if (_inProgress > 0) {
        _inProgress--;
    }
    if (!success) {
        _inProgress = 0; // in case of error, subsequent responses may not be received
        return;
    }
    QString name;
    if (!context->protocol().delimiter().isNull()) {
        //only update if we're dealing with a hierarchical system
        QChar delimiter = context->protocol().delimiter();
        if (folder.path().count(delimiter) == 0) {
            name = newPath;
        } else {
            name = newPath.section(delimiter, -1, -1);
        }
        QMailFolderKey affectedFolderKey(QMailFolderKey::ancestorFolderIds(folder.id()));
        QMailFolderIdList affectedFolders = QMailStore::instance()->queryFolders(affectedFolderKey);

        while (!affectedFolders.isEmpty()) {
            QMailFolder childFolder(affectedFolders.takeFirst());
            QString path = childFolder.path();
            path.replace(0, folder.path().length(), newPath);
            childFolder.setPath(path);
            if (!QMailStore::instance()->updateFolder(&childFolder))
                qWarning() << "Unable to locally change path of a subfolder";
        }
    } else {
        name = newPath;
    }

    QMailFolder newFolder = folder;
    newFolder.setPath(newPath);
    newFolder.setParentFolderId(newParentId);

    if (!QMailStore::instance()->updateFolder(&newFolder))
        qWarning() << "Unable to locally move folder";
    if (_inProgress == 0)
         context->operationCompleted();
}

/* A strategy to traverse a list of messages, preparing each one for transmission
   by generating a URLAUTH token.
*/
void ImapPrepareMessagesStrategy::setUnresolved(const QList<QPair<QMailMessagePart::Location, QMailMessagePart::Location> > &ids, bool external)
{
    _locations = ids;
    _external = external;
}

void ImapPrepareMessagesStrategy::newConnection(ImapStrategyContextBase *context)
{
    if (!_external) {
        // We only need internal references - generate IMAP URLs for each location
        while (!_locations.isEmpty()) {
            const QPair<QMailMessagePart::Location, QMailMessagePart::Location> &pair(_locations.first());

            QString url(ImapProtocol::url(pair.first, false, false));
            urlAuthorized(context, url);

            _locations.removeFirst();
        }

        context->operationCompleted();
        return;
    }

    ImapStrategy::newConnection(context);
}

void ImapPrepareMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus)
{
    switch( command ) {
        case IMAP_Login:
        {
            handleLogin(context);
            break;
        }
        
        case IMAP_GenUrlAuth:
        {
            handleGenUrlAuth(context);
            break;
        }
        
        case IMAP_Logout:
        {
            break;
        }
        
        default:
        {
            _error = true;
            qWarning() << "Unhandled IMAP response:" << command;
            break;
        }
    }
}

void ImapPrepareMessagesStrategy::handleLogin(ImapStrategyContextBase *context)
{
    nextMessageAction(context);
}

void ImapPrepareMessagesStrategy::handleGenUrlAuth(ImapStrategyContextBase *context)
{
    // We're finished with the previous location
    _locations.removeFirst();

    nextMessageAction(context);
}

void ImapPrepareMessagesStrategy::nextMessageAction(ImapStrategyContextBase *context)
{
    if (!_locations.isEmpty()) {
        const QPair<QMailMessagePart::Location, QMailMessagePart::Location> &pair(_locations.first());

        // Generate an authorized URL for this message content
        bool bodyOnly(false);
        if (!pair.first.isValid(false)) {
            // This is a full-message reference - for a single-part message, we should forward
            // only the body text; for multi-part we want the whole message
            QMailMessage message(pair.first.containingMessageId());
            if (message.multipartType() == QMailMessage::MultipartNone) {
                bodyOnly = true;
            }
        }
        context->protocol().sendGenUrlAuth(pair.first, bodyOnly);
    } else {
        messageListCompleted(context);
    }
}

void ImapPrepareMessagesStrategy::messageListCompleted(ImapStrategyContextBase *context)
{
    context->operationCompleted();
}

namespace {

struct ReferenceDetector
{
    bool operator()(const QMailMessagePart &part)
    {
        // Return false if there is an unresolved reference to stop traversal
        return ((part.referenceType() == QMailMessagePart::None) || !part.referenceResolution().isEmpty());
    }
};

bool hasUnresolvedReferences(const QMailMessage &message)
{
    // If foreachPart yields false there is at least one unresolved reference
    return (message.foreachPart(ReferenceDetector()) == false);
}

}

void ImapPrepareMessagesStrategy::urlAuthorized(ImapStrategyContextBase *, const QString &url)
{
    const QPair<QMailMessagePart::Location, QMailMessagePart::Location> &pair(_locations.first());

    // We now have an authorized URL for this location
    QMailMessageId referringId(pair.second.containingMessageId());
    if (referringId.isValid()) {
        QMailMessage referer(referringId);
        if (referer.contains(pair.second)) {
            QMailMessagePart &part(referer.partAt(pair.second));

            part.setReferenceResolution(url);

            // Have we resolved all references in this message?
            if (!hasUnresolvedReferences(referer)) {
                referer.setStatus(QMailMessage::HasUnresolvedReferences, false);
            }

            if (!QMailStore::instance()->updateMessage(&referer)) {
                _error = true;
                qWarning() << "Unable to update message for account:" << referer.parentAccountId();
            }
        } else {
            _error = true;
            qWarning() << "Unable to resolve reference to invalid part:" << pair.second.toString(false);
        }
    } else {
        // Update this message with its own location reference
        QMailMessage referencedMessage(pair.first.containingMessageId());
        referencedMessage.setExternalLocationReference(url);

        if (!QMailStore::instance()->updateMessage(&referencedMessage)) {
            _error = true;
            qWarning() << "Unable to update message for account:" << referencedMessage.parentAccountId();
        }
    }
}


/* A strategy that provides an interface for defining a set of messages 
   or message parts to operate on, and an abstract interface messageListMessageAction() 
   for operating on messages.
   
   Also implements logic to determine which messages or message part to operate 
   on next.
*/
void ImapMessageListStrategy::clearSelection()
{
    _selectionMap.clear();
    _folderItr = _selectionMap.end();
}

void ImapMessageListStrategy::selectedMailsAppend(const QMailMessageIdList& ids) 
{
    if (ids.count() == 0)
        return;

    QMailMessageKey::Properties props(QMailMessageKey::Id | QMailDisconnected::parentFolderProperties() | QMailMessageKey::ServerUid);
    foreach (const QMailMessageMetaData &metaData, QMailStore::instance()->messagesMetaData(QMailMessageKey::id(ids), props)) {
        uint serverUid(stripFolderPrefix(metaData.serverUid()).toUInt());
        _selectionMap[QMailDisconnected::sourceFolderId(metaData)].append(MessageSelector(serverUid, metaData.id(), SectionProperties()));
    }
}

void ImapMessageListStrategy::selectedSectionsAppend(const QMailMessagePart::Location &location)
{
    QMailMessageMetaData metaData(location.containingMessageId());
    if (metaData.id().isValid()) {
        uint serverUid(stripFolderPrefix(metaData.serverUid()).toUInt());
        _selectionMap[QMailDisconnected::sourceFolderId(metaData)].append(MessageSelector(serverUid, metaData.id(), SectionProperties(location)));
    }
}

void ImapMessageListStrategy::newConnection(ImapStrategyContextBase *context)
{
    setCurrentMailbox(QMailFolderId());

    ImapStrategy::newConnection(context);
}

void ImapMessageListStrategy::initialAction(ImapStrategyContextBase *context)
{
    resetMessageListTraversal();

    ImapStrategy::initialAction(context);
}

void ImapMessageListStrategy::checkUidValidity(ImapStrategyContextBase *context)
{
    const ImapMailboxProperties &properties(context->mailbox());
    QMailFolder folder(properties.id);
    QString oldUidValidity(folder.customField("qmf-uidvalidity"));
    
    if (!oldUidValidity.isEmpty()
        && !properties.uidValidity.isEmpty()
        && (oldUidValidity != properties.uidValidity)) {
        // uidvalidity has changed
        // mark all messages as removed, reset all folder sync custom fields
        qWarning() << "UidValidity has changed for folder:" << folder.displayName() << "account:" << context->config().id();
        folder.removeCustomField("qmf-min-serveruid");
        folder.removeCustomField("qmf-max-serveruid");
        folder.removeCustomField("qmf-highestmodseq");
        if (!QMailStore::instance()->updateFolder(&folder)) {
            _error = true;
            qWarning() << "Unable to update folder for account:" << context->config().id();
        }
        
        QMailMessageKey removedKey(QMailDisconnected::sourceKey(properties.id));
        if (!purge(context, removedKey)) {
            _error = true;
        }
    }
    if (!properties.uidValidity.isEmpty() &&
        (properties.uidValidity != oldUidValidity)) {
        folder.setCustomField("qmf-uidvalidity", properties.uidValidity);
        if (!QMailStore::instance()->updateFolder(&folder)) {
            _error = true;
            qWarning() << "Unable to update folder for account:" << context->config().id();
        }
    }
}

void ImapMessageListStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus)
{
    switch( command ) {
        case IMAP_Login:
        {
            handleLogin(context);
            break;
        }
        
        case IMAP_QResync: // fall through
        case IMAP_Select:
        {
            checkUidValidity(context);
            handleSelect(context);
            break;
        }
        
        case IMAP_Create:
        {
            handleCreate(context);
            break;
        }
        
        case IMAP_Delete:
        {
            handleDelete(context);
            break;
        }
        
        case IMAP_Rename:
        {
            handleRename(context);
            break;
        }

        case IMAP_Move:
        {
            handleMove(context);
            break;
        }

        case IMAP_Close:
        {
            handleClose(context);
            break;
        }

        case IMAP_Logout:
        {
            break;
        }
        
        default:
        {
            _error = true;
            qWarning() << "Unhandled IMAP response:" << command;
            break;
        }
    }
}

void ImapMessageListStrategy::handleLogin(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapMessageListStrategy::handleSelect(ImapStrategyContextBase *context)
{
    // We're completing a message or section
    messageListMessageAction(context);
}

void ImapMessageListStrategy::handleCreate(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapMessageListStrategy::handleDelete(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapMessageListStrategy::handleRename(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapMessageListStrategy::handleMove(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}


void ImapMessageListStrategy::handleClose(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapMessageListStrategy::resetMessageListTraversal()
{
    _folderItr = _selectionMap.begin();
    if (_folderItr != _selectionMap.end()) {
        FolderSelections &folder(*_folderItr);
        std::sort(folder.begin(), folder.end(), messageSelectorLessThan);

        _selectionItr = folder.begin();
    }
}

bool ImapMessageListStrategy::selectNextMessageSequence(ImapStrategyContextBase *context, int maximum, bool folderActionPermitted)
{
    _messageUids.clear();
    _msgSection = QMailMessagePart::Location();
    _sectionStart = 0;
    _sectionEnd = SectionProperties::All;

    if (!folderActionPermitted) {
        if (messageListFolderActionRequired()) {
            return false;
        }
    }

    if (_folderItr == _selectionMap.end()) {
        messageListCompleted(context);
        return false;
    }
        
    FolderSelections::ConstIterator selectionEnd = _folderItr.value().end();
    while (_selectionItr == selectionEnd) {
        ++_folderItr;
        if (_folderItr == _selectionMap.end())
            break;

        FolderSelections &folder(*_folderItr);
        std::sort(folder.begin(), folder.end(), messageSelectorLessThan);

        _selectionItr = folder.begin();
        selectionEnd = folder.end();
    }

    if ((_folderItr == _selectionMap.end()) || !_folderItr.key().isValid()) {
        setCurrentMailbox(QMailFolderId());
        _selectionMap.clear();
        messageListFolderAction(context);
        return false;
    }

    _transferState = Complete;

    QMailFolderId mailboxId = _folderItr.key();
    if (mailboxId != _currentMailbox.id()) {
        setCurrentMailbox(mailboxId);
        messageListFolderAction(context);
        return false;
    }

    QString mailboxIdStr = QString::number(mailboxId.toULongLong()) + UID_SEPARATOR;

    // Get consecutive full messages
    while ((_messageUids.count() < maximum) &&
           (_selectionItr != selectionEnd) &&
           (_selectionItr->_properties.isEmpty())) {
        _messageUids.append((*_selectionItr).uidString(mailboxIdStr));
        ++_selectionItr;
    }

    if (!_messageUids.isEmpty()) {
        return true;
    }

    // TODO: Get multiple parts for a single message in one roundtrip?
    if (_messageUids.isEmpty() && (_selectionItr != selectionEnd)) {
        const MessageSelector &selector(*_selectionItr);
        ++_selectionItr;

        _messageUids.append(selector.uidString(mailboxIdStr));
        _msgSection = selector._properties._location;
        
        // Determine the start position.
        // Find where we should continue (start) fetching from
        const QMailMessage message(_messageUids.first(), context->config().id());
        if (selector._properties._minimum != SectionProperties::HeadersOnly) {
            bool valid = findFetchContinuationStart(message, _msgSection, &_sectionStart);
            if (!valid) {
                qMailLog(IMAP) << "Could not complete part: invalid location or invalid uid";
                return selectNextMessageSequence(context, maximum, folderActionPermitted);
            }
        }
        // Determine the end position.
        if (selector._properties._minimum == SectionProperties::All) {
            const QMailMessagePart &part(message.partAt(_msgSection));
            _sectionEnd = part.contentDisposition().size();
            if (_sectionStart == 0 || _sectionEnd == -1) {
                // Cannot determine content size. Fetch it all then.
                _sectionStart = 0;
                _sectionEnd = SectionProperties::All;
            }
        } else if (selector._properties._minimum == SectionProperties::HeadersOnly) {
            _sectionEnd = SectionProperties::HeadersOnly;
        } else {
            _sectionEnd = (selector._properties._minimum - 1);
            if (_sectionEnd < 0) {
                qMailLog(IMAP) << "Invalid 'minimum' value: " << selector._properties._minimum;
                return selectNextMessageSequence(context, maximum, folderActionPermitted);
            }
        }

        if (_sectionEnd == SectionProperties::All
            || _sectionEnd == SectionProperties::HeadersOnly) {
            _sectionStart = 0;
        }
        // Try to avoid sending bad IMAP commands even when the server gives bogus values
        if (_sectionEnd >= 0
            && _sectionStart >= _sectionEnd) {
            qMailLog(IMAP) << "Invalid message section range"
                           << "account:" << message.parentAccountId()
                           << "UID:" << message.serverUid()
                           << "start:" << _sectionStart
                           << "end:" << _sectionEnd;
            return selectNextMessageSequence(context, maximum, folderActionPermitted);
        }
    }

    return !_messageUids.isEmpty();
}

bool ImapMessageListStrategy::messageListFolderActionRequired()
{
    return ((_folderItr == _selectionMap.end()) || (_selectionItr == _folderItr.value().end()));
}

void ImapMessageListStrategy::messageListFolderAction(ImapStrategyContextBase *context)
{
    if (_currentMailbox.id().isValid()) {
        if (_currentMailbox.id() == context->mailbox().id) {
            // We already have the appropriate mailbox selected
            handleSelect(context);
        } else if (_currentMailbox.id() == QMailFolder::LocalStorageFolderId) {
            // No folder should be selected
            context->protocol().sendClose();
        } else {
            selectFolder(context, _currentMailbox);
        }
    } else {
        messageListCompleted(context);
    }
}

void ImapMessageListStrategy::messageListCompleted(ImapStrategyContextBase *context)
{
    context->operationCompleted();
}

void ImapMessageListStrategy::setCurrentMailbox(const QMailFolderId &id)
{
    if (id.isValid()) { 
        _currentMailbox = QMailFolder(id);

        // Store the current modification sequence value for this folder, if we have one
        _currentModSeq = _currentMailbox.customField("qmf-highestmodseq");
    } else {
        _currentMailbox = QMailFolder();
        _currentModSeq.clear();
    }
}


/* A strategy which provides an interface for defining a set of messages
   or message parts to retrieve, and logic to retrieve those messages
   or message parts and emit progress notifications.
*/
void ImapFetchSelectedMessagesStrategy::clearSelection()
{
    ImapMessageListStrategy::clearSelection();
    _totalRetrievalSize = 0;
    _listSize = 0;
    _retrievalSize.clear();
}


void ImapFetchSelectedMessagesStrategy::metaDataAnalysis(ImapStrategyContextBase *context,
                                                   const QMailMessagePartContainer &partContainer,
                                                   const QList<QMailMessagePartContainer::Location> &attachmentLocations,
                                                   const QMailMessagePartContainer::Location &signedPartLocation,
                                                   QList<QPair<QMailMessagePart::Location, uint> > &sectionList,
                                                   QList<QPair<QMailMessagePart::Location, int> > &completionSectionList,
                                                   QMailMessagePartContainer::Location &preferredBody,
                                                   uint &bytesLeft)
{
    // Download limit has been exhausted for this message
    if (bytesLeft == 0) {
        return;
    }

    ImapConfiguration imapCfg(context->config());
    QByteArray preferred(imapCfg.preferredTextSubtype().toLatin1());

    // Iterate over all parts, looking for the preferred body,
    // download that first giving preference over all other parts
    if (!preferred.isEmpty() && !preferredBody.isValid()) {
        for (uint i = 0; i < partContainer.partCount(); ++i) {
            const QMailMessagePart part(partContainer.partAt(i));
            const QMailMessageContentDisposition disposition(part.contentDisposition());
            const QMailMessageContentType contentType(part.contentType());
            
            if ((part.partCount() == 0)
                && (!part.partialContentAvailable())
                && (disposition.size() > 0)
                && (contentType.matches("text", preferred))) {
                // There is a preferred text sub-part to retrieve.
                // The preferred text part has priority over other parts so,
                // we put it directly into the main completion list.
                // Text parts may be downloaded partially.
                if (bytesLeft >= (uint)disposition.size()) {
                    completionSectionList.append(qMakePair(part.location(), 0));
                    bytesLeft -= disposition.size();
                } else {
                    completionSectionList.append(qMakePair(part.location(), int(bytesLeft)));
                    bytesLeft = 0;
                }
                preferredBody = part.location();
                break;
            }
        }
    }

    // Otherwise, consider the subparts individually
    for (uint i = 0; i < partContainer.partCount(); ++i) {
        const QMailMessagePart part(partContainer.partAt(i));
        const QMailMessageContentDisposition disposition(part.contentDisposition());

        if (part.location() == signedPartLocation) {
            completionSectionList.append(qMakePair(part.location(),
                                                   SectionProperties::HeadersOnly));
            if (part.location() != preferredBody) {
                sectionList.append(qMakePair(part.location(), 0));
            }
        } else if (part.partCount() > 0) {
            metaDataAnalysis(context, part, attachmentLocations, signedPartLocation,
                             sectionList, completionSectionList,
                             preferredBody, bytesLeft);
        } else if (part.partialContentAvailable()) {
            continue;
        } else if (disposition.size() <= 0) {
            continue;
        } else if (_retrievalSpec == QMailRetrievalAction::Auto
                   && !imapCfg.downloadAttachments()
                   && attachmentLocations.contains(part.location())) {
            continue;
        } else {
            // This is a regular part. Try to download it completely, if it is not the preferred body
            // that is already added to the list.
            if (part.location() != preferredBody) {
                sectionList.append(qMakePair(part.location(), static_cast<uint>(disposition.size())));
            }
        }
    }
}

static bool qMailMessageImapStrategyLessThan(const QPair<QMailMessagePart::Location, uint> &l,
                                             const QPair<QMailMessagePart::Location, uint> &r)
{
    return l.second < r.second;
}

void ImapFetchSelectedMessagesStrategy::prepareCompletionList(
        ImapStrategyContextBase *context,
        const QMailMessage &message,
        QMailMessageIdList &completionList,
        QList<QPair<QMailMessagePart::Location, int> > &completionSectionList)
{
    ImapConfiguration imapCfg(context->config());
    const QList<QMailMessagePartContainer::Location> &attachmentLocations = message.findAttachmentLocations();

    if (message.size() < _headerLimit
        && (_retrievalSpec != QMailRetrievalAction::Auto
            || (attachmentLocations.isEmpty() || imapCfg.downloadAttachments()))
       ) {
        completionList.append(message.id());
    } else {
        const QMailMessageContentType contentType(message.contentType());
        if (contentType.matches("text")) {
            // It is a text part. So, we can retrieve the first
            // portion of it.
            QMailMessagePart::Location location;
            location.setContainingMessageId(message.id());
            completionSectionList.append(qMakePair(location, int(_headerLimit)));
        } else {
            QMailMessagePartContainer::Location signedPartLocation;
            if (message.status() & QMailMessage::HasSignature) {
                const QMailMessagePartContainer *signedContainer =
                    QMailCryptographicServiceFactory::findSignedContainer(&message);
                if (signedContainer && signedContainer->partCount() > 0) {
                    signedPartLocation = signedContainer->partAt(0).location();
                }
            }

            uint bytesLeft = _headerLimit;
            int partsToRetrieve = 0;
            const int maxParts = 100;
            QList<QPair<QMailMessagePart::Location, uint> > sectionList;
            QMailMessagePart::Location preferredBody;
            metaDataAnalysis(context, message, attachmentLocations, signedPartLocation,
                             sectionList, completionSectionList,
                             preferredBody, bytesLeft);

            std::sort(sectionList.begin(), sectionList.end(), qMailMessageImapStrategyLessThan);
            QList<QPair<QMailMessagePart::Location, uint> >::iterator it = sectionList.begin();
            while (it != sectionList.end() && (bytesLeft > 0) && (partsToRetrieve < maxParts)) {
                const QMailMessagePart &part = message.partAt(it->first);
                if (it->second <= (uint)bytesLeft) {
                    completionSectionList.append(qMakePair(it->first, 0));
                    bytesLeft -= it->second;
                    ++partsToRetrieve;
                } else if (part.contentType().matches("text")) {
                    // Text parts can be downloaded partially.
                    completionSectionList.append(qMakePair(it->first, int(bytesLeft)));
                    bytesLeft = 0;
                    ++partsToRetrieve;
                }
                ++it;
            }
        }
    }
}

void ImapFetchSelectedMessagesStrategy::setOperation(
        ImapStrategyContextBase *context,
        QMailRetrievalAction::RetrievalSpecification spec)
{
    QMailAccountConfiguration accountCfg(context->config().id());
    ImapConfiguration imapCfg(accountCfg);
    switch (spec) {
    case QMailRetrievalAction::Auto:
        {
            if (imapCfg.isAutoDownload()) {
                // Just download everything
                _headerLimit = UINT_MAX;
            } else {
                _headerLimit = imapCfg.maxMailSize() * 1024;
            }
        }
        break;
    case QMailRetrievalAction::Content:
        _headerLimit = UINT_MAX;
        break;
    case QMailRetrievalAction::MetaData:
    case QMailRetrievalAction::Flags:
    default:
        _headerLimit = 0;
        break;
    }

    _retrievalSpec = spec;
}

void ImapFetchSelectedMessagesStrategy::selectedMailsAppend(const QMailMessageIdList& ids) 
{
    _listSize += ids.count();
    if (_listSize == 0)
        return;

    QMailMessageKey::Properties props(QMailMessageKey::Id | QMailDisconnected::parentFolderProperties() | QMailMessageKey::ServerUid | QMailMessageKey::Size);

    // Break retrieval of message meta data into chunks to reduce peak memory use
    QMailMessageIdList idsBatch;
    const int batchSize(100);
    int i = 0;
    while (i < ids.count()) {
        idsBatch.clear();
        while ((i < ids.count()) && (idsBatch.count() < batchSize)) {
            idsBatch.append(ids[i]);
            ++i;
        }

        foreach (const QMailMessageMetaData &metaData, QMailStore::instance()->messagesMetaData(QMailMessageKey::id(idsBatch), props)) {
            uint serverUid(stripFolderPrefix(metaData.serverUid()).toUInt());

            QMailFolderId remoteFolderId(QMailDisconnected::sourceFolderId(metaData));

            _selectionMap[remoteFolderId].append(MessageSelector(serverUid, metaData.id(), SectionProperties()));

            uint size = metaData.indicativeSize();
            uint bytes = metaData.size();

            _retrievalSize.insert(metaData.serverUid(), qMakePair(qMakePair(size, bytes), 0u));
            _totalRetrievalSize += size;
        }
    }

    // Report the total size we will retrieve
    _progressRetrievalSize = 0;
}

void ImapFetchSelectedMessagesStrategy::selectedSectionsAppend(const QMailMessagePart::Location &location, int minimum)
{
    _listSize += 1;

    const QMailMessage message(location.containingMessageId());
    if (message.id().isValid()) {
        uint serverUid(stripFolderPrefix(message.serverUid()).toUInt());
        _selectionMap[QMailDisconnected::sourceFolderId(message)].append(MessageSelector(serverUid, message.id(), SectionProperties(location, minimum)));

        if (minimum == SectionProperties::All || minimum >= 0) {
            uint size = 0;
            uint bytes = minimum;

            if (minimum > 0) {
                size = 1;
            } else if (location.isValid() && message.contains(location)) {
                // Find the size of this part
                const QMailMessagePart &part(message.partAt(location));
                size = part.indicativeSize();
                bytes = part.contentDisposition().size();
            }
            // Required to show progress when downloading attachments
            if (size < 1)
                size = bytes/1024;

            _retrievalSize.insert(message.serverUid(), qMakePair(qMakePair(size, bytes), 0u));
            _totalRetrievalSize += size;
        }
    }
}

void ImapFetchSelectedMessagesStrategy::newConnection(ImapStrategyContextBase *context)
{
    _progressRetrievalSize = 0;
    _messageCount = 0;
    _outstandingFetches = 0;

    ImapMessageListStrategy::newConnection(context);
}

void ImapFetchSelectedMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_UIDFetch:
        {
            handleUidFetch(context);
            break;
        }
        
        default:
        {
            ImapMessageListStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapFetchSelectedMessagesStrategy::handleLogin(ImapStrategyContextBase *context)
{
    if (_totalRetrievalSize)
        context->progressChanged(0, _totalRetrievalSize);

    messageListMessageAction(context);
}

void ImapFetchSelectedMessagesStrategy::handleUidFetch(ImapStrategyContextBase *context)
{
    --_outstandingFetches;
    messageListMessageAction(context);
}

void ImapFetchSelectedMessagesStrategy::messageListMessageAction(ImapStrategyContextBase *context)
{
    if (!_outstandingFetches && messageListFolderActionRequired()) {
        // We need to change folders
        selectNextMessageSequence(context, 1, true);
        return;
    }

    _messageCountIncremental = _messageCount;

    while (selectNextMessageSequence(context, DefaultBatchSize, false)) {
        _messageCount += _messageUids.count();

        QString msgSectionStr;
        if (_msgSection.isValid()) {
            msgSectionStr = _msgSection.toString(false);
        }

        if (_msgSection.isValid() && _sectionEnd == SectionProperties::HeadersOnly) {
            context->protocol().sendUidFetchSectionHeader(numericUidSequence(_messageUids), msgSectionStr);
        } else if (_msgSection.isValid() || (_sectionEnd != SectionProperties::All)) {
            context->protocol().sendUidFetchSection(numericUidSequence(_messageUids), msgSectionStr, _sectionStart, _sectionEnd);
        } else {
            context->protocol().sendUidFetch(ContentFetchFlags, numericUidSequence(_messageUids));
        }

        ++_outstandingFetches;
        if (_outstandingFetches >= MaxPipeliningDepth)
            break;
    }
}

void ImapFetchSelectedMessagesStrategy::downloadSize(ImapStrategyContextBase *context, const QString &uid, int length)
{
    if (!uid.isEmpty()) {
        RetrievalMap::iterator it = _retrievalSize.find(uid);
        if (it != _retrievalSize.end()) {
            QPair<QPair<uint, uint>, uint> &values = it.value();

            // Calculate the percentage of the retrieval completed
            uint totalBytes = values.first.second;
            uint percentage = totalBytes ? qMin<uint>(length * 100 / totalBytes, 100) : 100;

            if (percentage > values.second) {
                values.second = percentage;

                // Update the progress figure to count the retrieved portion of this message
                uint partialSize = values.first.first * percentage / 100;
                context->progressChanged(_progressRetrievalSize + partialSize, _totalRetrievalSize);
            }
        }
    }
}

bool ImapFetchSelectedMessagesStrategy::messageFetched(ImapStrategyContextBase *context, QMailMessage &message)
{ 
    bool result = ImapMessageListStrategy::messageFetched(context, message);

    itemFetched(context, message.serverUid());
    return result;
}

void ImapFetchSelectedMessagesStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    ImapMessageListStrategy::messageFlushed(context, message);
    if (_error) return;
}

void ImapFetchSelectedMessagesStrategy::dataFetched(ImapStrategyContextBase *context, QMailMessage &message, const QString &uid, const QString &section)
{ 
    ImapMessageListStrategy::dataFetched(context, message, uid, section);

    itemFetched(context, message.serverUid());
}

void ImapFetchSelectedMessagesStrategy::dataFlushed(ImapStrategyContextBase *context, QMailMessage &message, const QString &uid, const QString &section)
{ 
    ImapMessageListStrategy::dataFlushed(context, message, uid, section);
    if (_error) return;
}

void ImapFetchSelectedMessagesStrategy::itemFetched(ImapStrategyContextBase *context, const QString &uid)
{ 
    RetrievalMap::iterator it = _retrievalSize.find(uid);
    if (it != _retrievalSize.end()) {
        // Update the progress figure
        _progressRetrievalSize += it.value().first.first;
        context->progressChanged(_progressRetrievalSize, _totalRetrievalSize);

        it = _retrievalSize.erase(it);
    }

    if (_listSize) {
        int count = qMin(++_messageCountIncremental + 1, _listSize);
        context->updateStatus(QObject::tr("Completing %1 / %2").arg(count).arg(_listSize));
    }
}
/* A strategy to search all folders */

void ImapSearchMessageStrategy::searchArguments(const QMailMessageKey &searchCriteria, const QString &bodyText, quint64 limit, const QMailMessageSortKey &sort, bool count)
{
    SearchData search;
    search.criteria = searchCriteria;
    search.bodyText = bodyText;
    search.limit = limit;
    search.sort = sort;
    search.count = count;

    _searches.append(search);
    _canceled = false;
}

void ImapSearchMessageStrategy::cancelSearch()
{
    _searches.clear();
    _canceled = true;
    _limit = -1;
    _count = false;
}

void ImapSearchMessageStrategy::transition(ImapStrategyContextBase *c, ImapCommand cmd, OperationStatus status)
{
    switch(cmd) {
    case IMAP_Search_Message:
        handleSearchMessage(c);
        break;
    default:
        ImapRetrieveFolderListStrategy::transition(c, cmd, status);
    }
}

void ImapSearchMessageStrategy::folderListCompleted(ImapStrategyContextBase *context)
{
    _mailboxList = context->client()->mailboxIds();

    ImapRetrieveFolderListStrategy::folderListCompleted(context);
    if(_currentMailbox.id().isValid()) {
        _searches.removeFirst();
        _limit = -1;
        _count = false;
    } else {
        QSet<QMailFolderId> accountFolders(_mailboxList.toSet());

        QMailFolderIdList foldersToSearch(foldersApplicableTo(_searches.first().criteria, accountFolders).toList());

        if (foldersToSearch.isEmpty()) {
            ImapRetrieveFolderListStrategy::folderListCompleted(context);
        } else {
            selectedFoldersAppend(foldersToSearch);
            processNextFolder(context);
        }
    }
}

void ImapSearchMessageStrategy::folderListFolderAction(ImapStrategyContextBase *context)
{
    if(_canceled)
        return; //stop it searching

    SearchData search(_searches.first());
    _limit = search.limit;
    _count = search.count;
    context->protocol().sendSearchMessages(search.criteria, search.bodyText, search.sort, _count);
}

bool ImapSearchMessageStrategy::messageFetched(ImapStrategyContextBase *context, QMailMessage &message)
{
    if(_canceled)
        return false;

    message.setStatus(QMailMessage::Temporary, true);
    return ImapRetrieveFolderListStrategy::messageFetched(context, message);
}

void ImapSearchMessageStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    ImapRetrieveFolderListStrategy::messageFlushed(context, message);
    if (_error) return;

    _fetchedList.append(message.id());
}

void ImapSearchMessageStrategy::handleUidFetch(ImapStrategyContextBase *context)
{
    if(_canceled)
        return;

    QMailMessageBuffer::instance()->flush();
    context->matchingMessageIds(_fetchedList);
    _fetchedList.clear();

    processNextFolder(context);
}

void ImapSearchMessageStrategy::handleSearchMessage(ImapStrategyContextBase *context)
{
    if(_canceled)
        return;

    const ImapMailboxProperties &properties(context->mailbox());
    QList<QMailMessageId> searchResults;
    IntegerRegion uidsToFetch;

    if (_count) {
        context->messagesCount(properties.searchCount);
        processNextFolder(context);
        return;
    }

    foreach(const QString &uidString, properties.uidList) {
        QMailMessageIdList ids(QMailStore::instance()->queryMessages(QMailMessageKey::serverUid(uidString) & QMailMessageKey::parentAccountId(context->config().id())));
        Q_ASSERT(ids.size() == 1 || ids.size() == 0);
        if (ids.size())
            searchResults.append(ids.first());
        else
            uidsToFetch.add(stripFolderPrefix(uidString).toInt());
    }

    context->messagesCount(properties.searchCount);

    if(!searchResults.isEmpty())
        context->matchingMessageIds(searchResults);

    int limit(_limit);
    context->remainingMessagesCount(qMax(0, int(uidsToFetch.cardinality()) - limit));

    if (limit) {
        QStringList uids = uidsToFetch.toStringList();
        int start = qMax(0, uids.count() - limit);
        if (start < uids.count()) {
            uidsToFetch = IntegerRegion(uids.mid(start, -1));
        } else {
            uidsToFetch.clear();
        }
    }

    if(uidsToFetch.isEmpty())
        processNextFolder(context);
    else
        context->protocol().sendUidFetch(MetaDataFetchFlags, uidsToFetch.toString());
}

void ImapSearchMessageStrategy::messageListCompleted(ImapStrategyContextBase *context)
 {
    if(_currentMailbox.id().isValid()) {
        context->operationCompleted();
    }
 }

/* A strategy that provides an interface for processing a set of folders.
*/
void ImapFolderListStrategy::clearSelection()
{
    ImapFetchSelectedMessagesStrategy::clearSelection();
    _mailboxIds.clear();
}

void ImapFolderListStrategy::selectedFoldersAppend(const QMailFolderIdList& ids) 
{
    _mailboxIds += ids;
    _processable += ids.count();
}

void ImapFolderListStrategy::newConnection(ImapStrategyContextBase *context)
{
    _folderStatus.clear();

    ImapFetchSelectedMessagesStrategy::newConnection(context);
}

void ImapFolderListStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_List:
        {
            handleList(context);
            break;
        }
        
        case IMAP_Search:
        {
            handleSearch(context);
            break;
        }
        
        default:
        {
            ImapFetchSelectedMessagesStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapFolderListStrategy::handleLogin(ImapStrategyContextBase *context)
{
    _transferState = List;

    processNextFolder(context);
}

void ImapFolderListStrategy::handleList(ImapStrategyContextBase *context)
{
    if (!_currentMailbox.id().isValid()) {
        // Try to proceed to another mailbox
        processNextFolder(context);
    } else {
        // If the current folder is not yet selected
        if (_currentMailbox.id() != context->mailbox().id) {
            if (_folderStatus.contains(_currentMailbox.id())) {
                FolderStatus folderState = _folderStatus[_currentMailbox.id()];
                if (folderState & NoSelect) {
                    // We can't select this folder
                    processNextFolder(context);
                } else {
                    // Select this folder
                    selectFolder(context,  _currentMailbox );
                    return;
                }
            } else {
                // This folder does not exist, according to the listing...
                processNextFolder(context);
            }
        } else {
            // This mailbox is selected
            folderListFolderAction(context);
        }
    }
}

void ImapFolderListStrategy::handleSelect(ImapStrategyContextBase *context)
{
    if (_transferState == List) {
        // We have selected this folder - find out how many undiscovered messages exist
        const ImapMailboxProperties &properties(context->mailbox());

        if (properties.exists == 0) {
            // No messages - nothing to discover...
        } else {
            // If CONDSTORE is available, we may know that no changes have occurred
            if (!properties.noModSeq && (properties.highestModSeq == _currentModSeq)) {
                // No changes have occurred - bypass searching entirely
            } else {
                // Find the UID of the most recent message we have previously discovered in this folder
                QMailFolder folder(properties.id);
                quint32 clientMaxUid(folder.customField("qmf-max-serveruid").toUInt());

                if (clientMaxUid == 0) {
                    // We don't have any messages for this folder - fall through to handleSearch,
                    // where undiscoveredCount will be set to properties.exists
                } else {
                    if (properties.uidNext <= (clientMaxUid + 1)) {
                        // There's no need to search because we already have the highest UID
                    } else {
                        // Start our search above the current highest UID
                        context->protocol().sendSearch(0, QString("UID %1:%2").arg(clientMaxUid + 1).arg(properties.uidNext));
                        return;
                    }
                }
            }
        }

        handleSearch(context);
    } else {
        ImapMessageListStrategy::handleSelect(context);
    }
}

void ImapFolderListStrategy::handleSearch(ImapStrategyContextBase *context)
{
    updateUndiscoveredCount(context);

    // We have finished with this folder
    folderListFolderAction(context);
}

void ImapFolderListStrategy::folderListFolderAction(ImapStrategyContextBase *context)
{
    // The current mailbox is now selected - subclasses clients should do something now

    // Go onto the next mailbox
    processNextFolder(context);
}

void ImapFolderListStrategy::processNextFolder(ImapStrategyContextBase *context)
{
    if (nextFolder()) {
        processFolder(context);
        return;
    }

    folderListCompleted(context);
}

bool ImapFolderListStrategy::nextFolder()
{
    while (!_mailboxIds.isEmpty()) {
        QMailFolderId folderId(_mailboxIds.takeFirst());

        // Process this folder
        setCurrentMailbox(folderId);

        // Bypass any folder for which synchronization is disabled
        if (synchronizationEnabled(_currentMailbox))
            return true;
    }

    return false;
}

void ImapFolderListStrategy::processFolder(ImapStrategyContextBase *context)
{
    QMailFolderId folderId = _currentMailbox.id();
    if(_folderStatus.contains(folderId) && _folderStatus[folderId] & NoSelect)
        context->protocol().sendList(_currentMailbox, QString('%'));
    else
        selectFolder(context, _currentMailbox);

    context->progressChanged(++_processed, _processable);
}

bool ImapFolderListStrategy::synchronizationEnabled(const QMailFolder &folder) const 
{
    return folder.status() & QMailFolder::SynchronizationEnabled;
}

void ImapFolderListStrategy::folderListCompleted(ImapStrategyContextBase *context)
{
    // We have retrieved all the folders - process any messages
    messageListMessageAction(context);
}

void ImapFolderListStrategy::mailboxListed(ImapStrategyContextBase *context, QMailFolder &folder, const QString &flags)
{
    ImapStrategy::mailboxListed(context, folder, flags);

    if (folder.id().isValid()) {
        // Record the status of the listed mailbox
        int status = 0;
        if (flags.indexOf("\\NoInferiors", 0, Qt::CaseInsensitive) != -1)
            status |= NoInferiors;
        if (flags.indexOf("\\NoSelect", 0, Qt::CaseInsensitive) != -1)
            status |= NoSelect;
        if (flags.indexOf("\\Marked", 0, Qt::CaseInsensitive) != -1)
            status |= Marked;
        if (flags.indexOf("\\Unmarked", 0, Qt::CaseInsensitive) != -1)
            status |= Unmarked;
        if (flags.indexOf("\\HasChildren", 0, Qt::CaseInsensitive) != -1)
            status |= HasChildren;
        if (flags.indexOf("\\HasNoChildren", 0, Qt::CaseInsensitive) != -1)
            status |= HasNoChildren;

        _folderStatus[folder.id()] = static_cast<FolderStatus>(status);
    }
}

void ImapFolderListStrategy::updateUndiscoveredCount(ImapStrategyContextBase *context)
{
    const ImapMailboxProperties &properties(context->mailbox());

    // Initial case set the undiscovered count to exists in the case of no 
    // max-serveruid set for the folder
    int undiscovered(properties.exists);

    QMailFolder folder(_currentMailbox.id());
    int clientMax(folder.customField("qmf-max-serveruid").toUInt());
    if (clientMax) {
        // The undiscovered count for a folder is the number of messages on the server newer 
        // than the most recent (highest server uid) message in the folder.
        undiscovered = properties.msnList.count();
    }
    
    if (uint(undiscovered) != folder.serverUndiscoveredCount()) {
        folder.setServerUndiscoveredCount(undiscovered);

        if (!QMailStore::instance()->updateFolder(&folder)) {
            _error = true;
            qWarning() << "Unable to update folder for account:" << context->config().id();
        }
    }
}


/* An abstract strategy. To be used as a base class for strategies that 
   iterate over mailboxes Previewing and Completing discovered mails.
*/
void ImapSynchronizeBaseStrategy::newConnection(ImapStrategyContextBase *context)
{
    _retrieveUids.clear();

    ImapFolderListStrategy::newConnection(context);
}

void ImapSynchronizeBaseStrategy::handleLogin(ImapStrategyContextBase *context)
{
    _completionList.clear();
    _completionSectionList.clear();

    ImapFolderListStrategy::handleLogin(context);
}

void ImapSynchronizeBaseStrategy::handleSelect(ImapStrategyContextBase *context)
{
    // We have selected the current mailbox
    if (_transferState == Preview) {
        fetchNextMailPreview(context);
    } else if (_transferState == Complete) {
        // We're completing a message or section
        messageListMessageAction(context);
    } else {
        ImapFolderListStrategy::handleSelect(context);
    }
}

void ImapSynchronizeBaseStrategy::handleUidFetch(ImapStrategyContextBase *context)
{
    if (_transferState == Preview) {    //getting headers
        --_outstandingPreviews;
        if (!_outstandingPreviews) {
            // Flush any pending messages now so that _completionList is up to date
            QMailMessageBuffer::instance()->flush();
        }
        fetchNextMailPreview(context);
    } else if (_transferState == Complete) {
        ImapFetchSelectedMessagesStrategy::handleUidFetch(context);
    }
}

void ImapSynchronizeBaseStrategy::processUidSearchResults(ImapStrategyContextBase *)
{
    _error = true;
    qWarning() << "ImapSynchronizeBaseStrategy::processUidSearchResults: Unexpected location!";
}

void ImapSynchronizeBaseStrategy::previewDiscoveredMessages(ImapStrategyContextBase *context)
{
    // Process our list of all messages to be retrieved for each mailbox
    _total = 0;
    QList<QPair<QMailFolderId, QStringList> >::const_iterator it = _retrieveUids.begin(), end = _retrieveUids.end();
    for ( ; it != end; ++it)
        _total += it->second.count();

    if (_total) {
        context->updateStatus(QObject::tr("Previewing", "Previewing <number of messages>") + QChar(' ') + QString::number(_total));
    }

    _progress = 0;
    context->progressChanged(_progress, _total);

    _transferState = Preview;

    if (!selectNextPreviewFolder(context)) {
        // Could be no mailbox has been selected to be stored locally
        messageListCompleted(context);
    }
}

bool ImapSynchronizeBaseStrategy::selectNextPreviewFolder(ImapStrategyContextBase *context)
{
    if (_retrieveUids.isEmpty()) {
        setCurrentMailbox(QMailFolderId());
        _newUids = QStringList();
        return false;
    }

    // In preview mode, select the mailboxes where retrievable messages are located
    QPair<QMailFolderId, QStringList> next = _retrieveUids.takeFirst();
    setCurrentMailbox(next.first);

    _newUids = next.second;
    _outstandingPreviews = 0;
    
    FolderStatus folderState = _folderStatus[_currentMailbox.id()];
    if (folderState & NoSelect) {
        // Bypass the select and UID search, and go directly to the search result handler
        processUidSearchResults(context);
    } else {
        if (_currentMailbox.id() == context->mailbox().id) {
            // We already have the appropriate mailbox selected
            fetchNextMailPreview(context);
        } else {
            if (_transferState == List) {
                QString status = QObject::tr("Checking", "Checking <mailbox name>") + QChar(' ') + _currentMailbox.displayName();
                context->updateStatus( status );
            }

            selectFolder(context,  _currentMailbox );
        }
    }

    return true;
}

void ImapSynchronizeBaseStrategy::fetchNextMailPreview(ImapStrategyContextBase *context)
{
    if (!_newUids.isEmpty() || _outstandingPreviews) {
        while (!_newUids.isEmpty()) {
            QStringList uidList;
            foreach(const QString &s, _newUids.mid(0, DefaultBatchSize)) {
                uidList << ImapProtocol::uid(s);
            }

            context->protocol().sendUidFetch(MetaDataFetchFlags, IntegerRegion(uidList).toString());
            ++_outstandingPreviews;

            _newUids = _newUids.mid(uidList.count());
            if (_outstandingPreviews > MaxPipeliningDepth)
                break;
        }
    } else {
        // We have previewed all messages from the current folder
        folderPreviewCompleted(context);

        if (!selectNextPreviewFolder(context)) {
            // No more messages to preview
            if ((_transferState == Preview) || (_retrieveUids.isEmpty())) {
                if (!_completionList.isEmpty() || !_completionSectionList.isEmpty()) {
                    // Fetch the messages that need completion
                    clearSelection();

                    selectedMailsAppend(_completionList);

                    QList<QPair<QMailMessagePart::Location, int> >::const_iterator it = _completionSectionList.begin(), end = _completionSectionList.end();
                    for ( ; it != end; ++it) {
                        if (it->second != 0) {
                            selectedSectionsAppend(it->first, it->second);
                        } else {
                            selectedSectionsAppend(it->first);
                        }
                    }

                    _completionList.clear();
                    _completionSectionList.clear();

                    resetMessageListTraversal();
                    messageListMessageAction(context);
                } else {
                    // No messages to be completed
                    messageListCompleted(context);
                }
            }
        }
    }
}

void ImapSynchronizeBaseStrategy::folderPreviewCompleted(ImapStrategyContextBase *)
{
}

bool ImapSynchronizeBaseStrategy::messageFetched(ImapStrategyContextBase *context, QMailMessage &message)
{ 
    bool result = ImapFolderListStrategy::messageFetched(context, message);
    if (_transferState == Preview)
        context->progressChanged(_progress++, _total);
    return result;
}

void ImapSynchronizeBaseStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    ImapFolderListStrategy::messageFlushed(context, message);
    if (_error) {
        return;
    }

    if ((_transferState == Preview) && (_headerLimit > 0)) {
        prepareCompletionList(context, message, _completionList, _completionSectionList);
    }
}


/* A strategy to fetch the list of folders available at the server
*/
void ImapRetrieveFolderListStrategy::setBase(const QMailFolderId &folderId)
{
    _baseId = folderId;
}

void ImapRetrieveFolderListStrategy::setQuickList(bool quickList)
{
    // Ideally clients wouldn't request listing all folders in an account as some accounts can be huge
    // but if the client does request this then do it as efficiently as possible
    // Doesn't work for search, synchronizeAll and retrieveAll subclasses.
    _quickList = quickList;
}

void ImapRetrieveFolderListStrategy::setDescending(bool descending)
{
    _descending = descending;
}

void ImapRetrieveFolderListStrategy::newConnection(ImapStrategyContextBase *context)
{
    _mailboxList.clear();
    _ancestorPaths.clear();

    ImapSynchronizeBaseStrategy::newConnection(context);
}

void ImapRetrieveFolderListStrategy::handleLogin(ImapStrategyContextBase *context)
{
    context->updateStatus( QObject::tr("Retrieving folders") );
    _mailboxPaths.clear();

    QMailFolderId folderId;

    ImapConfiguration imapCfg(context->config());
    if (_baseId.isValid()) {
        folderId = _baseId;
    }

    _transferState = List;

    if (folderId.isValid()) {
        // Begin processing with the specified base folder
        selectedFoldersAppend(QMailFolderIdList() << folderId);
        ImapSynchronizeBaseStrategy::handleLogin(context);
    } else {
        // We need to search for folders at the account root
        if (_quickList) {
            context->protocol().sendList(QMailFolder(), QString('*'));
        } else {
            context->protocol().sendList(QMailFolder(), QString('%'));
        }
    }
}

void ImapRetrieveFolderListStrategy::handleSearch(ImapStrategyContextBase *context)
{
    updateUndiscoveredCount(context);

    // Don't bother listing mailboxes that have no children
    FolderStatus folderState = _folderStatus[_currentMailbox.id()];
    if (!(folderState & NoInferiors) && !(folderState & HasNoChildren)) {
        // Find the child folders of this mailbox
        context->protocol().sendList(_currentMailbox, QString('%'));
    } else {
        folderListFolderAction(context);
    }
}

void ImapRetrieveFolderListStrategy::handleList(ImapStrategyContextBase *context)
{
    if (!_currentMailbox.id().isValid()) {
        // See if there are any more ancestors to search
        if (!_ancestorSearchPaths.isEmpty()) {
            QMailFolder ancestor;
            ancestor.setPath(_ancestorSearchPaths.takeFirst());

            context->protocol().sendList(ancestor, QString('%'));
            return;
        }
    }

    ImapSynchronizeBaseStrategy::handleList(context);
}

void ImapRetrieveFolderListStrategy::folderListCompleted(ImapStrategyContextBase *context)
{
    // We should have discovered all available mailboxes now
    _mailboxList = context->client()->mailboxIds();

    removeDeletedMailboxes(context);

    // We have retrieved all the folders - process any messages
    messageListMessageAction(context);
}

void ImapRetrieveFolderListStrategy::mailboxListed(ImapStrategyContextBase *context, QMailFolder &folder, const QString &flags)
{
    ImapSynchronizeBaseStrategy::mailboxListed(context, folder, flags);

    _mailboxPaths.append(folder.path());

    if (_descending) {
        QString path(folder.path());

        if (folder.id().isValid()) {
            if (folder.id() != _currentMailbox.id()) {
                if (_baseFolder.isEmpty() || 
                    (path.startsWith(_baseFolder, Qt::CaseInsensitive) && (path.length() == _baseFolder.length())) ||
                    (path.startsWith(_baseFolder + context->protocol().delimiter(), Qt::CaseInsensitive))) {
                    // We need to list this folder's contents, too
                    if (!_quickList) {
                        selectedFoldersAppend(QMailFolderIdList() << folder.id());
                    }
                }
            }
        } else {
            if (!_ancestorPaths.contains(path)) {
                if (_baseFolder.startsWith(path + context->protocol().delimiter(), Qt::CaseInsensitive)) {
                    // This folder must be an ancestor of the base folder - list its contents
                    _ancestorPaths.insert(path);
                    _ancestorSearchPaths.append(path);
                }
            }
        }
    }
}

void ImapRetrieveFolderListStrategy::removeDeletedMailboxes(ImapStrategyContextBase *context)
{
    // Do we have the full list of account mailboxes?
    if (_descending && !_baseId.isValid()) {
        // Find the local mailboxes that no longer exist on the server
        QMailFolderIdList nonexistent;
        foreach (const QMailFolderId &boxId, _mailboxList) {
            QMailFolder mailbox(boxId);
            bool exists = _mailboxPaths.contains(mailbox.path());
            if (!exists) {
                nonexistent.append(mailbox.id());
            }
        }

        foreach (const QMailFolderId &boxId, nonexistent) {
            // Any messages in this box should be removed also
            foreach (const QString& uid, context->client()->serverUids(boxId)) {
                // We might have a deletion record for this message
                QMailStore::instance()->purgeMessageRemovalRecords(context->config().id(), QStringList() << uid);
            }

            if (!QMailStore::instance()->removeFolder(boxId)) {
                _error = true;
                qWarning() << "Unable to remove nonexistent folder for account:" << context->config().id();
            }

            _mailboxList.removeAll(boxId);
        }
    }
}

/* A strategy to provide full account synchronization. 
   
   That is to export and import changes, to retrieve message previews for all 
   known messages in an account, and to complete messages where appropriate.
*/
ImapSynchronizeAllStrategy::ImapSynchronizeAllStrategy()
{
    _options = static_cast<Options>(RetrieveMail | ImportChanges | ExportChanges);
}

void ImapSynchronizeAllStrategy::setOptions(Options options)
{
    _options = options;
}

void ImapSynchronizeAllStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_UIDSearch:
        {
            handleUidSearch(context);
            break;
        }
        
        case IMAP_UIDStore:
        {
            handleUidStore(context);
            break;
        }

        case IMAP_Expunge:
        {
            handleExpunge(context);
            break;
        }

        default:
        {
            ImapRetrieveFolderListStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapSynchronizeAllStrategy::handleUidSearch(ImapStrategyContextBase *context)
{
    const ImapMailboxProperties &properties(context->mailbox());

    switch(_searchState)
    {
    case Seen:
    {
        _seenUids = properties.uidList;

        // The Unseen search command was pipelined
        _searchState = Unseen;
        context->protocol().sendUidSearch(MFlag_Unseen);
        break;
    }
    case Unseen:
    {
        _unseenUids = properties.uidList;

        // The Flagged search command was pipelined
        _searchState = Flagged;
        context->protocol().sendUidSearch(MFlag_Flagged);
        break;
    }
    case Flagged:
    {
        _flaggedUids = properties.uidList;
        if (static_cast<quint32>((_unseenUids.count() + _seenUids.count())) == properties.exists) {
            // We have a consistent set of search results
            processUidSearchResults(context);
        } else {
            qMailLog(IMAP) << "Inconsistent UID SEARCH result using SEEN/UNSEEN; reverting to ALL";

            // Try doing a search for ALL messages
            _unseenUids.clear();
            _seenUids.clear();
            _flaggedUids.clear();
            _searchState = All;
            context->protocol().sendUidSearch(MFlag_All);
        }
        break;
    }
    case All:
    {
        _unseenUids = properties.uidList;
        if (static_cast<quint32>(_unseenUids.count()) != properties.exists) {
            qMailLog(IMAP) << "Inconsistent UID SEARCH result";

            // No consistent search result, so don't delete anything
            _searchState = Inconclusive;
        }

        processUidSearchResults(context);
        break;
    }
    default:
        qMailLog(IMAP) << "Unknown search status in transition";
    }
}

void ImapSynchronizeAllStrategy::handleUidStore(ImapStrategyContextBase *context)
{
    if (!(_options & ExportChanges)) {
        processNextFolder(context);
        return;
    }

    if (!_storedReadUids.isEmpty()) {
        // Mark messages as read locally that have been marked as read on the server
        QMailMessageKey readKey(context->client()->messagesKey(_currentMailbox.id()) & QMailMessageKey::serverUid(_storedReadUids));
        if (QMailStore::instance()->updateMessagesMetaData(readKey, QMailMessage::ReadElsewhere, true)) {
            _storedReadUids.clear();
        } else {
            _error = true;
            qWarning() << "Unable to update marked as read message metadata for account:" << context->config().id() << "folder" << _currentMailbox.id();
        }
    }
    if (!_storedUnreadUids.isEmpty()) {
        // Mark messages as unread locally that have been marked as unread on the server
        QMailMessageKey unreadKey(context->client()->messagesKey(_currentMailbox.id()) & QMailMessageKey::serverUid(_storedUnreadUids));
        if (QMailStore::instance()->updateMessagesMetaData(unreadKey, QMailMessage::ReadElsewhere, false)) {
            _storedUnreadUids.clear();
        } else {
            _error = true;
            qWarning() << "Unable to update marked as unread message metadata for account:" << context->config().id() << "folder" << _currentMailbox.id();
        }
    }
    if (!_storedImportantUids.isEmpty()) {
        // Mark messages as important locally that have been marked as important on the server
        QMailMessageKey importantKey(context->client()->messagesKey(_currentMailbox.id()) & QMailMessageKey::serverUid(_storedImportantUids));
        if (QMailStore::instance()->updateMessagesMetaData(importantKey, QMailMessage::ImportantElsewhere, true)) {
            _storedImportantUids.clear();
        } else {
            _error = true;
            qWarning() << "Unable to update marked as important message metadata for account:" << context->config().id() << "folder" << _currentMailbox.id();
        }
    }
    if (!_storedUnimportantUids.isEmpty()) {
        // Mark messages as unimportant locally that have been marked as unimportant on the server
        QMailMessageKey unimportantKey(context->client()->messagesKey(_currentMailbox.id()) & QMailMessageKey::serverUid(_storedUnimportantUids));
        if (QMailStore::instance()->updateMessagesMetaData(unimportantKey, QMailMessage::ImportantElsewhere, false)) {
            _storedUnimportantUids.clear();
        } else {
            _error = true;
            qWarning() << "Unable to update marked as unimportant message metadata for account:" << context->config().id() << "folder" << _currentMailbox.id();
        }
    }

    if (!setNextSeen(context) && !setNextNotSeen(context) && !setNextImportant(context) && !setNextNotImportant(context) && !setNextDeleted(context)) {
        if (!_storedRemovedUids.isEmpty()) {
            // Remove records of deleted messages, after EXPUNGE
            if (QMailStore::instance()->purgeMessageRemovalRecords(context->config().id(), _storedRemovedUids)) {
                _storedRemovedUids.clear();
            } else {
                _error = true;
                qWarning() << "Unable to purge message record for account:" << context->config().id() << "folder" << _currentMailbox.id();
            }
        }
        
        processNextFolder(context);
    }
}

void ImapSynchronizeAllStrategy::handleExpunge(ImapStrategyContextBase *context)
{
    processNextFolder(context);
}

void ImapSynchronizeAllStrategy::folderListFolderAction(ImapStrategyContextBase *context)
{
    _seenUids = QStringList();
    _unseenUids = QStringList();
    _flaggedUids = QStringList();
    _readUids = QStringList();
    _unreadUids = QStringList();
    _importantUids = QStringList();
    _unimportantUids = QStringList();

    _removedUids = QStringList();
    _expungeRequired = false;
    
    // Search for messages in the current mailbox
    _searchState = Seen;

    if (context->mailbox().exists > 0) {
        // Start by looking for previously-seen and unseen messages
        context->protocol().sendUidSearch(MFlag_Seen);
    } else {
        // No messages, so no need to perform search
        processUidSearchResults(context);
    }
}

void ImapSynchronizeAllStrategy::folderListCompleted(ImapStrategyContextBase *context)
{
    // We should have discovered all available mailboxes now
    _mailboxList = context->client()->mailboxIds();

    removeDeletedMailboxes(context);

    previewDiscoveredMessages(context);
}

void ImapSynchronizeAllStrategy::processUidSearchResults(ImapStrategyContextBase *context)
{
    QMailFolderId boxId = _currentMailbox.id();
    QMailMessageKey accountKey(QMailMessageKey::parentAccountId(context->config().id()));
    QMailMessageKey partialContentKey(QMailMessageKey::status(QMailMessage::PartialContentAvailable));
    QMailFolder folder(boxId);
    
    if ((_currentMailbox.status() & QMailFolder::SynchronizationEnabled) &&
        !(_currentMailbox.status() & QMailFolder::Synchronized)) {
        // We have just synchronized this folder
        folder.setStatus(QMailFolder::Synchronized, true);
    }
    if (!QMailStore::instance()->updateFolder(&folder)) {
        _error = true;
        qWarning() << "Unable to update folder for account:" << context->config().id();
    }

    // Check if any of the message is flagged as deleted on server side and not expunged yet
    QStringList flaggedAsRemovedUids = flaggedAsDeletedUids(context);

    // Messages reported as being on the server
    QStringList reportedOnServerUids = _seenUids + _unseenUids;

    // Messages (with at least metadata) stored locally
    QMailMessageKey folderKey(context->client()->messagesKey(boxId) | context->client()->trashKey(boxId));
    QStringList deletedUids = context->client()->deletedMessages(boxId);
    QMailMessageKey storedKey((accountKey & folderKey & partialContentKey) | QMailMessageKey::serverUid(deletedUids) );

    // New messages reported by the server that we don't yet have
    if (_options & RetrieveMail) {
        // Opportunity for optimization here

        QStringList newUids(inFirstButNotSecond(reportedOnServerUids, context->client()->serverUids(storedKey)));
        if (!newUids.isEmpty()) {
            // Add this folder to the list to retrieve from later
            _retrieveUids.append(qMakePair(boxId, newUids));
        }
    }

    if (_searchState == Inconclusive) {
        // Don't mark or delete any messages without a correct server listing
        searchInconclusive(context);
    } else {
        QMailMessageKey readStatusKey(QMailMessageKey::status(QMailMessage::ReadElsewhere, QMailDataComparator::Includes));
        QMailMessageKey removedStatusKey(QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Includes));
        QMailMessageKey unreadElsewhereKey(folderKey & accountKey & ~readStatusKey);
        QMailMessageKey unavailableKey(folderKey & accountKey & removedStatusKey);
        QMailMessageKey unseenKey(QMailMessageKey::serverUid(_unseenUids));
        QMailMessageKey seenKey(QMailMessageKey::serverUid(_seenUids));
        QMailMessageKey flaggedKey(QMailMessageKey::serverUid(_flaggedUids));
        QMailMessageKey importantElsewhereKey(QMailMessageKey::status(QMailMessage::ImportantElsewhere, QMailDataComparator::Includes));
        QMailMessageKey flaggedAsRemoved(QMailMessageKey::serverUid(flaggedAsRemovedUids));
        
        // Only delete messages the server still has
        _removedUids = inFirstAndSecond(deletedUids, reportedOnServerUids);
        _expungeRequired = !_removedUids.isEmpty();

        if (_options & ImportChanges) {
            if (!updateMessagesMetaData(context, storedKey, unseenKey, seenKey, flaggedAsRemoved, flaggedKey, unreadElsewhereKey, importantElsewhereKey, unavailableKey)) {
                _error = true;
            }
        }
        
        // Update messages on the server that are still flagged as unseen but have been read locally
        QMailMessageKey readHereKey(folderKey 
                                    & accountKey
                                    & QMailMessageKey::status(QMailMessage::Read, QMailDataComparator::Includes)
                                    & QMailMessageKey::status(QMailMessage::ReadElsewhere, QMailDataComparator::Excludes)
                                    & QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes)
                                    & unseenKey);
        _readUids = context->client()->serverUids(readHereKey);

        // Update messages on the server that are flagged as seen but have been explicitly marked as unread locally
        QMailMessageKey markedAsUnreadHereKey(folderKey 
                                    & accountKey
                                    & QMailMessageKey::status(QMailMessage::Read, QMailDataComparator::Excludes)
                                    & QMailMessageKey::status(QMailMessage::ReadElsewhere, QMailDataComparator::Includes)
                                    & QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes)
                                    & seenKey);
        _unreadUids = context->client()->serverUids(markedAsUnreadHereKey);

        // Update messages on the server that are still not flagged as important but have been flagged as important locally
        QMailMessageKey reportedKey(seenKey | unseenKey);
        QMailMessageKey unflaggedKey(reportedKey & ~flaggedKey);
        QMailMessageKey markedAsImportantHereKey(folderKey 
                                                 & accountKey
                                                 & QMailMessageKey::status(QMailMessage::Important, QMailDataComparator::Includes)
                                                 & QMailMessageKey::status(QMailMessage::ImportantElsewhere, QMailDataComparator::Excludes)
                                                 & QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes)
                                                 & unflaggedKey);
        _importantUids = context->client()->serverUids(markedAsImportantHereKey);

        // Update messages on the server that are still flagged as important but have been flagged as not important locally
        QMailMessageKey markedAsNotImportantHereKey(folderKey 
                                                    & accountKey
                                                    & QMailMessageKey::status(QMailMessage::Important, QMailDataComparator::Excludes)
                                                    & QMailMessageKey::status(QMailMessage::ImportantElsewhere, QMailDataComparator::Includes)
                                                    & QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes)
                                                    & flaggedKey);
        _unimportantUids = context->client()->serverUids(markedAsNotImportantHereKey);

        // Mark any messages that we have read that the server thinks are unread
        handleUidStore(context);
    }
}

void ImapSynchronizeAllStrategy::searchInconclusive(ImapStrategyContextBase *context)
{
    processNextFolder(context);
}

bool ImapSynchronizeAllStrategy::setNextSeen(ImapStrategyContextBase *context)
{
    if (!_readUids.isEmpty()) {
        QStringList msgUidl = _readUids.mid(0, batchSize);
        QString msg = QObject::tr("Marking message as read");
        foreach(QString uid, msgUidl) {
            _readUids.removeAll(uid);
            _storedReadUids.append(uid);
        }

        context->updateStatus(msg);
        context->protocol().sendUidStore(MFlag_Seen, true, numericUidSequence(msgUidl));
        
        return true;
    }

    return false;
}

bool ImapSynchronizeAllStrategy::setNextNotSeen(ImapStrategyContextBase *context)
{
    if (!_unreadUids.isEmpty()) {
        QStringList msgUidl = _unreadUids.mid(0, batchSize);
        QString msg = QObject::tr("Marking message as unread");
        foreach(QString uid, msgUidl) {
            _unreadUids.removeAll(uid);
            _storedUnreadUids.append(uid);
        }

        context->updateStatus(msg);
        context->protocol().sendUidStore(MFlag_Seen, false, numericUidSequence(msgUidl));
        
        return true;
    }

    return false;
}

bool ImapSynchronizeAllStrategy::setNextImportant(ImapStrategyContextBase *context)
{
    if (!_importantUids.isEmpty()) {
        QStringList msgUidl = _importantUids.mid(0, batchSize);
        QString msg = QObject::tr("Marking message as important");
        foreach(QString uid, msgUidl) {
            _importantUids.removeAll(uid);
            _storedImportantUids.append(uid);
        }

        context->updateStatus(msg);
        context->protocol().sendUidStore(MFlag_Flagged, true, numericUidSequence(msgUidl));
        
        return true;
    }

    return false;
}

bool ImapSynchronizeAllStrategy::setNextNotImportant(ImapStrategyContextBase *context)
{
    if (!_unimportantUids.isEmpty()) {
        QStringList msgUidl = _unimportantUids.mid(0, batchSize);
        QString msg = QObject::tr("Marking message as unimportant");
        foreach(QString uid, msgUidl) {
            _unimportantUids.removeAll(uid);
            _storedUnimportantUids.append(uid);
        }

        context->updateStatus(msg);
        context->protocol().sendUidStore(MFlag_Flagged, false, numericUidSequence(msgUidl));
        
        return true;
    }

    return false;
}

bool ImapSynchronizeAllStrategy::setNextDeleted(ImapStrategyContextBase *context)
{
    ImapConfiguration imapCfg(context->config());
    if (imapCfg.canDeleteMail()) {
        if (!_removedUids.isEmpty()) {
            QStringList msgUidl = _removedUids.mid(0, batchSize);
            QString msg = QObject::tr("Deleting message");
            foreach(QString uid, msgUidl) {
                _removedUids.removeAll(uid);
                _storedRemovedUids.append(uid);
            }

            context->updateStatus(msg);
            context->protocol().sendUidStore(MFlag_Deleted, true, numericUidSequence(msgUidl));
            return true;
        } else if (_expungeRequired) {
            // All messages flagged as deleted, expunge them
            context->protocol().sendExpunge();
            return true;
        }
    }

    return false;
}

void ImapSynchronizeAllStrategy::folderPreviewCompleted(ImapStrategyContextBase *context)
{
    const ImapMailboxProperties &properties(context->mailbox());

    if (!_error && (properties.exists > 0)) {
        QMailFolder folder(properties.id);
        folder.setCustomField("qmf-min-serveruid", QString::number(1));
        folder.setCustomField("qmf-max-serveruid", QString::number(properties.uidNext - 1));
        folder.removeCustomField("qmf-highestmodseq");
        folder.setServerUndiscoveredCount(0);

        if (!QMailStore::instance()->updateFolder(&folder)) {
            _error = true;
            qWarning() << "Unable to update folder for account:" << context->config().id();
        }
    }
    
    if (!_error) {
        updateAccountLastSynchronized(context);
    }
    
}


/* A strategy to retrieve all messages from an account.
   
   That is to retrieve message previews for all known messages 
   in an account, and to complete messages where appropriate.
*/
ImapRetrieveAllStrategy::ImapRetrieveAllStrategy()
{
    // This is just synchronize without update-exporting
    setOptions(static_cast<Options>(RetrieveMail | ImportChanges));
}


/* A strategy to exports changes made on the client to the server.
   That is to export status flag changes (unread <-> read) and deletes.
*/
ImapExportUpdatesStrategy::ImapExportUpdatesStrategy()
{
    setOptions(ExportChanges);
}

static void updateFolderExportsMap(QMap<QMailFolderId, QStringList > *folderExportMap, QMailMessageKey filter)
{
    QMailMessageKey::Properties props(QMailMessageKey::Id | QMailDisconnected::parentFolderProperties() | QMailMessageKey::ServerUid);
    foreach (const QMailMessageMetaData &metaData, QMailStore::instance()->messagesMetaData(filter, props)) {
        if (!metaData.serverUid().isEmpty() && metaData.parentFolderId().isValid()) {
            (*folderExportMap)[metaData.parentFolderId()] +=  metaData.serverUid();
        } else {
            qWarning() << "Unable to export status change to message" << metaData.id();
            QMailMessageMetaData m(metaData.id());
            QMailMessageKey key(QMailMessageKey::id(m.id()));
            if (!QMailStore::instance()->updateMessagesMetaData(key, QMailMessage::ReadElsewhere, m.status() & QMailMessage::Read)
                || !QMailStore::instance()->updateMessagesMetaData(key, QMailMessage::ImportantElsewhere, m.status() & QMailMessage::Important)) {
                qWarning() << "Unable to revert malformed status update for message" << metaData.id();
            }
        }
    }
}

void ImapExportUpdatesStrategy::handleLogin(ImapStrategyContextBase *context)
{
    _transferState = List;
    _completionList.clear();
    _completionSectionList.clear();

    ImapConfiguration imapCfg(context->config());
    if (!imapCfg.canDeleteMail()) {
        QString name(QMailAccount(context->config().id()).name());
        qMailLog(IMAP) << "Not exporting deletions. Deleting mail is disabled for account name" << name;
    }

    QMailMessageKey readStatusKey(QMailMessageKey::status(QMailMessage::Read, QMailDataComparator::Includes));
    readStatusKey &= QMailMessageKey::status(QMailMessage::ReadElsewhere, QMailDataComparator::Excludes);
    readStatusKey &= QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes);
    QMailMessageKey unreadStatusKey(QMailMessageKey::status(QMailMessage::Read, QMailDataComparator::Excludes));
    unreadStatusKey &= QMailMessageKey::status(QMailMessage::ReadElsewhere, QMailDataComparator::Includes);
    unreadStatusKey &= QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes);
    QMailMessageKey importantStatusKey(QMailMessageKey::status(QMailMessage::Important, QMailDataComparator::Includes));
    importantStatusKey &= QMailMessageKey::status(QMailMessage::ImportantElsewhere, QMailDataComparator::Excludes);
    importantStatusKey &= QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes);
    QMailMessageKey unimportantStatusKey(QMailMessageKey::status(QMailMessage::Important, QMailDataComparator::Excludes));
    unimportantStatusKey &= QMailMessageKey::status(QMailMessage::ImportantElsewhere, QMailDataComparator::Includes);
    unimportantStatusKey &= QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Excludes);

    _folderMessageUids.clear();

    ImapClient *c(context->client());
    QMailMessageKey accountKey(QMailMessageKey::parentAccountId(c->account()));
    QMap<QMailFolderId, QStringList > read;
    QMap<QMailFolderId, QStringList > unread;
    QMap<QMailFolderId, QStringList > important;
    QMap<QMailFolderId, QStringList > unimportant;
    QMap<QMailFolderId, QStringList > deleted;
    
    updateFolderExportsMap(&read, accountKey & readStatusKey);
    updateFolderExportsMap(&unread, accountKey & unreadStatusKey);
    updateFolderExportsMap(&important, accountKey & importantStatusKey);
    updateFolderExportsMap(&unimportant, accountKey & unimportantStatusKey);
    if (imapCfg.canDeleteMail()) {
        // Also find messages deleted locally
        foreach (const QMailMessageRemovalRecord& r, QMailStore::instance()->messageRemovalRecords(c->account())) {
            int index = r.serverUid().indexOf(UID_SEPARATOR);
            if (index > 0) {
                const QMailFolderId folderId(r.serverUid().left(index).toUInt());
                if (folderId.isValid())
                    deleted[folderId] += r.serverUid();
                else
                    qWarning() << "invalid folder id" << folderId.toULongLong() <<"for serverUid" << r.serverUid();
            } else {
                qWarning() << "Unable to process malformed message removal record for account" << c->account();
                if (!QMailStore::instance()->purgeMessageRemovalRecords(c->account(), QStringList() << r.serverUid())) {
                    qWarning() << "Unable to purge message records for account:" << c->account();
                }
            }
        }
    }

    foreach (const QMailFolderId &folderId, context->client()->mailboxIds()) {
        if (read[folderId].count() || unread[folderId].count() || important[folderId].count() || unimportant[folderId].count() || deleted[folderId].count()) {
            QList<QStringList> entry;
            entry << read[folderId] << unread[folderId] << important[folderId] << unimportant[folderId] << deleted[folderId];
            _folderMessageUids.insert(folderId, entry);
        }
    }
    
    processNextFolder(context);
}

void ImapExportUpdatesStrategy::handleUidSearch(ImapStrategyContextBase *context)
{
    _serverReportedUids = context->mailbox().uidList;

    processUidSearchResults(context);
}

void ImapExportUpdatesStrategy::handleSelect(ImapStrategyContextBase *context)
{
    // Don't list subfolders, instead skip directly to processing current folder
    handleList(context);
}

void ImapExportUpdatesStrategy::folderListFolderAction(ImapStrategyContextBase *context)
{
    _serverReportedUids = QStringList();
    
    // We have selected the current mailbox
    if (context->mailbox().exists > 0) {
        // Find which of our messages-of-interest are still on the server
        IntegerRegion clientRegion(stripFolderPrefix(_clientReadUids + _clientUnreadUids + _clientImportantUids + _clientUnimportantUids + _clientDeletedUids));
        context->protocol().sendUidSearch(MFlag_All, "UID " + clientRegion.toString());
    } else {
        // No messages, so no need to perform search
        processUidSearchResults(context);
    }
}

bool ImapExportUpdatesStrategy::nextFolder()
{
    if (_folderMessageUids.isEmpty()) {
        return false;
    }

    QMap<QMailFolderId, QList<QStringList> >::iterator it = _folderMessageUids.begin();

    if (it.value().count() != 5) {
        qWarning() << "quintuple mismatch in export updates nextFolder, folder" << it.key() << "count" << it.value().count();
        it = _folderMessageUids.erase(it);
        return nextFolder();
    }
    
    setCurrentMailbox(it.key());
    _clientReadUids = it.value()[0];
    _clientUnreadUids = it.value()[1];
    _clientImportantUids = it.value()[2];
    _clientUnimportantUids = it.value()[3];
    _clientDeletedUids = it.value()[4];

    it = _folderMessageUids.erase(it);
    return true;
}

void ImapExportUpdatesStrategy::processUidSearchResults(ImapStrategyContextBase *context)
{
    // Messages marked deleted locally that the server reports exists
    _removedUids = inFirstAndSecond(_clientDeletedUids, _serverReportedUids);
    _expungeRequired = !_removedUids.isEmpty();

    // Messages marked read locally that the server reports exists
    _readUids = inFirstAndSecond(_clientReadUids, _serverReportedUids);

    // Messages marked unread locally that the server reports exists
    _unreadUids = inFirstAndSecond(_clientUnreadUids, _serverReportedUids);

    // Messages marked important locally that the server reports are unimportant
    _importantUids = inFirstAndSecond(_clientImportantUids, _serverReportedUids);

    // Messages marked unimportant locally that the server reports are important
    _unimportantUids = inFirstAndSecond(_clientUnimportantUids, _serverReportedUids);

    handleUidStore(context);
}


/* A strategy to update message flags for a list of messages.
   
   That is to detect changes to flags (unseen->seen) 
   and to detect deleted mails.
*/
void ImapUpdateMessagesFlagsStrategy::clearSelection()
{
    ImapFolderListStrategy::clearSelection();

    _selectedMessageIds.clear();
    _folderMessageUids.clear();
}

void ImapUpdateMessagesFlagsStrategy::selectedMailsAppend(const QMailMessageIdList &messageIds)
{
    _selectedMessageIds += messageIds;
}

QMailMessageIdList ImapUpdateMessagesFlagsStrategy::selectedMails()
{
    return _selectedMessageIds;
}

void ImapUpdateMessagesFlagsStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_UIDSearch:
        {
            handleUidSearch(context);
            break;
        }
        
        default:
        {
            ImapFolderListStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapUpdateMessagesFlagsStrategy::handleLogin(ImapStrategyContextBase *context)
{
    _transferState = List;
    _serverUids.clear();
    _searchState = Unseen;

    // Associate each message to the relevant folder
    _folderMessageUids.clear();
    if (!_selectedMessageIds.isEmpty()) {
        foreach (const QMailMessageMetaData &metaData, QMailStore::instance()->messagesMetaData(QMailMessageKey::id(_selectedMessageIds), 
                                                                                                QMailMessageKey::ServerUid 
                                                                                                | QMailDisconnected::parentFolderProperties(),
                                                                                                QMailStore::ReturnAll)) {
            if (!metaData.serverUid().isEmpty() && QMailDisconnected::sourceFolderId(metaData).isValid())
                _folderMessageUids[QMailDisconnected::sourceFolderId(metaData)].append(metaData.serverUid());
        }
    }

    processNextFolder(context);
}

void ImapUpdateMessagesFlagsStrategy::handleUidSearch(ImapStrategyContextBase *context)
{
    const ImapMailboxProperties &properties(context->mailbox());

    switch(_searchState)
    {
    case Unseen:
    {
        _unseenUids = properties.uidList;

        _searchState = Seen;
        context->protocol().sendUidSearch(MFlag_Seen, "UID " + _filter);
        break;
    }
    case Seen:
    {
        _seenUids = properties.uidList;

        _searchState = Flagged;
        context->protocol().sendUidSearch(MFlag_Flagged, "UID " + _filter);
        break;
    }
    case Flagged:
    {
        _flaggedUids = properties.uidList;
        processUidSearchResults(context);
        break;
    }
    default:
        qMailLog(IMAP) << "Unknown search status in transition";
        Q_ASSERT(0);

        processNextFolder(context);
    }
}

void ImapUpdateMessagesFlagsStrategy::folderListFolderAction(ImapStrategyContextBase *context)
{
    const ImapMailboxProperties &properties(context->mailbox());

    if (!properties.noModSeq && (properties.highestModSeq == _currentModSeq)) {
        // The mod sequence has not changed for this folder
        processNextFolder(context);
        return;
    }

    //  If we have messages, we need to discover any flag changes
    if (properties.exists > 0) {
        IntegerRegion clientRegion(stripFolderPrefix(_serverUids));
        _filter = clientRegion.toString();
        _searchState = Unseen;

        // Start by looking for previously-unseen messages
        // If a message is moved from Unseen to Seen between our two searches, then we will
        // miss this change by searching Unseen first; if, however, we were to search Seen first,
        // then we would miss the message altogether and mark it as deleted...
        context->protocol().sendUidSearch(MFlag_Unseen, "UID " + _filter);
    } else {
        processUidSearchResults(context);
    }
}

void ImapUpdateMessagesFlagsStrategy::folderListCompleted(ImapStrategyContextBase *context)
{
    messageListCompleted(context);
}

bool ImapUpdateMessagesFlagsStrategy::nextFolder()
{
    if (_folderMessageUids.isEmpty()) {
        return false;
    }

    QMap<QMailFolderId, QStringList>::iterator it = _folderMessageUids.begin();

    setCurrentMailbox(it.key());

    _serverUids = it.value();

    it = _folderMessageUids.erase(it);
    return true;
}

void ImapUpdateMessagesFlagsStrategy::processFolder(ImapStrategyContextBase *context)
{
    QMailFolderId folderId(_currentMailbox.id());
    
    //not not try select an unselectable mailbox
    if(!_folderStatus.contains(folderId) || !(_folderStatus.value(folderId) & NoSelect))
        selectFolder(context, _currentMailbox);
}

void ImapUpdateMessagesFlagsStrategy::processUidSearchResults(ImapStrategyContextBase *context)
{
    QMailFolderId folderId(_currentMailbox.id());
    if (!folderId.isValid()) {
        // Folder was removed while we were updating messages flags in it
        processNextFolder(context);
        return;
    }

    // Check if any of the message is flagged as deleted on server side and not expunged yet
    QStringList flaggedAsRemovedUids = flaggedAsDeletedUids(context);
    
    // Compare the server message list with our message list
    QMailMessageKey accountKey(QMailMessageKey::parentAccountId(context->config().id()));
    QMailMessageKey storedKey(QMailMessageKey::serverUid(_serverUids));
    QMailMessageKey unseenKey(QMailMessageKey::serverUid(_unseenUids));
    QMailMessageKey seenKey(QMailMessageKey::serverUid(_seenUids));
    QMailMessageKey readStatusKey(QMailMessageKey::status(QMailMessage::ReadElsewhere, QMailDataComparator::Includes));
    QMailMessageKey removedStatusKey(QMailMessageKey::status(QMailMessage::Removed, QMailDataComparator::Includes));
    QMailMessageKey folderKey(context->client()->messagesKey(folderId) | context->client()->trashKey(folderId));
    QMailMessageKey unreadElsewhereKey(folderKey & accountKey & ~readStatusKey);
    QMailMessageKey unavailableKey(folderKey & accountKey & removedStatusKey);
    QMailMessageKey flaggedKey(QMailMessageKey::serverUid(_flaggedUids));
    QMailMessageKey importantElsewhereKey(QMailMessageKey::status(QMailMessage::ImportantElsewhere, QMailDataComparator::Includes));
    QMailMessageKey flaggedAsRemoved(QMailMessageKey::serverUid(flaggedAsRemovedUids));

    if (!updateMessagesMetaData(context, storedKey, unseenKey, seenKey, flaggedAsRemoved, flaggedKey, unreadElsewhereKey, importantElsewhereKey, unavailableKey))
        _error = true;

    processNextFolder(context);
}


/* A strategy to ensure a given number of messages, for a given
   mailbox are on the client, retrieving messages if necessary.

   Retrieval order is defined by server uid.
*/
void ImapRetrieveMessageListStrategy::setMinimum(uint minimum)
{
    _minimum = minimum;
    _mailboxIds.clear();
}

void ImapRetrieveMessageListStrategy::setAccountCheck(bool check)
{
    _accountCheck = check;
}

void ImapRetrieveMessageListStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_FetchFlags:
        {
            handleFetchFlags(context);
            break;
        }
        
        case IMAP_UIDSearch:
        {
            handleUidSearch(context);
            break;
        }
        
        default:
        {
            ImapSynchronizeBaseStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapRetrieveMessageListStrategy::selectFolder(ImapStrategyContextBase *context, const QMailFolder &folder)
{
    if (context->protocol().capabilities().contains("QRESYNC")) {
        context->protocol().sendQResync(folder);
    } else {
        ImapSynchronizeBaseStrategy::selectFolder(context, folder);
    }
}

void ImapRetrieveMessageListStrategy::handleLogin(ImapStrategyContextBase *context)
{
    if (_accountCheck) {
        context->updateStatus(QObject::tr("Scanning folders"));
    } else {
        context->updateStatus(QObject::tr("Scanning folder"));
    }
    _transferState = List;
    _fillingGap = false;
    _completionList.clear();
    _completionSectionList.clear();
    _newMinMaxMap.clear();
    _listAll = false;
    _qresyncListingNew = false;
    _qresyncRetrieve.clear();
    _qresyncVanished = 0;
    
    ImapSynchronizeBaseStrategy::handleLogin(context);
}

void ImapRetrieveMessageListStrategy::messageListCompleted(ImapStrategyContextBase *context)
{
    foreach (const QMailFolderId &folderId, _updatedFolders) {
        QMailFolder folder(folderId);

        bool modified(false);
        if (!_error) { // Only update the range of downloaded messages if no store errors, including add errors have occurred
            if (_newMinMaxMap.contains(folderId)) {
                folder.setCustomField("qmf-min-serveruid", QString::number(_newMinMaxMap[folderId].minimum()));
                folder.setCustomField("qmf-max-serveruid", QString::number(_newMinMaxMap[folderId].maximum()));
            }
            modified = true;
        }

        if (folder.serverUndiscoveredCount() != 0) {
            folder.setServerUndiscoveredCount(0); // All undiscovered messages have been retrieved
            modified = true;
        }

        if (modified && !QMailStore::instance()->updateFolder(&folder)) {
            _error = true;
            qWarning() << "Unable to update folder for account:" << context->config().id();
        }
    }

    _updatedFolders.clear();
    _newMinMaxMap.clear();
    
    if (!_error) {
        updateAccountLastSynchronized(context);
    }
        
    ImapSynchronizeBaseStrategy::messageListCompleted(context);
}

void ImapRetrieveMessageListStrategy::folderListCompleted(ImapStrategyContextBase *context)
{
    previewDiscoveredMessages(context);
}

static void markMessages(IntegerRegion region, quint64 flag, bool set, const QMailFolderId &folderId, bool *error)
{
    if (!region.cardinality())
        return;
    
    QStringList uidList;
    foreach(QString uid, region.toStringList()) {
        uidList.append(QString::number(folderId.toULongLong()) + UID_SEPARATOR + uid);
    }
    QMailMessageKey uidKey(QMailMessageKey::serverUid(uidList) & QMailMessageKey::status(flag, set ? QMailDataComparator::Excludes : QMailDataComparator::Includes));
    if (!QMailStore::instance()->updateMessagesMetaData(uidKey, flag, set)) {
        qWarning() << "Unable to update message metadata for folder:" << folderId << "flag" << flag << "set" << set;
        *error = true;
    }
}

static void processFlagChanges(const QList<FlagChange> &changes, const QMailFolderId &id, bool *_error)
{
    IntegerRegion read;
    IntegerRegion unread;
    IntegerRegion important;
    IntegerRegion notImportant;
    IntegerRegion deletedElsewhere;
    IntegerRegion undeleted;
    foreach(FlagChange change, changes) {
        bool ok;
        QString uidStr(stripFolderPrefix(change.first));
        MessageFlags flags(change.second);
        int uid(uidStr.toUInt(&ok));
        if (!uidStr.isEmpty() && ok) {
            if (flags & MFlag_Seen) {
                read.add(uid);
            } else {
                unread.add(uid);
            }
            if (flags & MFlag_Flagged) {
                important.add(uid);
            } else {
                notImportant.add(uid);
            }
            if (flags & MFlag_Deleted) {
                deletedElsewhere.add(uid);
            } else {
                undeleted.add(uid);
            }
        }
    }
    markMessages(read, QMailMessage::Read, true, id, _error);
    markMessages(read, QMailMessage::ReadElsewhere, true, id, _error);
    markMessages(unread, QMailMessage::Read, false, id, _error);
    markMessages(unread, QMailMessage::ReadElsewhere, false, id, _error);
    markMessages(important, QMailMessage::Important, true, id, _error);
    markMessages(important, QMailMessage::ImportantElsewhere, true, id, _error);
    markMessages(notImportant, QMailMessage::Important, false, id, _error);
    markMessages(notImportant, QMailMessage::ImportantElsewhere, false, id, _error);
    markMessages(deletedElsewhere, QMailMessage::Removed, true, id, _error);
    markMessages(undeleted, QMailMessage::Removed, false, id, _error);
}

void ImapRetrieveMessageListStrategy::handleFetchFlags(ImapStrategyContextBase *context)
{
    // implement this, should only be hit in non qresync case.
    const ImapMailboxProperties &properties(context->mailbox());
    QMailFolder folder(properties.id);

    bool ok; // toUint returns 0 on error, which is an invalid IMAP uid
    int clientMin(folder.customField("qmf-min-serveruid").toUInt(&ok));
    int clientMax(folder.customField("qmf-max-serveruid").toUInt(&ok));
    
    // 
    // Determine which messages are on the client but not the server
    // (i.e. determine which messages have been deleted by other clients,
    //       or have been marked as deleted on the client but have not yet 
    //       been removed from the server using exportUpdates)
    //
    // The set of messages to fetch needs to be adjusted based on the number of such messages
    IntegerRegion rawServerRegion;
    processFlagChanges(properties.flagChanges, properties.id, &_error);
    foreach(const QString &uid, properties.uidList) {
        bool ok;
        uint number = stripFolderPrefix(uid).toUInt(&ok);
        if (ok)
            rawServerRegion.add(number);
    }

    QMailMessageKey sourceKey(QMailDisconnected::sourceKey(folder.id()));
    
    IntegerRegion trueClientRegion;
    foreach (const QMailMessageMetaData& r, QMailStore::instance()->messagesMetaData(sourceKey, QMailMessageKey::ServerUid)) {
        const QString uid(r.serverUid());
        int serverUid(stripFolderPrefix(uid).toUInt());
        trueClientRegion.add(serverUid);
    }

    IntegerRegion missingRegion(rawServerRegion.subtract(trueClientRegion));
    missingRegion = missingRegion.subtract(IntegerRegion(1, clientMin-1)).subtract(IntegerRegion(clientMax+1, properties.uidNext));
    if (missingRegion.cardinality()) {
        // Don't fetch message deleted on client but not yet deleted on remote server
        IntegerRegion removedRegion;
        foreach (const QMailMessageRemovalRecord& r, QMailStore::instance()->messageRemovalRecords(context->config().id(), folder.id())) {
            if (!r.serverUid().isEmpty() && (r.parentFolderId() == folder.id())) {
                const QString uid(r.serverUid());
                int serverUid(stripFolderPrefix(uid).toUInt());
                removedRegion.add(serverUid);
            }
        }
        missingRegion = missingRegion.subtract(removedRegion);
    }

    if (missingRegion.cardinality()) {
        qWarning() << "WARNING server contains uids in contiguous region not on client!!!" << missingRegion.toString();
        qWarning() << "WARNING clientMin" << clientMin << "clientMax" << clientMax;
    }
    
    int serverMinimum = properties.uidNext;
    int serverMaximum = properties.uidNext;
    if (!trueClientRegion.isEmpty()) {
        // Workaround for imap servers that don't return a UIDNEXT response when a folder is SELECTed
        serverMaximum = qMax(serverMaximum, trueClientRegion.maximum());
    }
    if (rawServerRegion.cardinality()) {
        // Found region on server
        serverMinimum = rawServerRegion.minimum();
        if (_listAll) {
            // Considering all messages on the client in the folder
            IntegerRegion beginningClientRegion; // none of these messages are on the server
            foreach (const QMailMessageMetaData& r, QMailStore::instance()->messagesMetaData(sourceKey, QMailMessageKey::ServerUid)) {
                const QString uid(r.serverUid());
                int serverUid(stripFolderPrefix(uid).toUInt());
                if (serverUid < serverMinimum)
                    beginningClientRegion.add(serverUid);
            }
            
            QStringList removedList;
            foreach(QString uid, beginningClientRegion.toStringList()) {
                removedList.append(QString::number(folder.id().toULongLong()) + UID_SEPARATOR + uid);
            }

            QMailMessageKey removedKey(QMailMessageKey::serverUid(removedList));
            if (!purge(context, removedKey)) {
                _error = true;
            }
        }
    }

    IntegerRegion serverRange(serverMinimum, serverMaximum);
    IntegerRegion clientRegionWithinServerRange(serverRange.intersect(trueClientRegion));
    IntegerRegion clientRemovedRegion(clientRegionWithinServerRange.subtract(rawServerRegion).toStringList());
    QStringList removed;
    foreach(QString uid, clientRemovedRegion.toStringList()) {
        removed.append(QString::number(folder.id().toULongLong()) + UID_SEPARATOR + uid);
    }

    // The list of existing messages on the server can be used to update the status of
    // messages on the client i.e. mark messages on the client as removed for messages that have
    // been removed on the server
    QMailMessageKey removedKey(QMailMessageKey::serverUid(removed));
    if (!purge(context, removedKey)) {
        _error = true;
    }
    
    // Use an optimization/simplification because client region should be contiguous
    IntegerRegion clientRegion;
    if ((clientMin != 0) && (clientMax != 0))
        clientRegion = IntegerRegion(clientMin, clientMax);
    
    IntegerRegion serverRegion(rawServerRegion);
    if (!_fillingGap) {
        if (!clientRegion.isEmpty() && !serverRegion.isEmpty()) {
            uint newestClient(clientRegion.maximum());
            uint oldestServer(serverRegion.minimum());
            uint newestServer(serverRegion.maximum());
            if (newestClient + 1 < oldestServer) {
                // There's a gap
                _fillingGap = true;
                _listAll = true;
                context->protocol().sendFetchFlags(QString("%1:%2").arg(clientRegion.minimum()).arg(newestServer), "UID");
                return;
            }
        }
    }
    
    IntegerRegion serverNew(serverRegion.subtract(IntegerRegion(1, clientMax)));
    IntegerRegion serverOld(serverRegion.subtract(IntegerRegion(clientMax + 1, INT_MAX)));
    // Adjust for messages on client not on server
    int adjustment(removed.count());
    if (_listAll) {
        // It's possible the server region extends to the beginning of the folder
        // In this case insufficient information is available to adjust accurately
        adjustment = 0;
    }
    // The idea here is that if the client has say n messages in a folder and 
    // minimum is set to n then don't get any more messages even if some messages 
    // are marked as removed on the client.
    QStringList serverOldList(serverOld.toStringList());
    if (adjustment < serverOldList.count()) {
        serverOld = IntegerRegion(serverOldList.mid(adjustment));
    } else {
        serverOld = IntegerRegion();
    }
    
    serverRegion = serverNew.add(serverOld);
    _updatedFolders.append(properties.id);
    _listAll = false;
    
    IntegerRegion difference(serverRegion.subtract(clientRegion));
    difference = difference.add(missingRegion);
    
    if (difference.cardinality()) {
        _retrieveUids.append(qMakePair(properties.id, difference.toStringList()));
        int newClientMin;
        int newClientMax;
        if (clientMin > 0)
            newClientMin = qMin(serverRegion.minimum(), clientMin);
        else
            newClientMin = serverRegion.minimum();
        if (clientMax > 0)
            newClientMax = qMax(serverRegion.maximum(), clientMax);
        else
            newClientMax = serverRegion.maximum();
        if ((newClientMin > 0) && (newClientMax > 0))
            _newMinMaxMap.insert(properties.id, IntegerRegion(newClientMin, newClientMax));
    }

    processNextFolder(context);
}

void ImapRetrieveMessageListStrategy::handleUidSearch(ImapStrategyContextBase *context)
{
    if (context->protocol().capabilities().contains("QRESYNC")) {
        qresyncHandleUidSearch(context);
        return;
    }
    qWarning() << "Unexpected code path reached, non QRESYNC case";
}

void ImapRetrieveMessageListStrategy::folderListFolderAction(ImapStrategyContextBase *context)
{
    // The current mailbox is now selected
    const ImapMailboxProperties &properties(context->mailbox());
    uint minimum(_minimum);
    QMailMessageKey sourceKey(QMailDisconnected::sourceKey(properties.id));

    // Could get flag changes mod sequences when CONDSTORE is available
    if ((properties.exists == 0) || (minimum <= 0)) {
        // No messages, so no need to perform search
        if (properties.exists == 0) {
            // Folder is completely empty mark all messages in it on client as removed
            QMailMessageKey removedKey(sourceKey);
            if (!purge(context, removedKey)) {
                _error = true;
            }
        }
        processUidSearchResults(context);
        return;
    }
    _fillingGap = false;
    _listAll = false;

    if (context->protocol().capabilities().contains("QRESYNC")) {
        qresyncFolderListFolderAction(context);
        return;
    }
    
    if (_accountCheck) {
        // Request all (non search) messages in this folder or _minimum which ever is greater
        // For all but the first retrieval detect vanished messages and update flags of existing messages
        QMailMessageKey countKey(sourceKey);
        countKey &= ~QMailMessageKey::status(QMailMessage::Temporary);
        minimum = qMax((uint)QMailStore::instance()->countMessages(countKey), _minimum);
    } // otherwise not an account check, so could be check for just new mail

    // Compute starting sequence number
    int start = static_cast<int>(properties.exists) - minimum + 1;
    if (start <= 1) {
        start = 1;
        _listAll = true;
    }
    // We need to determine these values again
    context->protocol().sendFetchFlags(QString("%1:*").arg(start));
}

void ImapRetrieveMessageListStrategy::qresyncHandleUidSearch(ImapStrategyContextBase *context)
{
    // Add the listed uids to the retrieve list
    const ImapMailboxProperties &properties(context->mailbox());
    foreach(const QString &uid, properties.uidList) {
        bool ok;
        uint number = stripFolderPrefix(uid).toUInt(&ok);
        if (ok) {
            _qresyncRetrieve.add(number);
        }
    }
        
    if (_qresyncListingNew) {
        // Only new messages have been retrieved, check to see if it's necessary to get more messages
        QMailMessageKey sourceKey(QMailDisconnected::sourceKey(properties.id));
        QMailMessageKey countKey(sourceKey);
        countKey &= ~QMailMessageKey::status(QMailMessage::Temporary);
        if ((uint)QMailStore::instance()->countMessages(countKey) < _minimum) {
            // Need to get more messages, min argument + new on server - removed on server and were on client
            uint adjustedMinimum = _minimum + _qresyncRetrieve.cardinality() - _qresyncVanished;
            int start = static_cast<int>(properties.exists) - adjustedMinimum + 1;
            if (start <= 1) {
                start = 1;
                _listAll = true;
            }
            _qresyncListingNew = false;
            context->protocol().sendUidSearch(MFlag_All, QString("%1:*").arg(start));
            return;
        }
    }
    
    if (_qresyncRetrieve.isEmpty()) {
        // Nothing to do
        processUidSearchResults(context);
        return;
    }
        
    // Retrieve messages list in _qresyncRetrieve region that are not already in the store
    QMailMessageKey sourceKey(QMailDisconnected::sourceKey(properties.id));
    IntegerRegion clientRegion;
    foreach (const QMailMessageMetaData& r, QMailStore::instance()->messagesMetaData(sourceKey, QMailMessageKey::ServerUid)) {
        const QString uid(r.serverUid());
        int serverUid(stripFolderPrefix(uid).toUInt());
        clientRegion.add(serverUid);
    }
    
    IntegerRegion difference(_qresyncRetrieve.subtract(clientRegion));
    if (difference.cardinality()) {
        _retrieveUids.append(qMakePair(properties.id, difference.toStringList()));
        _updatedFolders.append(properties.id);
        int newClientMin(difference.minimum());
        int newClientMax(difference.maximum());
        if (clientRegion.cardinality()) {
            newClientMin = qMin(newClientMin, clientRegion.minimum());
            newClientMax = qMax(newClientMax, clientRegion.maximum());
        }
        
        _newMinMaxMap.insert(properties.id, IntegerRegion(newClientMin, newClientMax));
    }
    processUidSearchResults(context);
}

void ImapRetrieveMessageListStrategy::qresyncFolderListFolderAction(ImapStrategyContextBase *context)
{
    _qresyncListingNew = false;
    _qresyncRetrieve.clear();
    _qresyncVanished = 0;
    bool ok;
    bool ok2;

    // Update highestModSeq for this folder
    const ImapMailboxProperties &properties(context->mailbox());
    
    IntegerRegion vanished(properties.vanished);
    QMailFolder folder(properties.id);
    QString minServerUid(folder.customField("qmf-min-serveruid"));
    int clientMin(minServerUid.toUInt(&ok));
    QString maxServerUid(folder.customField("qmf-max-serveruid"));
    int clientMax(maxServerUid.toUInt(&ok2));
    if (!minServerUid.isEmpty() && !maxServerUid.isEmpty() && ok && ok2) {
        IntegerRegion vanishedOnClient(vanished.intersect(IntegerRegion(clientMin, INT_MAX)));
        _qresyncVanished = vanishedOnClient.cardinality();
        QStringList removedList;
        foreach(QString uid, vanishedOnClient.toStringList()) {
            removedList.append(QString::number(folder.id().toULongLong()) + UID_SEPARATOR + uid);
        }
        if (!removedList.isEmpty()) {
            QMailMessageKey removedKey(QMailMessageKey::serverUid(removedList));
            if (!purge(context, removedKey)) {
                _error = true;
            }
        }
    }

    // Always set the highestmodseq for the folder
    {
        processFlagChanges(properties.flagChanges, properties.id, &_error);
        folder.setCustomField("qmf-highestmodseq", properties.highestModSeq.isEmpty() ? QLatin1String("0") : properties.highestModSeq);
        if (!QMailStore::instance()->updateFolder(&folder)) {
            _error = true;
            qWarning() << "Unable to update folder HIGHESTMODSEQ for account:" << context->config().id();
        }
    }
    
    if (!maxServerUid.isEmpty() && ok2 && (clientMax + 1 < static_cast<int>(properties.uidNext))) {
        // This folder has been synchronized before, fetch new messages in it on external server
        _qresyncListingNew = true;
        context->protocol().sendUidSearch(MFlag_All, QString("UID %1:*").arg(clientMax + 1));
        return;
    }
        
    if (!_minimum) {
        // Nothing to do
        processNextFolder(context);
        return;
    }
    
    // Get recent messages but no more than minimum of them
    QMailMessageKey sourceKey(QMailDisconnected::sourceKey(properties.id));
    QMailMessageKey countKey(sourceKey);
    countKey &= ~QMailMessageKey::status(QMailMessage::Temporary);
    if ((uint)QMailStore::instance()->countMessages(countKey) < _minimum) {
        uint adjustedMinimum = _minimum - _qresyncVanished;
        int start = static_cast<int>(properties.exists) - adjustedMinimum + 1;
        if (start <= 1) {
            start = 1;
            _listAll = true;
        }
        context->protocol().sendUidSearch(MFlag_All, QString("%1:*").arg(start));
        return;
    }
    
    // Nothing to do
    processNextFolder(context);
    return;
}

bool ImapRetrieveMessageListStrategy::synchronizationEnabled(const QMailFolder &folder) const
{
    return ImapSynchronizeBaseStrategy::synchronizationEnabled(folder) || !_accountCheck;
}

void ImapRetrieveMessageListStrategy::processUidSearchResults(ImapStrategyContextBase *context)
{
    processNextFolder(context);
}


/* A strategy to copy selected messages.
*/
// Note: the copy strategy is:
// NB: UIDNEXT doesn't work as expected with Cyrus 2.2; instead, just use the \Recent flag to identify copies...
//
// 1. Select the destination folder, resetting the Recent flag
// 2. Copy each specified message from its existing folder to the destination
// 3. Select the destination folder
// 4. Search for recent messages (unless we already have them because of UIDPLUS)
// 5. Retrieve metadata only for found messages
// 6. Delete any obsolete messages that were replaced by updated copies

void ImapCopyMessagesStrategy::appendMessageSet(const QMailMessageIdList &messageIds, const QMailFolderId& folderId)
{
    _messageSets.append(qMakePair(messageIds, folderId));
}

void ImapCopyMessagesStrategy::clearSelection()
{
    ImapFetchSelectedMessagesStrategy::clearSelection();
    _messageSets.clear();
}

void ImapCopyMessagesStrategy::newConnection(ImapStrategyContextBase *context)
{
    _sourceUid.clear();
    _sourceUids.clear();
    _sourceIndex = 0;
    _obsoleteDestinationUids.clear();

    ImapFetchSelectedMessagesStrategy::newConnection(context);
}

void ImapCopyMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_UIDCopy:
        {
            handleUidCopy(context);
            break;
        }
        
        case IMAP_Append:
        {
            handleAppend(context);
            break;
        }
        
        case IMAP_UIDSearch:
        {
            handleUidSearch(context);
            break;
        }
        
        case IMAP_UIDStore:
        {
            handleUidStore(context);
            break;
        }
        
        default:
        {
            ImapFetchSelectedMessagesStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapCopyMessagesStrategy::handleLogin(ImapStrategyContextBase *context)
{
    selectMessageSet(context);
}

void ImapCopyMessagesStrategy::handleSelect(ImapStrategyContextBase *context)
{
    if (_transferState == Init) {
        // Begin copying messages
        messageListMessageAction(context);
    } else if (_transferState == Search) {
        if (_createdUids.isEmpty()) {
            // We have selected the destination folder - find the newly added messages
            context->protocol().sendUidSearch(MFlag_Recent);
        } else {
            // We already have the UIDs via UIDPLUS
            removeObsoleteUids(context);
        }
    } else {
        ImapFetchSelectedMessagesStrategy::handleSelect(context);
    }
}

void ImapCopyMessagesStrategy::handleUidCopy(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapCopyMessagesStrategy::handleAppend(ImapStrategyContextBase *context)
{
    messageListMessageAction(context);
}

void ImapCopyMessagesStrategy::handleUidSearch(ImapStrategyContextBase *context)
{
    _createdUids = context->mailbox().uidList;

    removeObsoleteUids(context);
}

void ImapCopyMessagesStrategy::handleUidStore(ImapStrategyContextBase *context)
{
    fetchNextCopy(context);
}

void ImapCopyMessagesStrategy::handleUidFetch(ImapStrategyContextBase *context)
{
    fetchNextCopy(context);
}

void ImapCopyMessagesStrategy::messageListMessageAction(ImapStrategyContextBase *context)
{
    if (_messageCount < _listSize) {
        context->updateStatus( QObject::tr("Copying %1 / %2").arg(_messageCount + 1).arg(_listSize) );
    }

    copyNextMessage(context);
}

void ImapCopyMessagesStrategy::messageListFolderAction(ImapStrategyContextBase *context)
{
    if (_currentMailbox.id().isValid() && (_currentMailbox.id() == _destination.id())) {
        // This message is already in the destination folder - we need to append a new copy
        // after closing the destination folder
        context->protocol().sendClose(); //TODO: change this, e.g. use expunge
    } else {
        ImapFetchSelectedMessagesStrategy::messageListFolderAction(context);
    }
}

void ImapCopyMessagesStrategy::messageListCompleted(ImapStrategyContextBase *context)
{
    if (_transferState == Search) {
        // Move on to the next message set
        selectMessageSet(context);
    } else {
        // We have copied all the messages - now we need to retrieve the copies
        _transferState = Search;
        selectFolder(context, _destination);
    }
}

void ImapCopyMessagesStrategy::messageCopied(ImapStrategyContextBase *context, const QString &copiedUid, const QString &createdUid)
{
    if (!createdUid.isEmpty()) {
        _createdUids.append(createdUid);

        _sourceUid[createdUid] = copiedUid;
        _sourceUids.removeAll(copiedUid);
    }

    ImapFetchSelectedMessagesStrategy::messageCopied(context, copiedUid, createdUid);
}

void ImapCopyMessagesStrategy::messageCreated(ImapStrategyContextBase *context, const QMailMessageId &id, const QString &createdUid)
{
    if (!createdUid.isEmpty()) {
        _createdUids.append(createdUid);

        QString pseudoUid("id:" + QString::number(id.toULongLong()));
        _sourceUid[createdUid] = pseudoUid;
        _sourceUids.removeAll(pseudoUid);
    }

    ImapFetchSelectedMessagesStrategy::messageCreated(context, id, createdUid);
}

bool ImapCopyMessagesStrategy::messageFetched(ImapStrategyContextBase *context, QMailMessage &message)
{ 
    QString sourceUid = copiedMessageFetched(context, message);
    _remember[message.serverUid()] = sourceUid;

    return ImapFetchSelectedMessagesStrategy::messageFetched(context, message);
}

void ImapCopyMessagesStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    ImapFetchSelectedMessagesStrategy::messageFlushed(context, message);
    if (_error) return;

    QString sourceUid = _remember.take(message.serverUid());

    if (!sourceUid.isEmpty()) {
        // We're now completed with the source message also
        context->completedMessageAction(sourceUid);
    }
}

QString ImapCopyMessagesStrategy::copiedMessageFetched(ImapStrategyContextBase *context, QMailMessage &message)
{
    // See if we can update the status of the copied message from the source message
    QString sourceUid = _sourceUid[message.serverUid()];
    if (sourceUid.isEmpty()) {
        if (_sourceIndex < _sourceUids.count()) {
            sourceUid = _sourceUids.at(_sourceIndex);
            _sourceIndex++;
        }
    }

    if (!sourceUid.isEmpty()) {
        QMailMessage source;
        if (sourceUid.startsWith("id:")) {
            source = QMailMessage(QMailMessageId(sourceUid.mid(3).toULongLong()));
        } else {
            source = QMailMessage(sourceUid, context->config().id());
        }

        if (source.id().isValid()) {
            updateCopiedMessage(context, message, source);
        } else {
            _error = true;
            qWarning() << "Unable to update message from UID:" << sourceUid << "to copy:" << message.serverUid();
        }

        context->completedMessageCopy(message, source);
    }

    return sourceUid;
}

void ImapCopyMessagesStrategy::updateCopiedMessage(ImapStrategyContextBase *, QMailMessage &message, const QMailMessage &source)
{
    message.setStatus(QMailMessage::New, source.status() & QMailMessage::New);
    message.setStatus(QMailMessage::Read, source.status() & QMailMessage::Read);
    message.setStatus(QMailMessage::Important, source.status() & QMailMessage::Important);
    message.setRestoreFolderId(source.restoreFolderId());

    // Need to set these status fields as ImapClient::messageFetched only updates them for newly retrieved mails
    message.setStatus(QMailMessage::Incoming, source.status() & QMailMessage::Incoming);
    message.setStatus(QMailMessage::Outgoing, source.status() & QMailMessage::Outgoing);
    message.setStatus(QMailMessage::Draft, source.status() & QMailMessage::Draft);
    message.setStatus(QMailMessage::Sent, source.status() & QMailMessage::Sent);
    message.setStatus(QMailMessage::Junk, source.status() & QMailMessage::Junk);
    message.setStatus(QMailMessage::CalendarInvitation, source.hasCalendarInvitation());

    // Need to set content scheme and identifier to prevent file leaks
    message.setContentScheme(source.contentScheme());
    message.setContentIdentifier(source.contentIdentifier());
    
    // Don't emit new message notification for copied (or moved/externalized) message
    message.setStatus(QMailMessage::NoNotification, true);
}

void ImapCopyMessagesStrategy::copyNextMessage(ImapStrategyContextBase *context)
{
    if (selectNextMessageSequence(context, 1)) {
        const QString &messageUid(_messageUids.first());
        ++_messageCount;

        _transferState = Copy;

        if (messageUid.startsWith("id:")) {
            // This message does not exist on the server - append it
            QMailMessageId id(messageUid.mid(3).toULongLong());
            context->protocol().sendAppend(_destination, id);
        } else if (!context->mailbox().id.isValid()) {
            // This message is in the destination folder, which we have closed
            QMailMessageMetaData metaData(messageUid, context->config().id());
            context->protocol().sendAppend(_destination, metaData.id());

            // The existing message should be removed once we have appended the new message
            _obsoleteDestinationUids.append(ImapProtocol::uid(messageUid));
        } else {
            // Copy this message from its current location to the destination
            context->protocol().sendUidCopy(ImapProtocol::uid(messageUid), _destination);
        }

        _sourceUids.append(messageUid);
    }
}

void ImapCopyMessagesStrategy::removeObsoleteUids(ImapStrategyContextBase *context)
{
    if (!_obsoleteDestinationUids.isEmpty()) {
        context->protocol().sendUidStore(MFlag_Deleted, true, IntegerRegion(_obsoleteDestinationUids).toString());
        _obsoleteDestinationUids.clear();
    } else {
        fetchNextCopy(context);
    }
}

void ImapCopyMessagesStrategy::fetchNextCopy(ImapStrategyContextBase *context)
{
    if (!_createdUids.isEmpty()) {
        QString firstUid = ImapProtocol::uid(_createdUids.takeFirst());
        context->protocol().sendUidFetch(MetaDataFetchFlags, firstUid);
    } else {
        messageListCompleted(context);
    }
}

void ImapCopyMessagesStrategy::selectMessageSet(ImapStrategyContextBase *context)
{
    if (!_messageSets.isEmpty()) {
        const QPair<QMailMessageIdList, QMailFolderId> &set(_messageSets.first());

        selectedMailsAppend(set.first);
        resetMessageListTraversal();

        _destination = QMailFolder(set.second);

        _messageSets.takeFirst();

        _transferState = Init;
        _obsoleteDestinationUids.clear();

        // We need to select the destination folder to reset the Recent list
        if (_destination.id() == context->mailbox().id) {
            // We already have the appropriate mailbox selected
            handleSelect(context);
        } else {
            selectFolder(context, _destination);
        }
    } else {
        // Nothing more to do
        ImapFetchSelectedMessagesStrategy::messageListCompleted(context);
    }
}


/* A strategy to move selected messages.
*/
// Note: the move strategy is:
// 1. Select the destination folder, resetting Recent
// 2. Copy each specified message from its current folder to the destination, recording the local ID
// 3. Mark each specified message as deleted
// 4. Close each modified folder to expunge the marked messages
// 5. Update the status for each modified folder
// 6. Select the destination folder
// 7. Search for recent messages
// 8. Retrieve metadata only for found messages
// 9. When the metadata for a copy is fetched, move the local body of the source message into the copy

void ImapMoveMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_Examine:
        {
            handleExamine(context);
            break;
        }
        
        default:
        {
            ImapCopyMessagesStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapMoveMessagesStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    ImapCopyMessagesStrategy::messageFlushed(context, message);
    if (_error)
        return;

    QMailMessageId sourceId = _messagesToRemove.take(message.serverUid());
    if (!sourceId.isValid())
        return;

    if (!QMailStore::instance()->removeMessage(sourceId, QMailStore::NoRemovalRecord)) {
        _error = true;
        qWarning() << "Unable to remove message for account:" << context->config().id() << "ID:" << sourceId;
    }
}

void ImapMoveMessagesStrategy::handleUidCopy(ImapStrategyContextBase *context)
{
    // Mark the copied message(s) as deleted
    context->protocol().sendUidStore(MFlag_Deleted, true, numericUidSequence(_messageUids));
}

void ImapMoveMessagesStrategy::handleUidStore(ImapStrategyContextBase *context)
{
    if ((_transferState == Copy) || (_transferState == Complete)) {
        _lastMailbox = _currentMailbox;
        messageListMessageAction(context);
    } else {
        ImapCopyMessagesStrategy::handleUidStore(context);
    }
}

void ImapMoveMessagesStrategy::handleClose(ImapStrategyContextBase *context)
{
    if (_transferState == Copy) {
        context->protocol().sendExamine(_lastMailbox);
        _lastMailbox = QMailFolder();
    } else {
        ImapCopyMessagesStrategy::handleClose(context);
    }
}

void ImapMoveMessagesStrategy::handleExamine(ImapStrategyContextBase *context)
{
    // Select the next folder
    ImapCopyMessagesStrategy::messageListFolderAction(context);
}

void ImapMoveMessagesStrategy::handleUidFetch(ImapStrategyContextBase *context)
{
    // See if we can move the body of the source message to this message
    fetchNextCopy(context);
}

void ImapMoveMessagesStrategy::messageListFolderAction(ImapStrategyContextBase *context)
{
    // If we're already in a mailbox, we need to close it to expunge the messages
    if ((context->mailbox().isSelected()) && (_lastMailbox.id().isValid())) {
        context->protocol().sendClose();
    } else {
        ImapCopyMessagesStrategy::messageListFolderAction(context);
    }
}

void ImapMoveMessagesStrategy::messageListMessageAction(ImapStrategyContextBase *context)
{
    if (_messageCount < _listSize) {
        context->updateStatus( QObject::tr("Moving %1 / %2").arg(_messageCount + 1).arg(_listSize) );
    }

    copyNextMessage(context);
}

void ImapMoveMessagesStrategy::updateCopiedMessage(ImapStrategyContextBase *context, QMailMessage &message, const QMailMessage &source)
{ 
    ImapCopyMessagesStrategy::updateCopiedMessage(context, message, source);
    _messagesToRemove[message.serverUid()] = source.id();

    // Move the content of the source message to the new message
    if (!transferMessageData(message, source)) {
        _error = true;
        qWarning() << "Unable to transfer message data";
        return;
    }

    QMailDisconnected::clearPreviousFolder(&message);
}


/* A strategy to make selected messages available from the external server.
*/
// 1. Select and then close the destination folder, resetting the Recent flag
// 2. Append each specified message to the destination folder
// 3. Select the destination folder
// 4. Search for recent messages 
// 5. Replace the original local message with the remote one
// 6. Generate authorized URLs for each message

void ImapExternalizeMessagesStrategy::appendMessageSet(const QMailMessageIdList &messageIds, const QMailFolderId& folderId)
{
    if (folderId.isValid()) {
        ImapCopyMessagesStrategy::appendMessageSet(messageIds, folderId);
    } else {
        // These messages can't be externalised - just remove the flag to proceed as normal
        QMailMessageKey idsKey(QMailMessageKey::id(messageIds));
        if (!QMailStore::instance()->updateMessagesMetaData(idsKey, QMailMessage::TransmitFromExternal, false)) {
            _error = true;
            qWarning() << "Unable to update message metadata to remove transmit from external flag";
        }
    }
}

void ImapExternalizeMessagesStrategy::newConnection(ImapStrategyContextBase *context)
{
    _urlIds.clear();

    if (!_messageSets.isEmpty()) {
        ImapCopyMessagesStrategy::newConnection(context);
    } else {
        // Nothing to do
        context->operationCompleted();
    }
}

void ImapExternalizeMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_GenUrlAuth:
        {
            handleGenUrlAuth(context);
            break;
        }
        
        default:
        {
            ImapCopyMessagesStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapExternalizeMessagesStrategy::handleGenUrlAuth(ImapStrategyContextBase *context)
{
    // We're finished with the previous location
    _urlIds.removeFirst();

    resolveNextMessage(context);
}

bool ImapExternalizeMessagesStrategy::messageFetched(ImapStrategyContextBase *context, QMailMessage &message)
{ 
    copiedMessageFetched(context, message);

    return ImapFetchSelectedMessagesStrategy::messageFetched(context, message);
}

void ImapExternalizeMessagesStrategy::messageFlushed(ImapStrategyContextBase *context, QMailMessage &message)
{
    ImapCopyMessagesStrategy::messageFlushed(context, message);
    if (_error) return;

    _urlIds.append(message.id());
}

void ImapExternalizeMessagesStrategy::urlAuthorized(ImapStrategyContextBase *, const QString &url)
{
    const QMailMessageId &id(_urlIds.first());

    // We now have an authorized URL for this message
    QMailMessage message(id);
    message.setExternalLocationReference(url);

    if (!QMailStore::instance()->updateMessage(&message)) {
        _error = true;
        qWarning() << "Unable to update message for account:" << message.parentAccountId();
    }
}

void ImapExternalizeMessagesStrategy::updateCopiedMessage(ImapStrategyContextBase *context, QMailMessage &message, const QMailMessage &source)
{ 
    // Update the source message with the new 
    ImapCopyMessagesStrategy::updateCopiedMessage(context, message, source);

    // Move the content of the source message to the new message
    if (!transferMessageData(message, source)) {
        _error = true;
        qWarning() << "Unable to transfer message data";
        return;
    }

    // Replace the source message with the new message
    message.setId(source.id());

    if (source.status() & QMailMessage::Outbox) {
        message.setStatus(QMailMessage::Outbox, true);
    }
    if (source.status() & QMailMessage::TransmitFromExternal) {
        message.setStatus(QMailMessage::TransmitFromExternal, true);
    }
}

void ImapExternalizeMessagesStrategy::messageListCompleted(ImapStrategyContextBase *context)
{
    resolveNextMessage(context);
}

void ImapExternalizeMessagesStrategy::resolveNextMessage(ImapStrategyContextBase *context)
{
    if (!_urlIds.isEmpty()) {
        const QMailMessageId &id(_urlIds.first());

        // Generate an authorized URL for this message
        QMailMessagePart::Location location;
        location.setContainingMessageId(id);
        context->protocol().sendGenUrlAuth(location, false);
    } else {
        ImapCopyMessagesStrategy::messageListCompleted(context);
    }
}


/* A strategy to flag selected messages.
*/
void ImapFlagMessagesStrategy::newConnection(ImapStrategyContextBase *context)
{
    if (!_listSize) {
        // Nothing to do
        context->operationCompleted();
    }
    ImapFetchSelectedMessagesStrategy::newConnection(context);
}

void ImapFlagMessagesStrategy::clearSelection()
{
    _setMask = 0;
    _unsetMask = 0;
    _outstandingStores = 0;

    ImapFetchSelectedMessagesStrategy::clearSelection();
}

void ImapFlagMessagesStrategy::setMessageFlags(MessageFlags flags, bool set)
{
    if (set) {
        _setMask |= flags;
    } else {
        _unsetMask |= flags;
    }
}

void ImapFlagMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_UIDStore:
        {
            handleUidStore(context);
            break;
        }
        
        default:
        {
            ImapFetchSelectedMessagesStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapFlagMessagesStrategy::handleUidStore(ImapStrategyContextBase *context)
{
    --_outstandingStores;
    if (_outstandingStores == 0) {
        messageListMessageAction(context);
    }
}

void ImapFlagMessagesStrategy::messageListMessageAction(ImapStrategyContextBase *context)
{
    const int batchSize = 100;
    if (selectNextMessageSequence(context, batchSize)) {
        QString uidSequence(numericUidSequence(_messageUids));
        if (_setMask) {
            context->protocol().sendUidStore(_setMask, true, uidSequence);
            ++_outstandingStores;
        }
        if (_unsetMask) {
            context->protocol().sendUidStore(_unsetMask, false, uidSequence);
            ++_outstandingStores;
        }
        context->progressChanged(0, 0); // Don't timeout
    }
}


/* A strategy to delete selected messages.
*/
void ImapDeleteMessagesStrategy::setLocalMessageRemoval(bool removal)
{
    _removal = removal;

    setMessageFlags(MFlag_Deleted, true);
}

void ImapDeleteMessagesStrategy::clearSelection()
{
    _storedList.clear();
    _lastMailbox = QMailFolder();

    ImapFlagMessagesStrategy::clearSelection();
}

void ImapDeleteMessagesStrategy::transition(ImapStrategyContextBase *context, ImapCommand command, OperationStatus status)
{
    switch( command ) {
        case IMAP_Close:
        {
            handleClose(context);
            break;
        }
        
        case IMAP_Examine:
        {
            handleExamine(context);
            break;
        }
        
        default:
        {
            ImapFlagMessagesStrategy::transition(context, command, status);
            break;
        }
    }
}

void ImapDeleteMessagesStrategy::handleUidStore(ImapStrategyContextBase *context)
{
    if (_outstandingStores == 1) {
        // Add the stored UIDs to our list
        _storedList += _messageUids;
        _lastMailbox = _currentMailbox;
    }

    ImapFlagMessagesStrategy::handleUidStore(context);
}

void ImapDeleteMessagesStrategy::handleClose(ImapStrategyContextBase *context)
{
    if (_removal) {
        // All messages in the stored list are now expunged - delete the local copies
        QMailMessageKey accountKey(QMailMessageKey::parentAccountId(context->config().id()));
        QMailMessageKey uidKey(QMailMessageKey::serverUid(_storedList));

        if (!QMailStore::instance()->removeMessages(accountKey & uidKey, QMailStore::NoRemovalRecord)) {
            _error = true;
            qWarning() << "Unable to remove message for account:" << context->config().id() << "UIDs:" << _storedList;
        }
    }

    // We need to examine the closed mailbox to find the new server counts
    context->protocol().sendExamine(_lastMailbox);
    _lastMailbox = QMailFolder();
}

void ImapDeleteMessagesStrategy::handleExamine(ImapStrategyContextBase *context)
{
    // Select the next folder
    ImapFlagMessagesStrategy::messageListFolderAction(context);
}

void ImapDeleteMessagesStrategy::messageListFolderAction(ImapStrategyContextBase *context)
{
    // If we're already in a mailbox, we need to close it to expunge the messages
    if ((context->mailbox().isSelected()) && (_lastMailbox.id().isValid())) {
        context->protocol().sendClose();
    } else {
        // Select the next folder, and clear the stored list
        _storedList.clear();
        ImapFlagMessagesStrategy::messageListFolderAction(context);
    }
}

void ImapDeleteMessagesStrategy::messageListCompleted(ImapStrategyContextBase *context)
{
    // If we're already in a mailbox, we need to close it to expunge the messages
    if ((context->mailbox().isSelected()) && (_lastMailbox.id().isValid())) {
        context->protocol().sendClose();
    } else {
        ImapFlagMessagesStrategy::messageListCompleted(context);
    }
}


ImapStrategyContext::ImapStrategyContext(ImapClient *client)
  : ImapStrategyContextBase(client), 
    _strategy(0) 
{
}

