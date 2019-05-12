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

#include "qmailid.h"

class QMailIdPrivate
{
public:
    QMailIdPrivate();
    QMailIdPrivate(quint64 value);
    QMailIdPrivate(const QMailIdPrivate& other);

    QMailIdPrivate& operator=(const QMailIdPrivate& other);

    bool isValid() const;
    quint64 toULongLong() const;

    bool operator!=(const QMailIdPrivate& other) const;
    bool operator==(const QMailIdPrivate& other) const;
    bool operator<(const QMailIdPrivate& other) const;

    template <typename Stream> void serialize(Stream &stream) const;
    template <typename Stream> void deserialize(Stream &stream);

private:
    quint64 _value;
};

QMailIdPrivate::QMailIdPrivate()
    : _value(0)

{
}

QMailIdPrivate::QMailIdPrivate(quint64 value)
    : _value(value)
{
}

QMailIdPrivate::QMailIdPrivate(QMailIdPrivate const& other)
    : _value(other._value)
{
}

QMailIdPrivate& QMailIdPrivate::operator=(const QMailIdPrivate& other)
{
    _value = other._value;
    return *this;
}

bool QMailIdPrivate::isValid() const
{
    return _value != 0;
}

quint64 QMailIdPrivate::toULongLong() const
{
    return _value;
}

bool QMailIdPrivate::operator!=(const QMailIdPrivate & other) const
{
    return _value != other._value;
}

bool QMailIdPrivate::operator==(const QMailIdPrivate& other) const
{
    return _value == other._value;
}

bool QMailIdPrivate::operator<(const QMailIdPrivate& other) const
{
    return _value < other._value;
}

template <typename Stream> void QMailIdPrivate::serialize(Stream &stream) const
{
    stream << _value;
}

template <typename Stream> void QMailIdPrivate::deserialize(Stream &stream)
{
    stream >> _value;
}

QDebug& operator<<(QDebug& debug, const QMailIdPrivate &id)
{
    id.serialize(debug);
    return debug;
}

QTextStream& operator<<(QTextStream& s, const QMailIdPrivate &id)
{
    id.serialize(s);
    return s;
}

/*!
    \class QMailAccountId
    \ingroup messaginglibrary

    \preliminary
    \brief The QMailAccountId class is used to identify accounts stored by QMailStore.

    QMailAccountId is a class used to represent accounts stored by the QMailStore, identified
    by their unique numeric internal indentifer.

    A QMailAccountId instance can be tested for validity, and compared to other instances
    for equality.  The numeric value of the identifier is not intrinsically meaningful 
    and cannot be modified.
    
    \sa QMailAccount, QMailStore::account()
*/

/*!
    \typedef QMailAccountIdList
    \relates QMailAccountId
*/

Q_IMPLEMENT_USER_METATYPE(QMailAccountId)

/*! 
    Construct an uninitialized QMailAccountId, for which isValid() returns false.
*/
QMailAccountId::QMailAccountId()
    : d(new QMailIdPrivate)
{
}

/*! 
    Construct a QMailAccountId with the supplied numeric identifier \a value.
*/
QMailAccountId::QMailAccountId(quint64 value)
    : d(new QMailIdPrivate(value))
{
}

/*! \internal */
QMailAccountId::QMailAccountId(const QMailAccountId& other)
    : d(new QMailIdPrivate(*other.d))
{
}

/*! \internal */
QMailAccountId::~QMailAccountId()
{
}

/*! \internal */
QMailAccountId& QMailAccountId::operator=(const QMailAccountId& other) 
{
    *d = *other.d;
    return *this;
}

/*!
    Returns true if this object has been initialized with an identifier.
*/
bool QMailAccountId::isValid() const
{
    return d->isValid();
}

/*! \internal */
quint64 QMailAccountId::toULongLong() const
{
    return d->toULongLong();
}

/*!
    Returns a QVariant containing the value of this account identfier.
*/
QMailAccountId::operator QVariant() const
{
    return QVariant::fromValue(*this);
}

