/****************************************************************************
**
** Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
** Contact: Qt Software Information (qt-info@nokia.com)
**
** This file is part of the Qt Messaging Framework.
**
** $QT_BEGIN_LICENSE:LGPL$
** No Commercial Usage
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the either Technology Preview License Agreement or the
** Beta Release License Agreement.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain
** additional rights. These rights are described in the Nokia Qt LGPL
** Exception version 1.0, included in the file LGPL_EXCEPTION.txt in this
** package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
** $QT_END_LICENSE$
**
****************************************************************************/

#include "imapservice.h"
#include "imapsettings.h"
#include "imapconfiguration.h"
#include "imapstrategy.h"
#include <QtPlugin>
#include <QTimer>
#include <qmaillog.h>
#include <qmailmessage.h>

namespace { const QString serviceKey("imap4"); }


class ImapService::Source : public QMailMessageSource
{
    Q_OBJECT

public:
    Source(ImapService *service)
        : QMailMessageSource(service),
          _service(service),
          _flagsCheckQueued(false),
          _queuedMailCheckInProgress(false),
          _mailCheckPhase(RetrieveFolders),
          _unavailable(false),
          _synchronizing(false),
          _actionCompletedSignal(0)
    {
        connect(&_service->_client, SIGNAL(allMessagesReceived()), this, SIGNAL(newMessagesAvailable()));
        connect(&_service->_client, SIGNAL(messageActionCompleted(QString)), this, SLOT(messageActionCompleted(QString)));
        connect(&_service->_client, SIGNAL(retrievalCompleted()), this, SLOT(retrievalCompleted()));
        connect(&_service->_client, SIGNAL(idleNewMailNotification(QMailFolderId)), this, SLOT(queueMailCheck(QMailFolderId)));
        connect(&_service->_client, SIGNAL(idleFlagsChangedNotification(QMailFolderId)), this, SLOT(queueFlagsChangedCheck()));
        connect(&_intervalTimer, SIGNAL(timeout()), this, SLOT(queueMailCheckAll()));
    }
    
    void setIntervalTimer(int interval)
    {
        _intervalTimer.stop();
        if (interval > 0)
            _intervalTimer.start(interval*1000*60); // interval minutes
    }

    virtual QMailStore::MessageRemovalOption messageRemovalOption() const { return QMailStore::CreateRemovalRecord; }

public slots:
    virtual bool retrieveFolderList(const QMailAccountId &accountId, const QMailFolderId &folderId, bool descending);
    virtual bool retrieveMessageList(const QMailAccountId &accountId, const QMailFolderId &folderId, uint minimum, const QMailMessageSortKey &sort);

    virtual bool retrieveMessages(const QMailMessageIdList &messageIds, QMailRetrievalAction::RetrievalSpecification spec);
    virtual bool retrieveMessagePart(const QMailMessagePart::Location &partLocation);

    virtual bool retrieveMessageRange(const QMailMessageId &messageId, uint minimum);
    virtual bool retrieveMessagePartRange(const QMailMessagePart::Location &partLocation, uint minimum);

    virtual bool retrieveAll(const QMailAccountId &accountId);
    virtual bool exportUpdates(const QMailAccountId &accountId);

    virtual bool synchronize(const QMailAccountId &accountId);

    virtual bool deleteMessages(const QMailMessageIdList &ids);

    virtual bool copyMessages(const QMailMessageIdList &ids, const QMailFolderId &destinationId);
    virtual bool moveMessages(const QMailMessageIdList &ids, const QMailFolderId &destinationId);

    void messageActionCompleted(const QString &uid);
    void retrievalCompleted();
    void retrievalTerminated();
    void queueMailCheckAll();
    void queueMailCheck(QMailFolderId folderId);
    void queueFlagsChangedCheck();
    
private:
    virtual bool setStrategy(ImapStrategy *strategy, QMailMessageSource::MessageSignal signal = 0);

    enum MailCheckPhase { RetrieveFolders = 0, RetrieveMessages, CheckFlags };

    ImapService *_service;
    bool _flagsCheckQueued;
    bool _queuedMailCheckInProgress;
    MailCheckPhase _mailCheckPhase;
    QMailFolderId _mailCheckFolderId;
    bool _unavailable;
    bool _synchronizing;
    QTimer _intervalTimer;
    QList<QMailFolderId> _queuedFolders;

    QMailMessageSource::MessageSignal _actionCompletedSignal;
};

