#pragma once
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrlQuery>
#include <functional>

class GoogleAuth;
class QNetworkAccessManager;

// Accès à l'API REST Gmail v1 (https://developers.google.com/gmail/api/reference/rest)
class GmailApi : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonObject &result, const QString &error)>;
    using DataCallback = std::function<void(const QByteArray &data, const QString &error)>;

    GmailApi(GoogleAuth *auth, QNetworkAccessManager *nam, QObject *parent = nullptr);

    void getProfile(Callback cb);
    void listLabels(Callback cb);
    void getLabel(const QString &id, Callback cb);
    void listMessages(const QStringList &labelIds, const QString &query, const QString &pageToken,
                      int maxResults, Callback cb);
    void getMessage(const QString &id, bool full, Callback cb);
    // En-têtes choisis d'un message (format « metadata »)
    void getMessageHeaders(const QString &id, const QStringList &headers, Callback cb);
    // URL de messages.list (exposée pour les tests d'encodage)
    static void setBaseUrlForTesting(const QString &url); // faux serveur Gmail des tests
    static QUrl messagesUrl(const QStringList &labelIds, const QString &query, const QString &pageToken, int maxResults);
    void modifyMessages(const QStringList &ids, const QStringList &add, const QStringList &remove, Callback cb);
    void trashMessage(const QString &id, Callback cb);
    void untrashMessage(const QString &id, Callback cb);
    void getAttachment(const QString &messageId, const QString &attachmentId, DataCallback cb);
    void sendMessage(const QByteArray &rfc822, const QString &threadId, Callback cb);
    // Crée le brouillon (draftId vide) ou le met à jour ; le résultat contient son « id »
    void saveDraft(const QString &draftId, const QByteArray &rfc822, const QString &threadId, Callback cb);
    void sendDraft(const QString &draftId, const QByteArray &rfc822, const QString &threadId, Callback cb);

private:
    void call(const QByteArray &verb, const QString &path, const QUrlQuery &query,
              const QByteArray &body, Callback cb, int attempt = 0);

    GoogleAuth *m_auth;
    QNetworkAccessManager *m_nam;
};
