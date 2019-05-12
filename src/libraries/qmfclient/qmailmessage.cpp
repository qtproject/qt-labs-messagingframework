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

#include "qmailmessage_p.h"
#include "qmailaddress.h"
#include "qmailcodec.h"
#include "qmaillog.h"
#include "qmailnamespace.h"
#include "qmailtimestamp.h"
#include "qmailcrypto.h"
#include "longstring_p.h"

#ifndef QTOPIAMAIL_PARSING_ONLY
#include "qmailaccount.h"
#include "qmailfolder.h"
#include "qmailstore.h"
#endif

#include <QtGlobal>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qregexp.h>
#include <qtextstream.h>
#include <qtextcodec.h>
#include <QTextCodec>
#include <QtDebug>
#ifdef USE_HTML_PARSER
#include <QTextDocument>
#endif

#include <stdlib.h>
#include <limits.h>
#if defined(Q_OS_WIN) && defined(_WIN32_WCE)
#include <cctype>
#else
#include <ctype.h>
#endif

static const QByteArray internalPrefix()
{
    static const QByteArray prefix("X-qmf-internal-");
    return prefix;
}

template<typename CharType>
inline char toPlainChar(CharType value) { return value; }

template<>
inline char toPlainChar<QChar>(QChar value) { return static_cast<char>(value.unicode() & 0x7f); }

template<typename CharType>
inline bool asciiRepresentable(const CharType& value) { return ((value <= 127) && (value >= 0)); }

template<>
inline bool asciiRepresentable<unsigned char>(const unsigned char& value) { return (value <= 127); }

template<>
inline bool asciiRepresentable<signed char>(const signed char& value) { return (value >= 0); }

// The correct test for char depends on whether the platform defines char as signed or unsigned
// Default to signed char:
template<bool SignedChar>
inline bool asciiRepresentableChar(const char& value) { return asciiRepresentable(static_cast<signed char>(value)); }

template<>
inline bool asciiRepresentableChar<false>(const char& value) { return asciiRepresentable(static_cast<unsigned char>(value)); }

template<>
inline bool asciiRepresentable<char>(const char& value) { return asciiRepresentableChar<(SCHAR_MIN < CHAR_MIN)>(value); }

template<typename StringType>
QByteArray to7BitAscii(const StringType& src)
{
    QByteArray result;
    result.reserve(src.length());

    typename StringType::const_iterator it = src.begin();
    for (const typename StringType::const_iterator end = it + src.length(); it != end; ++it)
        if (asciiRepresentable(*it))
            result.append(toPlainChar(*it));

    return result;
}

template<typename StringType>
bool is7BitAscii(const StringType& src)
{
    bool result = true;

    typename StringType::const_iterator it = src.begin();
    for (const typename StringType::const_iterator end = it + src.length(); it != end && result; ++it)
        result &= (asciiRepresentable(*it));

    return result;
}

// Parsing functions
static int insensitiveIndexOf(const QByteArray& content, const QByteArray& container, int from = 0)
{
    const char* const matchBegin = content.constData();
    const char* const matchEnd = matchBegin + content.length();

    const char* const begin = container.constData();
    const char* const end = begin + container.length() - (content.length() - 1);

    const char* it = begin + from;
    while (it < end)
    {
        if (toupper(*it++) == toupper(*matchBegin))
        {
            const char* restart = it;

            // See if the remainder matches
            const char* searchIt = it;
            const char* matchIt = matchBegin + 1;

            do 
            {
                if (matchIt == matchEnd)
                    return ((it - 1) - begin);

                // We may find the next place to search in our scan
                if ((restart == it) && (*searchIt == *(it - 1)))
                    restart = searchIt;
            }
            while (toupper(*searchIt++) == toupper(*matchIt++));

            // No match
            it = restart;
        }
    }

    return -1;
}

static bool insensitiveEqual(const QByteArray& lhs, const QByteArray& rhs)
{
    if (lhs.isNull() || rhs.isNull())
        return (lhs.isNull() && rhs.isNull());

    if (lhs.length() != rhs.length())
        return false;

    return insensitiveIndexOf(lhs, rhs) == 0;
}

static QByteArray charsetForInput(const QString& input)
{
    // See if this input needs encoding
    bool latin1 = false;

    const QChar* it = input.constData();
    const QChar* const end = it + input.length();
    for ( ; it != end; ++it)
    {
        if ((*it).unicode() > 0xff)
        {
            // Multi-byte characters included - we need to use UTF-8
            return QByteArray("UTF-8");
        }
        else if (!latin1 && ((*it).unicode() > 0x7f))
        {
            // We need encoding from latin-1
            latin1 = true;
        }
    }

    return (latin1? QByteArray("ISO-8859-1") : QByteArray());
}

static QByteArray fromUnicode(const QString& input, const QByteArray& charset)
{
    if (!charset.isEmpty() && (insensitiveIndexOf("ascii", charset) == -1))
    {
        // See if we can convert using the nominated charset
        if (QTextCodec* textCodec = QMailCodec::codecForName(charset))
            return textCodec->fromUnicode(input);

        qWarning() << "fromUnicode: unable to find codec for charset:" << charset;
    }

    return to7BitAscii(input.toLatin1());
}

static QString toUnicode(const QByteArray& input, const QByteArray& charset, const QByteArray& bodyCharset = QByteArray())
{
    if (!charset.isEmpty() && (insensitiveIndexOf("ascii", charset) == -1)) {
        // See if we can convert using the nominated charset
        if (QTextCodec* textCodec = QMailCodec::codecForName(charset))
            return textCodec->toUnicode(input);

        qWarning() << "toUnicode: unable to find codec for charset:" << charset;
    } else {
        QByteArray autoCharset = QMailCodec::autoDetectEncoding(input).toLatin1();
        // We don't trust on Encoding Detection for the case of "ISO-8859-* charsets.
        if (insensitiveIndexOf("ISO-8859-", autoCharset) == -1) {
            QTextCodec* textCodec = QMailCodec::codecForName(autoCharset);
            if (!autoCharset.isEmpty() && textCodec)
                return textCodec->toUnicode(input);

            qWarning() << "toUnicode: unable to find codec for autodetected charset:" << autoCharset;
        }
    }
    if (is7BitAscii(input)) {
        return QString::fromLatin1(input.constData(), input.length());
    }
    if (!bodyCharset.isEmpty()) {
        // See if we can convert using the body charset
        if (QTextCodec* textCodec = QMailCodec::codecForName(bodyCharset))
            return textCodec->toUnicode(input);

        qWarning() << "toUnicode: unable to find codec for charset:" << charset;
    }
    return QString::fromLatin1(to7BitAscii(QString::fromLatin1(input.constData(), input.length())));
}

static QMailMessageBody::TransferEncoding encodingForName(const QByteArray& name)
{
    QByteArray ciName = name.toLower();

    if (ciName == "7bit")
        return QMailMessageBody::SevenBit;
    if (ciName == "8bit")
        return QMailMessageBody::EightBit;
    if (ciName == "base64")
        return QMailMessageBody::Base64;
    if (ciName == "quoted-printable")
        return QMailMessageBody::QuotedPrintable;
    if (ciName == "binary")
        return QMailMessageBody::Binary;

    return QMailMessageBody::NoEncoding;
}

static const char* nameForEncoding(QMailMessageBody::TransferEncoding te)
{
    switch( te ) 
    {
        case QMailMessageBody::SevenBit:
            return "7bit";
        case QMailMessageBody::EightBit:
            return "8bit";
        case QMailMessageBody::QuotedPrintable:
            return "quoted-printable";
        case QMailMessageBody::Base64:
            return "base64";
        case QMailMessageBody::Binary:
            return "binary";
        case QMailMessageBody::NoEncoding:
            break;
    }

    return Q_NULLPTR;
}

static QMailCodec* codecForEncoding(QMailMessageBody::TransferEncoding te, bool textualData)
{
    switch( te ) 
    {
        case QMailMessageBody::NoEncoding:
        case QMailMessageBody::Binary:
            return new QMailPassThroughCodec();

        case QMailMessageBody::SevenBit:
        case QMailMessageBody::EightBit:
            if (textualData) {
                return static_cast<QMailCodec*>(new QMailLineEndingCodec());
            } else {
                return new QMailPassThroughCodec();
            }

        case QMailMessageBody::QuotedPrintable:
            if (textualData) {
                return new QMailQuotedPrintableCodec(
                    QMailQuotedPrintableCodec::Text,
                    QMailQuotedPrintableCodec::Rfc2045);
            } else {
                return new QMailQuotedPrintableCodec(
                    QMailQuotedPrintableCodec::Binary, 
                    QMailQuotedPrintableCodec::Rfc2045);
            }
            
        case QMailMessageBody::Base64:
            if (textualData) {
                return new QMailBase64Codec(QMailBase64Codec::Text);
            } else {
                return new QMailBase64Codec(QMailBase64Codec::Binary);
            }
    }

    return Q_NULLPTR;
}

static QMailCodec* codecForEncoding(QMailMessageBody::TransferEncoding te, const QMailMessageContentType& content)
{
    return codecForEncoding(te, insensitiveEqual(content.type(), "text"));
}

//  Needs an encoded word of the form =?charset?q?word?=
static QString decodeWord(const QByteArray& encodedWord)
{
    QString result;
    int index[4];

    // Find the parts of the input
    index[0] = encodedWord.indexOf("=?");
    if (index[0] != -1)
    {
        index[1] = encodedWord.indexOf('?', index[0] + 2);
        if (index[1] != -1)
        {
            index[2] = encodedWord.indexOf('?', index[1] + 1);
            index[3] = encodedWord.lastIndexOf("?=");
            if ((index[2] != -1) && (index[3] > index[2]))
            {
                QByteArray charset = QMail::unquoteString(encodedWord.mid(index[0] + 2, (index[1] - index[0] - 2)));
                QByteArray encoding = encodedWord.mid(index[1] + 1, (index[2] - index[1] - 1)).toUpper();
                QByteArray encoded = encodedWord.mid(index[2] + 1, (index[3] - index[2] - 1));

                if (encoding == "Q")
                {
                    QMailQuotedPrintableCodec codec(QMailQuotedPrintableCodec::Text, QMailQuotedPrintableCodec::Rfc2047);
                    return codec.decode(encoded, QString::fromUtf8(charset));
                }
                else if (encoding == "B")
                {
                    QMailBase64Codec codec(QMailBase64Codec::Binary);
                    return codec.decode(encoded, QString::fromUtf8(charset));
                }
            }
        }
    }

    return result;
}

static QByteArray generateEncodedWord(const QByteArray& codec, char encoding, const QByteArray& text)
{
    QByteArray result("=?");
    result.append(codec);
    result.append('?');
    result.append(encoding);
    result.append('?');
    result.append(text);
    result.append("?=");
    return result;
}

static QByteArray generateEncodedWord(const QByteArray& codec, char encoding, const QList<QByteArray>& list)
{
    QByteArray result;

    foreach (const QByteArray& item, list)
    {
        if (!result.isEmpty())
            result.append(' ');

        result.append(generateEncodedWord(codec, encoding, item));
    }

    return result;
}

static inline bool isFirstCodePoint(const char *b)
{
    return (*b & 0xc0) != 0x80;
}

static QList<QByteArray> splitUtf8(const QByteArray& input, int maximumEncoded)
{
    QList<QByteArray> result;

    // Maximum Utf8 string to be codified in base64.
    int maxUtf8Chars = maximumEncoded / 4 * 3;

    // Utf8 string shorter than maximumEncoded in base64.
    if (input.length() < maxUtf8Chars) {
        result.append(input);
        return result;
    }

    qMailLog(Messaging) << Q_FUNC_INFO << "Need to cut the UTF-8 string !!!";

    // Need to cut the utf8 string.
    QByteArray str(input);
    do {

        const char *iter = str.constData() + maxUtf8Chars;

        // Check if we reached a valid cutting point.
        int index = maxUtf8Chars;
        while (!isFirstCodePoint(iter--)) {
            qMailLog(Messaging) << Q_FUNC_INFO << "Cutting point not valid, looking for the previous one !!!";
            Q_ASSERT (index >= 0);
            index--;
        }

        // Create substrings.
        result.append(str.left(index));
        str = str.mid(index);

        qMailLog(Messaging) << Q_FUNC_INFO << "The STR is still too long !!!";

    } while (str.length() > maxUtf8Chars);

    // Append te rest of the string.
    if (str.length() > 0) {
        result.append(str);
    }

    return result;
}

static QList<QByteArray> split(const QByteArray& input, const QByteArray& separator)
{
    QList<QByteArray> result;

    int index = -1;
    int lastIndex = -1;
    do
    {
        lastIndex = index;
        index = input.indexOf(separator, lastIndex + 1);

        int offset = (lastIndex == -1 ? 0 : lastIndex + separator.length());
        int length = (index == -1 ? -1 : index - offset);
        result.append(input.mid(offset, length));
    } while (index != -1);

    return result;
}

static QByteArray encodeWord(const QString &text, const QByteArray& cs, bool* encoded)
{
    // Do we need to encode this input?
    QByteArray charset(cs);
    if (charset.isEmpty())
        charset = charsetForInput(text);

    if (encoded)
        *encoded = true;

    // We can't allow more than 75 chars per encoded-word, including the boiler plate...
    int maximumEncoded = 75 - 7 - charset.length();

    // If this is an encodedWord, we need to include any whitespace that we don't want to lose
    if (insensitiveIndexOf("utf-8", charset) == 0)
    {
        QList<QByteArray> listEnc;
        QList<QByteArray> list = splitUtf8(text.toUtf8(), maximumEncoded);
        foreach (const QByteArray &item, list) {
            QMailBase64Codec codec(QMailBase64Codec::Binary, maximumEncoded);
            QByteArray encoded = codec.encode(item);
            listEnc.append(encoded);
        }

        return generateEncodedWord(charset, 'B', listEnc);
    }
    else if (insensitiveIndexOf("iso-8859-", charset) == 0)
    {
        QMailQuotedPrintableCodec codec(QMailQuotedPrintableCodec::Text, QMailQuotedPrintableCodec::Rfc2047, maximumEncoded);
        QByteArray encoded = codec.encode(text, QString::fromUtf8(charset));
        return generateEncodedWord(charset, 'Q', split(encoded, "=\r\n"));
    }

    if (encoded)
        *encoded = false;

    return to7BitAscii(text);
}

static QString decodeWordSequence(const QByteArray& str)
{
    QRegExp whitespace(QLatin1String("^\\s+$"));

    QString out;

    // From RFC 2047
    // encoded-word = "=?" charset "?" encoding "?" encoded-text "?="
    QRegExp encodedWord(QLatin1String("\"?=\\?[^\\s\\?]+\\?[^\\s\\?]+\\?[^\\s\\?]*\\?=\"?"));

    int pos = 0;
    int lastPos = 0;
    QString latin1Str(QString::fromLatin1(str.constData(), str.length()));

    while (pos != -1) {
        pos = encodedWord.indexIn(latin1Str, pos);
        if (pos != -1) {
            int endPos = pos + encodedWord.matchedLength();

            QString preceding(QString::fromLatin1(str.mid(lastPos, (pos - lastPos))));
            QString decoded = decodeWord(str.mid(pos, (endPos - pos)));

            // If there is only whitespace between two encoded words, it should not be included
            if (!whitespace.exactMatch(preceding))
                out.append(preceding);

            out.append(decoded);

            pos = endPos;
            lastPos = pos;
        }
    }

    // Copy anything left
    out.append(QString::fromLatin1(str.mid(lastPos)));
    return out;
}

enum EncodingTokenType
{
    Whitespace,
    Word,
    Quote
};

typedef QPair<const QChar*, int> TokenRange;
typedef QPair<EncodingTokenType, TokenRange> Token;

static Token makeToken(EncodingTokenType type, const QChar* begin, const QChar* end, bool escaped)
{
    return qMakePair(type, qMakePair(begin, static_cast<int>(end - begin) - (escaped ? 1 : 0)));
}

static QList<Token> tokenSequence(const QString& input)
{
    QList<Token> result;

    bool escaped = false;

    const QChar* it = input.constData();
    const QChar* const end = it + input.length();
    if (it != end) 
    {
        const QChar* token = it;
        EncodingTokenType state = ((*it) == '"' ? Quote : ((*it).isSpace() ? Whitespace : Word)); 

        for (++it; it != end; ++it) 
        {
            if (!escaped && (*it == '\\')) 
            {
                escaped = true;
                continue;
            }

            if (state == Quote)
            {
                // This quotation mark is a token by itself
                result.append(makeToken(state, token, it, escaped));

                state = ((*it) == '"' && !escaped ? Quote : ((*it).isSpace() ? Whitespace : Word)); 
                token = it;
            }
            else if (state == Whitespace)
            {
                if (!(*it).isSpace())
                {
                    // We have passed the end of this whitespace-sequence
                    result.append(makeToken(state, token, it, escaped));

                    state = ((*it) == '"' && !escaped ? Quote : Word);
                    token = it;
                }
            }
            else
            {
                if ((*it).isSpace() || ((*it) == '"' && !escaped))
                {
                    // We have passed the end of this word
                    result.append(makeToken(state, token, it, escaped));

                    state = ((*it).isSpace() ? Whitespace : Quote);
                    token = it;
                }
            }

            escaped = false;
        }

        result.append(makeToken(state, token, it, false));
    }

    return result;
}

static QByteArray encodeWordSequence(const QString& str, const QByteArray& charset)
{
    QByteArray result;

    bool quoted = false;
    bool tokenEncoded = false;
    QString quotedText;
    QString heldWhitespace;

    foreach (const Token& token, tokenSequence(str))
    {
        QString chars = QString::fromRawData(token.second.first, token.second.second);

        // See if we're processing some quoted words
        if (quoted)
        {
            if (token.first == Quote)
            {
                // We have reached the end of a quote sequence
                quotedText.append(chars);

                bool lastEncoded = tokenEncoded;

                QByteArray output = encodeWord(heldWhitespace + quotedText, charset, &tokenEncoded);

                quotedText.clear();;
                quoted = false;
                heldWhitespace.clear();

                if (lastEncoded && tokenEncoded)
                    result.append(' ');
                result.append(output);
            }
            else
            {
                quotedText.append(chars);
            }
        }
        else
        {
            if (token.first == Quote)
            {
                // This token begins a quoted sequence
                quotedText = chars;
                quoted = true;
            }
            else
            {
                if (token.first == Word)
                {
                    bool lastEncoded = tokenEncoded;

                    // See if this token needs encoding
                    QByteArray output = encodeWord(heldWhitespace + chars, charset, &tokenEncoded);
                    heldWhitespace.clear();

                    if (lastEncoded && tokenEncoded)
                        result.append(' ');
                    result.append(output);
                }
                else // whitespace
                {
                    // If the last token was an encoded-word, we may need to include this
                    // whitespace into the next token
                    if (tokenEncoded)
                        heldWhitespace.append(chars);
                    else
                        result.append(chars.toLatin1());
                }
            }
        }
    }

    // Process trailing text after unmatched double quote character
    if (quoted)
    {
        bool lastEncoded = tokenEncoded;

        QByteArray output = encodeWord(heldWhitespace + quotedText, charset, &tokenEncoded);

        if (lastEncoded && tokenEncoded)
            result.append(' ');
        result.append(output);
    }
    
    return result;
}

static int hexValue(char value)
{
    // Although RFC 2231 requires capitals, we may as well accept miniscules too
    if (value >= 'a')
        return (((value - 'a') + 10) & 0x0f);
    if (value >= 'A')
        return (((value - 'A') + 10) & 0x0f);

    return ((value - '0') & 0x0f);
}

static int hexValue(const char* it)
{
    return ((hexValue(*it) << 4) | hexValue(*(it + 1)));
}

static QString decodeParameterText(const QByteArray& text, const QByteArray& charset)
{
    QByteArray decoded;
    decoded.reserve(text.length());

    // Decode any encoded bytes in the data
    const char* it = text.constData();
    for (const char* const end = it + text.length(); it != end; ++it)
    {
        if (*it == '%')
        {
            if ((end - it) > 2)
                decoded.append(hexValue(it + 1));

            it += 2;
        }
        else
            decoded.append(*it);
    }

    // Decoded contains a bytestream - decode to unicode text if possible
    return toUnicode(decoded, charset);
}

//  Needs an encoded parameter of the form charset'language'text
static QString decodeParameter(const QByteArray& encodedParameter)
{
    QRegExp parameterFormat(QLatin1String("([^']*)'(?:[^']*)'(.*)"));
    if (parameterFormat.exactMatch(QString::fromUtf8(encodedParameter)))
        return decodeParameterText(parameterFormat.cap(2).toLatin1(), parameterFormat.cap(1).toLatin1());

    // Treat the whole thing as input, and deafult the charset to ascii
    // This is not required by the RFC, since the input is illegal.  But, it 
    // seems ok since the parameter name has already indicated that the text 
    // should be encoded...
    return decodeParameterText(encodedParameter, "us-ascii");
}

static char hexRepresentation(int value)
{
    value &= 0x0f;

    if (value < 10)
        return ('0' + value);
    return ('A' + (value - 10));
}

static QByteArray generateEncodedParameter(const QByteArray& charset, const QByteArray& language, const QByteArray& text)
{
    QByteArray result(charset);
    QByteArray lang(language);

    // If the charset contains a language part, extract it
    int index = result.indexOf('*');
    if (index != -1)
    {
        // If no language is specified, use the extracted part
        if (lang.isEmpty())
            lang = result.mid(index + 1);

        result = result.left(index);
    }

    result.append('\'');
    result.append(lang);
    result.append('\'');
    
    // Have a guess at how long the result will be
    result.reserve(result.length() + (2 * text.length()));

    // We could encode the exact set of permissible characters here, but they're basically the alphanumerics
    const char* it = text.constData();
    const char* const end = it + text.length();
    for ( ; it != end; ++it) {
        if (::isalnum(static_cast<unsigned char>(*it))) {
            result.append(*it);
        } else {
            // Encode to hex
            int value = (*it);
            result.append('%').append(hexRepresentation(value >> 4)).append(hexRepresentation(value));
        }
    }

    return result;
}

static QByteArray encodeParameter(const QString &text, const QByteArray& charset, const QByteArray& language)
{
    QByteArray encoding(charset);
    if (encoding.isEmpty())
        encoding = charsetForInput(text);

    return generateEncodedParameter(encoding, language, fromUnicode(text, encoding));
}

static QByteArray removeComments(const QByteArray& input, int (*classifier)(int), bool acceptedResult = true)
{
    QByteArray result;

    int commentDepth = 0;
    bool quoted = false;
    bool escaped = false;

    const char* it = input.constData();
    const char* const end = it + input.length();
    for ( ; it != end; ++it ) {
        if ( !escaped && ( *it == '\\' ) ) {
            escaped = true;
            continue;
        }

        if ( *it == '(' && !escaped && !quoted ) {
            commentDepth += 1;
        }
        else if ( *it == ')' && !escaped && !quoted && ( commentDepth > 0 ) ) {
            commentDepth -= 1;
        }
        else {
            bool quoteProcessed = false;
            if ( !quoted && *it == '"' && !escaped ) {
                quoted = true;
                quoteProcessed = true;
            }

            if ( commentDepth == 0 ) {
                if ( quoted || (bool((*classifier)(*it)) == acceptedResult) )
                    result.append( *it );
            }

            if ( quoted && !quoteProcessed && *it == '"' && !escaped ) {
                quoted = false;
            }
        }

        escaped = false;
    }

    return result;
}


// Necessary when writing to QDataStream, because the string/char literal is encoded
// in various pre-processed ways...

struct DataString
{
    DataString(char datum) : _data(Q_NULLPTR), _length(0), _datum(datum) {}
    DataString(const char* data) : _data(data), _length(strlen(_data)), _datum('\0') {}
    DataString(const QByteArray& array) : _data(array.constData()), _length(array.length()), _datum('\0') {}

    inline QDataStream& toDataStream(QDataStream& out) const
    {
        if (_data)
            out.writeRawData(_data, _length);
        else if (_datum == '\n')
            // Ensure that line-feeds are always CRLF sequences
            out.writeRawData(QMailMessage::CRLF, 2);
        else if (_datum != '\0')
            out.writeRawData(&_datum, 1);

        return out;
    }

private:
    const char* _data;
    int _length;
    char _datum;
};

QDataStream& operator<<(QDataStream& out, const DataString& dataString)
{
    return dataString.toDataStream(out);
}



/* Utility namespaces for message part detection, finding and building */

const static char* imageContentType = "image";
const static char* textContentType = "text";
const static char* plainContentSubtype = "plain";
const static char* htmlContentSubtype = "html";

namespace findBody
{
    struct Context
    {
        Context() : found (Q_NULLPTR), alternateParent (Q_NULLPTR), contentType (textContentType) {}
        QMailMessagePartContainer *found;
        QMailMessagePartContainer *alternateParent;
        QList<QMailMessagePart::Location> htmlImageLoc;
        QList<const QMailMessagePart *> htmlImageParts;
        QList<QMailMessagePart::Location> htmlExtraPartsLoc;
        QByteArray contentType;
        QByteArray contentSubtype;
    };

    // Forward declarations
    bool inMultipartRelated(const QMailMessagePartContainer &container, Context &ctx);
    bool inMultipartMixed(const QMailMessagePartContainer &container, Context &ctx);

    bool inMultipartNone(const QMailMessagePartContainer &container, Context &ctx)
    {
        if (!container.contentType().matches(ctx.contentType, ctx.contentSubtype))
            return false;
        ctx.found = const_cast<QMailMessagePartContainer*>(&container);
        return true;
    }

    bool inMultipartNone(const QMailMessagePart &part, Context &ctx)
    {
        if (part.contentDisposition().type() == QMailMessageContentDisposition::Attachment)
            return false;

        if (!part.contentType().matches(ctx.contentType, ctx.contentSubtype))
            return false;

        ctx.found = const_cast<QMailMessagePart*> (&part);
        return true;
    }


    bool inMultipartAlternative(const QMailMessagePartContainer &container, Context &ctx)
    {
        for (int i = (int)container.partCount() - 1; i >= 0; i--) {
            const QMailMessagePart &part = container.partAt(i);
            switch (part.multipartType()) {
            case QMailMessagePart::MultipartNone:
                if (inMultipartNone(part, ctx)) {
                    ctx.alternateParent = const_cast<QMailMessagePartContainer*> (&container);
                    return true;
                }
                break;
            case QMailMessagePart::MultipartRelated:
                if (inMultipartRelated(part, ctx)) {
                    ctx.alternateParent = const_cast<QMailMessagePartContainer*> (&container);
                    return true;
                }
                break;
            default:
                // Default to handling as MultipartMixed
                if (inMultipartMixed(part, ctx)) {
                    ctx.alternateParent = const_cast<QMailMessagePartContainer*> (&container);
                    return true;
                }
                break;
            }
        }
        //qWarning() << Q_FUNC_INFO << "Multipart alternative message without body";
        return false;
    }

    // We assume MultipartRelated parts will hold the images
    // related to the HTML body, so they will be stored in
    // ctx.htmlImageLoc and ctx.htmlImageParts.
    bool inMultipartRelated(const QMailMessagePartContainer &container, Context &ctx)
    {
        int bodyPart = -1;
        for (int i = (int)container.partCount() - 1; ((i >= 0) && (bodyPart == -1)); i--) {
            const QMailMessagePart &part = container.partAt(i);
            switch (part.multipartType()) {
            case QMailMessagePart::MultipartNone:
                if (inMultipartNone(part, ctx))
                    bodyPart = i;
                break;
            case QMailMessagePart::MultipartAlternative:
                if (inMultipartAlternative(part, ctx))
                    bodyPart = i;
                break;
            default:
                qWarning() << Q_FUNC_INFO << "Multipart related message with unexpected subpart";
                // Default to handling as MultipartMixed
                if (inMultipartMixed(part, ctx))
                    bodyPart = i;
                break;
            }
        }

        // The body is not inside the container, so stop
        // looking for inline images.
        if (bodyPart == -1) {
            return false;
        }

        // We assume the inline images are the sibling parts
        // of the html body part.
        for (int i = (int)container.partCount() - 1; i >= 0; i--) {
            if (i != bodyPart) {
                const QMailMessagePart &part = container.partAt(i);
                if (part.contentType().matches(imageContentType)) {
                    ctx.htmlImageLoc << part.location();
                    ctx.htmlImageParts << &part;
                } else if (!part.contentID().isEmpty()) {
                    // Adding extra inline part
                    ctx.htmlExtraPartsLoc << part.location();
                }
            }
        }

        return true;
    }

    bool inMultipartSigned(const QMailMessagePartContainer &container, Context &ctx)
    {
        // Multipart/signed defined in RFC 1847, Section 2.1

        if (container.partCount() < 1)
            return false;

        const QMailMessagePart &part = container.partAt(0);

        switch (part.multipartType()) {

        case QMailMessagePart::MultipartNone:
            return inMultipartNone(part, ctx);

        case QMailMessagePart::MultipartMixed:
            return inMultipartMixed(part, ctx);

        case QMailMessagePart::MultipartAlternative:
            return inMultipartAlternative(part, ctx);

        case QMailMessagePart::MultipartRelated:
            return inMultipartRelated(part, ctx);

        case QMailMessagePart::MultipartSigned:
            return inMultipartSigned(part, ctx);

        default:
            qWarning() << Q_FUNC_INFO << "Multipart signed message with unexpected multipart type";
            // Default to handling as MultipartMixed
            return inMultipartMixed(part, ctx);
        }
    }

    bool inMultipartMixed(const QMailMessagePartContainer &container, Context &ctx)
    {
        for (uint i = 0; i < container.partCount(); i++) {
            const QMailMessagePart &part = container.partAt(i);
            if (part.referenceType() == QMailMessagePartFwd::MessageReference)
                continue;
            switch (part.multipartType()) {
            case QMailMessagePart::MultipartNone:
                if (inMultipartNone(part, ctx))
                    return true;
                break;
            case QMailMessagePart::MultipartAlternative:
                if (inMultipartAlternative(part, ctx))
                    return true;
                break;
            case QMailMessagePart::MultipartRelated:
                if (inMultipartRelated(part, ctx))
                    return true;
                break;
            case QMailMessagePart::MultipartSigned:
                if (inMultipartSigned(part, ctx))
                    return true;
                break;
            default:
                qWarning() << Q_FUNC_INFO << "Multipart mixed message with unexpected multipart type";
                // Default to handling as MultipartMixed
                if (inMultipartMixed(part, ctx))
                    return true;
                break;
            }
            // we haven't found what we looking for..
        }
        return false;
    }

    bool inPartContainer(const QMailMessagePartContainer &container, Context &ctx)
    {
        if (container.multipartType() == QMailMessagePart::MultipartNone)
            return inMultipartNone(container, ctx);
        if (container.multipartType() == QMailMessagePart::MultipartMixed)
            return inMultipartMixed(container, ctx);
        if (container.multipartType() == QMailMessagePart::MultipartRelated)
            return inMultipartRelated(container, ctx);
        if (container.multipartType() == QMailMessagePart::MultipartAlternative)
            return inMultipartAlternative(container, ctx);
        if (container.multipartType() == QMailMessagePart::MultipartSigned)
            return inMultipartSigned(container, ctx);

        // Not implemented multipartTypes ... 
        // default to handling like MultipartMixed
        return inMultipartMixed(container, ctx);
    }

}

namespace findAttachments
{
    class AttachmentFindStrategy;

    typedef QList<findAttachments::AttachmentFindStrategy*> AttachmentFindStrategies;
    typedef QList<QMailMessagePart::Location> Locations;

    class AttachmentFindStrategy
    {
    public:
        virtual ~AttachmentFindStrategy() {}
        // Returns true if the strategy was applyable to this kind of container
        virtual bool findAttachmentLocations(const QMailMessagePartContainer& container,
                                             Locations* found,
                                             bool *hasAttachments) const = 0;
    protected:
        AttachmentFindStrategy() { }
    };

    class DefaultAttachmentFindStrategy : public AttachmentFindStrategy
    {
    public:
        DefaultAttachmentFindStrategy() { }
        bool findAttachmentLocations(const QMailMessagePartContainer& container,
                                     Locations* found,
                                     bool* hasAttachments) const
        {
            if (hasAttachments) {
                *hasAttachments = false;
            }
            if (found) {
                found->clear();
            }

            if (container.multipartType() == QMailMessagePart::MultipartMixed) {
                inMultipartMixed(container, found, hasAttachments);
            }
            if (container.multipartType() == QMailMessagePart::MultipartAlternative) {
                inMultipartMixed(container, found, hasAttachments);
            }
            if (container.multipartType() == QMailMessagePart::MultipartSigned) {
                inMultipartSigned(container, found, hasAttachments);
            }

            // In any case, the default strategy wins, even if there are no attachments
            return true;
        }
    private:
        void inMultipartNone(const QMailMessagePart &part,
                             Locations* found,
                             bool* hasAttachments) const
        {
            QMailMessageContentType contentType = part.contentType();

            // Excluded text content-types.
            bool excludedText = (contentType.matches("text", "plain")
                                 || contentType.matches("text", "html")
                                 || contentType.matches("text", "calendar"));

            // Excluded application content-types even when
            // explicitly marked as attachment.
            bool excludedApp = (contentType.matches("application", "pgp-signature")
                                || contentType.matches("application", "pkcs7-signature"));

            bool isInLine = (!part.contentDisposition().isNull()) &&
                (part.contentDisposition().type() == QMailMessageContentDisposition::Inline);

            bool isAttachment = (!part.contentDisposition().isNull()) &&
                (part.contentDisposition().type() == QMailMessageContentDisposition::Attachment);

            bool isRFC822 = contentType.matches("message", "rfc822");

            // Attached messages are considered as attachments even if content disposition
            // is inline instead of attachment, but only if they aren't text/plain nor text/html
            if (isRFC822 || (isAttachment && !excludedApp)
                || (isInLine && !excludedText && !excludedApp)) {
                if (found) {
                    *found << part.location();
                }
                if (hasAttachments) {
                    *hasAttachments = true;
                }
            }
        }