/*!
    Returns true if this object's identifier value differs from that of \a other.
*/
bool QMailAccountId::operator!=(const QMailAccountId& other) const
{
    return *d != *other.d;
}

/*!
    Returns true if this object's identifier value is equal to that of \a other.
*/
bool QMailAccountId::operator==(const QMailAccountId& other) const
{
    return *d == *other.d;
}

/*!
    Returns true if this object's identifier value is less than that of \a other.
*/
bool QMailAccountId::operator<(const QMailAccountId& other) const
{
    return *d < *other.d;
}

/*! 
    \fn QMailAccountId::serialize(Stream&) const
    \internal 
*/
template <typename Stream> void QMailAccountId::serialize(Stream &stream) const
{
    d->serialize(stream);
}

/*! 
    \fn QMailAccountId::deserialize(Stream&)
    \internal 
*/
template <typename Stream> void QMailAccountId::deserialize(Stream &stream)
{
    d->deserialize(stream);
}

/*! \internal */
QDebug& operator<<(QDebug& debug, const QMailAccountId &id)
{
    id.serialize(debug);
    return debug;
}

/*! \internal */
QTextStream& operator<< (QTextStream& s, const QMailAccountId &id)
{
    id.serialize(s);
    return s;
}

Q_IMPLEMENT_USER_METATYPE_TYPEDEF(QMailAccountIdList, QMailAccountIdList)


/*!
    \class QMailThreadId
    \ingroup messaginglibrary

    \preliminary
    \brief The QMailThreadId class is used to identify threads stored by QMailStore.

    QMailThreadId is a class used to represent threads stored by the QMailStore, identified
    by their unique numeric internal indentifer.

    A QMailThreadId instance can be tested for validity, and compared to other instances
    for equality.  The numeric value of the identifier is not intrinsically meaningful
    and cannot be modified.

    \sa QMailThread, QMailStore::thread()
*/

/*!
    \typedef QMailThreadIdList
    \relates QMailThreadId
*/

Q_IMPLEMENT_USER_METATYPE(QMailThreadId)

/*!
    Construct an uninitialized QMailThreadId, for which isValid() returns false.
*/
QMailThreadId::QMailThreadId()
    : d(new QMailIdPrivate)
{
}

/*!
    Construct a QMailThreadId with the supplied numeric identifier \a value.
*/
QMailThreadId::QMailThreadId(quint64 value)
    : d(new QMailIdPrivate(value))
{
}

/*! \internal */
QMailThreadId::QMailThreadId(const QMailThreadId &other)
    : d(new QMailIdPrivate(*other.d))
{
}

/*! \internal */
QMailThreadId::~QMailThreadId()
{
}

/*! \internal */
QMailThreadId& QMailThreadId::operator=(const QMailThreadId &other)
{
    *d = *other.d;
    return *this;
}

/*!
    Returns true if this object has been initialized with an identifier.
*/
bool QMailThreadId::isValid() const
{
    return d->isValid();
}

/*! \internal */
quint64 QMailThreadId::toULongLong() const
{
    return d->toULongLong();
}

/*!
    Returns a QVariant containing the value of this thread identfier.
*/
QMailThreadId::operator QVariant() const
{
    return QVariant::fromValue(*this);
}

/*!
    Returns true if this object's identifier value differs from that of \a other.
*/
bool QMailThreadId::operator!=(const QMailThreadId &other) const
{
    return *d != *other.d;
}

/*!
    Returns true if this object's identifier value is equal to that of \a other.
*/
bool QMailThreadId::operator==(const QMailThreadId& other) const
{
    return *d == *other.d;
}

/*!
    Returns true if this object's identifier value is less than that of \a other.
*/
bool QMailThreadId::operator<(const QMailThreadId& other) const
{
    return *d < *other.d;
}

/*!
    \fn QMailThreadId::serialize(Stream&) const
    \internal
*/
template <typename Stream> void QMailThreadId::serialize(Stream &stream) const
{
    d->serialize(stream);
}

