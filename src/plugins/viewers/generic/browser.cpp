/****************************************************************************
**
** This file is part of the $PACKAGE_NAME$.
**
** Copyright (C) $THISYEAR$ $COMPANY_NAME$.
**
** $QT_EXTENDED_DUAL_LICENSE$
**
****************************************************************************/

#include "browser.h"
#include <QImageReader>
#include <QKeyEvent>
#include <QStyle>
#include <QDebug>
#include <QApplication>
#include <qmailaddress.h>
#include <qmailmessage.h>
#include <qmailtimestamp.h>
#include <limits.h>

static QColor replyColor(Qt::darkGreen);

static QString dateString(const QDateTime& dt)
{
    QDateTime current = QDateTime::currentDateTime();
    if (dt.date() == current.date()) {
        //today
        return QString(qApp->translate("Browser", "Today %1")).arg(dt.toString("h:mm:ss ap"));
    } else if (dt.daysTo(current) == 1) {
        //yesterday
        return QString(qApp->translate("Browser", "Yesterday %1")).arg(dt.toString("h:mm:ss ap"));
    } else if (dt.daysTo(current) < 7) {
        //within 7 days
        return dt.toString("dddd h:mm:ss ap");
    } else {
        return dt.toString("dd/MM/yy h:mm:ss ap");
    }
}

static uint qHash(const QUrl &url)
{
    return qHash(url.toString());
}

Browser::Browser( QWidget *parent  )
    : QTextBrowser( parent ),
      replySplitter( &Browser::handleReplies )
{
    setFrameStyle( NoFrame );
    setFocusPolicy( Qt::StrongFocus );
}

Browser::~Browser()
{
}

void Browser::scrollBy(int dx, int dy)
{
    scrollContentsBy( dx, dy );
}

void Browser::setResource( const QUrl& name, QVariant var )
{
    if (!resourceMap.contains(name)) {
        resourceMap.insert(name, var);
    }
}

void Browser::clearResources()
{
    resourceMap.clear();
    numbers.clear();
}

QVariant Browser::loadResource(int type, const QUrl& name)
{
    if (resourceMap.contains(name))
        return resourceMap[name];

    return QTextBrowser::loadResource(type, name);
}

QList<QString> Browser::embeddedNumbers() const
{
    QList<QString> result;
    return result;
}

void Browser::setTextResource(const QSet<QUrl>& names, const QString& textData)
{
    QVariant data(textData);
    foreach (const QUrl &url, names) {
        setResource(url, data);
    }
}

void Browser::setImageResource(const QSet<QUrl>& names, const QByteArray& imageData)
{
    // Create a image from the data
    QDataStream imageStream(&const_cast<QByteArray&>(imageData), QIODevice::ReadOnly);
    QImageReader imageReader(imageStream.device());

    // Max size should be bounded by our display window, which will possibly
    // have a vertical scrollbar (and a small fudge factor...)
    int maxWidth = (width() - style()->pixelMetric(QStyle::PM_ScrollBarExtent) - 4);

    QSize imageSize;
    if (imageReader.supportsOption(QImageIOHandler::Size)) {
        imageSize = imageReader.size();

        // See if the image needs to be down-scaled during load
        if (imageSize.width() > maxWidth)
        {
            // And the loaded size should maintain the image aspect ratio
            imageSize.scale(maxWidth, (INT_MAX >> 4), Qt::KeepAspectRatio);
            imageReader.setQuality( 49 ); // Otherwise Qt smooth scales
            imageReader.setScaledSize(imageSize);
        }
    }

    QImage image = imageReader.read();

    if (!imageReader.supportsOption(QImageIOHandler::Size)) {
        // We need to scale it down now
        if (image.width() > maxWidth)
            image = image.scaled(maxWidth, INT_MAX, Qt::KeepAspectRatio);
    }

    QVariant data(image);
    foreach (const QUrl &url, names) {
        setResource(url, data);
    }
}

void Browser::setPartResource(const QMailMessagePart& part)
{
    QSet<QUrl> names;

    QString name(part.displayName());
    if (!name.isEmpty()) {
        names.insert(QUrl(Qt::escape(name)));
    }

    name = part.contentID();
    if (!name.isEmpty()) {
        names.insert(QUrl("cid:" + name));
        names.insert(QUrl("CID:" + name));
    }

    name = part.contentType().name();
    if (!name.isEmpty()) {
        names.insert(QUrl(Qt::escape(name)));
    }

    if (part.contentType().type().toLower() == "text") {
        setTextResource(names, part.body().data());
    } else if (part.contentType().type().toLower() == "image") {
        setImageResource(names, part.body().data(QMailMessageBody::Decoded));
    }
}

