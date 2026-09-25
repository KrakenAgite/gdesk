#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <functional>
#include <memory>

class GoogleAuth;
class QIODevice;
class QNetworkAccessManager;
class QNetworkReply;

// Fichier ou dossier Google Drive (réponse de files.list / files.get)
struct DriveFile
{
    QString id, name, mimeType, parentId, webViewLink, owner;
    QString targetId, targetMimeType; // raccourci : fichier pointé
    qint64 size = -1;                 // -1 : sans taille (dossier, fichier Google)
    QDateTime modified;
    bool starred = false, trashed = false, ownedByMe = false;
    bool canRename = false, canTrash = false, canAddChildren = false;

    bool isValid() const { return !id.isEmpty(); }
    bool isFolder() const;       // dossier, ou raccourci vers un dossier
    bool isGoogleFile() const;   // Docs, Sheets, Slides… : à exporter pour être téléchargé
    QString contentId() const { return targetId.isEmpty() ? id : targetId; }
    QString contentMimeType() const { return targetMimeType.isEmpty() ? mimeType : targetMimeType; }
    static DriveFile fromJson(const QJsonObject &json);
};
Q_DECLARE_METATYPE(DriveFile)

// Accès à l'API REST Google Drive v3 (https://developers.google.com/workspace/drive/api/reference/rest/v3)
class DriveApi : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonObject &result, const QString &error)>;
    using DataCallback = std::function<void(const QByteArray &data, const QString &error)>;
    using Progress = std::function<void(qint64 done, qint64 total)>;

    static constexpr const char *FolderMime = "application/vnd.google-apps.folder";
    static constexpr const char *FileFields =
        "id,name,mimeType,size,modifiedTime,starred,trashed,parents,webViewLink,ownedByMe,"
        "owners(displayName),shortcutDetails(targetId,targetMimeType),"
        "capabilities(canRename,canTrash,canAddChildren)";

    // Lieux de la barre latérale de Drive
    enum Place { MyDrive, Recent, Starred, Shared, Trash };

    DriveApi(GoogleAuth *auth, QNetworkAccessManager *nam, QObject *parent = nullptr);

    void about(Callback cb); // espace de stockage utilisé
    // files.list ; q : syntaxe de recherche de Drive
    void listFiles(const QString &q, const QString &orderBy, const QString &pageToken, int pageSize, Callback cb);
    void createFolder(const QString &name, const QString &parentId, Callback cb);
    void updateFile(const QString &id, const QJsonObject &changes, Callback cb); // renommer, suivre, corbeille

    // Téléchargement (les fichiers Google sont exportés, voir exportFormat)
    void download(const DriveFile &file, DataCallback cb, Progress progress = {});
    void downloadToFile(const DriveFile &file, const QString &path, Callback cb, Progress progress = {});
    // Envoi (en une ou plusieurs fois selon la taille) ; le résultat décrit le fichier créé
    void uploadData(const QString &name, const QString &mimeType, const QByteArray &data, const QString &parentId,
                    Callback cb, Progress progress = {});
    void uploadFile(const QString &localPath, const QString &parentId, Callback cb, Progress progress = {});

    // Demande à l'utilisateur d'autoriser G-Desk à accéder à Drive (voir MainWindow)
    void requestAuthorization() { emit authorizationRequested(); }

    // Requêtes de liste pour chaque lieu, un dossier ou une recherche
    static QString placeQuery(Place place);
    static QString placeOrder(Place place);
    static QString folderQuery(const QString &folderId, bool foldersOnly = false);
    static QString searchQuery(const QString &text, bool foldersOnly = false);
    // Fichier Google → format d'export (type MIME, extension) ; vide si non exportable
    static QPair<QString, QString> exportFormat(const QString &googleMime);
    static QString downloadName(const DriveFile &file); // nom avec l'extension du format exporté
    static QString iconName(const DriveFile &file);     // icône du thème
    // Erreurs à expliquer : jeton sans l'autorisation Drive, API non activée dans le projet Google Cloud
    static bool needsConsent(const QJsonObject &error);
    static bool apiDisabled(const QJsonObject &error);
    static constexpr const char *EnableApiUrl = "https://console.cloud.google.com/apis/library/drive.googleapis.com";

    static void setBaseUrlsForTesting(const QString &api, const QString &upload); // faux serveur des tests

signals:
    void authorizationRequested();
    void filesChanged(const QString &folderId); // fichier ou dossier ajouté (pour rafraîchir la vue Drive)

private:
    // networkError : panne réseau ou de connexion (sinon, voir status et le corps JSON de l'erreur)
    using RawCallback = std::function<void(int status, const QByteArray &body, const QString &networkError,
                                           QNetworkReply *reply)>;
    using Starter = std::function<QNetworkReply *(const QByteArray &authorization)>;
    // Envoie une requête avec le jeton d'accès ; relance si le jeton a expiré ou en cas de surcharge.
    // sink : reçoit le corps d'une réponse réussie au fil de l'eau (sinon il est rendu dans body).
    void send(Starter start, std::shared_ptr<QIODevice> sink, RawCallback cb, Progress progress, int attempt = 0);
    void json(const QByteArray &verb, const QUrl &url, const QByteArray &body, Callback cb);
    void upload(const QString &name, const QString &mimeType, std::shared_ptr<QIODevice> data, qint64 size,
                const QString &parentId, Callback cb, Progress progress);
    QUrl contentUrl(const DriveFile &file) const;

    GoogleAuth *m_auth;
    QNetworkAccessManager *m_nam;
};