        void inMultipartMixed(const QMailMessagePartContainer &container,
                              Locations* found,
                              bool* hasAttachments) const
        {
            for (uint i = 0; i < container.partCount(); i++) {
                const QMailMessagePart &part = container.partAt(i);
                switch (part.multipartType()) {
                case QMailMessagePart::MultipartNone:
                    inMultipartNone(part, found, hasAttachments);
                    break;
                case QMailMessagePart::MultipartMixed:
                case QMailMessagePart::MultipartAlternative:
                    inMultipartMixed(part, found, hasAttachments);
                    break;
                default:
                    break;
                }

                // We only want to know if there are attachments, not to really
                // get them, and we've already found one, so we break the loop
                if (!found && hasAttachments && *hasAttachments) {
                    break;
                }
            }
        }

        void inMultipartSigned(const QMailMessagePartContainer &container,
                               Locations* found,
                               bool* hasAttachments) const
        {
            for (uint i = 0; i < container.partCount(); i++) {
                const QMailMessagePart &part = container.partAt(i);
                switch (part.multipartType()) {
                case QMailMessagePart::MultipartNone:
                    inMultipartNone(part, found, hasAttachments);
                    break;
                default:
                    // Default to handling as MultipartMixed
                    inMultipartMixed(part, found, hasAttachments);
                    break;
                }
            }
        }
    };

    class TnefAttachmentFindStrategy : public AttachmentFindStrategy
    {
    public:
        TnefAttachmentFindStrategy() { }
        bool findAttachmentLocations(const QMailMessagePartContainer& container,
                                     Locations* found,
                                     bool* hasAttachments) const
        {
            if (hasAttachments) {
                *hasAttachments = false;
            }
            if (found) {
                found->clear();
            }

            if (container.headerFieldText(QLatin1String("X-MS-Has-Attach")).toLower() == QLatin1String("yes")) {
                if (hasAttachments) {
                    *hasAttachments = true;

                    // We only want to know if there are attachments, not to really
                    // get them, and we've already know that there are, we return
                    if (!found) {
                        return true;
                    }
                }
            } else {
                return false;
            }

            bool firstPartIsTextPlain = false;
            for (uint i = 0; i < container.partCount(); i++) {
                const QMailMessagePart &part = container.partAt(i);

                // Skip parts of the message body
                switch (i) {
                case 0:
                    if (part.contentType().matches("text", "plain")) {
                        firstPartIsTextPlain = true;
                        continue;
                    } else if (part.contentType().matches("text", "html")) {
                        continue;
                    }
                    break;
                case 1:
                    if (firstPartIsTextPlain && part.contentType().matches("text", "html")) {
                        continue;
                    }
                    break;
                default:
                    break;
                }

                // Mark not skipped single parts as attachments
                if (part.multipartType() == QMailMessagePart::MultipartNone) {
                    if (found) {
                        *found << part.location();
                    }
                }
            }

            return true;
        }
    };

    static const AttachmentFindStrategies allStrategies(AttachmentFindStrategies()
                                                  << new findAttachments::DefaultAttachmentFindStrategy());
}

namespace attachments
{
    static int maxDepth = 8;

    // Remove unnecessary parts of a message:
    // * empty containers,
    // * containers with a single child
    int cleanup(QMailMessagePartContainer &message, int depth)
    {
        // TODO: Check this cleanup code to see if it is applicable
        // to all the multipart types.
        if (message.multipartType() == QMailMessagePart::MultipartSigned ||
            message.multipartType() == QMailMessagePart::MultipartEncrypted) {
            // Do not mess with signed/encrypted containers.
            return -2;
        }
        if (depth > maxDepth) {
            qWarning() << Q_FUNC_INFO << "Maximum depth reached in message!!!";
            return -1;
        }
        int diff;
        int end = message.partCount();
        for (int i = 0; i < end; ++i) {
            QMailMessagePartContainer &part = message.partAt(i);
            switch (cleanup(part, depth + 1)) {
            case 1:
                // This part has only one child. Adopt the child and remove from the part.
                message.appendPart(part.partAt(0));
                part.clearParts();
                // Fall through.
            case 0:
                // This part is empty. Let's remove it.
                message.removePartAt(i);
                --i;
                diff = end - message.partCount();
                end -= diff;
                break;
            default:
                break;
            }
        }
        return message.partCount();
    }

    QMailMessagePart* partAt(QMailMessagePartContainer& container, QMailMessagePart::Location& location)
    {
        uint n = container.partCount();
        for (uint i = 0; i<n; i++) {
            QMailMessagePart& part = container.partAt(i);
            if (part.location().toString(false) == location.toString(false)) {
                return &part;
            }
        }
        return Q_NULLPTR;
    }

    void removeInlineImages(QMailMessagePartContainer &container, int depth)
    {
        if (container.multipartType() == QMailMessagePart::MultipartSigned ||
            container.multipartType() == QMailMessagePart::MultipartEncrypted) {
            // Do not mess with signed/encrypted containers.
            return;
        }
        if (depth > maxDepth) {
            qWarning() << Q_FUNC_INFO << "Maximum depth reached in message!!!";
            return;
        }
        int diff;
        int end = container.partCount();

        // Locations change when messages are removed, so better have a
        // list of pointers to the parts
        QList<QMailMessagePart*> imageParts;
        foreach (QMailMessagePart::Location location, container.findInlineImageLocations()) {
            QMailMessagePart *part = partAt(container, location);
            if (part) {
                imageParts << part;
            } else {
                qWarning() << Q_FUNC_INFO << "location"
                           << location.toString(true)
                           << "not found in container";
            }
        }

        for (int i = 0; i < end; ++i) {
            QMailMessagePart& part = container.partAt(i);
            if (imageParts.contains(&part)) {
                // Remove it.
                container.removePartAt(i);
                --i;
                // This is just a safeguard in case multiple parts are removed
                // starting at i.
                diff = end - container.partCount();
                end -= diff;
            } else {
                removeInlineImages(part, depth + 1);
            }
        }
    }

    void removeAttachments(QMailMessagePartContainer &message, int depth)
    {
        if (message.multipartType() == QMailMessagePart::MultipartSigned ||
            message.multipartType() == QMailMessagePart::MultipartEncrypted) {
            // Do not mess with signed/encrypted containers.
            return;
        }
        if (depth > maxDepth) {
            qWarning() << Q_FUNC_INFO << "Maximum depth reached in message!!!";
            return;
        }
        int diff;
        int end = message.partCount();

        // Locations change when messages are removed, so better have a
        // list of pointers to the parts
        QList<QMailMessagePart*> attachmentParts;
        foreach (QMailMessagePart::Location location, message.findAttachmentLocations()) {
            QMailMessagePart *part = partAt(message, location);
            if (part) {
                attachmentParts << part;
            } else {
                qWarning() << Q_FUNC_INFO << "location"
                           << location.toString(true)
                           << "not found in container";
            }
        }

        for (int i = 0; i < end; ++i) {
            QMailMessagePart& part = message.partAt(i);
            if (attachmentParts.contains(&part)) {
                // Remove it.
                message.removePartAt(i);
                --i;
                // This is just a safeguard in case multiple parts are removed
                // starting at i.
                diff = end - message.partCount();
                end -= diff;
            } else {
                removeAttachments(part, depth + 1);
            }
        }
    }

    void removeAllInlineImages(QMailMessagePartContainer &container)
    {
        removeInlineImages(container, 0);
        // Not sure if "cleaning up" is a good idea.
        //cleanup(message, 0);
    }

    void removeAll(QMailMessagePartContainer &message)
    {
        removeAttachments(message, 0);
        // Not sure if "cleaning up" is a good idea.
        //cleanup(message, 0);
    }

    void convertMessageToMultipart(QMailMessagePartContainer &message)
    {
        if (message.multipartType() != QMailMessagePartContainer::MultipartMixed) {
            // Convert the message to multipart/mixed.
            QMailMessagePart subpart;
            if (message.multipartType() == QMailMessagePartContainer::MultipartNone) {
                // Move the body
                subpart.setBody(message.body());
            } else {
                // Copy relevant data of the message to subpart
                subpart.setMultipartType(message.multipartType());
                for (uint i=0; i < message.partCount(); i++) {
                    subpart.appendPart(message.partAt(i));
                }
            }
            message.clearParts();
            message.setMultipartType(QMailMessagePartContainer::MultipartMixed);
            message.appendPart(subpart);
        }
    }

    void convertToMultipartRelated(QMailMessagePartContainer &message)
    {
        if (message.multipartType() != QMailMessagePartContainer::MultipartRelated) {
            // Convert the message to multipart/mixed.
            QMailMessagePart subpart;
            if (message.multipartType() == QMailMessagePartContainer::MultipartNone) {
                // Move the body
                subpart.setBody(message.body());
            } else {
                // Copy relevant data of the message to subpart
                subpart.setMultipartType(message.multipartType());
                for (uint i=0; i < message.partCount(); i++) {
                    subpart.appendPart(message.partAt(i));
                }
            }
            message.clearParts();
            message.setMultipartType(QMailMessagePartContainer::MultipartRelated);
            message.appendPart(subpart);
        }
    }

    void addImagesToMultipart(QMailMessagePartContainer *container, const QMap<QString, QString> &htmlImagesMap)
    {
        Q_ASSERT (NULL != container);
        Q_ASSERT (QMailMessagePartContainer::MultipartRelated == container->multipartType());

        foreach (const QString &imageID, htmlImagesMap) {

            const QString &imagePath = htmlImagesMap.value(imageID);
            const QFileInfo fi(imagePath);
            if (!fi.isFile()) {
                qWarning() << Q_FUNC_INFO << ":" << imagePath << "is not regular file. Cannot attach.";
                continue;
            }

            const QString &partName = fi.fileName();
            const QString &filePath = fi.absoluteFilePath();

            QMailMessageContentType attach_type(QMail::mimeTypeFromFileName(imagePath).toLatin1());
            attach_type.setName(partName.toLatin1());

            QMailMessageContentDisposition disposition(QMailMessageContentDisposition::Inline);
            disposition.setFilename(partName.toLatin1());
            disposition.setSize(fi.size());
            QMailMessagePart part = QMailMessagePart::fromFile(filePath, disposition, attach_type,
                                                               QMailMessageBody::Base64,
                                                               QMailMessageBody::RequiresEncoding);
            part.setContentID(imageID);
            container->appendPart(part);
        }
    }

    void addImagesToMultipart(QMailMessagePartContainer *container, const QList<const QMailMessagePart*> imageParts)
    {
        Q_ASSERT (NULL != container);
        Q_ASSERT (QMailMessagePartContainer::MultipartRelated == container->multipartType());

        foreach (const QMailMessagePart *imagePart, imageParts) {
            Q_ASSERT(NULL != imagePart);
            container->appendPart(*imagePart);
        }
    }

    void addAttachmentsToMultipart(QMailMessagePartContainer *container, const QStringList &attachmentPaths)
    {
        Q_ASSERT (NULL != container);
        Q_ASSERT (QMailMessagePartContainer::MultipartMixed == container->multipartType());

        bool addedSome = false;

        foreach (const QString &attachmentPath, attachmentPaths) {

            const QFileInfo fi(attachmentPath);
            if (!fi.isFile()) {
                qWarning() << Q_FUNC_INFO << ":" << attachmentPath << "is not regular file. Cannot attach.";
                continue;
            }

            const QString &filePath = fi.absoluteFilePath();

            QMailMessageContentType attach_type(QMail::mimeTypeFromFileName(attachmentPath).toLatin1());

            QMailMessageContentDisposition disposition(QMailMessageContentDisposition::Attachment);
            disposition.setSize(fi.size());

            QString input(fi.fileName());
            if (is7BitAscii(input)) {
                attach_type.setName(input.toLatin1());
                disposition.setFilename(input.toLatin1());
            } else {
                attach_type.setParameter("name*", QMailMessageContentDisposition::encodeParameter(input, "UTF-8"));
                disposition.setParameter("filename*", QMailMessageContentDisposition::encodeParameter(input, "UTF-8"));
            }

            container->appendPart(QMailMessagePart::fromFile(filePath, disposition,attach_type, QMailMessageBody::Base64,
                                                             QMailMessageBody::RequiresEncoding));
            addedSome = true;
        }

        QMailMessage* message = dynamic_cast<QMailMessage*>(container);
        if (message && addedSome) {
            message->setStatus(QMailMessage::HasAttachments, true);
        }
    }

    void addAttachmentsToMultipart(QMailMessagePartContainer *container,
                                   const QList<const QMailMessagePart*> attachmentParts)
    {
        Q_ASSERT (NULL != container);
        Q_ASSERT (QMailMessagePartContainer::MultipartMixed == container->multipartType());

        bool addedSome = false;

        foreach (const QMailMessagePart *attachmentPart, attachmentParts) {
            Q_ASSERT(NULL != attachmentPart);
            container->appendPart(*attachmentPart);
            addedSome = true;
        }

        QMailMessage* message = dynamic_cast<QMailMessage*>(container);
        if (message && addedSome) {
            message->setStatus(QMailMessage::HasAttachments, true);
        }
    }
}

/* QMailMessageHeaderField */

QMailMessageHeaderFieldPrivate::QMailMessageHeaderFieldPrivate()
    : QPrivateImplementationBase(this),
      _structured(true)
{
}

QMailMessageHeaderFieldPrivate::QMailMessageHeaderFieldPrivate(const QByteArray& text, bool structured)
    : QPrivateImplementationBase(this)
{
    parse(text, structured);
}

QMailMessageHeaderFieldPrivate::QMailMessageHeaderFieldPrivate(const QByteArray& id, const QByteArray& text, bool structured)
    : QPrivateImplementationBase(this)
{
    _id = id;
    parse(text, structured);
}

static bool validExtension(const QByteArray& trailer, int* number = Q_NULLPTR, bool* encoded = Q_NULLPTR)
{
    // Extensions according to RFC 2231:
    QRegExp extensionFormat(QLatin1String("(?:\\*(\\d+))?(\\*?)"));
    if (extensionFormat.exactMatch(QString::fromUtf8(trailer)))
    {
        if (number)
            *number = extensionFormat.cap(1).toInt();
        if (encoded)
            *encoded = !extensionFormat.cap(2).isEmpty();

        return true;
    }
    else
        return false;
}

static bool matchingParameter(const QByteArray& name, const QByteArray& other, bool* encoded = Q_NULLPTR)
{
    QByteArray match(name.trimmed());

    int index = insensitiveIndexOf(match, other);
    if (index == -1)
        return false;

    if (index > 0)
    {
        // Ensure that every preceding character is whitespace
        QByteArray leader(other.left(index).trimmed());
        if (!leader.isEmpty())
            return false;
    }

    int lastIndex = index + match.length() - 1;
    index = other.indexOf('=', lastIndex);
    if (index == -1)
        index = other.length();

    // Ensure that there is only whitespace between the matched name and the end of the name
    if ((index - lastIndex) > 1)
    {
        QByteArray trailer(other.mid(lastIndex + 1, (index - lastIndex)).trimmed());
        if (!trailer.isEmpty())
            return validExtension(trailer, Q_NULLPTR, encoded);
    }

    return true;
}

void QMailMessageHeaderFieldPrivate::addParameter(const QByteArray& name, const QByteArray& value)
{
    _parameters.append(qMakePair(name, QMail::unquoteString(value)));
}

void QMailMessageHeaderFieldPrivate::parse(const QByteArray& text, bool structured)
{
    _structured = structured;

    // Parse text into main and params
    const char* const begin = text.constData();
    const char* const end = begin + text.length();

    bool malformed = false;

    const char* token = begin;
    const char* it = begin;
    const char* separator = Q_NULLPTR;
    for (bool quoted = false; it != end; ++it)
    {
        if (*it == '"') {
            quoted = !quoted;
        }
        else if (*it == ':' && !quoted && token == begin) {
            // This is the end of the field id
            if (_id.isEmpty()) {
                _id = QByteArray(token, (it - token)).trimmed();
                token = (it + 1);
            }
            else if (_structured) {
                // If this is a structured header, there can be only one colon
                token = (it + 1);
            }
        }
        else if (*it == '=' && !quoted && structured) {
            if (separator == Q_NULLPTR) {
                // This is a parameter separator
                separator = it;
            }
            else  {
                // It would be nice to identify extra '=' chars, but it's too hard
                // to separate them from encoded-word formations...
                //malformed = true;
            }
        }
        else if (*it == ';' && !quoted && structured) {
            // This is the end of a token
            if (_content.isEmpty()) {
                _content = QByteArray(token, (it - token)).trimmed();
            }
            else if ((separator > token) && ((separator + 1) < it)) {
                QByteArray name = QByteArray(token, (separator - token)).trimmed();
                QByteArray value = QByteArray(separator + 1, (it - separator - 1)).trimmed();

                if (!name.isEmpty() && !value.isEmpty())
                    addParameter(name, value);
            }
            else {
                malformed = true;
            }

            token = (it + 1);
            separator = Q_NULLPTR;
        }
    }

    if (token != end) {
        if (_id.isEmpty()) {
            _id = QByteArray(token, (end - token)).trimmed();
        }
        else if (_content.isEmpty()) {
            _content = QByteArray(token, (end - token)).trimmed();
        }
        else if ((separator > token) && ((separator + 1) < end) && !malformed) {
            QByteArray name = QByteArray(token, (separator - token)).trimmed();
            QByteArray value = QByteArray(separator + 1, (end - separator - 1)).trimmed();

            if (!name.isEmpty() && !value.isEmpty())
                addParameter(name, value);
        }
        else if (_structured) {
            malformed = true;
        }
    }
}

bool QMailMessageHeaderFieldPrivate::operator== (const QMailMessageHeaderFieldPrivate& other) const
{
    if (!insensitiveEqual(_id, other._id))
        return false;
    
    if (_content != other._content)
        return false;

    if (_parameters.count() != other._parameters.count())
        return false;

    QList<QMailMessageHeaderField::ParameterType>::const_iterator it = _parameters.begin(), end = _parameters.end();
    QList<QMailMessageHeaderField::ParameterType>::const_iterator oit = other._parameters.begin();
    for ( ; it != end; ++it, ++oit)
        if (((*it).first != (*oit).first) || ((*it).second != (*oit).second))
            return false;

    return true;
}

bool QMailMessageHeaderFieldPrivate::isNull() const
{
    return (_id.isNull() && _content.isNull());
}

QByteArray QMailMessageHeaderFieldPrivate::id() const
{
    return _id;
}

void QMailMessageHeaderFieldPrivate::setId(const QByteArray& text)
{
    _id = text;
}

QByteArray QMailMessageHeaderFieldPrivate::content() const
{
    return _content;
}

void QMailMessageHeaderFieldPrivate::setContent(const QByteArray& text)
{
    _content = text;
}

QByteArray QMailMessageHeaderFieldPrivate::parameter(const QByteArray& name) const
{
    // Coalesce folded parameters into a single return value
    QByteArray result;

    QByteArray param = name.trimmed();
    foreach (const QMailMessageContentType::ParameterType& parameter, _parameters) {
        if (matchingParameter(param, parameter.first))
            result.append(parameter.second);
    }

    return result;
}

void QMailMessageHeaderFieldPrivate::setParameter(const QByteArray& name, const QByteArray& value)
{
    if (!_structured)
        return;

    QByteArray param = name.trimmed();

    bool encoded = false;
    int index = param.indexOf('*');
    if (index != -1) {
        encoded = true;
        param = param.left(index);
    }

    // Find all existing parts of this parameter, if present
    QList<QList<QMailMessageHeaderField::ParameterType>::iterator> matches;
    QList<QMailMessageHeaderField::ParameterType>::iterator it = _parameters.begin(), end = _parameters.end();
    for ( ; it != end; ++it) {
        if (matchingParameter(param, (*it).first))
            matches.prepend(it);
    }

    while (matches.count() > 1)
        _parameters.erase(matches.takeFirst());
    if (matches.count() == 1)
        it = matches.takeFirst();
    
    // If the value is too long to fit on one line, break it into manageable pieces
    const int maxInputLength = 78 - 9 - param.length();

    if (value.length() > maxInputLength) {
        // We have multiple pieces to insert
        QList<QByteArray> pieces;
        QByteArray input(value);
        do
        {
            int splitPoint = maxInputLength;
            if (encoded && input.length() > maxInputLength) {
                int percentPosition = input.indexOf("%", maxInputLength - 2);
                if (percentPosition != -1 && percentPosition < maxInputLength)
                    splitPoint = percentPosition;
            }
            pieces.append(input.left(splitPoint));
            input = input.mid(splitPoint);
        } while (input.length());

        if (it == end) {
            // Append each piece at the end
            int n = 0;
            while (pieces.count() > 0) {
                QByteArray id(param);
                id.append('*').append(QByteArray::number(n));
                if (encoded)
                    id.append('*');

                _parameters.append(qMakePair(id, pieces.takeFirst()));
                ++n;
            }
        }
        else {
            // Overwrite the remaining instance of the parameter, and place any 
            // following pieces immediately after
            int n = pieces.count() - 1;
            int initial = n;

            while (pieces.count() > 0) {
                QByteArray id(param);
                id.append('*').append(QByteArray::number(n));
                if (encoded && (n == 0))
                    id.append('*');

                QMailMessageHeaderField::ParameterType parameter = qMakePair(id, pieces.takeLast());
                if (n == initial) {
                    // Put the last piece into the existing position
                    (*it) = parameter;
                }
                else {
                    // Insert before the previous piece, and record the new iterator
                    it = _parameters.insert(it, parameter);
                }

                --n;
            }
        }
    }
    else {
        // Just one part to insert
        QByteArray id(param);
        if (encoded)
            id.append('*');
        QMailMessageHeaderField::ParameterType parameter = qMakePair(id, value);

        if (it == end) {
            _parameters.append(parameter);
        }
        else {
            (*it) = parameter;
        }
    }
}

bool QMailMessageHeaderFieldPrivate::isParameterEncoded(const QByteArray& name) const
{
    QByteArray param = name.trimmed();

    bool encoded = false;
    foreach (const QMailMessageContentType::ParameterType& parameter, _parameters)
        if (matchingParameter(param, parameter.first, &encoded))
            return encoded;

    return false;
}

void QMailMessageHeaderFieldPrivate::setParameterEncoded(const QByteArray& name)
{
    QByteArray param = name.trimmed();

    QList<QMailMessageHeaderField::ParameterType>::iterator it = _parameters.begin(), end = _parameters.end();
    for ( ; it != end; ++it) {
        bool encoded = false;
        if (matchingParameter(param, (*it).first, &encoded)) {
            if (!encoded)
                (*it).first.append('*');
        }
    }
}

static QByteArray protectedParameter(const QByteArray& value)
{
    QRegExp whitespace(QLatin1String("\\s+"));
    QRegExp tspecials = QRegExp(QLatin1String("[<>\\[\\]\\(\\)\\?:;@\\\\,=]"));

    QString valueUtf8(QString::fromUtf8(value));
    if ((whitespace.indexIn(valueUtf8) != -1) ||
        (tspecials.indexIn(valueUtf8) != -1))
        return QMail::quoteString(value);
    else
        return value;
}

static bool extendedParameter(const QByteArray& name, QByteArray* truncated = Q_NULLPTR, int* number = Q_NULLPTR, bool* encoded = Q_NULLPTR)
{
    QByteArray param(name.trimmed());

    int index = param.indexOf('*');
    if (index == -1)
        return false;

    if (truncated)
        *truncated = param.left(index).trimmed();

    return validExtension(param.mid(index), number, encoded);
}

QList<QMailMessageHeaderField::ParameterType> QMailMessageHeaderFieldPrivate::parameters() const
{
    QList<QMailMessageHeaderField::ParameterType> result;

    foreach (const QMailMessageContentType::ParameterType& param, _parameters) {
        QByteArray id;
        int number;
        if (extendedParameter(param.first, &id, &number)) {
            if (number == 0) {
                result.append(qMakePair(id, parameter(id)));
            }
        }
        else {
            result.append(param);
        }
    }

    return result;
}

QByteArray QMailMessageHeaderFieldPrivate::toString(bool includeName, bool presentable) const
{
    if (_id.isEmpty())
        return QByteArray();

    QByteArray result;
    if (includeName) {
        result = _id + ":";
    }
    
    if (!_content.isEmpty()) {
        if (includeName)
            result += ' ';
        result += _content;
    }

    if (_structured)
    {
        foreach (const QMailMessageContentType::ParameterType& parameter, (presentable ? parameters() : _parameters))
            result.append("; ").append(parameter.first).append('=').append(protectedParameter(parameter.second));
    }

    return result;
}

static void outputHeaderPart(QDataStream& out, const QByteArray& inText, int* lineLength, const int maxLineLength)
{
    const int maxHeaderLength(10000);
    QByteArray text(inText);
    QRegExp whitespace(QLatin1String("\\s"));
    QRegExp syntacticBreak(QLatin1String(";|,"));

    if (text.length() > maxHeaderLength) {
        qWarning() << "Maximum header length exceeded, truncating mail header";
        text.truncate(maxHeaderLength);
    }

    while (true) {
        int remaining = maxLineLength - *lineLength;
        if (text.length() <= remaining) {
            out << DataString(text);
            *lineLength += text.length();
            return;
        } else {
            // See if we can find suitable whitespace to break the line
            int wsIndex = -1;
            int lastIndex = -1;
            int preferredIndex = -1;
            bool syntacticBreakUsed = false;
            do {
                lastIndex = wsIndex;
                if ((lastIndex > 0) 
                   && ((text[lastIndex - 1] == ';') || (text[lastIndex - 1] == ','))) {
                    // Prefer to split after (possible) parameters and commas
                    preferredIndex = lastIndex;
                }

                wsIndex = whitespace.indexIn(QString::fromUtf8(text), wsIndex + 1);
            } while ((wsIndex != -1) && (wsIndex < remaining));

            if (preferredIndex != -1)
                lastIndex = preferredIndex;

            if (lastIndex == -1) {
                // We couldn't find any suitable whitespace, look for high-level syntactic break
                // allow a maximum of 998 characters excl CRLF on a line without white space
                remaining = 997 - *lineLength;
                int syntacticIn = -1;
                do {
                    lastIndex = syntacticIn;
                    syntacticIn = syntacticBreak.indexIn(QString::fromUtf8(text), syntacticIn + 1);
                } while ((syntacticIn != -1) && (syntacticIn < remaining - 1));

                if (lastIndex != -1) {
                    syntacticBreakUsed = true;
                    ++lastIndex;
                } else {
                    // We couldn't find any high-level syntactic break either - just break at the last char
                    //qWarning() << "Unable to break header field at white space or syntactic break";
                    lastIndex = remaining;
                }
            }

            if (lastIndex == 0) {
                out << DataString('\n') << DataString(text[0]);
                *lineLength = 1;
                lastIndex = 1;
            } else {
                out << DataString(text.left(lastIndex)) << DataString('\n');

                if ((lastIndex == remaining) || (syntacticBreakUsed)) {
                    // We need to insert some artifical whitespace
                    out << DataString(' ');
                } else {
                    // Append the breaking whitespace (ensure it does not get CRLF-ified)
                    out << DataString(QByteArray(1, text[lastIndex]));
                    ++lastIndex;
                }

                *lineLength = 1;
            }

            text = text.mid(lastIndex);
            if (text.isEmpty()) {
                return;
            }
        }
    }
}

void QMailMessageHeaderFieldPrivate::output(QDataStream& out) const
{
    static const int maxLineLength = 78;

    if (_id.isEmpty())
        return;

    if (_structured) {
        qWarning() << "Unable to output structured header field:" << _id;
        return;
    }

    QByteArray element(_id);
    element.append(':');
    out << DataString(element);

    if (!_content.isEmpty()) {
        int lineLength = element.length();
        outputHeaderPart(out, ' ' + _content, &lineLength, maxLineLength);
    }

    out << DataString('\n');
}

static bool parameterEncoded(const QByteArray& name)
{
    QByteArray param(name.trimmed());
    if (param.isEmpty())
        return false;

    return (param[param.length() - 1] == '*');
}

QString QMailMessageHeaderFieldPrivate::decodedContent() const
{
    QString result(QMailMessageHeaderField::decodeContent(_content));

    if (_structured)
    {
        foreach (const QMailMessageContentType::ParameterType& parameter, _parameters) {
            QString decoded;
            if (parameterEncoded(parameter.first))
                decoded = QMailMessageHeaderField::decodeParameter(protectedParameter(parameter.second));
            else
                decoded = QString::fromUtf8(protectedParameter(parameter.second));
            result.append(QLatin1String("; ")).append(QLatin1String(parameter.first)).append(QChar::fromLatin1('=')).append(decoded);
        }
    }

    return result;
}

template <typename Stream> 
void QMailMessageHeaderFieldPrivate::serialize(Stream &stream) const
{
    stream << _id;
    stream << _content;
    stream << _structured;
    stream << _parameters;
}

template <typename Stream> 
void QMailMessageHeaderFieldPrivate::deserialize(Stream &stream)
{
    stream >> _id;
    stream >> _content;
    stream >> _structured;
    stream >> _parameters;
}


/*!
    \class QMailMessageHeaderField

    \preliminary
    \brief The QMailMessageHeaderField class encapsulates the parsing of message header fields.
    
    \ingroup messaginglibrary
   
    QMailMessageHeaderField provides simplified access to the various components of the 
    header field, and allows the field content to be extracted in a standardized form.

    The content of a header field may be formed of unstructured text, or it may have an 
    internal structure.  If a structured field is specified, QMailMessageHeaderField assumes 
    that the contained header field is structured in a format equivalent to that used for the 
    RFC 2045 'Content-Type' and RFC 2183 'Content-Disposition' header fields.  If the field 
    is unstructured, or conforms to a different structure, then the parameter() and parameters() functions
    will return empty results, and the setParameter() function will have no effect.

    QMailMessageHeaderField contains static functions to assist in creating correct
    header field content, and presenting header field content.  The encodeWord() and 
    decodeWord() functions translate between plain text and the encoded-word specification
    defined in RFC 2045.  The encodeParameter() and decodeParameter() functions translate
    between plain text and the encoded-parameter format defined in RFC 2231.

    The removeWhitespace() function can be used to remove irrelevant whitespace characters
    from a string, and the removeComments() function can remove any comment sequences 
    present, encododed according to the RFC 2822 specification.
*/

/*!
    \typedef QMailMessageHeaderField::ImplementationType
    \internal
*/

/*!
    \typedef QMailMessageHeaderField::ParameterType
    \internal
*/

/*!
    Creates an uninitialised message header field object.
*/
QMailMessageHeaderField::QMailMessageHeaderField()
    : QPrivatelyImplemented<QMailMessageHeaderFieldPrivate>(new QMailMessageHeaderFieldPrivate())
{
}

/*!
    Creates a message header field object from the data in \a text. If \a fieldType is 
    QMailMessageHeaderField::StructuredField, then \a text will be parsed assuming a 
    format equivalent to that used for the RFC 2045 'Content-Type' and 
    RFC 2183 'Content-Disposition' header fields.
*/
QMailMessageHeaderField::QMailMessageHeaderField(const QByteArray& text, FieldType fieldType)
    : QPrivatelyImplemented<QMailMessageHeaderFieldPrivate>(new QMailMessageHeaderFieldPrivate(text, (fieldType == StructuredField)))
{
}

/*!
    Creates a message header field object with the field id \a id and the content 
    data in \a text.  If \a fieldType is QMailMessageHeaderField::StructuredField, 
    then \a text will be parsed assuming a format equivalent to that used for the 
    RFC 2045 'Content-Type' and RFC 2183 'Content-Disposition' header fields.
*/
QMailMessageHeaderField::QMailMessageHeaderField(const QByteArray& id, const QByteArray& text, FieldType fieldType)
    : QPrivatelyImplemented<QMailMessageHeaderFieldPrivate>(new QMailMessageHeaderFieldPrivate(id, text, (fieldType == StructuredField)))
{
}

/*! \internal */
bool QMailMessageHeaderField::operator== (const QMailMessageHeaderField& other) const
{
    return impl(this)->operator==(*other.impl(&other));
}

/*!
    Returns true if the header field has not been initialized.
*/
bool QMailMessageHeaderField::isNull() const
{
    return impl(this)->isNull();
}

