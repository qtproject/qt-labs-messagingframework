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
#include "qmailmessageset.h"
#include <private/qmailmessageset_p.h>
#include "qmailstore.h"

/*
    This class primarily tests that QMailMessageSet class correctly stores messages.
*/
class tst_QMailMessageSet : public QObject
{
    Q_OBJECT

public:
    tst_QMailMessageSet() {}
    virtual ~tst_QMailMessageSet() {}

private slots:
    void initTestCase();
    void cleanupTestCase();

    void test_foldermessageset();
    void test_accountmessageset();
    void test_filtermessageset();
    void test_messagesetcontainer();

private:
    QMailAccountId accountId1, accountId2, accountId3, accountId4;
    QMailFolderId inboxId1, inboxId2, savedId1, savedId2, archivedId1, archivedId2;
    QMailMessageId smsMessage, inboxMessage1, archivedMessage1, inboxMessage2, savedMessage2;

    QSet<QMailAccountId> noAccounts, verifiedAccounts, unverifiedAccounts, allAccounts;
    QSet<QMailFolderId> noFolders, allFolders, standardFolders;
    QSet<QMailMessageId> noMessages, allMessages, allEmailMessages;

};

QTEST_MAIN(tst_QMailMessageSet)

#include "tst_qmailmessageset.moc"

