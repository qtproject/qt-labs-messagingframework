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

#include "detailspage_p.h"
#include <qcheckbox.h>
#include <qlineedit.h>
#include <qtextedit.h>
#include <qcombobox.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qpixmap.h>
#include <qtoolbutton.h>
#include <qmenu.h>
#include <qdesktopwidget.h>
#include <qevent.h>
#include <qpushbutton.h>
#include <QScrollArea>
#include <QTimer>
#include <qmailstore.h>
#include <qmailaccount.h>
#include <QApplication>

static const QString placeholder("(no subject)");

// static const int MaximumSmsSubjectLength = 40;
// static const int MaximumInstantSubjectLength = 256;

class FromAddressComboBox : public QComboBox
{
    Q_OBJECT
public:
    FromAddressComboBox(QWidget *parent = Q_NULLPTR);
    ~FromAddressComboBox();

signals:
    void send();
    void done();

protected:
    void keyPressEvent(QKeyEvent *);
};

FromAddressComboBox::FromAddressComboBox(QWidget *parent)
    : QComboBox(parent)
{
}

FromAddressComboBox::~FromAddressComboBox()
{
}

void FromAddressComboBox::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Back)
        emit done();
    else
        QComboBox::keyPressEvent(e);
}


DetailsPage::DetailsPage( QWidget *parent, const char *name )
    : QWidget( parent ), m_type( -1 ), m_previousAction(0)
{
/*    QMenu *menu = QSoftMenuBar::menuFor( this );
    QWidget* p = QWidget::parentWidget();
    while(p)
    {
        addActionsFromWidget(p,menu);
        p = p->parentWidget();
    }
*/
    //m_previousAction = new QAction( QIcon(":icon/edit"),tr("Edit message"),this);
    //connect( m_previousAction, SIGNAL(triggered()), this, SIGNAL(editMessage()) );
    //menu->addAction(m_previousAction);

    m_ignoreFocus = true;
    setObjectName( name );
    QIcon abicon(":icon/addressbook/AddressBook");
//    QMenu *menu = QSoftMenuBar::menuFor( this );
//  if( !Qtopia::mousePreferred() )
//  {
//      menu->addAction( abicon, tr("From contacts", "Find recipient's phone number or email address from Contacts application"),
//                       this, SLOT(editRecipients()) );
//      menu->addSeparator();
//fndef QT_NO_CLIPBOARD
//      menu->addAction( QIcon(":icon/copy"), tr("Copy"),
//                       this, SLOT(copy()) );
//     menu->addAction( QIcon(":icon/paste"), tr("Paste"),
//                      this, SLOT(paste()) );
//ndif
//    }

    const int margin = 2;
    setMaximumWidth( qApp->desktop()->width() - 2 * margin );
    QGridLayout *l = new QGridLayout( this );
    int rowCount = 0;

    m_toFieldLabel = new QLabel( this );
    m_toFieldLabel->setText( tr( "To" ) );
    m_toBox = new QHBoxLayout( );
    m_toField = new QLineEdit( this );
    setFocusProxy(m_toField);
    m_toBox->addWidget( m_toField );
    m_toFieldLabel->setBuddy(m_toField);
    connect( m_toField, SIGNAL(textChanged(QString)), this, SIGNAL(changed()) );
    l->addWidget( m_toFieldLabel, rowCount, 0 );

    //m_toPicker = ( Qtopia::mousePreferred() ? new RecipientSelectorButton(this, m_toBox, m_toField) : 0 );
    l->addLayout( m_toBox, rowCount, 2 );
    ++rowCount;

    m_ccFieldLabel = new QLabel( this );
    m_ccFieldLabel->setText( tr( "CC" ) );
    m_ccBox = new QHBoxLayout( );
    m_ccField = new QLineEdit( this );
    m_ccBox->addWidget( m_ccField );
    m_ccFieldLabel->setBuddy(m_ccField);
    connect( m_ccField, SIGNAL(textChanged(QString)), this, SIGNAL(changed()) );
    l->addWidget( m_ccFieldLabel, rowCount, 0 );

    l->addLayout( m_ccBox, rowCount, 2 );
    ++rowCount;

    m_bccFieldLabel = new QLabel( this );
    m_bccFieldLabel->setText( tr( "BCC" ) );
    m_bccBox = new QHBoxLayout( );
    m_bccField = new QLineEdit( this );
    m_bccBox->addWidget( m_bccField );
    m_bccFieldLabel->setBuddy(m_bccField);
    connect( m_bccField, SIGNAL(textChanged(QString)), this, SIGNAL(changed()) );
    l->addWidget( m_bccFieldLabel, rowCount, 0 );

    l->addLayout( m_bccBox, rowCount, 2 );
    ++rowCount;

    m_subjectFieldLabel = new QLabel( this );
    m_subjectFieldLabel->setText( tr( "Subject" ) );
    m_subjectField = new QLineEdit( this );
    m_subjectFieldLabel->setBuddy(m_subjectField);
    connect( m_subjectField, SIGNAL(textChanged(QString)), this, SIGNAL(changed()) );
    l->addWidget( m_subjectFieldLabel, rowCount, 0 );
    l->addWidget( m_subjectField, rowCount, 2 );
    ++rowCount;

    m_deliveryReportField = new QCheckBox( tr("Delivery report"), this );
    l->addWidget( m_deliveryReportField, rowCount, 0, 1, 3, Qt::AlignLeft );
    ++rowCount;

    m_readReplyField = new QCheckBox( tr("Read reply"), this );
    l->addWidget( m_readReplyField, rowCount, 0, 1, 3, Qt::AlignLeft );
    ++rowCount;

    m_fromFieldLabel = new QLabel( this );
    m_fromFieldLabel->setEnabled( true );
    m_fromFieldLabel->setText( tr( "From" ) );
    m_fromField = new FromAddressComboBox( this );
    m_fromFieldLabel->setBuddy(m_fromField);
    m_fromField->setEnabled( true );
    m_fromField->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum )); // Why not automatic?

    connect( m_fromField, SIGNAL(activated(int)), this, SIGNAL(changed()) );
    connect( m_fromField, SIGNAL(done()), this, SIGNAL(cancel()) );
    l->addWidget( m_fromFieldLabel, rowCount, 0 );
    l->addWidget( m_fromField, rowCount, 2 );
    ++rowCount;

    QPushButton *sendButton = new QPushButton( tr("Send"), this );
    connect( sendButton, SIGNAL(clicked()), this, SIGNAL(sendMessage()) );
    l->addWidget( sendButton, rowCount, 0, 1, 3 );
    ++rowCount;

    QSpacerItem* spacer1 = new QSpacerItem( 4, 20, QSizePolicy::Minimum, QSizePolicy::Expanding );
    l->addItem( spacer1, rowCount, 1 );
    ++rowCount;

    QList<QWidget*> tabOrderList;

    tabOrderList.append( m_toField );