bool ImapService::Source::retrieveFolderList(const QMailAccountId &accountId, const QMailFolderId &folderId, bool descending)
{
    if (!accountId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No account specified"));
        return false;
    }

    _service->_client.strategyContext()->foldersOnlyStrategy.setBase(folderId);
    _service->_client.strategyContext()->foldersOnlyStrategy.setDescending(descending);
    _service->_client.strategyContext()->foldersOnlyStrategy.clearSelection();
    return setStrategy(&_service->_client.strategyContext()->foldersOnlyStrategy);
}

bool ImapService::Source::retrieveMessageList(const QMailAccountId &accountId, const QMailFolderId &folderId, uint minimum, const QMailMessageSortKey &sort)
{
    if (!accountId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No account specified"));
        return false;
    }

    if (!sort.isEmpty()) {
        qWarning() << "IMAP Search sorting not yet implemented!";
    }
    
    QMailFolderIdList folderIds;
    if (folderId.isValid()) {
        folderIds.append(folderId);
    } else {
        // Retrieve messages for all folders in the account that have undiscovered messages
        QMailFolderKey accountKey(QMailFolderKey::parentAccountId(accountId));
        QMailFolderKey undiscoveredKey(QMailFolderKey::serverUndiscoveredCount(0, QMailDataComparator::GreaterThan));

        folderIds = QMailStore::instance()->queryFolders(accountKey & undiscoveredKey, QMailFolderSortKey::id(Qt::AscendingOrder));
    }

    _service->_client.strategyContext()->retrieveMessageListStrategy.setMinimum(minimum);
    _service->_client.strategyContext()->retrieveMessageListStrategy.clearSelection();
    _service->_client.strategyContext()->retrieveMessageListStrategy.selectedFoldersAppend(folderIds);
    return setStrategy(&_service->_client.strategyContext()->retrieveMessageListStrategy);
}

bool ImapService::Source::retrieveMessages(const QMailMessageIdList &messageIds, QMailRetrievalAction::RetrievalSpecification spec)
{
    if (messageIds.isEmpty()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No messages to retrieve"));
        return false;
    }

    if (spec == QMailRetrievalAction::Flags) {
        _service->_client.strategyContext()->updateMessagesFlagsStrategy.clearSelection();
        _service->_client.strategyContext()->updateMessagesFlagsStrategy.selectedMailsAppend(messageIds);
        return setStrategy(&_service->_client.strategyContext()->updateMessagesFlagsStrategy);
    }

    _service->_client.strategyContext()->selectedStrategy.setOperation(spec);
    _service->_client.strategyContext()->selectedStrategy.clearSelection();
    _service->_client.strategyContext()->selectedStrategy.selectedMailsAppend(messageIds);
    return setStrategy(&_service->_client.strategyContext()->selectedStrategy);
}

bool ImapService::Source::retrieveMessagePart(const QMailMessagePart::Location &partLocation)
{
    if (!partLocation.containingMessageId().isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No message to retrieve"));
        return false;
    }
    if (!partLocation.isValid(false)) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No part specified"));
        return false;
    }
    if (!QMailMessage(partLocation.containingMessageId()).id().isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("Invalid message specified"));
        return false;
    }

    _service->_client.strategyContext()->selectedStrategy.setOperation(QMailRetrievalAction::Content);
    _service->_client.strategyContext()->selectedStrategy.clearSelection();
    _service->_client.strategyContext()->selectedStrategy.selectedSectionsAppend(partLocation);
    return setStrategy(&_service->_client.strategyContext()->selectedStrategy);
}

bool ImapService::Source::retrieveMessageRange(const QMailMessageId &messageId, uint minimum)
{
    if (!messageId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No message to retrieve"));
        return false;
    }
    if (!QMailMessage(messageId).id().isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("Invalid message specified"));
        return false;
    }
    // Not tested yet
    if (!minimum) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No minimum specified"));
        return false;
    }
    
    QMailMessagePart::Location location;
    location.setContainingMessageId(messageId);

    _service->_client.strategyContext()->selectedStrategy.setOperation(QMailRetrievalAction::Content);
    _service->_client.strategyContext()->selectedStrategy.clearSelection();
    _service->_client.strategyContext()->selectedStrategy.selectedSectionsAppend(location, minimum);
    return setStrategy(&_service->_client.strategyContext()->selectedStrategy);
}

