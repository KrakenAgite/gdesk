#include "gmailapi.h"
#include "googleauth.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

static const QString BaseUrl = "https://gmail.googleapis.com/gmail/v1/users/me/";

GmailApi::GmailApi(GoogleAuth *auth, QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent), m_auth(auth), m_nam(nam)
{
}

void GmailApi::call(const QByteArray &verb, const QString &path, const QUrlQuery &query,
                    const QByteArray &body, Callback cb, int attempt)
{
    m_auth->accessToken([=, this](const QString &token, const QString &authError) {
        if (!authError.isEmpty()) {
            cb({}, authError);
            return;
        }
        QUrl url(BaseUrl + path);
        url.setQuery(query);
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        if (verb != "GET")
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply *reply = m_nam->sendCustomRequest(req, verb, body);
        connect(reply, &QNetworkReply::finished, this, [=, this] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = reply->readAll();

            if (status == 401 && attempt == 0) { // jeton expiré entre-temps
                m_auth->invalidateAccessToken();
                call(verb, path, query, body, cb, attempt + 1);
                return;
            }
            if ((status == 429 || status >= 500) && attempt < 3) { // limite de débit : on patiente
                QTimer::singleShot(1000 * (attempt + 1), this,
                                   [=, this] { call(verb, path, query, body, cb, attempt + 1); });
                return;
            }
            const QJsonObject obj = QJsonDocument::fromJson(data).object();
            if (status == 0) {
                cb({}, reply->errorString());
            } else if (status >= 400) {
                const QString msg = obj.value("error").toObject().value("message").toString();
                cb(obj, msg.isEmpty() ? QString("Erreur HTTP %1").arg(status) : msg);
            } else {
                cb(obj, {});
            }
        });
    });
}

void GmailApi::getProfile(Callback cb)
{
    call("GET", "profile", {}, {}, cb);
}

void GmailApi::listLabels(Callback cb)
{
    call("GET", "labels", {}, {}, cb);
}

void GmailApi::getLabel(const QString &id, Callback cb)
{
    call("GET", "labels/" + QUrl::toPercentEncoding(id), {}, {}, cb);
}

static QUrlQuery messagesQuery(const QStringList &labelIds, const QString &query, const QString &pageToken,
                               int maxResults)
{
    QUrlQuery q;
    for (const QString &l : labelIds)
        q.addQueryItem("labelIds", l);
    if (!query.isEmpty())
        q.addQueryItem("q", QString::fromUtf8(QUrl::toPercentEncoding(query)));
    if (!pageToken.isEmpty())
        q.addQueryItem("pageToken", pageToken);
    q.addQueryItem("maxResults", QString::number(maxResults));
    if (labelIds.contains("SPAM") || labelIds.contains("TRASH"))
        q.addQueryItem("includeSpamTrash", "true");
    return q;
}

QUrl GmailApi::messagesUrl(const QStringList &labelIds, const QString &query, const QString &pageToken, int maxResults)
{
    QUrl url(BaseUrl + "messages");
    url.setQuery(messagesQuery(labelIds, query, pageToken, maxResults));
    return url;
}

void GmailApi::listMessages(const QStringList &labelIds, const QString &query, const QString &pageToken,
                            int maxResults, Callback cb)
{
    call("GET", "messages", messagesQuery(labelIds, query, pageToken, maxResults), {}, cb);
}

void GmailApi::getMessage(const QString &id, bool full, Callback cb)
{
    QUrlQuery q;
    q.addQueryItem("format", full ? "full" : "metadata");
    if (!full)
        for (const char *h : {"From", "To", "Subject", "Date"})
            q.addQueryItem("metadataHeaders", h);
    call("GET", "messages/" + id, q, {}, cb);
}

void GmailApi::modifyMessages(const QStringList &ids, const QStringList &add, const QStringList &remove, Callback cb)
{
    QJsonObject body{{"ids", QJsonArray::fromStringList(ids)},
                     {"addLabelIds", QJsonArray::fromStringList(add)},
                     {"removeLabelIds", QJsonArray::fromStringList(remove)}};
    call("POST", "messages/batchModify", {}, QJsonDocument(body).toJson(QJsonDocument::Compact), cb);
}

void GmailApi::trashMessage(const QString &id, Callback cb)
{
    call("POST", "messages/" + id + "/trash", {}, "", cb);
}

void GmailApi::untrashMessage(const QString &id, Callback cb)
{
    call("POST", "messages/" + id + "/untrash", {}, "", cb);
}

void GmailApi::getAttachment(const QString &messageId, const QString &attachmentId, DataCallback cb)
{
    call("GET", "messages/" + messageId + "/attachments/" + attachmentId, {}, {},
         [cb](const QJsonObject &obj, const QString &err) {
             if (!err.isEmpty()) {
                 cb({}, err);
                 return;
             }
             cb(QByteArray::fromBase64(obj.value("data").toString().toLatin1(), QByteArray::Base64UrlEncoding), {});
         });
}

static QJsonObject rawMessage(const QByteArray &rfc822, const QString &threadId)
{
    QJsonObject msg{{"raw", QString::fromLatin1(rfc822.toBase64(QByteArray::Base64UrlEncoding))}};
    if (!threadId.isEmpty())
        msg.insert("threadId", threadId);
    return msg;
}

void GmailApi::sendMessage(const QByteArray &rfc822, const QString &threadId, Callback cb)
{
    call("POST", "messages/send", {}, QJsonDocument(rawMessage(rfc822, threadId)).toJson(QJsonDocument::Compact), cb);
}

void GmailApi::saveDraft(const QString &draftId, const QByteArray &rfc822, const QString &threadId, Callback cb)
{
    QJsonObject body{{"message", rawMessage(rfc822, threadId)}};
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (draftId.isEmpty())
        call("POST", "drafts", {}, json, cb);
    else
        call("PUT", "drafts/" + draftId, {}, json, cb);
}

void GmailApi::sendDraft(const QString &draftId, const QByteArray &rfc822, const QString &threadId, Callback cb)
{
    QJsonObject body{{"id", draftId}, {"message", rawMessage(rfc822, threadId)}};
    call("POST", "drafts/send", {}, QJsonDocument(body).toJson(QJsonDocument::Compact), cb);
}