/*!
    \fn QMailThreadId::deserialize(Stream&)
    \internal
*/
template <typename Stream> void QMailThreadId::deserialize(Stream &stream)
{
    d->deserialize(stream);
}

/*! \internal */
QDebug& operator<<(QDebug& debug, const QMailThreadId &id)
{
    id.serialize(debug);
    return debug;
}

/*! \internal */
QTextStream& operator<< (QTextStream& s, const QMailThreadId &id)
{
    id.serialize(s);
    return s;
}

Q_IMPLEMENT_USER_METATYPE_TYPEDEF(QMailThreadIdList, QMailThreadIdList)


/*!
    \class QMailFolderId
    \ingroup messaginglibrary

    \preliminary
    \brief The QMailFolderId class is used to identify folders stored by QMailStore.

    QMailFolderId is a class used to represent folders stored by the QMailStore, identified
    by their unique numeric internal indentifer.

    A QMailFolderId instance can be tested for validity, and compared to other instances
    for equality.  The numeric value of the identifier is not intrinsically meaningful 
    and cannot be modified.
    
    \sa QMailFolder, QMailStore::folder()
*/

/*!
    \typedef QMailFolderIdList
    \relates QMailFolderId
*/

Q_IMPLEMENT_USER_METATYPE(QMailFolderId)

/*! 
    Construct an uninitialized QMailFolderId, for which isValid() returns false.
*/
QMailFolderId::QMailFolderId()
    : d(new QMailIdPrivate)
{
}

/*! 
    Construct a QMailFolderId corresponding to the predefined folder identifier \a id.
*/
QMailFolderId::QMailFolderId(QMailFolderFwd::PredefinedFolderId id)
    : d(new QMailIdPrivate(static_cast<quint64>(id)))
{
}

/*! 
    Construct a QMailFolderId with the supplied numeric identifier \a value.
*/
QMailFolderId::QMailFolderId(quint64 value)
    : d(new QMailIdPrivate(value))
{
}

/*! \internal */
QMailFolderId::QMailFolderId(const QMailFolderId& other)
    : d(new QMailIdPrivate(*other.d))
{
}

/*! \internal */
QMailFolderId::~QMailFolderId()
{
}

/*! \internal */
QMailFolderId& QMailFolderId::operator=(const QMailFolderId& other) 
{
    *d = *other.d;
    return *this;
}

/*!
    Returns true if this object has been initialized with an identifier.
*/
bool QMailFolderId::isValid() const
{
    return d->isValid();
}

/*! \internal */
quint64 QMailFolderId::toULongLong() const
{
    return d->toULongLong();
}

/*!
    Returns a QVariant containing the value of this folder identfier.
*/
QMailFolderId::operator QVariant() const
{
    return QVariant::fromValue(*this);
}

/*!
    Returns true if this object's identifier value differs from that of \a other.
*/
bool QMailFolderId::operator!=(const QMailFolderId& other) const
{
    return *d != *other.d;
}

/*!
    Returns true if this object's identifier value is equal to that of \a other.
*/
bool QMailFolderId::operator==(const QMailFolderId& other) const
{
    return *d == *other.d;
}

/*!
    Returns true if this object's identifier value is less than that of \a other.
*/
bool QMailFolderId::operator<(const QMailFolderId& other) const
{
    return *d < *other.d;
}

/*! 
    \fn QMailFolderId::serialize(Stream&) const
    \internal 
*/
template <typename Stream> void QMailFolderId::serialize(Stream &stream) const
{
    d->serialize(stream);
}

/*! 
    \fn QMailFolderId::deserialize(Stream&)
    \internal 
*/
template <typename Stream> void QMailFolderId::deserialize(Stream &stream)
{
    d->deserialize(stream);
}

/*! \internal */
QDebug& operator<< (QDebug& debug, const QMailFolderId &id)
{
    id.serialize(debug);
    return debug;
}