/*!
    Returns the ID of the header field.
*/
QByteArray QMailMessageHeaderField::id() const
{
    return impl(this)->id();
}

/*!
    Sets the ID of the header field to \a id.
*/
void QMailMessageHeaderField::setId(const QByteArray& id)
{
    impl(this)->setId(id);
}

/*!
    Returns the content of the header field, without any associated parameters.
*/
QByteArray QMailMessageHeaderField::content() const
{
    return impl(this)->content();
}

/*!
    Sets the content of the header field to \a text.
*/
void QMailMessageHeaderField::setContent(const QByteArray& text)
{
    impl(this)->setContent(text);
}

/*!
    Returns the value of the parameter with the name \a name.  
    Name comparisons are case-insensitive.
*/
QByteArray QMailMessageHeaderField::parameter(const QByteArray& name) const
{
    return impl(this)->parameter(name);
}

/*!
    Sets the parameter with the name \a name to have the value \a value, if present; 
    otherwise a new parameter is appended with the supplied properties.  If \a name
    ends with a single asterisk, the parameter will be flagged as encoded.

    \sa setParameterEncoded()
*/
void QMailMessageHeaderField::setParameter(const QByteArray& name, const QByteArray& value)
{
    impl(this)->setParameter(name, value);
}

/*!
    Returns true if the parameter with name \a name exists and is marked as encoded 
    according to RFC 2231; otherwise returns false.  
    Name comparisons are case-insensitive.
*/
bool QMailMessageHeaderField::isParameterEncoded(const QByteArray& name) const
{
    return impl(this)->isParameterEncoded(name);
}

/*!
    Sets any parameters with the name \a name to be marked as encoded.
    Name comparisons are case-insensitive.
*/
void QMailMessageHeaderField::setParameterEncoded(const QByteArray& name)
{
    impl(this)->setParameterEncoded(name);
}

/*!
    Returns the list of parameters from the header field. For each parameter, the
    member \c first contains the name text, and the member \c second contains the value text.
*/
QList<QMailMessageHeaderField::ParameterType> QMailMessageHeaderField::parameters() const
{
    return impl(this)->parameters();
}

/*!
    Returns the entire header field text as a formatted string, with the name of the field
    included if \a includeName is true.  If \a presentable is true, artifacts of RFC 2822 
    transmission format such as parameter folding will be removed.  For example: 
    
    \code
    QMailMessageHeaderField hdr;
    hdr.setId("Content-Type");
    hdr.setContent("text/plain");
    hdr.setParameter("charset", "us-ascii");

    QString s = hdr.toString();  // s: "Content-Type: text/plain; charset=us-ascii"
    \endcode
*/
QByteArray QMailMessageHeaderField::toString(bool includeName, bool presentable) const
{
    return impl(this)->toString(includeName, presentable);
}

/*!
    Returns the content of the header field as unicode text.  If the content of the
    field contains any encoded-word or encoded-parameter values, they will be decoded on output.
*/
QString QMailMessageHeaderField::decodedContent() const
{
    return impl(this)->decodedContent();
}

/*! \internal */
void QMailMessageHeaderField::parse(const QByteArray& text, FieldType fieldType)
{
    return impl(this)->parse(text, (fieldType == StructuredField));
}

/*!
    Returns the content of the string \a input encoded into a series of RFC 2045 'encoded-word'
    format tokens, each no longer than 75 characters.  The encoding used can be specified in 
    \a charset, or can be deduced from the content of \a input if \a charset is empty.
*/
QByteArray QMailMessageHeaderField::encodeWord(const QString& input, const QByteArray& charset)
{
    return ::encodeWord(input, charset, Q_NULLPTR);
}

/*!
    Returns the content of \a input decoded from RFC 2045 'encoded-word' format.
*/
QString QMailMessageHeaderField::decodeWord(const QByteArray& input)
{
    // This could actually be a sequence of encoded words...
    return decodeWordSequence(input);
}

/*!
    Returns the content of the string \a input encoded into RFC 2231 'extended-parameter'
    format.  The encoding used can be specified in \a charset, or can be deduced from the 
    content of \a input if \a charset is empty.  If \a language is non-empty, it will be 
    included in the encoded output; otherwise the language component will be extracted from 
    \a charset, if it contains a trailing language specifier as defined in RFC 2231.
*/
QByteArray QMailMessageHeaderField::encodeParameter(const QString& input, const QByteArray& charset, const QByteArray& language)
{
    return ::encodeParameter(input, charset, language);
}

/*!
    Returns the content of \a input decoded from RFC 2231 'extended-parameter' format.
*/
QString QMailMessageHeaderField::decodeParameter(const QByteArray& input)
{
    return ::decodeParameter(input);
}

/*!
    Returns the content of the string \a input encoded into a sequence of RFC 2045 'encoded-word'
    format tokens.  The encoding used can be specified in \a charset, or can be deduced for each
    token read from \a input if \a charset is empty.
*/
QByteArray QMailMessageHeaderField::encodeContent(const QString& input, const QByteArray& charset)
{
    return encodeWordSequence(input, charset);
}

/*!
    Returns the content of \a input, decoding any encountered RFC 2045 'encoded-word' format
    tokens to unicode.
*/
QString QMailMessageHeaderField::decodeContent(const QByteArray& input)
{
    return decodeWordSequence(input);
}

/*!
    Returns the content of \a input with any comment sections removed.
*/
QByteArray QMailMessageHeaderField::removeComments(const QByteArray& input)
{
    return ::removeComments(input, &::isprint);
}

/*!
    Returns the content of \a input with any whitespace characters removed. 
    Whitespace inside double quotes is preserved.
*/
QByteArray QMailMessageHeaderField::removeWhitespace(const QByteArray& input)
{
    QByteArray result;
    result.reserve(input.length());

    const char* const begin = input.constData();
    const char* const end = begin + input.length();
    const char* it = begin;
    for (bool quoted = false; it != end; ++it) {
        if (*it == '"') {
            if ((it == begin) || (*(it - 1) != '\\'))
                quoted = !quoted;
        }
        if (quoted || !isspace(*it))
            result.append(*it);
    }
    
    return result;
}

/*! \internal */
void QMailMessageHeaderField::output(QDataStream& out) const
{
    impl(this)->output(out);
}

/*! 
    \fn QMailMessageHeaderField::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailMessageHeaderField::serialize(Stream &stream) const
{
    impl(this)->serialize(stream);
}

/*! 
    \fn QMailMessageHeaderField::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessageHeaderField::deserialize(Stream &stream)
{
    impl(this)->deserialize(stream);
}


/*!
    \class QMailMessageContentType

    \preliminary
    \brief The QMailMessageContentType class encapsulates the parsing of the RFC 2822
    'Content-Type' header field.
    
    \ingroup messaginglibrary
   
    QMailMessageContentType provides simplified access to the various components of the 
    'Content-Type' header field.
    Components of the header field not exposed by member functions can be accessed using
    the functions inherited from QMailMessageHeaderField.
*/

/*! \internal */
QMailMessageContentType::QMailMessageContentType()
    : QMailMessageHeaderField("Content-Type")
{
}

/*!
    Creates a content type object from the data in \a type.
*/
QMailMessageContentType::QMailMessageContentType(const QByteArray& type)
    : QMailMessageHeaderField("Content-Type")
{
    // Find the components, and create a content value from them
    QByteArray content;

    // Although a conforming CT must be: <type> "/" <subtype> without whitespace,
    // we'll be a bit more accepting
    int index = type.indexOf('/');
    if (index == -1)
    {
        content = type.trimmed();
    }
    else
    {
        QByteArray primaryType = type.left(index).trimmed();
        QByteArray secondaryType = type.mid(index + 1).trimmed();

        content = primaryType;
        if (!secondaryType.isEmpty())
            content.append('/').append(secondaryType);
    }

    parse(content, StructuredField);
}

/*!
    Creates a content type object from the content of \a field.
*/
QMailMessageContentType::QMailMessageContentType(const QMailMessageHeaderField& field)
    : QMailMessageHeaderField(field)
{
    QMailMessageHeaderField::setId("Content-Type");
}

/*!
    Returns the primary type information of the content type header field.

    For example: if content() returns "text/plain", then type() returns "text"
*/
QByteArray QMailMessageContentType::type() const
{
    QByteArray entire = content();
    int index = entire.indexOf('/');
    if (index == -1)
        return entire.trimmed();

    return entire.left(index).trimmed();
}

/*!
    Sets the primary type information of the 'Content-Type' header field to \a type. If \a type 
    is empty, then any pre-existing sub-type information will be cleared.

    \sa setSubType()
*/
void QMailMessageContentType::setType(const QByteArray& type)
{
    if (type.isEmpty()) {
        // Note - if there is a sub-type, setting type to null will destroy it
        setContent(type);
    } else if (type.contains(';') || type.contains('/')) {
        qWarning() << Q_FUNC_INFO << "wrong usage of setType(), consider using setSubType() or setParameter()" << type;

    } else {
        QByteArray content(type);

        QByteArray secondaryType(subType());
        if (!secondaryType.isEmpty())
            content.append('/').append(secondaryType);

        setContent(content);
    }
}

/*!
    Returns the sub-type information of the 'Content-Type' header field.

    For example: if content() returns "text/plain", then subType() returns "plain"
*/
QByteArray QMailMessageContentType::subType() const
{
    QByteArray entire = content();
    int index = entire.indexOf('/');
    if (index == -1)
        return QByteArray();

    return entire.mid(index + 1).trimmed();
}

/*!
    Sets the sub-type information of the 'Content-Type' header field to \a subType. If no primary
    type has been set, then setting the sub-type has no effect.

    \sa setType()
*/
void QMailMessageContentType::setSubType(const QByteArray& subType)
{
    QByteArray primaryType(type());
    if (!primaryType.isEmpty())
    {
        if (!subType.isEmpty())
            primaryType.append('/').append(subType);

        setContent(primaryType);
    }
}

/*!
    Returns the value of the 'name' parameter, if present; otherwise returns an empty QByteArray.
*/
QByteArray QMailMessageContentType::name() const
{
    return parameter("name");
}

/*!
    Sets the value of the 'name' parameter to \a name.
*/
void QMailMessageContentType::setName(const QByteArray& name)
{
    setParameter("name", name);
}

/*!
    Returns the value of the 'boundary' parameter, if present; otherwise returns an empty QByteArray.
*/
QByteArray QMailMessageContentType::boundary() const
{
    QByteArray value = parameter("boundary");
    if (value.isEmpty() || !isParameterEncoded("boundary"))
        return value;

    // The boundary is an encoded parameter.  Therefore, we need to extract the
    // usable ascii part, since a valid message must be composed of ascii only
    return to7BitAscii(QMailMessageHeaderField::decodeParameter(value));
}

/*!
    Sets the value of the 'boundary' parameter to \a boundary.
*/
void QMailMessageContentType::setBoundary(const QByteArray& boundary)
{
    setParameter("boundary", boundary);
}

/*!
    Returns the value of the 'charset' parameter, if present; otherwise returns an empty QByteArray.
*/
QByteArray QMailMessageContentType::charset() const
{
    QByteArray value = parameter("charset");
    if (value.isEmpty() || !isParameterEncoded("charset"))
        return value;

    // The boundary is an encoded parameter.  Therefore, we need to extract the
    // usable ascii part, since a valid charset must be composed of ascii only
    return to7BitAscii(QMailMessageHeaderField::decodeParameter(value));
}

/*!
    Sets the value of the 'charset' parameter to \a charset.
*/
void QMailMessageContentType::setCharset(const QByteArray& charset)
{
    setParameter("charset", charset);
}

/*!
    Allow to test if the content type matches a specific "type / subtype" string.
*/
bool QMailMessageContentType::matches(const QByteArray& primary, const QByteArray& sub) const
{
    // Content type is not encoded, we can use qstricmp directly.
    return ((primary.isEmpty() || qstricmp(type(), primary) == 0)
            && (sub.isEmpty() || qstricmp(subType(), sub) == 0));
}

/*!
    \class QMailMessageContentDisposition

    \preliminary
    \brief The QMailMessageContentDisposition class encapsulates the parsing of the RFC 2822
    'Content-Disposition' header field.
    
    \ingroup messaginglibrary
   
    QMailMessageContentDisposition provides simplified access to the various components of the 
    'Content-Disposition' header field.
    Components of the header field not exposed by member functions can be accessed using
    the functions inherited from QMailMessageHeaderField.
*/

/*! \internal */
QMailMessageContentDisposition::QMailMessageContentDisposition()
    : QMailMessageHeaderField("Content-Disposition")
{
}

/*!
    Creates a disposition header field object from the data in \a type.
*/
QMailMessageContentDisposition::QMailMessageContentDisposition(const QByteArray& type)
    : QMailMessageHeaderField("Content-Disposition", type)
{
}

/*!
    Creates a 'Content-Disposition' header field object with the type \a type.
*/
QMailMessageContentDisposition::QMailMessageContentDisposition(QMailMessageContentDisposition::DispositionType type)
    : QMailMessageHeaderField("Content-Disposition")
{
    setType(type);
}

/*!
    Creates a disposition header field object from the content of \a field.
*/
QMailMessageContentDisposition::QMailMessageContentDisposition(const QMailMessageHeaderField& field)
    : QMailMessageHeaderField(field)
{
    QMailMessageHeaderField::setId("Content-Disposition");
}

/*!
    Returns the disposition type of this header field.
*/
QMailMessageContentDisposition::DispositionType QMailMessageContentDisposition::type() const
{
    const QByteArray& type = content();

    if (insensitiveEqual(type, "inline"))
        return Inline;
    else if (insensitiveEqual(type, "attachment"))
        return Attachment;

    return None;
}

/*!
    Sets the disposition type of this field to \a type.
*/
void QMailMessageContentDisposition::setType(QMailMessageContentDisposition::DispositionType type)
{
    if (type == Inline)
        setContent("inline");
    else if (type == Attachment)
        setContent("attachment");
    else
        setContent(QByteArray());
}

/*!
    Returns the value of the 'filename' parameter, if present; otherwise returns an empty QByteArray.
*/
QByteArray QMailMessageContentDisposition::filename() const
{
    return parameter("filename");
}

/*!
    Sets the value of the 'filename' parameter to \a filename.
*/
void QMailMessageContentDisposition::setFilename(const QByteArray& filename)
{
    setParameter("filename", filename);
}

/*!
    Returns the value of the 'creation-date' parameter, if present; otherwise returns an uninitialised time stamp.
*/
QMailTimeStamp QMailMessageContentDisposition::creationDate() const
{
    return QMailTimeStamp(QString::fromUtf8(parameter("creation-date")));
}

/*!
    Sets the value of the 'creation-date' parameter to \a timeStamp.
*/
void QMailMessageContentDisposition::setCreationDate(const QMailTimeStamp& timeStamp)
{
    setParameter("creation-date", to7BitAscii(timeStamp.toString()));
}

/*!
    Returns the value of the 'modification-date' parameter, if present; otherwise returns an uninitialised time stamp.
*/
QMailTimeStamp QMailMessageContentDisposition::modificationDate() const
{
    return QMailTimeStamp(QString::fromUtf8(parameter("modification-date")));
}

/*!
    Sets the value of the 'modification-date' parameter to \a timeStamp.
*/
void QMailMessageContentDisposition::setModificationDate(const QMailTimeStamp& timeStamp)
{
    setParameter("modification-date", to7BitAscii(timeStamp.toString()));
}


/*!
    Returns the value of the 'read-date' parameter, if present; otherwise returns an uninitialised time stamp.
*/
QMailTimeStamp QMailMessageContentDisposition::readDate() const
{
    return QMailTimeStamp(QString::fromUtf8(parameter("read-date")));
}

/*!
    Sets the value of the 'read-date' parameter to \a timeStamp.
*/
void QMailMessageContentDisposition::setReadDate(const QMailTimeStamp& timeStamp)
{
    setParameter("read-date", to7BitAscii(timeStamp.toString()));
}

/*!
    Returns the value of the 'size' parameter, if present; otherwise returns -1.
*/
int QMailMessageContentDisposition::size() const
{
    QByteArray sizeText = parameter("size");

    if (sizeText.isEmpty())
        return -1;

    return sizeText.toUInt();
}

/*!
    Sets the value of the 'size' parameter to \a size.
*/
void QMailMessageContentDisposition::setSize(int size)
{
    setParameter("size", QByteArray::number(size));
}


/* QMailMessageHeader*/
// According to RFC2822-3.6, these header fields should be present at most once.
Q_GLOBAL_STATIC_WITH_ARGS(QList<QByteArray>, singleHeaders,
                          (QList<QByteArray>() << "date:"
                           << "from:"
                           << "sender:"
                           << "reply-to:"
                           << "to:"
                           << "cc:"
                           << "bcc:"
                           << "message-id:"
                           << "in-reply-to:"
                           << "references:"
                           << "subject:"));

QMailMessageHeaderPrivate::QMailMessageHeaderPrivate()
    : QPrivateImplementationBase(this)
{
}

enum NewLineStatus { None, Cr, CrLf, Lf };

static QList<QByteArray> parseHeaders(const QByteArray& input)
{
    QList<QByteArray> result;
    QByteArray progress;

    // Find each terminating newline, which must be CR, LF, then non-whitespace or end
    NewLineStatus status = None;

    const char* begin = input.constData();
    const char* it = begin;
    for (const char* const end = it + input.length(); it != end; ++it) {
        if (status == CrLf) {
            if (*it == ' ' || *it == '\t') {
                // The CRLF was folded
                if ((it - begin) > 2) {
                    progress.append(QByteArray(begin, (it - begin - 2)));
                }
                begin = it;
            }
            else {
                // That was an unescaped CRLF
                if ((it - begin) > 2) {
                    progress.append(QByteArray(begin, (it - begin) - 2));
                }
                if (!progress.isEmpty()) {
                    // Non-empty field
                    result.append(progress);
                    progress.clear();
                }
                begin = it;
            }
            status = None;
        }
        else if (status == Cr) {
            if (*it == QMailMessage::LineFeed) {
                // CRLF sequence completed
                status = CrLf;
            }
            else {
                status = None;
            }
        } else if (status == Lf) {
            if (*it == ' ' || *it == '\t') {
                // The LF was folded
                if ((it - begin) > 1) {
                    progress.append(QByteArray(begin, (it - begin - 1)));
                }
                begin = it;
            }
            else {
                // That was an unescaped CRLF
                if ((it - begin) > 1) {
                    progress.append(QByteArray(begin, (it - begin) - 1));
                }
                if (!progress.isEmpty()) {
                    // Non-empty field
                    result.append(progress);
                    progress.clear();
                }
                begin = it;
            }
            status = None;

        } else {
            if (*it == QMailMessage::CarriageReturn)
                status = Cr;
            else if (*it == QMailMessage::LineFeed)
                status = Lf;
        }
    }

    if (it != begin) {
        int skip = (status == CrLf ? 2 : (status == None ? 0 : 1));
        if ((it - begin) > skip) {
            progress.append(QByteArray(begin, (it - begin) - skip));
        }
        if (!progress.isEmpty()) {
            result.append(progress);
        }
    }

    return result;
}

QMailMessageHeaderPrivate::QMailMessageHeaderPrivate(const QByteArray& input)
    : QPrivateImplementationBase(this),
      _headerFields(parseHeaders(input))
{
}

static QByteArray fieldId(const QByteArray &id)
{
    QByteArray name = id.trimmed();
    if ( !name.endsWith(':') )
        name.append(':');
    return name;
}

static QPair<QByteArray, QByteArray> fieldParts(const QByteArray &id, const QByteArray &content)
{
    QByteArray value(QByteArray(1, ' ') + content.trimmed());
    return qMakePair(fieldId(id), value);
}

static bool matchingId(const QByteArray& id, const QByteArray& other, bool allowPartial = false)
{
    QByteArray match(id.trimmed());

    int index = insensitiveIndexOf(match, other);
    if (index == -1)
        return false;

    if (index > 0)
    {
        // Ensure that every preceding character is whitespace
        QByteArray leader(other.left(index).trimmed());
        if (!leader.isEmpty())
            return false;
    }

    if (allowPartial)
        return true;

    int lastIndex = index + match.length() - 1;
    index = other.indexOf(':', lastIndex);
    if (index == -1)
        index = other.length() - 1;

    // Ensure that there is only whitespace between the matched ID and the end of the ID
    if ((index - lastIndex) > 1)
    {
        QByteArray trailer(other.mid(lastIndex + 1, (index - lastIndex)).trimmed());
        if (!trailer.isEmpty())
            return false;
    }

    return true;
}

void QMailMessageHeaderPrivate::update(const QByteArray &id, const QByteArray &content)
{
    QPair<QByteArray, QByteArray> parts = fieldParts(id, content);
    QByteArray updated = parts.first + parts.second;

    const QList<QByteArray>::Iterator end = _headerFields.end();
    for (QList<QByteArray>::Iterator it = _headerFields.begin(); it != end; ++it) {
        if ( matchingId(id, (*it)) ) {
            *it = updated;
            return;
        }
    }

    // new header field, add it
    _headerFields.append( updated );
}

void QMailMessageHeaderPrivate::append(const QByteArray &id, const QByteArray &content)
{
    if (singleHeaders->contains(fieldId(id).toLower())) {
        // Ensure that specific fields (see RFC2822, 3.6) cannot be present
        // more that once.
        update(id, content);
    } else {
        QPair<QByteArray, QByteArray> parts = fieldParts(id, content);
        _headerFields.append( parts.first + parts.second );
    }
}

void QMailMessageHeaderPrivate::remove(const QByteArray &id)
{
    QList<QList<QByteArray>::Iterator> matches;

    const QList<QByteArray>::Iterator end = _headerFields.end();
    for (QList<QByteArray>::Iterator it = _headerFields.begin(); it != end; ++it) {
        if ( matchingId(id, (*it)) )
            matches.prepend(it);
    }

    foreach (QList<QByteArray>::Iterator it, matches)
        _headerFields.erase(it);
}

QList<QMailMessageHeaderField> QMailMessageHeaderPrivate::fields(const QByteArray& id, int maximum) const
{
    QList<QMailMessageHeaderField> result;

    foreach (const QByteArray& field, _headerFields) {
        QMailMessageHeaderField headerField(field, QMailMessageHeaderField::UnstructuredField);
        if ( matchingId(id, headerField.id()) ) {
            result.append(headerField);
            if (maximum > 0 && result.count() == maximum)
                return result;
        }
    }

    return result;
}

void QMailMessageHeaderPrivate::output(QDataStream& out, const QList<QByteArray>& exclusions, bool excludeInternalFields) const
{
    foreach (const QByteArray& field, _headerFields) {
        QMailMessageHeaderField headerField(field, QMailMessageHeaderField::UnstructuredField);
        const QByteArray& id = headerField.id();
        bool excluded = false;

        // Bypass any header field that has the internal prefix
        if (excludeInternalFields)
            excluded = matchingId(internalPrefix(), id, true);

        // Bypass any header in the list of exclusions
        if (!excluded)
            foreach (const QByteArray& exclusion, exclusions)
                if (matchingId(exclusion, id))
                    excluded = true;

        if (!excluded)
            headerField.output(out);
    }
}

template <typename Stream> 
void QMailMessageHeaderPrivate::serialize(Stream &stream) const
{
    stream << _headerFields;
}

template <typename Stream> 
void QMailMessageHeaderPrivate::deserialize(Stream &stream)
{
    stream >> _headerFields;
}


/*!
    \class QMailMessageHeader
    \internal
*/

QMailMessageHeader::QMailMessageHeader()
    : QPrivatelyImplemented<QMailMessageHeaderPrivate>(new QMailMessageHeaderPrivate())
{
}

QMailMessageHeader::QMailMessageHeader(const QByteArray& input)
    : QPrivatelyImplemented<QMailMessageHeaderPrivate>(new QMailMessageHeaderPrivate(input))
{
}

void QMailMessageHeader::update(const QByteArray &id, const QByteArray &content)
{
    impl(this)->update(id, content);
}

void QMailMessageHeader::append(const QByteArray &id, const QByteArray &content)
{
    impl(this)->append(id, content);
}

void QMailMessageHeader::remove(const QByteArray &id)
{
    impl(this)->remove(id);
}

QMailMessageHeaderField QMailMessageHeader::field(const QByteArray& id) const
{
    QList<QMailMessageHeaderField> result = impl(this)->fields(id, 1);
    if (result.count())
        return result[0];

    return QMailMessageHeaderField();
}

QList<QMailMessageHeaderField> QMailMessageHeader::fields(const QByteArray& id) const
{
    return impl(this)->fields(id);
}

QList<const QByteArray*> QMailMessageHeader::fieldList() const
{
    QList<const QByteArray*> result;

    QList<QByteArray>::ConstIterator const end = impl(this)->_headerFields.end();
    for (QList<QByteArray>::ConstIterator it = impl(this)->_headerFields.begin(); it != end; ++it)
        result.append(&(*it));

    return result;
}

void QMailMessageHeader::output(QDataStream& out, const QList<QByteArray>& exclusions, bool excludeInternalFields) const
{
    impl(this)->output(out, exclusions, excludeInternalFields);
}

/*! 
    \fn QMailMessageHeader::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailMessageHeader::serialize(Stream &stream) const
{
    impl(this)->serialize(stream);
}

/*! 
    \fn QMailMessageHeader::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessageHeader::deserialize(Stream &stream)
{
    impl(this)->deserialize(stream);
}


/* QMailMessageBody */

QMailMessageBodyPrivate::QMailMessageBodyPrivate()
    : QPrivateImplementationBase(this),
    _encoding(QMailMessageBody::SevenBit), // Default encoding
    _encoded(true)
{
}

void QMailMessageBodyPrivate::ensureCharsetExist()
{
    if (!_type.matches("text", "plain") && !_type.matches("text", "html")) {
        QByteArray best(QMailCodec::bestCompatibleCharset(_type.charset(), true));
        if (!best.isEmpty()) {
            _type.setCharset(best);
        }
        return;
    }

    QByteArray charset = _type.charset();
    if (charset == "UNKNOWN_PARAMETER_VALUE") {
        charset = "";
    }
    if (charset.isEmpty() || insensitiveIndexOf("ascii", charset) != -1) {
        // Load the data and do the charset detection only when absolutely
        // necessary. It can be a slow operation if it contains megabytes
        // of data.
        const QByteArray &data(_bodyData.toQByteArray());
        if (data.isEmpty()) {
            return;
        }
        QByteArray autoCharset;
        if (_encoded && _encoding != QMailMessageBody::SevenBit) {
            QMailCodec* codec = codecForEncoding(_encoding, _type);
            const QByteArray &decoded = codec->decode(data);
            autoCharset = QMailCodec::autoDetectEncoding(decoded).toLatin1();
            delete codec;
        } else {
            autoCharset = QMailCodec::autoDetectEncoding(data).toLatin1();
        }
        if (!autoCharset.isEmpty() && (insensitiveIndexOf("ISO-8859-", autoCharset) == -1)) {
            QByteArray best(QMailCodec::bestCompatibleCharset(autoCharset, true));
            if (!best.isEmpty()) {
                _type.setCharset(best);
            }
        }
    } else {
        QByteArray best(QMailCodec::bestCompatibleCharset(charset, true));
        if (!best.isEmpty()) {
            _type.setCharset(best);
        }
    }
}

void QMailMessageBodyPrivate::fromLongString(LongString& ls, const QMailMessageContentType& content, QMailMessageBody::TransferEncoding te, QMailMessageBody::EncodingStatus status)
{
    _encoding = te;
    _type = content;
    _encoded = (status == QMailMessageBody::AlreadyEncoded);
    _filename.clear();
    _bodyData = ls;

    ensureCharsetExist();
}

void QMailMessageBodyPrivate::fromFile(const QString& file, const QMailMessageContentType& content, QMailMessageBody::TransferEncoding te, QMailMessageBody::EncodingStatus status)
{
    _encoding = te;
    _type = content;
    _encoded = (status == QMailMessageBody::AlreadyEncoded);
    _filename = file;
    _bodyData = LongString(file);

    ensureCharsetExist();
}

void QMailMessageBodyPrivate::fromStream(QDataStream& in, const QMailMessageContentType& content, QMailMessageBody::TransferEncoding te, QMailMessageBody::EncodingStatus status)
{
    _encoding = te;
    _type = content;
    _encoded = true;
    _filename.clear();
    _bodyData = LongString();

    // If the data is already encoded, we don't need to do it again
    if (status == QMailMessageBody::AlreadyEncoded)
        te = QMailMessageBody::SevenBit;

    QMailCodec* codec = codecForEncoding(te, content);
    if (codec)
    {
        // Stream to the buffer, encoding as required
        QByteArray encoded;
        {
            QDataStream out(&encoded, QIODevice::WriteOnly);
            codec->encode(out, in);
        }
        _bodyData = LongString(encoded);
        delete codec;
    }

    ensureCharsetExist();
}

void QMailMessageBodyPrivate::fromStream(QTextStream& in, const QMailMessageContentType& content, QMailMessageBody::TransferEncoding te)
{
    _encoding = te;
    _type = content;
    _encoded = true;
    _filename.clear();
    _bodyData = LongString();

    QMailCodec* codec = codecForEncoding(te, content);
    if (codec)
    {
        QByteArray encoded;
        {
            QDataStream out(&encoded, QIODevice::WriteOnly);

            // Convert the unicode string to a byte-stream, via the nominated character set
            QString charset(QString::fromUtf8(_type.charset()));

            // If no character set is specified - treat the data as UTF-8; since it is
            // textual data, it must have some character set...
            if (charset.isEmpty())
                charset = QLatin1String("UTF-8");

            codec->encode(out, in, charset);
        }
        _bodyData = LongString(encoded);
        delete codec;
    }

    ensureCharsetExist();
}

static bool unicodeConvertingCharset(const QByteArray& charset)
{
    // See if this is a unicode-capable codec
    if (QTextCodec* textCodec = QMailCodec::codecForName(charset, true))
    {
        const QChar multiByteChar = 0x1234;
        return textCodec->canEncode(multiByteChar);
    }
    else
    {
        qWarning() << "unicodeConvertingCharset: unable to find codec for charset:" << charset;
    }

    return false;
}

static QByteArray extractionCharset(const QMailMessageContentType& type)
{
    QByteArray charset;

    // Find the charset for this data, if it is text data
    if (insensitiveEqual(type.type(), "text"))
    {
        charset = type.charset();
        if (!charset.isEmpty())
        {
            // If the codec can't handle multi-byte characters, don't extract to/from unicode
            if (!unicodeConvertingCharset(charset))
                charset = QByteArray();
        }
    }

    return charset;
}

bool QMailMessageBodyPrivate::toFile(const QString& file, QMailMessageBody::EncodingFormat format) const
{
    QFile outFile(file);
    if (!outFile.open(QIODevice::WriteOnly))
    {
        qWarning() << "Unable to open for write:" << file;
        return false;
    }

    bool encodeOutput = (format == QMailMessageBody::Encoded);

    // Find the charset for this data, if it is text data
    QByteArray charset(extractionCharset(_type));

    QMailMessageBody::TransferEncoding te = _encoding;

    // If our data is in the required condition, we don't need to encode/decode
    if (encodeOutput == _encoded)
        te = QMailMessageBody::Binary;

    QMailCodec* codec = codecForEncoding(te, _type);
    if (codec)
    {
        bool result = false;

        // Empty charset indicates no unicode encoding; encoded return data means binary streams
        if (charset.isEmpty() || encodeOutput)
        {
            // We are dealing with binary data
            QDataStream out(&outFile);
            QDataStream* in = _bodyData.dataStream();
            if (encodeOutput)
                codec->encode(out, *in);
            else
                codec->decode(out, *in);
            result = (in->status() == QDataStream::Ok);
            delete in;
        }
        else // we should probably check that charset matches this->charset
        {
            // We are dealing with unicode text data, which we want in unencoded form
            QTextStream out(&outFile);
            out.setCodec(charset);

            // If the content is unencoded we can pass it back via a text stream
            if (!_encoded)
            {
                QTextStream* in = _bodyData.textStream();
                in->setCodec(charset);
                QMailCodec::copy(out, *in);
                result = (in->status() == QTextStream::Ok);
                delete in;
            }
            else
            {
                QDataStream* in = _bodyData.dataStream();
                codec->decode(out, *in, QString::fromUtf8(charset));
                result = (in->status() == QDataStream::Ok);
                delete in;
            }
        }

        delete codec;
        return result;
    }

    return false;
}

bool QMailMessageBodyPrivate::toStream(QDataStream& out, QMailMessageBody::EncodingFormat format) const
{
    bool encodeOutput = (format == QMailMessageBody::Encoded);
    QMailMessageBody::TransferEncoding te = _encoding;

    // If our data is in the required condition, we don't need to encode/decode
    if (encodeOutput == _encoded)
        te = QMailMessageBody::Binary;

    QMailCodec* codec = codecForEncoding(te, _type);
    if (codec)
    {
        bool result = false;

        QByteArray charset(extractionCharset(_type));
        if (!charset.isEmpty() && !_filename.isEmpty() && encodeOutput)
        {
            // This data must be unicode in the file
            QTextStream* in = _bodyData.textStream();
            in->setCodec(charset);
            codec->encode(out, *in, QString::fromUtf8(charset));
            result = (in->status() == QTextStream::Ok);
            delete in;
        }
        else
        {
            QDataStream* in = _bodyData.dataStream();
            if (encodeOutput)
                codec->encode(out, *in);
            else
                codec->decode(out, *in);
            result = (in->status() == QDataStream::Ok);
            delete in;
        }

        delete codec;
        return result;
    }

    return false;
}

