#include "googleauth.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <qt6keychain/keychain.h>
#include <utility>

static const char *AuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
static const char *TokenEndpoint = "https://oauth2.googleapis.com/token";
static const char *RevokeEndpoint = "https://oauth2.googleapis.com/revoke";
static const char *KeychainService = "gdesk";
static const char *KeychainKey = "google-refresh-token";

static QString randomString(int length)
{
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString s;
    s.reserve(length);
    for (int i = 0; i < length; ++i)
        s += QChar(chars[QRandomGenerator::system()->bounded(int(sizeof(chars) - 1))]);
    return s;
}

GoogleAuth::GoogleAuth(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent), m_nam(nam)
{
}

void GoogleAuth::setClient(const QString &clientId, const QString &clientSecret)
{
    m_clientId = clientId.trimmed();
    m_clientSecret = clientSecret.trimmed();
}

// --- stockage du jeton de rafraîchissement ------------------------------------
QString GoogleAuth::fallbackPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/refresh-token";
}

void GoogleAuth::loadStoredToken(std::function<void(bool)> done)
{
    auto *job = new QKeychain::ReadPasswordJob(KeychainService, this);
    job->setKey(KeychainKey);
    connect(job, &QKeychain::Job::finished, this, [this, job, done](QKeychain::Job *) {
        if (job->error() == QKeychain::NoError)
            m_refreshToken = job->textData();
        if (m_refreshToken.isEmpty()) {
            // Portefeuille indisponible : fichier lisible uniquement par l'utilisateur
            QFile f(fallbackPath());
            if (f.open(QIODevice::ReadOnly))
                m_refreshToken = QString::fromUtf8(f.readAll()).trimmed();
        }
        job->deleteLater();
        done(!m_refreshToken.isEmpty());
    });
    job->start();
}

void GoogleAuth::storeRefreshToken(const QString &token)
{
    if (token.isEmpty()) {
        auto *job = new QKeychain::DeletePasswordJob(KeychainService, this);
        job->setKey(KeychainKey);
        connect(job, &QKeychain::Job::finished, job, &QObject::deleteLater);
        job->start();
        QFile::remove(fallbackPath());
        return;
    }
    auto *job = new QKeychain::WritePasswordJob(KeychainService, this);
    job->setKey(KeychainKey);
    job->setTextData(token);
    connect(job, &QKeychain::Job::finished, this, [job, token](QKeychain::Job *) {
        if (job->error() == QKeychain::NoError) {
            QFile::remove(fallbackPath());
        } else {
            qWarning("Portefeuille KDE indisponible (%s) : jeton stocké dans un fichier privé",
                     qPrintable(job->errorString()));
            QDir().mkpath(QFileInfo(fallbackPath()).absolutePath());
            QFile f(fallbackPath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                f.write(token.toUtf8());
            }
        }
        job->deleteLater();
    });
    job->start();
}

// --- connexion ------------------------------------------------------------------
void GoogleAuth::login()
{
    cancelLogin();
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        emit loginFailed("Impossible d'ouvrir le port local de connexion : " + m_server->errorString());
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &GoogleAuth::onNewConnection);

    m_redirectUri = QString("http://127.0.0.1:%1").arg(m_server->serverPort());
    m_verifier = randomString(64);
    m_state = randomString(24);
    const QByteArray challenge = QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QUrl url(AuthEndpoint);
    QUrlQuery q;
    q.addQueryItem("client_id", m_clientId);
    q.addQueryItem("redirect_uri", m_redirectUri);
    q.addQueryItem("response_type", "code");
    q.addQueryItem("scope", Scope);
    q.addQueryItem("code_challenge", QString::fromLatin1(challenge));
    q.addQueryItem("code_challenge_method", "S256");
    q.addQueryItem("state", m_state);
    q.addQueryItem("access_type", "offline");
    q.addQueryItem("prompt", "select_account consent"); // choix du compte + jeton de rafraîchissement
    url.setQuery(q);
    QDesktopServices::openUrl(url);
}

void GoogleAuth::cancelLogin()
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