/*! \internal */
QTextStream& operator<< (QTextStream& s, const QMailFolderId &id)
{
    id.serialize(s);
    return s;
}

Q_IMPLEMENT_USER_METATYPE_TYPEDEF(QMailFolderIdList, QMailFolderIdList)


/*!
    \class QMailMessageId
    \ingroup messaginglibrary

    \preliminary
    \brief The QMailMessageId class is used to identify messages stored by QMailStore.

    QMailMessageId is a class used to represent messages stored by the QMailStore, identified
    by their unique numeric internal indentifer.

    A QMailMessageId instance can be tested for validity, and compared to other instances
    for equality.  The numeric value of the identifier is not intrinsically meaningful 
    and cannot be modified.
    
    \sa QMailMessage, QMailStore::message()
*/

/*!
    \typedef QMailMessageIdList
    \relates QMailMessageId
*/

Q_IMPLEMENT_USER_METATYPE(QMailMessageId)

/*! 
    Construct an uninitialized QMailMessageId, for which isValid() returns false.
*/
QMailMessageId::QMailMessageId()
    : d(new QMailIdPrivate)
{
}

/*! 
    Construct a QMailMessageId with the supplied numeric identifier \a value.
*/
QMailMessageId::QMailMessageId(quint64 value)
    : d(new QMailIdPrivate(value))
{
}

/*! \internal */
QMailMessageId::QMailMessageId(const QMailMessageId& other)
    : d(new QMailIdPrivate(*other.d))
{
}

/*! \internal */
QMailMessageId::~QMailMessageId()
{
}

/*! \internal */
QMailMessageId& QMailMessageId::operator=(const QMailMessageId& other) 
{
    *d = *other.d;
    return *this;
}

/*!
    Returns true if this object has been initialized with an identifier.
*/
bool QMailMessageId::isValid() const
{
    return d->isValid();
}

/*! \internal */
quint64 QMailMessageId::toULongLong() const
{
    return d->toULongLong();
}

/*!
    Returns a QVariant containing the value of this message identfier.
*/
QMailMessageId::operator QVariant() const
{
    return QVariant::fromValue(*this);
}

/*!
    Returns true if this object's identifier value differs from that of \a other.
*/
bool QMailMessageId::operator!= (const QMailMessageId& other) const
{
    return *d != *other.d;
}

/*!
    Returns true if this object's identifier value is equal to that of \a other.
*/
bool QMailMessageId::operator== (const QMailMessageId& other) const
{
    return *d == *other.d;
}

/*!
    Returns true if this object's identifier value is less than that of \a other.
*/
bool QMailMessageId::operator< (const QMailMessageId& other) const
{
    return *d < *other.d;
}

/*! 
    \fn QMailMessageId::serialize(Stream&) const
    \internal 
*/
template <typename Stream> void QMailMessageId::serialize(Stream &stream) const
{
    d->serialize(stream);
}

/*! 
    \fn QMailMessageId::deserialize(Stream&)
    \internal 
*/
template <typename Stream> void QMailMessageId::deserialize(Stream &stream)
{
    d->deserialize(stream);
}

/*! \internal */
QDebug& operator<<(QDebug& debug, const QMailMessageId &id)
{
    id.serialize(debug);
    return debug;
}

/*! \internal */
QTextStream& operator<<(QTextStream& s, const QMailMessageId &id)
{
    id.serialize(s);
    return s;
}

Q_IMPLEMENT_USER_METATYPE_TYPEDEF(QMailMessageIdList, QMailMessageIdList)

/*! \internal */
uint qHash(const QMailAccountId &id)
{
    return qHash(id.toULongLong());
}

/*! \internal */
uint qHash(const QMailFolderId &id)
{
    return qHash(id.toULongLong());
}

/*! \internal */
uint qHash(const QMailMessageId &id)
{
    return qHash(id.toULongLong());
}
/*! \internal */
uint qHash(const QMailThreadId &id)
{
    return qHash(id.toULongLong());
}