bool QMailMessageBodyPrivate::toStream(QTextStream& out) const
{
    QByteArray charset = _type.charset();
    if (charset.isEmpty() || insensitiveIndexOf("ascii", charset) != -1) {
        // We'll assume the text is plain ASCII, to be extracted to Latin-1
        charset = "ISO-8859-1";
    }

    out.setCodec(charset);

    QMailMessageBody::TransferEncoding te = _encoding;

    // If our data is not encoded, we don't need to decode
    if (!_encoded)
        te = QMailMessageBody::Binary;

    QMailCodec* codec = codecForEncoding(te, _type);
    if (codec)
    {
        bool result = false;

        if (!_encoded && !_filename.isEmpty() && unicodeConvertingCharset(charset))
        { 
            // The data is already in unicode format
            QTextStream* in = _bodyData.textStream();
            in->setCodec(charset);
            QMailCodec::copy(out, *in);
            result = (in->status() == QTextStream::Ok);
            delete in;
        }
        else
        {
            // Write the data to out, decoding if necessary
            QDataStream* in = _bodyData.dataStream();
            codec->decode(out, *in, QString::fromUtf8(charset));
            result = (in->status() == QDataStream::Ok);
            delete in;
        }

        delete codec;
        return result;
    }

    return false;
}

QMailMessageContentType QMailMessageBodyPrivate::contentType() const
{
    return _type;
}

QMailMessageBody::TransferEncoding QMailMessageBodyPrivate::transferEncoding() const
{
    return _encoding;
}

bool QMailMessageBodyPrivate::isEmpty() const
{
    return _bodyData.isEmpty();
}

int QMailMessageBodyPrivate::length() const
{
    return _bodyData.length();
}

bool QMailMessageBodyPrivate::encoded() const
{
    return _encoded;
}

void QMailMessageBodyPrivate::setEncoded(bool value)
{
    _encoded = value;
}

uint QMailMessageBodyPrivate::indicativeSize() const
{
    return (_bodyData.length() / IndicativeSizeUnit);
}

void QMailMessageBodyPrivate::output(QDataStream& out, bool includeAttachments) const
{
    if ( includeAttachments )
        toStream( out, QMailMessageBody::Encoded );
}

template <typename Stream> 
void QMailMessageBodyPrivate::serialize(Stream &stream) const
{
    stream << _encoding;
    stream << _bodyData;
    stream << _filename;
    stream << _encoded;
    stream << _type;
}

template <typename Stream> 
void QMailMessageBodyPrivate::deserialize(Stream &stream)
{
    stream >> _encoding;
    stream >> _bodyData;
    stream >> _filename;
    stream >> _encoded;
    stream >> _type;
}


/*!
    \class QMailMessageBody

    \preliminary
    \brief The QMailMessageBody class contains the body element of a message or message part.
    
    \ingroup messaginglibrary
   
    The body of a message or message part is treated as an atomic unit by the Qt Extended messaging library.  It can only be inserted into a message part container or extracted
    from one.  It can be inserted or extracted using either a QByteArray, a QDataStream
    or to/from a file.  In the case of unicode text data, the insertion and extraction can
    operate on either a QString, a QTextStream or to/from a file.

    The body data must be associated with a QMailMessageContentType describing that data.
    When extracting body data from a message or part to unicode text, the content type
    description must include a parameter named 'charset'; this parameter is used to locate
    a QTextCodec to be used to extract unicode data from the body data octet stream.  
    
    If the Content-Type of the data is a subtype of "text", then line-ending translation 
    will be used to ensure that the text is transmitted with CR/LF line endings.  The text 
    data supplied to QMailMessageBody must conform to the RFC 2822 restrictions on maximum 
    line lengths: "Each line of characters MUST be no more than 998 characters, and SHOULD 
    be no more than 78 characters, excluding the CRLF."  Textual message body data decoded 
    from a QMailMessageBody object will have transmitted CR/LF line endings converted to 
    \c \n on extraction.

    The body data can also be encoded from 8-bit octets to 7-bit ASCII characters for
    safe transmission through obsolete email systems.  When creating an instance of the 
    QMailMessageBody class, the encoding to be used must be specified using the 
    QMailMessageBody::TransferEncoding enum.

    \sa QMailMessagePart, QMailMessage, QTextCodec
*/    

/*!
    \typedef QMailMessageBody::ImplementationType
    \internal
*/

/*! 
    Creates an instance of QMailMessageBody.
*/
QMailMessageBody::QMailMessageBody()
    : QPrivatelyImplemented<QMailMessageBodyPrivate>(new QMailMessageBodyPrivate())
{
}

/*!
    Creates a message body from the data contained in the file \a filename, having the content type 
    \a type.  If \a status is QMailMessageBody::RequiresEncoding, the data from the file will be 
    encoded to \a encoding for transmission; otherwise it must already be in that encoding, which
    will be reported to recipients of the data.

    If \a type is a subtype of "text", the data will be treated as text, and line-ending
    translation will be employed.  Otherwise, the file will be treated as containing binary 
    data.  If the file contains unicode text data, it will be converted to an octet stream using
    a QTextCodec object identified by the 'charset' parameter of \a type.

    If \a encoding is QMailMessageBody::QuotedPrintable, encoding will be performed assuming
    conformance to RFC 2045.

    Note that the data is not actually read from the file until it is requested by another function,
    unless it is of type "text/plain" or "text/html". In these latter cases, automatic character
    set detection may take place by reading all the data from the file.

    \sa QMailCodec, QMailQuotedPrintableCodec, QMailMessageContentType, QTextCodec
*/
QMailMessageBody QMailMessageBody::fromFile(const QString& filename, const QMailMessageContentType& type, TransferEncoding encoding, EncodingStatus status)
{
    QMailMessageBody body;
    body.impl<QMailMessageBodyPrivate>()->fromFile(filename, type, encoding, status);
    return body;
}

/*!
    Creates a message body from the data read from \a in, having the content type \a type.  
    If \a status is QMailMessageBody::RequiresEncoding, the data from the file will be 
    encoded to \a encoding for transmission; otherwise it must already be in that encoding, 
    which will be reported to recipients of the data.

    If \a type is a subtype of "text", the data will be treated as text, and line-ending
    translation will be employed.  Otherwise, the file will be treated as containing binary 
    data.

    If \a encoding is QMailMessageBody::QuotedPrintable, encoding will be performed assuming
    conformance to RFC 2045.

    \sa QMailCodec, QMailQuotedPrintableCodec
*/
QMailMessageBody QMailMessageBody::fromStream(QDataStream& in, const QMailMessageContentType& type, TransferEncoding encoding, EncodingStatus status)
{
    QMailMessageBody body;
    body.impl<QMailMessageBodyPrivate>()->fromStream(in, type, encoding, status);
    return body;
}

/*!
    Creates a message body from the data contained in \a input, having the content type 
    \a type.  If \a status is QMailMessageBody::RequiresEncoding, the data from the file will be 
    encoded to \a encoding for transmission; otherwise it must already be in that encoding, 
    which will be reported to recipients of the data.

    If \a type is a subtype of "text", the data will be treated as text, and line-ending
    translation will be employed.  Otherwise, the file will be treated as containing binary 
    data.

    If \a encoding is QMailMessageBody::QuotedPrintable, encoding will be performed assuming
    conformance to RFC 2045.

    \sa QMailCodec, QMailQuotedPrintableCodec
*/
QMailMessageBody QMailMessageBody::fromData(const QByteArray& input, const QMailMessageContentType& type, TransferEncoding encoding, EncodingStatus status)
{
    QMailMessageBody body;
    {
        QDataStream in(input);
        body.impl<QMailMessageBodyPrivate>()->fromStream(in, type, encoding, status);
    }
    return body;
}

/*!
    Creates a message body from the data read from \a in, having the content type \a type.  
    The data read from \a in will be encoded to \a encoding for transmission, and line-ending
    translation will be employed.  The unicode text data will be converted to an octet stream 
    using a QTextCodec object identified by the 'charset' parameter of \a type.

    If \a encoding is QMailMessageBody::QuotedPrintable, encoding will be performed assuming
    conformance to RFC 2045.

    \sa QMailCodec, QMailQuotedPrintableCodec, QMailMessageContentType, QTextCodec
*/
QMailMessageBody QMailMessageBody::fromStream(QTextStream& in, const QMailMessageContentType& type, TransferEncoding encoding)
{
    QMailMessageBody body;
    body.impl<QMailMessageBodyPrivate>()->fromStream(in, type, encoding);
    return body;
}

/*!
    Creates a message body from the data contained in \a input, having the content type 
    \a type.  The data from \a input will be encoded to \a encoding for transmission, and 
    line-ending translation will be employed.  The unicode text data will be converted to 
    an octet stream using a QTextCodec object identified by the 'charset' parameter of \a type.

    If \a encoding is QMailMessageBody::QuotedPrintable, encoding will be performed assuming
    conformance to RFC 2045.

    \sa QMailCodec, QMailMessageContentType, QTextCodec
*/
QMailMessageBody QMailMessageBody::fromData(const QString& input, const QMailMessageContentType& type, TransferEncoding encoding)
{
    QMailMessageBody body;
    {
        QTextStream in(const_cast<QString*>(&input), QIODevice::ReadOnly);
        body.impl<QMailMessageBodyPrivate>()->fromStream(in, type, encoding);
    }
    return body;
}

QMailMessageBody QMailMessageBody::fromLongString(LongString& ls, const QMailMessageContentType& type, TransferEncoding encoding, EncodingStatus status)
{
    QMailMessageBody body;
    {
        body.impl<QMailMessageBodyPrivate>()->fromLongString(ls, type, encoding, status);
    }
    return body;
}

/*!
    Writes the data of the message body to the file named \a filename.  If \a format is
    QMailMessageBody::Encoded, then the data is written in the transfer encoding it was
    created with; otherwise, it is written in unencoded form.

    If the body has a content type with a QMailMessageContentType::type() of "text", and the 
    content type parameter 'charset' is not empty, then the unencoded data will be written 
    as unicode text data, using the charset parameter to locate the appropriate QTextCodec.

    Returns false if the operation causes an error; otherwise returns true.

    \sa QMailCodec, QMailMessageContentType, QTextCodec
*/
bool QMailMessageBody::toFile(const QString& filename, EncodingFormat format) const
{
    return impl(this)->toFile(filename, format);
}

/*!
    Returns the data of the message body as a QByteArray.  If \a format is
    QMailMessageBody::Encoded, then the data is written in the transfer encoding it was
    created with; otherwise, it is written in unencoded form.

    \sa QMailCodec
*/
QByteArray QMailMessageBody::data(EncodingFormat format) const
{
    QByteArray result;
    {
        QDataStream out(&result, QIODevice::WriteOnly);
        impl(this)->toStream(out, format);
    }
    return result;
}

/*!
    Writes the data of the message body to the stream \a out. If \a format is
    QMailMessageBody::Encoded, then the data is written in the transfer encoding it was
    created with; otherwise, it is written in unencoded form.

    Returns false if the operation causes an error; otherwise returns true.

    \sa QMailCodec
*/
bool QMailMessageBody::toStream(QDataStream& out, EncodingFormat format) const
{
    return impl(this)->toStream(out, format);
}

/*!
    Returns the data of the message body as a QString, in unencoded form.  Line-endings
    transmitted as CR/LF pairs are converted to \c \n on extraction.

    The 'charset' parameter of the body's content type is used to locate the appropriate 
    QTextCodec to convert the data from an octet stream to unicode, if necessary.

    \sa QMailCodec, QMailMessageContentType, QTextCodec
*/
QString QMailMessageBody::data() const
{
    QString result;
    {
        QTextStream out(&result, QIODevice::WriteOnly);
        impl(this)->toStream(out);
    }
    return result;
}

/*!
    Writes the data of the message body to the stream \a out, in unencoded form. 
    Line-endings transmitted as CR/LF pairs are converted to \c \n on extraction.
    Returns false if the operation causes an error; otherwise returns true.

    The 'charset' parameter of the body's content type is used to locate the appropriate 
    QTextCodec to convert the data from an octet stream to unicode, if necessary.

    \sa QMailCodec, QMailMessageContentType, QTextCodec
*/
bool QMailMessageBody::toStream(QTextStream& out) const
{
    return impl(this)->toStream(out);
}

/*!
    Returns the content type that the body was created with.
*/
QMailMessageContentType QMailMessageBody::contentType() const
{
    return impl(this)->contentType();
}

/*!
    Returns the transfer encoding type that the body was created with.
*/
QMailMessageBody::TransferEncoding QMailMessageBody::transferEncoding() const
{
    return impl(this)->transferEncoding();
}

/*!
    Returns true if the body does not contain any data.
*/
bool QMailMessageBody::isEmpty() const
{
    return impl(this)->isEmpty();
}

/*!
    Returns the length of the body data in bytes.
*/
int QMailMessageBody::length() const
{
    return impl(this)->length();
}

/*! \internal */
uint QMailMessageBody::indicativeSize() const
{
    return impl(this)->indicativeSize();
}

/*! \internal */
bool QMailMessageBody::encoded() const
{
    return impl(this)->encoded();
}

/*! \internal */
void QMailMessageBody::setEncoded(bool value)
{
    impl(this)->setEncoded(value);
}

/*! \internal */
void QMailMessageBody::output(QDataStream& out, bool includeAttachments) const
{
    impl(this)->output(out, includeAttachments);
}

/*! 
    \fn QMailMessageBody::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailMessageBody::serialize(Stream &stream) const
{
    impl(this)->serialize(stream);
}

/*! 
    \fn QMailMessageBody::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessageBody::deserialize(Stream &stream)
{
    impl(this)->deserialize(stream);
}


class QMailMessagePartContainer::LocationPrivate
{
public:
    QMailMessageId _messageId;
    QList<uint> _indices;
};


/* QMailMessagePartContainer */

template<typename Derived>
QMailMessagePartContainerPrivate::QMailMessagePartContainerPrivate(Derived* p)
    : QPrivateImplementationBase(p)
    , _multipartType(QMailMessagePartContainer::MultipartNone)
    , _hasBody(false)
    , _dirty(false)
    , _previewDirty(false)
{
}

void QMailMessagePartContainerPrivate::setLocation(const QMailMessageId& id, const QList<uint>& indices)
{
    _messageId = id;
    _indices = indices;

    if (!_messageParts.isEmpty()) {
        QList<QMailMessagePart>::iterator it = _messageParts.begin(), end = _messageParts.end();
        for (uint i = 0; it != end; ++it, ++i) {
            QList<uint> location(_indices);
            location.append(i + 1);
            (*it).impl<QMailMessagePartContainerPrivate>()->setLocation(_messageId, location);
        }
    }
}

int QMailMessagePartContainerPrivate::partNumber() const
{
    return (_indices.last() - 1);
}

bool QMailMessagePartContainerPrivate::contains(const QMailMessagePart::Location& location) const
{
    const QMailMessagePart* part = Q_NULLPTR;
    const QList<QMailMessagePart>* partList = &_messageParts; 

    foreach (int index, location.d->_indices) {
        if (partList->count() < index) {
            return false;
        }

        part = &(partList->at(index - 1));
        partList = &(part->impl<const QMailMessagePartContainerPrivate>()->_messageParts);
    }

    return true;
}

const QMailMessagePart& QMailMessagePartContainerPrivate::partAt(const QMailMessagePart::Location& location) const
{
    const QMailMessagePart* part = Q_NULLPTR;
    const QList<QMailMessagePart>* partList = &_messageParts; 

    foreach (int index, location.d->_indices) {
        if (index > 0 && index <= partList->size()) {
            part = &(partList->at(index - 1));
            partList = &(part->impl<const QMailMessagePartContainerPrivate>()->_messageParts);
        } else {
            qMailLog(Messaging) << Q_FUNC_INFO << "Invalid index, container does not have a part at " << index;
            Q_ASSERT(false);
        }
    }

    Q_ASSERT(part);
    return *part;
}

QMailMessagePart& QMailMessagePartContainerPrivate::partAt(const QMailMessagePart::Location& location)
{
    QMailMessagePart* part = Q_NULLPTR;
    QList<QMailMessagePart>* partList = &_messageParts; 

    foreach (int index, location.d->_indices) {
        if (index > 0 && index <= partList->size()) {
            part = &((*partList)[index - 1]);
            partList = &(part->impl<QMailMessagePartContainerPrivate>()->_messageParts);
        } else {
            qMailLog(Messaging) << Q_FUNC_INFO << "Invalid index, container does not have a part at " << index;
            Q_ASSERT(false);
        }
    }

    return *part;
}

void QMailMessagePartContainerPrivate::setHeader(const QMailMessageHeader& partHeader, const QMailMessagePartContainerPrivate* parent)
{
    _header = partHeader;

    defaultContentType(parent);

    QByteArray contentType = headerField("Content-Type");
    if (!contentType.isEmpty())
    {
        // Extract the stored parts from the supplied field
        QMailMessageContentType type(contentType);
        _multipartType = QMailMessagePartContainer::multipartTypeForName(type.content());
        _boundary = type.boundary();
    }
}

void QMailMessagePartContainerPrivate::defaultContentType(const QMailMessagePartContainerPrivate* parent)
{
    QMailMessageContentType type;

    // Find the content-type, or use default values
    QByteArray contentType = headerField("Content-Type");
    bool useDefault = contentType.isEmpty();

    if (!useDefault)
    {
        type = QMailMessageContentType(contentType);

        if (type.type().isEmpty() || type.subType().isEmpty())
        {
            useDefault = true;
        }
        else if (insensitiveEqual(type.content(), "application/octet-stream"))
        {
            // Sender's client might not know what type, but maybe we do. Try...
            QByteArray contentDisposition = headerField("Content-Disposition");
            if (!contentDisposition.isEmpty())
            {
                QMailMessageContentDisposition disposition(contentDisposition);

                QString mimeType = QMail::mimeTypeFromFileName(QString::fromUtf8(disposition.filename()));
                if (!mimeType.isEmpty())
                {
                    type.setContent(to7BitAscii(mimeType));
                    updateHeaderField(type.id(), type.toString(false, false));
                }
            }
        }
    }

    if (useDefault && parent)
    {
        // Note that the default is 'message/rfc822' when the parent is 'multipart/digest'
        QMailMessageContentType parentType = parent->contentType();
        if (parentType.matches("multipart", "digest"))
        {
            type.setType("message");
            type.setSubType("rfc822");
            updateHeaderField(type.id(), type.toString(false, false));
            useDefault = false;
        }
    }

    if (useDefault)
    {
        type.setType("text");
        type.setSubType("plain");
        type.setCharset("us-ascii");
        updateHeaderField(type.id(), type.toString(false, false));
    }
}

/*! \internal */
uint QMailMessagePartContainerPrivate::indicativeSize() const
{
    uint size = 0;

    if (hasBody()) {
        size = body().indicativeSize();
    } else {
        for (int i = 0; i < _messageParts.count(); ++i)
            size += _messageParts[i].indicativeSize();
    }

    return size;
}

template <typename F>
void QMailMessagePartContainerPrivate::outputParts(QDataStream **out, bool addMimePreamble, bool includeAttachments, bool excludeInternalFields, F *func) const
{
    static const DataString newLine('\n');
    static const DataString marker("--");

    if (_multipartType == QMailMessagePartContainer::MultipartNone)
        return;

    if (addMimePreamble) {
        // This is a preamble (not for conformance, to assist readibility on non-conforming renderers):
        **out << DataString("This is a multipart message in Mime 1.0 format"); // No tr
        **out << newLine;
    }

    for ( int i = 0; i < _messageParts.count(); i++ ) {
        **out << newLine << marker << DataString(_boundary) << newLine;

        QMailMessagePart& part = const_cast<QMailMessagePart&>(_messageParts[i]);

        if (part.multipartType() != QMailMessagePartContainer::MultipartNone) {
            const QString &partBoundary(QString::fromUtf8(part.boundary()));

            if (partBoundary.isEmpty()) {
                QString subBoundary(QString::fromUtf8(_boundary));
                int index = subBoundary.indexOf(':');
                if (index != -1) {
                    subBoundary.insert(index, QString::number(part.partNumber()).prepend(QLatin1String("-")));
                } else {
                    // Shouldn't happen...
                    subBoundary.insert(0, QString::number(part.partNumber()).append(QLatin1String(":")));
                }

                part.setBoundary(to7BitAscii(subBoundary));
            }
        }
        QMailMessagePartPrivate *partImpl(part.impl<QMailMessagePartPrivate>());
        partImpl->output<F>(out, false, includeAttachments, excludeInternalFields, func);
    }

    **out << newLine << marker << DataString(_boundary) << marker << newLine;
}

void QMailMessagePartContainerPrivate::outputBody(QDataStream& out, bool includeAttachments) const
{
    _body.output(out, includeAttachments);
}

static QString decodedContent(const QString& id, const QByteArray& content)
{
    // TODO: Potentially, we should disallow decoding here based on the specific header field
    // return (permitDecoding ? QMailMessageHeaderField::decodeContent(content) : QString(content));
    Q_UNUSED(id);

    return QMailMessageHeaderField::decodeContent(content);
}

/*!
    Returns the text of the first header field with the given \a id.
*/
QString QMailMessagePartContainerPrivate::headerFieldText( const QString &id ) const
{
    const QByteArray& content = headerField( to7BitAscii(id) );
    return decodedContent( id, content );
}

static QMailMessageContentType updateContentType(const QByteArray& existing, QMailMessagePartContainer::MultipartType multipartType, const QByteArray& boundary)
{
    // Ensure that any parameters of the existing field are preserved
    QMailMessageContentType existingType(existing);
    QList<QMailMessageHeaderField::ParameterType> parameters = existingType.parameters();

    QMailMessageContentType type(QMailMessagePartContainer::nameForMultipartType(multipartType));
    foreach (const QMailMessageHeaderField::ParameterType& param, parameters)
        type.setParameter(param.first, param.second);

    if (!boundary.isEmpty())
        type.setBoundary(boundary);

    return type;
}

void QMailMessagePartContainerPrivate::setMultipartType(QMailMessagePartContainer::MultipartType type, const QList<QMailMessageHeaderField::ParameterType> &parameters)
{
    // TODO: Is there any value in keeping _multipartType and _boundary externally from
    // Content-type header field?

    if (_multipartType != type) {
        _multipartType = type;
        setDirty();
        setPreviewDirty(true); // this could change the preview string

        if (_multipartType == QMailMessagePartContainer::MultipartNone) {
            removeHeaderField("Content-Type");
        } else  {
            QMailMessageContentType contentType = updateContentType(headerField("Content-Type"), _multipartType, _boundary);
            foreach (const QMailMessageHeaderField::ParameterType& param, parameters)
                contentType.setParameter(param.first, param.second);
            updateHeaderField("Content-Type", contentType.toString(false, false));

            if (_hasBody) {
                _body = QMailMessageBody();
                _hasBody = false;
                removeHeaderField("Content-Transfer-Encoding");
            }
        }
    }
}

QByteArray QMailMessagePartContainerPrivate::boundary() const
{
    return _boundary;
}

void QMailMessagePartContainerPrivate::setBoundary(const QByteArray& text)
{
    _boundary = text;

    if (_multipartType != QMailMessagePartContainer::MultipartNone) {
        QMailMessageContentType type = updateContentType(headerField("Content-Type"), _multipartType, _boundary);
        updateHeaderField("Content-Type", type.toString(false, false));
    } else {
        QMailMessageHeaderField type("Content-Type", headerField("Content-Type"));
        type.setParameter("boundary", _boundary);
        updateHeaderField("Content-Type", type.toString(false, false));
    }
}

static QByteArray boundaryString(const QByteArray &hash);
void QMailMessagePartContainerPrivate::generateBoundary()
{
    if (_multipartType != QMailMessagePartContainer::MultipartNone
        && _boundary.isEmpty()) {
        // Include a hash of the header data in the boundary
        QCryptographicHash hash(QCryptographicHash::Md5);
        foreach (const QByteArray *field, _header.fieldList())
            hash.addData(*field);

        setBoundary(boundaryString(hash.result()));
    }
}

QMailMessageBody& QMailMessagePartContainerPrivate::body()
{
    return _body;
}

const QMailMessageBody& QMailMessagePartContainerPrivate::body() const
{
    return const_cast<QMailMessagePartContainerPrivate*>(this)->_body;
}

void QMailMessagePartContainerPrivate::setBody(const QMailMessageBody& body, QMailMessageBody::EncodingFormat encodingStatus)
{
    // Set the body's properties into our header
    setBodyProperties(body.contentType(), body.transferEncoding());

    // Multipart messages do not have their own bodies
    if (!body.contentType().matches("multipart")) {
        _body = body;
        _hasBody = !_body.isEmpty();
    }

    if (encodingStatus == QMailMessageBody::Encoded) {
        _body.setEncoded(true);
    } else if (encodingStatus == QMailMessageBody::Decoded) {
        _body.setEncoded(false);
    }

    setPreviewDirty(true);
}

void QMailMessagePartContainerPrivate::setBodyProperties(const QMailMessageContentType &type, QMailMessageBody::TransferEncoding encoding)
{
    updateHeaderField(type.id(), type.toString(false, false));

    QByteArray encodingName(nameForEncoding(encoding));
    if (!encodingName.isEmpty()) {
        updateHeaderField("Content-Transfer-Encoding", encodingName);
    } else {
        removeHeaderField("Content-Transfer-Encoding");
    }

    setDirty();
}

bool QMailMessagePartContainerPrivate::hasBody() const
{
    return _hasBody;
}

static QByteArray plainId(const QByteArray &id)
{
    QByteArray name(id.trimmed());
    if (name.endsWith(':'))
        name.chop(1);
    return name.trimmed();
}

QByteArray QMailMessagePartContainerPrivate::headerField( const QByteArray &name ) const
{
    QList<QByteArray> result = headerFields(name, 1);
    if (result.count())
        return result[0];

    return QByteArray();
}

QList<QByteArray> QMailMessagePartContainerPrivate::headerFields( const QByteArray &name, int maximum ) const
{
    QList<QByteArray> result;

    QByteArray id(plainId(name));

    foreach (const QByteArray* field, _header.fieldList()) {
        QMailMessageHeaderField headerField(*field, QMailMessageHeaderField::UnstructuredField);
        if (insensitiveEqual(headerField.id(), id)) {
            result.append(headerField.content());
            if (maximum > 0 && result.count() == maximum)
                break;
        }
    }

    return result;
}

QList<QByteArray> QMailMessagePartContainerPrivate::headerFields() const
{
    QList<QByteArray> result;

    foreach (const QByteArray* field, _header.fieldList())
        result.append(*field);

    return result;
}

void QMailMessagePartContainerPrivate::updateHeaderField(const QByteArray &id, const QByteArray &content)
{
    _header.update(id, content);
    setDirty();

    if (insensitiveEqual(plainId(id), "Content-Type"))
    {
        // Extract the stored parts from the supplied field
        QMailMessageContentType type(content);
        _multipartType = QMailMessagePartContainer::multipartTypeForName(type.content());
        _boundary = type.boundary();
    }
}

static QByteArray encodedContent(const QByteArray& id, const QString& content)
{
    // TODO: Potentially, we should disallow encoding here based on the specific header field
    // return (permitEncoding ? QMailMessageHeaderField::encodeContent(content) : to7BitAscii(content));
    Q_UNUSED(id)

    return QMailMessageHeaderField::encodeContent(content);
}

void QMailMessagePartContainerPrivate::updateHeaderField(const QByteArray &id, const QString &content)
{
    updateHeaderField(id, encodedContent(id, content));
}

void QMailMessagePartContainerPrivate::appendHeaderField(const QByteArray &id, const QByteArray &content)
{
    _header.append( id, content );
    setDirty();

    if (insensitiveEqual(plainId(id), "Content-Type"))
    {
        // Extract the stored parts from the supplied field
        QMailMessageContentType type(content);
        _multipartType = QMailMessagePartContainer::multipartTypeForName(type.content());
        _boundary = type.boundary();
    }
}

void QMailMessagePartContainerPrivate::appendHeaderField(const QByteArray &id, const QString &content)
{
    appendHeaderField(id, encodedContent(id, content));
}

void QMailMessagePartContainerPrivate::removeHeaderField(const QByteArray &id)
{
    _header.remove(id);
    setDirty();

    if (insensitiveEqual(plainId(id), "Content-Type"))
    {
        // Extract the stored parts from the supplied field
        _multipartType = QMailMessagePartContainer::MultipartNone;
        _boundary = QByteArray();
    }
}

void QMailMessagePartContainerPrivate::appendPart(const QMailMessagePart &part)
{
    QList<QMailMessagePart>::iterator it = _messageParts.insert( _messageParts.end(), part );

    QList<uint> location(_indices);
    location.append(_messageParts.count());
    (*it).impl<QMailMessagePartContainerPrivate>()->setLocation(_messageId, location);

    setDirty();
    setPreviewDirty(true);
}

void QMailMessagePartContainerPrivate::prependPart(const QMailMessagePart &part)
{
    // Increment the part numbers for existing parts
    QList<QMailMessagePart>::iterator it = _messageParts.begin(), end = _messageParts.end();
    for (uint i = 1; it != end; ++it, ++i) {
        QList<uint> location(_indices);
        location.append(i + 1);
        (*it).impl<QMailMessagePartContainerPrivate>()->setLocation(_messageId, location);
    }

    it = _messageParts.insert( _messageParts.begin(), part );

    QList<uint> location(_indices);
    location.append(1);
    (*it).impl<QMailMessagePartContainerPrivate>()->setLocation(_messageId, location);

    setDirty();
    setPreviewDirty(true);
}

void QMailMessagePartContainerPrivate::removePartAt(uint pos)
{
    _messageParts.removeAt(pos);

    // Update the part numbers of the existing parts
    QList<uint> location(_indices);

    uint partCount(static_cast<uint>(_messageParts.count()));

    for (uint i = pos; i < partCount; ++i) {
        location.append(i + 1);
        _messageParts[i].impl<QMailMessagePartContainerPrivate>()->setLocation(_messageId, location);
        location.removeLast();
    }

    setDirty();
    setPreviewDirty(true);
}

void QMailMessagePartContainerPrivate::clear()
{
    if (!_messageParts.isEmpty()) {
        _messageParts.clear();
        setDirty();
        setPreviewDirty(true);
    }
}

QMailMessageContentType QMailMessagePartContainerPrivate::contentType() const
{
    return QMailMessageContentType(headerField("Content-Type"));
}

QMailMessageBody::TransferEncoding QMailMessagePartContainerPrivate::transferEncoding() const
{
    return encodingForName(headerField("Content-Transfer-Encoding"));
}

void QMailMessagePartContainerPrivate::parseMimeSinglePart(const QMailMessageHeader& partHeader, LongString body)
{
    // Create a part to contain this data
    QMailMessagePart part;
    part.setHeader(partHeader, this);

    QMailMessageContentType contentType(part.headerField(QStringLiteral("Content-Type")));
    QMailMessageBody::TransferEncoding encoding = encodingForName(part.headerFieldText(QStringLiteral("Content-Transfer-Encoding")).toLatin1());
    if ( encoding == QMailMessageBody::NoEncoding )
        encoding = QMailMessageBody::SevenBit;

    if ( contentType.type() == "message" ) { // No tr
        // TODO: We can't currently handle these types
    }

    part.setBody(QMailMessageBody::fromLongString(body, contentType, encoding, QMailMessageBody::AlreadyEncoded));

    appendPart(part);
}