bool ImapService::Source::retrieveMessagePartRange(const QMailMessagePart::Location &partLocation, uint minimum)
{
    if (!partLocation.containingMessageId().isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No message to retrieve"));
        return false;
    }
    if (!partLocation.isValid(false)) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No part specified"));
        return false;
    }
    if (!QMailMessage(partLocation.containingMessageId()).id().isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("Invalid message specified"));
        return false;
    }
    if (!minimum) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No minimum specified"));
        return false;
    }

    _service->_client.strategyContext()->selectedStrategy.setOperation(QMailRetrievalAction::Content);
    _service->_client.strategyContext()->selectedStrategy.clearSelection();
    _service->_client.strategyContext()->selectedStrategy.selectedSectionsAppend(partLocation, minimum);
    return setStrategy(&_service->_client.strategyContext()->selectedStrategy);
}

bool ImapService::Source::retrieveAll(const QMailAccountId &accountId)
{
    if (!accountId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No account specified"));
        return false;
    }

    _service->_client.strategyContext()->retrieveAllStrategy.setBase(QMailFolderId());
    _service->_client.strategyContext()->retrieveAllStrategy.setDescending(true);
    _service->_client.strategyContext()->retrieveAllStrategy.clearSelection();
    _service->_client.strategyContext()->retrieveAllStrategy.setOperation(QMailRetrievalAction::MetaData);
    return setStrategy(&_service->_client.strategyContext()->retrieveAllStrategy);
}

bool ImapService::Source::exportUpdates(const QMailAccountId &accountId)
{
    if (!accountId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No account specified"));
        return false;
    }

    _service->_client.strategyContext()->exportUpdatesStrategy.clearSelection();
    return setStrategy(&_service->_client.strategyContext()->exportUpdatesStrategy);
}

bool ImapService::Source::synchronize(const QMailAccountId &accountId)
{
    if (!accountId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No account specified"));
        return false;
    }

    _service->_client.strategyContext()->synchronizeAccountStrategy.setBase(QMailFolderId());
    _service->_client.strategyContext()->synchronizeAccountStrategy.setDescending(true);
    _service->_client.strategyContext()->synchronizeAccountStrategy.clearSelection();
    _service->_client.strategyContext()->synchronizeAccountStrategy.setOperation(QMailRetrievalAction::MetaData);
    return setStrategy(&_service->_client.strategyContext()->synchronizeAccountStrategy);
}

bool ImapService::Source::deleteMessages(const QMailMessageIdList &messageIds)
{
    if (messageIds.isEmpty()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No messages to delete"));
        return false;
    }

    QMailAccountConfiguration accountCfg(_service->accountId());
    ImapConfiguration imapCfg(accountCfg);
    if (imapCfg.canDeleteMail()) {
        // Delete the messages from the server
        _service->_client.strategyContext()->deleteMessagesStrategy.setLocalMessageRemoval(true);
        _service->_client.strategyContext()->deleteMessagesStrategy.clearSelection();
        _service->_client.strategyContext()->deleteMessagesStrategy.selectedMailsAppend(messageIds);
        return setStrategy(&_service->_client.strategyContext()->deleteMessagesStrategy, deletedSignal());
    }

    // Just delete the local copies
    return QMailMessageSource::deleteMessages(messageIds);
}

bool ImapService::Source::copyMessages(const QMailMessageIdList &messageIds, const QMailFolderId &destinationId)
{
    if (messageIds.isEmpty()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No messages to copy"));
        return false;
    }
    if (!destinationId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("Invalid destination folder"));
        return false;
    }

    QMailFolder destination(destinationId);
    if (destination.parentAccountId() == _service->accountId()) {
        _service->_client.strategyContext()->copyMessagesStrategy.clearSelection();
        _service->_client.strategyContext()->copyMessagesStrategy.selectedMailsAppend(messageIds);
        _service->_client.strategyContext()->copyMessagesStrategy.setDestination(destinationId);
        return setStrategy(&_service->_client.strategyContext()->copyMessagesStrategy, copiedSignal());
    }

    // Otherwise create local copies
    return QMailMessageSource::copyMessages(messageIds, destinationId);
}

