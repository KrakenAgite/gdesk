#include "driveapi.h"
#include "googleauth.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>

static QString &apiBase()
{
    static QString url = "https://www.googleapis.com/drive/v3/";
    return url;
}

static QString &uploadBase()
{
    static QString url = "https://www.googleapis.com/upload/drive/v3/";
    return url;
}

void DriveApi::setBaseUrlsForTesting(const QString &api, const QString &upload)
{
    apiBase() = api;
    uploadBase() = upload;
}

// --- DriveFile ----------------------------------------------------------------
bool DriveFile::isFolder() const
{
    return contentMimeType() == DriveApi::FolderMime;
}

bool DriveFile::isGoogleFile() const
{
    return contentMimeType().startsWith("application/vnd.google-apps.") && !isFolder();
}

DriveFile DriveFile::fromJson(const QJsonObject &j)
{
    DriveFile f;
    f.id = j.value("id").toString();
    f.name = j.value("name").toString();
    f.mimeType = j.value("mimeType").toString();
    f.parentId = j.value("parents").toArray().first().toString();
    f.webViewLink = j.value("webViewLink").toString();
    f.owner = j.value("owners").toArray().first().toObject().value("displayName").toString();
    if (j.contains("size")) // entier 64 bits transmis sous forme de texte
        f.size = j.value("size").toString().toLongLong();
    f.modified = QDateTime::fromString(j.value("modifiedTime").toString(), Qt::ISODateWithMs);
    f.starred = j.value("starred").toBool();
    f.trashed = j.value("trashed").toBool();
    f.ownedByMe = j.value("ownedByMe").toBool();
    const QJsonObject caps = j.value("capabilities").toObject();
    f.canRename = caps.value("canRename").toBool();
    f.canTrash = caps.value("canTrash").toBool();
    f.canAddChildren = caps.value("canAddChildren").toBool();
    const QJsonObject shortcut = j.value("shortcutDetails").toObject();
    f.targetId = shortcut.value("targetId").toString();
    f.targetMimeType = shortcut.value("targetMimeType").toString();
    return f;
}

// --- Erreurs ------------------------------------------------------------------
static bool hasReason(const QJsonObject &error, const char *reason)
{
    for (const QJsonValue &v : error.value("errors").toArray())
        if (v.toObject().value("reason").toString() == QLatin1String(reason))
            return true;
    for (const QJsonValue &v : error.value("details").toArray())
        if (v.toObject().value("reason").toString() == QLatin1String(reason))
            return true;
    return false;
}

bool DriveApi::needsConsent(const QJsonObject &obj)
{
    const QJsonObject e = obj.value("error").toObject();
    if (e.value("code").toInt() != 403)
        return false;
    return hasReason(e, "insufficientPermissions") || hasReason(e, "ACCESS_TOKEN_SCOPE_INSUFFICIENT")
           || e.value("message").toString().contains("insufficient authentication scopes", Qt::CaseInsensitive);
}

bool DriveApi::apiDisabled(const QJsonObject &obj)
{
    const QJsonObject e = obj.value("error").toObject();
    return hasReason(e, "accessNotConfigured") || hasReason(e, "SERVICE_DISABLED");
}

static void finish(int status, const QByteArray &data, const QString &networkError, const DriveApi::Callback &cb)
{
    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (!networkError.isEmpty()) {
        cb(obj, networkError);
    } else if (status >= 400) {
        QString msg = obj.value("error").toObject().value("message").toString();
        if (DriveApi::needsConsent(obj))
            msg = "G-Desk n'a pas encore l'autorisation d'accéder à votre Google Drive.";
        else if (DriveApi::apiDisabled(obj))
            msg = "L'API Google Drive n'est pas activée dans votre projet Google Cloud.";
        cb(obj, msg.isEmpty() ? QString("Erreur HTTP %1").arg(status) : msg);
    } else {
        cb(obj, {});
    }
}

// --- Requêtes -----------------------------------------------------------------
DriveApi::DriveApi(GoogleAuth *auth, QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent), m_auth(auth), m_nam(nam)
{
}