void Browser::setSource(const QUrl &name)
{
    Q_UNUSED(name)
// We deal with this ourselves.
//    QTextBrowser::setSource( name );
}

void Browser::setMessage(const QMailMessage& email, bool plainTextMode)
{
    if (plainTextMode) {
        // Complete MMS messages must be displayed in HTML
        if (email.messageType() == QMailMessage::Mms) {
            QString mmsType = email.headerFieldText("X-Mms-Message-Type");
            if (mmsType.contains("m-retrieve-conf") || mmsType.contains("m-send-req"))
                plainTextMode = false;
        }
    }

    // Maintain original linelengths if display width allows it
    if (email.messageType() == QMailMessage::Sms) {
        replySplitter = &Browser::smsBreakReplies;
    } else {
        uint lineCharLength;
        if ( fontInfo().pointSize() >= 10 ) {
            lineCharLength = width() / (fontInfo().pointSize() - 4 );
        } else {
            lineCharLength = width() / (fontInfo().pointSize() - 3 );
        }

        // Determine how to split lines in text
        if ( lineCharLength >= 78 )
            replySplitter = &Browser::noBreakReplies;
        else
            replySplitter = &Browser::handleReplies;
    }

    if (plainTextMode)
        displayPlainText(&email);
    else
        displayHtml(&email);
}

void Browser::displayPlainText(const QMailMessage* mail)
{
    QString bodyText;

    if ( (mail->status() & QMailMessage::Incoming) && 
        !(mail->status() & QMailMessage::PartialContentAvailable) ) {
        if ( !(mail->status() & QMailMessage::Removed) ) {
            bodyText += "\n" + tr("Awaiting download") + "\n";
            bodyText += tr("Size of message") + ": " + describeMailSize(mail->size());
        } else {
            // TODO: what?
        }
    } else {
        if (mail->hasBody()) {
            bodyText += mail->body().data();
        } else {
            if ( mail->multipartType() == QMailMessagePartContainer::MultipartAlternative ) {
                const QMailMessagePart* bestPart = 0;

                // Find the best alternative for text rendering
                for ( uint i = 0; i < mail->partCount(); i++ ) {
                    const QMailMessagePart &part = mail->partAt( i );

                    // TODO: A good implementation would be able to extract the plain text parts
                    // from text/html and text/enriched...

                    if (part.contentType().type().toLower() == "text") {
                        if (part.contentType().subType().toLower() == "plain") {
                            // This is the best part for us
                            bestPart = &part;
                            break;
                        }
                        else if (part.contentType().subType().toLower() == "html") {
                            // This is the worst, but still acceptable, text part for us
                            if (bestPart == 0)
                                bestPart = &part;
                        }
                        else  {
                            // Some other text - better than html, probably
                            if ((bestPart != 0) && (bestPart->contentType().subType().toLower() == "html"))
                                bestPart = &part;
                        }
                    }
                }

                if (bestPart != 0)
                    bodyText += bestPart->body().data() + "\n";
                else
                    bodyText += "\n<" + tr("Message part is not displayable") + ">\n";
            }
            else if ( mail->multipartType() == QMailMessagePartContainer::MultipartRelated ) {
                const QMailMessagePart* startPart = &mail->partAt(0);

                // If not specified, the first part is the start
                QByteArray startCID = mail->contentType().parameter("start");
                if (!startCID.isEmpty()) {
                    for ( uint i = 1; i < mail->partCount(); i++ ) 
                        if (mail->partAt(i).contentID() == startCID) {
                            startPart = &mail->partAt(i);
                            break;
                        }
                }

                // Render the start part, if possible
                if (startPart->contentType().type().toLower() == "text")
                    bodyText += startPart->body().data() + "\n";
                else
                    bodyText += "\n<" + tr("Message part is not displayable") + ">\n";
            }
            else {
                // According to RFC 2046, any unrecognised type should be treated as 'mixed'
                if (mail->multipartType() != QMailMessagePartContainer::MultipartMixed)
                    qWarning() << "Generic viewer: Unimplemented multipart type:" << mail->contentType().toString();

                // Render each succesive part to text, where possible
                for ( uint i = 0; i < mail->partCount(); i++ ) {
                    const QMailMessagePart &part = mail->partAt( i );

                    if (part.hasBody() && (part.contentType().type().toLower() == "text")) {
                        bodyText += part.body().data() + "\n";
                    } else {
                        bodyText += "\n<" + tr("Part") + ": " + part.displayName() + ">\n";
                    }
                }
            }
        }
    }

    QString text;

    if ((mail->messageType() != QMailMessage::Sms) && (mail->messageType() != QMailMessage::Instant))
        text += tr("Subject") + ": " + mail->subject() + "\n";

    QMailAddress fromAddress(mail->from());
    if (!fromAddress.isNull())
        text += tr("From") + ": " + fromAddress.toString() + "\n";

    if (mail->to().count() > 0) {
        text += tr("To") + ": ";
        text += QMailAddress::toStringList(mail->to()).join(", ");
    }
    if (mail->cc().count() > 0) {
        text += "\n" + tr("CC") + ": ";
        text += QMailAddress::toStringList(mail->cc()).join(", ");
    }
    if (mail->bcc().count() > 0) {
        text += "\n" + tr("BCC") + ": ";
        text += QMailAddress::toStringList(mail->bcc()).join(", ");
    }
    if ( !mail->replyTo().isNull() ) {
        text += "\n" + tr("Reply-To") + ": ";
        text += mail->replyTo().toString();
    }

    text += "\n" + tr("Date") + ": ";
    text += dateString(mail->date().toLocalTime()) + "\n";

    if (mail->status() & QMailMessage::Removed) {
        if (!bodyText.isEmpty()) {
            // Don't include the notice - the most likely reason to view plain text
            // is for printing, and we don't want to print the notice
        } else {
            text += "\n";
            text += tr("Message deleted from server");
        }
    }

    if (!bodyText.isEmpty()) {
        text += "\n";
        text += bodyText;
    }

    setPlainText(text);
}