void QMailMessagePartContainerPrivate::parseMimeMultipart(const QMailMessageHeader& partHeader, LongString body, bool insertIntoSelf)
{
    static const QByteArray lineFeed(1, QMailMessage::LineFeed);
    static const QByteArray marker("--");

    QMailMessagePart part;
    QMailMessageContentType contentType;
    QByteArray boundary;
    QMailMessagePartContainerPrivate* multipartContainer = Q_NULLPTR;

    if (insertIntoSelf) {
        // Insert the parts into ourself
        multipartContainer = this;
        contentType = QMailMessageContentType(headerField("Content-Type"));
        boundary = _boundary;
    } else {
        // This object already contains part(s) - use a new part to contain the parts
        multipartContainer = privatePointer(part);

        // Parse the header fields, and update the part
        part.setHeader(partHeader, this);
        contentType = QMailMessageContentType(part.headerField(QStringLiteral("Content-Type")));
        boundary = contentType.boundary();
    }

    // Separate the body into parts delimited by the boundary, and parse them individually
    QByteArray partDelimiter = marker + boundary;
    QByteArray partTerminator = lineFeed + partDelimiter + marker;

    int startPos = body.indexOf(partDelimiter, 0);
    if (startPos != -1)
        startPos += partDelimiter.length();

    // Subsequent delimiters include the leading newline
    partDelimiter.prepend(lineFeed);

    int endPos = body.indexOf(partTerminator, 0);

    // Consider CRLF also (which happens to be the standard, so we honor it)
    if (endPos > 1 && body.mid(endPos - 1, 1).indexOf(QByteArray(1, QMailMessage::CarriageReturn)) != -1)
        endPos--;
    
    // invalid message handling: handles truncated multipart messages
    if (endPos == -1)
        endPos = body.length() - 1;
    
    while ((startPos != -1) && (startPos < endPos))
    {
        // Skip the boundary line
        startPos = body.indexOf(lineFeed, startPos);

        if (startPos > 0 && body.mid(startPos - 1, 1).indexOf(QByteArray(1, QMailMessage::CarriageReturn)) != -1)
            startPos--;

        if ((startPos != -1) && (startPos < endPos))
        {
            // Parse the section up to the next boundary marker
            int nextPos = body.indexOf(partDelimiter, startPos);

            // Honor CRLF too...
            if (nextPos > 0 && body.mid(nextPos - 1, 1).indexOf(QByteArray(1, QMailMessage::CarriageReturn)) != -1)
                nextPos--;

            // invalid message handling: handles truncated multipart messages
            if (nextPos == -1)
                nextPos = body.length() - 1;
            
            multipartContainer->parseMimePart(body.mid(startPos, (nextPos - startPos)));

            if (body.mid(nextPos, 1).indexOf(QByteArray(1, QMailMessage::CarriageReturn)) ==0)
                nextPos++;

            // Try the next part
            startPos = nextPos + partDelimiter.length();
        }
    }

    if (part.partCount() > 0) {
        appendPart(part);
    }
}

bool QMailMessagePartContainerPrivate::parseMimePart(LongString body)
{
    static const QByteArray CRLFdelimiter((QByteArray(QMailMessage::CRLF) + QMailMessage::CRLF));
    static const QByteArray CRdelimiter(2, QMailMessage::CarriageReturn);
    static const QByteArray LFdelimiter(2, QMailMessage::LineFeed);

    int CRLFindex = body.indexOf(CRLFdelimiter);
    int LFindex = body.indexOf(LFdelimiter);
    int CRindex = body.indexOf(CRdelimiter);

    int endPos = CRLFindex;
    QByteArray delimiter = CRLFdelimiter;
    if (endPos == -1 || (LFindex > -1 && LFindex < endPos)) {
        endPos = LFindex;
        delimiter = LFdelimiter;
    }
    if (endPos == -1 || (CRindex > -1 && CRindex < endPos)) {
        endPos = CRindex;
        delimiter = CRdelimiter;
    }

    int startPos = 0;

    if (startPos <= endPos) {
        // startPos is the offset of the header, endPos of the delimiter preceding the body
        QByteArray header(body.mid(startPos, endPos - startPos).toQByteArray());

        // Bypass the delimiter
        LongString remainder = body.mid(endPos + delimiter.length());

        QMailMessageHeader partHeader = QMailMessageHeader(header);
        QMailMessageContentType contentType(partHeader.field("Content-Type"));

        // If the content is not available, treat the part as simple
        if (insensitiveEqual(contentType.type(), "multipart") && !remainder.isEmpty()) {
            // Parse the body as a multi-part
            parseMimeMultipart(partHeader, remainder, false);
        } else {
            // Parse the remainder as a single part
            parseMimeSinglePart(partHeader, remainder);
        }
        return true;
    }

    return false;
}

bool QMailMessagePartContainerPrivate::dirty(bool recursive) const
{
    if (_dirty)
        return true;

    if (recursive) {
        foreach (const QMailMessagePart& part, _messageParts)
            if (part.impl<const QMailMessagePartContainerPrivate>()->dirty(true))
                return true;
    }

    return false;
}

void QMailMessagePartContainerPrivate::setDirty(bool value, bool recursive)
{
    _dirty = value;

    if (recursive) {
        const QList<QMailMessagePart>::Iterator end = _messageParts.end();
        for (QList<QMailMessagePart>::Iterator it = _messageParts.begin(); it != end; ++it)
            (*it).impl<QMailMessagePartContainerPrivate>()->setDirty(value, true);
    }
}

bool QMailMessagePartContainerPrivate::previewDirty() const
{
    if (_previewDirty)
        return true;

    const QList<QMailMessagePart>::const_iterator end = _messageParts.end();
    for (QList<QMailMessagePart>::const_iterator it = _messageParts.begin(); it != end; ++it)
        if ((*it).impl<QMailMessagePartContainerPrivate>()->previewDirty())
            return true;

    return false;
}

void QMailMessagePartContainerPrivate::setPreviewDirty(bool value)
{
    _previewDirty = value;

    const QList<QMailMessagePart>::Iterator end = _messageParts.end();
    for (QList<QMailMessagePart>::Iterator it = _messageParts.begin(); it != end; ++it)
        (*it).impl<QMailMessagePartContainerPrivate>()->setPreviewDirty(value);
}

template <typename Stream> 
void QMailMessagePartContainerPrivate::serialize(Stream &stream) const
{
    stream << _multipartType;
    stream << _messageParts;
    stream << _boundary;
    stream << _header;
    stream << _messageId;
    stream << _indices;
    stream << _hasBody;
    if (_hasBody)
        stream << _body;
    stream << _dirty;
    stream << _previewDirty;
}

template <typename Stream> 
void QMailMessagePartContainerPrivate::deserialize(Stream &stream)
{
    stream >> _multipartType;
    stream >> _messageParts;
    stream >> _boundary;
    stream >> _header;
    stream >> _messageId;
    stream >> _indices;
    stream >> _hasBody;
    if (_hasBody)
        stream >> _body;
    stream >> _dirty;
    stream >> _previewDirty;
}


/*!
    \class QMailMessagePartContainer

    \preliminary
    \brief The QMailMessagePartContainer class provides access to a collection of message parts.
    
    \ingroup messaginglibrary
   
    Message formats such as email messages conforming to 
    \l{http://www.ietf.org/rfc/rfc2822.txt} {RFC 2822} (Internet Message Format) can consist of 
    multiple independent parts, whose relationship to each other is defined by the message that 
    contains those parts.  The QMailMessagePartContainer class provides storage for these related 
    message parts, and the interface through which they are accessed.

    The multipartType() function returns a member of the MultipartType enumeration, which 
    describes the relationship of the parts in the container to each other.

    The part container can instead contain a message body element.  In this case, it cannot contain
    sub-parts, and the multipartType() function will return MultipartType::MultipartNone for the part. 
    The body element can be accessed via the body() function.

    The QMailMessagePart class is itself derived from QMailMessagePartContainer, which allows
    messages to support the nesting of part collections within other part collections.

    \sa QMailMessagePart, QMailMessage, QMailMessageBody
*/    

/*!
    \typedef QMailMessagePartContainer::ImplementationType
    \internal
*/

/*!
    \fn QMailMessagePartContainer::QMailMessagePartContainer(Subclass*)

    Constructs an empty part container object, in the space allocated 
    within the subclass instance at \a p.
*/
template<typename Subclass>
QMailMessagePartContainer::QMailMessagePartContainer(Subclass* p)
    : QPrivatelyImplemented<QMailMessagePartContainerPrivate>(p)
{
}

/*! \internal */
void QMailMessagePartContainer::setHeader(const QMailMessageHeader& partHeader, const QMailMessagePartContainerPrivate* parent)
{
    impl(this)->setHeader(partHeader, parent);
}

/*!
    Returns the number of attachments the message has.
*/  
uint QMailMessagePartContainer::partCount() const
{
    return impl(this)->_messageParts.count();
}

/*!
    Append \a part to the list of attachments for the message.
*/
void QMailMessagePartContainer::appendPart(const QMailMessagePart &part)
{
    impl(this)->appendPart(part);
}

/*!
    Prepend \a part to the list of attachments for the message.
*/
void QMailMessagePartContainer::prependPart(const QMailMessagePart &part)
{
    impl(this)->prependPart(part);
}

/*!
    Removes the part at the index \a pos.

    \a pos must be a valid index position in the list (i.e., 0 <= i < partCount()).
*/
void QMailMessagePartContainer::removePartAt(uint pos)
{
    impl(this)->removePartAt(pos);
}

/*!
    Returns a const reference to the item at position \a pos in the list of 
    attachments for the message.

    \a pos must be a valid index position in the list (i.e., 0 <= i < partCount()).
*/
const QMailMessagePart& QMailMessagePartContainer::partAt(uint pos) const
{
    return impl(this)->_messageParts[pos];
}

/*!
    Returns a non-const reference to the item at position \a pos in the list of 
    attachments for the message.

    \a pos must be a valid index position in the list (i.e., 0 <= i < partCount()).
*/
QMailMessagePart& QMailMessagePartContainer::partAt(uint pos)
{
    return impl(this)->_messageParts[pos];
}

/*!
    Clears the list of attachments associated with the message.
*/
void QMailMessagePartContainer::clearParts()
{
    impl(this)->clear();
}

/*!
    Returns the type of multipart relationship shared by the parts contained within this container, or
    \l {QMailMessagePartContainerFwd::MultipartNone}{MultipartNone} if the content is not a multipart message.
*/  
QMailMessagePartContainer::MultipartType QMailMessagePartContainer::multipartType() const
{
    return impl(this)->_multipartType;
}

/*!
    Sets the multipart state of the message to \a type.
*/
void QMailMessagePartContainer::setMultipartType(QMailMessagePartContainer::MultipartType type, const QList<QMailMessageHeaderField::ParameterType> &parameters)
{
    impl(this)->setMultipartType(type, parameters);
}

/*!
    Returns the boundary text used to delimit the container's parts when encoded in RFC 2822 form.
*/
QByteArray QMailMessagePartContainer::boundary() const
{
    return impl(this)->boundary();
}

/*!
    Sets the boundary text used to delimit the container's parts when encoded in RFC 2822 form to \a text.
*/
void QMailMessagePartContainer::setBoundary(const QByteArray& text)
{
    impl(this)->setBoundary(text);
}

/*!
    Returns the Content-Description header field for the part, if present;
    otherwise returns an empty string.
*/
QString QMailMessagePartContainer::contentDescription() const
{
    return headerFieldText(QLatin1String("Content-Description"));
}

/*!
    Sets the Content-Description header field for the part to contain \a description.
*/
void QMailMessagePartContainer::setContentDescription(const QString &description)
{
    setHeaderField(QStringLiteral("Content-Description"), description);
}

/*!
    Returns the Content-Disposition header field for the part.
*/
QMailMessageContentDisposition QMailMessagePartContainer::contentDisposition() const
{
    return QMailMessageContentDisposition(headerField(QStringLiteral("Content-Disposition")));
}

/*!
    Sets the Content-Disposition header field for the part to contain \a disposition.
*/
void QMailMessagePartContainer::setContentDisposition(const QMailMessageContentDisposition &disposition)
{
    setHeaderField(QStringLiteral("Content-Disposition"), QString::fromUtf8(disposition.toString(false, false)));
}

/*!
    Sets the part to contain the body element \a body, \a encodingStatus describes the current status of \a body regarding encoding.
    Note: No encoding/decoding operation will be performed in the body element, only the encoding status flag
    will be set if provided.
*/
void QMailMessagePartContainer::setBody(const QMailMessageBody& body, QMailMessageBody::EncodingFormat encodingStatus)
{
    impl(this)->setBody(body, encodingStatus);
}

/*!
    Returns the body element contained by the part.
*/
QMailMessageBody QMailMessagePartContainer::body() const
{
    return impl(this)->body();
}

/*!
    Returns true if the part contains a body element; otherwise returns false.
*/
bool QMailMessagePartContainer::hasBody() const
{
    return impl(this)->hasBody();
}

/*!
    Returns the content type of this part.  Where hasBody() is true, the type of the
    contained body element is returned; otherwise a content type matching the 
    multipartType() for this part is returned.

    \sa hasBody(), QMailMessageBody::contentType(), multipartType()
*/
QMailMessageContentType QMailMessagePartContainer::contentType() const
{
    return impl(this)->contentType();
}

/*!
    Returns the transfer encoding type of this part.  Where hasBody() is true, the
    transfer encoding type of the contained body element is returned; otherwise, the
    transfer encoding type specified by the 'Content-Transfer-Encoding' field of the 
    header for this part is returned.

    \sa hasBody(), QMailMessageBody::transferEncoding()
*/
QMailMessageBody::TransferEncoding QMailMessagePartContainer::transferEncoding() const
{
    return impl(this)->transferEncoding();
}

/*!
    Returns the text of the first header field with the given \a id.
*/
QString QMailMessagePartContainer::headerFieldText( const QString &id ) const
{
    return impl(this)->headerFieldText(id);
}

/*!
    Returns an object containing the value of the first header field with the given \a id.
    If \a fieldType is QMailMessageHeaderField::StructuredField, then the field content 
    will be parsed assuming a format equivalent to that used for the RFC 2045 'Content-Type' 
    and RFC 2183 'Content-Disposition' header fields.
*/
QMailMessageHeaderField QMailMessagePartContainer::headerField( const QString &id, QMailMessageHeaderField::FieldType fieldType ) const
{
    QByteArray plainId( to7BitAscii(id) );
    const QByteArray& content = impl(this)->headerField( plainId );
    if ( !content.isEmpty() )
        return QMailMessageHeaderField( plainId, content, fieldType );

    return QMailMessageHeaderField();
}

/*!
    Returns a list containing the text of each header field with the given \a id.
*/
QStringList QMailMessagePartContainer::headerFieldsText( const QString &id ) const
{
    QStringList result;

    foreach (const QByteArray& item, impl(this)->headerFields( to7BitAscii(id) ))
        result.append(decodedContent( id, item ));

    return result;
}

/*!
    Returns a list of objects containing the value of each header field with the given \a id.
    If \a fieldType is QMailMessageHeaderField::StructuredField, then the field content will 
    be parsed assuming a format equivalent to that used for the RFC 2045 'Content-Type' and 
    RFC 2183 'Content-Disposition' header fields.
*/
QList<QMailMessageHeaderField> QMailMessagePartContainer::headerFields( const QString &id, QMailMessageHeaderField::FieldType fieldType ) const
{
    QList<QMailMessageHeaderField> result;

    QByteArray plainId( to7BitAscii(id) );
    foreach (const QByteArray& content, impl(this)->headerFields( plainId ))
        result.append( QMailMessageHeaderField( plainId, content, fieldType ) );

    return result;
}

/*!
    Returns a list of objects containing the value of each header field contained by the part.
    Header field objects returned by this function are not 'structured'.
*/
QList<QMailMessageHeaderField> QMailMessagePartContainer::headerFields() const
{
    QList<QMailMessageHeaderField> result;

    foreach (const QByteArray& content, impl(this)->headerFields())
        result.append( QMailMessageHeaderField( content, QMailMessageHeaderField::UnstructuredField) );

    return result;
}

/*!
    Sets the value of the first header field with identity \a id to \a value if it already 
    exists; otherwise adds the header with the supplied id and value.  If \a value is of 
    the form "<id>:<content>", then only the part after the semi-colon is processed.

    RFC 2822 encoding requires header fields to be transmitted in ASCII characters.  
    If \a value contains non-ASCII characters, it will be encoded to ASCII via the 
    QMailMessageHeaderField::encodeContent() function; depending on the specific header 
    field this may result in illegal content.  Where possible, clients should encode 
    non-ASCII data prior to calling setHeaderField.

    \sa QMailMessageHeaderField
*/
void QMailMessagePartContainer::setHeaderField( const QString& id, const QString& value )
{
    QByteArray plainId( to7BitAscii(id) );

    int index = value.indexOf(':');
    if (index != -1 ) {
        // Is the header field id replicated in the value?
        QString prefix(value.left(index));
        if ( insensitiveEqual( to7BitAscii(prefix.trimmed()), plainId.trimmed() ) ) {
            impl(this)->updateHeaderField( plainId, value.mid(index + 1) );
            return;
        }
    }

    impl(this)->updateHeaderField( plainId, value );
}

/*!
    Sets the first header field with identity matching \a field to have the content of
    \a field.
*/
void QMailMessagePartContainer::setHeaderField( const QMailMessageHeaderField& field )
{
    impl(this)->updateHeaderField( field.id(), field.toString(false, false) );
}

/*!
    Appends a new header field with id \a id and value \a value to the existing
    list of header fields. If the \a id should be present only once according
    to RFC2822 and is already existing, it will be updated instead of appended.
    If \a value is of the form "<id>:<content>", then only the part after the 
    semi-colon is processed.

    RFC 2822 encoding requires header fields to be transmitted in ASCII characters.  
    If \a value contains non-ASCII characters, it will be encoded to ASCII via the 
    QMailMessageHeaderField::encodeContent() function; depending on the specific header 
    field this may result in illegal content.  Where possible, clients should encode 
    non-ASCII data prior to calling appendHeaderField.

    \sa QMailMessageHeaderField
*/
void QMailMessagePartContainer::appendHeaderField( const QString& id, const QString& value )
{
    QByteArray plainId( to7BitAscii(id) );

    int index = value.indexOf(':');
    if (index != -1 ) {
        // Is the header field id replicated in the value?
        QString prefix(value.left(index));
        if ( insensitiveEqual( to7BitAscii(prefix.trimmed()), plainId.trimmed() ) ) {
            impl(this)->appendHeaderField( plainId, value.mid(index + 1) );
            return;
        }
    }

    impl(this)->appendHeaderField( plainId, value );
}

/*!
    Appends a new header field with the properties of \a field. If the \a id
    should be present only once according to RFC2822 and is already existing,
    it will be updated instead of appended.
*/
void QMailMessagePartContainer::appendHeaderField( const QMailMessageHeaderField& field )
{
    impl(this)->appendHeaderField( field.id(), field.toString(false, false) );
}

/*!
    Removes all existing header fields with id equal to \a id.
*/
void QMailMessagePartContainer::removeHeaderField( const QString& id )
{
    impl(this)->removeHeaderField( to7BitAscii(id) );
}

/*!
    Returns the multipart type that corresponds to the type name \a name.
*/
QMailMessagePartContainer::MultipartType QMailMessagePartContainer::multipartTypeForName(const QByteArray &name)
{
    QByteArray ciName = name.toLower();

    if ((ciName == "multipart/signed") || (ciName == "signed"))
        return QMailMessagePartContainer::MultipartSigned;

    if ((ciName == "multipart/encrypted") || (ciName == "encrypted"))
        return QMailMessagePartContainer::MultipartEncrypted;

    if ((ciName == "multipart/mixed") || (ciName == "mixed"))
        return QMailMessagePartContainer::MultipartMixed;

    if ((ciName == "multipart/alternative") || (ciName == "alternative"))
        return QMailMessagePartContainer::MultipartAlternative;

    if ((ciName == "multipart/digest") || (ciName == "digest"))
        return QMailMessagePartContainer::MultipartDigest;

    if ((ciName == "multipart/parallel") || (ciName == "parallel"))
        return QMailMessagePartContainer::MultipartParallel;

    if ((ciName == "multipart/related") || (ciName == "related"))
        return QMailMessagePartContainer::MultipartRelated;

    if ((ciName == "multipart/form") || (ciName == "form"))
        return QMailMessagePartContainer::MultipartFormData;

    if ((ciName == "multipart/report") || (ciName == "report"))
        return QMailMessagePartContainer::MultipartReport;

    return QMailMessagePartContainer::MultipartNone;
}

/*!
    Returns the standard textual representation for the multipart type \a type.
*/
QByteArray QMailMessagePartContainer::nameForMultipartType(QMailMessagePartContainer::MultipartType type)
{
    switch (type) 
    {
        case QMailMessagePartContainer::MultipartSigned:
        {
            return "multipart/signed";
        }
        case QMailMessagePartContainer::MultipartEncrypted:
        {
            return "multipart/encrypted";
        }
        case QMailMessagePartContainer::MultipartMixed:
        {
            return "multipart/mixed";
        }
        case QMailMessagePartContainer::MultipartAlternative:
        {
            return "multipart/alternative";
        }
        case QMailMessagePartContainer::MultipartDigest:
        {
            return "multipart/digest";
        }
        case QMailMessagePartContainer::MultipartParallel:
        {
            return "multipart/parallel";
        }
        case QMailMessagePartContainer::MultipartRelated:
        {
            return "multipart/related";
        }
        case QMailMessagePartContainer::MultipartFormData:
        {
            return "multipart/form-data";
        }
        case QMailMessagePartContainer::MultipartReport:
        {
            return "multipart/report";
        }
        case QMailMessagePartContainer::MultipartNone:
            break;
    }

    return QByteArray();
}

/*!
  Searches for the container that encapsulates the plain text body of this container, returning a pointer to it or 0 if it's not present.
 */
QMailMessagePartContainer* QMailMessagePartContainer::findPlainTextContainer() const
{
    QMailMessagePartContainer* result;
    findBody::Context ctx;

    ctx.contentSubtype = plainContentSubtype;

    if (findBody::inPartContainer(*this, ctx)) {
        result = ctx.found;
    } else {
        result = Q_NULLPTR;
    }
    return result;
}

/*!
  Searches for the container that encapsulates the HTML body of this container, returning a pointer to it or 0 if it's not present.
 */
QMailMessagePartContainer* QMailMessagePartContainer::findHtmlContainer() const
{
    QMailMessagePartContainer* result;
    findBody::Context ctx;
    ctx.contentSubtype = htmlContentSubtype;

    if (findBody::inPartContainer(*this, ctx)) {
        result = ctx.found;
    } else {
        result = Q_NULLPTR;
    }
    return result;
}

/*!
  Returns the locations of the attachments in a container, dealing with a range of different message structures and exceptions.
 */
QList<QMailMessagePart::Location> QMailMessagePartContainer::findAttachmentLocations() const
{
    QList<QMailMessagePart::Location> found;

    foreach (const findAttachments::AttachmentFindStrategy* strategy, findAttachments::allStrategies) {
        if (strategy->findAttachmentLocations(*this, &found, Q_NULLPTR)) {
            break;
        } else {
            found = QList<QMailMessagePart::Location>();
        }
    }
    return found;
}

/*!
  Returns the locations of the inline images in a HTML body container, only parts with content type "image" will be returned.
  Note that sometimes inline images content type is not defined or is other than "image".

  \sa findInlinePartLocations()
 */
QList<QMailMessagePart::Location> QMailMessagePartContainer::findInlineImageLocations() const
{
    findBody::Context ctx;
    ctx.contentSubtype = htmlContentSubtype;
    if (findBody::inPartContainer(*this, ctx)) {
        return ctx.htmlImageLoc;
    } else {
        return QList<QMailMessagePart::Location>();
    }
}

/*!
  Returns the locations of the inline parts in a HTML body container, only parts with a content id reference will be returned.
 */
QList<QMailMessagePart::Location> QMailMessagePartContainer::findInlinePartLocations() const
{
    findBody::Context ctx;
    ctx.contentSubtype = htmlContentSubtype;
    if (findBody::inPartContainer(*this, ctx)) {
        return ctx.htmlImageLoc << ctx.htmlExtraPartsLoc;
    } else {
        return QList<QMailMessagePart::Location>();
    }
}

/*!
  Returns true if a plain text body is present in the container.
 */
bool QMailMessagePartContainer::hasPlainTextBody() const
{
    return (findPlainTextContainer() != Q_NULLPTR);
}

/*!
  Returns true if an HTML body is present in the container.
 */
bool QMailMessagePartContainer::hasHtmlBody() const
{
    return (findHtmlContainer() != Q_NULLPTR);
}

/*!
  Returns true if attachments are present in the container, dealing with a range of different message structures and exceptions.
 */
bool QMailMessagePartContainer::hasAttachments() const
{
    bool hasAttachments;
    foreach (const findAttachments::AttachmentFindStrategy* strategy, findAttachments::allStrategies) {
        if (strategy->findAttachmentLocations(*this, Q_NULLPTR, &hasAttachments)) {
            return hasAttachments;
        }
    }
    return false;
}

/*!
  Sets the plain text body of a container to \a plainTextBody.
 */
void QMailMessagePartContainer::setPlainTextBody(const QMailMessageBody& plainTextBody)
{
    findBody::Context ctx;
    if (findBody::inPartContainer(*this, ctx)) {
        if (Q_NULLPTR == ctx.alternateParent) {
            ctx.found->setBody(plainTextBody);
        } else {
            ctx.alternateParent->clearParts();
            ctx.alternateParent->setMultipartType(QMailMessagePartContainer::MultipartNone);
            ctx.alternateParent->setBody(plainTextBody);
        }
    } else {
        if (partCount() == 0) {
            setMultipartType(QMailMessagePartContainer::MultipartNone);
            setBody(plainTextBody);
        } else {
            setMultipartType(QMailMessagePartContainer::MultipartMixed);
            QMailMessagePart plainTextPart;
            plainTextPart.setBody(plainTextBody);
            prependPart(plainTextPart);
        }
    }
}

/*!
  Simultaneously sets the html and plain text body of a container to \a htmlBody and \a plainTextBody respectively.
 */
void QMailMessagePartContainer::setHtmlAndPlainTextBody(const QMailMessageBody& htmlBody, const QMailMessageBody& plainTextBody)
{
    QMailMessagePartContainer *bodyContainer = Q_NULLPTR;
    QMailMessagePart subpart;
    bool hasInlineImages = false;

    findBody::Context ctx;
    if (findBody::inPartContainer(*this, ctx)) {
        Q_ASSERT (Q_NULLPTR != ctx.found);
        hasInlineImages = !ctx.htmlImageParts.isEmpty();
        if (Q_NULLPTR != ctx.alternateParent) {
            bodyContainer = ctx.alternateParent;
        } else {
            bodyContainer = ctx.found;
        }
        if (hasInlineImages) {
            // Copy relevant data of the message to subpart
            subpart.setMultipartType(QMailMessagePartContainer::MultipartRelated);
            foreach (const QMailMessagePart *part, ctx.htmlImageParts) {
                subpart.appendPart(*part);
            }
        }
        bodyContainer->clearParts();
    } else {
        switch (multipartType()) {
        case QMailMessagePartContainer::MultipartNone:
            bodyContainer = this;
            break;
        case QMailMessagePartContainer::MultipartMixed:
            // Message with attachments but still without body (eg: being
            // forwarded, still in composing stage)
            // We make room for the new body container
            prependPart(QMailMessagePart());
            bodyContainer = &partAt(0);
            break;
        default:
            qWarning() << Q_FUNC_INFO << "Wrong multipart type: " << multipartType();
            Q_ASSERT(false);
            break;
        }
    }
    Q_ASSERT (NULL != bodyContainer);
    bodyContainer->setMultipartType(QMailMessagePartContainer::MultipartAlternative);

    QMailMessagePart plainTextBodyPart;
    plainTextBodyPart.setBody(plainTextBody);
    bodyContainer->appendPart(plainTextBodyPart);

    QMailMessagePart htmlBodyPart;
    htmlBodyPart.setBody(htmlBody);
    if (hasInlineImages){
        subpart.prependPart(htmlBodyPart);
        bodyContainer->appendPart(subpart);
    } else {
        bodyContainer->appendPart(htmlBodyPart);
    }
}

/*!
  Sets the image map of a container to \a htmlImagesMap.
  \a htmlImagesMap String paths to local files to be added
 */
void QMailMessagePartContainer::setInlineImages(const QMap<QString, QString> &htmlImagesMap)
{
    attachments::removeAllInlineImages(*this);

    if (htmlImagesMap.isEmpty()) {
        return;
    }

    // Add attachments
    attachments::convertToMultipartRelated(*this);
    attachments::addImagesToMultipart(this, htmlImagesMap);
}

/*!
  Sets the images list of a container to \a imageParts.
  \a imageParts List of already created message parts representing the inline images (might come from other existing messages)
*/
void QMailMessagePartContainer::setInlineImages(const QList<const QMailMessagePart*> imageParts)
{
    attachments::removeAllInlineImages(*this);

    if (imageParts.isEmpty()) {
        return;
    }

    // Add attachments
    attachments::convertToMultipartRelated(*this);
    attachments::addImagesToMultipart(this, imageParts);
}

/*!
  Sets the attachment list of a container to \a attachments.
  \a attachments String paths to local files to be attached
 */
void QMailMessagePartContainer::setAttachments(const QStringList& attachments)
{
    attachments::removeAll(*this);

    if (attachments.isEmpty()) {
        return;
    }

    // Add attachments
    if (multipartType() != QMailMessagePartContainer::MultipartMixed) {
        // Convert the message to multipart/mixed.
        QMailMessagePart subpart;
        if (multipartType() == QMailMessagePartContainer::MultipartNone) {
            // Move the body
            subpart.setBody(body());
        } else {
            // Copy relevant data of the message to subpart
            subpart.setMultipartType(multipartType());
            for (uint i=0; i < partCount(); i++) {
                subpart.appendPart(partAt(i));
            }
        }
        clearParts();
        setMultipartType(QMailMessagePartContainer::MultipartMixed);
        appendPart(subpart);
    }
    attachments::addAttachmentsToMultipart(this, attachments);
}

/*!
  Sets the attachment list of a container to \a attachments.
  \a attachments String paths to local files to be attached of already created message
   parts representing the attachments (might come from other existing messages)
 */
void QMailMessagePartContainer::setAttachments(const QList<const QMailMessagePart*> attachments)
{
    attachments::removeAll(*this);

    if (attachments.isEmpty()) {
        return;
    }

    // Add attachments
    attachments::convertMessageToMultipart(*this);
    attachments::addAttachmentsToMultipart(this, attachments);
}

/*!
  Sets the attachment list of a container to \a attachments.
  \a attachments String paths to local files to be attached of already created message
   parts representing the attachments (might come from other existing messages)
 */
void QMailMessagePartContainer::addAttachments(const QStringList& attachments)
{
    if (attachments.isEmpty()) {
        return;
    }

    // Add attachments
    attachments::convertMessageToMultipart(*this);
    attachments::addAttachmentsToMultipart(this, attachments);
}

/*! \internal */
uint QMailMessagePartContainer::indicativeSize() const
{
    return impl(this)->indicativeSize();
}

struct DummyChunkProcessor
{
    void operator()(QMailMessage::ChunkType) {}
};

/*! \internal */
void QMailMessagePartContainer::outputParts(QDataStream& out, bool addMimePreamble, bool includeAttachments, bool excludeInternalFields) const
{
    QDataStream* ds(&out);
    impl(this)->outputParts<DummyChunkProcessor>(&ds, addMimePreamble, includeAttachments, excludeInternalFields, Q_NULLPTR);
}

/*! \internal */
void QMailMessagePartContainer::outputBody( QDataStream& out, bool includeAttachments ) const
{
    impl(this)->outputBody( out, includeAttachments );
}

/*!
    \fn QMailMessagePartContainer::contentAvailable() const

    Returns true if the entire content of this element is available; otherwise returns false.
*/

/*!
    \fn QMailMessagePartContainer::partialContentAvailable() const

    Returns true if some portion of the content of this element is available; otherwise returns false.
*/


/* QMailMessagePart */

QMailMessagePartPrivate::QMailMessagePartPrivate()
    : QMailMessagePartContainerPrivate(this)
{
}

QMailMessagePart::ReferenceType QMailMessagePartPrivate::referenceType() const
{
    if (_referenceId.isValid())
        return QMailMessagePart::MessageReference;
        
    if (_referenceLocation.isValid())
        return QMailMessagePart::PartReference;

    return QMailMessagePart::None;
}

QMailMessageId QMailMessagePartPrivate::messageReference() const
{
    return _referenceId;
}

QMailMessagePart::Location QMailMessagePartPrivate::partReference() const
{
    return _referenceLocation;
}

QString QMailMessagePartPrivate::referenceResolution() const
{
    return _resolution;
}

void QMailMessagePartPrivate::setReferenceResolution(const QString &uri)
{
    _resolution = uri;
}

bool QMailMessagePartPrivate::contentAvailable() const
{
    if (_multipartType != QMailMessage::MultipartNone)
        return true;

    if (_body.isEmpty())
        return false;

    // Complete content is available only if the 'partial-content' header field is not present
    QByteArray fieldName(internalPrefix() + "partial-content");
    return (headerField(fieldName).isEmpty());
}

bool QMailMessagePartPrivate::partialContentAvailable() const
{
    return ((_multipartType != QMailMessage::MultipartNone) || !_body.isEmpty());
}

const QByteArray QMailMessagePartPrivate::undecodedData() const
{
    return _undecodedData;
}

void QMailMessagePartPrivate::setUndecodedData(const QByteArray &data)
{
    _undecodedData = data;
    setDirty();
}

void QMailMessagePartPrivate::appendUndecodedData(const QByteArray &data)
{
    _undecodedData.append(data);
    setDirty();
}

