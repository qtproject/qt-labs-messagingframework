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

#include "qmailserviceaction_p.h"
#include "qmailipc.h"
#include "qmailmessagekey.h"
#include "qmailstore.h"
#include "qmaillog.h"
#include "qmaildisconnected.h"
#include "qmailnamespace.h"
#include <qmailcontentmanager.h>
#include <QCoreApplication>
#include <QPair>
#include <QTimer>
#include <QDir>
#include <QTemporaryFile>

namespace {

// NOTE: messageserver ServiceHandler::dispatchRequest() relies on internal format defined below.
uint messageCounter = 0;
const uint pid = static_cast<uint>(QCoreApplication::applicationPid() & 0xffffffff);

quint64 nextMessageAction()
{
    return ((quint64(pid) << 32) | quint64(++messageCounter));
}

QPair<uint, uint> messageActionParts(quint64 action)
{
    return qMakePair(uint(action >> 32), uint(action & 0xffffffff));
}

}


template<typename Subclass>
QMailServiceActionPrivate::QMailServiceActionPrivate(Subclass *p, QMailServiceAction *i)
    : QPrivateNoncopyableBase(p),
      _interface(i),
      _server(new QMailMessageServer(this)),
      _connectivity(QMailServiceAction::Offline),
      _activity(QMailServiceAction::Pending),
      _status(QMailServiceAction::Status::ErrNoError, QString(), QMailAccountId(), QMailFolderId(), QMailMessageId()),
      _total(0),
      _progress(0),
      _isValid(false),
      _action(0),
      _connectivityChanged(false),
      _activityChanged(false),
      _progressChanged(false),
      _statusChanged(false)
{
    connect(_server, SIGNAL(activityChanged(quint64, QMailServiceAction::Activity)),
            this, SLOT(activityChanged(quint64, QMailServiceAction::Activity)));
    connect(_server, SIGNAL(connectivityChanged(quint64, QMailServiceAction::Connectivity)),
            this, SLOT(connectivityChanged(quint64, QMailServiceAction::Connectivity)));
    connect(_server, SIGNAL(statusChanged(quint64, const QMailServiceAction::Status)),
            this, SLOT(statusChanged(quint64, const QMailServiceAction::Status)));
    connect(_server, SIGNAL(progressChanged(quint64, uint, uint)),
            this, SLOT(progressChanged(quint64, uint, uint)));
    connect(_server, SIGNAL(connectionDown()),
            this, SLOT(serverFailure()));
    connect(_server, SIGNAL(reconnectionTimeout()),
            this, SLOT(serverFailure()));
}

QMailServiceActionPrivate::~QMailServiceActionPrivate()
{
}

void QMailServiceActionPrivate::cancelOperation()
{
    Q_ASSERT(_action != 0 && _isValid);
    if (_isValid) {
        if (_pendingActions.count()) {
            _pendingActions.first().action->cancelOperation();
            clearSubActions();
        }
        _server->cancelTransfer(_action);
    }
}

void QMailServiceActionPrivate::setAction(quint64 action)
{
    _isValid = action != 0;
    _action = action;
}

void QMailServiceActionPrivate::activityChanged(quint64 action, QMailServiceAction::Activity activity)
{
    if (validAction(action)) {
        setActivity(activity);

        emitChanges();
    }
}

void QMailServiceActionPrivate::connectivityChanged(quint64 action, QMailServiceAction::Connectivity connectivity)
{
    if (validAction(action)) {
        setConnectivity(connectivity);

        emitChanges();
    }
}

void QMailServiceActionPrivate::statusChanged(quint64 action, const QMailServiceAction::Status status)
{
    if (validAction(action)) {
        setStatus(status);

        emitChanges();
    }
}

void QMailServiceActionPrivate::progressChanged(quint64 action, uint progress, uint total)
{
    if (validAction(action)) {
        setProgress(progress, total);

        emitChanges();
    }
}

void QMailServiceActionPrivate::subActionConnectivityChanged(QMailServiceAction::Connectivity c)
{
    connectivityChanged(_action, c);
}

void QMailServiceActionPrivate::subActionActivityChanged(QMailServiceAction::Activity a)
{
    if (a == QMailServiceAction::Failed) {
        clearSubActions();
    }
    if (a == QMailServiceAction::Successful) {
        if (_pendingActions.count()) {
            disconnectSubAction(_pendingActions.first().action);
            // Don't delete QObject while it's emitting a signal
            _pendingActions.first().action->deleteLater();
            _pendingActions.removeFirst();
        }
        if (_pendingActions.count()) {
            // Composite action does not finish until all sub actions finish
            _activityChanged = false;
            executeNextSubAction();
            return;
        }
    }
    activityChanged(_action, a);
}

void QMailServiceActionPrivate::subActionStatusChanged(const QMailServiceAction::Status &s)
{
    statusChanged(_action, s);
}

void QMailServiceActionPrivate::subActionProgressChanged(uint value, uint total)
{
    progressChanged(_action, value, total);
}

void QMailServiceActionPrivate::connectSubAction(QMailServiceAction *subAction)
{
    connect(subAction, SIGNAL(connectivityChanged(QMailServiceAction::Connectivity)),
            this, SLOT(subActionConnectivityChanged(QMailServiceAction::Connectivity)));
    connect(subAction, SIGNAL(activityChanged(QMailServiceAction::Activity)),
            this, SLOT(subActionActivityChanged(QMailServiceAction::Activity)));
    connect(subAction, SIGNAL(statusChanged(const QMailServiceAction::Status &)),
            this, SLOT(subActionStatusChanged(const QMailServiceAction::Status &)));
    connect(subAction, SIGNAL(progressChanged(uint, uint)),
            this, SLOT(subActionProgressChanged(uint, uint)));
}

void QMailServiceActionPrivate::disconnectSubAction(QMailServiceAction *subAction)
{
    disconnect(subAction, SIGNAL(connectivityChanged(QMailServiceAction::Connectivity)),
            this, SLOT(subActionConnectivityChanged(QMailServiceAction::Connectivity)));
    disconnect(subAction, SIGNAL(activityChanged(QMailServiceAction::Activity)),
            this, SLOT(subActionActivityChanged(QMailServiceAction::Activity)));
    disconnect(subAction, SIGNAL(statusChanged(const QMailServiceAction::Status &)),
            this, SLOT(subActionStatusChanged(const QMailServiceAction::Status &)));
    disconnect(subAction, SIGNAL(progressChanged(uint, uint)),
            this, SLOT(subActionProgressChanged(uint, uint)));
}

void QMailServiceActionPrivate::clearSubActions()
{
        if (_pendingActions.count())
            disconnectSubAction(_pendingActions.first().action);
        foreach(ActionCommand a, _pendingActions) {
            // Don't delete QObject while it's emitting a signal
            a.action->deleteLater();
        }
        _pendingActions.clear();
}

void QMailServiceActionPrivate::serverFailure()
{
    if (_isValid && _activity != QMailServiceAction::Failed) {
        _activity = QMailServiceAction::Failed;
        _activityChanged = true;
        emitChanges();
    }
}

void QMailServiceActionPrivate::init()
{
    _connectivity = QMailServiceAction::Offline;
    _activity = QMailServiceAction::Pending;
    _status = QMailServiceAction::Status(QMailServiceAction::Status::ErrNoError, QString(), QMailAccountId(), QMailFolderId(), QMailMessageId());
    _total = 0;
    _progress = 0;
    _action = 0;
    _connectivityChanged = false;
    _activityChanged = false;
    _progressChanged = false;
    _statusChanged = false;
    _isValid = false;
    _pendingActions.clear();
}

quint64 QMailServiceActionPrivate::newAction()
{
    if (_isValid) {
        qWarning() << "Unable to allocate new action - oustanding:" << messageActionParts(_action).second;
        return _action;
    }

    init();

    _action = nextMessageAction();
    _isValid = true;
    _activityChanged = true;
    emitChanges();

    return _action;
}

bool QMailServiceActionPrivate::validAction(quint64 action)
{
   return (action && _action == action);
}

void QMailServiceActionPrivate::appendSubAction(QMailServiceAction *subAction, QSharedPointer<QMailServiceActionCommand> command)
{
    ActionCommand a;
    a.action = subAction;
    a.command = command;
    _pendingActions.append(a);
}

void QMailServiceActionPrivate::executeNextSubAction()
{
    if (_pendingActions.isEmpty())
        return;

    connectSubAction(_pendingActions.first().action);
    _pendingActions.first().command->execute();
}

void QMailServiceActionPrivate::setConnectivity(QMailServiceAction::Connectivity newConnectivity)
{
    if (_isValid && (_connectivity != newConnectivity)) {
        _connectivity = newConnectivity;
        _connectivityChanged = true;
    }
}

void QMailServiceActionPrivate::setActivity(QMailServiceAction::Activity newActivity)
{
    if (_isValid && (_activity != newActivity)) {
        _activity = newActivity;

        if (_activity == QMailServiceAction::Failed || _activity == QMailServiceAction::Successful) {
            // We're finished
            _isValid = false;
        }

        _activityChanged = true;
    }
}

void QMailServiceActionPrivate::setStatus(const QMailServiceAction::Status &status)
{
    if (_isValid) {
        _status = status;
        _statusChanged = true;
    }
}

void QMailServiceActionPrivate::setStatus(QMailServiceAction::Status::ErrorCode code, const QString &text)
{
    setStatus(code, text, QMailAccountId(), QMailFolderId(), QMailMessageId());
}

void QMailServiceActionPrivate::setStatus(QMailServiceAction::Status::ErrorCode code,
                                          const QString &text,
                                          const QMailAccountId &accountId,
                                          const QMailFolderId &folderId,
                                          const QMailMessageId &messageId)
{
    if (_isValid) {
        _status = QMailServiceAction::Status(code, text, accountId, folderId, messageId);
        _statusChanged = true;
    }
}

void QMailServiceActionPrivate::setProgress(uint newProgress, uint newTotal)
{
    if (_isValid) {
        if (newTotal != _total) {
            _total = newTotal;
            _progressChanged = true;
        }

        newProgress = qMin(newProgress, _total);
        if (newProgress != _progress) {
            _progress = newProgress;
            _progressChanged = true;
        }
    }
}

void QMailServiceActionPrivate::emitChanges()
{
    if (_connectivityChanged) {
        _connectivityChanged = false;
        emit _interface->connectivityChanged(_connectivity);
    }
    if (_activityChanged) {
        _activityChanged = false;
        emit _interface->activityChanged(_activity);
    }
    if (_progressChanged) {
        _progressChanged = false;
        emit _interface->progressChanged(_progress, _total);
    }
    if (_statusChanged) {
        _statusChanged = false;
        emit _interface->statusChanged(_status);
    }
}


/*!
    \class QMailServiceAction::Status

    \preliminary
    \ingroup messaginglibrary

    \brief The Status class encapsulates the instantaneous state of a QMailServiceAction.

    QMailServiceAction::Status contains the pieces of information that describe the state of a
    requested action.  The \l errorCode reflects the overall state, and may be supplemented
    by a description in \l text.

    If \l errorCode is not equal to \l ErrNoError, then each of \l accountId, \l folderId and 
    \l messageId may have been set to a valid identifier, if pertinent to the situation.
*/