static QString replaceLast(const QString container, const QString& before, const QString& after)
{
    QString result(container);

    int index;
    if ((index = container.lastIndexOf(before)) != -1)
        result.replace(index, before.length(), after);

    return result;
}

QString Browser::renderSimplePart(const QMailMessagePart& part)
{
    QString result;

    QString partId = Qt::escape(part.displayName());

    QMailMessageContentType contentType = part.contentType();
    if ( contentType.type().toLower() == "text") { // No tr
        if (part.hasBody()) {
            QString partText = part.body().data();
            if ( !partText.isEmpty() ) {
                if ( contentType.subType().toLower() == "html" ) {
                    result = partText + "<br>";
                } else {
                    result = formatText( partText );
                }
            }
        } else {
            result = renderAttachment(part);
        }
    } else if ( contentType.type().toLower() == "image") { // No tr
        setPartResource(part);
        result = "<img src=\"" + partId + "\"></img>";
    } else {
        result = renderAttachment(part);
    }

    return result;
}

QString Browser::renderAttachment(const QMailMessagePart& part)
{
    QString partId = Qt::escape(part.displayName());

    QString attachmentTemplate = 
"<hr><b>ATTACHMENT_TEXT</b>: <a href=\"attachment;ATTACHMENT_ACTION;ATTACHMENT_LOCATION\">NAME_TEXT</a>DISPOSITION<br>";

    attachmentTemplate = replaceLast(attachmentTemplate, "ATTACHMENT_TEXT", tr("Attachment"));
    attachmentTemplate = replaceLast(attachmentTemplate, "ATTACHMENT_ACTION", part.partialContentAvailable() ? "view" : "retrieve");
    attachmentTemplate = replaceLast(attachmentTemplate, "ATTACHMENT_LOCATION", part.location().toString(false));
    attachmentTemplate = replaceLast(attachmentTemplate, "NAME_TEXT", partId);
    return replaceLast(attachmentTemplate, "DISPOSITION", part.partialContentAvailable() ? "" : tr(" (on server)"));
}

QString Browser::renderPart(const QMailMessagePart& part)
{
    QString result;

    if (part.multipartType() != QMailMessage::MultipartNone) {
        result = renderMultipart(part);
    } else {
        bool displayAsAttachment(!part.contentAvailable());
        if (!displayAsAttachment) {
            QMailMessageContentDisposition disposition = part.contentDisposition();
            if (!disposition.isNull() && disposition.type() == QMailMessageContentDisposition::Attachment) {
                displayAsAttachment = true;
            }
        }

        result = (displayAsAttachment ? renderAttachment(part) : renderSimplePart(part));
    }

    return result;
}

