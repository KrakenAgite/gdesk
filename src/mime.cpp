#include "mime.h"

#include <QJsonArray>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QUrl>

namespace {

QString header(const QJsonArray &headers, const QString &name)
{
    for (const QJsonValue &v : headers) {
        const QJsonObject o = v.toObject();
        if (o.value("name").toString().compare(name, Qt::CaseInsensitive) == 0)
            return o.value("value").toString();
    }
    return {};
}

QByteArray base64UrlDecode(const QString &s)
{
    return QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding);
}

QString decodeCharset(const QByteArray &data, QString charset)
{
    charset = charset.toLower().section('*', 0, 0); // RFC 2231 : « utf-8*fr »
    if (charset.isEmpty() || charset == "utf-8" || charset == "utf8" || charset == "us-ascii")
        return QString::fromUtf8(data);
    QStringDecoder decoder(charset.toLatin1().constData()); // ICU : tous les jeux de caractères courants
    if (decoder.isValid())
        return decoder.decode(data);
    if (charset.startsWith("iso-8859") || charset.startsWith("windows-125"))
        return QString::fromLatin1(data);
    return QString::fromUtf8(data);
}

QString decodeText(const QByteArray &data, const QString &contentType)
{
    static const QRegularExpression re(R"(charset\s*=\s*"?([^";\s]+))", QRegularExpression::CaseInsensitiveOption);
    const QString charset = re.match(contentType).captured(1).toLower();
    return decodeCharset(data, charset);
}

// Décode les mots encodés RFC 2047 (=?charset?B|Q?texte?=) restés dans un en-tête.
// Les mots consécutifs de même jeu de caractères sont décodés ensemble, pour recoller
// un caractère (emoji, idéogramme…) coupé entre deux mots.
QString decodeEncodedWords(const QString &value)
{
    static const QRegularExpression word(R"(=\?([^?\s]+)\?([BbQq])\?([^?\s]*)\?=)");
    if (!value.contains("=?"))
        return value;
    QString out;
    QByteArray pending;
    QString pendingCharset;
    auto flush = [&] {
        if (!pending.isEmpty())
            out += decodeCharset(pending, pendingCharset);
        pending.clear();
    };
    qsizetype pos = 0;
    bool previousWasWord = false;
    auto it = word.globalMatch(value);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString between = value.mid(pos, m.capturedStart() - pos);
        // Les blancs entre deux mots encodés ne font pas partie du texte (RFC 2047 §6.2)
        if (!(previousWasWord && between.trimmed().isEmpty())) {
            flush();
            out += between;
        }
        const QString charset = m.captured(1);
        QByteArray bytes;
        const QByteArray payload = m.captured(3).toLatin1();
        if (m.captured(2).compare("B", Qt::CaseInsensitive) == 0) {
            bytes = QByteArray::fromBase64(payload);
        } else {
            QByteArray q = payload;
            q.replace('_', ' ');
            bytes = QByteArray::fromPercentEncoding(q, '=');
        }
        if (charset.compare(pendingCharset, Qt::CaseInsensitive) != 0)
            flush();
        pendingCharset = charset;
        pending += bytes;
        pos = m.capturedEnd();
        previousWasWord = true;
    }
    flush();
    out += value.mid(pos);
    return out;
}

void walk(const QJsonObject &part, MailMessage &m)
{
    const QString mime = part.value("mimeType").toString().toLower();
    const QJsonArray headers = part.value("headers").toArray();
    const QJsonObject body = part.value("body").toObject();
    const QString filename = part.value("filename").toString();
    const QJsonArray parts = part.value("parts").toArray();

    if (mime.startsWith("multipart/") || mime == "message/rfc822") {
        for (const QJsonValue &p : parts)
            walk(p.toObject(), m);
        return;
    }

    const QString disposition = header(headers, "Content-Disposition").toLower();
    const bool isText = mime == "text/plain" || mime == "text/html";
    if (!filename.isEmpty() || disposition.startsWith("attachment") || (!isText && body.contains("attachmentId"))) {
        Attachment a;
        a.filename = filename.isEmpty() ? QStringLiteral("piece-jointe") : decodeEncodedWords(filename);
        a.mimeType = mime;
        a.attachmentId = body.value("attachmentId").toString();
        a.size = body.value("size").toInteger();
        a.contentId = header(headers, "Content-ID").trimmed();
        if (a.contentId.startsWith('<') && a.contentId.endsWith('>'))
            a.contentId = a.contentId.mid(1, a.contentId.size() - 2);
        a.isInline = !a.contentId.isEmpty() && mime.startsWith("image/") && !disposition.startsWith("attachment");
        if (body.contains("data"))
            a.data = base64UrlDecode(body.value("data").toString());
        m.attachments.append(a);
        return;
    }

    const QString text = decodeText(base64UrlDecode(body.value("data").toString()), header(headers, "Content-Type"));
    if (mime == "text/html")
        m.html += text;
    else if (mime == "text/plain")
        m.text += text;
}

bool isPlainAscii(const QString &s)
{
    for (QChar c : s)
        if (c.unicode() > 126 || c.unicode() < 32)
            return false;
    return true;
}

// Mot encodé RFC 2047, découpé en morceaux de moins de 75 caractères
QByteArray encodeWord(const QString &s)
{
    if (isPlainAscii(s))
        return s.toUtf8();
    QByteArray out;
    QString chunk;
    auto flush = [&] {
        if (chunk.isEmpty())
            return;
        if (!out.isEmpty())
            out += "\r\n ";
        out += "=?UTF-8?B?" + chunk.toUtf8().toBase64() + "?=";
        chunk.clear();
    };
    for (int i = 0; i < s.size(); ++i) {
        QString c = s.mid(i, 1);
        if (s.at(i).isHighSurrogate() && i + 1 < s.size())
            c = s.mid(i++, 2);
        if ((chunk + c).toUtf8().size() > 45)
            flush();
        chunk += c;
    }
    flush();
    return out;
}

QByteArray encodeAddressList(const QString &list)
{
    static const QRegularExpression re(R"(^(.*?)\s*<([^>]+)>$)");
    static const QRegularExpression special(R"([()<>@,;:\\".\[\]])");
    QList<QByteArray> out;
    for (const QString &raw : Mime::splitAddresses(list)) {
        const auto m = re.match(raw);
        if (!m.hasMatch()) {
            out << raw.toUtf8();
            continue;
        }
        QString name = m.captured(1).trimmed();
        if (name.size() >= 2 && name.startsWith('"') && name.endsWith('"'))
            name = name.mid(1, name.size() - 2);
        const QByteArray addr = "<" + m.captured(2).trimmed().toUtf8() + ">";
        if (name.isEmpty())
            out << addr;
        else if (!isPlainAscii(name))
            out << encodeWord(name) + " " + addr;
        else if (name.contains(special))
            out << "\"" + name.toUtf8().replace("\\", "\\\\").replace("\"", "\\\"") + "\" " + addr;
        else
            out << name.toUtf8() + " " + addr;
    }
    return out.join(", ");
}

