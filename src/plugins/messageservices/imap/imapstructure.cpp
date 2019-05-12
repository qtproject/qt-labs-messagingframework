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

#include "imapstructure.h"

#include <qmaillog.h>
#include <qmailmessage.h>


namespace {

template <typename Processor>
void processChars(Processor p, const QString& field, QString::const_iterator origin)
{
    bool quoted = false;
    bool escaped = false;
    int depth = 0;

    QString::const_iterator begin = origin, it = begin, end = field.end();

    do {
        char c((*it).toLatin1());

        switch (c) {
        case ')':
            if (!quoted && !escaped) {
                if (it != begin) {
                    if (p(c, depth, quoted, field, begin, it)) {
                        begin = it + 1;
                    }
                }

                --depth;
            }
            break;

        case '(':
            if (!quoted && !escaped) {
                if (p(c, depth, quoted, field, begin, it)) {
                    begin = it + 1;
                }

                ++depth;
            }
            break;

        case '"':
            if (p(c, depth, quoted, field, begin, it)) {
                begin = it + 1;
            }
            if (!escaped) {
                quoted = !quoted;
            }
            break;

        default:
            if (p(c, depth, quoted, field, begin, it)) {
                begin = it + 1;
            }
            break;
        }

        escaped = (c == '\\');
    } while (++it != end);
    
    if ((it != begin) && (depth >= 0)) {
        // Terminate the sequence
        p('\0', depth, quoted, field, begin, it);
    }
}


struct StructureDecomposer
{
    int reportDepth;
    QStringList result;

    StructureDecomposer() : reportDepth(0) {}

    bool operator()(char c, int depth, bool, const QString &field, QString::const_iterator begin, QString::const_iterator it)
    {
        if ((c == ')') || (c == '\0')) {
            if (depth == reportDepth) {

                // Allow reportDepth to have negative values, meaning that we're
                // processing more fields than requested (happens with Yahoo IMAP)
                // When processing a negative depth, the unrequested content is silently
                // discarded

                if (depth >= 0)
                    result.append(field.mid(begin - field.begin(), it - begin).trimmed());

                --reportDepth;

                return true;
            }
        } else if (c == '(') {
            if (it == begin) {
                if (reportDepth != 0) return false;

                ++reportDepth;
                return true;
            }
        }

        return false;
    }
};

QStringList decomposeStructure(const QString &structure, int offset)
{
    if (structure.isEmpty())
        return QStringList();

    StructureDecomposer sd;
    processChars<StructureDecomposer&>(sd, structure, structure.begin() + offset);
    return sd.result;
}


struct ElementDecomposer
{
    int reportDepth;
    QStringList result;

    ElementDecomposer() : reportDepth(0) {}

    void append(const QString &token, int begin, int length, int depth)
    {
        if ((depth == 0) && (!token.isEmpty()) && (token.at(begin) == QChar('"')) && (token.at(begin + length - 1) == QChar('"'))) {
            ++begin;
            length -= 2;
        }

        result.append(token.mid(begin, length));
    }