QString Browser::renderMultipart(const QMailMessagePartContainer& partContainer)
{
    QString result;

    if (partContainer.multipartType() == QMailMessagePartContainer::MultipartAlternative) {
        int partIndex = -1;

        // Find the best alternative for rendering
        for (uint i = 0; i < partContainer.partCount(); ++i) {
            const QMailMessagePart &part = partContainer.partAt(i);

            // Parts are ordered simplest to most complex
            QString type(part.contentType().type().toLower());
            if ((type == "text") || (type == "image")) {
                // These parts are probably displayable
                partIndex = i;
            }
        }

        if (partIndex != -1) {
            result += renderPart(partContainer.partAt(partIndex));
        } else {
            result += "\n<" + tr("No displayable part") + ">\n";
        }
    } else if (partContainer.multipartType() == QMailMessagePartContainer::MultipartRelated) {
        uint startIndex = 0;

        // If not specified, the first part is the start
        QByteArray startCID = partContainer.contentType().parameter("start");
        if (!startCID.isEmpty()) {
            for (uint i = 1; i < partContainer.partCount(); ++i) {
                if (partContainer.partAt(i).contentID() == startCID) {
                    startIndex = i;
                    break;
                }
            }
        }

        // Add any other parts as resources
        QList<const QMailMessagePart*> absentParts;
        for (uint i = 0; i < partContainer.partCount(); ++i) {
            if (i != startIndex) {
                const QMailMessagePart &part = partContainer.partAt(i);
                if (part.partialContentAvailable()) {
                    setPartResource(partContainer.partAt(i));
                } else {
                    absentParts.append(&part);
                }
            }
        }

        // Render the start part
        result += renderPart(partContainer.partAt(startIndex));

        // Show any unavailable parts as attachments
        foreach (const QMailMessagePart *part, absentParts) {
            result += renderAttachment(*part);
        }
    } else {
        // According to RFC 2046, any unrecognised type should be treated as 'mixed'
        if (partContainer.multipartType() != QMailMessagePartContainer::MultipartMixed)
            qWarning() << "Unimplemented multipart type:" << partContainer.contentType().toString();

        // Render each part successively according to its disposition
        for (uint i = 0; i < partContainer.partCount(); ++i) {
            result += renderPart(partContainer.partAt(i));
        }
    }

    return result;
}

typedef QPair<QString, QString> TextPair;