bool ImapService::Source::moveMessages(const QMailMessageIdList &messageIds, const QMailFolderId &destinationId)
{
    if (messageIds.isEmpty()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("No messages to copy"));
        return false;
    }
    if (!destinationId.isValid()) {
        _service->errorOccurred(QMailServiceAction::Status::ErrInvalidData, tr("Invalid destination folder"));
        return false;
    }

    QMailFolder destination(destinationId);
    if (destination.parentAccountId() == _service->accountId()) {
        _service->_client.strategyContext()->moveMessagesStrategy.clearSelection();
        _service->_client.strategyContext()->moveMessagesStrategy.selectedMailsAppend(messageIds);
        _service->_client.strategyContext()->moveMessagesStrategy.setDestination(destinationId);
        return setStrategy(&_service->_client.strategyContext()->moveMessagesStrategy, movedSignal());
    }

    // Otherwise - if any of these messages are in folders on the server, we should remove them
    QMailMessageIdList serverMessages;

    // Do we need to remove these messages from the server?
    QMailAccountConfiguration accountCfg(_service->accountId());
    ImapConfiguration imapCfg(accountCfg);
    if (imapCfg.canDeleteMail()) {
        QMailFolderKey accountFoldersKey(QMailFolderKey::parentAccountId(_service->accountId()));
        serverMessages = QMailStore::instance()->queryMessages(QMailMessageKey::id(messageIds) & QMailMessageKey::parentFolderId(accountFoldersKey));
        if (!serverMessages.isEmpty()) {
            // Delete the messages from the server
            _service->_client.strategyContext()->deleteMessagesStrategy.setLocalMessageRemoval(false);
            _service->_client.strategyContext()->deleteMessagesStrategy.clearSelection();
            _service->_client.strategyContext()->deleteMessagesStrategy.selectedMailsAppend(serverMessages);
            setStrategy(&_service->_client.strategyContext()->deleteMessagesStrategy);
        }
    }

    // Move the local copies
    QMailMessageMetaData metaData;
    metaData.setParentFolderId(destinationId);

    // Clear the server UID, because it won't refer to anything useful...
    metaData.setServerUid(QString());

    QMailMessageKey idsKey(QMailMessageKey::id(messageIds));
    if (!QMailStore::instance()->updateMessagesMetaData(idsKey, QMailMessageKey::ParentFolderId | QMailMessageKey::ServerUid, metaData)) {
        qWarning() << "Unable to update message metadata for move to folder:" << destinationId;
    } else {
        if (destinationId != QMailFolderId(QMailFolder::TrashFolder)) {
            // If they were in Trash folder before, they are trash no longer
            if (!QMailStore::instance()->updateMessagesMetaData(idsKey, QMailMessage::Trash, false)) {
                qWarning() << "Unable to mark messages as not trash!";
            }
        }
    }

    if (serverMessages.isEmpty()) {
        QTimer::singleShot(0, this, SLOT(retrievalCompleted()));
    }
    return true;
}

bool ImapService::Source::setStrategy(ImapStrategy *strategy, QMailMessageSource::MessageSignal signal)
{
    _actionCompletedSignal = signal;
    _unavailable = true;
    _service->_client.setStrategy(strategy);
    _service->_client.newConnection();
    return true;
}

void ImapService::Source::messageActionCompleted(const QString &uid)
{
    if (_actionCompletedSignal) {
        QMailMessageMetaData metaData(uid, _service->accountId());
        if (metaData.id().isValid()) {
            emit (this->*_actionCompletedSignal)(QMailMessageIdList() << metaData.id());
        }
    }
}

void ImapService::Source::retrievalCompleted()
{
    _unavailable = false;

    if (_queuedMailCheckInProgress) {
        if (_mailCheckPhase == RetrieveFolders) {
            _mailCheckPhase = RetrieveMessages;
            retrieveMessageList(_service->accountId(), _mailCheckFolderId, 1, QMailMessageSortKey());
        } else {
            _queuedMailCheckInProgress = false;
            emit _service->availabilityChanged(true);
        }
    }

    emit _service->activityChanged(QMailServiceAction::Successful);
    emit _service->actionCompleted(true);

    if (_synchronizing) {
        _synchronizing = false;

        // Mark this account as synchronized
        QMailAccount account(_service->accountId());
        if (!(account.status() & QMailAccount::Synchronized)) {
            account.setStatus(QMailAccount::Synchronized, true);
            QMailStore::instance()->updateAccount(&account);
        }
    }

    if (!_queuedFolders.isEmpty()) {
        queueMailCheck(_queuedFolders.first());
    }
    if (_flagsCheckQueued) {
        queueFlagsChangedCheck();
    }
}

void ImapService::Source::queueMailCheckAll()
{
    _flagsCheckQueued = true; //Also check for flag changes
    queueMailCheck(QMailFolderId());
}

void ImapService::Source::queueMailCheck(QMailFolderId folderId)
{
    if (_unavailable) {
        if (!_queuedFolders.contains(folderId)) {
            _queuedFolders.append(folderId);
        }
        return;
    }

    _queuedFolders.removeAll(folderId);
    _queuedMailCheckInProgress = true;
    _mailCheckPhase = RetrieveFolders;
    _mailCheckFolderId = folderId;

    emit _service->availabilityChanged(false);
    retrieveFolderList(_service->accountId(), folderId, true);
}