/*!
    \enum QMailServiceAction::Status::ErrorCode

    This enum type is used to identify common error conditions encountered when implementing service actions.

    \value ErrNoError               No error was encountered.
    \value ErrNotImplemented        The requested operation is not implemented by the relevant service component.
    \value ErrFrameworkFault        A fault in the messaging framework prevented the execution of the request.
    \value ErrSystemError           A system-level error prevented the execution of the request.
    \value ErrInternalServer        A system error on server.
    \value ErrCancel                The operation was cancelled by user intervention.
    \value ErrConfiguration         The configuration needed for the requested action is invalid.
    \value ErrNoConnection          A connection could not be established to the external service.
    \value ErrConnectionInUse       The connection to the external service is exclusively held by another user.
    \value ErrConnectionNotReady    The connection to the external service is not ready for operation.
    \value ErrUnknownResponse       The response from the external service could not be handled.
    \value ErrLoginFailed           The external service did not accept the supplied authentication details.
    \value ErrFileSystemFull        The action could not be performed due to insufficient storage space.
    \value ErrNonexistentMessage    The specified message does not exist.
    \value ErrEnqueueFailed         The specified message could not be enqueued for transmission.
    \value ErrInvalidAddress        The specified address is invalid for the requested action.
    \value ErrInvalidData           The supplied data is inappropriate for the requested action.
    \value ErrTimeout               The service failed to report any activity for an extended period.
    \value ErrInternalStateReset    The service was reset due to state change, such as an external process modifying the owner account.
    \value ErrorCodeMinimum         The lowest value of any error condition code.
    \value ErrorCodeMaximum         The highest value of any error condition code.
*/

/*! \variable QMailServiceAction::Status::errorCode
    
    Describes the error condition encountered by the action.
*/

/*! \variable QMailServiceAction::Status::text
    
    Provides a human-readable description of the error condition in \l errorCode.
*/

/*! \variable QMailServiceAction::Status::accountId
    
    If relevant to the \l errorCode, contains the ID of the associated account.
*/

/*! \variable QMailServiceAction::Status::folderId
    
    If relevant to the \l errorCode, contains the ID of the associated folder.
*/

/*! \variable QMailServiceAction::Status::messageId
    
    If relevant to the \l errorCode, contains the ID of the associated message.
*/

/*! 
    \fn QMailServiceAction::Status::Status()

    Constructs a status object with \l errorCode set to \l{QMailServiceAction::Status::ErrNoError}{ErrNoError}.
*/
QMailServiceAction::Status::Status()
    : errorCode(ErrNoError)
{
}

/*! 
    \fn QMailServiceAction::Status::Status(ErrorCode c, const QString &t, const QMailAccountId &a, const QMailFolderId &f, const QMailMessageId &m)

    Constructs a status object with 
    \l errorCode set to \a c, 
    \l text set to \a t, 
    \l accountId set to \a a, 
    \l folderId set to \a f and 
    \l messageId set to \a m.
*/
QMailServiceAction::Status::Status(ErrorCode c, const QString &t, const QMailAccountId &a, const QMailFolderId &f, const QMailMessageId &m)
    : errorCode(c),
      text(t),
      accountId(a),
      folderId(f),
      messageId(m)
{
}

/*! 
    \fn QMailServiceAction::Status::Status(const Status&)

    Constructs a status object which is a copy of \a other.
*/
QMailServiceAction::Status::Status(const QMailServiceAction::Status &other)
    : errorCode(other.errorCode),
      text(other.text),
      accountId(other.accountId),
      folderId(other.folderId),
      messageId(other.messageId)
{
}

/*! 
    \fn QMailServiceAction::Status::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailServiceAction::Status::serialize(Stream &stream) const
{
    stream << errorCode;
    stream << text;
    stream << accountId;
    stream << folderId;
    stream << messageId;
}

template void QMailServiceAction::Status::serialize(QDataStream &) const;

/*! 
    \fn QMailServiceAction::Status::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailServiceAction::Status::deserialize(Stream &stream)
{
    stream >> errorCode;
    stream >> text;
    stream >> accountId;
    stream >> folderId;
    stream >> messageId;
}

template void QMailServiceAction::Status::deserialize(QDataStream &);

/*!
    \class QMailServiceAction

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailServiceAction class provides the interface for requesting actions from external message services.

    QMailServiceAction provides the mechanisms for messaging clients to request actions from 
    external services, and to receive information in response.  The details of requests
    differ for each derived action, and the specific data returned is also variable.  However,
    all actions present the same interface for communicating status, connectivity and 
    progress information.

    All actions communicate changes in their operational state by emitting the activityChanged()
    signal.  If the execution of the action requires connectivity to an external service, then
    changes in connectivity state are emitted via the connectivityChanged() signal.  Some actions
    are able to provide progress information as they execute; these changes are reported via the 
    progressChanged() signal.  If error conditions are encountered, the statusChanged() signal is
    emitted to report the new status.

    A user may attempt to cancel an operation after it has been initiated.  The cancelOperation()
    slot is provided for this purpose.

    A QMailServiceAction instance supports only a single request at any time. Attempting 
    to initiate an operation on a QMailServiceAction instace while another operation is already 
    in progress for that action is not supported.
    
    An application may however use multiple QMailServiceAction instances to send independent 
    requests concurrently. Each QMailServiceAction instance will report only the changes 
    pertaining to the request that instance delivered.  Whether or not concurrent requests are 
    concurrently serviced by the messageserver depends on whether those requests must be 
    serviced by the same QMailMessageService instance.
    
    \sa QMailMessageService
*/

/*!
    \enum QMailServiceAction::Connectivity

    This enum type is used to describe the connectivity state of the service implementing an action.

    \value Offline          The service is offline.
    \value Connecting       The service is currently attempting to establish a connection.
    \value Connected        The service is connected.
    \value Disconnected     The service has been disconnected.
*/

/*!
    \enum QMailServiceAction::Activity

    This enum type is used to describe the activity state of the requested action.

    \value Pending          The action has not yet begun execution.
    \value InProgress       The action is currently executing.
    \value Successful       The action has completed successfully.
    \value Failed           The action could not be completed successfully, and has finished execution.
*/

/*!
    \fn QMailServiceAction::connectivityChanged(QMailServiceAction::Connectivity c)

    This signal is emitted when the connectivity status of the service performing 
    the action changes, with the new state described by \a c.

    \sa connectivity()
*/

/*!
    \fn QMailServiceAction::activityChanged(QMailServiceAction::Activity a)

    This signal is emitted when the activity status of the action changes,
    with the new state described by \a a.

    \sa activity()
*/

/*!
    \fn QMailServiceAction::statusChanged(const QMailServiceAction::Status &s)

    This signal is emitted when the error status of the action changes, with
    the new status described by \a s.

    \sa status()
*/

/*!
    \fn QMailServiceAction::progressChanged(uint value, uint total)

    This signal is emitted when the progress of the action changes, with
    the new state described by \a value and \a total.

    \sa progress()
*/

/*!
    \typedef QMailServiceAction::ImplementationType
    \internal
*/

/*!
    \fn QMailServiceAction::QMailServiceAction(Subclass*, QObject*)
    \internal
*/
template<typename Subclass>
QMailServiceAction::QMailServiceAction(Subclass* p, QObject *parent)
    : QObject(parent),
      QPrivatelyNoncopyable<QMailServiceActionPrivate>(p)
{
}

/*! \internal */
QMailServiceAction::~QMailServiceAction()
{
}

/*!
    Returns the current connectivity state of the service implementing this action.

    \sa connectivityChanged()
*/
QMailServiceAction::Connectivity QMailServiceAction::connectivity() const
{
    return impl(this)->_connectivity;
}

/*!
    Returns the current activity state of the action.

    \sa activityChanged()
*/
QMailServiceAction::Activity QMailServiceAction::activity() const
{
    return impl(this)->_activity;
}

/*!
    Returns the current status of the service action.

    \sa statusChanged()
*/
const QMailServiceAction::Status QMailServiceAction::status() const
{
    return impl(this)->_status;
}

/*!
    Returns the current progress of the service action.

    \sa progressChanged()
*/
QPair<uint, uint> QMailServiceAction::progress() const
{
    return qMakePair(impl(this)->_progress, impl(this)->_total);
}

/*!
    Returns if the service action is currently running (i.e. if it's doing something, but hasn't yet finished)
*/

bool QMailServiceAction::isRunning() const
{
    return impl(this)->_isValid;
}

/*!
    Attempts to cancel the last requested operation.
*/
void QMailServiceAction::cancelOperation()
{
    impl(this)->cancelOperation();
}

/*!
    Set the current status so that 
    \l{QMailServiceAction::Status::errorCode} errorCode is set to \a c and 
    \l{QMailServiceAction::Status::text} text is set to \a t.
*/
void QMailServiceAction::setStatus(QMailServiceAction::Status::ErrorCode c, const QString &t)
{
    impl(this)->setStatus(c, t, QMailAccountId(), QMailFolderId(), QMailMessageId());
}

/*!
    Set the current status so that 
    \l{QMailServiceAction::Status::errorCode} errorCode is set to \a c, 
    \l{QMailServiceAction::Status::text} text is set to \a t,
    \l{QMailServiceAction::Status::accountId} accountId is set to \a a, 
    \l{QMailServiceAction::Status::folderId} folderId is set to \a f and 
    \l{QMailServiceAction::Status::messageId} messageId is set to \a m.
*/
void QMailServiceAction::setStatus(QMailServiceAction::Status::ErrorCode c, const QString &t, const QMailAccountId &a, const QMailFolderId &f, const QMailMessageId &m)
{
    impl(this)->setStatus(c, t, a, f, m);
}


QMailRetrievalActionPrivate::QMailRetrievalActionPrivate(QMailRetrievalAction *i)
    : QMailServiceActionPrivate(this, i)
{
    connect(_server, SIGNAL(retrievalCompleted(quint64)),
            this, SLOT(retrievalCompleted(quint64)));

    init();
}

void QMailRetrievalActionPrivate::retrieveFolderListHelper(const QMailAccountId &accountId, const QMailFolderId &folderId, bool descending)
{
    _server->retrieveFolderList(newAction(), accountId, folderId, descending);
}

void QMailRetrievalActionPrivate::retrieveFolderList(const QMailAccountId &accountId, const QMailFolderId &folderId, bool descending)
{
    Q_ASSERT(!_pendingActions.count());
    retrieveFolderListHelper(accountId, folderId, descending);
}

void QMailRetrievalActionPrivate::retrieveMessageListHelper(const QMailAccountId &accountId, const QMailFolderId &folderId, uint minimum, const QMailMessageSortKey &sort)
{
    _server->retrieveMessageList(newAction(), accountId, folderId, minimum, sort);
}

void QMailRetrievalActionPrivate::retrieveMessageList(const QMailAccountId &accountId, const QMailFolderId &folderId, uint minimum, const QMailMessageSortKey &sort)
{
    Q_ASSERT(!_pendingActions.count());
    retrieveMessageListHelper(accountId, folderId, minimum, sort);
}

void QMailRetrievalActionPrivate::retrieveMessageLists(const QMailAccountId &accountId, const QMailFolderIdList &folderIds, uint minimum, const QMailMessageSortKey &sort)
{
    Q_ASSERT(!_pendingActions.count());
    _server->retrieveMessageLists(newAction(), accountId, folderIds, minimum, sort);
}