void Browser::displayHtml(const QMailMessage* mail)
{
    QString subjectText, bodyText;
    QList<TextPair> metadata;

    // For SMS messages subject is the same as body, so for SMS don't 
    // show the message text twice (same for IMs)
    if ((mail->messageType() != QMailMessage::Sms) && (mail->messageType() != QMailMessage::Instant))
        subjectText = mail->subject();

    QString from = mail->headerFieldText("From");
    if (!from.isEmpty() && from != "\"\" <>") // ugh
        metadata.append(qMakePair(tr("From"), refMailTo( mail->from() )));

    if (mail->to().count() > 0) 
        metadata.append(qMakePair(tr("To"), listRefMailTo( mail->to() )));

    if (mail->cc().count() > 0) 
        metadata.append(qMakePair(tr("CC"), listRefMailTo( mail->cc() )));

    if (mail->bcc().count() > 0) 
        metadata.append(qMakePair(tr("BCC"), listRefMailTo( mail->bcc() )));

    if (!mail->replyTo().isNull())
        metadata.append(qMakePair(tr("Reply-To"), refMailTo( mail->replyTo() )));

    metadata.append(qMakePair(tr("Date"), dateString(mail->date().toLocalTime())));

    if ( (mail->status() & QMailMessage::Incoming) && 
        !(mail->status() & QMailMessage::PartialContentAvailable) ) {
        if ( !(mail->status() & QMailMessage::Removed) ) {
            bodyText = 
"<b>WAITING_TEXT</b><br>"
"SIZE_TEXT<br>"
"<br>"
"<a href=\"download\">DOWNLOAD_TEXT</a>";

            bodyText = replaceLast(bodyText, "WAITING_TEXT", tr("Awaiting download"));
            bodyText = replaceLast(bodyText, "SIZE_TEXT", tr("Size of message content") + ": " + describeMailSize(mail->contentSize()));
            bodyText = replaceLast(bodyText, "DOWNLOAD_TEXT", tr("Download this message"));
        } else {
            // TODO: what?
        }
    } else {
        if (mail->partCount() > 0) {
            bodyText = renderMultipart(*mail);
        } else if (mail->messageType() == QMailMessage::System) {
            // Assume this is appropriately formatted for display
            bodyText = mail->body().data();
        } else {
            bodyText = formatText( mail->body().data() );

            if (!mail->contentAvailable()) {
                QString trailer =
"<br>"
"WAITING_TEXT<br>"
"SIZE_TEXT<br>"
"<a href=\"DOWNLOAD_ACTION\">DOWNLOAD_TEXT</a>";

                trailer = replaceLast(trailer, "WAITING_TEXT", tr("More data available"));
                trailer = replaceLast(trailer, "SIZE_TEXT", tr("Size") + ": " + describeMailSize(mail->body().length()) + tr(" of ") + describeMailSize(mail->contentSize()));
                if ((mail->contentType().type().toLower() == "text") && (mail->contentType().subType().toLower() == "plain")) {
                    trailer = replaceLast(trailer, "DOWNLOAD_ACTION", "download;5120");
                } else {
                    trailer = replaceLast(trailer, "DOWNLOAD_ACTION", "download");
                }
                trailer = replaceLast(trailer, "DOWNLOAD_TEXT", tr("Retrieve more data"));

                bodyText += trailer;
            }
        }
    }

    // Form our parts into a displayable page
    QString pageData;

    if (mail->status() & QMailMessage::Removed) {
        QString noticeTemplate =
"<div align=center>"
    "NOTICE_TEXT<br>"
"</div>";

        QString notice = tr("Message deleted from server");
        if (!bodyText.isEmpty()) {
            notice.prepend("<font size=\"-5\">[");
            notice.append("]</font>");
        }

        pageData += replaceLast(noticeTemplate, "NOTICE_TEXT", notice);
    }

    QString headerTemplate = \
"<div align=left>"
    "<table border=0 cellspacing=0 cellpadding=0 width=100\%>"
        "<tr>"
            "<td bgcolor=\"#000000\">"
                "<table border=0 width=100\% cellspacing=1 cellpadding=4>"
                    "<tr>"
                        "<td align=left bgcolor=\"HIGHLIGHT_COLOR\">"
                            "<b><font color=\"LINK_COLOR\">SUBJECT_TEXT</font></b>"
                        "</td>"
                    "</tr>"
                    "<tr>"
                        "<td bgcolor=\"WINDOW_COLOR\">"
                            "<table border=0>"
                                "METADATA_TEXT"
                            "</table>"
                        "</td>"
                    "</tr>"
                "</table>"
            "</td>"
        "</tr>"
    "</div>"
"<br>";

    headerTemplate = replaceLast(headerTemplate, "HIGHLIGHT_COLOR", palette().color(QPalette::Highlight).name());
    headerTemplate = replaceLast(headerTemplate, "LINK_COLOR", palette().color(QPalette::HighlightedText).name());
    headerTemplate = replaceLast(headerTemplate, "SUBJECT_TEXT", Qt::escape(subjectText));
    headerTemplate = replaceLast(headerTemplate, "WINDOW_COLOR", palette().color(QPalette::Window).name());

    QString itemTemplate =
"<tr>"
    "<td align=right>"
        "<b>ID_TEXT: </b>"
    "</td>"
    "<td width=50\%>"
        "CONTENT_TEXT"
    "</td>"
"</tr>";

    QString metadataText;
    foreach (const TextPair item, metadata) {
        QString element = replaceLast(itemTemplate, "ID_TEXT", Qt::escape(item.first));
        element = replaceLast(element, "CONTENT_TEXT", item.second);
        metadataText.append(element);
    }

    pageData += replaceLast(headerTemplate, "METADATA_TEXT", metadataText);

    QString bodyTemplate = 
 "<div align=left>BODY_TEXT</div>";

    pageData += replaceLast(bodyTemplate, "BODY_TEXT", bodyText);

    QString pageTemplate =
"<table width=100\% height=100\% border=0 cellspacing=8 cellpadding=0>"
    "<tr>"
        "<td>"
            "PAGE_DATA"
        "</td>"
    "</tr>"
"</table>";

    setHtml(replaceLast(pageTemplate, "PAGE_DATA", pageData));
}

QString Browser::describeMailSize(uint bytes) const
{
    QString size;

    // No translation?
    if (bytes < 1024) {
        size.setNum(bytes);
        size += " Bytes";
    } else if (bytes < 1024*1024) {
        size.setNum( (bytes / 1024) );
        size += " KB";
    } else {
        float f = static_cast<float>( bytes )/ (1024*1024);
        size.setNum(f, 'g', 3);
        size += " MB";
    }

    return size;
}

