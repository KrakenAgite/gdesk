#pragma once
#include <QDateTime>
#include <QObject>
#include <functional>

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;

// Connexion OAuth 2.0 « application de bureau » recommandée par Google :
// le navigateur système affiche la page de connexion Google, puis redirige vers
// un petit serveur local (127.0.0.1) ; le code obtenu est échangé contre des jetons
// avec PKCE. Le jeton de rafraîchissement est rangé dans KWallet (via QtKeychain).
class GoogleAuth : public QObject
{
    Q_OBJECT
public:
    using TokenCallback = std::function<void(const QString &accessToken, const QString &error)>;

    // Gmail (lecture, envoi, libellés) et Google Drive (parcourir, joindre, enregistrer des pièces jointes)
    static constexpr const char *Scope =
        "https://www.googleapis.com/auth/gmail.modify https://www.googleapis.com/auth/drive";

    GoogleAuth(QNetworkAccessManager *nam, QObject *parent = nullptr);

    void setClient(const QString &clientId, const QString &clientSecret);
    bool hasClient() const { return !m_clientId.isEmpty(); }
    bool isLoggedIn() const { return !m_refreshToken.isEmpty(); }

    void loadStoredToken(std::function<void(bool found)> done);
    void login();
    void cancelLogin();
    void logout();

    // Fournit un jeton d'accès valide (le rafraîchit si besoin)
    void accessToken(TokenCallback cb);
    void invalidateAccessToken() { m_accessToken.clear(); }
    static void setAccessTokenForTesting(const QString &token); // tests : jeton fixe, sans connexion

signals:
    void loggedIn();
    void loginFailed(const QString &error);
    void sessionExpired();

private:
    void onNewConnection();
    void exchangeCode(const QString &code);
    void postToken(const QList<QPair<QString, QString>> &form, std::function<void(const QJsonObject &, const QString &)> cb);
    void storeRefreshToken(const QString &token);
    static QString fallbackPath();

    QNetworkAccessManager *m_nam;
    QTcpServer *m_server = nullptr;
    QString m_clientId, m_clientSecret;
    QString m_redirectUri, m_verifier, m_state;
    QString m_accessToken, m_refreshToken;
    QDateTime m_expiry;
    QList<TokenCallback> m_waiting;
    bool m_refreshing = false;
};