void QMailRetrievalActionPrivate::retrieveNewMessages(const QMailAccountId &accountId, const QMailFolderIdList &folderIds)
{
    Q_ASSERT(!_pendingActions.count());
    _server->retrieveNewMessages(newAction(), accountId, folderIds);
}

void QMailRetrievalActionPrivate::createStandardFolders(const QMailAccountId &accountId)
{
    Q_ASSERT(!_pendingActions.count());

    QMailAccount account(accountId);

    if (!QMail::detectStandardFolders(accountId)) {
        if (!(account.status() & QMailAccount::CanCreateFolders)) {
            qMailLog(Messaging) << "Unable to create folders for account: " << accountId;
            if (validAction(newAction())) {
                setActivity(QMailServiceAction::Successful);
                emitChanges();
            }
            return;
        }
        else {
            _server->createStandardFolders(newAction(), accountId);
        }
    }
    else {
        qMailLog(Messaging) << "Standard folders matched for account: " << accountId;
        if (validAction(newAction())) {
            setActivity(QMailServiceAction::Successful);
            emitChanges();
        }
    }
}

void QMailRetrievalActionPrivate::retrieveMessages(const QMailMessageIdList &messageIds, QMailRetrievalAction::RetrievalSpecification spec)
{
    _server->retrieveMessages(newAction(), messageIds, spec);
}

void QMailRetrievalActionPrivate::retrieveMessagePart(const QMailMessagePart::Location &partLocation)
{
    _server->retrieveMessagePart(newAction(), partLocation);
}

void QMailRetrievalActionPrivate::retrieveMessageRange(const QMailMessageId &messageId, uint minimum)
{
    _server->retrieveMessageRange(newAction(), messageId, minimum);
}

void QMailRetrievalActionPrivate::retrieveMessagePartRange(const QMailMessagePart::Location &partLocation, uint minimum)
{
    _server->retrieveMessagePartRange(newAction(), partLocation, minimum);
}

void QMailRetrievalActionPrivate::retrieveAll(const QMailAccountId &accountId)
{
    _server->retrieveAll(newAction(), accountId);
}

void QMailRetrievalActionPrivate::exportUpdatesHelper(const QMailAccountId &accountId)
{
    _server->exportUpdates(newAction(), accountId);
}

void QMailRetrievalActionPrivate::exportUpdates(const QMailAccountId &accountId)
{
    Q_ASSERT(!_pendingActions.count());
    exportUpdatesHelper(accountId);
}

void QMailRetrievalActionPrivate::synchronizeAllHelper(const QMailAccountId &accountId)
{
    _server->synchronize(newAction(), accountId);
}

void QMailRetrievalActionPrivate::synchronizeAll(const QMailAccountId &accountId)
{
    Q_ASSERT(!_pendingActions.count());
    synchronizeAllHelper(accountId);
}

void QMailRetrievalActionPrivate::synchronize(const QMailAccountId &accountId, uint minimum)
{
    Q_ASSERT(!_pendingActions.count());
    newAction();
    
    QMailRetrievalAction *exportAction = new QMailRetrievalAction();
    QMailExportUpdatesCommand *exportCommand = new QMailExportUpdatesCommand(exportAction->impl(exportAction), accountId);
    appendSubAction(exportAction, QSharedPointer<QMailServiceActionCommand>(exportCommand));

    QMailRetrievalAction *retrieveFolderListAction = new QMailRetrievalAction();
    QMailRetrieveFolderListCommand *retrieveFolderListCommand = new QMailRetrieveFolderListCommand(retrieveFolderListAction->impl(retrieveFolderListAction), accountId);
    appendSubAction(retrieveFolderListAction, QSharedPointer<QMailServiceActionCommand>(retrieveFolderListCommand));
    
    QMailRetrievalAction *retrieveMessageListAction = new QMailRetrievalAction();
    QMailRetrieveMessageListCommand *retrieveMessageListCommand = new QMailRetrieveMessageListCommand(retrieveMessageListAction->impl(retrieveMessageListAction), accountId, minimum);
    appendSubAction(retrieveMessageListAction, QSharedPointer<QMailServiceActionCommand>(retrieveMessageListCommand));
    
    executeNextSubAction();
}

void QMailRetrievalActionPrivate::retrievalCompleted(quint64 action)
{
    if (validAction(action)) {
        setActivity(QMailServiceAction::Successful);
        emitChanges();
    }
}


/*!
    \class QMailRetrievalAction

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailRetrievalAction class provides the interface for retrieving messages from external message services.

    QMailRetrievalAction provides the mechanism for messaging clients to request that the message
    server retrieve messages from external services.  The retrieval action object reports on
    the progress and outcome of its activities via the signals inherited from QMailServiceAction.

    A range of functions are available to support varying client operations:

    The retrieveFolderList() function allows a client to retrieve the list of folders available for an account.
    The retrieveMessageList() and retrieveMessageLists() functions allows a client to retrieve a subset of messages available for an account or folder.

    The retrieveMessages() function allows a client to retrieve the flags, meta data or content of a 
    specific list of messages.

    The retrieveMessageRange() functions allows a portion of a message's content to be retrieved, rather than
    the entire content data.

    The retrieveMessagePart() function allows a specific part of a multi-part message to be retrieved.
    The retrieveMessagePartRange() function allows a portion of a specific part of a multi-part message to be retrieved.

    The exportUpdates() function allows a client to push local changes such as message-read notifications 
    and pending disconnected operations to the external server.
    
    All of these functions require the device to be online, they may initiate communication 
    with external servers.

    A QMailRetrievalAction instance supports only a single request at any time. Attempting 
    to initiate an operation on a QMailRetrievalAction instace while another operation is already 
    in progress for that action is not supported.
    
    An application may however use multiple QMailRetrievalAction instances to send independent 
    requests concurrently. Each QMailServiceAction instance will report only the changes 
    pertaining to the request that instance delivered.  Whether or not concurrent requests are 
    concurrently serviced by the messageserver depends on whether those requests must be 
    serviced by the same QMailMessageService instance.    
*/

/*!
    \enum QMailRetrievalAction::RetrievalSpecification

    This enum type is specify the form of retrieval that the message server should perform.

    \value Flags        Changes to the state of the message should be retrieved.
    \value MetaData     Only the meta data of the message should be retrieved.
    \value Content      The entire content of the message should be retrieved.
    \value Auto         Account specific settings are used during synchronization.
                        For example, this may mean skipping the attachments.
*/

/*!
    \typedef QMailRetrievalAction::ImplementationType
    \internal
*/

/*! 
    Constructs a new retrieval action object with the supplied \a parent.
*/
QMailRetrievalAction::QMailRetrievalAction(QObject *parent)
    : QMailServiceAction(new QMailRetrievalActionPrivate(this), parent)
{
}

/*! \internal */
QMailRetrievalAction::~QMailRetrievalAction()
{
}

/*!
    Requests that the message server retrieve the list of folders available for the 
    account \a accountId.  If \a folderId is valid, only the identified folder is 
    searched for child folders; otherwise the search begins at the root of the
    account.  If \a descending is true, the search should also recursively search 
    for child folders within folders discovered during the search.

    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for each 
    folder that is searched for child folders; these properties are not updated 
    for folders that are merely discovered by searching.
    
    This function requires the device to be online, it may initiate communication 
    with external servers.

    \sa retrieveMessageList(),  retrieveMessageLists()
*/
void QMailRetrievalAction::retrieveFolderList(const QMailAccountId &accountId, const QMailFolderId &folderId, bool descending)
{
    impl(this)->retrieveFolderList(accountId, folderId, descending);
}

/*!
    Requests that the message server retrieve the list of messages available for the account \a accountId.
    If \a folderId is valid, then only messages within that folder should be retrieved; otherwise 
    messages within all folders in the account should be retrieved, and the lastSynchronized() time 
    of the account updated.  If \a minimum is non-zero, then that value will be used to restrict the 
    number of messages to be retrieved from each folder.
    
    If \a sort is not empty, the external service will report the discovered messages in the 
    ordering indicated by the sort criterion, if possible.  Services are not required to support 
    this facility.

    If a folder messages are being retrieved from contains at least \a minimum messages then the 
    messageserver should ensure that at least \a minimum messages are available from the mail 
    store for that folder; otherwise if the folder contains less than \a minimum messages the 
    messageserver should ensure all the messages for that folder are available from the mail store.
    If a folder has messages locally available, then all previously undiscovered messages will be
    retrieved for that folder, even if that number exceeds \a minimum.
    
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for each folder 
    from which messages are retrieved.
    
    New messages will be added to the mail store as they are discovered, and 
    marked with the \l QMailMessage::New status flag. Messages in folders inspected that 
    are present in the mail store but found to be no longer available are marked with the 
    \l QMailMessage::Removed status flag. The status flags of messages in folders inspected 
    that are present in the mail store will be updated including the QMailMessage::Read and 
    QMailMessage::Important flags.
    
    This function requires the device to be online, it may initiate communication 
    with external servers.

    \sa QMailAccount::lastSynchronized()
*/
void QMailRetrievalAction::retrieveMessageList(const QMailAccountId &accountId, const QMailFolderId &folderId, uint minimum, const QMailMessageSortKey &sort)
{
    impl(this)->retrieveMessageList(accountId, folderId, minimum, sort);
}

/*!
    Requests that the message server retrieve the list of messages available for the account \a accountId.
    If \a folderIds is not empty, then only messages within those folders should be retrieved and the 
    lastSynchronized() time of the account updated; otherwise 
    no messages should be retrieved, .  If \a minimum is non-zero, then that value will be used to restrict the 
    number of messages to be retrieved from each folder.
    
    If \a sort is not empty, the external service will report the discovered messages in the 
    ordering indicated by the sort criterion, if possible.  Services are not required to support 
    this facility.

    If a folder messages are being retrieved from contains at least \a minimum messages then the 
    messageserver should ensure that at least \a minimum messages are available from the mail 
    store for that folder; otherwise if the folder contains less than \a minimum messages the 
    messageserver should ensure all the messages for that folder are available from the mail store.
    If a folder has messages locally available, then all previously undiscovered messages will be
    retrieved for that folder, even if that number exceeds \a minimum.
    
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for each folder 
    from which messages are retrieved.
    
    New messages will be added to the mail store as they are discovered, and 
    marked with the \l QMailMessage::New status flag. Messages in folders inspected that 
    are present in the mail store but found to be no longer available are marked with the 
    \l QMailMessage::Removed status flag. The status flags of messages in folders inspected 
    that are present in the mail store will be updated including the QMailMessage::Read and 
    QMailMessage::Important flags.
    
    This function requires the device to be online, it may initiate communication 
    with external servers.

    \sa QMailAccount::lastSynchronized()
*/
void QMailRetrievalAction::retrieveMessageLists(const QMailAccountId &accountId, const QMailFolderIdList &folderIds, uint minimum, const QMailMessageSortKey &sort)
{
    if (folderIds.isEmpty()) {
        // nothing to do
        impl(this)->newAction();
        impl(this)->setActivity(QMailServiceAction::Successful);
        impl(this)->emitChanges();
        return;
    }

    impl(this)->retrieveMessageLists(accountId, folderIds, minimum, sort);
}