QString Browser::formatText(const QString& txt) const
{
    return (*this.*replySplitter)(txt);
}

QString Browser::smsBreakReplies(const QString& txt) const
{
    /*  Preserve white space, add linebreaks so text is wrapped to
        fit the display width */
    QString str = "";
    QStringList p = txt.split("\n");

    QStringList::Iterator it = p.begin();
    while ( it != p.end() ) {
        str += buildParagraph( *it, "", true ) + "<br>";
        it++;
    }

    return str;
}

QString Browser::noBreakReplies(const QString& txt) const
{
    /*  Maintains the original linebreaks, but colours the lines
        according to reply level    */
    QString str = "";
    QStringList p = txt.split("\n");

    int x, levelList;
    QStringList::Iterator it = p.begin();
    while ( it != p.end() ) {

        x = 0;
        levelList = 0;
        while (x < (*it).length() ) {
            if ( (*it)[x] == '>' ) {
                levelList++;
            } else if ( (*it)[x] == ' ' ) {
            } else break;

            x++;
        }

        if (levelList == 0 ) {
            str += encodeUrlAndMail(*it) + "<br>";
        } else {
            const QString para("<font color=\"%1\">%2</font><br>");
            str += para.arg(replyColor.name()).arg(encodeUrlAndMail(*it));
        }

        it++;
    }

    return str;
}

QString appendLine(const QString& preceding, const QString& suffix)
{
    if (suffix.isEmpty())
        return preceding;

    QString result(preceding);

    int nwsIndex = QRegExp("[^\\s]").indexIn(suffix);
    if (nwsIndex > 0) {
        // This line starts with whitespace, which we'll have to protect to keep

        // We can't afford to make huge tracts of whitespace; ASCII art will be broken!
        // Convert any run of up to 4 spaces to a tab; convert all tabs to two spaces each
        QString leader(suffix.left(nwsIndex));
        leader.replace(QRegExp(" {1,4}"), "\t");

        // Convert the spaces to non-breaking
        leader.replace("\t", "&nbsp;&nbsp;");
        result.append(leader).append(suffix.mid(nwsIndex));
    }
    else
        result.append(suffix);

    return result;
}

QString unwrap(const QString& txt, const QString& prepend)
{
    QStringList lines = txt.split("\n", QString::KeepEmptyParts);

    QString result;
    result.reserve(txt.length());

    QStringList::iterator it = lines.begin(), prev = it, end = lines.end();
    if (it != end) {
        for (++it; it != end; ++prev, ++it) {
            QString terminator = "<br>";

            int prevLength = (*prev).length();
            if (prevLength == 0) {
                // If the very first line is empty, skip it; otherwise include
                if (prev == lines.begin())
                    continue;
            } else {
                int wsIndex = (*it).indexOf(QRegExp("\\s"));
                if (wsIndex == 0) {
                    // This was probably an intentional newline
                } else {
                    if (wsIndex == -1)
                        wsIndex = (*it).length();

                    bool logicalEnd = false;

                    const QChar last = (*prev)[prevLength - 1];
                    logicalEnd = ((last == '.') || (last == '!') || (last == '?'));

                    if ((*it)[0].isUpper() && logicalEnd) {
                        // This was probably an intentional newline
                    } else {
                        int totalLength = prevLength + prepend.length();
                        if ((wsIndex != -1) && ((totalLength + wsIndex) > 78)) {
                            // This was probably a forced newline - convert it to a space
                            terminator = " ";
                        }
                    }
                }
            }

            result = appendLine(result, Browser::encodeUrlAndMail(*prev) + terminator);
        }
        if (!(*prev).isEmpty())
            result = appendLine(result, Browser::encodeUrlAndMail(*prev));
    }

    return result;
}