void tst_QMailMessageSet::initTestCase()
{
    // Instantiate the store to initialise the values of the status flags and create the standard folders
    QMailStore::instance();

    // Create the data set we will test our keys upon

    standardFolders << QMailFolderId(QMailFolder::LocalStorageFolderId);

    {
        QMailAccount account;
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

        QMailAccountConfiguration config;
        config.addServiceConfiguration("imap4");
        if (QMailAccountConfiguration::ServiceConfiguration *svcCfg = &config.serviceConfiguration("imap4")) {
            svcCfg->setValue("server", "mail.example.org");
            svcCfg->setValue("username", "account1");
        }
        config.addServiceConfiguration("smtp");
        if (QMailAccountConfiguration::ServiceConfiguration *svcCfg = &config.serviceConfiguration("smtp")) {
            svcCfg->setValue("server", "mail.example.org");
            svcCfg->setValue("username", "account1");
        }

        QVERIFY(QMailStore::instance()->addAccount(&account, &config));
        accountId1 = account.id();
        allAccounts << account.id();
        verifiedAccounts << account.id();
    }

    {
        QMailAccount account;
        account.setName("Account 2");
        account.setMessageType(QMailMessage::Instant);
        account.setFromAddress(QMailAddress("Account 2", "account2@example.org"));
        account.setStatus(QMailAccount::SynchronizationEnabled, true);
        account.setStatus(QMailAccount::Synchronized, false);
        account.setStatus(QMailAccount::MessageSource, true);
        account.setStatus(QMailAccount::CanRetrieve, true);
        account.setCustomField("verified", "true");
        account.setCustomField("question", "What is your dog's name?");
        account.setCustomField("answer", "Lassie");

        QMailAccountConfiguration config;
        config.addServiceConfiguration("imap4");
        if (QMailAccountConfiguration::ServiceConfiguration *svcCfg = &config.serviceConfiguration("imap4")) {
            svcCfg->setValue("server", "imap.example.org");
            svcCfg->setValue("username", "account2");
        }

        QVERIFY(QMailStore::instance()->addAccount(&account, &config));
        accountId2 = account.id();
        allAccounts << account.id();
        verifiedAccounts << account.id();
    }

    {
        QMailAccount account;
        account.setName("Account 3");
        account.setMessageType(QMailMessage::None);
        account.setFromAddress(QMailAddress("Account 3", "account3@test"));
        account.setCustomField("verified", "false");

        QVERIFY(QMailStore::instance()->addAccount(&account, Q_NULLPTR));
        accountId3 = account.id();
        allAccounts << account.id();
        unverifiedAccounts << account.id();
    }

    {
        QMailAccount account;
        account.setName("Account 4");
        account.setMessageType(QMailMessage::None);
        account.setFromAddress(QMailAddress("Account 4", "account4@test"));
        account.setCustomField("verified", "false");

        QVERIFY(QMailStore::instance()->addAccount(&account, Q_NULLPTR));
        accountId4 = account.id();
        allAccounts << account.id();
        unverifiedAccounts << account.id();
    }

    {
        QMailFolder folder;
        folder.setPath("Inbox");
        folder.setDisplayName("Inbox");
        folder.setParentFolderId(QMailFolderId());
        folder.setParentAccountId(accountId1);
        folder.setStatus(QMailFolder::SynchronizationEnabled, true);
        folder.setStatus(QMailFolder::Synchronized, false);
        folder.setCustomField("uidValidity", "abcdefg");
        folder.setCustomField("uidNext", "1");

        QVERIFY(QMailStore::instance()->addFolder(&folder));
        inboxId1 = folder.id();
        allFolders << folder.id();
    }

    {
        QMailFolder folder;
        folder.setPath("Inbox/Saved");
        folder.setDisplayName("Saved");
        folder.setParentFolderId(inboxId1);
        folder.setParentAccountId(accountId1);
        folder.setStatus(QMailFolder::SynchronizationEnabled, true);
        folder.setStatus(QMailFolder::Synchronized, true);
        folder.setCustomField("uidValidity", "hijklmnop");
        folder.setCustomField("uidNext", "11");

        QVERIFY(QMailStore::instance()->addFolder(&folder));
        savedId1 = folder.id();
        allFolders << folder.id();
    }

    {
        QMailFolder folder;
        folder.setPath("Inbox/Saved/Archived");
        folder.setDisplayName("Archived");
        folder.setParentFolderId(savedId1);
        folder.setParentAccountId(accountId1);
        folder.setStatus(QMailFolder::SynchronizationEnabled, false);
        folder.setStatus(QMailFolder::Synchronized, false);
        folder.setCustomField("archived", "true");
        folder.setCustomField("uidNext", "111");

        QVERIFY(QMailStore::instance()->addFolder(&folder));
        archivedId1 = folder.id();
        allFolders << folder.id();
    }

    {
        QMailFolder folder;
        folder.setPath("Inbox");
        folder.setDisplayName("Inbox");
        folder.setParentFolderId(QMailFolderId());
        folder.setParentAccountId(accountId2);
        folder.setStatus(QMailFolder::SynchronizationEnabled, false);
        folder.setStatus(QMailFolder::Synchronized, false);
        folder.setCustomField("uidValidity", "qrstuv");
        folder.setCustomField("uidNext", "1");

        QVERIFY(QMailStore::instance()->addFolder(&folder));
        inboxId2 = folder.id();
        allFolders << folder.id();
    }

    {
        QMailFolder folder;
        folder.setPath("Inbox/Saved");
        folder.setDisplayName("Saved");
        folder.setParentFolderId(inboxId2);
        folder.setParentAccountId(accountId2);
        folder.setStatus(QMailFolder::SynchronizationEnabled, false);
        folder.setStatus(QMailFolder::Synchronized, true);
        folder.setCustomField("uidValidity", "wxyz");
        folder.setCustomField("uidNext", "11");

        QVERIFY(QMailStore::instance()->addFolder(&folder));
        savedId2 = folder.id();
        allFolders << folder.id();
    }

    {
        QMailFolder folder;
        folder.setPath("Inbox/Saved/Archived");
        folder.setDisplayName("Archived");
        folder.setParentFolderId(savedId2);
        folder.setParentAccountId(accountId2);
        folder.setStatus(QMailFolder::SynchronizationEnabled, false);
        folder.setStatus(QMailFolder::Synchronized, false);
        folder.setCustomField("archived", "true");
        folder.setCustomField("uidNext", "111");

        QVERIFY(QMailStore::instance()->addFolder(&folder));
        archivedId2 = folder.id();
        allFolders << folder.id();
    }

    {
        QMailMessage message;
        message.setMessageType(QMailMessage::Sms);
        message.setParentAccountId(accountId4);
        message.setParentFolderId(QMailFolder::LocalStorageFolderId);
        message.setFrom(QMailAddress("0404404040"));
        message.setTo(QMailAddress("0404040404"));
        message.setSubject("Where are you?");
        message.setDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
        message.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
        message.setStatus(QMailMessage::Incoming, true);
        message.setStatus(QMailMessage::New, false);
        message.setStatus(QMailMessage::Read, true);
        message.setServerUid("sim:12345");
        message.setSize(1 * 1024);
        message.setContent(QMailMessage::PlainTextContent);
        message.setCustomField("present", "true");
        message.setCustomField("todo", "true");

        QVERIFY(QMailStore::instance()->addMessage(&message));
        smsMessage = message.id();
        allMessages << message.id();
    }

    {
        QMailMessage message;
        message.setMessageType(QMailMessage::Email);
        message.setParentAccountId(accountId1);
        message.setParentFolderId(inboxId1);
        message.setFrom(QMailAddress("account2@example.org"));
        message.setTo(QMailAddress("account1@example.org"));
        message.setSubject("inboxMessage1");
        message.setDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
        message.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
        message.setStatus(QMailMessage::Incoming, true);
        message.setStatus(QMailMessage::New, true);
        message.setStatus(QMailMessage::Read, false);
        message.setServerUid("inboxMessage1");
        message.setSize(5 * 1024);
        message.setContent(QMailMessage::PlainTextContent);
        message.setCustomField("present", "true");

        QVERIFY(QMailStore::instance()->addMessage(&message));
        inboxMessage1 = message.id();
        allMessages << message.id();
        allEmailMessages << message.id();
    }

    {
        QMailMessage message;
        message.setMessageType(QMailMessage::Email);
        message.setParentAccountId(accountId1);
        message.setParentFolderId(inboxId1);
        message.setFrom(QMailAddress("account1@example.org"));
        message.setTo(QMailAddress("fred@example.net"));
        message.setSubject("archivedMessage1");
        message.setDate(QMailTimeStamp(QDateTime(QDate::currentDate().addDays(-1))));
        message.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate().addDays(-1))));
        message.setStatus(QMailMessage::Outgoing, true);
        message.setStatus(QMailMessage::New, false);
        message.setStatus(QMailMessage::Sent, true);
        message.setServerUid("archivedMessage1");
        message.setSize(15 * 1024);
        message.setContent(QMailMessage::VideoContent);
        message.setCustomField("present", "true");

        QVERIFY(QMailStore::instance()->addMessage(&message));

        message.setPreviousParentFolderId(message.parentFolderId());
        message.setParentFolderId(archivedId1);
        QVERIFY(QMailStore::instance()->updateMessage(&message));

        archivedMessage1 = message.id();
        allMessages << message.id();
        allEmailMessages << message.id();
    }

    {
        QMailMessage message;
        message.setMessageType(QMailMessage::Email);
        message.setParentAccountId(accountId2);
        message.setParentFolderId(inboxId2);
        message.setFrom(QMailAddress("account1@example.org"));
        message.setTo(QMailAddress("account2@example.org"));
        message.setSubject("Fwd:inboxMessage2");
        message.setDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
        message.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate())));
        message.setStatus(QMailMessage::Incoming, true);
        message.setStatus(QMailMessage::New, true);
        message.setStatus(QMailMessage::Read, true);
        message.setServerUid("inboxMessage2");
        message.setSize(5 * 1024);
        message.setContent(QMailMessage::HtmlContent);
        message.setInResponseTo(inboxMessage1);
        message.setResponseType(QMailMessage::Forward);
        message.setCustomField("present", "true");
        message.setCustomField("todo", "false");

        QVERIFY(QMailStore::instance()->addMessage(&message));
        inboxMessage2 = message.id();
        allMessages << message.id();
        allEmailMessages << message.id();
    }

    {
        QMailMessage message;
        message.setMessageType(QMailMessage::Email);
        message.setParentAccountId(accountId2);
        message.setParentFolderId(inboxId2);
        message.setFrom(QMailAddress("fred@example.net"));
        message.setTo(QMailAddressList() << QMailAddress("account2@example.org") << QMailAddress("testing@test"));
        message.setSubject("Re:savedMessage2");
        message.setDate(QMailTimeStamp(QDateTime(QDate::currentDate().addDays(-7))));
        message.setReceivedDate(QMailTimeStamp(QDateTime(QDate::currentDate().addDays(-7))));
        message.setStatus(QMailMessage::Incoming, true);
        message.setStatus(QMailMessage::New, false);
        message.setStatus(QMailMessage::Read, false);
        message.setServerUid("savedMessage2");
        message.setSize(5 * 1024);
        message.setContent(QMailMessage::HtmlContent);
        message.setInResponseTo(archivedMessage1);
        message.setResponseType(QMailMessage::Reply);
        message.setCustomField("present", "true");

        QVERIFY(QMailStore::instance()->addMessage(&message));

        message.setPreviousParentFolderId(message.parentFolderId());
        message.setParentFolderId(savedId2);
        QVERIFY(QMailStore::instance()->updateMessage(&message));

        savedMessage2 = message.id();
        allMessages << message.id();
        allEmailMessages << message.id();
    }
}