//    if( Qtopia::mousePreferred() && m_toPicker)
//        tabOrderList.append( m_toPicker );
    tabOrderList.append( m_ccField );
    tabOrderList.append( m_bccField );
 //   if( Qtopia::mousePreferred() && m_bccPicker)
 //       tabOrderList.append( m_bccPicker );
    tabOrderList.append( m_subjectField );
    tabOrderList.append( m_fromField );

    QListIterator<QWidget*> it( tabOrderList );
    QWidget *prev = Q_NULLPTR;
    QWidget *next;
    while ( it.hasNext() ) {
        next = it.next();
        if ( prev )
            setTabOrder( prev, next );
        prev = next;
    }
}

void DetailsPage::setDefaultAccount( const QMailAccountId& defaultId )
{
    QStringList accounts;

    QMailAccountKey accountsKey(QMailAccountKey::status(QMailAccount::CanTransmit, QMailDataComparator::Includes));
    accountsKey &= QMailAccountKey::status(QMailAccount::Enabled, QMailDataComparator::Includes);
    accountsKey &= QMailAccountKey::messageType(QMailMessage::MessageType(m_type));

    QMailAccountIdList accountIds = QMailStore::instance()->queryAccounts(accountsKey);
    foreach (const QMailAccountId &id, accountIds) {
        QMailAccount account(id);
        if (id == defaultId)
            accounts.prepend(account.name());
        else
            accounts.append(account.name());
    }

    if ((m_type != QMailMessage::Email) && (m_type != QMailMessage::Instant)) {
        if (!accounts.isEmpty()) {
            // Just select the first acceptable account for this type
            m_fromField->addItem( accounts.first() );
        }

        m_fromField->hide();
        m_fromFieldLabel->hide();
        return;
    }

    bool matching(accounts.count() == m_fromField->count());
    if (matching) {
        int i = 0;
        foreach (const QString& item, accounts) {
            if (item != m_fromField->itemText(i)) {
                matching = false;
                break;
            }
        }
    }

    if (!matching) {
        m_fromField->clear();
        m_fromField->addItems( accounts );
        if (m_fromField->count() < 2) {
            m_fromField->hide();
            m_fromFieldLabel->hide();
        } else {
            m_fromField->show();
            m_fromFieldLabel->show();
        }
    }
}