/*  This one is a bit complicated.  It divides up all lines according
    to their reply level, defined as count of ">" before normal text
    It then strips them from the text, builds the formatted paragraph
    and inserts them back into the beginning of each line.  Probably not
    too speed efficient on large texts, but this manipulation greatly increases
    the readability (trust me, I'm using this program for my daily email reading..)
*/
QString Browser::handleReplies(const QString& txt) const
{
    QStringList out;
    QStringList p = txt.split("\n");
    QList<uint> levelList;
    QStringList::Iterator it = p.begin();
    uint lastLevel = 0, level = 0;

    // Skip the last string, if it's non-existent
    int offset = (txt.endsWith("\n") ? 1 : 0);

    QString str, line;
    while ( (it + offset) != p.end() ) {
        line = (*it);
        level = 0;

        if ( line.startsWith(">") ) {
            for (int x = 0; x < line.length(); x++) {
                if ( line[x] == ' ') {  
                    // do nothing
                } else if ( line[x] == '>' ) {
                    level++;
                    if ( (level > 1 ) && (line[x-1] != ' ') ) {
                        line.insert(x, ' ');    //we need it to be "> > " etc..
                        x++;
                    }
                } else {
                    // make sure it follows style "> > This is easier to format"
                    if ( line[x - 1] != ' ' )
                        line.insert(x, ' ');
                    break;
                }
            }
        }

        if ( level != lastLevel ) {
            if ( !str.isEmpty() ) {
                out.append( str );
                levelList.append( lastLevel );
            }

            str.clear();
            lastLevel = level;
            it--;
        } else {
            str += line.mid(level * 2) + "\n";
        }

        it++;
    }
    if ( !str.isEmpty() ) {
        out.append( str );
        levelList.append( level );
    }

    str = "";
    lastLevel = 0;
    int pos = 0;
    it = out.begin();
    while ( it != out.end() ) {
        if ( levelList[pos] == 0 ) {
            str += unwrap( *it, "" ) + "<br>";
        } else {
            QString pre = "";
            QString preString = "";
            for ( uint x = 0; x < levelList[pos]; x++) {
                pre += "&gt; ";
                preString += "> ";
            }

            QString segment = unwrap( *it, preString );

            const QString para("<font color=\"%1\">%2</font><br>");
            str += para.arg(replyColor.name()).arg(pre + segment);
        }

        lastLevel = levelList[pos];
        pos++;
        it++;
    }

    if ( str.endsWith("<br>") ) {
        str.chop(4);   //remove trailing br
    }

    return str;
}

QString Browser::buildParagraph(const QString& txt, const QString& prepend, bool preserveWs) const
{
    Q_UNUSED(prepend);
    QStringList out;

    QString input = encodeUrlAndMail( preserveWs ? txt : txt.simplified() );
    if (preserveWs)
        return input.replace("\n", "<br>");

    QStringList p = input.split( " ", QString::SkipEmptyParts );
    return p.join(" ");
}