void GoogleAuth::onNewConnection()
{
    while (QTcpSocket *sock = m_server ? m_server->nextPendingConnection() : nullptr) {
        // Le serveur est fermé dès le code reçu : la connexion doit lui survivre
        // le temps d'envoyer la page de confirmation au navigateur.
        sock->setParent(this);
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            QByteArray buf = sock->property("buf").toByteArray() + sock->readAll();
            sock->setProperty("buf", buf);
            if (!buf.contains("\r\n\r\n"))
                return;
            const QList<QByteArray> requestLine = buf.left(buf.indexOf("\r\n")).split(' ');
            const QUrlQuery q(QUrl("http://127.0.0.1" + QString::fromLatin1(requestLine.value(1))));
            const bool isRedirect = q.hasQueryItem("code") || q.hasQueryItem("error");

            const QByteArray page = isRedirect
                ? QByteArray("<!doctype html><meta charset=utf-8><title>G-Desk</title>"
                             "<body style='font-family:sans-serif;text-align:center;margin-top:15%'>"
                             "<h2>Connexion terminée</h2><p>Vous pouvez fermer cet onglet et revenir à G-Desk.</p>")
                : QByteArray("Not found");
            sock->write((isRedirect ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n")
                        + QByteArray("Content-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: ")
                        + QByteArray::number(page.size()) + "\r\n\r\n" + page);
            sock->disconnectFromHost();
            if (!isRedirect)
                return;

            cancelLogin();
            if (q.hasQueryItem("error")) {
                const QString err = q.queryItemValue("error");
                emit loginFailed(err == "access_denied" ? "Connexion refusée." : "Erreur Google : " + err);
            } else if (q.queryItemValue("state", QUrl::FullyDecoded) != m_state) {
                emit loginFailed("Réponse de connexion invalide (state).");
            } else {
                exchangeCode(q.queryItemValue("code", QUrl::FullyDecoded));
            }
        });
    }
}

void GoogleAuth::postToken(const QList<QPair<QString, QString>> &form,
                           std::function<void(const QJsonObject &, const QString &)> cb)
{
    QUrlQuery body;
    for (const auto &[k, v] : form)
        body.addQueryItem(k, QString::fromLatin1(QUrl::toPercentEncoding(v)));
    QNetworkRequest req{QUrl(TokenEndpoint)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QNetworkReply *reply = m_nam->post(req, body.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [reply, cb] {
        reply->deleteLater();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.contains("access_token"))
            cb(obj, {});
        else if (obj.contains("error"))
            cb(obj, obj.value("error").toString());
        else
            cb(obj, reply->errorString());
    });
}

void GoogleAuth::exchangeCode(const QString &code)
{
    postToken({{"code", code}, {"client_id", m_clientId}, {"client_secret", m_clientSecret},
               {"redirect_uri", m_redirectUri}, {"grant_type", "authorization_code"}, {"code_verifier", m_verifier}},
              [this](const QJsonObject &obj, const QString &err) {
                  if (!err.isEmpty()) {
                      const QString desc = obj.value("error_description").toString();
                      emit loginFailed(desc.isEmpty() ? err : desc);
                      return;
                  }
                  m_accessToken = obj.value("access_token").toString();
                  m_expiry = QDateTime::currentDateTimeUtc().addSecs(obj.value("expires_in").toInt(3600));
                  m_refreshToken = obj.value("refresh_token").toString();
                  storeRefreshToken(m_refreshToken);
                  emit loggedIn();
              });
}

void GoogleAuth::accessToken(TokenCallback cb)
{
    if (!m_accessToken.isEmpty() && QDateTime::currentDateTimeUtc() < m_expiry.addSecs(-60)) {
        cb(m_accessToken, {});
        return;
    }
    if (m_refreshToken.isEmpty()) {
        cb({}, "Non connecté");
        return;
    }
    m_waiting.append(cb);
    if (m_refreshing)
        return;
    m_refreshing = true;
    postToken({{"client_id", m_clientId}, {"client_secret", m_clientSecret},
               {"refresh_token", m_refreshToken}, {"grant_type", "refresh_token"}},
              [this](const QJsonObject &obj, const QString &err) {
                  m_refreshing = false;
                  const QList<TokenCallback> waiting = std::exchange(m_waiting, {});
                  if (err.isEmpty()) {
                      m_accessToken = obj.value("access_token").toString();
                      m_expiry = QDateTime::currentDateTimeUtc().addSecs(obj.value("expires_in").toInt(3600));
                      for (const auto &w : waiting)
                          w(m_accessToken, {});
                      return;
                  }
                  if (err == "invalid_grant" || err == "invalid_client" || err == "unauthorized_client") {
                      // Jeton révoqué ou expiré (7 jours tant que l'appli Google Cloud est « en test »)
                      m_refreshToken.clear();
                      storeRefreshToken({});
                      emit sessionExpired();
                  }
                  for (const auto &w : waiting)
                      w({}, "Session Google : " + err);
              });
}

void GoogleAuth::logout()
{
    if (!m_refreshToken.isEmpty()) {
        QNetworkRequest req{QUrl(RevokeEndpoint)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        QNetworkReply *reply = m_nam->post(req, "token=" + QUrl::toPercentEncoding(m_refreshToken));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    m_accessToken.clear();
    m_refreshToken.clear();
    storeRefreshToken({});
}