void DetailsPage::editRecipients()
{
    /*
    RecipientEdit *edit = 0;
    if (Qtopia::mousePreferred()) {
//        if( sender() == m_toPicker )
//            edit = m_toField;
        else if( sender() == m_ccPicker )
            edit = m_ccField;
        else if( sender() == m_bccPicker )
            edit = m_bccField;
    } else {
        QWidget *w = focusWidget();
        if( w && w->inherits("RecipientEdit") )
            edit = static_cast<RecipientEdit *>(w);
    }
    if (edit)
        edit->editRecipients();
    else
        qWarning("DetailsPage::editRecipients: Couldn't find line edit for recipients.");
   */
}

void DetailsPage::setType( int t )
{
    //QtopiaApplication::InputMethodHint imHint = QtopiaApplication::Normal;
    if( m_type != t )
    {
        m_allowPhoneNumbers = false;
        m_allowEmails = false;
        m_type = t;
        m_ccField->hide();
                m_ccFieldLabel->hide();
        m_bccField->hide();
                m_bccFieldLabel->hide();
        m_subjectField->hide();
        m_subjectFieldLabel->hide();
        m_fromField->hide();
        m_fromFieldLabel->hide();
        m_readReplyField->hide();
        m_deliveryReportField->hide();

        if( t == QMailMessage::Mms )
        {
            m_allowPhoneNumbers = true;
            //m_allowEmails = true; //TODO reenable when address picker supports selection of multiple types
            m_ccFieldLabel->show();
            m_ccField->show();
                        m_bccFieldLabel->show();
            m_bccField->show();
                        m_subjectField->show();
            m_subjectFieldLabel->show();
            m_readReplyField->show();
            m_deliveryReportField->show();
        }
        else if( t == QMailMessage::Sms )
        {
            m_allowPhoneNumbers = true;

        }
        else if( t == QMailMessage::Email )
        {
            m_allowEmails = true;
            m_ccFieldLabel->show();
            m_ccField->show();
                        m_bccFieldLabel->show();
            m_bccField->show();
                        m_subjectField->show();
            m_subjectFieldLabel->show();
            if (m_fromField->count() >= 2) {
                m_fromField->show();
                m_fromFieldLabel->show();
            }
        }

    }

    // If this type has a preferred sender account, set that as the default
    QMailAccountKey typeKey(QMailAccountKey::messageType(QMailMessage::MessageType(m_type)));
    QMailAccountKey preferredKey(QMailAccountKey::status(QMailAccount::PreferredSender, QMailDataComparator::Includes));
    QMailAccountIdList ids(QMailStore::instance()->queryAccounts(typeKey & preferredKey));

    setDefaultAccount(ids.isEmpty() ? QMailAccountId() : ids.first());

    layout()->activate();
}

void DetailsPage::setDetails( const QMailMessage &mail )
{
    setTo(QMailAddress::toStringList(mail.to()).join(", "));
    setCc(QMailAddress::toStringList(mail.cc()).join(", "));
    setBcc(QMailAddress::toStringList(mail.bcc()).join(", "));

    if ((mail.subject() != placeholder) &&
        (m_type != QMailMessage::Instant) &&
        !((m_type == QMailMessage::Sms) && (mail.contentType().content().toLower() == "text/x-vcard"))) {
        setSubject(mail.subject());
    }

    if (mail.parentAccountId().isValid()) {
        setFrom(mail.parentAccountId());
    } else {
        setFrom(mail.from().address());
    }

    if (mail.headerFieldText("X-Mms-Delivery-Report") == "Yes") {
        m_deliveryReportField->setChecked(true);
    }
    if (mail.headerFieldText("X-Mms-Read-Reply") == "Yes") {
        m_readReplyField->setChecked(true);
    }
}

void DetailsPage::getDetails( QMailMessage &mail ) const
{
    if (m_type == QMailMessage::Sms) {
        // For the time being limit sending SMS messages so that they can
        // only be sent to phone numbers and not email addresses
        QString number = to();
        QString n;
        QStringList numbers;
        bool mightBeNumber = true;
        for ( int posn = 0, len = number.length(); posn < len; ++posn ) {
            uint ch = number[posn].unicode();
            // If we know this isn't a number...
            if (!mightBeNumber) {
                // If this is the end of this token, next one might be a number
                if (ch == ' ' || ch == ',') {
                    mightBeNumber = true;
                }
                continue;
            }
            if ( ch >= '0' && ch <= '9' ) {
                n += QChar(ch);
            } else if ( ch == '+' || ch == '#' || ch == '*' ) {
                n += QChar(ch);
            } else if ( ch == '-' || ch == '(' || ch == ')' ) {
                n += QChar(ch);
            } else if ( ch == ' ' ) {
                n += QChar(ch);
            } else if ( ch == ',' || ch == '<' || ch == '>' ) {
                if (!n.isEmpty())
                    numbers.append( n );
                n = "";
            } else {
                // this token is definitely not a valid number
                mightBeNumber = false;
                n = "";
            }
        }
        if (!n.isEmpty())
            numbers.append( n );
        mail.setTo(QMailAddress::fromStringList(numbers));
    } else {
        mail.setTo(QMailAddress::fromStringList(to()));
    }

    mail.setCc(QMailAddress::fromStringList(cc()));
    mail.setBcc(QMailAddress::fromStringList(bcc()));

    QString subjectText = subject();

    if (!subjectText.isEmpty())
        mail.setSubject(subjectText);
    else
        subjectText = placeholder;

    QMailAccount account = fromAccount();
    if (account.id().isValid()) {
        mail.setParentAccountId(account.id());
        mail.setFrom(account.fromAddress());
    }

    if (m_type == QMailMessage::Mms) {
        if (m_deliveryReportField->isChecked())
            mail.setHeaderField("X-Mms-Delivery-Report", "Yes");
        if (m_readReplyField->isChecked())
            mail.setHeaderField("X-Mms-Read-Reply", "Yes");
    }
}