template <typename F>
void QMailMessagePartPrivate::output(QDataStream **out, bool addMimePreamble, bool includeAttachments, bool excludeInternalFields, F *func) const
{
    static const DataString newLine('\n');

    // Ensure that pristine data are used when writing the mail
    // because some header ordering, case... may have been altered
    // while parsing the mail.
    if (includeAttachments && excludeInternalFields && !_undecodedData.isEmpty()) {
        **out << DataString(_undecodedData);
        return;
    }

    _header.output( **out, QList<QByteArray>(), excludeInternalFields );
    **out << DataString('\n');

    QMailMessagePart::ReferenceType type(referenceType());
    if (type == QMailMessagePart::None) {
        if ( hasBody() ) {
            outputBody( **out, includeAttachments );
        } else {
            outputParts<F>( out, addMimePreamble, includeAttachments, excludeInternalFields, func );
        }
    } else {
        if (includeAttachments) {
            // The next part must be a different chunk
            if (func) {
                (*func)(QMailMessage::Text);
            }

            if (!_resolution.isEmpty()) {
                **out << DataString(_resolution.toLatin1());
            } else {
                qWarning() << "QMailMessagePartPrivate::output - unresolved reference part!";
            }

            if (func) {
                (*func)(QMailMessage::Reference);
            }
        }
    }
}

template <typename Stream> 
void QMailMessagePartPrivate::serialize(Stream &stream) const
{
    QMailMessagePartContainerPrivate::serialize(stream);

    stream << _referenceId;
    stream << _referenceLocation;
    stream << _resolution;
}

template <typename Stream> 
void QMailMessagePartPrivate::deserialize(Stream &stream)
{
    QMailMessagePartContainerPrivate::deserialize(stream);

    stream >> _referenceId;
    stream >> _referenceLocation;
    stream >> _resolution;
}

void QMailMessagePartPrivate::setReference(const QMailMessageId &id, 
                                           const QMailMessageContentType& type, 
                                           QMailMessageBody::TransferEncoding encoding)
{
    _referenceId = id;
    setBodyProperties(type, encoding);
}

void QMailMessagePartPrivate::setReference(const QMailMessagePart::Location &location, 
                                           const QMailMessageContentType& type, 
                                           QMailMessageBody::TransferEncoding encoding)
{
    _referenceLocation = location;
    setBodyProperties(type, encoding);
}

bool QMailMessagePartPrivate::contentModified() const
{
    // Specific to this part
    return dirty(false);
}

void QMailMessagePartPrivate::setUnmodified()
{
    setDirty(false, false);
}

QMailMessagePartContainerPrivate* QMailMessagePartContainerPrivate::privatePointer(QMailMessagePart& part)
{
    /* Nasty workaround required to access this data without detaching a copy... */
    return const_cast<QMailMessagePartPrivate*>(static_cast<const QMailMessagePartPrivate*>(part.d.constData()));
}

/*!
    \fn bool QMailMessagePartContainer::foreachPart(F func)
    
    Applies the function or functor \a func to each part contained within the container.
    \a func must implement the signature 'bool operator()(QMailMessagePart &)', and must
    return true to indicate success, or false to end the traversal operation.

    Returns true if all parts of the message were traversed, and \a func returned true for
    every invocation; else returns false.
*/

/*!
    \fn bool QMailMessagePartContainer::foreachPart(F func) const
    
    Applies the function or functor \a func to each part contained within the container.
    \a func must implement the signature 'bool operator()(const QMailMessagePart &)', and must
    return true to indicate success, or false to end the traversal operation.

    Returns true if all parts of the message were traversed, and \a func returned true for
    every invocation; else returns false.
*/


//===========================================================================
/*      Mail Message Part   */

/*!
    \class QMailMessagePart
    \preliminary

    \brief The QMailMessagePart class provides a convenient interface for working 
    with message attachments.

    \ingroup messaginglibrary
    
    A message part inherits the properties of QMailMessagePartContainer, and can 
    therefore contain a message body or a collection of sub-parts.  
    
    A message part differs from a message proper in that a part will often have 
    properties specified by the MIME multipart specification, not relevant to 
    messages.  These include the 'name' and 'filename' parameters of the Content-Type 
    and Content-Disposition fields, and the Content-Id and Content-Location fields.

    A message part may consist entirely of a reference to an external message, or
    a part within an external message.  Parts that consists of references may be 
    used with some protocols that permit data to be transmitted by reference, such
    as IMAP with the URLAUTH extension.  Not all messaging protocols support the
    use of content references. The partReference() and messageReference() functions
    enable the creation of reference parts.

    \sa QMailMessagePartContainer
*/

/*!
    \typedef QMailMessagePart::ImplementationType
    \internal
*/


/*!
    \class QMailMessagePartContainer::Location
    \preliminary

    \brief The Location class contains a specification of the location of a message part
    with the message that contains it.

    \ingroup messaginglibrary
    
    A Location object is used to refer to a single part within a multi-part message.  The
    location can be used to reference a part within a QMailMessage object, via the 
    \l{QMailMessage::partAt()}{partAt} function.
*/

/*!
    Creates an empty part location object.
*/
QMailMessagePartContainer::Location::Location()
    : d(new QMailMessagePartContainer::LocationPrivate)
{
}

/*!
    Creates a part location object referring to the location given by \a description.

    \sa toString()
*/
QMailMessagePartContainer::Location::Location(const QString& description)
    : d(new QMailMessagePartContainer::LocationPrivate)
{
    QString indices;

    int separator = description.indexOf('-');
    if (separator != -1) {
        d->_messageId = QMailMessageId(description.left(separator).toULongLong());
        indices = description.mid(separator + 1);
    } else {
        indices = description;
    }

    if (!indices.isEmpty()) {
        foreach (const QString &index, indices.split('.')) {
            d->_indices.append(index.toUInt());
        }
    }

    Q_ASSERT(description == toString(separator == -1 ? false : true));
}

/*!
    Creates a part location object containing a copy of \a other.
*/
QMailMessagePartContainer::Location::Location(const Location& other)
    : d(new QMailMessagePartContainer::LocationPrivate)
{
    *this = other;
}

/*!
    Creates a location object containing the location of \a part.
*/
QMailMessagePartContainer::Location::Location(const QMailMessagePart& part)
    : d(new QMailMessagePartContainer::LocationPrivate)
{
    const QMailMessagePartContainerPrivate* partImpl = part.impl<const QMailMessagePartContainerPrivate>();

    d->_messageId = partImpl->_messageId;
    d->_indices = partImpl->_indices;
}

/*! \internal */
QMailMessagePartContainer::Location::~Location()
{
    delete d;
}

/*! \internal */
const QMailMessagePartContainer::Location &QMailMessagePartContainer::Location::operator=(const QMailMessagePartContainer::Location &other)
{
    d->_messageId = other.d->_messageId;
    d->_indices = other.d->_indices;

    return *this;
}

/*! Returns true if \a other is referring to the same location */
bool QMailMessagePartContainer::Location::operator==(const QMailMessagePartContainer::Location &other) const
{
    return toString(true) == other.toString(true);
}

bool QMailMessagePartContainer::Location::operator!=(const QMailMessagePartContainer::Location &other) const
{
  return !(*this == other);
}

/*! 
    Returns true if the location object contains the location of a valid message part.
    If \a extended is true, the location must also contain a valid message identifier.
*/
bool QMailMessagePartContainer::Location::isValid(bool extended) const
{
    return ((!extended || d->_messageId.isValid()) && !d->_indices.isEmpty());
}

/*! 
    Returns the identifier of the message that contains the part with this location.
*/
QMailMessageId QMailMessagePartContainer::Location::containingMessageId() const
{
    return d->_messageId;
}

/*! 
    Sets the identifier of the message that contains the part with this location to \a id.
*/
void QMailMessagePartContainer::Location::setContainingMessageId(const QMailMessageId &id)
{
    d->_messageId = id;
}

/*!
    Returns a textual representation of the part location.
    If \a extended is true, the representation contains the identifier of the containing message.
*/
QString QMailMessagePartContainer::Location::toString(bool extended) const
{
    QString result;
    if (extended)
        result = QString::number(d->_messageId.toULongLong()) % QStringLiteral("-");

    QStringList numbers;
    foreach (uint index, d->_indices)
        numbers.append(QString::number(index));

    return result.append(numbers.join(QString('.')));
}

/*! 
    \fn QMailMessagePartContainer::Location::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailMessagePartContainer::Location::serialize(Stream &stream) const
{
    stream << d->_messageId;
    stream << d->_indices;
}

template void QMailMessagePartContainer::Location::serialize(QDataStream &) const;

/*! 
    \fn QMailMessagePartContainer::Location::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessagePartContainer::Location::deserialize(Stream &stream)
{
    stream >> d->_messageId;
    stream >> d->_indices;
}

template void QMailMessagePartContainer::Location::deserialize(QDataStream &);

/*!
    Constructs an empty message part object.
*/
QMailMessagePart::QMailMessagePart()
    : QMailMessagePartContainer(new QMailMessagePartPrivate)
{
}

/*!
   Creates a QMailMessagePart containing an attachment of type \a disposition, from the 
   data contained in \a filename, of content type \a type and using the transfer encoding
   \a encoding.  The current status of the data is specified as \a status.

   \sa QMailMessageBody::fromFile()
*/
QMailMessagePart QMailMessagePart::fromFile(const QString& filename,
                                            const QMailMessageContentDisposition& disposition, 
                                            const QMailMessageContentType& type, 
                                            QMailMessageBody::TransferEncoding encoding, 
                                            QMailMessageBody::EncodingStatus status)
{
    QMailMessagePart part;
    part.setBody( QMailMessageBody::fromFile( filename, type, encoding, status ) );
    part.setContentDisposition( disposition );

    return part;
}

/*!
   Creates a QMailMessagePart containing an attachment of type \a disposition, from the 
   data read from \a in, of content type \a type and using the transfer encoding
   \a encoding.  The current status of the data is specified as \a status.

   \sa QMailMessageBody::fromStream()
*/
QMailMessagePart QMailMessagePart::fromStream(QDataStream& in,
                                              const QMailMessageContentDisposition& disposition, 
                                              const QMailMessageContentType& type, 
                                              QMailMessageBody::TransferEncoding encoding, 
                                              QMailMessageBody::EncodingStatus status)
{
    QMailMessagePart part;
    part.setBody( QMailMessageBody::fromStream( in, type, encoding, status ) );
    part.setContentDisposition( disposition );

    return part;
}

/*!
   Creates a QMailMessagePart containing an attachment of type \a disposition, from the 
   data contained in \a input, of content type \a type and using the transfer encoding
   \a encoding.  The current status of the data is specified as \a status.

   \sa QMailMessageBody::fromData()
*/
QMailMessagePart QMailMessagePart::fromData(const QByteArray& input,
                                            const QMailMessageContentDisposition& disposition, 
                                            const QMailMessageContentType& type, 
                                            QMailMessageBody::TransferEncoding encoding, 
                                            QMailMessageBody::EncodingStatus status)
{
    QMailMessagePart part;
    part.setBody( QMailMessageBody::fromData( input, type, encoding, status ) );
    part.setContentDisposition( disposition );

    return part;
}

/*!
   Creates a QMailMessagePart containing an attachment of type \a disposition, from the 
   data read from \a in, of content type \a type and using the transfer encoding
   \a encoding.

   \sa QMailMessageBody::fromStream()
*/
QMailMessagePart QMailMessagePart::fromStream(QTextStream& in,
                                              const QMailMessageContentDisposition& disposition, 
                                              const QMailMessageContentType& type, 
                                              QMailMessageBody::TransferEncoding encoding)
{
    QMailMessagePart part;
    part.setBody( QMailMessageBody::fromStream( in, type, encoding ) );
    part.setContentDisposition( disposition );

    return part;
}

/*!
   Creates a QMailMessagePart containing an attachment of type \a disposition, from the 
   data contained in \a input, of content type \a type and using the transfer encoding
   \a encoding.

   \sa QMailMessageBody::fromData()
*/
QMailMessagePart QMailMessagePart::fromData(const QString& input,
                                            const QMailMessageContentDisposition& disposition, 
                                            const QMailMessageContentType& type, 
                                            QMailMessageBody::TransferEncoding encoding)
{
    QMailMessagePart part;
    part.setBody( QMailMessageBody::fromData( input, type, encoding ) );
    part.setContentDisposition( disposition );

    return part;
}

/*!
    Creates a QMailMessagePart containing an attachment of type \a disposition, whose
    content is a reference to the message identified by \a messageId.  The resulting 
    part has content type \a type and uses the transfer encoding \a encoding.
    
    The message reference can only be resolved by transmitting the message to an external 
    server, where both the originating server of the referenced message and the receiving 
    server of the new message support resolution of the content reference.
*/
QMailMessagePart QMailMessagePart::fromMessageReference(const QMailMessageId &messageId,
                                                        const QMailMessageContentDisposition& disposition, 
                                                        const QMailMessageContentType& type, 
                                                        QMailMessageBody::TransferEncoding encoding)
{
    QMailMessagePart part;
    part.setReference(messageId, type, encoding);
    part.setContentDisposition(disposition);

    return part;
}

/*!
    Creates a QMailMessagePart containing an attachment of type \a disposition, whose
    content is a reference to the message part identified by \a partLocation.  The 
    resulting part has content type \a type and uses the transfer encoding \a encoding.
    
    The part reference can only be resolved by transmitting the message to an external 
    server, where both the originating server of the referenced part's message and the 
    receiving server of the new message support resolution of the content reference.
*/
QMailMessagePart QMailMessagePart::fromPartReference(const QMailMessagePart::Location &partLocation,
                                                     const QMailMessageContentDisposition& disposition, 
                                                     const QMailMessageContentType& type, 
                                                     QMailMessageBody::TransferEncoding encoding)
{
    QMailMessagePart part;
    part.setReference(partLocation, type, encoding);
    part.setContentDisposition(disposition);

    return part;
}

/*!
    Sets the part content to contain a reference to the message identified by \a id,
    having content type \a type and using the transfer encoding \a encoding.
    
    The message reference can only be resolved by transmitting the message to an external 
    server, where both the originating server of the referenced message and the receiving 
    server of the new message support resolution of the content reference.

    \sa referenceType(), setReferenceResolution()
*/
void QMailMessagePart::setReference(const QMailMessageId &id, const QMailMessageContentType& type, QMailMessageBody::TransferEncoding encoding)
{
    impl(this)->setReference(id, type, encoding);
}

/*!
    Sets the part content to contain a reference to the message part identified by \a location,
    having content type \a type and using the transfer encoding \a encoding.
    
    The part reference can only be resolved by transmitting the message to an external 
    server, where both the originating server of the referenced part's message and the 
    receiving server of the new message support resolution of the content reference.

    \sa referenceType(), setReferenceResolution()
*/
void QMailMessagePart::setReference(const QMailMessagePart::Location &location, const QMailMessageContentType& type, QMailMessageBody::TransferEncoding encoding)
{
    impl(this)->setReference(location, type, encoding);
}

/*!
    Returns the Content-Id header field for the part, if present; otherwise returns an empty string.

    If the header field content is surrounded by angle brackets, these are removed.
*/
QString QMailMessagePart::contentID() const
{
    QString result(headerFieldText(QLatin1String("Content-ID")));
    if (!result.isEmpty() && (result[0] == QChar::fromLatin1('<')) && (result[result.length() - 1] == QChar::fromLatin1('>'))) {
        return result.mid(1, result.length() - 2);
    }

    return result;
}

/*!
    Sets the Content-Id header field for the part to contain \a id.

    If \a id is not surrounded by angle brackets, these are added.
*/
void QMailMessagePart::setContentID(const QString &id)
{
    QString str(id);
    if (!str.isEmpty()) {
        if (str[0] != QChar::fromLatin1('<')) {
            str.prepend(QChar::fromLatin1('<'));
        }
        if (str[str.length() - 1] != QChar::fromLatin1('>')) {
            str.append(QChar::fromLatin1('>'));
        }
    }

    setHeaderField(QStringLiteral("Content-ID"), str);
}

/*!
    Returns the Content-Location header field for the part, if present; 
    otherwise returns an empty string.
*/
QString QMailMessagePart::contentLocation() const
{
    return headerFieldText(QLatin1String("Content-Location"));
}

/*!
    Sets the Content-Location header field for the part to contain \a location.
*/
void QMailMessagePart::setContentLocation(const QString &location)
{
    setHeaderField(QStringLiteral("Content-Location"), location);
}

/*!
    Returns the Content-Language header field for the part, if present; 
    otherwise returns an empty string.
*/
QString QMailMessagePart::contentLanguage() const
{
    return headerFieldText(QLatin1String("Content-Language"));
}

/*!
    Sets the Content-Language header field for the part to contain \a language.
*/
void QMailMessagePart::setContentLanguage(const QString &language)
{
    setHeaderField(QStringLiteral("Content-Language"), language);
}

bool QMailMessagePart::hasUndecodedData() const
{
    return !impl(this)->undecodedData().isEmpty();
}

const QByteArray QMailMessagePart::undecodedData() const
{
    return impl(this)->undecodedData();
}

void QMailMessagePart::setUndecodedData(const QByteArray &data)
{
    impl(this)->setUndecodedData(data);
}

void QMailMessagePart::appendUndecodedData(const QByteArray &data)
{
    impl(this)->appendUndecodedData(data);
}

/*!
    Returns the number of the part, if it has been set; otherwise returns -1.
*/
int QMailMessagePart::partNumber() const
{
    return impl(this)->partNumber();
}

/*!
    Returns the location of the part within the message.
*/
QMailMessagePart::Location QMailMessagePart::location() const
{
    return QMailMessagePart::Location(*this);
}

/*!
    Returns a non-empty string to identify the part, appropriate for display.  If the part 
    'Content-Type' header field contains a 'name' parameter, that value is used. Otherwise, 
    if the part has a 'Content-Disposition' header field containing a 'filename' parameter, 
    that value is used.  Otherwise, if the part has a 'Content-ID' header field, that value 
    is used.  Finally, a usable name will be created by combining the content type of the 
    part with the part's number.

    \sa identifier()
*/
QString QMailMessagePart::displayName() const
{
    QString id(contentType().isParameterEncoded("name")?decodeParameter(contentType().name()):decodeWordSequence(contentType().name()));

    if (id.isEmpty())
        id = contentDisposition().isParameterEncoded("filename")?decodeParameter(contentDisposition().filename()):decodeWordSequence(contentDisposition().filename());

    if (id.isEmpty())
        id = contentID();

    if (id.isEmpty() && contentType().matches("message", "rfc822")) {
        // TODO don't load entire body into memory
        QMailMessage msg = QMailMessage::fromRfc2822(body().data(QMailMessageBody::Decoded));
        id = msg.subject();
    }

    if (id.isEmpty()) {
        int partNumber = impl(this)->partNumber();
        if (partNumber != -1) {
            id = QString::number(partNumber) % QStringLiteral(" ");
        }
        id += QString::fromUtf8(contentType().content());
    }

    return id;
}

/*!
    Returns a non-empty string to identify the part, appropriate for storage.  If the part 
    has a 'Content-ID' header field, that value is used.  Otherwise, if the part has a 
    'Content-Disposition' header field containing a 'filename' parameter, that value is used.
    Otherwise, if the part 'Content-Type' header field contains a 'name' parameter, that value 
    is used.  Finally, the part's number will be returned.
*/
QString QMailMessagePart::identifier() const
{
    QString id(contentID());

    if (id.isEmpty())
        id = contentDisposition().isParameterEncoded("filename")?decodeParameter(contentDisposition().filename()):decodeWordSequence(contentDisposition().filename());

    if (id.isEmpty())
        id = contentType().isParameterEncoded("name")?decodeParameter(contentType().name()):decodeWordSequence(contentType().name());

    if (id.isEmpty())
        id = QString::number(impl(this)->partNumber());

    return id;
}

/*!
    Returns the type of reference that this message part constitutes.

    \sa setReference()
*/
QMailMessagePart::ReferenceType QMailMessagePart::referenceType() const
{
    return impl(this)->referenceType();
}

/*!
    Returns the identifier of the message that this part references.

    The result will be meaningful only when referenceType() yields
    \l{QMailMessagePartFwd::MessageReference}{QMailMessagePart::MessageReference}.

    \sa referenceType(), partReference(), referenceResolution()
*/
QMailMessageId QMailMessagePart::messageReference() const
{
    return impl(this)->messageReference();
}

/*!
    Returns the location of the message part that this part references.

    The result will be meaningful only when referenceType() yields
    \l{QMailMessagePartFwd::PartReference}{QMailMessagePart::PartReference}.

    \sa referenceType(), messageReference(), referenceResolution()
*/
QMailMessagePart::Location QMailMessagePart::partReference() const
{
    return impl(this)->partReference();
}

/*!
    Returns the URI that resolves the reference encoded into this message part.

    The result will be meaningful only when referenceType() yields other than
    \l{QMailMessagePartFwd::None}{QMailMessagePart::None}.

    \sa setReferenceResolution(), referenceType()
*/
QString QMailMessagePart::referenceResolution() const
{
    return impl(this)->referenceResolution();
}

/*!
    Sets the URI that resolves the reference encoded into this message part to \a uri.

    The reference URI is meaningful only when referenceType() yields other than
    \l{QMailMessagePartFwd::None}{QMailMessagePart::None}.

    \sa referenceResolution(), referenceType()
*/
void QMailMessagePart::setReferenceResolution(const QString &uri)
{
    impl(this)->setReferenceResolution(uri);
}

static QString randomString(int length)
{
    if (length <= 0) 
        return QString();

    QString str;
    str.resize( length );

    int i = 0;
    while (length--){
        int r=qrand() % 62;
        r+=48;
        if (r>57) r+=7;
        if (r>90) r+=6;
        str[i++] = QChar(r);
    }
    return str;
}

static QString partFileName(const QMailMessagePart &part)
{
    QString fileName(part.displayName());
    if (!fileName.isEmpty()) {
        // Remove any slash characters which are invalid in filenames
        QChar* first = fileName.data(), *last = first + (fileName.length() - 1);
        for ( ; last >= first; --last)
            if (*last == '/')
                fileName.remove((last - first), 1);
    }

    // Application/octet-stream is a fallback for the case when mimetype
    // is unknown by MUA, so it's treated exceptionally here, so the original
    // attachment name is preserved
    if (!part.contentType().matches("application", "octet-stream")) {
        // If possible, create the file with a useful filename extension
        QString existing;
        int index = fileName.lastIndexOf('.');
        if (index != -1)
            existing = fileName.mid(index + 1);

        QStringList extensions = QMail::extensionsForMimeType(QString::fromUtf8(part.contentType().content().toLower()));
        if (!extensions.isEmpty()) {
            // See if the existing extension is a known one
            if (existing.isEmpty() || !extensions.contains(existing, Qt::CaseInsensitive)) {
                if (!fileName.endsWith('.')) {
                    fileName.append('.');
                }
                fileName.append(extensions.first());
            }
        }
    }

    return fileName;
}

/*!
    Writes the decoded body of the part to a file under the directory specified by \a path.
    The name of the resulting file is taken from the part. If that file name already exists 
    in the path a new unique name of the format <random chars>.<filename> is saved.

    Returns the path of the file written on success, or an empty string otherwise.
*/

QString QMailMessagePart::writeBodyTo(const QString &path) const
{
    QString directory(path);
    if (directory.endsWith('/'))
        directory.chop(1);

    if (!QDir(directory).exists()) {
        QDir base;
        if (QDir::isAbsolutePath(directory))
            base = QDir::root();
        else
            base = QDir::current();

        if (!base.mkpath(directory)) {
            qWarning() << "Could not create directory to save file " << directory;
            return QString();
        }
    }

    QString fileName(partFileName(*this));

    QString filepath = directory % QStringLiteral("/") % fileName;
    while (QFile::exists(filepath))
        filepath = directory % QStringLiteral("/") % randomString(5) % QStringLiteral(".") % fileName;

    if (!body().toFile(filepath, QMailMessageBody::Decoded)) {
        qWarning() << "Could not write part data to file " << filepath;
        return QString();
    }
    
    return filepath;
}

/*!
    Returns an indication of the size of the part.  This measure should be used
    only in comparing the relative size of parts with respect to transmission.
*/  
uint QMailMessagePart::indicativeSize() const
{
    return impl(this)->indicativeSize();
}

/*!
    Returns true if the entire content of this part is available; otherwise returns false.
*/
bool QMailMessagePart::contentAvailable() const
{
    return impl(this)->contentAvailable();
}

/*!
    Returns true if some portion of the content of this part is available; otherwise returns false.
*/
bool QMailMessagePart::partialContentAvailable() const
{
    return impl(this)->partialContentAvailable();
}

/*! \internal */
void QMailMessagePart::output(QDataStream& out, bool includeAttachments, bool excludeInternalFields) const
{
    QDataStream *ds(&out);
    return impl(this)->output<DummyChunkProcessor>(&ds, false, includeAttachments, excludeInternalFields, Q_NULLPTR);
}

QByteArray QMailMessagePart::toRfc2822() const
{
    // Generate boundaries for this part, as in QMailMessage::toRfc2822().
    const_cast<QMailMessagePartPrivate*>(impl(this))->generateBoundary();

    QByteArray result;
    QDataStream out(&result, QIODevice::WriteOnly);
    output(out, true, true);

    return result;
}

/*! 
    \fn QMailMessagePart::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailMessagePart::serialize(Stream &stream) const
{
    impl(this)->serialize(stream);
}

template void QMailMessagePart::serialize(QDataStream &) const;

/*! 
    \fn QMailMessagePart::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessagePart::deserialize(Stream &stream)
{
    impl(this)->deserialize(stream);
}

template void QMailMessagePart::deserialize(QDataStream &);

/*! \internal */
bool QMailMessagePart::contentModified() const
{
    return impl(this)->contentModified();
}

/*! \internal */
void QMailMessagePart::setUnmodified()
{
    impl(this)->setUnmodified();
}


static quint64 incomingFlag = 0;
static quint64 outgoingFlag = 0;
static quint64 sentFlag = 0;
static quint64 repliedFlag = 0;
static quint64 repliedAllFlag = 0;
static quint64 forwardedFlag = 0;
static quint64 contentAvailableFlag = 0;
static quint64 readFlag = 0;
static quint64 removedFlag = 0;
static quint64 readElsewhereFlag = 0;
static quint64 unloadedDataFlag = 0;
static quint64 newFlag = 0;
static quint64 readReplyRequestedFlag = 0;
static quint64 trashFlag = 0;
static quint64 partialContentAvailableFlag = 0;
static quint64 hasAttachmentsFlag = 0;
static quint64 hasReferencesFlag = 0;
static quint64 hasSignatureFlag = 0;
static quint64 hasUnresolvedReferencesFlag = 0;
static quint64 draftFlag = 0;
static quint64 outboxFlag = 0;
static quint64 junkFlag = 0;
static quint64 transmitFromExternalFlag = 0;
static quint64 localOnlyFlag = 0;
static quint64 temporaryFlag = 0;
static quint64 importantElsewhereFlag = 0;
static quint64 importantFlag = 0;
static quint64 highPriorityFlag = 0;
static quint64 lowPriorityFlag = 0;
static quint64 calendarInvitationFlag = 0;
static quint64 todoFlag = 0;
static quint64 noNotificationFlag = 0;


/*  QMailMessageMetaData */

QMailMessageMetaDataPrivate::QMailMessageMetaDataPrivate()
    : QPrivateImplementationBase(this),
      _messageType(QMailMessage::None),
      _status(0),
      _contentType(QMailMessage::UnknownContent),
      _size(0),
      _copyServerUid(QStringLiteral("")),
      _listId(QStringLiteral("")),
      _rfcId(QStringLiteral("")),
      _responseType(QMailMessage::NoResponse),
      _preview(QStringLiteral("")),
      _customFieldsModified(false),
      _dirty(false)
{
}

void QMailMessageMetaDataPrivate::setMessageType(QMailMessage::MessageType type)
{
    updateMember(_messageType, type);
}

#ifndef QTOPIAMAIL_PARSING_ONLY
void QMailMessageMetaDataPrivate::setParentFolderId(const QMailFolderId& id)
{
    updateMember(_parentFolderId, id);
}

void QMailMessageMetaDataPrivate::setPreviousParentFolderId(const QMailFolderId& id)
{
    updateMember(_previousParentFolderId, id);
}
#endif

void QMailMessageMetaDataPrivate::setId(const QMailMessageId& id)
{
    updateMember(_id, id);
}

void QMailMessageMetaDataPrivate::setStatus(quint64 newStatus)
{
    updateMember(_status, newStatus);
}

#ifndef QTOPIAMAIL_PARSING_ONLY
void QMailMessageMetaDataPrivate::setParentAccountId(const QMailAccountId& id)
{
    updateMember(_parentAccountId, id);
}
#endif

void QMailMessageMetaDataPrivate::setServerUid(const QString &uid)
{
    updateMember(_serverUid, uid);
}

void QMailMessageMetaDataPrivate::setSize(uint size)
{
    updateMember(_size, size);
}

void QMailMessageMetaDataPrivate::setContent(QMailMessage::ContentType type)
{
    updateMember(_contentType, type);
}

void QMailMessageMetaDataPrivate::setSubject(const QString& s)
{
    updateMember(_subject, s);
}

void QMailMessageMetaDataPrivate::setDate(const QMailTimeStamp& timeStamp)
{
    updateMember(_date, timeStamp);
}

void QMailMessageMetaDataPrivate::setReceivedDate(const QMailTimeStamp& timeStamp)
{
    updateMember(_receivedDate, timeStamp);
}

void QMailMessageMetaDataPrivate::setFrom(const QString& s)
{
    updateMember(_from, s);
} 

void QMailMessageMetaDataPrivate::setRecipients(const QString& s)
{
    updateMember(_to, s);
}

void QMailMessageMetaDataPrivate::setCopyServerUid(const QString &copyServerUid)
{
    updateMember(_copyServerUid, copyServerUid.isNull() ? QStringLiteral("") : copyServerUid);
}

void QMailMessageMetaDataPrivate::setListId(const QString &listId)
{
    updateMember(_listId, listId.isNull() ? QStringLiteral("") : listId);
}

void QMailMessageMetaDataPrivate::setRestoreFolderId(const QMailFolderId &folderId)
{
    updateMember(_restoreFolderId, folderId);
}

void QMailMessageMetaDataPrivate::setRfcId(const QString &rfcId)
{
    updateMember(_rfcId, rfcId.isNull() ? QStringLiteral("") : rfcId);
}

void QMailMessageMetaDataPrivate::setContentScheme(const QString& scheme)
{
    updateMember(_contentScheme, scheme);
}

void QMailMessageMetaDataPrivate::setContentIdentifier(const QString& identifier)
{
    updateMember(_contentIdentifier, identifier);
}

void QMailMessageMetaDataPrivate::setInResponseTo(const QMailMessageId &id)
{
    updateMember(_responseId, id);
}

void QMailMessageMetaDataPrivate::setResponseType(QMailMessageMetaData::ResponseType type)
{
    updateMember(_responseType, type);
}

void QMailMessageMetaDataPrivate::setPreview(const QString &s)
{
    updateMember(_preview, s.isNull() ? QString("") : s);
}

uint QMailMessageMetaDataPrivate::indicativeSize() const
{
    uint size = (_size / QMailMessageBodyPrivate::IndicativeSizeUnit);

    // Count the message header as one size unit
    return (size + 1);
}

bool QMailMessageMetaDataPrivate::dataModified() const
{
    return _dirty || _customFieldsModified;
}

void QMailMessageMetaDataPrivate::setUnmodified()
{
    _dirty = false;
    _customFieldsModified = false;
}

#ifndef QTOPIAMAIL_PARSING_ONLY
quint64 QMailMessageMetaDataPrivate::registerFlag(const QString &name)
{
    if (!QMailStore::instance()->registerMessageStatusFlag(name)) {
        qMailLog(Messaging) << "Unable to register message status flag:" << name << "!";
    }

    return QMailMessage::statusMask(name);
}
#endif

void QMailMessageMetaDataPrivate::ensureCustomFieldsLoaded() const
{
    if (!_customFields.isInitialized()) {
        if (_id.isValid())
            _customFields = QMailStore::instance()->messageCustomFields(_id);
        else
            _customFields = QMap<QString, QString>();
    }
}

const QMap<QString, QString> &QMailMessageMetaDataPrivate::customFields() const
{
    ensureCustomFieldsLoaded();
    return *_customFields;
}

QString QMailMessageMetaDataPrivate::customField(const QString &name) const
{
    ensureCustomFieldsLoaded();
    QMap<QString, QString>::const_iterator it = _customFields->find(name);
    if (it != _customFields->end()) {
        return *it;
    }

    return QString();
}

void QMailMessageMetaDataPrivate::setCustomField(const QString &name, const QString &value)
{
    ensureCustomFieldsLoaded();
    QMap<QString, QString>::iterator it = _customFields->find(name);
    if (it != _customFields->end()) {
        if (*it != value) {
            *it = value;
            _customFieldsModified = true;
        }
    } else {
        _customFields->insert(name, value);
        _customFieldsModified = true;
    }
}

void QMailMessageMetaDataPrivate::removeCustomField(const QString &name)
{
    ensureCustomFieldsLoaded();
    QMap<QString, QString>::iterator it = _customFields->find(name);
    if (it != _customFields->end()) {
        _customFields->erase(it);
        _customFieldsModified = true;
    }
}

void QMailMessageMetaDataPrivate::setCustomFields(const QMap<QString, QString> &fields)
{
    _customFields = fields;
    _customFieldsModified = true;
}