QByteArray wrappedBase64(const QByteArray &data)
{
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    for (qsizetype i = 0; i < b64.size(); i += 76)
        out += b64.mid(i, 76) + "\r\n";
    return out;
}

} // namespace

namespace Mime {

MailMessage parseMessage(const QJsonObject &json)
{
    MailMessage m;
    m.id = json.value("id").toString();
    m.threadId = json.value("threadId").toString();
    // L'extrait est fourni avec des entités HTML (&#39; …)
    m.snippet = QTextDocumentFragment::fromHtml(json.value("snippet").toString()).toPlainText();
    for (const QJsonValue &l : json.value("labelIds").toArray())
        m.labelIds << l.toString();
    m.date = QDateTime::fromMSecsSinceEpoch(json.value("internalDate").toString().toLongLong());

    const QJsonObject payload = json.value("payload").toObject();
    const QJsonArray headers = payload.value("headers").toArray();
    m.from = decodeEncodedWords(header(headers, "From"));
    m.to = decodeEncodedWords(header(headers, "To"));
    m.cc = decodeEncodedWords(header(headers, "Cc"));
    m.replyTo = decodeEncodedWords(header(headers, "Reply-To"));
    m.subject = decodeEncodedWords(header(headers, "Subject"));
    m.messageId = header(headers, "Message-ID");
    m.references = header(headers, "References");
    if (payload.contains("body") || payload.contains("parts"))
        walk(payload, m);
    return m;
}

QByteArray build(const OutgoingMail &mail)
{
    QByteArray out;
    auto addHeader = [&out](const char *name, const QByteArray &value) {
        if (!value.isEmpty())
            out += QByteArray(name) + ": " + value + "\r\n";
    };
    addHeader("From", encodeAddressList(mail.from));
    addHeader("To", encodeAddressList(mail.to));
    addHeader("Cc", encodeAddressList(mail.cc));
    addHeader("Bcc", encodeAddressList(mail.bcc));
    addHeader("Subject", encodeWord(mail.subject));
    addHeader("Date", QDateTime::currentDateTime().toString(Qt::RFC2822Date).toUtf8());
    addHeader("In-Reply-To", mail.inReplyTo.toUtf8());
    addHeader("References", mail.references.toUtf8());
    addHeader("MIME-Version", "1.0");
    addHeader("X-Mailer", "G-Desk " GDESK_VERSION);

    QString body = mail.body;
    body.replace("\r\n", "\n").replace("\n", "\r\n");
    const QByteArray textPart = "Content-Type: text/plain; charset=UTF-8\r\n"
                                "Content-Transfer-Encoding: base64\r\n\r\n"
                                + wrappedBase64(body.toUtf8());
    if (mail.attachments.isEmpty())
        return out + textPart;

    const QByteArray boundary = "=_gdesk_" + QByteArray::number(QRandomGenerator::global()->generate64(), 36);
    out += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
    out += "--" + boundary + "\r\n" + textPart;
    for (const Attachment &a : mail.attachments) {
        const QByteArray type = a.mimeType.isEmpty() ? QByteArray("application/octet-stream") : a.mimeType.toUtf8();
        QByteArray name = a.filename.toUtf8();
        name.replace("\"", "");
        const QByteArray encodedName = isPlainAscii(a.filename) ? name : encodeWord(a.filename);
        out += "--" + boundary + "\r\n";
        out += "Content-Type: " + type + "; name=\"" + encodedName + "\"\r\n";
        out += "Content-Disposition: attachment; filename=\"" + encodedName + "\"";
        if (!isPlainAscii(a.filename))
            out += ";\r\n filename*=UTF-8''" + QUrl::toPercentEncoding(a.filename);
        out += "\r\nContent-Transfer-Encoding: base64\r\n\r\n" + wrappedBase64(a.data);
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

QStringList splitAddresses(const QString &list)
{
    QStringList result;
    QString current;
    bool quoted = false;
    int angle = 0;
    for (QChar c : list) {
        if (c == '"')
            quoted = !quoted;
        else if (c == '<')
            ++angle;
        else if (c == '>')
            angle = qMax(0, angle - 1);
        if ((c == ',' || c == ';') && !quoted && angle == 0) {
            if (!current.trimmed().isEmpty())
                result << current.trimmed();
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.trimmed().isEmpty())
        result << current.trimmed();
    return result;
}

QString displayName(const QString &address)
{
    const int lt = address.indexOf('<');
    QString name = lt > 0 ? address.left(lt).trimmed() : QString();
    if (name.size() >= 2 && name.startsWith('"') && name.endsWith('"'))
        name = name.mid(1, name.size() - 2);
    return name.isEmpty() ? emailOnly(address) : name;
}

QString emailOnly(const QString &address)
{
    const int lt = address.indexOf('<');
    const int gt = address.indexOf('>', lt);
    return (lt >= 0 && gt > lt) ? address.mid(lt + 1, gt - lt - 1).trimmed() : address.trimmed();
}

QString textToHtml(const QString &text)
{
    static const QRegularExpression url(R"((https?://[^\s<>"]+))");
    QString html = text.toHtmlEscaped();
    html.replace(url, R"(<a href="\1">\1</a>)");
    return "<div style=\"white-space:pre-wrap;font-family:sans-serif;font-size:14px\">" + html + "</div>";
}

QString htmlToText(const QString &html)
{
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText();
}

QString shortDate(const QDateTime &date)
{
    const QDateTime local = date.toLocalTime();
    const QDate today = QDate::currentDate();
    QLocale locale;
    if (local.date() == today)
        return locale.toString(local.time(), "HH:mm");
    if (local.date().year() == today.year())
        return locale.toString(local.date(), "d MMM");
    return locale.toString(local.date(), "dd/MM/yyyy");
}

QString longDate(const QDateTime &date)
{
    // Le format « long » de Qt ajoute le nom du fuseau (« heure d'été d'Europe centrale ») et les secondes :
    // on compose la date longue et l'heure courte séparément.
    if (!date.isValid() || date.toMSecsSinceEpoch() == 0)
        return {};
    const QDateTime local = date.toLocalTime();
    QLocale locale;
    return QString("%1 à %2").arg(locale.toString(local.date(), QLocale::LongFormat),
                                  locale.toString(local.time(), "HH:mm"));
}

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace Mime