void DriveApi::send(Starter start, std::shared_ptr<QIODevice> sink, RawCallback cb, Progress progress, int attempt)
{
    m_auth->accessToken([=, this](const QString &token, const QString &authError) {
        if (!authError.isEmpty()) {
            cb(0, {}, authError, nullptr);
            return;
        }
        QNetworkReply *reply = start("Bearer " + token.toUtf8());
        auto body = std::make_shared<QByteArray>();
        // Seul le contenu d'une réponse réussie va dans le fichier ; une erreur reste en mémoire (JSON)
        auto drain = [reply, sink, body] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (sink && status >= 200 && status < 300)
                sink->write(reply->readAll());
            else
                body->append(reply->readAll());
        };
        connect(reply, &QNetworkReply::readyRead, this, drain);
        if (progress) {
            connect(reply, &QNetworkReply::uploadProgress, this, [progress](qint64 done, qint64 total) {
                if (total > 0)
                    progress(done, total);
            });
            connect(reply, &QNetworkReply::downloadProgress, this, [progress, reply](qint64 done, qint64 total) {
                if (reply->operation() == QNetworkAccessManager::GetOperation)
                    progress(done, total);
            });
        }
        connect(reply, &QNetworkReply::finished, this, [=, this] {
            reply->deleteLater();
            drain();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 401 && attempt == 0) { // jeton expiré entre-temps
                m_auth->invalidateAccessToken();
                send(start, sink, cb, progress, attempt + 1);
                return;
            }
            if ((status == 429 || status >= 500) && attempt < 3) { // surcharge : on patiente
                QTimer::singleShot(1000 * (attempt + 1), this,
                                   [=, this] { send(start, sink, cb, progress, attempt + 1); });
                return;
            }
            const bool failed = status == 0 || (status < 400 && reply->error() != QNetworkReply::NoError);
            cb(status, *body, failed ? reply->errorString() : QString(), reply);
        });
    });
}

void DriveApi::json(const QByteArray &verb, const QUrl &url, const QByteArray &body, Callback cb)
{
    send([=, this](const QByteArray &authorization) {
            QNetworkRequest req(url);
            req.setRawHeader("Authorization", authorization);
            if (!body.isEmpty())
                req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
            return m_nam->sendCustomRequest(req, verb, body);
        },
        nullptr, [cb](int status, const QByteArray &data, const QString &err, QNetworkReply *) {
            finish(status, data, err, cb);
        },
        {});
}

static QUrl apiUrl(const QString &path, QUrlQuery q = {})
{
    QUrl url(apiBase() + path);
    q.addQueryItem("supportsAllDrives", "true");
    url.setQuery(q);
    return url;
}

static QUrlQuery fileFields()
{
    QUrlQuery q;
    q.addQueryItem("fields", DriveApi::FileFields);
    return q;
}

void DriveApi::about(Callback cb)
{
    QUrlQuery q;
    q.addQueryItem("fields", "storageQuota(limit,usage)");
    QUrl url(apiBase() + "about");
    url.setQuery(q);
    json("GET", url, {}, cb);
}

void DriveApi::listFiles(const QString &query, const QString &orderBy, const QString &pageToken, int pageSize,
                         Callback cb)
{
    QUrlQuery q;
    q.addQueryItem("q", QString::fromUtf8(QUrl::toPercentEncoding(query)));
    if (!orderBy.isEmpty())
        q.addQueryItem("orderBy", QString::fromUtf8(QUrl::toPercentEncoding(orderBy)));
    if (!pageToken.isEmpty())
        q.addQueryItem("pageToken", QString::fromUtf8(QUrl::toPercentEncoding(pageToken)));
    q.addQueryItem("pageSize", QString::number(pageSize));
    q.addQueryItem("includeItemsFromAllDrives", "true");
    q.addQueryItem("fields", QString("nextPageToken,files(%1)").arg(FileFields));
    json("GET", apiUrl("files", q), {}, cb);
}

void DriveApi::createFolder(const QString &name, const QString &parentId, Callback cb)
{
    QJsonObject body{{"name", name}, {"mimeType", FolderMime}};
    if (!parentId.isEmpty())
        body.insert("parents", QJsonArray{parentId});
    json("POST", apiUrl("files", fileFields()), QJsonDocument(body).toJson(QJsonDocument::Compact),
         [this, cb, parentId](const QJsonObject &obj, const QString &err) {
             if (err.isEmpty())
                 emit filesChanged(parentId);
             cb(obj, err);
         });
}

void DriveApi::updateFile(const QString &id, const QJsonObject &changes, Callback cb)
{
    json("PATCH", apiUrl("files/" + QUrl::toPercentEncoding(id), fileFields()),
         QJsonDocument(changes).toJson(QJsonDocument::Compact), cb);
}