void tst_QMailMessageSet::cleanupTestCase()
{
    QMailStore::instance()->removeAccounts(QMailAccountKey::customField("verified"));
    QMailStore::instance()->removeFolders(QMailFolderKey::customField("uidNext"));
    QMailStore::instance()->removeMessages(QMailMessageKey::customField("present"));
}

void tst_QMailMessageSet::test_foldermessageset()
{
    QMailMessageSetModel model;
    QMailFolderMessageSet *folderset1 = new QMailFolderMessageSet(&model, inboxId1);

    QMailFolderMessageSet *child = new QMailFolderMessageSet(folderset1, savedId1);
    QMailFolderMessageSet *child2 = new QMailFolderMessageSet(folderset1, savedId2);
    QMailFolderMessageSet *child3 = new QMailFolderMessageSet(folderset1, archivedId1);
    QMailFolderMessageSet *child4 = new QMailFolderMessageSet(folderset1, archivedId2);
    model.append(child);
    model.append(child2);
    model.append(child3);
    model.append(child4);
    QCOMPARE(model.count(), 4);

    QCOMPARE(folderset1->folderId(), inboxId1);
    QCOMPARE(folderset1->hierarchical(), true);
    QCOMPARE(folderset1->displayName(), QString("Inbox"));

    QCOMPARE(folderset1->messageKey(), QMailFolderMessageSet::contentKey(inboxId1, false));
    QCOMPARE(folderset1->descendantsMessageKey(), QMailFolderMessageSet::contentKey(inboxId1, true));
}

