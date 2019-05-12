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

#include "qmailviewer.h"
#include <QApplication>
#include <QIcon>
#include <QMap>
#include <QUrl>
#include <QWidget>
#include <qmaillog.h>

#include "genericviewer.h"

Q_GLOBAL_STATIC(GenericViewer, genericViewerInstance);

/*!
    \class QMailViewerInterface
    \ingroup qmfutil

    \brief The QMailViewerInterface class defines the interface to objects that can display a mail message.

    Qt Extended uses the QMailViewerInterface interface for displaying mail messages.  A class may implement the
    QMailViewerInterface interface to display a mail message format.

    The message to be displayed is provided to the viewer class using the \l {QMailViewerInterface::setMessage()}
    {setMessage()} function.  If the message refers to external resources, these should be provided using the
    \l {QMailViewerInterface::setResource()}{setResource()} function.  The  \l {QMailViewerInterface::clear()}{clear()}
    function clears any message or resources previously set.

    The viewer object should emit the \l {QMailViewerInterface::anchorClicked()}{anchorClicked()} signal if the user 
    selects a link in the message.  If the message supports a concept of completion, then the 
    \l {QMailViewerInterface::finished()}{finished()} signal should be emitted after the display has been completed.

    Rather than creating objects that implement the QMailViewerInterface directly, clients should create an object
    of an appropriate type by using the QMailViewerFactory class:

    \code
    QString key = QMailViewerFactory::defaultKey( QMailViewerFactory::SmilContent );
    QMailViewerInterface* smilViewer = QMailViewerFactory::create( key, this, "smilViewer" );
    \endcode

    \sa QMailViewerFactory
*/

/*!
    \fn QWidget* QMailViewerInterface::widget() const

    Returns the top-level widget that implements the viewer functionality.
*/

/*!
    \fn bool QMailViewerInterface::setMessage(const QMailMessage& mail)

    Displays the contents of \a mail.  Returns whether the message could be successfully displayed.
*/

/*!
    \fn void QMailViewerInterface::clear()

    Resets the display to have no content, and removes any resource associations.
*/

/*!
    \fn QMailViewerInterface::respondToMessage(QMailMessage::ResponseType type)

    This signal is emitted by the viewer to initiate a response action, of type \a type.
*/

/*!
    \fn QMailViewerInterface::respondToMessagePart(const QMailMessagePart::Location &partLocation, QMailMessage::ResponseType type)

    This signal is emitted by the viewer to initiate a response to the message
    part indicated by \a partLocation, of type \a type.
*/

/*!
    \fn QMailViewerInterface::contactDetails(const QContact &contact)

    This signal is emitted by the viewer to request a display of \a{contact}'s details.
*/

/*!
    \fn QMailViewerInterface::anchorClicked(const QUrl& link)

    This signal is emitted when the user presses the select key while the display has the 
    anchor \a link selected.
*/

/*!
    \fn QMailViewerInterface::messageChanged(const QMailMessageId &id)

    This signal is emitted by the viewer to report that it is now viewing a different message, 
    identified by \a id.
*/

/*!
    \fn QMailViewerInterface::viewMessage(const QMailMessageId &id, QMailViewerFactory::PresentationType type)

    This signal is emitted by the viewer to request a display of the message identified by \a id, 
    using the presentation style \a type.
*/

/*!
    \fn QMailViewerInterface::sendMessage(QMailMessage &message)

    This signal is emitted by the viewer to send a new message, whose contents are held by \a message.
*/

/*!
    \fn QMailViewerInterface::finished()

    This signal is emitted when the display of the current mail message is completed.  This signal 
    is emitted only for message types that define a concept of completion, such as SMIL slideshows.
*/

/*!
    \fn QMailViewerInterface::retrieveMessage()

    This signal is emitted by the viewer to initiate a message completion action.  
    This is only meaningful if the message has not yet been completely retrieved.

    \sa QMailMessage::status(), QMailRetrievalAction::retrieveMessages()
*/

/*!
    \fn QMailViewerInterface::retrieveMessagePortion(uint bytes)

    This signal is emitted by the viewer to retrieve an additional \a bytes from the message.
    This is only meaningful if the message has not yet been completely retrieved.

    \sa QMailMessage::status(), QMailRetrievalAction::retrieveMessages()
*/

/*!
    \fn QMailViewerInterface::retrieveMessagePart(const QMailMessagePart &part)

    This signal is emitted by the viewer to initiate a message part retrieval action for \a part.
*/

/*!
    \fn QMailViewerInterface::retrieveMessagePartPortion(const QMailMessagePart &part, uint bytes)

    This signal is emitted by the viewer to initiate a message part retrieval action for an additional \a bytes of the \a part.
*/