void QMailMessageMetaDataPrivate::setParentThreadId(const QMailThreadId &id)
{
    updateMember(_parentThreadId, id);
}

template <typename Stream> 
void QMailMessageMetaDataPrivate::serialize(Stream &stream) const
{
    stream << _messageType;
    stream << _status;
    stream << _contentType;
#ifndef QTOPIAMAIL_PARSING_ONLY
    stream << _parentAccountId;
#endif
    stream << _serverUid;
    stream << _size;
    stream << _id;
#ifndef QTOPIAMAIL_PARSING_ONLY
    stream << _parentFolderId;
    stream << _previousParentFolderId;
#endif
    stream << _subject;
    stream << _date.toString();
    stream << _receivedDate.toString();
    stream << _from;
    stream << _to;
    stream << _copyServerUid;
    stream << _restoreFolderId;
    stream << _listId;
    stream << _rfcId;
    stream << _contentScheme;
    stream << _contentIdentifier;
    stream << _responseId;
    stream << _responseType;
    stream << customFields();
    stream << _customFieldsModified;
    stream << _dirty;
    stream << _preview;
    stream << _parentThreadId;
}

template <typename Stream> 
void QMailMessageMetaDataPrivate::deserialize(Stream &stream)
{
    QString timeStamp;
    QMap<QString, QString> customFields;

    stream >> _messageType;
    stream >> _status;
    stream >> _contentType;
#ifndef QTOPIAMAIL_PARSING_ONLY
    stream >> _parentAccountId;
#endif
    stream >> _serverUid;
    stream >> _size;
    stream >> _id;
#ifndef QTOPIAMAIL_PARSING_ONLY
    stream >> _parentFolderId;
    stream >> _previousParentFolderId;
#endif
    stream >> _subject;
    stream >> timeStamp;
    _date = QMailTimeStamp(timeStamp);
    stream >> timeStamp;
    _receivedDate = QMailTimeStamp(timeStamp);
    stream >> _from;
    stream >> _to;
    stream >> _copyServerUid;
    stream >> _restoreFolderId;
    stream >> _listId;
    stream >> _rfcId;
    stream >> _contentScheme;
    stream >> _contentIdentifier;
    stream >> _responseId;
    stream >> _responseType;
    stream >> customFields;
    _customFields = customFields;
    stream >> _customFieldsModified;
    stream >> _dirty;
    stream >> _preview;
    stream >> _parentThreadId;
}


/*!
    \class QMailMessageMetaData

    \preliminary
    \brief The QMailMessageMetaData class provides information about a message stored by QMF.
    
    \ingroup messaginglibrary
   
    The QMailMessageMetaData class provides information about messages stored in the Qt Extended system as QMailMessage objects.  The meta data is more compact and more easily accessed and
    manipulated than the content of the message itself.  Many messaging-related tasks can 
    be accomplished by manipulating the message meta data, such as listing, filtering, and 
    searching through sets of messages.

    QMailMessageMetaData objects can be created as needed, specifying the identifier of
    the message whose meta data is required.  The meta data of a message can be located by 
    specifying the QMailMessageId identifier directly, or by specifying the account and server UID 
    pair needed to locate the message.

    The content of the message described by the meta data object can be accessed by creating 
    a QMailMessage object specifying the identifier returned by QMailMessageMetaData::id().

    \sa QMailStore, QMailMessageId
*/    
    
/*!
    \typedef QMailMessageMetaData::ImplementationType
    \internal
*/

/*!
    \variable QMailMessageMetaData::Incoming

    The status mask needed for testing the value of the registered status flag named 
    \c "Incoming" against the result of QMailMessage::status().

    This flag indicates that the message has been sent from an external source to an
    account whose messages are retrieved to Qt Extended.
*/

/*!
    \variable QMailMessageMetaData::Outgoing

    The status mask needed for testing the value of the registered status flag named 
    \c "Outgoing" against the result of QMailMessage::status().

    This flag indicates that the message originates within Qt Extended, for transmission 
    to an external message sink.
*/

/*!
    \variable QMailMessageMetaData::Sent

    The status mask needed for testing the value of the registered status flag named 
    \c "Sent" against the result of QMailMessage::status().

    This flag indicates that the message has been delivered to an external message sink.
*/

/*!
    \variable QMailMessageMetaData::Replied

    The status mask needed for testing the value of the registered status flag named 
    \c "Replied" against the result of QMailMessage::status().

    This flag indicates that a message replying to the source of this message has been 
    created, in response to this message.
*/

/*!
    \variable QMailMessageMetaData::RepliedAll

    The status mask needed for testing the value of the registered status flag named 
    \c "RepliedAll" against the result of QMailMessage::status().

    This flag indicates that a message replying to the source of this message and all 
    its recipients, has been created in response to this message.
*/

/*!
    \variable QMailMessageMetaData::Forwarded

    The status mask needed for testing the value of the registered status flag named 
    \c "Forwarded" against the result of QMailMessage::status().

    This flag indicates that a message forwarding the content of this message has been created.
*/

/*!
    \variable QMailMessageMetaData::Read

    The status mask needed for testing the value of the registered status flag named 
    \c "Read" against the result of QMailMessage::status().

    This flag indicates that the content of this message has been displayed to the user.
*/

/*!
    \variable QMailMessageMetaData::Removed

    The status mask needed for testing the value of the registered status flag named 
    \c "Removed" against the result of QMailMessage::status().

    This flag indicates that the message has been deleted from or moved on the originating server.
*/

/*!
    \variable QMailMessageMetaData::ReadElsewhere

    The status mask needed for testing the value of the registered status flag named 
    \c "ReadElsewhere" against the result of QMailMessage::status().

    This flag indicates that the content of this message has been reported as having
    been displayed to the user by some other client.
*/

/*!
    \variable QMailMessageMetaData::UnloadedData

    The status mask needed for testing the value of the registered status flag named 
    \c "UnloadedData" against the result of QMailMessage::status().

    This flag indicates that the meta data of the message is not loaded in entirety.
*/

/*!
    \variable QMailMessageMetaData::New

    The status mask needed for testing the value of the registered status flag named 
    \c "New" against the result of QMailMessage::status().

    This flag indicates that the meta data of the message has not yet been displayed to the user.
*/

/*!
    \variable QMailMessageMetaData::ReadReplyRequested

    The status mask needed for testing the value of the registered status flag named 
    \c "ReadReplyRequested" against the result of QMailMessage::status().

    This flag indicates that the message has requested that a read confirmation reply be returned to the sender.
*/

/*!
    \variable QMailMessageMetaData::Trash

    The status mask needed for testing the value of the registered status flag named 
    \c "Trash" against the result of QMailMessage::status().

    This flag indicates that the message has been marked as trash, and should be considered logically deleted.
*/

/*!
    \variable QMailMessageMetaData::ContentAvailable

    The status mask needed for testing the value of the registered status flag named 
    \c "ContentAvailable" against the result of QMailMessage::status().

    This flag indicates that the entire content of the message has been retrieved from the originating server,
    excluding any sub-parts of the message.

    \sa QMailMessagePartContainer::contentAvailable()
*/

/*!
    \variable QMailMessageMetaData::PartialContentAvailable

    The status mask needed for testing the value of the registered status flag named 
    \c "PartialContentAvailable" against the result of QMailMessage::status().

    This flag indicates that some portion of the  content of the message has been retrieved from the originating server.

    \sa QMailMessagePartContainer::contentAvailable()
*/

/*!
    \variable QMailMessageMetaData::HasAttachments

    The status mask needed for testing the value of the registered status flag named 
    \c "HasAttachments" against the result of QMailMessage::status().

    This flag indicates that the message contains at least one sub-part with 
    'Attachment' disposition, or a "X-MS-Has-Attach" headerfield with value yes.

    \sa QMailMessageContentDisposition
*/

/*!
    \variable QMailMessageMetaData::HasReferences

    The status mask needed for testing the value of the registered status flag named 
    \c "HasReferences" against the result of QMailMessage::status().

    This flag indicates that the message contains at least one sub-part which is a reference to an external message element.

    \sa QMailMessagePart::referenceType()
*/

/*!
    \variable QMailMessageMetaData::HasUnresolvedReferences

    The status mask needed for testing the value of the registered status flag named 
    \c "HasUnresolvedReferences" against the result of QMailMessage::status().

    This flag indicates that the message contains at least one sub-part which is a reference, that has no corresponding resolution value.

    \sa QMailMessagePart::referenceType(), QMailMessagePart::referenceResolution()
*/

/*!
    \variable QMailMessageMetaData::Draft

    The status mask needed for testing the value of the registered status flag named 
    \c "Draft" against the result of QMailMessage::status().

    This flag indicates that the message has been marked as a draft, and should be considered subject to further composition.
*/

/*!
    \variable QMailMessageMetaData::Outbox

    The status mask needed for testing the value of the registered status flag named 
    \c "Outbox" against the result of QMailMessage::status().

    This flag indicates that the message has been marked as ready for transmission.
*/

/*!
    \variable QMailMessageMetaData::Junk

    The status mask needed for testing the value of the registered status flag named 
    \c "Junk" against the result of QMailMessage::status().

    This flag indicates that the message has been marked as junk, and should be considered unsuitable for standard listings.
*/

/*!
    \variable QMailMessageMetaData::TransmitFromExternal

    The status mask needed for testing the value of the registered status flag named 
    \c "TransmitFromExternal" against the result of QMailMessage::status().

    This flag indicates that the message should be transmitted by reference to its external server location.
*/

/*!
    \variable QMailMessageMetaData::LocalOnly

    The status mask needed for testing the value of the registered status flag named 
    \c "LocalOnly" against the result of QMailMessage::status().

    This flag indicates that the message exists only on the local device, and has no representation on any external server.
*/

/*!
    \variable QMailMessageMetaData::Temporary

    The status mask needed for testing the value of the registered status flag named
    \c "Temporary" against the result of QMailMessage::status().

    This flag indicates that the message will not exist permanently and should be removed at a later time.
*/

/*!
    \variable QMailMessageMetaData::ImportantElsewhere

    The status mask needed for testing the value of the registered status flag named 
    \c "ImportantElsewhere" against the result of QMailMessage::status().

    This flag indicates that the message has been reported as having been marked as 
    important by some other client.
*/

/*!
    \variable QMailMessageMetaData::Important

    The status mask needed for testing the value of the registered status flag named 
    \c "Important" against the result of QMailMessage::status().

    This flag indicates that the message is marked as important.
*/

/*!
    \variable QMailMessageMetaData::HighPriority

    The status mask needed for testing the value of the registered status flag named 
    \c "HighPriority" against the result of QMailMessage::status().

    This flag indicates that the message has a header field specifying that the message 
    is high priority. This flag is set only during message parsing.
    
    \sa QMailMessage::fromRfc2822()
*/

/*!
    \variable QMailMessageMetaData::LowPriority

    The status mask needed for testing the value of the registered status flag named 
    \c "LowPriority" against the result of QMailMessage::status().

    This flag indicates that the message has a header field specifying that the message 
    is low priority. This flag is set only during message parsing.
    
    \sa QMailMessage::fromRfc2822()
*/

/*!
    \variable QMailMessageMetaData::CalendarInvitation

    The status mask needed for testing the value of the registered status flag named 
    \c "CalendarInvitation" against the result of QMailMessage::status().

    This flag indicates that the message includes a calendar invitation request part.
*/

/*!
    \variable QMailMessageMetaData::Todo

    The status mask needed for testing the value of the registered status flag named 
    \c "Todo" against the result of QMailMessage::status().

    This flag indicates that the message has been marked as a todo item.
*/

/*!
    \variable QMailMessageMetaData::NoNotification

    The status mask needed for testing the value of the registered status flag named 
    \c "NoNotification" against the result of QMailMessage::status().

    This flag indicates that a new message notification should not be shown
    for the message. e.g. an older message retrieved in a folder that has previously
    been synchronized, or an existing message moved to a folder such as trash, or a 
    message externalized by saving in a drafts or sent folder.
*/

const quint64 &QMailMessageMetaData::Incoming = incomingFlag;
const quint64 &QMailMessageMetaData::Outgoing = outgoingFlag;
const quint64 &QMailMessageMetaData::Sent = sentFlag;
const quint64 &QMailMessageMetaData::Replied = repliedFlag;
const quint64 &QMailMessageMetaData::RepliedAll = repliedAllFlag;
const quint64 &QMailMessageMetaData::Forwarded = forwardedFlag;
const quint64 &QMailMessageMetaData::ContentAvailable = contentAvailableFlag;
const quint64 &QMailMessageMetaData::Read = readFlag;
const quint64 &QMailMessageMetaData::Removed = removedFlag;
const quint64 &QMailMessageMetaData::ReadElsewhere = readElsewhereFlag;
const quint64 &QMailMessageMetaData::UnloadedData = unloadedDataFlag;
const quint64 &QMailMessageMetaData::New = newFlag;
const quint64 &QMailMessageMetaData::ReadReplyRequested = readReplyRequestedFlag;
const quint64 &QMailMessageMetaData::Trash = trashFlag;
const quint64 &QMailMessageMetaData::PartialContentAvailable = partialContentAvailableFlag;
const quint64 &QMailMessageMetaData::HasAttachments = hasAttachmentsFlag;
const quint64 &QMailMessageMetaData::HasReferences = hasReferencesFlag;
const quint64 &QMailMessageMetaData::HasSignature = hasSignatureFlag;
const quint64 &QMailMessageMetaData::HasUnresolvedReferences = hasUnresolvedReferencesFlag;
const quint64 &QMailMessageMetaData::Draft = draftFlag;
const quint64 &QMailMessageMetaData::Outbox = outboxFlag;
const quint64 &QMailMessageMetaData::Junk = junkFlag;
const quint64 &QMailMessageMetaData::TransmitFromExternal = transmitFromExternalFlag;
const quint64 &QMailMessageMetaData::LocalOnly = localOnlyFlag;
const quint64 &QMailMessageMetaData::Temporary = temporaryFlag;
const quint64 &QMailMessageMetaData::ImportantElsewhere = importantElsewhereFlag;
const quint64 &QMailMessageMetaData::Important = importantFlag;
const quint64 &QMailMessageMetaData::HighPriority = highPriorityFlag;
const quint64 &QMailMessageMetaData::LowPriority = lowPriorityFlag;
const quint64 &QMailMessageMetaData::CalendarInvitation = calendarInvitationFlag;
const quint64 &QMailMessageMetaData::Todo = todoFlag;
const quint64 &QMailMessageMetaData::NoNotification = noNotificationFlag;

/*!
    Constructs an empty message meta data object.
*/
QMailMessageMetaData::QMailMessageMetaData()
    : QPrivatelyImplemented<QMailMessageMetaDataPrivate>(new QMailMessageMetaDataPrivate())
{
}

#ifndef QTOPIAMAIL_PARSING_ONLY
/*!
    Constructs a message meta data object from data stored in the message store with QMailMessageId \a id.
*/
QMailMessageMetaData::QMailMessageMetaData(const QMailMessageId& id)
    : QPrivatelyImplemented<QMailMessageMetaDataPrivate>(Q_NULLPTR)
{
    *this = QMailStore::instance()->messageMetaData(id);
}

/*!
    Constructs a message meta data object from data stored in the message store with the unique 
    identifier \a uid from the account with id \a accountId.
*/
QMailMessageMetaData::QMailMessageMetaData(const QString& uid, const QMailAccountId& accountId)
    : QPrivatelyImplemented<QMailMessageMetaDataPrivate>(Q_NULLPTR)
{
    *this = QMailStore::instance()->messageMetaData(uid, accountId);
}
#endif

/*!
    Sets the MessageType of the message to \a type.

    \sa messageType()
*/
void QMailMessageMetaData::setMessageType(QMailMessageMetaData::MessageType type)
{
    switch (type) {
        case QMailMessage::Mms:
        case QMailMessage::Sms:
        case QMailMessage::Email:
        case QMailMessage::Instant:
        case QMailMessage::System:
            break;
        default:
            qWarning() << "QMailMessageMetaData::setMessageType:" << type;
            return;
    }

    impl(this)->setMessageType(type);
}

/*!
    Returns the MessageType of the message.

    \sa setMessageType()
*/
QMailMessageMetaData::MessageType QMailMessageMetaData::messageType() const
{
    return impl(this)->_messageType;
}

#ifndef QTOPIAMAIL_PARSING_ONLY
/*!
    Return the QMailFolderId of the folder that contains the message.
*/
QMailFolderId QMailMessageMetaData::parentFolderId() const
{
    return impl(this)->_parentFolderId;
}

/*!
    Sets the QMailFolderId of the folder that contains the message to \a id.
*/
void QMailMessageMetaData::setParentFolderId(const QMailFolderId &id)
{
    impl(this)->setParentFolderId(id);
}
#endif

/*!
    Returns the Qt Extended unique QMailMessageId of the message.
*/
QMailMessageId QMailMessageMetaData::id() const
{
    return impl(this)->_id;
}

/*!
    Sets the QMailMessageId of the message to \a id.
    \a id should be different for each message known to QMF.
*/
void QMailMessageMetaData::setId(const QMailMessageId &id)
{
    impl(this)->setId(id);
}

/*!
    Returns the originating address of the message.
*/
QMailAddress QMailMessageMetaData::from() const
{
    return QMailAddress(impl(this)->_from);
}

/*!
    Sets the from address, that is the originating address of the message to \a from.
*/
void QMailMessageMetaData::setFrom(const QMailAddress &from)
{
    impl(this)->setFrom(from.toString());
}

/*!
    Returns the subject of the message, if present; otherwise returns an empty string.
*/
QString QMailMessageMetaData::subject() const
{
    return impl(this)->_subject;
}

/*!
    Sets the subject of the message to \a subject.
*/
void QMailMessageMetaData::setSubject(const QString &subject)
{
    impl(this)->setSubject(subject);
}


/*!
    Returns the timestamp contained in the origination date header field of the message, if present; 
    otherwise returns an empty timestamp.
*/
QMailTimeStamp QMailMessageMetaData::date() const
{
    return QMailTimeStamp(impl(this)->_date);
}

/*!
    Sets the origination date header field specifying the timestamp of the message to \a timeStamp.
*/
void QMailMessageMetaData::setDate(const QMailTimeStamp &timeStamp)
{
    impl(this)->setDate(timeStamp);
}

/*!
    Returns the timestamp when the message was recieved by the remote server, if known;
    otherwise returns the timestamp contained in the origination date header field of the message, if present; 
    otherwise returns an empty timestamp.
*/
QMailTimeStamp QMailMessageMetaData::receivedDate() const
{
    return QMailTimeStamp(impl(this)->_receivedDate);
}

/*!
    Sets the timestamp indicating the time of message reception by the remote server to \a timeStamp.
*/
void QMailMessageMetaData::setReceivedDate(const QMailTimeStamp &timeStamp)
{
    impl(this)->setReceivedDate(timeStamp);
}

/*! 
    Returns the list of all recipients for the message.

    \sa QMailAddress
*/
QList<QMailAddress> QMailMessageMetaData::recipients() const
{
    return QMailAddress::fromStringList(impl(this)->_to);
}

/*! 
    Sets the list of recipients for the message to \a toList.
*/
void QMailMessageMetaData::setRecipients(const QList<QMailAddress>& toList)
{
    impl(this)->setRecipients(QMailAddress::toStringList(toList).join(QLatin1String(", ")));
}

/*! 
    Sets the list of primary recipients for the message to contain \a address.
*/
void QMailMessageMetaData::setRecipients(const QMailAddress& address)
{
    setRecipients(QList<QMailAddress>() << address);
}

/*!
    If this messsage is an unsynchronized copy, it will return the server identifier of
    the message it is a copy of. Otherwise an empty string is returned.

    Most clients should probably not need to use this.

    \sa serverUid()
*/
QString QMailMessageMetaData::copyServerUid() const
{
    return impl(this)->_copyServerUid;
}

/*!
    Sets the server identifier to \a serverUid of which message this is an unsychronized copy of.

    There is little reason for clients to use this.

    \sa copyServerUid()
*/
void QMailMessageMetaData::setCopyServerUid(const QString &serverUid)
{
    impl(this)->setCopyServerUid(serverUid);
}

/*!
    Return the folder in which this message should be moved to, if it were to be restored from trash.
    Returns an invalid QMailFolderId if this message is not in trash or require a move when restored.
*/
QMailFolderId QMailMessageMetaData::restoreFolderId() const
{
    return impl(this)->_restoreFolderId;
}

/*!
    Sets the identifier for which folder this message should be restorable to \a id
    \sa restoreFolderId()
*/
void QMailMessageMetaData::setRestoreFolderId(const QMailFolderId &id)
{
    impl(this)->setRestoreFolderId(id);
}

/*!
    Returns the list identifier. This corresponds to "list-id-namespace" specified in RFC 2919. Returns
    an empty string if this message does not belong to a list.
*/
QString QMailMessageMetaData::listId() const
{
    return impl(this)->_listId;
}

/*!
    Sets the list identifier to \a id
    \sa listId()
*/
void QMailMessageMetaData::setListId(const QString &id)
{
    impl(this)->setListId(id);
}

/*!
    Returns the message-id identifier. This is taken from the message-id field of an RFC2822 message. Returns
    and empty string if not available.
*/
QString QMailMessageMetaData::rfcId() const
{
    return impl(this)->_rfcId;
}

/*!
    Sets the RfcId to \a id
    \sa rfcId()
*/
void QMailMessageMetaData::setRfcId(const QString &id)
{
    impl(this)->setRfcId(id);
}

/*!
    Returns the id of the thread this message belongs to.
*/
QMailThreadId QMailMessageMetaData::parentThreadId() const
{
    return impl(this)->_parentThreadId;
}

/*!
    Sets the id of the thread this message belongs to \a id. If this is left blank then the thread will be detected/generated.
*/
void QMailMessageMetaData::setParentThreadId(const QMailThreadId &id)
{
    impl(this)->setParentThreadId(id);
}

/*!
    Returns the status value for the message.

    \sa setStatus(), statusMask()
*/  
quint64 QMailMessageMetaData::status() const
{
    return impl(this)->_status;
}

/*!
    Sets the status value for the message to \a newStatus.

    \sa status(), statusMask()
*/
void QMailMessageMetaData::setStatus(quint64 newStatus)
{
    impl(this)->setStatus(newStatus);
}

/*!
    Sets the status flags indicated in \a mask to \a set.

    \sa status(), statusMask()
*/
void QMailMessageMetaData::setStatus(quint64 mask, bool set)
{
    quint64 newStatus = impl(this)->_status;

    if (set)
        newStatus |= mask;
    else
        newStatus &= ~mask;
    impl(this)->setStatus(newStatus);
}

#ifndef QTOPIAMAIL_PARSING_ONLY
/*!
    Returns the id of the originating account for the message.
*/  
QMailAccountId QMailMessageMetaData::parentAccountId() const
{
    return impl(this)->_parentAccountId;
}

/*!
    Sets the id of the originating account for the message to \a id.
*/ 
void QMailMessageMetaData::setParentAccountId(const QMailAccountId& id)
{
    impl(this)->setParentAccountId(id);
}
#endif

/*!
    Returns the identifier for the message on the originating server.
*/  
QString QMailMessageMetaData::serverUid() const
{
    return impl(this)->_serverUid;
}

/*!
    Sets the originating server identifier for the message to \a server.
    The identifier specified should be unique.
*/  
void QMailMessageMetaData::setServerUid(const QString &server)
{
    impl(this)->setServerUid(server);
}

/*!
    Returns the complete size of the message as indicated on the originating server.
*/  
uint QMailMessageMetaData::size() const
{
    return impl(this)->_size;
}

/*!
    Sets the complete size of the message as found on the server to \a size.
*/  
void QMailMessageMetaData::setSize(uint size)
{
    impl(this)->setSize(size);
}

/*!
    Returns an indication of the size of the message.  This measure should be used
    only in comparing the relative size of messages with respect to transmission.
*/  
uint QMailMessageMetaData::indicativeSize() const
{
    return impl(this)->indicativeSize();
}

/*!
    Returns the type of content contained within the message.
*/  
QMailMessage::ContentType QMailMessageMetaData::content() const
{
    return impl(this)->_contentType;
}

/*!
    \fn QMailMessageMetaData::setContent(QMailMessageMetaData::ContentType)

    Sets the type of content contained within the message to \a type.  
    It is the caller's responsibility to ensure that this value matches the actual content.
*/  
void QMailMessageMetaData::setContent(QMailMessage::ContentType type)
{
    impl(this)->setContent(type);
}

#ifndef QTOPIAMAIL_PARSING_ONLY
/*!
    Return the QMailFolderId of the folder that contained the message before it was 
    moved into the current parent folder.
*/
QMailFolderId QMailMessageMetaData::previousParentFolderId() const
{
    return impl(this)->_previousParentFolderId;
}

/*!
    Sets the QMailFolderId of the folder that contained the message before it was 
    moved into the current parent folder to \a id.
*/
void QMailMessageMetaData::setPreviousParentFolderId(const QMailFolderId &id)
{
    impl(this)->setPreviousParentFolderId(id);
}
#endif

/*!
    Returns the scheme used to store the content of this message.
*/  
QString QMailMessageMetaData::contentScheme() const
{
    return impl(this)->_contentScheme;
}

/*!
    Sets the scheme used to store the content of this message to \a scheme, and returns
    true if successful.  Once set, the scheme cannot be modified.
*/  
bool QMailMessageMetaData::setContentScheme(const QString &scheme)
{
    if (!impl(this)->_contentScheme.isEmpty() && (impl(this)->_contentScheme != scheme)) {
        qMailLog(Messaging) << "Warning - modifying existing content scheme from:" << impl(this)->_contentScheme << "to:" << scheme;
    }

    impl(this)->setContentScheme(scheme);
    return true;
}

/*!
    Returns the identifer used to locate the content of this message.
*/  
QString QMailMessageMetaData::contentIdentifier() const
{
    return impl(this)->_contentIdentifier;
}

/*!
    Sets the identifer used to locate the content of this message to \a identifier, and returns
    true if successful.  Once set, the identifier cannot be modified.

    The identifier specified should be unique within the scheme returned by contentScheme().
*/  
bool QMailMessageMetaData::setContentIdentifier(const QString &identifier)
{
    impl(this)->setContentIdentifier(identifier);
    return true;
}

/*!
    Returns the identifier of the message that this message was created in response to.
*/  
QMailMessageId QMailMessageMetaData::inResponseTo() const
{
    return impl(this)->_responseId;
}

/*!
    Sets the identifier of the message that this message was created in response to, to \a id.
*/
void QMailMessageMetaData::setInResponseTo(const QMailMessageId &id)
{
    impl(this)->setInResponseTo(id);
}

/*!
    Returns the type of response that this message was created as.

    \sa inResponseTo()
*/  
QMailMessageMetaData::ResponseType QMailMessageMetaData::responseType() const
{
    return impl(this)->_responseType;
}

/*!
    Sets the type of response that this message was created as to \a type.

    \sa setInResponseTo()
*/
void QMailMessageMetaData::setResponseType(QMailMessageMetaData::ResponseType type)
{
    impl(this)->setResponseType(type);
}

/*!
    Returns the preview text of this message.

    \sa setPreview()
*/  
QString QMailMessageMetaData::preview() const
{
    return impl(this)->_preview;
}

/*!
    Sets the preview text of this message to \a s.

    \sa preview()
*/
void QMailMessageMetaData::setPreview(const QString &s)
{
    impl(this)->setPreview(s);
}

/*!
    Returns true if the entire content of this message is available; otherwise returns false.
*/
bool QMailMessageMetaData::contentAvailable() const
{
    return (status() & QMailMessage::ContentAvailable);
}

/*!
    Returns true if some portion of the content of this message is available; otherwise returns false.
*/
bool QMailMessageMetaData::partialContentAvailable() const
{
    return (status() & QMailMessage::PartialContentAvailable);
}

/*! \internal */
bool QMailMessageMetaData::dataModified() const
{
    return impl(this)->dataModified();
}

/*! \internal */
void QMailMessageMetaData::setUnmodified()
{
    impl(this)->setUnmodified();
}

#ifndef QTOPIAMAIL_PARSING_ONLY
/*!
    Returns the status bitmask needed to test the result of QMailMessageMetaData::status() 
    against the QMailMessageMetaData status flag registered with the identifier \a flagName.

    \sa status(), QMailStore::messageStatusMask()
*/
quint64 QMailMessageMetaData::statusMask(const QString &flagName)
{
    return QMailStore::instance()->messageStatusMask(flagName);
}
#endif

/*! 
    Returns the value recorded in the custom field named \a name.

    \sa setCustomField(), customFields()
*/
QString QMailMessageMetaData::customField(const QString &name) const
{
    return d->customField(name);
}

/*! 
    Sets the value of the custom field named \a name to \a value.

    \sa customField(), customFields()
*/
void QMailMessageMetaData::setCustomField(const QString &name, const QString &value)
{
    d->setCustomField(name, value);
}

/*! 
    Removes the custom field named \a name.

    \sa customField(), customFields()
*/
void QMailMessageMetaData::removeCustomField(const QString &name)
{
    d->removeCustomField(name);
}

/*! 
    Returns the map of custom fields stored in the message.

    \sa customField(), setCustomField()
*/
const QMap<QString, QString> &QMailMessageMetaData::customFields() const
{
    return d->customFields();
}

/*! \internal */
void QMailMessageMetaData::setCustomFields(const QMap<QString, QString> &fields)
{
    d->setCustomFields(fields);
}

/*! \internal */
bool QMailMessageMetaData::customFieldsModified() const
{
    return d->_customFieldsModified;
}

/*! \internal */
void QMailMessageMetaData::setCustomFieldsModified(bool set)
{
    d->_customFieldsModified = set;
}


/*! 
    \fn QMailMessageMetaData::serialize(Stream&) const
    \internal 
*/
template <typename Stream>
void QMailMessageMetaData::serialize(Stream &stream) const
{
    impl(this)->serialize(stream);
}

template void QMailMessageMetaData::serialize(QDataStream &) const;

/*! 
    \fn QMailMessageMetaData::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessageMetaData::deserialize(Stream &stream)
{
    impl(this)->deserialize(stream);
}

template void QMailMessageMetaData::deserialize(QDataStream &);


/*  QMailMessage */

QMailMessagePrivate::QMailMessagePrivate()
    : QMailMessagePartContainerPrivate(this)
{
}

void QMailMessagePrivate::fromRfc2822(const LongString &ls)
{
    _messageParts.clear();

    if (ls.length()) {
        QMailMessageContentType contentType(headerField("Content-Type"));

        // Is this a simple mail or a multi-part collection?
        QByteArray mimeVersion = headerField("MIME-Version");
        QByteArray minimalVersion = QMailMessageHeaderField::removeWhitespace(QMailMessageHeaderField::removeComments(mimeVersion));
        if (!mimeVersion.isEmpty() && (minimalVersion != "1.0")) {
            qWarning() << "Unknown MIME-Version:" << mimeVersion;
        } else if (_multipartType != QMailMessagePartContainer::MultipartNone) {
            parseMimeMultipart(_header, ls, true);
        } else {
            QByteArray bodyData;

            // Remove the pop-style terminator if present
            const QByteArray popTerminator((QByteArray(QMailMessage::CRLF) + '.' + QMailMessage::CRLF));
            if ( ls.indexOf(popTerminator, -popTerminator.length()) != -1)
                bodyData = ls.left( ls.length() - popTerminator.length() ).toQByteArray();
            else
                bodyData = ls.toQByteArray();

            // The body data is already encoded
            QDataStream in(bodyData);
            QMailMessageBody::TransferEncoding encoding = encodingForName(headerField("Content-Transfer-Encoding"));
            if ( encoding == QMailMessageBody::NoEncoding )
                encoding = QMailMessageBody::SevenBit;

            setBody( QMailMessageBody::fromStream(in, contentType, encoding, QMailMessageBody::AlreadyEncoded) );
        }
    }
}

void QMailMessagePrivate::setId(const QMailMessageId& id)
{
    setLocation(id, _indices);
}

void QMailMessagePrivate::setSubject(const QString& s)
{
    updateHeaderField( "Subject:", s );
}

void QMailMessagePrivate::setDate(const QMailTimeStamp& timeStamp)
{
    updateHeaderField( "Date:", to7BitAscii(timeStamp.toString()) );
}

void QMailMessagePrivate::setFrom(const QString& s)
{
    updateHeaderField( "From:", s );
} 

void QMailMessagePrivate::setReplyTo(const QString& s)
{
    updateHeaderField( "Reply-To:", s );
}

void QMailMessagePrivate::setInReplyTo(const QString& s)
{
    updateHeaderField( "In-Reply-To:", s );
}

void QMailMessagePrivate::setTo(const QString& s)
{
    updateHeaderField( "To:", s );
}

void QMailMessagePrivate::setBcc(const QString& s)
{
    updateHeaderField( "Bcc:", s );
}

void QMailMessagePrivate::setCc(const QString& s)
{
    updateHeaderField( "Cc:", s );
}

bool QMailMessagePrivate::hasRecipients() const
{
    if ( !headerField("To").isEmpty() )
        return true;
    if ( !headerField("Cc").isEmpty() )
        return true;
    if ( !headerField("Bcc").isEmpty() )
        return true;

    return false;
}