void ImapService::Source::queueFlagsChangedCheck()
{
    if (_unavailable) {
        _flagsCheckQueued = true;
        return;
    }
    
    _flagsCheckQueued = false;
    _queuedMailCheckInProgress = true;
    _mailCheckPhase = CheckFlags;

    emit _service->availabilityChanged(false);
    
    // Check same messages as last time
    setStrategy(&_service->_client.strategyContext()->updateMessagesFlagsStrategy);
}

void ImapService::Source::retrievalTerminated()
{
    _unavailable = false;
    _synchronizing = false;
    if (_queuedMailCheckInProgress) {
        _queuedMailCheckInProgress = false;
        emit _service->availabilityChanged(true);
    }
    
    // Just give up if an error occurs
    _queuedFolders.clear();
    _flagsCheckQueued = false;
}


ImapService::ImapService(const QMailAccountId &accountId)
    : QMailMessageService(),
      _client(this),
      _source(new Source(this))
{
    connect(&_client, SIGNAL(progressChanged(uint, uint)), this, SIGNAL(progressChanged(uint, uint)));

    connect(&_client, SIGNAL(errorOccurred(int, QString)), this, SLOT(errorOccurred(int, QString)));
    connect(&_client, SIGNAL(errorOccurred(QMailServiceAction::Status::ErrorCode, QString)), this, SLOT(errorOccurred(QMailServiceAction::Status::ErrorCode, QString)));
    connect(&_client, SIGNAL(updateStatus(QString)), this, SLOT(updateStatus(QString)));

    _client.setAccount(accountId);
    QMailAccountConfiguration accountCfg(accountId);
    ImapConfiguration imapCfg(accountCfg);
    if (ImapConfiguration(imapCfg).pushEnabled())
        QTimer::singleShot(0, _source, SLOT(queueMailCheckAll()));
    _source->setIntervalTimer(imapCfg.checkInterval());
}

ImapService::~ImapService()
{
    delete _source;
}

QString ImapService::service() const
{
    return serviceKey;
}

QMailAccountId ImapService::accountId() const
{
    return _client.account();
}

bool ImapService::hasSource() const
{
    return true;
}

QMailMessageSource &ImapService::source() const
{
    return *_source;
}

bool ImapService::available() const
{
    return true;
}

bool ImapService::cancelOperation()
{
    _client.cancelTransfer();
    _client.closeConnection();
    _source->retrievalTerminated();
    return true;
}

void ImapService::errorOccurred(int code, const QString &text)
{
    _source->retrievalTerminated();
    updateStatus(code, text, _client.account());
    emit actionCompleted(false);
}

void ImapService::errorOccurred(QMailServiceAction::Status::ErrorCode code, const QString &text)
{
    _source->retrievalTerminated();
    updateStatus(code, text, _client.account());
    emit actionCompleted(false);
}

void ImapService::updateStatus(const QString &text)
{
    updateStatus(QMailServiceAction::Status::ErrNoError, text, _client.account());
}


class ImapConfigurator : public QMailMessageServiceConfigurator
{
public:
    ImapConfigurator();
    ~ImapConfigurator();

    virtual QString service() const;
    virtual QString displayName() const;

    virtual QMailMessageServiceEditor *createEditor(QMailMessageServiceFactory::ServiceType type);
};

ImapConfigurator::ImapConfigurator()
{
}

ImapConfigurator::~ImapConfigurator()
{
}

QString ImapConfigurator::service() const
{
    return serviceKey;
}

QString ImapConfigurator::displayName() const
{
    return qApp->translate("QMailMessageService", "IMAP");
}

QMailMessageServiceEditor *ImapConfigurator::createEditor(QMailMessageServiceFactory::ServiceType type)
{
    if (type == QMailMessageServiceFactory::Source)
        return new ImapSettings;

    return 0;
}

Q_EXPORT_PLUGIN2(imap,ImapServicePlugin)

ImapServicePlugin::ImapServicePlugin()
    : QMailMessageServicePlugin()
{
}

QString ImapServicePlugin::key() const
{
    return serviceKey;
}

bool ImapServicePlugin::supports(QMailMessageServiceFactory::ServiceType type) const
{
    return (type == QMailMessageServiceFactory::Source);
}

bool ImapServicePlugin::supports(QMailMessage::MessageType type) const
{
    return (type == QMailMessage::Email);
}

QMailMessageService *ImapServicePlugin::createService(const QMailAccountId &id)
{
    return new ImapService(id);
}

QMailMessageServiceConfigurator *ImapServicePlugin::createServiceConfigurator()
{
    return new ImapConfigurator();
}


#include "imapservice.moc"

