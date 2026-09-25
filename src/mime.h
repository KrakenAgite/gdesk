#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct Attachment
{
    QString filename;
    QString mimeType;
    QString attachmentId; // à télécharger via l'API si data est vide
    QString contentId;    // pour les images intégrées (cid:)
    qint64 size = 0;
    QByteArray data;
    bool isInline = false;
};

struct MailMessage
{
    QString id, threadId, snippet;
    QStringList labelIds;
    QString from, to, cc, replyTo, subject;
    QString messageId, references;
    QDateTime date;
    QString html, text;
    QList<Attachment> attachments;

    bool isUnread() const { return labelIds.contains("UNREAD"); }
    bool isStarred() const { return labelIds.contains("STARRED"); }
};

struct OutgoingMail
{
    QString from, to, cc, bcc, subject, body;
    QString inReplyTo, references;
    QList<Attachment> attachments;
};

namespace Mime {
// Message renvoyé par l'API Gmail (format « full » ou « metadata »)
MailMessage parseMessage(const QJsonObject &json);
// Message RFC 2822 prêt à être envoyé
QByteArray build(const OutgoingMail &mail);

QStringList splitAddresses(const QString &list);
QString displayName(const QString &address); // « Jean Dupont <j@x.fr> » → « Jean Dupont »
QString emailOnly(const QString &address);   // « Jean Dupont <j@x.fr> » → « j@x.fr »
QString textToHtml(const QString &text);
QString htmlToText(const QString &html);
QString shortDate(const QDateTime &date);
QString humanSize(qint64 bytes);
}
