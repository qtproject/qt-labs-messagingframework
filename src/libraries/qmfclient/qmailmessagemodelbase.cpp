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

#include "qmailmessagemodelbase.h"
#include "qmailmessage.h"
#include "qmailstore.h"
#include <QCoreApplication>

namespace {

QString messageAddressText(const QMailMessageMetaData& m, bool incoming) 
{
    //message sender or recipients
    if ( incoming ) {
        QMailAddress fromAddress(m.from());
        return fromAddress.toString();
    } else {
        QMailAddressList toAddressList(m.recipients());
        if (!toAddressList.isEmpty()) {
            QMailAddress firstRecipient(toAddressList.first());
            QString text = firstRecipient.toString();

            bool multipleRecipients(toAddressList.count() > 1);
            if (multipleRecipients)
                text += QLatin1String(", ...");

            return text;
        } else  {
            return qApp->translate("QMailMessageModelBase", "Draft message");
        }
    }
}

QString messageSizeText(const QMailMessageMetaData& m)
{
    const uint size(m.size());

    if (size < 1024)
        return qApp->translate("QMailMessageModelBase", "%n byte(s)", "", size);
    else if (size < (1024 * 1024))
        return qApp->translate("QMailMessageModelBase", "%1 KB").arg(((float)size)/1024.0, 0, 'f', 1);
    else if (size < (1024 * 1024 * 1024))
        return qApp->translate("QMailMessageModelBase", "%1 MB").arg(((float)size)/(1024.0 * 1024.0), 0, 'f', 1);
    else
        return qApp->translate("QMailMessageModelBase", "%1 GB").arg(((float)size)/(1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

}


/*! \internal */
QMailMessageModelImplementation::~QMailMessageModelImplementation()
{
}


/*!
    \class QMailMessageModelBase

    \preliminary
    \ingroup messaginglibrary 
    \brief The QMailMessageModelBase class provides an interface to a model containing messages.

    The QMailMessageModelBase presents a model of all the messages currently stored in
    the message store. By using the setKey() and sortKey() functions it is possible to have the model
    represent specific user-filtered subsets of messages, sorted in a particular order.

    The QMailMessageModelBase is a descendant of QAbstractListModel, so it is suitable for use with
    the Qt View classes such as QListView and QTreeView to visually present groups of messages. 

    The model listens for changes reported by the QMailStore, and automatically synchronizes
    its content with that of the store.  This behaviour can be optionally or temporarily disabled 
    by calling the setIgnoreMailStoreUpdates() function.

    Messages can be extracted from the view with the idFromIndex() function and the resultant id can be 
    used to load a message from the store. 

    For filters or sorting not provided by the QMailMessageModelBase it is recommended that
    QSortFilterProxyModel is used to wrap the model to provide custom sorting and filtering. 

    \sa QMailMessage, QSortFilterProxyModel
*/

/*!
    \enum QMailMessageModelBase::Roles

    Represents common display roles of a message. These roles are used to display common message elements 
    in a view and its attached delegates.

    \value MessageAddressTextRole 
        The address text of a message. This a can represent a name if the address is tied to a contact in the addressbook and 
        represents either the incoming or outgoing address depending on the message direction.
    \value MessageSubjectTextRole  
        The subject of a message. For-non email messages this may represent the body text of a message.
    \value MessageFilterTextRole 
        The MessageAddressTextRole concatenated with the MessageSubjectTextRole. This can be used by filtering classes to filter
        messages based on the text of these commonly displayed roles. 
    \value MessageTimeStampTextRole
        The timestamp of a message. "Received" or "Sent" is prepended to the timestamp string depending on the message direction.
    \value MessageSizeTextRole
        The size of a message, formatted as text.
    \value MessageTypeIconRole
        A string that can be passed to QIcon representing the type of the message.
    \value MessageStatusIconRole
        A string that can be passed to QIcon representing the status of the message. e.g Read, Unread, Downloaded
    \value MessageDirectionIconRole
        A string that can be passed to QIcon representing the incoming or outgoing direction of a message.
    \value MessagePresenceIconRole
        A string that can be passed to QIcon representing the presence status of the contact associated with the MessageAddressTextRole.
    \value MessageBodyTextRole  
        The body of a message represented as text.
    \value MessageIdRole
        The QMailMessageId value identifying the message.
*/

/*!
    Constructs a QMailMessageModelBase with the parent \a parent.
*/
QMailMessageModelBase::QMailMessageModelBase(QObject* parent)
    : QAbstractItemModel(parent)
{
    connect(QMailStore::instance(), SIGNAL(messagesAdded(QMailMessageIdList)), this, SLOT(messagesAdded(QMailMessageIdList)));
    connect(QMailStore::instance(), SIGNAL(messagesRemoved(QMailMessageIdList)), this, SLOT(messagesRemoved(QMailMessageIdList)));
    connect(QMailStore::instance(), SIGNAL(messagesUpdated(QMailMessageIdList)), this, SLOT(messagesUpdated(QMailMessageIdList)));
}

/*! \internal */
QMailMessageModelBase::~QMailMessageModelBase()
{
}

/*! \reimp */
int QMailMessageModelBase::rowCount(const QModelIndex& index) const
{
    return impl()->rowCount(index);
}

/*! \reimp */
int QMailMessageModelBase::columnCount(const QModelIndex& index) const
{
    return impl()->columnCount(index);
}

/*!
    Returns true if the model contains no messages.
*/
bool QMailMessageModelBase::isEmpty() const
{
    return impl()->isEmpty();
}

/*! \reimp */
QVariant QMailMessageModelBase::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    QMailMessageId id = idFromIndex(index);
    if (!id.isValid())
        return QVariant();

    // Some items can be processed without loading the message data
    switch(role)
    {
    case MessageIdRole:
        return id;

    case Qt::CheckStateRole:
        return impl()->checkState(index);

    default:
        break;
    }

    // Otherwise, load the message data
    return data(QMailMessageMetaData(id), role);
}

/*! \internal */
QVariant QMailMessageModelBase::data(const QMailMessageMetaData &message, int role) const
{
    static QString outgoingIcon(QLatin1String(":icon/sendmail"));
    static QString incomingIcon(QLatin1String(":icon/getmail"));

    static QString readIcon(QLatin1String(":icon/flag_normal"));
    static QString unreadIcon(QLatin1String(":icon/flag_unread"));
    static QString toGetIcon(QLatin1String(":icon/flag_toget"));
    static QString removedIcon(QLatin1String(":icon/flag_removed"));

    /* No longer used...
    static QString toSendIcon(":icon/flag_tosend");
    static QString unfinishedIcon(":icon/flag_unfinished");
    static QString noPresenceIcon(":icon/presence-none");
    static QString offlineIcon(":icon/presence-offline");
    static QString awayIcon(":icon/presence-away");
    static QString busyIcon(":icon/presence-busy");
    static QString onlineIcon(":icon/presence-online");

    static QString messageIcon(":icon/txt");
    static QString mmsIcon(":icon/multimedia");
    static QString emailIcon(":icon/email");
    static QString instantMessageIcon(":icon/im");
    */

    bool sent(message.status() & QMailMessage::Sent);
    //bool incoming(message.status() & QMailMessage::Incoming);
    bool incoming = !sent;

    switch(role)
    {
        case Qt::DisplayRole:
        case MessageAddressTextRole:
            return messageAddressText(message,incoming);

        case MessageSizeTextRole:
            return messageSizeText(message);

        case MessageTimeStampTextRole:
        {
            QString action;
            if (incoming) {
                action = qApp->translate("QMailMessageModelBase", "Received");
            } else {
                if (sent) {
                    action = qApp->translate("QMailMessageModelBase", "Sent");
                } else {
                    action = qApp->translate("QMailMessageModelBase", "Last edited");
                }
            }

            QDateTime messageTime(message.date().toLocalTime());
            QString date(messageTime.date().toString(QLatin1String("dd MMM")));
            QString time(messageTime.time().toString(QLatin1String("h:mm")));
            QString sublabel(QString::fromLatin1("%1 %2 %3").arg(action).arg(date).arg(time));
            return sublabel;
        }

        case MessageSubjectTextRole:
            return message.subject();

        case MessageFilterTextRole:
            return QString(messageAddressText(message,incoming) % QStringLiteral(" ") % message.subject());

        case Qt::DecorationRole:
        case MessageTypeIconRole:
        {
            // Not currently implemented...
            return QString();
        }

        case MessageDirectionIconRole:
        {
            QString mainIcon = incoming ? incomingIcon : outgoingIcon;
            return mainIcon;
        }

        case MessageStatusIconRole:
        if (incoming) {
            quint64 status = message.status();
            if ( status & QMailMessage::Removed ) {
                return removedIcon;
            } else if ( status & QMailMessage::PartialContentAvailable ) {
                if ( status & QMailMessage::Read ) {
                    return readIcon;
                } else {
                    return unreadIcon;
                }
            } else {
                return toGetIcon;
            }
        } else {
            return readIcon;
            // TODO: use unfinishedIcon or toSendIcon depending on the message state
        }

        case MessagePresenceIconRole:
        {
            // Not currently implemented...
            return QString();
        }

        case MessageBodyTextRole:
        {
            if ((message.messageType() == QMailMessage::Instant) && !message.subject().isEmpty()) {
                // For IMs that contain only text, the body is replicated in the subject
                return message.subject();
            } else {
                QMailMessage fullMessage(message.id());

                // Some items require the entire message data
                if (fullMessage.hasBody())
                    return fullMessage.body().data();

                return QString();
            }
        }
    }

    return QVariant();
}

/*! \reimp */
bool QMailMessageModelBase::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (index.isValid()) {
        // The only role we allow to be changed is the check state
        if (role == Qt::CheckStateRole || role == Qt::EditRole) {
            impl()->setCheckState(index, static_cast<Qt::CheckState>(value.toInt()));
            emit dataChanged(index, index);
            return true;
        }
    }

    return false;
}

/*!
    Returns the QMailMessageKey used to populate the contents of this model.
*/
QMailMessageKey QMailMessageModelBase::key() const
{
    return impl()->key(); 
}

/*!
    Sets the QMailMessageKey used to populate the contents of the model to \a key.
    If the key is empty, the model is populated with all the messages from the 
    database.
    
    \sa modelChanged()
*/
void QMailMessageModelBase::setKey(const QMailMessageKey& key) 
{
    impl()->setKey(key);
    fullRefresh(true);
}

/*!
    Returns the QMailMessageSortKey used to sort the contents of the model.
*/
QMailMessageSortKey QMailMessageModelBase::sortKey() const
{
   return impl()->sortKey();
}

/*!
    Sets the QMailMessageSortKey used to sort the contents of the model to \a sortKey.
    If the sort key is invalid, no sorting is applied to the model contents and messages
    are displayed in the order in which they were added into the database.
    
    \sa modelChanged()
*/
void QMailMessageModelBase::setSortKey(const QMailMessageSortKey& sortKey) 
{
    // We need a sort key defined, to preserve the ordering in DB records for addition/removal events
    impl()->setSortKey(sortKey.isEmpty() ? QMailMessageSortKey::id() : sortKey);
    fullRefresh(true);
}

/*!
    Returns the the limit set on the model, i.e the max number of messages that the model can hold.
*/
uint QMailMessageModelBase::limit() const
{
   return impl()->limit();
}

/*!
    Sets the max number of messages that model can hold, by default this value is zero, that means,
    all messages matching the model sort key.
*/
void QMailMessageModelBase::setLimit(uint limit)
{
    impl()->setLimit(limit);
}

/*!
    Returns the total count of messages available in the database for the QMailMessageKey
    used to populate the contents of this model. If no limit is set the return value is the same
    as rowCount(), i.e the current number of messages in the model.

    \sa rowCount()
 */
int QMailMessageModelBase::totalCount() const
{
    return impl()->totalCount();
}

/*! 
    \fn QModelIndex QMailMessageModelBase::generateIndex(int row, int column, void *ptr)
    \internal 
*/

/*! \internal */
void QMailMessageModelBase::emitDataChanged(const QModelIndex& idx, const QModelIndex& jdx)
{
    emit dataChanged(idx, jdx);
}

/*! \internal */
void QMailMessageModelBase::emitBeginInsertRows(const QModelIndex& idx, int start, int end)
{
    beginInsertRows(idx, start, end);
}

/*! \internal */
void QMailMessageModelBase::emitEndInsertRows()
{
    endInsertRows();
}

/*! \internal */
void QMailMessageModelBase::emitBeginRemoveRows(const QModelIndex& idx, int start, int end)
{
    beginRemoveRows(idx, start, end);
}

/*! \internal */
void QMailMessageModelBase::emitEndRemoveRows()
{
    endRemoveRows();
}

/*! \internal */
void QMailMessageModelBase::messagesAdded(const QMailMessageIdList& ids)
{
    if (!impl()->processMessagesAdded(ids)) {
        fullRefresh(false);
    }
}

/*! \internal */
void QMailMessageModelBase::messagesUpdated(const QMailMessageIdList& ids)
{
    if (!impl()->processMessagesUpdated(ids)) {
        fullRefresh(false);
    }
}

/*! \internal */
void QMailMessageModelBase::messagesRemoved(const QMailMessageIdList& ids)
{
    if (!impl()->processMessagesRemoved(ids)) {
        fullRefresh(false);
    }
}

/*!
    Returns the QMailMessageId of the message represented by the QModelIndex \a index.
    If the index is not valid an invalid QMailMessageId is returned.
*/
QMailMessageId QMailMessageModelBase::idFromIndex(const QModelIndex& index) const
{
    return impl()->idFromIndex(index);
}

/*!
    Returns the QModelIndex that represents the message with QMailMessageId \a id.
    If the id is not contained in this model, an invalid QModelIndex is returned.
*/
QModelIndex QMailMessageModelBase::indexFromId(const QMailMessageId& id) const
{
    return impl()->indexFromId(id);
}

/*!
    Returns true if the model has been set to ignore updates emitted by 
    the mail store; otherwise returns false.
*/
bool QMailMessageModelBase::ignoreMailStoreUpdates() const
{
    return impl()->ignoreMailStoreUpdates();
}

/*!
    Sets whether or not mail store updates are ignored to \a ignore.

    If ignoring updates is set to true, the model will ignore updates reported 
    by the mail store.  If set to false, the model will automatically synchronize 
    its content in reaction to updates reported by the mail store.

    If updates are ignored, signals such as rowInserted and dataChanged will not 
    be emitted; instead, the modelReset signal will be emitted when the model is
    later changed to stop ignoring mail store updates, and detailed change 
    information will not be accessible.
*/
void QMailMessageModelBase::setIgnoreMailStoreUpdates(bool ignore)
{
    if (impl()->setIgnoreMailStoreUpdates(ignore))
        fullRefresh(false);
}

/*! \internal */
void QMailMessageModelBase::fullRefresh(bool changed) 
{
    beginResetModel();
    impl()->reset();
    endResetModel();

    if (changed)
        emit modelChanged();
}

/*!
    \fn void QMailMessageModelBase::modelChanged()

    Signal that is emitted when the content or ordering of the model is reset.
*/

/*!
    \fn QMailMessageModelImplementation *QMailMessageModelBase::impl()
    \internal
*/

/*!
    \fn const QMailMessageModelImplementation *QMailMessageModelBase::impl() const
    \internal
*/