// --- Téléchargement -------------------------------------------------------------
QUrl DriveApi::contentUrl(const DriveFile &file) const
{
    const QString id = QString::fromLatin1(QUrl::toPercentEncoding(file.contentId()));
    QUrlQuery q;
    if (file.isGoogleFile()) {
        q.addQueryItem("mimeType", QString::fromUtf8(QUrl::toPercentEncoding(exportFormat(file.contentMimeType()).first)));
        QUrl url(apiBase() + "files/" + id + "/export");
        url.setQuery(q);
        return url;
    }
    q.addQueryItem("alt", "media");
    return apiUrl("files/" + id, q);
}

void DriveApi::download(const DriveFile &file, DataCallback cb, Progress progress)
{
    if (file.isFolder() || (file.isGoogleFile() && exportFormat(file.contentMimeType()).first.isEmpty())) {
        cb({}, QString("« %1 » ne peut pas être téléchargé.").arg(file.name));
        return;
    }
    const QUrl url = contentUrl(file);
    send([this, url](const QByteArray &authorization) {
            QNetworkRequest req(url);
            req.setRawHeader("Authorization", authorization);
            return m_nam->get(req);
        },
        nullptr, [cb](int status, const QByteArray &data, const QString &err, QNetworkReply *) {
            if (err.isEmpty() && status < 400) {
                cb(data, {});
                return;
            }
            finish(status, data, err, [cb](const QJsonObject &, const QString &e) { cb({}, e); });
        },
        progress);
}

void DriveApi::downloadToFile(const DriveFile &file, const QString &path, Callback cb, Progress progress)
{
    if (file.isFolder() || (file.isGoogleFile() && exportFormat(file.contentMimeType()).first.isEmpty())) {
        cb({}, QString("« %1 » ne peut pas être téléchargé.").arg(file.name));
        return;
    }
    auto out = std::make_shared<QFile>(path);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cb({}, out->errorString());
        return;
    }
    const QUrl url = contentUrl(file);
    send([this, url, out](const QByteArray &authorization) {
            out->resize(0); // nouvelle tentative : on repart de zéro
            out->seek(0);
            QNetworkRequest req(url);
            req.setRawHeader("Authorization", authorization);
            return m_nam->get(req);
        },
        out, [cb, out](int status, const QByteArray &data, const QString &err, QNetworkReply *) {
            out->close();
            if (!err.isEmpty() || status >= 400)
                out->remove();
            finish(status, data, err, cb);
        },
        progress);
}

// --- Envoi (« resumable upload ») -------------------------------------------------
// 1) POST des métadonnées → adresse de session ; 2) PUT du contenu, lu directement depuis le disque.
void DriveApi::upload(const QString &name, const QString &mimeType, std::shared_ptr<QIODevice> data, qint64 size,
                      const QString &parentId, Callback cb, Progress progress)
{
    QUrlQuery q = fileFields();
    q.addQueryItem("uploadType", "resumable");
    q.addQueryItem("supportsAllDrives", "true");
    QUrl url(uploadBase() + "files");
    url.setQuery(q);
    QJsonObject meta{{"name", name}};
    if (!parentId.isEmpty())
        meta.insert("parents", QJsonArray{parentId});
    const QByteArray metaJson = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    const QByteArray type = (mimeType.isEmpty() ? QString("application/octet-stream") : mimeType).toUtf8();

    send([=, this](const QByteArray &authorization) {
            QNetworkRequest req(url);
            req.setRawHeader("Authorization", authorization);
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
            req.setRawHeader("X-Upload-Content-Type", type);
            req.setRawHeader("X-Upload-Content-Length", QByteArray::number(size));
            return m_nam->post(req, metaJson);
        },
        nullptr,
        [=, this](int status, const QByteArray &resp, const QString &err, QNetworkReply *reply) {
            if (!err.isEmpty() || status >= 400) {
                finish(status, resp, err, cb);
                return;
            }
            const QUrl session(QString::fromUtf8(reply->rawHeader("Location")));
            if (!session.isValid() || session.isRelative()) {
                cb({}, "Réponse inattendue de Google Drive pendant l'envoi.");
                return;
            }
            send([=, this](const QByteArray &authorization) {
                    data->reset();
                    QNetworkRequest req(session);
                    req.setRawHeader("Authorization", authorization);
                    req.setHeader(QNetworkRequest::ContentTypeHeader, type);
                    req.setHeader(QNetworkRequest::ContentLengthHeader, size);
                    return m_nam->put(req, data.get());
                },
                nullptr,
                [=, this](int status2, const QByteArray &resp2, const QString &err2, QNetworkReply *) {
                    finish(status2, resp2, err2, [=, this](const QJsonObject &obj, const QString &e) {
                        if (e.isEmpty())
                            emit filesChanged(parentId);
                        cb(obj, e);
                    });
                },
                progress);
        },
        {});
}

