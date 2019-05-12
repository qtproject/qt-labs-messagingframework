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

#include <QObject>
#include <QTest>
#include <ctype.h>
#include <qmailaccountlistmodel.h>
#include <qmailmessagelistmodel.h>
#include <qmailaccount.h>
#include <qmailaccountconfiguration.h>
#include <qmailstore.h>
#include "qmailmessagethreadedmodel.h"

#define CRLF "\015\012"

class tst_QMail_ListModels : public QObject
{
    Q_OBJECT

public:
    tst_QMail_ListModels() {}
    virtual ~tst_QMail_ListModels() {}

    //friend class QMailAccountKey;

private slots:
    void initTestCase();
    void cleanupTestCase();

    void test_qmailaccountlistmodel();
    void test_qmailmessagelistmodel();
    void test_messagethreadedmodel();

private:
    QMailAccountConfiguration makeConfig(const QString &accountName);
    QMailAccount account1;
    QMailAccount account2;

    QMailMessage msg1;
    QMailMessage msg2;
};

QTEST_MAIN(tst_QMail_ListModels)

#include "tst_qmail_listmodels.moc"


QMailAccountConfiguration tst_QMail_ListModels::makeConfig(const QString &accountName)
{
    QMailAccountConfiguration config;
    config.addServiceConfiguration("imap4");
    if (QMailAccountConfiguration::ServiceConfiguration *svcCfg = &config.serviceConfiguration("imap4")) {
        svcCfg->setValue("server", "mail.example.org");
        svcCfg->setValue("username", accountName);
    }
    config.addServiceConfiguration("smtp");
    if (QMailAccountConfiguration::ServiceConfiguration *svcCfg = &config.serviceConfiguration("smtp")) {
        svcCfg->setValue("server", "mail.example.org");
        svcCfg->setValue("username", accountName);
    }
    return config;
}

void tst_QMail_ListModels::initTestCase()
{
    QMailStore::instance();

    // QMailAccount initializations

    QMailAccount &account = account1;
    account.setName("Account 1");
    account.setMessageType(QMailMessage::Email);
    account.setFromAddress(QMailAddress("Account 1", "account1@example.org"));
    account.setStatus(QMailAccount::SynchronizationEnabled, false);
    account.setStatus(QMailAccount::Synchronized, false);
    account.setStatus(QMailAccount::MessageSource, true);
    account.setStatus(QMailAccount::CanRetrieve, true);
    account.setStatus(QMailAccount::MessageSink, true);
    account.setStatus(QMailAccount::CanTransmit, true);
    account.setCustomField("verified", "true");
    account.setCustomField("question", "What is your dog's name?");
    account.setCustomField("answer", "Fido");
    QMailAccountConfiguration config1 = makeConfig("account1");
    QVERIFY(QMailStore::instance()->addAccount(&account, &config1));

    account2.setName("Account 2");
    account2.setMessageType(QMailMessage::Email);
    account2.setFromAddress(QMailAddress("Account 2", "account2@example.org"));
    account2.setStatus(QMailAccount::SynchronizationEnabled, true);
    account2.setStatus(QMailAccount::Synchronized, true);
    account2.setStatus(QMailAccount::MessageSource, true);
    account2.setStatus(QMailAccount::CanRetrieve, true);
    account2.setStatus(QMailAccount::MessageSink, true);
    account2.setStatus(QMailAccount::CanTransmit, true);
    account2.setCustomField("verified", "true");
    account2.setCustomField("question", "What is your dog's name?");
    account2.setCustomField("answer", "Milo");
    QMailAccountConfiguration config2 = makeConfig("account2");
    QVERIFY(QMailStore::instance()->addAccount(&account2, &config2));

    // QMailMessage initializations
    QByteArray m1("From: newguy@example.org" CRLF
                  "To: oldguy@example.org" CRLF
                  "Cc: anotherguy@example.org" CRLF
                  "Date: Tue, 22 Feb 2011 16:00 +0200" CRLF
                  "Subject: Email message 1" CRLF
                  "Content-Type: text/plain; charset=UTF-8" CRLF
                  CRLF
                  "Plain text message for testing.");

    msg1.setMessageType(QMailMessage::Sms);
    msg1.setParentAccountId(account1.id());
    msg1.setParentFolderId(QMailFolder::LocalStorageFolderId);
    msg1.setFrom(QMailAddress("0404404040"));
    msg1.setTo(QMailAddress("0404040404"));
    msg1.setSubject("Where are you?");
    msg1.setDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
    msg1.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
    msg1.setStatus(QMailMessage::Incoming, true);
    msg1.setStatus(QMailMessage::New, false);
    msg1.setStatus(QMailMessage::Read, true);
    msg1.setServerUid("sim:12345");
    msg1.setSize(1 * 1024);
    msg1.setContent(QMailMessage::PlainTextContent);
    msg1.setCustomField("present", "true");
    msg1.setCustomField("todo", "true");
    QVERIFY(QMailStore::instance()->addMessage(&msg1));

    msg2.setMessageType(QMailMessage::Email);
    msg2.setParentAccountId(account2.id());
    msg2.setParentFolderId(QMailFolder::LocalStorageFolderId);
    msg2.setFrom(QMailAddress("newguy@example.org"));
    msg2.setTo(QMailAddress("old@example.org"));
    msg2.setCc(QMailAddressList() << QMailAddress("anotherguy@example.org"));
    msg2.setSubject("email message test");
    msg2.setDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
    msg2.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
    msg2.setStatus(QMailMessage::Incoming, true);
    msg2.setStatus(QMailMessage::New, true);
    msg2.setStatus(QMailMessage::Read, false);
    msg2.setServerUid("inboxmsg21");
    msg2.setSize(5 * 1024);
    msg2.setContent(QMailMessage::PlainTextContent);
    msg2.setCustomField("present", "true");
    msg2.setBody(QMailMessageBody::fromData(m1,QMailMessageContentType("text/plain"),QMailMessageBody::QuotedPrintable));
    QVERIFY(QMailStore::instance()->addMessage(&msg2));
}