    bool operator()(char c, int depth, bool quoted, const QString &field, QString::const_iterator begin, QString::const_iterator it)
    {
        if (((c == ' ') && !quoted) || (c == '\0')) {
            if (depth == 0) {
                if (it != begin) {
                    append(field, begin - field.begin(), it - begin, depth);
                }

                return true;
            }
        } else if (c == ')') {
            if (depth == reportDepth) {
                append(field, begin - field.begin(), it - begin, depth);

                if (reportDepth > 0)
                    --reportDepth;

                return true;
            }
        } else if (c == '(') {
            if (it == begin) {
                if (reportDepth > 0) return false;

                ++reportDepth;
                return true;
            }
        }

        return false;
    }
};

QStringList decomposeElements(const QString &element)
{
    if (element.isEmpty() || (element.trimmed().toUpper() == "NIL"))
        return QStringList();

    ElementDecomposer ed;
    processChars<ElementDecomposer&>(ed, element, element.begin());
    return ed.result;
}


QMailMessageBody::TransferEncoding fromEncodingDescription(const QString &desc)
{
    if (!desc.isEmpty()) {
        const QString value(desc.trimmed().toUpper());

        // TODO: these strings aren't actually specified by RFC3501...
        if (value == "7BIT") {
            return QMailMessageBody::SevenBit;
        } else if (value == "8BIT") {
            return QMailMessageBody::EightBit;
        } else if (value == "BINARY") {
            return QMailMessageBody::Binary;
        } else if (value == "QUOTED-PRINTABLE") {
            return QMailMessageBody::QuotedPrintable;
        } else if (value == "BASE64") {
            return QMailMessageBody::Base64;
        }
    }

    return QMailMessageBody::NoEncoding;
}

QMailMessagePartContainer::MultipartType fromMultipartDescription(const QString &desc)
{
    if (!desc.isEmpty()) {
        const QString value(desc.trimmed().toUpper());

        // TODO: these strings aren't actually specified by RFC3501...
        if (value == "MIXED") {
            return QMailMessagePartContainer::MultipartMixed;
        } else if (value == "ALTERNATIVE") {
            return QMailMessagePartContainer::MultipartAlternative;
        } else if (value == "DIGEST") {
            return QMailMessagePartContainer::MultipartDigest;
        } else if (value == "SIGNED") {
            return QMailMessagePartContainer::MultipartSigned;
        } else if (value == "ENCRYPTED") {
            return QMailMessagePartContainer::MultipartEncrypted;
        } else if (value == "PARALLEL") {
            return QMailMessagePartContainer::MultipartParallel;
        } else if (value == "RELATED") {
            return QMailMessagePartContainer::MultipartRelated;
        } else if (value == "FORMDATA") {
            return QMailMessagePartContainer::MultipartFormData;
        } else if (value == "REPORT") {
            return QMailMessagePartContainer::MultipartReport;
        }
    }

    return QMailMessagePartContainer::MultipartNone;
}

QMailMessageContentDisposition fromDispositionDescription(const QString &desc, const QString &size)
{
    QMailMessageContentDisposition disposition;

    QStringList details(decomposeElements(desc));
    if (!details.isEmpty()) {
        const QString value(details.at(0).trimmed().toUpper());

        if (value == "INLINE") {
            disposition.setType(QMailMessageContentDisposition::Inline);
        } else {
            // Anything else should be treated as attachment
            disposition.setType(QMailMessageContentDisposition::Attachment);
        }

        if (details.count() > 1) {
            const QStringList parameters(decomposeElements(details.at(1)));
            if (parameters.count() % 2)
                qWarning() << "Incorrect fromDispositionDescription parameters:" << parameters;
            QStringList::const_iterator it = parameters.begin(), end = parameters.end();
            while ((it != end) && (it + 1 != end)) {
                disposition.setParameter((*it).toLatin1(), (*(it + 1)).toLatin1());
                it+=2;
            }
        }
    } else {
        // Default to inline for no specification
        disposition.setType(QMailMessageContentDisposition::Inline);
    }

    if (!size.isEmpty() && disposition.size() == -1) {
        // We use the body size only if Content-Disposition/Size is not available.
        // From RFC3501 INTERNET MESSAGE ACCESS PROTOCOL - VERSION 4rev1:
        // body size
        //   A number giving the size of the body in octets.  Note that
        //   this size is the size in its transfer encoding and not the
        //   resulting size after any decoding.
        disposition.setSize(size.toInt());
    }

    return disposition;
}

void setBodyFromDescription(const QStringList &details, QMailMessagePartContainer *container, uint *size, bool *wellFormed)
{
    QMailMessageContentType type;

    // [0]: type
    // [1]: sub-type
    type.setType(details.at(0).toLatin1());
    type.setSubType(details.at(1).toLatin1());

    // [2]: parameter list
    const QStringList parameters(decomposeElements(details.at(2)));
    if (parameters.count() % 2) {
        qWarning() << "Incorrect setBodyFromDescription parameters:" << parameters;
        if (wellFormed) {
            *wellFormed = false;
        }
    }
    QStringList::const_iterator it = parameters.begin(), end = parameters.end();
    for ( ; (it != end) && ((it + 1) != end); it += 2) {
        type.setParameter((*it).toLatin1(), (*(it + 1)).toLatin1());
    }

    // [3]: content-ID
    // [4]: content-description

    // [5]: content-encoding
    QMailMessageBody::TransferEncoding encoding(fromEncodingDescription(details.at(5)));

    // [6]: size
    if (size) {
        bool ok;
        *size = details.at(6).toUInt(&ok);
        if (!ok && wellFormed) {
            *wellFormed = false;
        }
    }

    container->setBody(QMailMessageBody::fromData(QByteArray(), type, encoding, QMailMessageBody::AlreadyEncoded));
}

void setPartContentFromStructure(const QStringList &structure, QMailMessagePart *part, uint *size, bool *wellFormed);

void setPartFromDescription(const QStringList &details, QMailMessagePart *part)
{
    // [3]: content-ID
    const QString &id(details.at(3));
    if (!id.isEmpty() && (id.trimmed().toUpper() != "NIL")) {
        part->setContentID(id);
    }

    // [4]: content-description
    const QString &description(details.at(4));
    if (!description.isEmpty() && (description.trimmed().toUpper() != "NIL")) {
        part->setContentDescription(description);
    }

    // [6]: content size
    const QString &size(details.at(6));
    int next = 7;

    const QMailMessageContentType &type(part->contentType());
    if (type.matches("text")) {
        // The following field is the part's line count
        ++next;
    } else if (type.matches("message", "rfc822")) {
        // For parts of type 'message/rfc822', there are three extra fields here...
        next += 3;
    }

    // [6 + n + 1]: MD5-sum of content
    // Ignore the potential MD5-sum for now
    ++next;

    // [6 + n + 2]: content-disposition
    QString disposition;
    if (next < details.count()) {
        disposition = details.at(next);
        if (disposition.trimmed().toUpper() == "NIL") {
            disposition.clear();
        }
    }
    part->setContentDisposition(fromDispositionDescription(disposition, size));
    ++next;

    // [6 + n + 3]: content-language
    if (next < details.count()) {
        const QString &language(details.at(next));

        // TODO: this may need compression from multiple tokens to one?
        if (!language.isEmpty() && (language.trimmed().toUpper() != "NIL"))
            part->setContentLanguage(language);
    }
    ++next;

    // [6 + n + 3]: content-location
    if (next < details.count()) {
        const QString &location(details.at(next));

        // TODO: this may need compression from multiple tokens to one?
        if (!location.isEmpty() && (location.trimmed().toUpper() != "NIL"))
            part->setContentLocation(location);
    }
    ++next;
}

void setMultipartFromDescription(const QStringList &structure, QMailMessagePartContainer *container, QMailMessagePart *part, uint *size, bool *wellFormed)
{
    QStringList details = decomposeElements(structure.last());

    // [0]: type
    container->setMultipartType(fromMultipartDescription(details.at(0)));

    // [1]: parameter list
    if (details.count() > 1) {
        const QStringList parameters(decomposeElements(details.at(1)));
        QStringList::const_iterator it = parameters.begin(), end = parameters.end();
        if (parameters.count() % 2) {
            qWarning() << "Incorrect setMultipartFromDescription parameter count" << parameters.last();
            if (wellFormed) {
                *wellFormed = false;
            }
        }
        for ( ; (it != end) && ((it + 1) != end); it += 2) {
            if ((*it).trimmed().toUpper() == "BOUNDARY") {
                const QString boundary((*(it + 1)));
                if (boundary.toUpper() == "NIL") {
                    container->setBoundary(QByteArray());
                } else {
                    container->setBoundary(boundary.toLatin1());
                }
            }
        }
    }

    if (part) {
        // [2]: content-disposition
        QString disposition;
        if (details.count() > 2) {
            disposition = details.at(2);
            if (disposition.trimmed().toUpper() == "NIL") {
                disposition.clear();
            }
        }
        part->setContentDisposition(fromDispositionDescription(disposition, QString()));

        // [3]: content-language
        if (details.count() > 3) {
            const QString &language(details.at(3));

            // TODO: this may need compression from multiple tokens to one?
            if (!language.isEmpty() && (language.trimmed().toUpper() != "NIL"))
                part->setContentLanguage(language);
        }
        
        // [4]: content-location
        if (details.count() > 4) {
            const QString &location(details.at(4));

            // TODO: this may need compression from multiple tokens to one?
            if (!location.isEmpty() && (location.trimmed().toUpper() != "NIL"))
                part->setContentLocation(location);
        }
    }

    // Create the other pieces described by the structure
    for (int i = 0; i < (structure.count() - 1); ++i) {
        QMailMessagePart part;
        uint partSize = 0;

        setPartContentFromStructure(decomposeStructure(structure.at(i), 0), &part, &partSize, wellFormed);
        container->appendPart(part);

        if (size) {
            *size += partSize;
        }
    }
}

void setPartContentFromStructure(const QStringList &structure, QMailMessagePart *part, uint *size, bool *wellFormed)
{
    if (!structure.isEmpty()) {
        // The last element is the message
        const QString &message = structure.last();
        if (!message.isEmpty()) {
            if (structure.count() == 1) {
                QStringList details(decomposeElements(message));
                if (details.count() < 7) {
                    qWarning() << "Ill-formed part structure:" << details;
                    if (wellFormed) {
                        *wellFormed = false;
                    }
                } else {
                    setBodyFromDescription(details, part, size, wellFormed);

                    if (details.count() > 7) {
                        setPartFromDescription(details, part);
                    }
                }
            } else {
                // This is a multi-part message
                setMultipartFromDescription(structure, part, part, size, wellFormed);
            }
        }
    }
}

}