/*
    Requests that the message server retrieve new messages for the account \a accountId in the
    folders specified by \a folderIds.
    
    If a folder inspected has been previously inspected then new mails in that folder will
    be retrieved, otherwise the most recent message in that folder, if any, will be retrieved.
    
    This function is intended for use by protocol plugins to retrieve new messages when
    a push notification event is received from the remote server.
    
    Detection of deleted messages, and flag updates for messages in the mail store will
    not necessarily be performed.    
*/
void QMailRetrievalAction::retrieveNewMessages(const QMailAccountId &accountId, const QMailFolderIdList &folderIds)
{
    impl(this)->retrieveNewMessages(accountId, folderIds);
}

/*!
    Requests that the message server create the standard folders for the
    account \a accountId. If all standard folders are already set in the storage
    the service action will return success immediately, in case some standard folders are
    not set, a matching attempt against a predefined list of translations will be made,
    if the folders can't be matched, messageserver will try to create them in the server side
    and match them if the creation is successful. In case folder creation is not allowed for
    the account \a accountId the service action will still complete successfully, clients
    must use local standard folders only in this case.

    \sa retrieveFolderList
*/
void QMailRetrievalAction::createStandardFolders(const QMailAccountId &accountId)
{
    impl(this)->createStandardFolders(accountId);
}

/*!
    Requests that the message server retrieve data regarding the messages identified by \a messageIds.  

    If \a spec is \l QMailRetrievalAction::Flags, then the message server should detect if 
    the messages identified by \a messageIds have been marked as read or have been removed.
    Messages that have been read will be marked with the \l QMailMessage::ReadElsewhere flag, and
    messages that have been removed will be marked with the \l QMailMessage::Removed status flag.

    If \a spec is \l QMailRetrievalAction::MetaData, then the message server should 
    retrieve the meta data of the each message listed in \a messageIds.
    
    If \a spec is \l QMailRetrievalAction::Content, then the message server should 
    retrieve the entirety of each message listed in \a messageIds.
    
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for each folder 
    from which messages are retrieved.

    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailRetrievalAction::retrieveMessages(const QMailMessageIdList &messageIds, RetrievalSpecification spec)
{
    impl(this)->retrieveMessages(messageIds, spec);
}

/*!
    Requests that the message server retrieve the message part that is indicated by the 
    location \a partLocation.
    
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for the folder 
    from which the part is retrieved.

    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailRetrievalAction::retrieveMessagePart(const QMailMessagePart::Location &partLocation)
{
    impl(this)->retrieveMessagePart(partLocation);
}

/*!
    Requests that the message server retrieve a subset of the message \a messageId, such that
    at least \a minimum bytes are available from the mail store.
    
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for the folder 
    from which the message is retrieved.

    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailRetrievalAction::retrieveMessageRange(const QMailMessageId &messageId, uint minimum)
{
    impl(this)->retrieveMessageRange(messageId, minimum);
}

/*!
    Requests that the message server retrieve a subset of the message part that is indicated 
    by the location \a partLocation.  The messageserver should ensure that at least 
    \a minimum bytes are available from the mail store.
    
    The total size of the part on the server is given by QMailMessagePart::contentDisposition().size(),
    the amount of the part available locally is given by QMailMessagePart::body().length().
    
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for the folder 
    from which the part is retrieved.

    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailRetrievalAction::retrieveMessagePartRange(const QMailMessagePart::Location &partLocation, uint minimum)
{
    impl(this)->retrieveMessagePartRange(partLocation, minimum);
}

/*!
    \deprecated
  
    Requests that the message server retrieve all folders and meta data for messages available 
    for the account \a accountId.
    
    All folders within the account will be discovered and searched for child folders.
    The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for each folder 
    in the account.

    Meta data will be retrieved for every message found in the account.
    New messages will be added to the mail store as they are retrieved, and 
    marked with the \l QMailMessage::New status flag.  Messages that are no longer 
    available will be marked with the \l QMailMessage::Removed status flag.  

    This function requires the device to be online, it may initiate communication 
    with external servers.

    \sa retrieveFolderList(), retrieveMessageList(), retrieveMessageLists()
*/
void QMailRetrievalAction::retrieveAll(const QMailAccountId &accountId)
{
    impl(this)->retrieveAll(accountId);
}

/*!
    Requests that the message server update the external server with changes that have 
    been effected on the local device for account \a accountId.
    Local changes to \l QMailMessage::Read, and \l QMailMessage::Important message status 
    flags should be exported to the external server, messages that have been removed
    using the \l QMailStore::CreateRemovalRecord option should be removed from the 
    external server, and any flag, copy or move operations that have been applied
    using \l QMailDisconnected should be applied to the external server.

    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailRetrievalAction::exportUpdates(const QMailAccountId &accountId)
{
    if (!QMailDisconnected::updatesOutstanding(accountId)) {
        // nothing to do
        impl(this)->newAction();
        impl(this)->setActivity(QMailServiceAction::Successful);
        impl(this)->emitChanges();
        return;
    }
        
    impl(this)->exportUpdates(accountId);
}

/*!
    \deprecated
  
    Requests that the message server synchronize the set of known folder and message 
    identifiers with those currently available for the account identified by \a accountId.
    Newly discovered messages should have their meta data retrieved, messages that have been 
    removed locally using the \l QMailStore::CreateRemovalRecord option should be removed 
    from the external server.
    
    Changes to the \l QMailMessage::Read, and \l QMailMessage::Important status flags of a 
    message should be exported to the external server, and the status flags of the message
    should be updated to reflect any changes to the message on the external server.

    New messages will be added to the mail store as they are discovered, and 
    marked with the \l QMailMessage::New status flag.  Messages that are no longer 
    available will be marked with the \l QMailMessage::Removed status flag.  

    The folder structure of the account will be synchronized with that available from 
    the external server.  The QMailFolder::serverCount(), QMailFolder::serverUnreadCount() and 
    QMailFolder::serverUndiscoveredCount() properties will be updated for each folder.

    This function requires the device to be online, it may initiate communication 
    with external servers.

    \sa retrieveFolderList(), retrieveMessageList(), retrieveMessageLists(), exportUpdates()
*/
void QMailRetrievalAction::synchronizeAll(const QMailAccountId &accountId)
{
    impl(this)->synchronizeAll(accountId);
}


/*!
    Essentially performs the same functions as calling exportUpdates(), retrieveFolderList() 
    and retrieveMessageList() consecutively using the given \a accountId and \a minimum, but 
    may perform optimizations such as retrieving the folder list at the same time as 
    retrieving messages, and retrieving messages in multiple folder simultaneously by using 
    multiple connections to the server.
    
    On a successful synchronization the lastSynchronized() value of the account should be updated.

    This function requires the device to be online, it may initiate communication 
    with external servers.

    \sa exportUpdates(), retrieveMessageList(), retrieveFolderList(), QMailAccount::lastSynchronized() 
*/
void QMailRetrievalAction::synchronize(const QMailAccountId &accountId, uint minimum)
{
    impl(this)->synchronize(accountId, minimum);
}


/*!
    \fn QMailRetrievalAction::defaultMinimum()

    Default minimum argument clients and protocol plugins should use when 
    retrieving messages. Maybe be used by protocol plugins to determine if 
    either new messages are being retrieved or if more messages are being 
    retrieved.

    \sa retrieveMessageList()
*/