void DriveApi::uploadData(const QString &name, const QString &mimeType, const QByteArray &data,
                          const QString &parentId, Callback cb, Progress progress)
{
    auto buffer = std::make_shared<QBuffer>();
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    upload(name, mimeType, buffer, data.size(), parentId, cb, progress);
}

void DriveApi::uploadFile(const QString &localPath, const QString &parentId, Callback cb, Progress progress)
{
    auto file = std::make_shared<QFile>(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        cb({}, file->errorString());
        return;
    }
    upload(QFileInfo(localPath).fileName(), QMimeDatabase().mimeTypeForFile(localPath).name(), file, file->size(),
           parentId, cb, progress);
}

// --- Requêtes de liste --------------------------------------------------------------
static QString quoted(QString s)
{
    s.replace('\\', "\\\\").replace('\'', "\\'");
    return "'" + s + "'";
}

QString DriveApi::folderQuery(const QString &folderId, bool foldersOnly)
{
    QString q = quoted(folderId) + " in parents and trashed = false";
    if (foldersOnly)
        q += QString(" and mimeType = '%1'").arg(FolderMime);
    return q;
}

QString DriveApi::searchQuery(const QString &text, bool foldersOnly)
{
    QString q = foldersOnly ? QString("name contains %1").arg(quoted(text))
                            : QString("(name contains %1 or fullText contains %1)").arg(quoted(text));
    q += " and trashed = false";
    if (foldersOnly)
        q += QString(" and mimeType = '%1'").arg(FolderMime);
    return q;
}

QString DriveApi::placeQuery(Place place)
{
    switch (place) {
    case MyDrive:
        return folderQuery("root");
    case Recent:
        return QString("trashed = false and mimeType != '%1'").arg(FolderMime);
    case Starred:
        return "starred = true and trashed = false";
    case Shared:
        return "sharedWithMe = true and trashed = false";
    case Trash:
        return "trashed = true";
    }
    return {};
}

QString DriveApi::placeOrder(Place place)
{
    switch (place) {
    case Recent:
        return "recency desc";
    case Shared:
        return "sharedWithMeTime desc";
    case Trash:
        return "modifiedTime desc";
    default:
        return "folder,name_natural";
    }
}

QPair<QString, QString> DriveApi::exportFormat(const QString &googleMime)
{
    static const QHash<QString, QPair<QString, QString>> formats = {
        {"application/vnd.google-apps.document",
         {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx"}},
        {"application/vnd.google-apps.spreadsheet",
         {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx"}},
        {"application/vnd.google-apps.presentation",
         {"application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx"}},
        {"application/vnd.google-apps.drawing", {"application/pdf", "pdf"}},
        {"application/vnd.google-apps.script", {"application/vnd.google-apps.script+json", "json"}},
    };
    return formats.value(googleMime);
}

QString DriveApi::downloadName(const DriveFile &file)
{
    if (!file.isGoogleFile())
        return file.name;
    const QString ext = exportFormat(file.contentMimeType()).second;
    if (ext.isEmpty() || file.name.endsWith("." + ext, Qt::CaseInsensitive))
        return file.name;
    return file.name + "." + ext;
}

QString DriveApi::iconName(const DriveFile &file)
{
    if (file.isFolder())
        return file.ownedByMe || file.mimeType == FolderMime ? QString("folder") : QString("folder-publicshare");
    static const QHash<QString, QString> google = {
        {"application/vnd.google-apps.document", "x-office-document"},
        {"application/vnd.google-apps.spreadsheet", "x-office-spreadsheet"},
        {"application/vnd.google-apps.presentation", "x-office-presentation"},
        {"application/vnd.google-apps.drawing", "x-office-drawing"},
        {"application/vnd.google-apps.form", "x-office-document"},
        {"application/vnd.google-apps.script", "text-x-script"},
    };
    const QString mime = file.contentMimeType();
    if (mime.startsWith("application/vnd.google-apps."))
        return google.value(mime, "text-x-generic");
    const QMimeType type = QMimeDatabase().mimeTypeForName(mime);
    return type.isValid() ? type.iconName() : QString("text-x-generic");
}
