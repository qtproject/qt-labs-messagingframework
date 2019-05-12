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

#include "qmailheartbeattimer.h"
#include <QPair>

class QMailHeartbeatTimerPrivate
{
public:
    QMailHeartbeatTimerPrivate()
        : timer(new QTimer), interval(0, 0)
    {}

    ~QMailHeartbeatTimerPrivate()
    {
        delete timer;
    }

    QTimer* timer;
    QPair<int, int> interval;
};

QMailHeartbeatTimer::QMailHeartbeatTimer(QObject *parent)
    : QObject(parent), d_ptr(new QMailHeartbeatTimerPrivate)
{
    Q_D(QMailHeartbeatTimer);
    connect(d->timer, SIGNAL(timeout()), this, SIGNAL(timeout()));
}

QMailHeartbeatTimer::~QMailHeartbeatTimer()
{
    delete d_ptr;
}

bool QMailHeartbeatTimer::isActive() const
{
    const Q_D(QMailHeartbeatTimer);
    return d->timer->isActive();
}

void QMailHeartbeatTimer::setInterval(int interval)
{
    setInterval(interval, interval);
}

void QMailHeartbeatTimer::setInterval(int minimum, int maximum)
{
    Q_ASSERT(minimum <= maximum);
    Q_D(QMailHeartbeatTimer);
    d->timer->setInterval((minimum + maximum) / 2);
    d->interval = qMakePair(minimum, maximum);
}

QPair<int, int> QMailHeartbeatTimer::interval() const
{
    const Q_D(QMailHeartbeatTimer);
    return d->interval;
}

void QMailHeartbeatTimer::setSingleShot(bool singleShot)
{
    Q_D(QMailHeartbeatTimer);
    d->timer->setSingleShot(singleShot);
}

bool QMailHeartbeatTimer::isSingleShot() const
{
    const Q_D(QMailHeartbeatTimer);
    return d->timer->isSingleShot();
}

void QMailHeartbeatTimer::singleShot(int minimum, int maximum, QObject *receiver, const char *member)
{
    Q_ASSERT(minimum <= maximum);
    QTimer::singleShot((minimum + maximum) / 2, receiver, member);
}

void QMailHeartbeatTimer::singleShot(int interval, QObject *receiver, const char *member)
{
    QTimer::singleShot(interval, receiver, member);
}

void QMailHeartbeatTimer::start(int interval)
{
    start(interval, interval);
}

void QMailHeartbeatTimer::start(int minimum, int maximum)
{
    Q_ASSERT(minimum <= maximum);
    Q_D(QMailHeartbeatTimer);
    d->timer->start((minimum + maximum) / 2);
}

void QMailHeartbeatTimer::start()
{
    Q_D(QMailHeartbeatTimer);
    d->timer->start();
}

void QMailHeartbeatTimer::stop()
{
    Q_D(QMailHeartbeatTimer);
    d->timer->stop();
}

void QMailHeartbeatTimer::wokeUp()
{
    if (isSingleShot()) {
        stop();
    } else {
        start();
    }
}