QMailTransmitActionPrivate::QMailTransmitActionPrivate(QMailTransmitAction *i)
    : QMailServiceActionPrivate(this, i)
{
    connect(_server, SIGNAL(messagesTransmitted(quint64, QMailMessageIdList)),
            this, SLOT(messagesTransmitted(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(messagesFailedTransmission(quint64, QMailMessageIdList, QMailServiceAction::Status::ErrorCode)),
            this, SLOT(messagesFailedTransmission(quint64, QMailMessageIdList, QMailServiceAction::Status::ErrorCode)));
    connect(_server, SIGNAL(transmissionCompleted(quint64)),
            this, SLOT(transmissionCompleted(quint64)));

    init();
}

void QMailTransmitActionPrivate::transmitMessages(const QMailAccountId &accountId)
{
    _server->transmitMessages(newAction(), accountId);

    emitChanges();
}

void QMailTransmitActionPrivate::transmitMessage(const QMailMessageId &messageId)
{
    _server->transmitMessage(newAction(), messageId);

    emitChanges();
}

void QMailTransmitActionPrivate::init()
{
    QMailServiceActionPrivate::init();
}

void QMailTransmitActionPrivate::messagesTransmitted(quint64 action, const QMailMessageIdList &ids)
{
    if (validAction(action)) {
        emit messagesTransmitted(ids);
    }
}

void QMailTransmitActionPrivate::messagesFailedTransmission(quint64 action, const QMailMessageIdList &ids, QMailServiceAction::Status::ErrorCode error)
{
    if (validAction(action)) {
        emit messagesFailedTransmission(ids, error);
    }
}

void QMailTransmitActionPrivate::transmissionCompleted(quint64 action)
{
    if (validAction(action)) {
        QMailServiceAction::Activity result(QMailServiceAction::Successful);
        setActivity(result);
        emitChanges();
    }
}


/*!
    \class QMailTransmitAction

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailTransmitAction class provides the interface for transmitting messages to external message services.

    QMailTransmitAction provides the mechanism for messaging clients to request that the message
    server transmit messages to external services.  The transmit action object reports on
    the progress and outcome of its activities via the signals inherited from QMailServiceAction.

    The send() slot requests that the message server transmit each identified message to the
    external service associated with each messages' account.

    A QMailTransmitAction instance supports only a single request at any time. Attempting 
    to initiate an operation on a QMailTransmitAction instace while another operation is already 
    in progress for that action is not supported.
    
    An application may however use multiple QMailTransmitAction instances to send independent 
    requests concurrently. Each QMailServiceAction instance will report only the changes 
    pertaining to the request that instance delivered.  Whether or not concurrent requests are 
    concurrently serviced by the messageserver depends on whether those requests must be 
    serviced by the same QMailMessageService instance.
    
    Note: the slots exported by QMailTransmitAction are expected to change in future releases, as 
    the message server is extended to provide a finer-grained interface for message transmission.
*/

/*!
    \typedef QMailTransmitAction::ImplementationType
    \internal
*/

/*!
    \fn QMailTransmitAction::messagesTransmitted(const QMailMessageIdList &ids)

    This signal is emitted to report the successful transmission of the messages listed in \a ids.
*/

/*!
    \fn QMailTransmitAction::messagesFailedTransmission(const QMailMessageIdList &ids, QMailServiceAction::Status::ErrorCode error)

    This signal is emitted to report the failure of an attempt at transmission of the messages listed in \a ids.

    The failure is of type \a error.
*/


/*!
    Constructs a new transmit action object with the supplied \a parent.
*/
QMailTransmitAction::QMailTransmitAction(QObject *parent)
    : QMailServiceAction(new QMailTransmitActionPrivate(this), parent)
{
    connect(impl(this), SIGNAL(messagesTransmitted(QMailMessageIdList)), 
            this, SIGNAL(messagesTransmitted(QMailMessageIdList)));
    connect(impl(this), SIGNAL(messagesFailedTransmission(QMailMessageIdList, QMailServiceAction::Status::ErrorCode)), 
            this, SIGNAL(messagesFailedTransmission(QMailMessageIdList, QMailServiceAction::Status::ErrorCode)));
}

/*! \internal */
QMailTransmitAction::~QMailTransmitAction()
{
}

/*!
    Requests that the message server transmit each message belonging to the account identified
    by \a accountId that is currently scheduled for transmission (located in the Outbox folder).

    Any message successfully transmitted will be moved to the account's Sent folder.

    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailTransmitAction::transmitMessages(const QMailAccountId &accountId)
{
    impl(this)->transmitMessages(accountId);
}

/*!
    Requests that the message server transmit the message identified by
    by \a messageId that is currently scheduled for transmission (located in the Outbox folder).

    Any message successfully transmitted will be moved to the account's Sent folder.
*/
void QMailTransmitAction::transmitMessage(const QMailMessageId &messageId)
{
    impl(this)->transmitMessage(messageId);
}

QMailStorageActionPrivate::QMailStorageActionPrivate(QMailStorageAction *i)
    : QMailServiceActionPrivate(this, i)
{
    connect(_server, SIGNAL(messagesDeleted(quint64, QMailMessageIdList)),
            this, SLOT(messagesEffected(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(messagesMoved(quint64, QMailMessageIdList)),
            this, SLOT(messagesEffected(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(messagesCopied(quint64, QMailMessageIdList)),
            this, SLOT(messagesEffected(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(messagesFlagged(quint64, QMailMessageIdList)),
            this, SLOT(messagesEffected(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(messagesAdded(quint64, QMailMessageIdList)),
            this, SLOT(messagesAdded(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(messagesUpdated(quint64, QMailMessageIdList)),
            this, SLOT(messagesUpdated(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(storageActionCompleted(quint64)),
            this, SLOT(storageActionCompleted(quint64)));

    init();
}

void QMailStorageActionPrivate::onlineDeleteMessagesHelper(const QMailMessageIdList &ids)
{
    _server->onlineDeleteMessages(newAction(), ids, QMailStore::CreateRemovalRecord);
    // Successful as long as ids have been deleted, 
    // this action doesn't have to be the one to have deleted them
    _ids.clear();
    emitChanges();
}

void QMailStorageActionPrivate::onlineDeleteMessages(const QMailMessageIdList &ids)
{
    Q_ASSERT(!_pendingActions.count());
    onlineDeleteMessagesHelper(ids);
}

void QMailStorageActionPrivate::discardMessages(const QMailMessageIdList &ids)
{
    _server->onlineDeleteMessages(newAction(), ids, QMailStore::NoRemovalRecord);
    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::onlineCopyMessages(const QMailMessageIdList &ids, const QMailFolderId &destination)
{
    _server->onlineCopyMessages(newAction(), ids, destination);
    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::onlineMoveMessages(const QMailMessageIdList &ids, const QMailFolderId &destination)
{
    _server->onlineMoveMessages(newAction(), ids, destination);
    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::onlineFlagMessagesAndMoveToStandardFolder(const QMailMessageIdList &ids, quint64 setMask, quint64 unsetMask)
{
    // Ensure that nothing is both set and unset
    setMask &= ~unsetMask;
    _server->onlineFlagMessagesAndMoveToStandardFolder(newAction(), ids, setMask, unsetMask);

    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::addMessages(const QMailMessageList &list)
{
    _ids.clear();
    _addedOrUpdatedIds.clear();

    // Check to see if any of the messages has unresolved parts (forward without download)
    // If so must use sync adding
    bool fwod(false);
    foreach (QMailMessage message, list) {
        if (message.status() & (QMailMessage::HasUnresolvedReferences | QMailMessage::TransmitFromExternal)) {
            fwod = true;
            break;
        }
    }
    if (fwod) {
        foreach (QMailMessage mail, list) {
            if (!mail.id().isValid()) {
                mail.setStatus(QMailMessage::LocalOnly, true);
                QMailStore::instance()->addMessage(&mail);
            } else {
                QMailStore::instance()->updateMessage(&mail);
            }
        }

        if (validAction(newAction())) {
            setActivity(QMailServiceAction::Successful);
            emitChanges();
        }
        return;
    }

    // Make non durable changes to the content manager
    // Existing message data in mail store and content manager should not be
    // changed directly by this function, instead the messageserver should do it.
    QMailMessageMetaDataList metadata;
    foreach (QMailMessage message, list) {
        if (message.contentScheme().isEmpty()) {
            message.setContentScheme(QMailContentManagerFactory::defaultScheme());
        }
        if (QMailContentManager *contentManager = QMailContentManagerFactory::create(message.contentScheme())) {
            QMailStore::ErrorCode code = contentManager->add(&message, QMailContentManager::NoDurability);
            if (code != QMailStore::NoError) {
                qWarning() << "Unable to ensure message content durability for scheme:" << message.contentScheme();
                if (validAction(newAction())) {
                    setActivity(QMailServiceAction::Failed);
                    emitChanges();
                }
                return;
            }
            metadata.append(message);
        }
    }
    _server->addMessages(newAction(), metadata);

    emitChanges();
}

void QMailStorageActionPrivate::updateMessages(const QMailMessageList &list)
{
    _ids.clear();
    _addedOrUpdatedIds.clear();

    // Check to see if any of the messages has unresolved parts (forward without download)
    // If so must use sync updating
    bool fwod(false);
    foreach (QMailMessage message, list) {
        if (message.status() & (QMailMessage::HasUnresolvedReferences | QMailMessage::TransmitFromExternal | QMailMessage::Outgoing)) {
            fwod = true;
            break;
        }
    }
    if (fwod) {
        foreach (QMailMessage mail, list) {
            if (!mail.id().isValid()) {
                mail.setStatus(QMailMessage::LocalOnly, true);
                QMailStore::instance()->addMessage(&mail);
            } else {
                QMailStore::instance()->updateMessage(&mail);
            }
        }

        if (validAction(newAction())) {
            setActivity(QMailServiceAction::Successful);
            emitChanges();
        }
        return;
    }

    // Ensure that message non metadata is written and flushed to the filesystem,
    // but not synced
    // Existing message data in mail store and content manager should not be
    // changed directly by this function, instead the messageserver should do it.
    QMailMessageMetaDataList metadata;
    foreach (QMailMessage message, list) {
        if (message.contentScheme().isEmpty()) {
            message.setContentScheme(QMailContentManagerFactory::defaultScheme());
        }
        message.setCustomField(QStringLiteral("qmf-obsolete-contentid"), message.contentIdentifier());
        if (QMailContentManager *contentManager = QMailContentManagerFactory::create(message.contentScheme())) {
            QMailStore::ErrorCode code = contentManager->update(&message, QMailContentManager::EnsureDurability);
            if (code != QMailStore::NoError) {
                qWarning() << "Unable to ensure message content durability for scheme:" << message.contentScheme();
                if (validAction(newAction())) {
                    setActivity(QMailServiceAction::Failed);
                    emitChanges();
                }
                return;
            }
            metadata.append(message);
        }
    }
    _server->updateMessages(newAction(), metadata);

    emitChanges();
}

void QMailStorageActionPrivate::updateMessages(const QMailMessageMetaDataList &list)
{
    _ids.clear();
    _addedOrUpdatedIds.clear();

    QMailMessageMetaDataList metadata = list;
    _server->updateMessages(newAction(), metadata);

    emitChanges();
}

void QMailStorageActionPrivate::deleteMessages(const QMailMessageIdList &ids)
{
    _server->deleteMessages(newAction(), ids);

    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::rollBackUpdates(const QMailAccountId &mailAccountId)
{
    _server->rollBackUpdates(newAction(), mailAccountId);
    emitChanges();
}

void QMailStorageActionPrivate::moveToStandardFolder(const QMailMessageIdList& ids, quint64 standardFolder)
{
    _server->moveToStandardFolder(newAction(), ids, standardFolder);

    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::moveToFolder(const QMailMessageIdList& ids, const QMailFolderId& folderId)
{
    _server->moveToFolder(newAction(), ids, folderId);
                
    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::flagMessages(const QMailMessageIdList &ids, quint64 setMask, quint64 unsetMask)
{
    // Ensure that nothing is both set and unset
    setMask &= ~unsetMask;
    _server->flagMessages(newAction(), ids, setMask, unsetMask);

    _ids = ids;
    emitChanges();
}

void QMailStorageActionPrivate::restoreToPreviousFolder(const QMailMessageKey& key)
{
    _server->restoreToPreviousFolder(newAction(), key);
    emitChanges();
}

void QMailStorageActionPrivate::onlineCreateFolder(const QString &name, const QMailAccountId &accountId, const QMailFolderId &parentId)
{
    _server->onlineCreateFolder(newAction(), name, accountId, parentId);
    emitChanges();
}

void QMailStorageActionPrivate::onlineRenameFolder(const QMailFolderId &folderId, const QString &name)
{
    _server->onlineRenameFolder(newAction(), folderId, name);
    emitChanges();
}

void QMailStorageActionPrivate::onlineDeleteFolderHelper(const QMailFolderId &folderId)
{
    _server->onlineDeleteFolder(newAction(), folderId);
    emitChanges();
}

void QMailStorageActionPrivate::onlineDeleteFolder(const QMailFolderId &folderId)
{
    Q_ASSERT(!_pendingActions.count());
    onlineDeleteFolderHelper(folderId);
}

void QMailStorageActionPrivate::onlineMoveFolder(const QMailFolderId &folderId, const QMailFolderId &newParentId)
{
    _server->onlineMoveFolder(newAction(), folderId, newParentId);
    emitChanges();
}

void QMailStorageActionPrivate::init()
{
    QMailServiceActionPrivate::init();

    _ids.clear();
}

void QMailStorageActionPrivate::messagesEffected(quint64 action, const QMailMessageIdList &ids)
{
    if (validAction(action)) {
        foreach (const QMailMessageId &id, ids)
            _ids.removeAll(id);
    }
}

void QMailStorageActionPrivate::messagesAdded(quint64 action, const QMailMessageIdList &ids)
{
    if (validAction(action)) {
        _addedOrUpdatedIds.append(ids);
    }
}

void QMailStorageActionPrivate::messagesUpdated(quint64 action, const QMailMessageIdList &ids)
{
    if (validAction(action)) {
        _addedOrUpdatedIds.append(ids);
    }
}

void QMailStorageActionPrivate::storageActionCompleted(quint64 action)
{
    if (validAction(action)) {
        QMailServiceAction::Activity result(_ids.isEmpty() ? QMailServiceAction::Successful : QMailServiceAction::Failed);
        setActivity(result);
        emitChanges();
    }
}


/*!
    \class QMailStorageAction

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailStorageAction class provides the interface for requesting operations on the
    storage of messages within external message services.

    QMailStorageAction provides the mechanism for messaging clients to request that the message
    server move or copy messages within an external message service.  The storage action object 
    reports on the progress and outcome of its activities via the signals inherited from QMailServiceAction.

    The onlineCopyMessages() slot requests that the message server create a copy of each identified 
    message in the nominated destination folder.  The onlineMoveMessages() slot requests that the 
    message server move each identified message from its current location to the nominated destination 
    folder.  Messages cannot be moved or copied between accounts.
    
    A QMailStorageAction instance supports only a single request at any time. Attempting 
    to initiate an operation on a QMailStorageAction instace while another operation is already 
    in progress for that action is not supported.
    
    An application may however use multiple QMailStorageAction instances to send independent 
    requests concurrently. Each QMailServiceAction instance will report only the changes 
    pertaining to the request that instance delivered.  Whether or not concurrent requests are 
    concurrently serviced by the messageserver depends on whether those requests must be 
    serviced by the same QMailMessageService instance.
*/

/*!
    \typedef QMailStorageAction::ImplementationType
    \internal
*/

/*!
    Constructs a new transmit action object with the supplied \a parent.
*/
QMailStorageAction::QMailStorageAction(QObject *parent)
    : QMailServiceAction(new QMailStorageActionPrivate(this), parent)
{
}

/*! \internal */
QMailStorageAction::~QMailStorageAction()
{
}

/*!
    Requests that the message server delete the messages listed in \a ids, both from the local device 
    and the external message source.  Whether the external messages resources are actually removed is 
    at the discretion of the relevant QMailMessageSource object.
    
    This function requires the device to be online, it may initiate communication with external servers.
*/
void QMailStorageAction::onlineDeleteMessages(const QMailMessageIdList &ids)
{
    impl(this)->onlineDeleteMessages(ids);
}

/*!
    Requests that the message server delete the messages listed in \a ids from the local device only.
*/
void QMailStorageAction::discardMessages(const QMailMessageIdList &ids)
{
    impl(this)->discardMessages(ids);
}

/*!
    Requests that the message server create a new copy of each message listed in \a ids within the folder 
    identified by \a destinationId.

    This function requires the device to be online, it may initiate communication with external servers.
*/
void QMailStorageAction::onlineCopyMessages(const QMailMessageIdList &ids, const QMailFolderId &destinationId)
{
    impl(this)->onlineCopyMessages(ids, destinationId);
}

/*!
    Requests that the message server move each message listed in \a ids from its current location 
    to the folder identified by \a destinationId.

    This function requires the device to be online, it may initiate communication with external servers.
*/
void QMailStorageAction::onlineMoveMessages(const QMailMessageIdList &ids, const QMailFolderId &destinationId)
{
    impl(this)->onlineMoveMessages(ids, destinationId);
}

/*!
    Requests that the message server flag each message listed in \a ids, by setting any status flags
    set in the \a setMask, and unsetting any status flags set in the \a unsetMask.  The status
    flag values should correspond to those of QMailMessage::status().

    The service implementing the account may choose to take further actions in response to flag
    changes, such as moving or deleting messages.

    This function requires the device to be online, it may initiate communication with external servers.
    
    \sa QMailMessage::setStatus(), QMailStore::updateMessagesMetaData()
*/
void QMailStorageAction::onlineFlagMessagesAndMoveToStandardFolder(const QMailMessageIdList &ids, quint64 setMask, quint64 unsetMask)
{
    impl(this)->onlineFlagMessagesAndMoveToStandardFolder(ids, setMask, unsetMask);
}

/*!
    Requests that the message server add \a messages to the mail store.

    The messages will be added asynchronously.

    All messages must use the same content scheme.

    \sa QMailStorageAction::messagesAdded(), QMailMessageMetaData::contentScheme()
*/
void QMailStorageAction::addMessages(const QMailMessageList &messages)
{
    impl(this)->addMessages(messages);
}

/*!
    Returns the ids of the messages added to the mail store.
*/
QMailMessageIdList QMailStorageAction::messagesAdded() const
{
    return impl(this)->_addedOrUpdatedIds;
}

/*!
    Requests that the message server update \a messages in the mail store.

    The messages will be updated asynchronously.

    All messages must use the same content scheme.

    \sa QMailStorageAction::messagesUpdated(), QMailMessageMetaData::contentScheme()
*/
void QMailStorageAction::updateMessages(const QMailMessageList &messages)
{
    impl(this)->updateMessages(messages);
}

/*!
    Requests that the message server updates the meta data of the existing 
    messages in the message store, to match each of the messages listed in 
    \a messages.

    The messages will be updated asynchronously.

    All messages must use the same content scheme.

    \sa QMailStorageAction::messagesUpdated(), QMailMessageMetaData::contentScheme()
*/
void QMailStorageAction::updateMessages(const QMailMessageMetaDataList &messages)
{
    impl(this)->updateMessages(messages);
}

/*!
    Returns the ids of the messages updated in the mail store.
*/
QMailMessageIdList QMailStorageAction::messagesUpdated() const
{
    return impl(this)->_addedOrUpdatedIds;
}

/*!
    Asynchronously deletes the messages in \a mailList, messages
    will be removed locally from the device, and if necessary information needed 
    to delete messages from an external server is recorded.

    Deleting messages using this slot does not initiate communication with any external
    server; Deletion from the external server will occur when 
    QMailRetrievalAction::exportUpdates is called successfully.
    
    \sa QMailStore::removeMessage()
*/
void QMailStorageAction::deleteMessages(const QMailMessageIdList& mailList)
{
    emit impl(this)->deleteMessages(mailList);
}

/*!
    Asynchronous version of QMailDisconnected::rollBackUpdates()
    
    Rolls back all disconnected move and copy operations that have been applied to the 
    message store since the most recent synchronization of the message with the account 
    specified by \a mailAccountId.
    
    \sa QMailDisconnected::updatesOutstanding()
*/
void QMailStorageAction::rollBackUpdates(const QMailAccountId &mailAccountId)
{
    impl(this)->rollBackUpdates(mailAccountId);
}

/*!
    Asynchronous version of QMailDisconnected::moveToStandardFolder()

    Disconnected moves the list of messages identified by \a ids into the standard folder \a standardFolder, setting standard 
    folder flags as appropriate.
    
    The move operation will be propagated to the server by a successful call to QMailRetrievalAction::exportUpdates().
            
    \sa QMailDisconnected::moveToStandardFolder()
*/
void QMailStorageAction::moveToStandardFolder(const QMailMessageIdList& ids, QMailFolder::StandardFolder standardFolder)
{
    impl(this)->moveToStandardFolder(ids, standardFolder);
}

/*!
    Asynchronous version of QMailDisconnected::moveToFolder()

    Disconnected moves the list of messages identified by \a ids into the folder identified by \a folderId, setting standard 
    folder flags as appropriate.

    Moving to another account is not supported.

    The move operation will be propagated to the server by a successful call to QMailRetrievalAction::exportUpdates().
    
    \sa QMailDisconnected::moveToFolder()
*/
void QMailStorageAction::moveToFolder(const QMailMessageIdList& ids, const QMailFolderId& folderId)
{
    emit impl(this)->moveToFolder(ids, folderId);
}

/*!
    Asynchronous version of QMailDisconnected::flagMessages()

    Disconnected flags the list of messages identified by \a ids, setting the flags specified by the bit mask \a setMask 
    to on and setting the flags set by the bit mask \a unsetMask to off.
    
    For example this function may be used to mark messages as important.

    The flagging operation will be propagated to the server by a successful call to QMailRetrievalAction::exportUpdates().
            
    \sa QMailDisconnected::flagMessages(), QMailStorageAction::moveToFolder(), QMailStorageAction::moveToStandardFolder()
*/
void QMailStorageAction::flagMessages(const QMailMessageIdList& ids, quint64 setMask, quint64 unsetMask)
{
    emit impl(this)->flagMessages(ids, setMask, unsetMask);
}

/*!
    Asynchronous version of QMailDisconnected::restoreToPreviousFolder()

    Updates all QMailMessages identified by the key \a key to move the messages back to the
    previous folder they were contained by.
        
    \sa QMailDisconnected::restoreToPreviousFolder()
*/
void QMailStorageAction::restoreToPreviousFolder(const QMailMessageKey& key)
{
    emit impl(this)->restoreToPreviousFolder(key);
}

/*!
    Requests that the message server create a new folder named \a name, created in the
    account identified by \a accountId.
    If \a parentId is a valid folder identifier the new folder will be a child of the parent;
    otherwise the folder will be have no parent and will be created at the highest level.

    This function requires the device to be online, it may initiate communication with external servers.
    
    \sa onlineDeleteFolder()
*/
void QMailStorageAction::onlineCreateFolder(const QString &name, const QMailAccountId &accountId, const QMailFolderId &parentId)
{
    impl(this)->onlineCreateFolder(name, accountId, parentId);
}

/*!
    Requests that the message server rename the folder identified by \a folderId to \a name.

    This function requires the device to be online, it may initiate communication with external servers.
    
    \sa onlineCreateFolder(), onlineMoveFolder()
*/
void QMailStorageAction::onlineRenameFolder(const QMailFolderId &folderId, const QString &name)
{
    impl(this)->onlineRenameFolder(folderId, name);
}

/*!
    Requests that the message server delete the folder identified by \a folderId.
    Any existing folders or messages contained by the folder will also be deleted.

    This function requires the device to be online, it may initiate communication with external servers.

    \sa onlineCreateFolder(), onlineRenameFolder(), onlineMoveFolder()
*/
void QMailStorageAction::onlineDeleteFolder(const QMailFolderId &folderId)
{
    impl(this)->onlineDeleteFolder(folderId);
}

/*!
    Requests that the message server move the folder identified by \a folderId.
    If \a newParentId is a valid folder identifier the folder will be a child of the parent;
    otherwise the folder will be have no parent and will be moved to the highest level.

    This function requires the device to be online, it may initiate communication with external servers.

    \sa onlineCreateFolder(), onlineRenameFolder()
*/
void QMailStorageAction::onlineMoveFolder(const QMailFolderId &folderId, const QMailFolderId &newParentId)
{
    impl(this)->onlineMoveFolder(folderId, newParentId);
}

QMailSearchActionPrivate::QMailSearchActionPrivate(QMailSearchAction *i)
    : QMailServiceActionPrivate(this, i)
{
    connect(_server, SIGNAL(matchingMessageIds(quint64, QMailMessageIdList)),
            this, SLOT(matchingMessageIds(quint64, QMailMessageIdList)));
    connect(_server, SIGNAL(remainingMessagesCount(quint64, uint)),
            this, SLOT(remainingMessagesCount(quint64, uint)));
    connect(_server, SIGNAL(messagesCount(quint64, uint)),
            this, SLOT(messagesCount(quint64, uint)));
    connect(_server, SIGNAL(searchCompleted(quint64)),
            this, SLOT(searchCompleted(quint64)));

    init();
}

QMailSearchActionPrivate::~QMailSearchActionPrivate()
{
}

void QMailSearchActionPrivate::searchMessages(const QMailMessageKey &filter, const QString &bodyText, QMailSearchAction::SearchSpecification spec, const QMailMessageSortKey &sort)
{
    _server->searchMessages(newAction(), filter, bodyText, spec, sort);
    emitChanges();
}

void QMailSearchActionPrivate::searchMessages(const QMailMessageKey &filter, const QString &bodyText, QMailSearchAction::SearchSpecification spec, quint64 limit, const QMailMessageSortKey &sort)
{
    _server->searchMessages(newAction(), filter, bodyText, spec, limit, sort);
    emitChanges();
}

void QMailSearchActionPrivate::countMessages(const QMailMessageKey &filter, const QString &bodyText)
{
    _server->countMessages(newAction(), filter, bodyText);
    emitChanges();
}

void QMailSearchActionPrivate::cancelOperation()
{
    Q_ASSERT(_isValid && _action != 0);
    if (_isValid)
        _server->cancelSearch(_action);
}

void QMailSearchActionPrivate::init()
{
    QMailServiceActionPrivate::init();

    _matchingIds.clear();
    _remainingMessagesCount = 0;
    _messagesCount = 0;
}

void QMailSearchActionPrivate::matchingMessageIds(quint64 action, const QMailMessageIdList &ids)
{
    if (validAction(action)) {
        _matchingIds += ids;

        emit messageIdsMatched(ids);
    }
}

void QMailSearchActionPrivate::remainingMessagesCount(quint64 action, uint count)
{
    if (validAction(action)) {
        _remainingMessagesCount = count;

        emit remainingMessagesCount(count);
    }
}

void QMailSearchActionPrivate::messagesCount(quint64 action, uint count)
{
    if (validAction(action)) {
        _messagesCount = count;

        emit messagesCount(count);
    }
}

void QMailSearchActionPrivate::searchCompleted(quint64 action)
{
    if (validAction(action)) {
        setActivity(QMailServiceAction::Successful);
        emitChanges();
    }
}

void QMailSearchActionPrivate::finaliseSearch()
{
    emit messageIdsMatched(_matchingIds);

    setActivity(QMailServiceAction::Successful);
    emitChanges();
}


/*!
    \class QMailSearchAction

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailSearchAction class provides the interface for identifying messages that match a set of search criteria.

    QMailSearchAction provides the mechanism for messaging clients to request that the message
    server perform a search for messages that match the supplied search criteria.  For criteria
    pertaining to message meta data, the message server will search within the locally held
    meta data.  For a textual content criterion, the message server will search locally where
    the message content is held locally, and on the external server for messages whose content
    has not been retrieved (provided the external service provides a search facility).

    A QMailSearchAction instance supports only a single request at any time. Attempting 
    to initiate an operation on a QMailSearchAction instace while another operation is already 
    in progress for that action is not supported.
    
    An application may however use multiple QMailSearchAction instances to send independent 
    requests concurrently. Each QMailServiceAction instance will report only the changes 
    pertaining to the request that instance delivered.  Whether or not concurrent requests are 
    concurrently serviced by the messageserver depends on whether those requests must be 
    serviced by the same QMailMessageService instance.
*/

/*!
    \enum QMailSearchAction::SearchSpecification

    This enum type is specify the form of search that the message server should perform.

    \value Local    Only the message data stored on the local device should be searched.
    \value Remote   The search should be extended to message data stored on external servers.
*/

/*!
    \typedef QMailSearchAction::ImplementationType
    \internal
*/

/*!
    Constructs a new search action object with the supplied \a parent.
*/
QMailSearchAction::QMailSearchAction(QObject *parent)
    : QMailServiceAction(new QMailSearchActionPrivate(this), parent)
{
    connect(impl(this), SIGNAL(messageIdsMatched(QMailMessageIdList)), this, SIGNAL(messageIdsMatched(QMailMessageIdList)));
    connect(impl(this), SIGNAL(remainingMessagesCount(uint)), this, SIGNAL(remainingMessagesCount(uint)));
    connect(impl(this), SIGNAL(messagesCount(uint)), this, SIGNAL(messagesCount(uint)));
}

/*! \internal */
QMailSearchAction::~QMailSearchAction()
{
}

/*!
    Requests that the message server identify all messages that match the criteria
    specified by \a filter.  If \a bodyText is non-empty then messages that
    contain the supplied text in their content will also be identified.  

    If \a spec is \l{QMailSearchAction::Remote}{Remote}, then the device must be,
    online and an external service will be requested to perform the search for 
    messages not stored locally. 

    If \a sort is not empty, the external service will return matching messages in 
    the ordering indicated by the sort criterion if possible.
*/
void QMailSearchAction::searchMessages(const QMailMessageKey &filter, const QString &bodyText, SearchSpecification spec, const QMailMessageSortKey &sort)
{
    impl(this)->searchMessages(filter, bodyText, spec, sort);
}

/*!
    Requests that the message server identify all messages that match the criteria
    specified by \a filter.  If \a bodyText is non-empty then messages that
    contain the supplied text in their content will also be identified.  

    If \a spec is \l{QMailSearchAction::Remote}{Remote}, then the device must be,
    online and an external service will be requested to perform the search for 
    messages not stored locally. 

    A maximum of \a limit messages will be retrieved from the remote server. 

    If \a sort is not empty, the external service will return matching messages in 
    the ordering indicated by the sort criterion if possible.
*/
void QMailSearchAction::searchMessages(const QMailMessageKey &filter, const QString &bodyText, SearchSpecification spec, quint64 limit, const QMailMessageSortKey &sort)
{
    impl(this)->searchMessages(filter, bodyText, spec, limit, sort);
}

/*!
    Requests that the message server count all messages that match the criteria
    specified by \a filter, both local and on remote servers.  If \a bodyText 
    is non-empty then messages that contain the supplied text in their content 
    will also be matched and counted.
    
    This function requires the device to be online, it may initiate communication 
    with external servers.
*/
void QMailSearchAction::countMessages(const QMailMessageKey &filter, const QString &bodyText)
{
    impl(this)->countMessages(filter, bodyText);
}

/*!
    Attempts to cancel the last requested search operation.
*/
void QMailSearchAction::cancelOperation()
{
    impl(this)->cancelOperation();
}

/*!
    Returns the list of message identifiers found to match the search criteria.
    Unless activity() returns \l Successful, an empty list is returned.
*/
QMailMessageIdList QMailSearchAction::matchingMessageIds() const
{
    return impl(this)->_matchingIds;
}

/*!
    Returns the count of messages remaining on the remote server; that
    is the count of matching messages that will not be retrieved
    to the device.
*/
uint QMailSearchAction::remainingMessagesCount() const
{
    return impl(this)->_remainingMessagesCount;
}

/*!
    Returns the count of matching messages on the remote server.
*/
uint QMailSearchAction::messagesCount() const
{
    return impl(this)->_messagesCount;
}

/*!
    Returns a key matching messages that are temporary messages existing only as 
    the result of a search action.
*/
QMailMessageKey QMailSearchAction::temporaryKey()
{
    return QMailMessageKey::status(QMailMessage::Temporary);
}

/*!
    \fn QMailSearchAction::messageIdsMatched(const QMailMessageIdList &ids)

    This signal is emitted when the messages in \a ids are discovered to match
    the criteria of the search in progress.

    \sa matchingMessageIds()
*/

/*!
    \fn QMailSearchAction::remainingMessagesCount(uint count)

    This signal emits the \a count of messages remaining on the remote server; that
    is the count of matching messages that will not be retrieved to the device.

    Only emitted for remote searches.

    \sa remainingMessagesCount()
*/

/*!
    \fn QMailSearchAction::messagesCount(uint count)

    This signal emits the \a count of matching messages on the remote server.

    Only emitted for remote searches.

    \sa messagesCount()
*/

QMailActionInfoPrivate::QMailActionInfoPrivate(const QMailActionData &data, QMailActionInfo *i)
    : QMailServiceActionPrivate(this, i),
      _requestType(data.requestType()),
      _actionCompleted(false)
{
    // Service handler really should be sending the activity,
    // rather than us faking it..
    connect(_server, SIGNAL(retrievalCompleted(quint64)),
            this, SLOT(activityCompleted(quint64)));
    connect(_server, SIGNAL(storageActionCompleted(quint64)),
            this, SLOT(activityCompleted(quint64)));
    connect(_server, SIGNAL(searchCompleted(quint64)),
            this, SLOT(activityCompleted(quint64)));
    connect(_server, SIGNAL(transmissionCompleted(quint64)),
            this, SLOT(activityCompleted(quint64)));

    init();
    _progress = data.progressCurrent();
    _total = data.progressTotal();
    _status = QMailServiceAction::Status(QMailServiceAction::Status::ErrorCode(data.errorCode()), 
                                         data.text(), data.accountId(), data.folderId(), data.messageId());
    setAction(data.id());
}

void QMailActionInfoPrivate::activityCompleted(quint64 action)
{
    if (validAction(action)) {
        setActivity(QMailServiceAction::Successful);
        emitChanges();
    }
}

float QMailActionInfoPrivate::totalProgress() const
{
    if (_total == 0)
        return 0.0;
    else
        return static_cast<float>(_progress) / _total;
}

void QMailActionInfoPrivate::theProgressChanged(uint, uint)
{
    emit totalProgressChanged(totalProgress());
}

void QMailActionInfoPrivate::theStatusChanged(const QMailServiceAction::Status &newStatus)
{
    if (_lastStatus.accountId != newStatus.accountId)
        emit statusAccountIdChanged(newStatus.accountId);
    if (_lastStatus.errorCode != newStatus.errorCode)
        emit statusErrorCodeChanged(newStatus.errorCode);
    if (_lastStatus.folderId != newStatus.folderId)
        emit statusFolderIdChanged(newStatus.folderId);
    if (_lastStatus.messageId != newStatus.messageId)
        emit statusMessageIdChanged(newStatus.messageId);

    _lastStatus = newStatus;
}

quint64 QMailActionInfoPrivate::actionId() const
{
    return _action;
}

QMailServerRequestType QMailActionInfoPrivate::requestType() const
{
    return _requestType;
}

QMailActionInfo::Status::ErrorCode QMailActionInfoPrivate::statusErrorCode() const
{
    return _status.errorCode;
}

QString QMailActionInfoPrivate::statusText() const
{
    return _status.text;
}

QMailAccountId QMailActionInfoPrivate::statusAccountId() const
{
    return _status.accountId;
}
QMailFolderId QMailActionInfoPrivate::statusFolderId() const
{
    return _status.folderId;
}

QMailMessageId QMailActionInfoPrivate::statusMessageId() const
{
    return _status.messageId;
}

/*!
    \class QMailActionInfo

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailActionInfo class provides the interface for tracking individual actions

    QMailActionInfo provides the mechanism for messaging clients to track an action that is
    or has run on the server. QMailActionInfo objects can only be created by QMailActionObserver
    and derive most of their functionality and signals from \l QMailServiceAction.

    QMailActionInfo presents \l requestType() and \l id() to the the client. For future QML
    interoperability properties and notification signals have been added.
*/

/*!
    \typedef QMailActionInfo::ImplementationType
    \internal
*/

/*!
    \typedef QMailActionInfo::StatusErrorCode

    A typedef for QMailServiceAction::Status::ErrorCode so moc can parse it
*/

/*!
  \fn QMailActionInfo::statusErrorCodeChanged(QMailActionInfo::StatusErrorCode error)

  Signal emitted when \l statusErrorCode() has changed to a new value of \a error
*/

/*!
  \fn QMailActionInfo::statusTextChanged(const QString &text)

  Signal emittted when \l statusText() has changed to a new \a text.
*/

/*!
  \fn QMailActionInfo::statusAccountIdChanged(const QMailAccountId &accountId)

  Signal emitted when \l statusAccountId() has changed to a new \a accountId.
*/

/*!
  \fn QMailActionInfo::statusFolderIdChanged(const QMailFolderId &folderId)

  Signal emitted when \l statusFolderId() has changed to a new \a folderId.
*/

/*!
  \fn QMailActionInfo::statusMessageIdChanged(const QMailMessageId &messageId)

  Signal emitted when \l statusMessageId() has changed to a new \a messageId.
*/

/*!
    \fn QMailActionInfo::totalProgressChanged(float progress)

    Signal emitted when \l totalProgress() has changed to a new \a progress.
*/

/*! \internal */
QMailActionInfo::QMailActionInfo(const QMailActionData &data)
    : QMailServiceAction(new QMailActionInfoPrivate(data, this), 0) // NB: No qobject parent!
{
    connect(impl(this), SIGNAL(statusAccountIdChanged(QMailAccountId)),
            this, SIGNAL(statusAccountIdChanged(QMailAccountId)));
    connect(impl(this), SIGNAL(statusErrorCodeChanged(QMailActionInfo::StatusErrorCode)),
             this, SIGNAL(statusErrorCodeChanged(QMailActionInfo::StatusErrorCode)));
    connect(impl(this), SIGNAL(statusTextChanged(QString)),
            this, SIGNAL(statusTextChanged(QString)));
    connect(impl(this), SIGNAL(statusFolderIdChanged(QMailFolderId)),
            this, SIGNAL(statusFolderIdChanged(QMailFolderId)));
    connect(impl(this), SIGNAL(statusMessageIdChanged(QMailMessageId)),
            this, SIGNAL(statusMessageIdChanged(QMailMessageId)));

    // Hack to get around _interface not being "ready" in the private class
    connect(this, SIGNAL(progressChanged(uint,uint)), impl(this), SLOT(theProgressChanged(uint,uint)));
    connect(this, SIGNAL(statusChanged(QMailServiceAction::Status)), impl(this), SLOT(theStatusChanged(QMailServiceAction::Status)));
}

/*!
    \fn QMailActionInfo::requestType() const
    Returns the \c QMailServiceRequestType of action is it tracking
*/
QMailServerRequestType QMailActionInfo::requestType() const
{
    return impl(this)->requestType();
}

/*!
    Returns the unique action id that was assigned to the action QMailActionInfo is tracking
*/
quint64 QMailActionInfo::id() const
{
    return impl(this)->actionId();
}

/*!
    \fn QMailActionInfo::totalProgress() const
    Returns a floating point between (0..1) indicating the actions progress. This is a convience
    method to \l QMailServiceAction::progress()
*/
float QMailActionInfo::totalProgress() const
{
    return impl(this)->totalProgress();
}

/*!
  \fn QMailActionInfo::statusErrorCode() const
  This is a convience accessor to \l QMailServiceAction::Status::errorCode
*/
QMailActionInfo::StatusErrorCode QMailActionInfo::statusErrorCode() const
{
    return impl(this)->statusErrorCode();
}

/*!
  \fn QMailActionInfo::statusText() const
  This is a convience accessor to \l QMailServiceAction::Status::text
*/
QString QMailActionInfo::statusText() const
{
    return impl(this)->statusText();
}

/*!
  \fn QMailActionInfo::statusAccountId() const
  This is a convience accessor to \l QMailServiceAction::Status::accountId
*/
QMailAccountId QMailActionInfo::statusAccountId() const
{
    return impl(this)->statusAccountId();
}

/*!
  \fn QMailActionInfo::statusFolderId() const
  This is a convience accessor to \l QMailServiceAction::Status::errorCode
*/
QMailFolderId QMailActionInfo::statusFolderId() const
{
    return impl(this)->statusFolderId();
}

/*!
  \fn QMailActionInfo::statusMessageId() const
  This is a convience accessor for \l QMailServiceAction::Status::messageId
*/

QMailMessageId QMailActionInfo::statusMessageId() const
{
    return impl(this)->statusMessageId();
}

QMailActionObserverPrivate::QMailActionObserverPrivate(QMailActionObserver *i)
    : QMailServiceActionPrivate(this, i),
      _isReady(false)
{
    connect(_server, SIGNAL(actionStarted(QMailActionData)),
            this, SLOT(actionStarted(QMailActionData)));
    connect(_server, SIGNAL(actionsListed(QMailActionDataList)),
            this, SLOT(actionsListed(QMailActionDataList)));

    _server->listActions();

    init();
}

QList< QSharedPointer<QMailActionInfo> > QMailActionObserverPrivate::runningActions() const
{
    return _runningActions.values();
}

void QMailActionObserverPrivate::listActionsRequest()
{
    _server->listActions();
}

void QMailActionObserverPrivate::actionsListed(const QMailActionDataList &actions)
{
    if (_isReady)
        _runningActions.clear();
    else
        _isReady = true;

    Q_ASSERT(_runningActions.isEmpty());

    foreach (QMailActionData const& action, actions) {
       addAction(action);
    }

    _isReady = true;
    emit actionsChanged(runningActions());
}

void QMailActionObserverPrivate::removeOldActions()
{
    Q_ASSERT(_isReady);
    bool changed(false);
    while(!_delayRemoveList.isEmpty()) {
        _runningActions.remove(_delayRemoveList.takeFirst());
        changed = true;
    }

    if (changed)
        emit actionsChanged(runningActions());
}

void QMailActionObserverPrivate::actionStarted(const QMailActionData &action)
{
    if (_isReady) {
        addAction(action);
        emit actionsChanged(runningActions());
    }
}

QSharedPointer<QMailActionInfo> QMailActionObserverPrivate::addAction(const QMailActionData &action)
{
    QSharedPointer<QMailActionInfo> actionInfo(new QMailActionInfo(action));
    connect(actionInfo.data(), SIGNAL(activityChanged(QMailServiceAction::Activity)),
            this, SLOT(anActionActivityChanged(QMailServiceAction::Activity)));
    _runningActions.insert(action.id(), actionInfo);

    return actionInfo;
}

void QMailActionObserverPrivate::anActionActivityChanged(QMailServiceAction::Activity activity)
{
    const QMailActionInfo *theAction(qobject_cast<QMailActionInfo *>(sender()));
    if (theAction) {
        if (activity == QMailServiceAction::Successful || activity == QMailServiceAction::Failed) {
            // Avoid possibly deleting from within its own signal
            _delayRemoveList.append(theAction->id());
            QTimer::singleShot(0, this, SLOT(removeOldActions()));
        }
    } else {
        qWarning() << "Unable to determine who sent signal";
    }
}

/*! \class QMailActionObserver

   \preliminary
   \ingroup messaginglibrary

   \brief The QMailActionObserver class provides an interface for monitoring currently running actions

    QMailActionObserver provides a mechanism for messaging clients to observe what actions
    the messageserver is currently running. A list of currently running actions can be retrieved
    with \l actions(). When actions are started or finished \l actionsChanged() is emitted with the
    new list of actions.

    QMailActionObserver is initialised asynchronously, thus reading \l actions() immediately will
    return an empty list.
*/

/*!
    \typedef QMailActionObserver::ImplementationType
    \internal
*/

/*!
    \fn QMailActionObserver::actionsChanged(const QList< QSharedPointer<QMailActionInfo> > &newActions)

    This signal is emitted whenever the list of actions we are observing changes. This can be
    for three reasons: actions have started, actions have finished or the action list has just been
    initialized.

    New list given by \a newActions

    \sa actions()

*/

/*! Constructs a new QMailActionObserver with a supplied \a parent  */
QMailActionObserver::QMailActionObserver(QObject *parent)
    : QMailServiceAction(new QMailActionObserverPrivate(this), parent)
{
    connect(impl(this), SIGNAL(actionsChanged(QList<QSharedPointer<QMailActionInfo> >)), this,
            SIGNAL(actionsChanged(QList<QSharedPointer<QMailActionInfo> >)));
}

/*! Destructs QMailActionObserver */
QMailActionObserver::~QMailActionObserver()
{
}

/*!
    \fn QMailActionObserver::actions() const
    Returns a list of the currently running actions on the message server.

    \sa QMailActionInfo
*/
QList< QSharedPointer<QMailActionInfo> > QMailActionObserver::actions() const
{
    return impl(this)->runningActions();
}

/*!
    \fn QMailActionObserver::listActionsRequest()
    Makes request of running actions. Result will be returned with actionsChanged() signal.
*/
void QMailActionObserver::listActionsRequest()
{
    impl(this)->listActionsRequest();
}

QMailProtocolActionPrivate::QMailProtocolActionPrivate(QMailProtocolAction *i)
    : QMailServiceActionPrivate(this, i)
{
    connect(_server, SIGNAL(protocolResponse(quint64, QString, QVariant)),
            this, SLOT(protocolResponse(quint64, QString, QVariant)));
    connect(_server, SIGNAL(protocolRequestCompleted(quint64)),
            this, SLOT(protocolRequestCompleted(quint64)));

    init();
}

void QMailProtocolActionPrivate::protocolRequest(const QMailAccountId &accountId, const QString &request, const QVariant &data)
{
    _server->protocolRequest(newAction(), accountId, request, data);
}

void QMailProtocolActionPrivate::protocolResponse(quint64 action, const QString &response, const QVariant &data)
{
    if (validAction(action)) {
        emit protocolResponse(response, data);
    }
}

void QMailProtocolActionPrivate::protocolRequestCompleted(quint64 action)
{
    if (validAction(action)) {
        setActivity(QMailServiceAction::Successful);
        emitChanges();
    }
}


/*!
    \class QMailProtocolAction

    \preliminary
    \ingroup messaginglibrary

    \brief The QMailProtocolAction class provides a mechanism for messageserver clients
    and services to collaborate without messageserver involvement.

    QMailProtocolAction provides a mechanism for messaging clients to request actions from
    message services that are not implemented by the messageserver.  If a client can
    determine that the service implementing a specific account supports a particular
    operation (by inspecting the output of QMailAccount::messageSources()), it may
    invoke that operation by passing appropriately formatted data to the service via the
    protocolRequest() slot.

    If the service is able to provide the requested service, and extract the necessary 
    data from the received input, it should perform the requested operation.  If any
    output is produced, it may be passed back to the client application via the 
    protocolResponse() signal.
*/

/*!
    \typedef QMailProtocolAction::ImplementationType
    \internal
*/

/*!
    Constructs a new protocol action object with the supplied \a parent.
*/
QMailProtocolAction::QMailProtocolAction(QObject *parent)
    : QMailServiceAction(new QMailProtocolActionPrivate(this), parent)
{
    connect(impl(this), SIGNAL(protocolResponse(QString, QVariant)), this, SIGNAL(protocolResponse(QString, QVariant)));
}

/*! \internal */
QMailProtocolAction::~QMailProtocolAction()
{
}

/*!
    Requests that the message server forward the protocol-specific request \a request
    to the QMailMessageSource configured for the account identified by \a accountId.
    The request may have associated \a data, in a protocol-specific form.
*/
void QMailProtocolAction::protocolRequest(const QMailAccountId &accountId, const QString &request, const QVariant &data)
{
    impl(this)->protocolRequest(accountId, request, data);
}

/*!
    \fn QMailProtocolAction::protocolResponse(const QString &response, const QVariant &data)

    This signal is emitted when the response \a response is emitted by the messageserver,
    with the associated \a data.
*/
