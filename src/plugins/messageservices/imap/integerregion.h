/****************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the Qt Messaging Framework.
**
** $QT_BEGIN_LICENSE:LGPL$
** No Commercial Usage
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef INTEGERREGION_H
#define INTEGERREGION_H

#include <QString>
#include <QStringList>
#include <QPair>

typedef QPair<int, int> IntegerRange;

class IntegerRegion
{
public:
    IntegerRegion();
    IntegerRegion(const QStringList &uids);
    IntegerRegion(const QString &uidString);
    IntegerRegion(int begin, int end);

    void clear();
    bool isEmpty() const;
    uint cardinality() const;
    int maximum() const;
    int minimum() const;
    QStringList toStringList() const;
    QString toString() const;

    void add(int number);
    IntegerRegion subtract(IntegerRegion) const;
    IntegerRegion add(IntegerRegion) const;
    IntegerRegion intersect(IntegerRegion) const;

    static bool isIntegerRegion(QStringList uids);

    static QList<int> toList(const QString &region);

#if 0
// TODO Convert these tests to standard qunit style
    static IntegerRegion fromBinaryString(const QString &);
    static QString toBinaryString(const IntegerRegion &);
    static int tests();
#endif

private:
    // TODO: Consider using shared pointer
    QList< QPair<int, int> > mRangeList;
};

#endif