void tst_QMailMessageSet::test_accountmessageset()
{
    QMailMessageSetModel model;
    QMailAccountMessageSet *set1 = new QMailAccountMessageSet(&model, accountId1);
    QMailAccountMessageSet *child1 = new QMailAccountMessageSet(set1, accountId2);
    QMailAccountMessageSet *child2 = new QMailAccountMessageSet(set1, accountId3);
    QMailAccountMessageSet *child3 = new QMailAccountMessageSet(set1, accountId4);

    model.append(child1);
    model.append(child2);
    model.append(child3);
    QCOMPARE(model.count(), 3);

    QCOMPARE(set1->accountId(), accountId1);
    QCOMPARE(set1->hierarchical(), true);
    QCOMPARE(set1->displayName(), QString("Account 1"));

    QCOMPARE(set1->messageKey(), QMailAccountMessageSet::contentKey(accountId1, false));
    QCOMPARE(set1->descendantsMessageKey(), QMailAccountMessageSet::contentKey(accountId1, true));
}

void tst_QMailMessageSet::test_filtermessageset()
{
    QMailMessageSetModel model;
    QMailMessageKey keyemail = QMailMessageKey::messageType(QMailMessageMetaDataFwd::Email);
    QMailMessageKey keysms = QMailMessageKey::messageType(QMailMessageMetaDataFwd::Sms);
    QString name("Filter 1");
    QMailFilterMessageSet *set1 = new QMailFilterMessageSet(&model, keyemail, name);

    QCOMPARE(set1->messageKey(), keyemail);
    QCOMPARE(set1->displayName(), name);
    QCOMPARE(set1->updatesMinimized(), true);
    set1->setUpdatesMinimized(false);
    QCOMPARE(set1->updatesMinimized(), false);

    set1->setMessageKey(keysms);
    QCOMPARE(set1->messageKey(), keysms);

    QString newname("Filter Set1");
    set1->setDisplayName(newname);
    QCOMPARE(set1->displayName(), newname);

    QCOMPARE(model.data(set1, QMailMessageSetModel::DisplayNameRole, 0).toString(), newname);
    QCOMPARE(model.data(set1, QMailMessageSetModel::MessageKeyRole, 0).value<QMailMessageKey>(), keysms);

}

void tst_QMailMessageSet::test_messagesetcontainer()
{
    QMailMessageSetModel model;
    QMailFolderMessageSet *folderset1 = new QMailFolderMessageSet(&model, inboxId1);

    QMailFolderMessageSet *child = new QMailFolderMessageSet(&model, savedId1);
    QMailFolderMessageSet *child2 = new QMailFolderMessageSet(&model, savedId2);
    QMailFolderMessageSet *child3 = new QMailFolderMessageSet(&model, archivedId1);
    QMailFolderMessageSet *child4 = new QMailFolderMessageSet(&model, archivedId2);


    model.append(folderset1);
    model.append(child);
    model.append(child2);
    model.append(child3);
    model.append(child4);

    QCOMPARE(model.count(), 5);
    QCOMPARE(model.at(0), folderset1);
    QCOMPARE(model.indexOf(folderset1), 0);

    //update


    //remove
    model.remove(child);
    QCOMPARE(model.count(), 4);
    QCOMPARE(model.indexOf(child), -1);
    QCOMPARE(model.at(0), folderset1);

    model.remove( QList<QMailMessageSet *>() << child2 << child3);
    QCOMPARE(model.count(), 2);

    model.removeDescendants();
    QCOMPARE(model.count(), 0);
}