bool setMessageContentFromStructure(const QStringList &structure, QMailMessage *message)
{
    bool wellFormed = false;
    if (!structure.isEmpty()) {
        wellFormed = true;
        // The last element is the message description
        const QString &description = structure.last();
        if (!description.isEmpty()) {
            uint size = 0;
            if (structure.count() == 1) {
                QStringList details(decomposeElements(description));
                if (details.count() < 7) {
                    qWarning() << "Ill-formed body structure:" << details;
                    wellFormed = false;
                } else {
                    setBodyFromDescription(details, message, &size, &wellFormed);
                }
            } else {
                // This is a multi-part message
                setMultipartFromDescription(structure, message, Q_NULLPTR, &size, &wellFormed);
            }

            message->setContentSize(size);
            if (!wellFormed) {
                // Ill-formed message set status of message as not retrieved
                message->setStatus(QMailMessage::ContentAvailable, false);
                message->setStatus(QMailMessage::PartialContentAvailable, false);
            }
        }
        
        // See if any of the parts are attachments
        if (message->hasAttachments()) {
            message->setStatus( QMailMessage::HasAttachments, true );
        }
    }
    return wellFormed;
}

QStringList getMessageStructure(const QString &field)
{
    static const QString marker("BODYSTRUCTURE (");
    int index = field.indexOf(marker, Qt::CaseInsensitive);
    if (index != -1) {
        return decomposeStructure(field, index + marker.length());
    }

    return QStringList();
}