void tst_QMail_ListModels::cleanupTestCase()
{
    QMailStore::instance()->removeAccounts(QMailAccountKey::customField("verified"));
    QMailStore::instance()->removeMessages(QMailMessageKey::customField("present"));
}

void tst_QMail_ListModels::test_qmailaccountlistmodel()
{
    QMailAccountListModel model;

    QMailAccountKey key(QMailAccountKey::fromAddress(account1.fromAddress().address()));
    model.setKey(key);
    QCOMPARE(model.key(), key);

    QModelIndex idx = model.indexFromId(account1.id());
    QCOMPARE(model.idFromIndex(idx), account1.id());

    model.setSynchronizeEnabled(false);
    QVERIFY(!model.synchronizeEnabled());

    model.setSynchronizeEnabled(true);
    QVERIFY(model.synchronizeEnabled());

    QCOMPARE(model.data(idx).toString(), account1.name());
    QCOMPARE(model.rowCount(), 1);

    QMailAccountSortKey sortasc = QMailAccountSortKey::id();
    model.setSortKey(sortasc);
    QCOMPARE(model.sortKey(), sortasc);

    // with email key
    QMailAccountKey key2(QMailAccountKey::messageType(QMailMessageMetaDataFwd::Email));
    model.setKey(key2);
    QCOMPARE(model.key(), key2);
    QCOMPARE(model.rowCount(), 2);

    QString val = model.data(model.indexFromId(account1.id())).toString();
    QCOMPARE(val, account1.name());

    val = model.data(model.indexFromId(account2.id())).toString();
    QCOMPARE(val, account2.name());



    QMailAccount account3;
    account3.setName("Account 3");
    account3.setMessageType(QMailMessage::Email);
    account3.setFromAddress(QMailAddress("Account 3", "account3@example.org"));
    account3.setStatus(QMailAccount::SynchronizationEnabled, true);
    account3.setStatus(QMailAccount::Synchronized, true);
    account3.setStatus(QMailAccount::MessageSource, true);
    account3.setStatus(QMailAccount::CanRetrieve, true);
    account3.setStatus(QMailAccount::MessageSink, true);
    account3.setStatus(QMailAccount::CanTransmit, true);
    account3.setCustomField("verified", "true");
    account3.setCustomField("question", "What is your dog's name?");
    account3.setCustomField("answer", "Milo1");
    QMailAccountConfiguration config2 = makeConfig("account3");

    QMailAccountId invalidId(999u);
    QVERIFY(QMailStore::instance()->addAccount(&account3, &config2));
    model.accountsAdded(QMailAccountIdList() << account3.id() << invalidId);

    account3.setCustomField("answer", "teenu");
    QVERIFY(QMailStore::instance()->updateAccount(&account3, &config2));
    model.accountsUpdated(QMailAccountIdList() << account3.id() << invalidId);

    QVERIFY(QMailStore::instance()->removeAccount(account3.id()));
    model.accountsRemoved(QMailAccountIdList() << account3.id() << invalidId);
}

void tst_QMail_ListModels::test_qmailmessagelistmodel()
{
    QMailMessageListModel model;
    QMailMessageModelImplementation *p = const_cast<QMailMessageModelImplementation *>(model.impl());

    QModelIndex idx = p->indexFromId(msg1.id());
    //QCOMPARE(p->rowCount(idx), 2);
    //QCOMPARE(model.data(idx).toString(), msg1.subject());
    QMailMessageId id = msg2.id();

    QMailMessageKey key = QMailMessageKey::messageType(QMailMessageMetaDataFwd::Email);
    QMailMessageSortKey sortkey = QMailMessageSortKey::id();
    p->setKey(key);
    QCOMPARE(p->key(), key);
    p->setSortKey(sortkey);
    QCOMPARE(p->sortKey(), sortkey);

    idx = model.indexFromId(msg1.id());
    QString data = model.data(idx).toString();
    QString stop;

}

void tst_QMail_ListModels::test_messagethreadedmodel()
{
    QMailMessageThreadedModel model;
    QModelIndex idx = model.generateIndex(0, 0, Q_NULLPTR);
    Q_UNUSED(idx);
}