uint QMailMessagePrivate::indicativeSize() const
{
    uint size = QMailMessagePartContainerPrivate::indicativeSize(); 

    // Count the message header as one size unit
    return (size + 1);
}

static uint currentTimeValue()
{
    return QDateTime::currentDateTime().toTime_t();
}

static bool seedRng()
{
    qsrand(currentTimeValue());
    return true;
}

static int randomNumber()
{
    static bool initialised = seedRng();
    Q_UNUSED(initialised)

    return qrand();
}

static QByteArray gBoundaryString;

void QMF_EXPORT setQMailMessageBoundaryString(const QByteArray &boundary)
{
    gBoundaryString = boundary;
}

static QByteArray boundaryString(const QByteArray &hash)
{
    static const QByteArray boundaryLeader = "[)}<";
    static const QByteArray boundaryTrailer = ")}<]";

    if (!gBoundaryString.isEmpty())
        return gBoundaryString;

    // Formulate a boundary that is very unlikely to clash with the content
    return boundaryLeader + "qmf:" + QByteArray::number(randomNumber()) + hash.toBase64() + boundaryTrailer;
}

template <typename F>
void QMailMessagePrivate::toRfc2822(QDataStream **out, QMailMessage::EncodingFormat format, quint64 messageStatus, F *func) const
{
    bool isOutgoing = (messageStatus & (QMailMessage::Outgoing | QMailMessage::Sent));

    bool addTimeStamp = (format != QMailMessage::IdentityFormat);
    bool addContentHeaders = ((format != QMailMessage::IdentityFormat) && 
                              ((format != QMailMessage::StorageFormat) || isOutgoing || !hasBody()));
    bool includeBcc = (format != QMailMessage::TransmissionFormat);
    bool excludeInternalFields = (format == QMailMessage::TransmissionFormat);

    const_cast<QMailMessagePrivate*>(this)->generateBoundary();

    outputHeaders(**out, addTimeStamp, addContentHeaders, includeBcc, excludeInternalFields);
    **out << DataString('\n');

    if (format != QMailMessage::HeaderOnlyFormat) {
        if ( hasBody() ) {
            outputBody( **out, true); //not multipart so part should not be an attachment
        } else {
            bool addMimePreamble = (format == QMailMessage::TransmissionFormat);
            bool includeAttachments = (format != QMailMessage::StorageFormat);

            outputParts<F>( out, addMimePreamble, includeAttachments, excludeInternalFields, func );
        }
    }
}

void QMailMessagePrivate::outputHeaders( QDataStream& out, bool addTimeStamp, bool addContentHeaders, bool includeBcc, bool excludeInternalFields ) const
{
    QList<QByteArray> exclusions;

    if (addContentHeaders) {
        // Don't include the nominated MIME-Version if specified - we implement 1.0
        exclusions.append("MIME-Version");
    }
    if (!includeBcc) {
        exclusions.append("bcc");
    }

    _header.output( out, exclusions, excludeInternalFields );

    if (addTimeStamp && headerField("Date").isEmpty()) {
        QString timeStamp = QMailTimeStamp( QDateTime::currentDateTime() ).toString();
        out << DataString("Date: ") << DataString(to7BitAscii(timeStamp)) << DataString('\n');
    }

    if (addContentHeaders) {
        // Output required content header fields
        out << DataString("MIME-Version: 1.0") << DataString('\n');
    }
}

bool QMailMessagePrivate::contentModified() const
{
    // For this part of any sub-part
    return dirty(true);
}

void QMailMessagePrivate::setUnmodified()
{
    setDirty(false, true);
}

template <typename Stream> 
void QMailMessagePrivate::serialize(Stream &stream) const
{
    QMailMessagePartContainerPrivate::serialize(stream);
}

template <typename Stream> 
void QMailMessagePrivate::deserialize(Stream &stream)
{
    QMailMessagePartContainerPrivate::deserialize(stream);
}


//===========================================================================

/*!
    \class QMailMessage

    \preliminary
    \brief The QMailMessage class provides a convenient interface for working with messages.
    
    \ingroup messaginglibrary
   
    QMailMessage supports a number of types. These include telephony types 
    such as SMS and MMS, and Internet email messages as defined in
    \l{http://www.ietf.org/rfc/rfc2822.txt} {RFC 2822} (Internet Message Format), and 
    \l{http://www.ietf.org/rfc/rfc2045.txt} {RFC 2045} (Format of Internet Message Bodies) through 
    \l{http://www.ietf.org/rfc/rfc2049.txt} {RFC 2049} (Conformance Criteria and Examples).
    
    The most common way to use QMailMessage is to initialize it from raw
    data using QMailMessage::fromRfc2822().
    
    A QMailMessage can also be constructed piece by piece using functions such as 
    setMessageType(), setFrom(), setTo(), setSubject(), and setBody() or appendPart(). 
    Convenience functions such as from()/setFrom() and date()/setDate() accept and
    return wrapper types, to simplify the exchange of correctly-formatted data.
    In some cases, however, it may be simpler for clients to get and set the content 
    of header fields directly, using the headerField() and setHeaderField() functions inherited
    from QMailMessagePartContainer.
    
    Messages can be added to the QMailStore, or retrieved from the store via their QMailMessageId 
    identifier.  The QMailMessage object also provides access to any relevant meta data
    describing the message, using the functions inherited from QMailMessageMetaData.

    A message may be serialized to a QDataStream, or returned as a QByteArray using toRfc2822().
    
    \sa QMailMessageMetaData, QMailMessagePart, QMailMessageBody, QMailStore, QMailMessageId
*/    


const char QMailMessage::CarriageReturn = '\015';
const char QMailMessage::LineFeed = '\012';
const char* QMailMessage::CRLF = "\015\012";

/*!
    Constructs an empty message object.
*/
QMailMessage::QMailMessage()
    : QMailMessageMetaData(),
      QMailMessagePartContainer(new QMailMessagePrivate())
{
}

#ifndef QTOPIAMAIL_PARSING_ONLY
/*!
    Constructs a message object from data stored in the message store with QMailMessageId \a id.
*/
QMailMessage::QMailMessage(const QMailMessageId& id)
    : QMailMessageMetaData(id),
      QMailMessagePartContainer(reinterpret_cast<QMailMessagePrivate*>(0))
{
    *this = QMailStore::instance()->message(id);
}

/*!
    Constructs a message object from data stored in the message store with the unique 
    identifier \a uid from the account with id \a accountId.
*/
QMailMessage::QMailMessage(const QString& uid, const QMailAccountId& accountId)
    : QMailMessageMetaData(uid, accountId),
      QMailMessagePartContainer(reinterpret_cast<QMailMessagePrivate*>(0))
{
    *this = QMailStore::instance()->message(uid, accountId);
}
#endif

/*!
    Constructs a mail message from the RFC 2822 data contained in \a byteArray.
*/
QMailMessage QMailMessage::fromRfc2822(const QByteArray &byteArray)
{
    LongString ls(byteArray);
    QMailMessage mail = fromRfc2822(ls);
    mail.extractUndecodedData(ls);
    return mail;
}

/*!
    Constructs a mail message from the RFC 2822 data contained in the file \a fileName.
*/
QMailMessage QMailMessage::fromRfc2822File(const QString& fileName)
{
    LongString ls(fileName);
    QMailMessage mail = fromRfc2822(ls);
    mail.extractUndecodedData(ls);
    return mail;
}

/*!
    Constructs a mail message from the RFC 2822 data contained in the file \a fileName
    TODO, modify the load method not to populate the bodies, just load the structure
    and the meta-data.
*/
QMailMessage QMailMessage::fromSkeletonRfc2822File(const QString& fileName)
{
    LongString ls(fileName);
    return fromRfc2822(ls);
}

/*!
    Returns true if the message contains a part with the location \a location.
*/
bool QMailMessage::contains(const QMailMessagePart::Location& location) const
{
    return partContainerImpl()->contains(location);
}

/*!
    Returns a const reference to the part at the location \a location within the message.
*/
const QMailMessagePart& QMailMessage::partAt(const QMailMessagePart::Location& location) const
{
    return partContainerImpl()->partAt(location);
}

/*!
    Returns a non-const reference to the part at the location \a location within the message.
*/
QMailMessagePart& QMailMessage::partAt(const QMailMessagePart::Location& location)
{
    return partContainerImpl()->partAt(location);
}

/*! \reimp */
void QMailMessage::setHeaderField( const QString& id, const QString& value )
{
    QMailMessagePartContainer::setHeaderField(id, value);

    QByteArray duplicatedId(duplicatedData(id));
    if (!duplicatedId.isNull()) {
        updateMetaData(duplicatedId, value);
    }
}

/*! \reimp */
void QMailMessage::setHeaderField( const QMailMessageHeaderField& field )
{
    setHeaderField(QString::fromUtf8(field.id()), QString::fromUtf8(field.toString(false, false)));
}

/*! \reimp */
void QMailMessage::appendHeaderField( const QString& id, const QString& value )
{
    QMailMessagePartContainer::appendHeaderField(id, value);

    QByteArray duplicatedId(duplicatedData(id));
    if (!duplicatedId.isNull()) {
        // We need to keep the value of the first item with this ID in the meta data object
        updateMetaData(duplicatedId, headerFieldText(QString::fromUtf8(duplicatedId)));
    }
}

/*! \reimp */
void QMailMessage::appendHeaderField( const QMailMessageHeaderField& field )
{
    appendHeaderField(QString::fromUtf8(field.id()), QString::fromUtf8(field.toString(false, false)));
}

/*! \reimp */
void QMailMessage::removeHeaderField( const QString& id )
{
    QMailMessagePartContainer::removeHeaderField(id);

    QByteArray duplicatedId(duplicatedData(id));
    if (!duplicatedId.isNull()) {
        updateMetaData(duplicatedId, QString());
    }
}

/*!
    Returns the message in RFC 2822 format. The encoded content will vary depending on the value of \a format.
*/
QByteArray QMailMessage::toRfc2822(EncodingFormat format) const
{
    QByteArray result;
    {
        QDataStream out(&result, QIODevice::WriteOnly);
        toRfc2822(out, format);
    }
    return result;
}

/*!
    Writes the message to the output stream \a out, in RFC 2822 format. 
    The encoded content will vary depending on the value of \a format.
*/
void QMailMessage::toRfc2822(QDataStream& out, EncodingFormat format) const
{
    QDataStream *ds(&out);
    partContainerImpl()->toRfc2822<DummyChunkProcessor>(&ds, format, status(), Q_NULLPTR);
}

struct ChunkStore
{
    QList<QMailMessage::MessageChunk> chunks;
    QByteArray chunk;
    QDataStream *ds;

    ChunkStore()
        : ds(new QDataStream(&chunk, QIODevice::WriteOnly | QIODevice::Unbuffered))
    {
    }

    ~ChunkStore()
    {
        close();
    }

    void close() 
    {
        if (ds) {
            delete ds;
            ds = Q_NULLPTR;

            if (!chunk.isEmpty()) {
                chunks.append(qMakePair(QMailMessage::Text, chunk));
            }
        }
    }

    void operator()(QMailMessage::ChunkType type)
    {
        // This chunk is now complete
        delete ds;
        chunks.append(qMakePair(type, chunk));

        chunk.clear();
        ds = new QDataStream(&chunk, QIODevice::WriteOnly | QIODevice::Unbuffered);
    }
};

/*!
    Returns the message in RFC 2822 format, separated into chunks that can
    be individually transferred by a mechanism such as that defined by RFC 3030.
    The encoded content will vary depending on the value of \a format.
*/
QList<QMailMessage::MessageChunk> QMailMessage::toRfc2822Chunks(EncodingFormat format) const
{
    ChunkStore store;

    partContainerImpl()->toRfc2822<ChunkStore>(&store.ds, format, status(), &store);
    store.close();

    return store.chunks;
}

/*! \reimp */
void QMailMessage::setId(const QMailMessageId &id)
{
    metaDataImpl()->setId(id);
    partContainerImpl()->setId(id);
}

/*! \reimp */
void QMailMessage::setFrom(const QMailAddress &from)
{
    metaDataImpl()->setFrom(from.toString());
    partContainerImpl()->setFrom(from.toString());
}

/*! \reimp */
void QMailMessage::setSubject(const QString &subject)
{
    metaDataImpl()->setSubject(subject);
    partContainerImpl()->setSubject(subject);
}

/*! \reimp */
void QMailMessage::setDate(const QMailTimeStamp &timeStamp)
{
    metaDataImpl()->setDate(timeStamp);
    partContainerImpl()->setDate(timeStamp);
}

/*!
    Returns a list of all the primary recipients specified for the message.

    \sa cc(), bcc(), QMailAddress
*/
QList<QMailAddress> QMailMessage::to() const
{
    return QMailAddress::fromStringList(headerFieldText(QLatin1String("To")));
}

/*!
    Set the list of to recipients for the message to \a toList.

    \sa setCc(), setBcc()
*/  
void QMailMessage::setTo(const QList<QMailAddress>& toList)
{
    metaDataImpl()->setRecipients(QMailAddress::toStringList(toList + cc() + bcc()).join(QLatin1String(", ")));
    partContainerImpl()->setTo(QMailAddress::toStringList(toList).join(QLatin1String(", ")));
}

/*!
    Set the list of to recipients for the message to a list containing a single item \a address.

    \sa setCc(), setBcc(), QMailAddress
*/  
void QMailMessage::setTo(const QMailAddress& address)
{
    setTo(QList<QMailAddress>() << address);
}

/*!
    Returns a list of all the cc (carbon copy) recipients specified for the message.

    \sa to(), bcc(), QMailAddress
*/  
QList<QMailAddress> QMailMessage::cc() const
{
    return QMailAddress::fromStringList(headerFieldText(QLatin1String("Cc")));
}

/*!
    Set the list of cc (carbon copy) recipients for the message to \a ccList.

    \sa setTo(), setBcc()
*/  
void QMailMessage::setCc(const QList<QMailAddress>& ccList)
{
    metaDataImpl()->setRecipients(QMailAddress::toStringList(to() + ccList + bcc()).join(QLatin1String(", ")));
    partContainerImpl()->setCc(QMailAddress::toStringList(ccList).join(QLatin1String(", ")));
}

/*!
    Returns a list of all the bcc (blind carbon copy) recipients specified for the message.

    \sa to(), cc(), QMailAddress
*/  
QList<QMailAddress> QMailMessage::bcc() const
{
    return QMailAddress::fromStringList(headerFieldText(QLatin1String("Bcc")));
}

/*!
    Set the list of bcc (blind carbon copy) recipients for the message to \a bccList.

    \sa setTo(), setCc()
*/  
void QMailMessage::setBcc(const QList<QMailAddress>& bccList)
{
    metaDataImpl()->setRecipients(QMailAddress::toStringList(to() + cc() + bccList).join(QLatin1String(", ")));
    partContainerImpl()->setBcc(QMailAddress::toStringList(bccList).join(QLatin1String(", ")));
}

/*!
    Returns the address specified by the RFC 2822 'Reply-To' field for the message, if present.
*/  
QMailAddress QMailMessage::replyTo() const
{
    return QMailAddress(headerFieldText(QLatin1String("Reply-To")));
}

/*!
    Sets the RFC 2822 'Reply-To' address of the message to \a address.
*/  
void QMailMessage::setReplyTo(const QMailAddress &address)
{
    partContainerImpl()->setReplyTo(address.toString());
}

/*!
    Returns the message ID specified by the RFC 2822 'In-Reply-To' field for the message, if present.
*/  
QString QMailMessage::inReplyTo() const
{
    return headerFieldText(QLatin1String("In-Reply-To"));
}

/*!
    Sets the RFC 2822 'In-Reply-To' field for the message to \a messageId.
*/
void QMailMessage::setInReplyTo(const QString &messageId)
{
    partContainerImpl()->setInReplyTo(messageId);
}

/*!
    Returns a list of all the recipients specified for the message, either as To, CC, or BCC addresses.

    \sa to(), cc(), bcc(), hasRecipients()
*/  
QList<QMailAddress> QMailMessage::recipients() const
{
    QList<QMailAddress> addresses;

    QStringList list;
    list.append( headerFieldText(QLatin1String("To")).trimmed() );
    list.append( headerFieldText(QLatin1String("Cc")).trimmed() );
    list.append( headerFieldText(QLatin1String("Bcc")).trimmed() );
    if (!list.isEmpty()) {
        list.removeAll(QLatin1String(""));
        list.removeAll(QString());
    }
    if (!list.isEmpty()) {
        addresses += QMailAddress::fromStringList(list.join(QLatin1String(",")));
    }

    return addresses;
}

/*!
    Returns true if there are any recipients (either To, CC or BCC addresses) 
    defined for this message; otherwise returns false.
*/  
bool QMailMessage::hasRecipients() const
{
    return partContainerImpl()->hasRecipients();
}

/*! \reimp */  
uint QMailMessage::indicativeSize() const
{
    // Count the message header as one size unit
    return partContainerImpl()->indicativeSize() + 1;
}

/*!
    Returns the size of the message content excluding any meta data, in bytes.
*/  
uint QMailMessage::contentSize() const
{
    return customField(QStringLiteral("qtopiamail-content-size")).toUInt();
}

/*!
    Sets the size of the message content excluding any meta data to \a size, in bytes.
*/
void QMailMessage::setContentSize(uint size)
{
    setCustomField(QStringLiteral("qtopiamail-content-size"), QString::number(size));
}

/*!
    Returns a value by which the external location of the message can be referenced, if available.
*/  
QString QMailMessage::externalLocationReference() const
{
    return customField(QStringLiteral("qtopiamail-external-location-reference"));
}

/*!
    Returns the value by which the external location of the message can be referenced to \a location.
*/
void QMailMessage::setExternalLocationReference(const QString &location)
{
    setCustomField(QStringLiteral("qtopiamail-external-location-reference"), location);
}

/*! \reimp */
bool QMailMessage::contentAvailable() const
{
    return QMailMessageMetaData::contentAvailable();
}

/*! \reimp */
bool QMailMessage::partialContentAvailable() const
{
    return QMailMessageMetaData::partialContentAvailable();
}

/*! \reimp */
QString QMailMessage::preview() const
{
    if (partContainerImpl()->previewDirty())
    {
        const_cast<QMailMessage*>(this)->refreshPreview();
    }

    return QMailMessageMetaData::preview();
}


// The QMMMetaData half of this object is implemented in a QMailMessageMetaDataPrivate object
/*! \internal */
QMailMessageMetaDataPrivate* QMailMessage::metaDataImpl() 
{ 
    return QMailMessageMetaData::impl<QMailMessageMetaDataPrivate>(); 
}

/*! \internal */
const QMailMessageMetaDataPrivate* QMailMessage::metaDataImpl() const 
{ 
    return QMailMessageMetaData::impl<const QMailMessageMetaDataPrivate>(); 
}

// The QMMPartContainer half of this object is implemented in a QMailMessagePrivate object
/*! \internal */
QMailMessagePrivate* QMailMessage::partContainerImpl() 
{ 
    return QMailMessagePartContainer::impl<QMailMessagePrivate>(); 
}

/*! \internal */
const QMailMessagePrivate* QMailMessage::partContainerImpl() const 
{ 
    return QMailMessagePartContainer::impl<const QMailMessagePrivate>(); 
}
    
/*! \internal */
bool QMailMessage::contentModified() const
{
    return partContainerImpl()->contentModified();
}

/*!
    Returns true if the message contains a calendar invitation;
    otherwise returns false.
*/  
bool QMailMessage::hasCalendarInvitation() const
{
    QList<const QMailMessagePartContainer*> parts;
    parts.append(this);

    while (!parts.isEmpty()) {
        const QMailMessagePartContainer *part(parts.takeFirst());
        if (part->multipartType() != QMailMessagePartContainer::MultipartNone) {
            for (uint i = 0; i < part->partCount(); ++i) {
            parts.append(&part->partAt(i));
            }
        } else {
            const QMailMessageContentType &ct(part->contentType());
            if (ct.matches("text", "calendar") &&
                (ct.parameter("method").toLower() == "request")) {
                return true;
            }
        }
    }
    return false;
}

/*! \internal */
void QMailMessage::setUnmodified()
{
    metaDataImpl()->setUnmodified();
    partContainerImpl()->setUnmodified();
}

/*! \internal */
void QMailMessage::setHeader(const QMailMessageHeader& partHeader, const QMailMessagePartContainerPrivate* parent)
{
    QMailMessagePartContainer::setHeader(partHeader, parent);
    // See if any of the header fields need to be propagated to the meta data object
    foreach (const QMailMessageHeaderField& field, headerFields()) {
        QByteArray duplicatedId(duplicatedData(QString::fromUtf8(field.id())));
        if (!duplicatedId.isNull()) {
            QMailMessageContentType ct(headerField(QStringLiteral("Content-Type")));
            if (!is7BitAscii(field.content()) && unicodeConvertingCharset(ct.charset())) {
                updateMetaData(duplicatedId, toUnicode(field.content(), ct.charset()));
            } else {
                updateMetaData(duplicatedId, field.decodedContent());
            }
        }
    }
}

/*! \internal */
QByteArray QMailMessage::duplicatedData(const QString& id) const
{
    // These items are duplicated in both the message content and the meta data
    QByteArray plainId( to7BitAscii(id).trimmed().toLower() );

    if ((plainId == "from") || (plainId == "to") || (plainId == "subject") ||
        (plainId == "date") || (plainId == "list-id") || plainId == "message-id" ||
        plainId == "cc" || plainId == "bcc")
        return plainId;

    return QByteArray();
}

/*! \internal */
void QMailMessage::updateMetaData(const QByteArray& id, const QString& value)
{
    if (id == "from") {
        metaDataImpl()->setFrom(value);
    } else if (id == "to") {
        metaDataImpl()->setRecipients(QMailAddress::toStringList(QMailAddress::fromStringList(value) + cc() + bcc()).join(QLatin1String(", ")));
    } else if (id == "cc") {
        metaDataImpl()->setRecipients(QMailAddress::toStringList(to() + QMailAddress::fromStringList(value) + bcc()).join(QLatin1String(", ")));
    } else if (id == "bcc") {
        metaDataImpl()->setRecipients(QMailAddress::toStringList(to() + cc() + QMailAddress::fromStringList(value)).join(QLatin1String(", ")));
    } else if (id == "subject") {
        metaDataImpl()->setSubject(value);
    } else if (id == "date") {
        metaDataImpl()->setDate(QMailTimeStamp(value));
    } else if (id == "list-id") {
        int to(value.lastIndexOf('>'));
        int from(value.lastIndexOf('<', to)+1);

        if (from > 0 && to > from)
            metaDataImpl()->setListId(value.mid(from, to-from).trimmed());
    } else if (id == "message-id") {
        QStringList identifiers(QMail::messageIdentifiers(value));
        if (!identifiers.isEmpty())
            metaDataImpl()->setRfcId(identifiers.first());
    }
}

static void setMessagePriorityFromHeaderFields(QMailMessage *mail) 
{
    bool ok;
    QString priority = mail->headerFieldText(QLatin1String("X-Priority"));
    if (!priority.isEmpty()) {
        // Consider X-Priority field first
        int value = priority.toInt(&ok);
        if (ok) {
            if (value < 3) {
                mail->setStatus(QMailMessage::HighPriority, true);
                return;
            } else if (value > 3) {
                mail->setStatus(QMailMessage::LowPriority, true);
                return;
            } else {
                return; // Normal Priority
            }
        }
    }

    // If X-Priority is not set, consider X-MSMail-Priority
    QString msPriority = mail->headerFieldText (QLatin1String("X-MSMail-Priority"));
    if (!msPriority.isEmpty()) {
        if (msPriority.contains (QLatin1String("high"), Qt::CaseInsensitive)) {
            mail->setStatus(QMailMessage::HighPriority, true);
            return;
        } else if (msPriority.contains (QLatin1String("low"), Qt::CaseInsensitive)) {
            mail->setStatus(QMailMessage::LowPriority, true);
            return;
        } else if (msPriority.contains (QLatin1String("normal"), Qt::CaseInsensitive)) {
            return; // Normal Priority
        }
    }

    // Finally, consider Importance
    QString importance = mail->headerFieldText(QLatin1String("Importance"));
    if (!importance.isEmpty()) {
        if (importance.contains (QLatin1String("high"), Qt::CaseInsensitive)) {
            mail->setStatus(QMailMessage::HighPriority, true);
            return;
        } else if (importance.contains (QLatin1String("low"), Qt::CaseInsensitive)) {
            mail->setStatus(QMailMessage::LowPriority, true);
            return;
        } else if (importance.contains (QLatin1String("normal"), Qt::CaseInsensitive)) {
            return; // Normal Priority
        }
    }

    return; // Normal Priority
}

static QString htmlToPlainText(const QString &html)
{
#ifdef USE_HTML_PARSER
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText();
#else
    QString plainText = html;
    plainText.remove(QRegExp(QLatin1String("<\\s*(style|head|form|script)[^<]*<\\s*/\\s*\\1\\s*>"), Qt::CaseInsensitive));
    plainText.remove(QRegExp(QLatin1String("<(.)[^>]*>")));
    plainText.replace(QLatin1String("&quot;"), QLatin1String("\""), Qt::CaseInsensitive);
    plainText.replace(QLatin1String("&nbsp;"), QLatin1String(" "), Qt::CaseInsensitive);
    plainText.replace(QLatin1String("&amp;"), QLatin1String("&"), Qt::CaseInsensitive);
    plainText.replace(QLatin1String("&lt;"), QLatin1String("<"), Qt::CaseInsensitive);
    plainText.replace(QLatin1String("&gt;"), QLatin1String(">"), Qt::CaseInsensitive);

    // now replace stuff like "&#1084;"
    int pos = 0;
    while (true) {
        pos = plainText.indexOf(QLatin1String("&#"), pos);
        if (pos < 0)
            break;
        int semicolon = plainText.indexOf(';', pos+2);
        if (semicolon < 0) {
            ++pos;
            continue;
        }
        int code = (plainText.mid(pos+2, semicolon-pos-2)).toInt();
        if (code == 0) {
            ++pos;
            continue;
        }
        plainText.replace(pos, semicolon-pos+1, QChar(code));
    }

    return plainText.simplified();
#endif
}

/*! \internal */
void QMailMessage::refreshPreview()
{
    const int maxPreviewLength = 280;
    // TODO: don't load entire body into memory
    QMailMessagePartContainer *htmlPart= findHtmlContainer();
    QMailMessagePartContainer *plainTextPart= findPlainTextContainer();

    if (multipartType() == MultipartRelated && htmlPart) // force taking the html in this case
        plainTextPart=Q_NULLPTR;

    if ( plainTextPart && plainTextPart->hasBody()) {
        QString plainText = plainTextPart->body().data();
        metaDataImpl()->setPreview(plainText.left(maxPreviewLength));
    } else if (htmlPart && ( multipartType() == MultipartRelated || htmlPart->hasBody())) {
        QString markup = htmlPart->body().data();
        metaDataImpl()->setPreview(htmlToPlainText(markup).left(maxPreviewLength));
    }

    partContainerImpl()->setPreviewDirty(false);
}

/*! \internal */
QMailMessage QMailMessage::fromRfc2822(LongString& ls)
{
    const QByteArray CRLFterminator((QByteArray(QMailMessage::CRLF) + QMailMessage::CRLF));
    const QByteArray LFterminator(2, QMailMessage::LineFeed);
    const QByteArray CRterminator(2, QMailMessage::CarriageReturn);

    QMailMessage mail;
    mail.setMessageType(QMailMessage::Email);

    int CRLFindex = ls.indexOf(CRLFterminator);
    int LFindex = ls.indexOf(LFterminator);
    int CRindex = ls.indexOf(CRterminator);

    int pos = CRLFindex;
    QByteArray terminator = CRLFterminator;
    if (pos == -1 || (LFindex > -1 && LFindex < pos)) {
        pos = LFindex;
        terminator = LFterminator;
    }
    if (pos == -1 || (CRindex > -1 && CRindex < pos)) {
        pos = CRindex;
        terminator = CRterminator;
    }

    if (pos == -1) {
        // No body? Parse entirety as header
        mail.setHeader( QMailMessageHeader( ls.toQByteArray() ) );
    } else {
        // Parse the header part to know what we've got
        mail.setHeader( QMailMessageHeader( ls.left(pos).toQByteArray() ) );
 
        // Parse the remainder as content
        mail.partContainerImpl()->fromRfc2822( ls.mid(pos + terminator.length()) );
    }

    // See if any of the header fields need to be propagated to the meta data object
    QMailMessagePartContainer *textBody(Q_NULLPTR);
    QByteArray auxCharset;
    QMailMessageContentType ct(mail.headerField(QLatin1String("Content-Type")));
    if (ct.charset().isEmpty()) {
        if (!textBody) {
            textBody = mail.findPlainTextContainer();
        }
        if (!textBody) {
            textBody = mail.findHtmlContainer();
        }
        if (textBody) {
            QMailMessageHeaderField hf(textBody->headerField(QLatin1String("Content-Type")));
            auxCharset = QMailMessageContentType(hf).charset();
        }
    }

    foreach (const QMailMessageHeaderField& field, mail.headerFields()) {
        QByteArray duplicatedId(mail.duplicatedData(QString::fromUtf8(field.id())));
        if (!duplicatedId.isNull()) {
            if (!is7BitAscii(field.content())) {
                mail.updateMetaData(duplicatedId, toUnicode(field.content(), ct.charset(), auxCharset));
            } else {
                mail.updateMetaData(duplicatedId, field.decodedContent());
            }
        }
    }

    if (mail.metaDataImpl()->_date.isNull()) {
        QByteArray hReceived(mail.partContainerImpl()->headerField("Received"));
        if (!hReceived.isEmpty()) {
            // From rfc2822 received is formatted: "Received:" name-val-list ";" date-time CRLF
            // As the ";" is mandatory this should never fail unless the email is badly formatted
            QStringList sl(QString::fromLatin1(hReceived.data()).split(QLatin1String(";")));
            if (sl.length() == 2) {
                mail.metaDataImpl()->setDate(QMailTimeStamp(sl.at(1)));
            } else {
                qWarning() << "Ill formatted message, bad Received field";
            }
        } else {
            mail.metaDataImpl()->setDate(QMailTimeStamp::currentDateTime());
        }
    }

    setMessagePriorityFromHeaderFields(&mail);
    mail.refreshPreview();
    if (mail.hasAttachments()) {
        mail.setStatus( QMailMessage::HasAttachments, true );
    }

    return mail;
}

bool QMailMessage::extractUndecodedData(const LongString &ls)
{
    QMailMessagePartContainer *signedContainer =
        QMailCryptographicServiceFactory::findSignedContainer(this);
    if (signedContainer) {
        const QByteArray CRLFterminator((QByteArray(QMailMessage::CRLF) + QMailMessage::CRLF));
        const QByteArray LFterminator(2, QMailMessage::LineFeed);
        const QByteArray CRterminator(2, QMailMessage::CarriageReturn);

        int CRLFindex = ls.indexOf(CRLFterminator);
        int LFindex = ls.indexOf(LFterminator);
        int CRindex = ls.indexOf(CRterminator);

        int pos = CRLFindex;
        if (pos == -1 || (LFindex > -1 && LFindex < pos))
            pos = LFindex;
        if (pos == -1 || (CRindex > -1 && CRindex < pos))
            pos = CRindex;
        if (pos == -1) {
            qWarning() << "extractUndecodedData: unable to find line terminator.";
            return false;
        }

        setStatus(QMailMessage::HasSignature, true);
        /* Retrieve the raw data of the signed body. */
        QByteArray boundary = QByteArray(2, '-') + signedContainer->boundary();
        int body_start = ls.indexOf(boundary, pos) + boundary.length();
        // Add one or two bytes depending if the the boundary is followed
        // by CR, LF or CRLF.
        body_start += (ls.mid(body_start, 2).toQByteArray().startsWith("\r\n")) ? 2 : 1;
        int body_stop = ls.indexOf(boundary, body_start);
        int len = body_stop - body_start;
        // Same as above, remove one or two bytes, depending if they are
        // CR, LF or CRLF.
        len -= (ls.mid(body_stop - 2, 2).toQByteArray().startsWith("\r\n")) ? 2 : 1;
        QByteArray raw = QByteArray(ls.mid(body_start, len).toQByteArray().data(), len); /* Do a copy here because data from LongString are volatile. */
        signedContainer->partAt(0).setUndecodedData(raw);
    }
    return true;
}

/*! 
    \fn QMailMessage::serialize(Stream&) const
    \internal 
*/
template <typename Stream> 
void QMailMessage::serialize(Stream &stream) const
{
    metaDataImpl()->serialize(stream);
    partContainerImpl()->serialize(stream);
}

template void QMailMessage::serialize(QDataStream &) const;

/*! 
    \fn QMailMessage::deserialize(Stream&)
    \internal 
*/
template <typename Stream> 
void QMailMessage::deserialize(Stream &stream)
{
    metaDataImpl()->deserialize(stream);
    partContainerImpl()->deserialize(stream);
}

template void QMailMessage::deserialize(QDataStream &);
