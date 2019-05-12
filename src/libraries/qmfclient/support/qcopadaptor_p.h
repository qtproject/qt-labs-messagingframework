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

#ifndef QCOPADAPTOR_H
#define QCOPADAPTOR_H

#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qbytearray.h>
#include <QtCore/qvariant.h>
#include <QtCore/qstringlist.h>

#if !defined(Q_QCOP_EXPORT)
#if defined(QT_BUILD_QCOP_LIB)
#define Q_QCOP_EXPORT Q_DECL_EXPORT
#else
#define Q_QCOP_EXPORT Q_DECL_IMPORT
#endif
#endif

class QCopAdaptorPrivate;
class QCopAdaptorEnvelopePrivate;

class Q_QCOP_EXPORT QCopAdaptorEnvelope
{
    friend class QCopAdaptor;
private:
    QCopAdaptorEnvelope(const QStringList& channels, const QString& message);

public:
    QCopAdaptorEnvelope();
    QCopAdaptorEnvelope(const QCopAdaptorEnvelope& value);
    ~QCopAdaptorEnvelope();

    QCopAdaptorEnvelope& operator=(const QCopAdaptorEnvelope& value);
    template <class T>
    QCopAdaptorEnvelope& operator<<(const T &value);

    inline QCopAdaptorEnvelope& operator<<(const char *value)
    {
        addArgument(QVariant(QString::fromUtf8(value)));
        return *this;
    }

private:
    QCopAdaptorEnvelopePrivate *d;

    void addArgument(const QVariant& value);
};

class Q_QCOP_EXPORT QCopAdaptor : public QObject
{
    Q_OBJECT
    friend class QCopAdaptorPrivate;
    friend class QCopAdaptorEnvelope;
    friend class QCopAdaptorChannel;
public:
    explicit QCopAdaptor(const QString& channel, QObject *parent = Q_NULLPTR);
    ~QCopAdaptor();

    QString channel() const;

    static bool connect(QObject *sender, const QByteArray& signal,
                        QObject *receiver, const QByteArray& member);

    QCopAdaptorEnvelope send(const QByteArray& member);
    void send(const QByteArray& member, const QVariant &arg1);
    void send(const QByteArray& member, const QVariant &arg1,
              const QVariant &arg2);
    void send(const QByteArray& member, const QVariant &arg1,
              const QVariant &arg2, const QVariant &arg3);
    void send(const QByteArray& member, const QVariant &arg1,
              const QVariant &arg2, const QVariant &arg3,
              const QVariant &arg4);
    void send(const QByteArray& member, const QList<QVariant>& args);

    bool isConnected(const QByteArray& signal);

 signals:
    void connectionDown();
    void reconnectionTimeout();

protected:
    enum PublishType
    {
        Signals,
        Slots,
        SignalsAndSlots
    };

    bool publish(const QByteArray& member);
    void publishAll(QCopAdaptor::PublishType type);
    virtual QString memberToMessage(const QByteArray& member);
    virtual QStringList sendChannels(const QString& channel);
    virtual QString receiveChannel(const QString& channel);

private slots:
    void received(const QString& msg, const QByteArray& data);
    void receiverDestroyed();

private:
    QCopAdaptorPrivate *d;

    bool connectLocalToRemote(QObject *sender, const QByteArray& signal,
                              const QByteArray& member);
    bool connectRemoteToLocal(const QByteArray& signal, QObject *receiver,
                              const QByteArray& member);
    void sendMessage(const QString& msg, const QList<QVariant>& args);
    static void send(const QStringList& channels,
                     const QString& msg, const QList<QVariant>& args);
};

template<class T>
QCopAdaptorEnvelope& QCopAdaptorEnvelope::operator<<(const T &value)
{
    addArgument(qVariantFromValue(value));
    return *this;
}

// Useful alias to make it clearer when connecting to messages on a channel.
#define MESSAGE(x)      "3"#x
#define QMESSAGE_CODE   3

#endif