/*!
    Constructs the QMailViewerInterface object with the parent widget \a parent.
*/
QMailViewerInterface::QMailViewerInterface( QWidget *parent )
    : QObject( parent )
{
}

/*! 
    Destructs the QMailViewerInterface object.
*/
QMailViewerInterface::~QMailViewerInterface()
{
}

/*!
    Scrolls the display to position the \a link within the viewable area.
*/
void QMailViewerInterface::scrollToAnchor(const QString& link)
{
    // default implementation does nothing
    Q_UNUSED(link)
}

/*!
    \fn void QMailViewerInterface::addActions(const QList<QAction*>& actions)

    Requests that the viewer add the content of \a actions to the set of available user actions.
*/

/*!
    \fn void QMailViewerInterface::removeAction(QAction* action)

    Requests that the viewer remove \a action from the set of available user actions.
*/

/*!
    Allows the viewer object to handle the notification of the arrival of new messages, 
    identified by \a list.

    Return true to indicate that the event has been handled, or false to let the caller handle
    the new message event.
*/
bool QMailViewerInterface::handleIncomingMessages( const QMailMessageIdList &list ) const
{
    // default implementation does nothing
    Q_UNUSED(list)
    return false;
}

/*!
    Allows the viewer object to handle the notification of the transmission of queued messages, 
    identified by \a list.

    Return true to indicate that the event has been handled, or false to let the caller handle
    the new message event.
*/
bool QMailViewerInterface::handleOutgoingMessages( const QMailMessageIdList &list ) const
{
    // default implementation does nothing
    Q_UNUSED(list)
    return false;
}

/*!
    \fn QString QMailViewerInterface::key() const

    Returns a value that uniquely identifies the viewer component.
*/

/*!
    \fn QMailViewerFactory::PresentationType QMailViewerInterface::presentation() const

    Returns the type of message presentation that this viewer implements.
*/

/*!
    \fn bool QMailViewerInterface::isSupported(QMailMessage::ContentType t, QMailViewerFactory::PresentationType pres) const

    Returns true if the viewer can present a message containing data of content type \a t, using the 
    presentation type \a pres.
*/

/*!
    \fn QList<QMailMessage::ContentType> QMailViewerInterface::types() const

    Returns a list of the content types that can be presented by this viewer component.
*/

/*! 
    Supplies the viewer object with a resource that may be referenced by a mail message.  The resource 
    identified as \a name will be displayed as the object \a value.  
*/
void QMailViewerInterface::setResource(const QUrl& name, QVariant value)
{
    // default implementation does nothing
    Q_UNUSED(name)
    Q_UNUSED(value)
}


/*!
    \class QMailViewerFactory
    \ingroup qmfutil

    \brief The QMailViewerFactory class creates objects implementing the QMailViewerInterface interface.

    The QMailViewerFactory class creates objects that are able to display mail messages, and 
    implement the QMailViewerInterface interface.  The factory chooses an implementation
    based on the type of message to be displayed.

    The QMailViewerInterface class describes the interface supported by classes that can be created
    by the QMailViewerFactory class.  To create a new class that can be created via the QMailViewerFactory,
    implement a plug-in that derives from QMailViewerInterface.

    \sa QMailViewerInterface
*/

/*!
    \enum QMailViewerFactory::PresentationType

    This enum defines the types of presentation that message viewer components may implement.

    \value AnyPresentation Do not specify the type of presentation to be employed.
    \value StandardPresentation Present the message in the standard fashion for the relevant content type.
    \value ConversationPresentation Present the message in the context of a conversation with a contact.
    \value UserPresentation The first value that can be used for application-specific purposes.
*/

/*!
    Returns a list of keys identifying classes that can display a message containing \a type content,
    using the presentation type \a pres.
*/
QStringList QMailViewerFactory::keys(QMailMessage::ContentType type, PresentationType pres)
{
    QStringList in;

    if (genericViewerInstance()->isSupported(type, pres))
        in << genericViewerInstance()->key();

    return in;
}

/*!
    Returns the key identifying the first class found that can display message containing \a type content,
    using the presentation type \a pres.
*/
QString QMailViewerFactory::defaultKey(QMailMessage::ContentType type, PresentationType pres)
{
    QStringList list(QMailViewerFactory::keys(type, pres));
    return (list.isEmpty() ? QString() : list.first());
}

/*!
    Creates a viewer object of the class identified by \a key, setting the returned object to
    have the parent widget \a parent.
*/
QMailViewerInterface *QMailViewerFactory::create(const QString &key, QWidget *parent)
{
    Q_UNUSED(key);
    return new GenericViewer(parent);
}