QString Browser::encodeUrlAndMail(const QString& txt)
{
    QStringList result;

    // TODO: is this necessary?
    // Find and encode URLs that aren't already inside anchors
    //QRegExp anchorPattern("<\\s*a\\s*href.*/\\s*a\\s*>");
    //anchorPattern.setMinimal(true);

    // We should be optimistic in our URL matching - the link resolution can
    // always fail, but if we don't match it, then we can't make it into a link
    QRegExp urlPattern("((?:http|https|ftp)://)?"               // optional scheme
                       "((?:[^:]+:)?[^@]+@)?"                   // optional credentials
                       "("                                      // either:
                            "localhost"                             // 'localhost'
                       "|"                                      // or:
                            "(?:"                                   // one-or-more: 
                            "[A-Za-z\\d]"                           // one: legal char, 
                            "(?:"                                   // zero-or-one:
                                "[A-Za-z\\d-]*[A-Za-z\\d]"              // (zero-or-more: (legal char or '-'), one: legal char)
                            ")?"                                    // end of optional group
                            "\\."                                   // '.'
                            ")+"                                    // end of mandatory group
                            "[A-Za-z\\d]"                           // one: legal char
                            "(?:"                                   // zero-or-one:
                                "[A-Za-z\\d-]*[A-Za-z\\d]"              // (zero-or-more: (legal char or '-'), one: legal char)
                            ")?"                                    // end of optional group
                       ")"                                      // end of alternation
                       "(:\\d+)?"                               // optional port number
                       "("                                      // optional trailer
                            "/"                                 // beginning with a slash
                            "[A-Za-z\\d\\.\\!\\#\\$\\%\\'"      // containing any sequence of legal chars
                             "\\*\\+\\-\\/\\=\\?\\^\\_\\`"
                             "\\{\\|\\}\\~\\&\\(\\)]*"       
                       ")?");

    QRegExp filePattern("(file://\\S+)");

    // Find and encode email addresses
    QRegExp addressPattern(QMailAddress::emailAddressPattern());

    int lastUrlPos = 0;
    while (lastUrlPos != -1) {
        QString url;
        QString scheme;
        QString trailing;
        QString nonurl;

        int nextUrlPos = -1;
        int urlPos = urlPattern.indexIn(txt, lastUrlPos);
        if (urlPos != -1) {
            // Is this a valid URL?
            url = urlPattern.cap(0);
            scheme = urlPattern.cap(1);

            nextUrlPos = urlPos + url.length();

            // Ensure that the host is not purely a number
            QString host(urlPattern.cap(3));
            if (scheme.isEmpty() && (host.indexOf(QRegExp("[^\\d\\.]")) == -1)) {
                // Ignore this match
                nonurl = txt.mid(lastUrlPos, (nextUrlPos - lastUrlPos));
                url = QString();
            } else {
                QString trailer(urlPattern.cap(5));
                if (trailer.endsWith(')')) {
                    // Check for unbalanced parentheses
                    int left = trailer.count('(');
                    int right = trailer.count(')');

                    if (right > left) {
                        // The last parentheses are probably not part of the URL
                        url = url.left(url.length() - 1);
                        trailing = ')';
                    }
                }

                nonurl = txt.mid(lastUrlPos, (urlPos - lastUrlPos));
            }

        } else {
            nonurl = txt.mid(lastUrlPos);
        }
        
        if (!nonurl.isEmpty()) {
            // See if there are any file:// links in the non-URL part
            int lastFilePos = 0;
            while (lastFilePos != -1) {
                QString file;
                QString nonfile;

                int nextFilePos = -1;
                int filePos = filePattern.indexIn(nonurl, lastFilePos);
                if (filePos != -1) {
                    nonfile = nonurl.mid(lastFilePos, (filePos - lastFilePos));
                    file = filePattern.cap(0);
                    nextFilePos = filePos + file.length();
                } else {
                    nonfile = nonurl.mid(lastFilePos);
                }
                
                if (!nonfile.isEmpty()) {
                    // See if there are any addresses in the non-URL part
                    int lastAddressPos = 0;
                    while (lastAddressPos != -1) {
                        QString address;
                        QString nonaddress;

                        int nextAddressPos = -1;
                        int addressPos = addressPattern.indexIn(nonfile, lastAddressPos);
                        if (addressPos != -1) {
                            nonaddress = nonfile.mid(lastAddressPos, (addressPos - lastAddressPos));
                            address = addressPattern.cap(0);
                            nextAddressPos = addressPos + address.length();
                        } else {
                            nonaddress = nonfile.mid(lastAddressPos);
                        }
                        
                        if (!nonaddress.isEmpty()) {
                            // Write this plain text out in escaped form
                            result.append(Qt::escape(nonaddress));
                        }
                        if (!address.isEmpty()) {
                            // Write out the address link
                            result.append(refMailTo(QMailAddress(address)));
                        }

                        lastAddressPos = nextAddressPos;
                    }
                }
                if (!file.isEmpty()) {
                    // Write out the file link
                    result.append(refUrl(file, "file://", QString()));
                }

                lastFilePos = nextFilePos;
            }
        }
        if (!url.isEmpty()) {
            // Write out the URL link
            result.append(refUrl(url, scheme, trailing));
        }

        lastUrlPos = nextUrlPos;
    }

    return result.join("");
}

QString Browser::listRefMailTo(const QList<QMailAddress>& list)
{
    QStringList result;
    foreach ( const QMailAddress& address, list )
        result.append( refMailTo( address ) );

    return result.join( ", " );
}

QString Browser::refMailTo(const QMailAddress& address)
{
    QString name = Qt::escape(address.toString());
    if (name == "System")
        return name;

    if (address.isPhoneNumber() || address.isEmailAddress())
        return "<a href=\"mailto:" + Qt::escape(address.address()) + "\">" + name + "</a>";

    return name;
}

QString Browser::refNumber(const QString& number)
{
    return "<a href=\"dial;" + Qt::escape(number) + "\">" + number + "</a>";
}

QString Browser::refUrl(const QString& url, const QString& scheme, const QString& trailing)
{
    // Assume HTTP if there is no scheme
    QString escaped(Qt::escape(url));
    QString target(scheme.isEmpty() ? "http://" + escaped : escaped);

    return "<a href=\"" + target + "\">" + escaped + "</a>" + Qt::escape(trailing);
}

void Browser::keyPressEvent(QKeyEvent* event)
{
    const int factor = width() * 2 / 3;

    switch (event->key()) {
        case Qt::Key_Left:
            scrollBy(-factor, 0);
            event->accept();
            break;

        case Qt::Key_Right:
            scrollBy(factor, 0);
            event->accept();
            break;

        default:
            QTextBrowser::keyPressEvent(event);
            break;
    }
}