bool DetailsPage::isDetailsOnlyMode() const
{
    return !m_previousAction->isVisible();
}

void DetailsPage::setDetailsOnlyMode(bool val)
{
    m_previousAction->setVisible(!val);
}

void DetailsPage::setBcc( const QString &a_bcc )
{
    m_bccField->setText( a_bcc );
}

QString DetailsPage::bcc() const
{
    QString text;
    if( !m_bccField->isHidden() )
        text = m_bccField->text();
    return text;
}


void DetailsPage::setCc( const QString &a_cc )
{
    m_ccField->setText( a_cc );
}

QString DetailsPage::cc() const
{
    QString text;
    if( !m_ccField->isHidden() )
        text = m_ccField->text();
    return text;
}

void DetailsPage::setTo( const QString &a_to )
{
    m_toField->setText( a_to );
}

QString DetailsPage::to() const
{
    return m_toField->text();
}

QString DetailsPage::subject() const
{
    return m_subjectField->text();
}

void DetailsPage::setSubject( const QString &sub )
{
    m_subjectField->setText( sub );
}

QString DetailsPage::from() const
{
    QMailAccount account(fromAccount());
    if (account.id().isValid())
        return account.fromAddress().toString();
    return QString();
}

/*  Find the account matching the email address */
QMailAccount DetailsPage::fromAccount() const
{
    QMailAccountKey nameKey(QMailAccountKey::name(m_fromField->currentText()));

    QMailAccountKey statusKey(QMailAccountKey::status(QMailAccount::CanTransmit, QMailDataComparator::Includes));
    statusKey &= QMailAccountKey::status(QMailAccount::Enabled, QMailDataComparator::Includes);

    QMailAccountIdList accountIds = QMailStore::instance()->queryAccounts(nameKey & statusKey);
    if (!accountIds.isEmpty())
        return QMailAccount(accountIds.first());

    return QMailAccount();
}

void DetailsPage::setFrom( const QString &from )
{
    // Find the account matching this address
    QMailAccountKey addressKey(QMailAccountKey::fromAddress(from));
    QMailAccountIdList accountIds = QMailStore::instance()->queryAccounts(addressKey);

    if (!accountIds.isEmpty()) {
        setFrom(accountIds.first());
    }
}

void DetailsPage::setFrom( const QMailAccountId &accountId )
{
    QMailAccount account(accountId);

    // Find the entry matching this account
    for (int i = 0; i < static_cast<int>(m_fromField->count()); ++i) {
        if (m_fromField->itemText(i) == account.name()) {
            m_fromField->setCurrentIndex(i);
            break;
        }
    }
}

void DetailsPage::copy()
{
#ifndef QT_NO_CLIPBOARD 
    QWidget *fw = focusWidget();
    if( !fw )
        return;
    if( fw->inherits( "QLineEdit" ) )
        static_cast<QLineEdit*>(fw)->copy();
    else if( fw->inherits( "QTextEdit" ) )
        static_cast<QTextEdit*>(fw)->copy();
#endif
}

void DetailsPage::paste()
{
#ifndef QT_NO_CLIPBOARD 
    QWidget *fw = focusWidget();
    if( !fw )
        return;
    if( fw->inherits( "QLineEdit" ) )
        static_cast<QLineEdit*>(fw)->paste();
    else if( fw->inherits( "QTextEdit" ))
        static_cast<QTextEdit*>(fw)->paste();
#endif
}

void DetailsPage::clear()
{
    m_toField->clear();
    m_ccField->clear();
    m_bccField->clear();
    m_subjectField->clear();
    m_readReplyField->setChecked( false );
    // don't clear from fields
}

#include <detailspage.moc>
