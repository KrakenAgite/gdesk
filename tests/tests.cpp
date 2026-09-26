// Tests sans compte Google : MIME, connexion OAuth (jusqu'au serveur de Google) et visionneuse.
#include "composer.h"
#include "driveapi.h"
#include "drivebrowser.h"
#include "driveview.h"
#include "unsubscribe.h"
#include "gmailapi.h"
#include "googleauth.h"
#include "maillistdelegate.h"
#include "sidebar.h"
#include "mainwindow.h"
#include "messageview.h"
#include "mime.h"
#include "settingsdialog.h"
#include "theme.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QJsonArray>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QToolBar>
#include <QHeaderView>
#include <QLabel>
#include <QGroupBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QStringEncoder>
#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMessageBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QRegularExpression>
#include <QSet>
#include <QStyleOptionViewItem>
#include <QPlatformSurfaceEvent>
#include <QTest>
#include <QWindow>
#include <QTextDocumentFragment>
#include <QUrlQuery>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineView>

class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QUrl last;
public slots:
    void handle(const QUrl &url) { last = url; }
};

static QString b64url(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding));
}

static QJsonArray headers(std::initializer_list<std::pair<const char *, QString>> list)
{
    QJsonArray a;
    for (const auto &[n, v] : list)
        a.append(QJsonObject{{"name", n}, {"value", v}});
    return a;
}


// Faux serveur Gmail (HTTP local) pour tester les opérations en masse sans vrai compte
class FakeGmail : public QObject
{
    Q_OBJECT
public:
    struct Batch {
        int count;
        QStringList add, remove;
    };
    QTcpServer server;
    QMap<QString, QSet<QString>> messages;
    QStringList order;
    QList<Batch> batches;
    QStringList trashed;
    QStringList metadataRequests; // identifiants dont les détails ont été demandés
    QStringList listFields;       // paramètre « fields » des listes
    QMap<QString, QJsonArray> customHeaders; // en-têtes propres à un message (newsletters)
    QList<QByteArray> sent;                  // messages envoyés (RFC 2822)
    QStringList unsubscribePosts;            // « chemin corps » des désabonnements en un clic

    FakeGmail()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] { handle(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString base() const { return QString("http://127.0.0.1:%1/gmail/v1/users/me/").arg(server.serverPort()); }
    void add(const QString &id, const QSet<QString> &labels, bool newest = false)
    {
        messages.insert(id, labels);
        if (newest)
            order.prepend(id);
        else
            order << id;
    }
    int count(const QStringList &labels) const
    {
        int n = 0;
        for (const QSet<QString> &l : messages)
            n += std::all_of(labels.begin(), labels.end(), [&](const QString &x) { return l.contains(x); });
        return n;
    }

private:
    QJsonObject route(const QByteArray &method, const QString &path, const QUrlQuery &q, const QByteArray &body)
    {
        if (method == "GET" && path == "messages") {
            const QStringList labels = q.allQueryItemValues("labelIds");
            const QString query = q.queryItemValue("q", QUrl::FullyDecoded);
            const bool spamTrash = q.queryItemValue("includeSpamTrash") == "true";
            QStringList hits;
            for (const QString &id : std::as_const(order)) {
                const QSet<QString> &l = messages[id];
                if (!std::all_of(labels.begin(), labels.end(), [&](const QString &x) { return l.contains(x); }))
                    continue;
                if (query == "is:read" && l.contains("UNREAD"))
                    continue;
                if (!spamTrash && (l.contains("TRASH") || l.contains("SPAM")))
                    continue;
                hits << id;
            }
            listFields << q.queryItemValue("fields", QUrl::FullyDecoded);
            const int offset = q.queryItemValue("pageToken").toInt();
            const int max = q.queryItemValue("maxResults").toInt();
            QJsonArray page;
            for (const QString &id : hits.mid(offset, max))
                page.append(QJsonObject{{"id", id}, {"threadId", id}});
            QJsonObject r{{"messages", page}, {"resultSizeEstimate", int(hits.size())}};
            if (offset + max < hits.size())
                r.insert("nextPageToken", QString::number(offset + max));
            return r;
        }
        if (method == "POST" && path == "messages/batchModify") {
            const QJsonObject b = QJsonDocument::fromJson(body).object();
            Batch batch{int(b.value("ids").toArray().size()), {}, {}};
            for (const QJsonValue &v : b.value("addLabelIds").toArray())
                batch.add << v.toString();
            for (const QJsonValue &v : b.value("removeLabelIds").toArray())
                batch.remove << v.toString();
            for (const QJsonValue &v : b.value("ids").toArray()) {
                QSet<QString> &l = messages[v.toString()];
                for (const QString &x : batch.remove)
                    l.remove(x);
                for (const QString &x : batch.add)
                    l.insert(x);
            }
            batches << batch;
            return {};
        }
        if (method == "POST" && path.endsWith("/trash")) {
            const QString id = path.section('/', 1, 1);
            messages[id].insert("TRASH");
            trashed << id;
            return {{"id", id}};
        }
        if (method == "POST" && path == "messages/send") {
            sent << QByteArray::fromBase64(QJsonDocument::fromJson(body).object().value("raw").toString().toLatin1(),
                                           QByteArray::Base64UrlEncoding);
            return {{"id", "envoye"}};
        }
        if (method == "GET" && path.startsWith("messages/") && customHeaders.contains(path.section('/', 1, 1))) {
            const QString id = path.section('/', 1, 1);
            return {{"id", id}, {"internalDate", QString::number(1758800000000LL + order.indexOf(id) * -60000)},
                    {"payload", QJsonObject{{"headers", customHeaders.value(id)}}}};
        }
        if (method == "GET" && path.startsWith("messages/")) {
            const QString id = path.section('/', 1, 1);
            metadataRequests << id;
            QJsonArray labels;
            for (const QString &l : messages.value(id))
                labels.append(l);
            return {{"id", id}, {"threadId", id}, {"labelIds", labels}, {"snippet", "Aperçu"},
                    {"internalDate", "1758800000000"},
                    {"payload", QJsonObject{{"headers", QJsonArray{
                        QJsonObject{{"name", "From"}, {"value", "Test <t@x.fr>"}},
                        QJsonObject{{"name", "Subject"}, {"value", "Sujet " + id}}}}}}};
        }
        if (method == "GET" && path.startsWith("labels/")) {
            const QString id = path.section('/', 1, 1);
            return {{"id", id}, {"messagesUnread", count({id, "UNREAD"})}, {"messagesTotal", count({id})}};
        }
        if (path == "labels")
            return {{"labels", QJsonArray{}}};
        return {{"emailAddress", "test@exemple.fr"}};
    }

    void handle(QTcpSocket *s)
    {
        QByteArray buf = s->property("buf").toByteArray() + s->readAll();
        const int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            s->setProperty("buf", buf);
            return;
        }
        const QByteArray head = buf.left(headerEnd);
        int length = 0;
        for (const QByteArray &line : head.split('\n'))
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toInt();
        if (buf.size() < headerEnd + 4 + length) {
            s->setProperty("buf", buf);
            return;
        }
        s->setProperty("buf", QByteArray());
        const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
        const QUrl url("http://x" + QString::fromLatin1(requestLine.value(1)));
        if (url.path().startsWith("/unsub/")) { // lien de désabonnement en un clic d'une newsletter
            unsubscribePosts << requestLine.value(0) + " " + url.path() + " " + buf.mid(headerEnd + 4, length);
            s->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const QByteArray out = QJsonDocument(route(requestLine.value(0), url.path().section("/users/me/", 1),
                                                   QUrlQuery(url), buf.mid(headerEnd + 4, length)))
                                   .toJson(QJsonDocument::Compact);
        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                 + QByteArray::number(out.size()) + "\r\n\r\n" + out);
        s->disconnectFromHost();
    }
};

// Faux serveur Google Drive : liste, création, modification, envoi « resumable », téléchargement, export
// Réponses et fichiers du faux serveur écrits en initialisation partielle ({statut, corps})
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

class FakeDrive : public QObject
{
    Q_OBJECT
public:
    struct File {
        QString id, name, mime, parent;
        QByteArray data;
        bool starred = false, trashed = false;
    };
    QTcpServer server;
    QMap<QString, File> files;
    QStringList queries;      // paramètre « q » des listes
    bool denyScope = false;   // simule un jeton sans l'autorisation Drive
    int sessions = 0;

    FakeDrive()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] { handle(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString root() const { return QString("http://127.0.0.1:%1/").arg(server.serverPort()); }
    QString api() const { return root() + "drive/v3/"; }
    QString upload() const { return root() + "upload/drive/v3/"; }
    QString add(const QString &name, const QString &mime, const QString &parent, const QByteArray &data = {})
    {
        const QString id = QString("f%1").arg(files.size() + 1);
        files.insert(id, {id, name, mime, parent, data});
        return id;
    }
    QStringList namesIn(const QString &parent) const
    {
        QStringList names;
        for (const File &f : files)
            if (f.parent == parent && !f.trashed)
                names << f.name;
        return names;
    }

private:
    struct Reply {
        int status = 200;
        QByteArray body;
        QByteArray type = "application/json";
        QByteArray headers;
    };
    QJsonObject json(const File &f) const
    {
        QJsonObject o{{"id", f.id}, {"name", f.name}, {"mimeType", f.mime}, {"parents", QJsonArray{f.parent}},
                      {"modifiedTime", "2026-09-20T10:00:00.000Z"}, {"starred", f.starred}, {"trashed", f.trashed},
                      {"ownedByMe", true}, {"owners", QJsonArray{QJsonObject{{"displayName", "Moi"}}}},
                      {"webViewLink", "https://drive.google.com/file/d/" + f.id},
                      {"capabilities", QJsonObject{{"canRename", true}, {"canTrash", true}, {"canAddChildren", true}}}};
        if (!f.mime.startsWith("application/vnd.google-apps."))
            o.insert("size", QString::number(f.data.size()));
        return o;
    }
    static Reply ok(const QJsonObject &o) { return {200, QJsonDocument(o).toJson(QJsonDocument::Compact)}; }

    Reply route(const QByteArray &method, const QString &path, const QUrlQuery &q, const QByteArray &body,
                const QByteArray &head)
    {
        if (denyScope)
            return {403, R"({"error":{"code":403,"message":"Request had insufficient authentication scopes.",)"
                         R"("status":"PERMISSION_DENIED","details":[{"reason":"ACCESS_TOKEN_SCOPE_INSUFFICIENT"}]}})"};
        if (method == "GET" && path == "drive/v3/about")
            return ok({{"storageQuota", QJsonObject{{"usage", "3221225472"}, {"limit", "16106127360"}}}});
        if (method == "GET" && path == "drive/v3/files") {
            const QString query = q.queryItemValue("q", QUrl::FullyDecoded);
            queries << query;
            const QRegularExpressionMatch parent = QRegularExpression("'([^']*)' in parents").match(query);
            const bool foldersOnly = query.contains("mimeType = 'application/vnd.google-apps.folder'");
            QJsonArray list;
            for (const File &f : std::as_const(files)) {
                if (parent.hasMatch() && f.parent != parent.captured(1))
                    continue;
                if (query.contains("starred = true") && !f.starred)
                    continue;
                if (f.trashed != query.contains("trashed = true"))
                    continue;
                if (foldersOnly && f.mime != DriveApi::FolderMime)
                    continue;
                list.append(json(f));
            }
            return ok({{"files", list}});
        }
        if (method == "POST" && path == "drive/v3/files") {
            const QJsonObject b = QJsonDocument::fromJson(body).object();
            const QString id = add(b.value("name").toString(), b.value("mimeType").toString(),
                                   b.value("parents").toArray().first().toString());
            return ok(json(files[id]));
        }
        if (method == "PATCH" && path.startsWith("drive/v3/files/")) {
            File &f = files[path.section('/', 3, 3)];
            const QJsonObject b = QJsonDocument::fromJson(body).object();
            if (b.contains("name"))
                f.name = b.value("name").toString();
            if (b.contains("starred"))
                f.starred = b.value("starred").toBool();
            if (b.contains("trashed"))
                f.trashed = b.value("trashed").toBool();
            return ok(json(f));
        }
        if (method == "GET" && path.endsWith("/export")) {
            const File &f = files[path.section('/', 3, 3)];
            return {200, "EXPORT " + f.name.toUtf8() + " " + q.queryItemValue("mimeType", QUrl::FullyDecoded).toUtf8(),
                    "application/octet-stream"};
        }
        if (method == "GET" && path.startsWith("drive/v3/files/") && q.queryItemValue("alt") == "media")
            return {200, files[path.section('/', 3, 3)].data, "application/octet-stream"};
        if (method == "POST" && path == "upload/drive/v3/files" && q.queryItemValue("uploadType") == "resumable") {
            const QJsonObject meta = QJsonDocument::fromJson(body).object();
            const QString session = QString::number(++sessions);
            pending.insert(session, {{}, meta.value("name").toString(),
                                     QString::fromUtf8(headerValue(head, "x-upload-content-type")),
                                     meta.value("parents").toArray().first().toString()});
            return {200, {}, "application/json", "Location: " + (root() + "upload/session/" + session).toUtf8() + "\r\n"};
        }
        if (method == "PUT" && path.startsWith("upload/session/")) {
            const File meta = pending.take(path.section('/', 2, 2));
            const QString id = add(meta.name, meta.mime, meta.parent == "root" ? "root" : meta.parent, body);
            return ok(json(files[id]));
        }
        return {404, R"({"error":{"code":404,"message":"introuvable"}})"};
    }
    static QByteArray headerValue(const QByteArray &head, const QByteArray &name)
    {
        for (const QByteArray &line : head.split('\n'))
            if (line.toLower().startsWith(name + ":"))
                return line.mid(name.size() + 1).trimmed();
        return {};
    }

    void handle(QTcpSocket *s)
    {
        QByteArray buf = s->property("buf").toByteArray() + s->readAll();
        const int headerEnd = buf.indexOf("\r\n\r\n");
        const int length = headerEnd < 0 ? 0 : headerValue(buf.left(headerEnd), "content-length").toInt();
        if (headerEnd < 0 || buf.size() < headerEnd + 4 + length) {
            s->setProperty("buf", buf);
            return;
        }
        s->setProperty("buf", QByteArray());
        const QByteArray head = buf.left(headerEnd);
        const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
        const QUrl url("http://x" + QString::fromLatin1(requestLine.value(1)));
        const Reply r = route(requestLine.value(0), url.path().mid(1), QUrlQuery(url), buf.mid(headerEnd + 4, length), head);
        s->write("HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Type: " + r.type
                 + "\r\nConnection: close\r\n" + r.headers + "Content-Length: " + QByteArray::number(r.body.size())
                 + "\r\n\r\n" + r.body);
        s->disconnectFromHost();
    }
    QMap<QString, File> pending; // sessions d'envoi ouvertes
};

class Tests : public QObject
{
    Q_OBJECT
private slots:
    void splitAddresses()
    {
        const QStringList l = Mime::splitAddresses(R"("Dupont, Jean" <j@x.fr>; paul@y.com, Élodie <e@z.fr>)");
        QCOMPARE(l.size(), 3);
        QCOMPARE(Mime::displayName(l[0]), QString("Dupont, Jean"));
        QCOMPARE(Mime::emailOnly(l[0]), QString("j@x.fr"));
        QCOMPARE(Mime::emailOnly(l[1]), QString("paul@y.com"));
        QCOMPARE(Mime::displayName(l[2]), QString("Élodie"));
    }

    // Jeux de caractères des corps de message : chaque cas est (charset, octets bruts, texte attendu)
    void charsets_data()
    {
        QTest::addColumn<QString>("charset");
        QTest::addColumn<QByteArray>("raw");
        QTest::addColumn<QString>("expected");
        QTest::newRow("utf-8 emojis") << "UTF-8" << QString("Salut 😀 👨‍👩‍👧 🇫🇷 ✓ ½ — « ok »").toUtf8()
                                     << QString("Salut 😀 👨‍👩‍👧 🇫🇷 ✓ ½ — « ok »");
        QTest::newRow("windows-1252") << "windows-1252" << QByteArray("C\x9cur \x80 5 \x93ok\x94") << QString("Cœur € 5 “ok”");
        QTest::newRow("iso-8859-15") << "iso-8859-15" << QByteArray("Prix : 5 \xa4, \xbc\xbd") << QString("Prix : 5 €, Œœ");
        QTest::newRow("iso-8859-1") << "\"ISO-8859-1\"" << QByteArray("caf\xe9 d\xe9j\xe0") << QString("café déjà");
        QTest::newRow("koi8-r") << "koi8-r" << QByteArray("\xf0\xd2\xc9\xd7\xc5\xd4") << QString("Привет");
        QTest::newRow("shift_jis") << "Shift_JIS" << QByteArray("\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd") << QString("こんにちは");
        QTest::newRow("iso-2022-jp") << "ISO-2022-JP" << QByteArray("\x1b$B$3$s$K$A$O\x1b(B") << QString("こんにちは");
        QTest::newRow("gb2312") << "gb2312" << QByteArray("\xc4\xe3\xba\xc3") << QString("你好");
    }

    void charsets()
    {
        QFETCH(QString, charset);
        QFETCH(QByteArray, raw);
        QFETCH(QString, expected);
        QJsonObject json{{"id", "m"}, {"payload", QJsonObject{
            {"mimeType", "text/plain"},
            {"headers", headers({{"Content-Type", "text/plain; charset=" + charset}})},
            {"body", QJsonObject{{"data", b64url(raw)}}}}}};
        QCOMPARE(Mime::parseMessage(json).text, expected);
    }

    // En-têtes encodés (RFC 2047) que Gmail renvoie parfois tels quels
    void encodedHeaders()
    {
        QJsonObject json{{"id", "m"}, {"payload", QJsonObject{
            {"mimeType", "text/plain"},
            {"headers", headers({{"Subject", "=?UTF-8?B?8J+YgCBCb25qb3Vy?= =?ISO-8859-1?Q?caf=E9_cr=E8me?="},
                                 {"From", "=?utf-8?q?=C3=89lodie_=F0=9F=8C=B8?= <e@x.fr>"}})},
            {"body", QJsonObject{{"data", ""}}}}}};
        const MailMessage m = Mime::parseMessage(json);
        QCOMPARE(m.subject, QString("😀 Bonjourcafé crème"));
        QCOMPARE(m.from, QString("Élodie 🌸 <e@x.fr>"));
        QCOMPARE(Mime::displayName(m.from), QString("Élodie 🌸"));

        // Un caractère coupé entre deux mots encodés (autorisé en pratique) doit être recollé
        QJsonObject split{{"id", "m"}, {"payload", QJsonObject{
            {"mimeType", "text/plain"},
            {"headers", headers({{"Subject", "=?UTF-8?B?8J+Y?= =?UTF-8?B?gA==?= fin"}})},
            {"body", QJsonObject{{"data", ""}}}}}};
        QCOMPARE(Mime::parseMessage(split).subject, QString("😀 fin"));
        // Texte ordinaire contenant « =? » : inchangé
        QJsonObject plain{{"id", "m"}, {"payload", QJsonObject{
            {"mimeType", "text/plain"},
            {"headers", headers({{"Subject", "Résultat =? 2+2 ?= 4 😀"}})},
            {"body", QJsonObject{{"data", ""}}}}}};
        QCOMPARE(Mime::parseMessage(plain).subject, QString("Résultat =? 2+2 ?= 4 😀"));
    }

    // Aller-retour complet : ce que G-Desk envoie, relu par un analyseur indépendant (Python)
    void sendEmojis()
    {
        OutgoingMail mail;
        mail.from = "Gabriel 🚀 <moi@gmail.com>";
        mail.to = "Zoë 🌸 <z@x.fr>";
        mail.subject = "👨‍👩‍👧 Famille 🇫🇷 : « Noël » 🎄 — ½ € ✓ 日本語 " + QString(40, QChar(0x00E9));
        mail.body = "Bises 😘\nÇa marche ✓ — ½ € — Привет — こんにちは";
        Attachment a;
        a.filename = "photo 📷 été.jpg";
        a.mimeType = "image/jpeg";
        a.data = "x";
        mail.attachments << a;
        const QByteArray raw = Mime::build(mail);
        for (const QByteArray &line : raw.split('\n')) {
            QVERIFY2(line.size() <= 998, "ligne trop longue");
            for (char c : line)
                QVERIFY2(uchar(c) < 128, "octet non ASCII dans le message brut"); // tout doit être encodé
        }
        QTemporaryDir dir;
        QFile f(dir.filePath("m.eml"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(raw);
        f.close();
        QProcess py;
        py.start("python3", {"-c", R"(
import sys, email, json
from email import policy
m = email.message_from_bytes(open(sys.argv[1],'rb').read(), policy=policy.default)
body = next(p for p in m.walk() if p.get_content_type()=='text/plain').get_content()
att = next(p for p in m.walk() if p.get_filename())
print(json.dumps({'subject': m['subject'], 'from': str(m['from']), 'to': str(m['to']),
                  'body': body.replace('\r\n','\n'), 'file': att.get_filename()}))
)", f.fileName()});
        QVERIFY(py.waitForFinished(10000));
        const QJsonObject r = QJsonDocument::fromJson(py.readAllStandardOutput()).object();
        QVERIFY2(!r.isEmpty(), py.readAllStandardError().constData());
        QCOMPARE(r.value("subject").toString(), mail.subject);
        QCOMPARE(r.value("from").toString(), mail.from);
        QCOMPARE(r.value("to").toString(), mail.to);
        QCOMPARE(r.value("body").toString(), mail.body);
        QCOMPARE(r.value("file").toString(), a.filename);
    }

    // Recherche : les caractères spéciaux doivent arriver intacts dans l'URL envoyée à Gmail
    void searchEncoding()
    {
        const QString query = "café & co +1 😀 from:\"Zoë\" #tag 100%";
        const QUrl url = GmailApi::messagesUrl({"INBOX"}, query, {}, 50);
        const QString sent = QUrlQuery(url).queryItemValue("q", QUrl::FullyDecoded);
        QCOMPARE(sent, query);
        QVERIFY(!url.toEncoded().contains(' '));
    }

    void snippetEntities()
    {
        const QStringList samples = {
            "L&#39;été &amp; l&#39;hiver", "&quot;Promo&quot; -50% &lt;aujourd&#39;hui&gt;",
            "Caf&#233; &#x1F600; &#128512;", "Rien à décoder 😀", "5&nbsp;€ &amp;amp; &unknown; & fin",
            "&#39;&#39;&#39;", "b &#99999999; c"};
        for (const QString &s : samples) {
            QJsonObject json{{"id", "m"}, {"snippet", s}};
            const QString expected = QTextDocumentFragment::fromHtml(s).toPlainText().replace(QChar(0xA0), ' ');
            QCOMPARE(Mime::parseMessage(json).snippet, expected);
        }
        // Différence voulue avec QTextDocument : pas de caractère nul inséré
        QCOMPARE(Mime::parseMessage(QJsonObject{{"id", "m"}, {"snippet", "a &#0; b"}}).snippet, QString("a &#0; b"));
    }

    void dates()
    {
        QLocale::setDefault(QLocale(QLocale::French, QLocale::France));
        const QDateTime summer(QDate(2026, 7, 14), QTime(14, 32, 10));
        const QDateTime winter(QDate(2026, 1, 5), QTime(8, 5, 59));
        QCOMPARE(Mime::longDate(summer), QString("mardi 14 juillet 2026 à 14:32"));
        QCOMPARE(Mime::longDate(winter), QString("lundi 5 janvier 2026 à 08:05"));
        for (const QDateTime &d : {summer, winter})
            for (const char *forbidden : {"heure", "UTC", "GMT", "CET", "CEST", ":10", ":59"})
                QVERIFY2(!Mime::longDate(d).contains(forbidden), qPrintable(Mime::longDate(d)));
        QLocale::setDefault(QLocale::system());
    }

    void parseMessage()
    {
        QStringEncoder latin1(QStringEncoder::Latin1);
        const QByteArray plain = latin1.encode(QString("Déjà vu"));
        QJsonObject alt{{"mimeType", "multipart/alternative"},
                        {"parts", QJsonArray{
                             QJsonObject{{"mimeType", "text/plain"},
                                         {"headers", headers({{"Content-Type", "text/plain; charset=ISO-8859-1"}})},
                                         {"body", QJsonObject{{"data", b64url(plain)}}}},
                             QJsonObject{{"mimeType", "text/html"},
                                         {"headers", headers({{"Content-Type", "text/html; charset=utf-8"}})},
                                         {"body", QJsonObject{{"data", b64url(QString("<b>Déjà</b>").toUtf8())}}}}}}};
        QJsonObject pdf{{"mimeType", "application/pdf"}, {"filename", "facture.pdf"},
                        {"headers", headers({{"Content-Disposition", "attachment; filename=facture.pdf"}})},
                        {"body", QJsonObject{{"attachmentId", "ATT1"}, {"size", 12345}}}};
        QJsonObject img{{"mimeType", "image/png"}, {"filename", "logo.png"},
                        {"headers", headers({{"Content-ID", "<logo@x>"}, {"Content-Disposition", "inline"}})},
                        {"body", QJsonObject{{"attachmentId", "ATT2"}, {"size", 99}}}};
        QJsonObject json{{"id", "m1"}, {"threadId", "t1"}, {"labelIds", QJsonArray{"INBOX", "UNREAD"}},
                         {"snippet", "L&#39;été"}, {"internalDate", "1758800000000"},
                         {"payload", QJsonObject{{"mimeType", "multipart/mixed"},
                                                 {"headers", headers({{"From", "Élodie Martin <elodie@example.fr>"},
                                                                      {"Subject", "Café ☕"},
                                                                      {"Message-ID", "<abc@x>"}})},
                                                 {"parts", QJsonArray{alt, pdf, img}}}}};
        const MailMessage m = Mime::parseMessage(json);
        QCOMPARE(m.subject, QString("Café ☕"));
        QCOMPARE(m.snippet, QString("L'été"));
        QCOMPARE(m.text, QString("Déjà vu"));
        QCOMPARE(m.html, QString("<b>Déjà</b>"));
        QVERIFY(m.isUnread());
        QCOMPARE(m.attachments.size(), 2);
        QCOMPARE(m.attachments[0].filename, QString("facture.pdf"));
        QCOMPARE(m.attachments[0].attachmentId, QString("ATT1"));
        QVERIFY(!m.attachments[0].isInline);
        QCOMPARE(m.attachments[1].contentId, QString("logo@x"));
        QVERIFY(m.attachments[1].isInline);
        QCOMPARE(m.date.toMSecsSinceEpoch(), 1758800000000LL);
    }

    void buildMessage()
    {
        OutgoingMail mail;
        mail.from = "moi@gmail.com";
        mail.to = "Élodie Martin <elodie@example.fr>, \"Dupont, Jean\" <j@x.fr>";
        mail.subject = "Réunion de rentrée : ordre du jour très détaillé avec des accents éàü ☕";
        mail.body = "Bonjour,\nVoici le document.\n\nÀ bientôt";
        mail.inReplyTo = "<abc@x>";
        mail.references = "<abc@x>";
        Attachment a;
        a.filename = "présentation.pdf";
        a.mimeType = "application/pdf";
        a.data = QByteArray(5000, 'x');
        mail.attachments << a;
        const QByteArray raw = Mime::build(mail);
        const QString out = qEnvironmentVariable("GDESK_TEST_OUT");
        if (!out.isEmpty()) {
            QFile f(out + "/build.eml");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(raw);
        }
        for (const QByteArray &line : raw.split('\n'))
            QVERIFY2(line.size() <= 998, "ligne trop longue");
        QVERIFY(raw.contains("In-Reply-To: <abc@x>"));
    }

    void oauthFlowReachesGoogle()
    {
        UrlCatcher catcher;
        QDesktopServices::setUrlHandler("https", &catcher, "handle");
        QNetworkAccessManager nam;
        GoogleAuth auth(&nam);
        auth.setClient("123456-test.apps.googleusercontent.com", "GOCSPX-faux");
        QSignalSpy failed(&auth, &GoogleAuth::loginFailed);
        QSignalSpy ok(&auth, &GoogleAuth::loggedIn);

        auth.login();
        QCOMPARE(catcher.last.host(), QString("accounts.google.com"));
        const QUrlQuery q(catcher.last);
        QCOMPARE(q.queryItemValue("code_challenge_method"), QString("S256"));
        QCOMPARE(q.queryItemValue("scope", QUrl::FullyDecoded), QString(GoogleAuth::Scope));
        QCOMPARE(q.queryItemValue("access_type"), QString("offline"));
        const QString redirect = q.queryItemValue("redirect_uri", QUrl::FullyDecoded);
        QVERIFY(redirect.startsWith("http://127.0.0.1:"));
        const QString state = q.queryItemValue("state");

        // Le navigateur revient sur le serveur local avec un code (faux)
        QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(redirect + "/?state=" + state + "&code=4%2Ffaux-code")));
        QSignalSpy done(reply, &QNetworkReply::finished);
        QVERIFY(done.wait(5000));
        QVERIFY(QString::fromUtf8(reply->readAll()).contains("Connexion terminée"));

        // G-Desk échange le code auprès de Google, qui refuse ce faux client
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 20000);
        QCOMPARE(ok.count(), 0);
        qInfo() << "Réponse de Google :" << failed.first().first().toString();
        QDesktopServices::unsetUrlHandler("https");
    }

    void oauthRejectsWrongState()
    {
        UrlCatcher catcher;
        QDesktopServices::setUrlHandler("https", &catcher, "handle");
        QNetworkAccessManager nam;
        GoogleAuth auth(&nam);
        auth.setClient("123456-test.apps.googleusercontent.com", "x");
        QSignalSpy failed(&auth, &GoogleAuth::loginFailed);
        auth.login();
        const QString redirect = QUrlQuery(catcher.last).queryItemValue("redirect_uri", QUrl::FullyDecoded);
        nam.get(QNetworkRequest(QUrl(redirect + "/?state=pirate&code=abc")));
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 5000);
        QVERIFY(failed.first().first().toString().contains("state"));
        QDesktopServices::unsetUrlHandler("https");
    }

    void messageViewSandbox()
    {
        QImage image(10, 10, QImage::Format_RGB32);
        image.fill(Qt::red);
        QBuffer png;
        png.open(QIODevice::WriteOnly);
        image.save(&png, "PNG");

        MailMessage m;
        m.id = "t1";
        m.subject = "Test é";
        m.from = "Élodie <e@x.fr>";
        m.html = "<p id=t>Bonjour éàü 😀👨‍👩‍👧🇫🇷 € œ</p><img id=cid src=\"cid:img1@x\">"
                 "<img id=remote src=\"https://www.google.com/images/branding/googlelogo/1x/googlelogo_color_92x30dp.png\">"
                 "<script>document.title='js-a-tourne'</script>";
        Attachment a;
        a.contentId = "img1@x";
        a.mimeType = "image/png";
        a.data = png.data();
        a.isInline = true;
        m.attachments << a;

        MessageView view;
        view.resize(800, 600);
        QVERIFY(!view.findChild<QWebEngineView *>()); // moteur web créé seulement à la demande
        auto *web = view.ensureEngine();
        QVERIFY(web);

        auto inspect = [&](bool allowRemote) {
            QSignalSpy loaded(web, &QWebEngineView::loadFinished);
            view.showMessage(m, allowRemote);
            if (!loaded.wait(15000))
                return QString("timeout");
            QTest::qWait(allowRemote ? 3000 : 500); // laisser le temps à l'image distante
            QString result;
            web->page()->runJavaScript(
                "JSON.stringify({title: document.title, text: document.getElementById('t').textContent,"
                " cid: document.getElementById('cid').naturalWidth,"
                " remote: document.getElementById('remote').naturalWidth})",
                QWebEngineScript::ApplicationWorld, [&](const QVariant &v) { result = v.toString(); });
            for (int i = 0; i < 50 && result.isEmpty(); ++i)
                QTest::qWait(100);
            return result;
        };

        const QString blocked = inspect(false);
        view.resize(700, 260);
        view.show();
        QTest::qWait(1500);
        if (!qEnvironmentVariable("GDESK_TEST_OUT").isEmpty())
            view.grab().save(qEnvironmentVariable("GDESK_TEST_OUT") + "/viewer-emoji.png");
        qInfo() << "Images distantes bloquées :" << blocked;
        QVERIFY(blocked.contains("\"text\":\"Bonjour éàü 😀👨‍👩‍👧🇫🇷 € œ\""));
        QVERIFY(blocked.contains("\"cid\":10"));
        QVERIFY(blocked.contains("\"remote\":0"));
        QVERIFY(!blocked.contains("js-a-tourne"));
        QVERIFY(!view.findChild<QWidget *>("remoteBar")->isHidden());

        const QString allowed = inspect(true);
        qInfo() << "Images distantes autorisées :" << allowed;
        QVERIFY(allowed.contains("\"remote\":92"));
        QVERIFY(view.findChild<QWidget *>("remoteBar")->isHidden());
    }
    void settingsDialog()
    {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("gdesk.conf"), QSettings::IniFormat);
        settings.setValue("trusted_senders", QStringList{"news@exemple.fr"});
        settings.setValue("known_addresses", QStringList{"a@b.fr", "c@d.fr"});
        const QString out = qEnvironmentVariable("GDESK_TEST_OUT");

        for (const QString &theme : {QString("light"), QString("dark")}) {
            Theme::apply(theme);
            SettingsDialog dlg(settings, "marie.dupont@gmail.com");
            dlg.resize(840, 900);
            dlg.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dlg));
            auto *nav = dlg.findChildren<QListWidget *>().value(0);
            QVERIFY(nav && nav->count() == 4);
            for (int i = 0; i < nav->count(); ++i) {
                nav->setCurrentRow(i);
                QTest::qWait(50);
                if (!out.isEmpty())
                    dlg.grab().save(QString("%1/settings-%2-%3.png").arg(out, theme).arg(i));
                if (!out.isEmpty() && i == 0)
                    for (QGroupBox *box : dlg.findChildren<QGroupBox *>())
                        if (box->title() == "Dates")
                            box->grab().save(QString("%1/settings-dates-%2.png").arg(out, theme));
            }
            // Choix : thème sombre, lignes espacées, aperçu à droite
            for (PreviewCard *card : dlg.findChildren<PreviewCard *>())
                if (card->value() == "dark" || card->value() == "spacious" || card->value() == "right")
                    card->click();
            QSignalSpy applied(&dlg, &SettingsDialog::applied);
            QMetaObject::invokeMethod(&dlg, "accept"); // Annuler ne doit rien enregistrer…
            QCOMPARE(applied.count(), 0);
        }
        QCOMPARE(settings.value("theme", "system").toString(), QString("system"));

        SettingsDialog dlg(settings, "marie.dupont@gmail.com");
        for (PreviewCard *card : dlg.findChildren<PreviewCard *>())
            if (card->value() == "dark" || card->value() == "spacious" || card->value() == "right")
                card->click();
        QSignalSpy applied(&dlg, &SettingsDialog::applied);
        auto *apply = dlg.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
        apply->click(); // … « Appliquer » si
        QCOMPARE(applied.count(), 1);
        QCOMPARE(settings.value("theme").toString(), QString("dark"));
        QCOMPARE(settings.value("density").toString(), QString("spacious"));
        QCOMPARE(settings.value("layout").toString(), QString("right"));
        QCOMPARE(settings.value("trusted_senders").toStringList(), QStringList{"news@exemple.fr"});
        Theme::apply("system");
    }
    // Régression : la clé « splitter_right » de la 2.0 (séparateur vertical) empêchait l'aperçu à droite
    void previewLayoutFromOldConfig()
    {
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        {
            QSplitter old(Qt::Vertical);
            old.addWidget(new QWidget);
            old.addWidget(new QWidget);
            old.setSizes({192, 288});
            QSettings s("gdesk", "gdesk");
            s.setValue("splitter_right", old.saveState()); // état laissé par la 2.0
            s.setValue("layout", "right");
            s.setValue("close_to_tray", false);
        }

        MainWindow win;
        win.resize(1200, 800);
        qobject_cast<QStackedWidget *>(win.centralWidget())->setCurrentIndex(1); // page des mails, comme connecté
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto *view = win.findChild<MessageView *>();
        QVERIFY(view);
        auto *splitter = qobject_cast<QSplitter *>(view->parentWidget());
        QVERIFY(splitter);
        auto *list = splitter->widget(0);

        auto check = [&](Qt::Orientation expected) {
            QCOMPARE(splitter->orientation(), expected);
            const QRect l = list->geometry(), v = view->geometry();
            if (expected == Qt::Horizontal) { // côte à côte, même hauteur
                QVERIFY2(v.left() >= l.right(), "l'aperçu doit être à droite de la liste");
                QCOMPARE(v.height(), l.height());
            } else {                           // l'un sous l'autre, même largeur
                QVERIFY2(v.top() >= l.bottom(), "l'aperçu doit être sous la liste");
                QCOMPARE(v.width(), l.width());
            }
            QVERIFY2(l.width() > 100 && l.height() > 100 && v.width() > 100 && v.height() > 100,
                     qPrintable(QString("liste %1x%2, aperçu %3x%4").arg(l.width()).arg(l.height())
                                    .arg(v.width()).arg(v.height())));
        };
        check(Qt::Horizontal);

        QSettings s("gdesk", "gdesk");
        QVERIFY(!s.contains("splitter_right"));
        for (const auto &[layout, orientation] : {std::pair{"below", Qt::Vertical}, std::pair{"right", Qt::Horizontal},
                                                  std::pair{"below", Qt::Vertical}}) {
            s.setValue("layout", layout);
            win.applySettings();
            QTest::qWait(50);
            check(orientation);
        }
        win.close();
    }
    void mailCards()
    {
        struct Row { const char *who, *date, *subject, *snippet; QStringList labels; };
        const QList<Row> rows = {
            {"Élodie 🌸 Martin", "14:32", "🎉 Réunion de rentrée : ordre du jour ✅",
             "Bonjour à tous 👋 voici l'ordre du jour de lundi — café ☕ offert, 5 € la part ½ 🇫🇷",
             {"INBOX", "UNREAD"}},
            {"Banque Populaire — Service client très long nom d'expéditeur", "11:05",
             "Votre relevé de compte du mois de septembre est disponible dans votre espace personnel en ligne",
             "Madame, Monsieur, votre relevé est disponible.", {"INBOX", "STARRED"}},
            {"Paul", "3 sept.", "Re: photos 📷 Привет こんにちは", "Super, merci ! 😂👍", {"INBOX"}},
            {"À : Jean Dupont, Marie", "28/08/2025", "(sans objet)", "", {"SENT"}},
        };
        const QString out = qEnvironmentVariable("GDESK_TEST_OUT");
        for (const QString &theme : {QString("light"), QString("dark")}) {
            Theme::apply(theme);
            for (const QString &density : {QString("compact"), QString("comfortable"), QString("spacious")}) {
                for (int width : {400, 760}) {
                    QTreeWidget list;
                    list.setHeaderHidden(true);
                    list.setRootIsDecorated(false);
                    list.setIndentation(0);
                    list.setFrameShape(QFrame::NoFrame);
                    list.setUniformRowHeights(true);
                    list.viewport()->setBackgroundRole(QPalette::Window);
                    auto *delegate = new MailListDelegate(&list);
                    delegate->density = density;
                    list.setItemDelegate(delegate);
                    delegate->attachTo(&list);
                    for (const Row &r : rows) {
                        auto *item = new QTreeWidgetItem(&list);
                        item->setData(0, MailRoles::Who, r.who);
                        item->setData(0, MailRoles::Date, r.date);
                        item->setData(0, MailRoles::Subject, r.subject);
                        item->setData(0, MailRoles::Snippet, r.snippet);
                        item->setData(0, MailRoles::Labels, r.labels);
                    }
                    new QTreeWidgetItem(&list); // ligne encore en chargement
                    list.setCurrentItem(list.topLevelItem(2));
                    list.resize(width, 520);
                    list.show();
                    QVERIFY(QTest::qWaitForWindowExposed(&list));
                    if (!out.isEmpty())
                        list.grab().save(QString("%1/cards-%2-%3-%4.png").arg(out, theme, density).arg(width));

                    // Hauteur : 2 lignes en compact, 3 en aéré, 4 en espacé
                    const int h = list.visualItemRect(list.topLevelItem(0)).height();
                    const int line = QFontMetrics(list.font()).height();
                    QVERIFY2(h >= line * (density == "compact" ? 2 : density == "spacious" ? 4 : 3),
                             qPrintable(QString("hauteur %1 pour %2").arg(h).arg(density)));

                    // Clic sur l'étoile de la 1re carte
                    QSignalSpy star(delegate, &MailListDelegate::starClicked);
                    QStyleOptionViewItem opt;
                    opt.initFrom(list.viewport());
                    opt.rect = list.visualItemRect(list.topLevelItem(0));
                    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, delegate->starRect(opt).center());
                    QCOMPARE(star.count(), 1);
                    QCOMPARE(star.first().first().value<QModelIndex>().row(), 0);
                }
            }
        }
        Theme::apply("system");
    }
    void sidebar()
    {
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        const QString out = qEnvironmentVariable("GDESK_TEST_OUT");
        QJsonArray labels;
        auto label = [&](const char *id, const char *name, const char *color) {
            QJsonObject l{{"id", id}, {"name", name}, {"type", "user"}};
            if (*color)
                l.insert("color", QJsonObject{{"backgroundColor", color}, {"textColor", "#ffffff"}});
            labels.append(l);
        };
        label("Label_1", "Factures", "#fb4c2f");
        label("Label_2", "Voyages", "#16a766");
        label("Label_3", "Voyages/Japon 2026", "#4a86e8");
        label("Label_4", "Association", "");

        for (const QString &theme : {QString("light"), QString("dark")}) {
            QSettings("gdesk", "gdesk").setValue("theme", theme);
            MainWindow win;
            win.resize(1100, 860);
            qobject_cast<QStackedWidget *>(win.centralWidget())->setCurrentIndex(1);
            win.populateFolders(QJsonObject{{"labels", labels}});
            win.show();
            QVERIFY(QTest::qWaitForWindowExposed(&win));

            QTreeWidget *folders = nullptr;
            for (QTreeWidget *t : win.findChildren<QTreeWidget *>())
                if (qobject_cast<FolderDelegate *>(t->itemDelegate()))
                    folders = t;
            QVERIFY(folders);

            // 4 sections : Messagerie, Catégories, Plus, Libellés
            QStringList sections;
            QHash<QString, QTreeWidgetItem *> byId;
            QTreeWidgetItemIterator it(folders);
            for (; *it; ++it) {
                if (!(*it)->data(0, FolderRoles::SectionKey).toString().isEmpty())
                    sections << (*it)->data(0, FolderRoles::Name).toString();
                else
                    byId.insert((*it)->data(0, FolderRoles::Id).toString(), *it);
            }
            QCOMPARE(sections, (QStringList{"Messagerie", "Catégories", "Plus", "Libellés"}));
            for (const char *id : {"INBOX", "STARRED", "IMPORTANT", "SENT", "DRAFT", "CATEGORY_PERSONAL",
                                   "CATEGORY_SOCIAL", "CATEGORY_PROMOTIONS", "CATEGORY_UPDATES", "CATEGORY_FORUMS",
                                   "", "SPAM", "TRASH", "Label_1", "Label_3"})
                QVERIFY2(byId.contains(id), id);
            QCOMPARE(byId["Label_3"]->parent(), byId["Label_2"]); // sous-libellé
            QCOMPARE(byId["Label_1"]->data(0, FolderRoles::Color).value<QColor>(), QColor("#fb4c2f"));
            QCOMPARE(folders->currentItem(), byId["INBOX"]);

            // Compteurs d'exemple pour la capture
            byId["INBOX"]->setData(0, FolderRoles::Count, 12);
            byId["CATEGORY_SOCIAL"]->setData(0, FolderRoles::Count, 3);
            byId["CATEGORY_PROMOTIONS"]->setData(0, FolderRoles::Count, 128);
            byId["DRAFT"]->setData(0, FolderRoles::Count, 2);
            byId["Label_1"]->setData(0, FolderRoles::Count, 1);
            QTest::qWait(50);
            if (!out.isEmpty())
                win.grab().save(QString("%1/sidebar-%2.png").arg(out, theme));

            // Clic sur l'en-tête « Plus » : section repliée et mémorisée, sélection conservée
            QTreeWidgetItem *more = byId[""]->parent();
            QTest::mouseClick(folders->viewport(), Qt::LeftButton, {}, folders->visualItemRect(more).center());
            QVERIFY(!more->isExpanded());
            QCOMPARE(QSettings("gdesk", "gdesk").value("sidebar_collapsed").toStringList(), QStringList{"more"});
            QCOMPARE(folders->currentItem(), byId["INBOX"]);
            QTest::mouseClick(folders->viewport(), Qt::LeftButton, {}, folders->visualItemRect(more).center());
            QVERIFY(more->isExpanded());
            win.close();
        }
        Theme::apply("system");
    }
    void composerEmojiPicker()
    {
        Composer c(nullptr, "moi@gmail.com", "Gabriel 🚀", {});
        c.prepare(Composer::New);
        c.show();
        QVERIFY(QTest::qWaitForWindowExposed(&c));
        QToolButton *picker = nullptr;
        for (QToolButton *b : c.findChildren<QToolButton *>())
            if (b->text() == "😀" && b->menu())
                picker = b;
        QVERIFY(picker);
        auto *subject = c.findChildren<QLineEdit *>().value(3); // À, Cc, Cci, Objet
        auto *body = c.findChild<QPlainTextEdit *>();
        QVERIFY(subject && body);
        QVERIFY(body->toPlainText().contains("Gabriel 🚀")); // signature avec emoji

        auto pick = [&](QWidget *focus, const QString &emoji) {
            focus->setFocus();
            c.activateWindow();
            QTest::qWait(20);
            QMenu *menu = picker->menu();
            QMetaObject::invokeMethod(menu, "aboutToShow"); // comme à l'ouverture du menu
            for (QToolButton *b : menu->findChildren<QToolButton *>())
                if (b->text() == emoji) {
                    b->click();
                    return true;
                }
            return false;
        };
        QVERIFY(pick(subject, "🎉"));
        QCOMPARE(subject->text(), QString("🎉"));
        QVERIFY(pick(body, "🇫🇷"));
        QVERIFY(body->toPlainText().startsWith("🇫🇷"));

        const QString out = qEnvironmentVariable("GDESK_TEST_OUT");
        if (!out.isEmpty()) {
            picker->menu()->popup(picker->mapToGlobal(QPoint(0, picker->height())));
            QTest::qWait(100);
            picker->menu()->grab().save(out + "/emoji-picker.png");
            picker->menu()->close();
        }
    }
    void startFolderAndNotifications()
    {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("gdesk.conf"), QSettings::IniFormat);
        const QList<LabelChoice> labels{{"Label_1", "Factures", QColor("#fb4c2f")},
                                        {"Label_3", "Voyages / Japon 2026", QColor("#4a86e8")}};
        {
            SettingsDialog dlg(settings, "moi@gmail.com", labels);
            auto combos = dlg.findChildren<QComboBox *>();
            QComboBox *start = nullptr;
            for (QComboBox *c : combos)
                if (c->findData("__last__") >= 0)
                    start = c;
            QVERIFY(start);
            QCOMPARE(start->currentData().toString(), QString("INBOX")); // valeur par défaut
            for (const char *id : {"CATEGORY_SOCIAL", "CATEGORY_PROMOTIONS", "STARRED", "Label_3"})
                QVERIFY2(start->findData(id) >= 0, id);
            start->setCurrentIndex(start->findData("CATEGORY_SOCIAL"));

            QTreeWidget *tree = nullptr;
            for (QTreeWidget *t : dlg.findChildren<QTreeWidget *>())
                if (t->topLevelItemCount() > 0 && t->topLevelItem(0)->flags() & Qt::ItemIsUserCheckable
                    && t->objectName() != "badgeTree")
                    tree = t;
            QVERIFY(tree);
            QHash<QString, QTreeWidgetItem *> items;
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                items.insert((*it)->data(0, Qt::UserRole).toString(), *it);
            // Par défaut : réception cochée, donc toutes ses catégories
            QCOMPARE(items["INBOX"]->checkState(0), Qt::Checked);
            QCOMPARE(items["CATEGORY_PROMOTIONS"]->checkState(0), Qt::Checked);
            QCOMPARE(items["Label_1"]->checkState(0), Qt::Unchecked);
            // Tout sauf Promotions, plus un libellé
            items["CATEGORY_PROMOTIONS"]->setCheckState(0, Qt::Unchecked);
            QCOMPARE(items["INBOX"]->checkState(0), Qt::PartiallyChecked);
            items["Label_1"]->setCheckState(0, Qt::Checked);
            items["STARRED"]->setCheckState(0, Qt::Checked);
            dlg.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        }
        QCOMPARE(settings.value("start_folder").toString(), QString("CATEGORY_SOCIAL"));
        QStringList notify = settings.value("notify_folders").toStringList();
        notify.sort();
        QCOMPARE(notify, (QStringList{"CATEGORY_FORUMS", "CATEGORY_PERSONAL", "CATEGORY_SOCIAL", "CATEGORY_UPDATES",
                                     "Label_1", "STARRED"}));
        {
            // Relecture : Promotions décochée, réception partielle
            SettingsDialog dlg(settings, "moi@gmail.com", labels);
            QHash<QString, QTreeWidgetItem *> items;
            for (QTreeWidget *t : dlg.findChildren<QTreeWidget *>())
                if (t->objectName() != "badgeTree")
                    for (QTreeWidgetItemIterator it(t); *it; ++it)
                        items.insert((*it)->data(0, Qt::UserRole).toString(), *it);
            QCOMPARE(items["INBOX"]->checkState(0), Qt::PartiallyChecked);
            QCOMPARE(items["CATEGORY_PROMOTIONS"]->checkState(0), Qt::Unchecked);
            QCOMPARE(items["Label_1"]->checkState(0), Qt::Checked);
            // Tout recocher : la réception entière est enregistrée, sans lister ses catégories
            items["INBOX"]->setCheckState(0, Qt::Checked);
            items["Label_1"]->setCheckState(0, Qt::Unchecked);
            items["STARRED"]->setCheckState(0, Qt::Unchecked);
            dlg.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
            const QString out = qEnvironmentVariable("GDESK_TEST_OUT");
            if (!out.isEmpty()) {
                items["CATEGORY_PROMOTIONS"]->setCheckState(0, Qt::Unchecked);
                auto *nav = dlg.findChildren<QListWidget *>().value(0);
                nav->setCurrentRow(2);
                dlg.resize(840, 1000);
                dlg.show();
                QTest::qWait(100);
                dlg.grab().save(out + "/settings-general.png");
            }
        }
        QCOMPARE(settings.value("notify_folders").toStringList(), QStringList{"INBOX"});

        // Démarrage sur une catégorie dont la section est repliée
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        {
            QSettings s("gdesk", "gdesk");
            s.setValue("start_folder", "CATEGORY_SOCIAL");
            s.setValue("sidebar_collapsed", QStringList{"categories"});
        }
        MainWindow win;
        win.restoreStartFolder();
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        QTreeWidget *folders = nullptr;
        for (QTreeWidget *t : win.findChildren<QTreeWidget *>())
            if (qobject_cast<FolderDelegate *>(t->itemDelegate()))
                folders = t;
        QVERIFY(folders && folders->currentItem());
        QCOMPARE(folders->currentItem()->data(0, FolderRoles::Id).toString(), QString("CATEGORY_SOCIAL"));
        QVERIFY(folders->currentItem()->parent()->isExpanded());

        // « Dernière boîte consultée »
        {
            QSettings s("gdesk", "gdesk");
            s.setValue("start_folder", "__last__");
            s.setValue("last_folder", "STARRED");
        }
        win.restoreStartFolder();
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        QCOMPARE(folders->currentItem()->data(0, FolderRoles::Id).toString(), QString("STARRED"));
    }
    void folderContextMenu()
    {
        FakeGmail gmail;
        for (int i = 0; i < 60; ++i)
            gmail.add(QString("i%1").arg(i), {"INBOX", "CATEGORY_PERSONAL"});
        for (int i = 0; i < 1200; ++i) {
            QSet<QString> l{"INBOX", "CATEGORY_PROMOTIONS"};
            if (i < 700)
                l.insert("UNREAD");
            gmail.add(QString("p%1").arg(i), l);
        }
        GmailApi::setBaseUrlForTesting(gmail.base());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());

        MainWindow win;
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        auto actions = [&](const QString &id) {
            QMenu *menu = win.folderMenu(id);
            QHash<QString, QAction *> h;
            for (QAction *a : menu->actions())
                if (!a->objectName().isEmpty())
                    h.insert(a->objectName(), a);
            menu->deleteLater();
            return h;
        };
        auto idle = [&] { return actions("INBOX").value("markRead")->isEnabled(); };

        // Contenu du menu selon la boîte
        QVERIFY(actions("TRASH").contains("emptyTrashWeb"));
        QVERIFY(!actions("TRASH").contains("empty"));
        QVERIFY(!actions("").value("empty")->isEnabled());           // « Tous les messages »
        QVERIFY(!actions("DRAFT").value("markRead")->isEnabled());   // brouillons
        QVERIFY(actions("CATEGORY_PROMOTIONS").value("empty")->isEnabled());
        if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty()) {
            QMenu *menu = win.folderMenu("CATEGORY_PROMOTIONS");
            menu->popup(QPoint(0, 0));
            QTest::qWait(100);
            menu->grab().save(out + "/folder-menu.png");
            menu->close();
        }

        // Tout marquer comme lu : seuls les 700 non-lus, en un appel
        actions("CATEGORY_PROMOTIONS").value("markRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);
        QCOMPARE(gmail.count({"CATEGORY_PROMOTIONS", "UNREAD"}), 0);
        QCOMPARE(gmail.batches.size(), 1);
        QCOMPARE(gmail.batches[0].count, 700);
        QCOMPARE(gmail.batches[0].remove, QStringList{"UNREAD"});
        QCOMPARE(gmail.count({"CATEGORY_PERSONAL", "UNREAD"}), 0); // autres boîtes intactes

        // Tout marquer comme non lu : 1 200 messages → paquets de 1 000 + 200
        gmail.batches.clear();
        actions("CATEGORY_PROMOTIONS").value("markUnread")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);
        QCOMPARE(gmail.count({"CATEGORY_PROMOTIONS", "UNREAD"}), 1200);
        QCOMPARE(gmail.batches.size(), 2);
        QCOMPARE(gmail.batches[0].count, 1000);
        QCOMPARE(gmail.batches[1].count, 200);
        QCOMPARE(gmail.batches[0].add, QStringList{"UNREAD"});

        // Vider : confirmation annulée → rien ne bouge
        QString dialogText;
        // Répond automatiquement à la boîte de confirmation dès qu'elle s'ouvre
        auto answer = [&](QMessageBox::StandardButton button) {
            auto *timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [&, timer, button] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    dialogText = box->text();
                    box->button(button)->click();
                    timer->deleteLater();
                }
            });
            timer->start(20);
        };
        gmail.batches.clear();
        answer(QMessageBox::Cancel);
        actions("CATEGORY_PROMOTIONS").value("empty")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!dialogText.isEmpty() && idle(), 15000);
        QVERIFY2(QString(dialogText).remove(QRegularExpression("\\D")).startsWith("1200"), qPrintable(dialogText));
        QVERIFY(dialogText.contains("Promotions"));
        QCOMPARE(gmail.batches.size(), 0);
        QCOMPARE(gmail.count({"TRASH"}), 0);

        // Vider, confirmé : les 1 200 promotions partent à la corbeille, le reste de la réception non
        dialogText.clear();
        answer(QMessageBox::Yes);
        actions("CATEGORY_PROMOTIONS").value("empty")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!dialogText.isEmpty() && idle(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(gmail.count({"TRASH"}), 1200, 15000);
        QCOMPARE(gmail.count({"CATEGORY_PROMOTIONS", "TRASH"}), 1200);
        QCOMPARE(gmail.count({"CATEGORY_PERSONAL", "TRASH"}), 0);
        QCOMPARE(gmail.batches.size(), 2);
        QCOMPARE(gmail.batches[0].add, QStringList{"TRASH"});

        // Sélectionner tous les messages : la réception (60 messages hors corbeille) se charge puis tout est sélectionné
        auto *list = [&]() -> QTreeWidget * {
            for (QTreeWidget *t : win.findChildren<QTreeWidget *>())
                if (qobject_cast<MailListDelegate *>(t->itemDelegate()))
                    return t;
            return nullptr;
        }();
        QVERIFY(list);
        actions("INBOX").value("selectAll")->trigger();
        auto checkedCount = [&] {
            int n = 0;
            for (int i = 0; i < list->topLevelItemCount(); ++i)
                n += list->topLevelItem(i)->data(0, MailRoles::Checked).toBool();
            return n;
        };
        QTRY_COMPARE_WITH_TIMEOUT(checkedCount(), 50, 15000); // première page de 50, toutes cochées
        QCOMPARE(list->topLevelItemCount(), 50);

        GmailApi::setBaseUrlForTesting("https://gmail.googleapis.com/gmail/v1/users/me/");
        GoogleAuth::setAccessTokenForTesting({});
    }
    void checkboxSelection()
    {
        FakeGmail gmail;
        for (int i = 0; i < 12; ++i) {
            QSet<QString> l{"INBOX", "CATEGORY_PERSONAL"};
            if (i < 4)
                l.insert("UNREAD");
            if (i == 5)
                l.insert("STARRED");
            gmail.add(QString("m%1").arg(i), l);
        }
        GmailApi::setBaseUrlForTesting(gmail.base());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());

        MainWindow win;
        win.resize(1200, 800);
        qobject_cast<QStackedWidget *>(win.centralWidget())->setCurrentIndex(1);
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        QTreeWidget *list = nullptr;
        for (QTreeWidget *t : win.findChildren<QTreeWidget *>())
            if (qobject_cast<MailListDelegate *>(t->itemDelegate()))
                list = t;
        auto *delegate = qobject_cast<MailListDelegate *>(list->itemDelegate());
        auto *bar = win.findChild<QWidget *>("selectionBar");
        QVERIFY(list && bar);
        auto *master = bar->findChild<QCheckBox *>();
        auto button = [&](const QString &text) {
            for (QToolButton *b : bar->findChildren<QToolButton *>())
                if (b->text() == text)
                    return b;
            return static_cast<QToolButton *>(nullptr);
        };
        auto checked = [&] {
            QStringList ids;
            for (int i = 0; i < list->topLevelItemCount(); ++i)
                if (list->topLevelItem(i)->data(0, MailRoles::Checked).toBool())
                    ids << list->topLevelItem(i)->data(0, MailRoles::Id).toString();
            return ids;
        };
        auto clickCheck = [&](int row, Qt::KeyboardModifiers mods = {}) {
            const QRect r = list->visualItemRect(list->topLevelItem(row));
            QTest::mouseClick(list->viewport(), Qt::LeftButton, mods, delegate->checkRect(r).center());
        };
        auto *view = win.findChild<MessageView *>();

        // Chargement de la réception
        win.folderMenu("INBOX")->actions().first()->trigger(); // « Sélectionner tous les messages »
        QTRY_COMPARE_WITH_TIMEOUT(checked().size(), 12, 15000);
        QCOMPARE(master->checkState(), Qt::Checked);
        QVERIFY(button("Non lu")->isVisible());
        QVERIFY(delegate->selectionMode);

        // Échap : tout est décoché, la barre d'actions disparaît
        QTest::keyClick(&win, Qt::Key_Escape);
        QCOMPARE(checked().size(), 0);
        QCOMPARE(master->checkState(), Qt::Unchecked);
        QVERIFY(!button("Non lu")->isVisible());

        // Cocher un message ne l'ouvre pas (et ne le marque donc pas comme lu)
        clickCheck(1);
        QCOMPARE(checked(), QStringList{"m1"});
        QTest::qWait(200);
        QVERIFY(view->message().id.isEmpty());
        QVERIFY(list->selectedItems().isEmpty());
        QCOMPARE(master->checkState(), Qt::PartiallyChecked);

        // Maj+clic : plage m1 → m4
        clickCheck(4, Qt::ShiftModifier);
        QCOMPARE(checked(), (QStringList{"m1", "m2", "m3", "m4"}));

        // « Lu » sur la sélection
        button("Lu")->click();
        QTRY_COMPARE_WITH_TIMEOUT(gmail.batches.size(), 1, 10000);
        QCOMPARE(gmail.batches[0].count, 4);
        QCOMPARE(gmail.batches[0].remove, QStringList{"UNREAD"});
        QCOMPARE(gmail.count({"UNREAD"}), 1); // seul m0 reste non lu
        QCOMPARE(checked().size(), 4);        // la sélection est conservée

        // L'étoile d'une carte ne concerne que cette carte, même avec une sélection
        QStyleOptionViewItem opt;
        opt.initFrom(list->viewport());
        opt.font = list->font();
        opt.rect = list->visualItemRect(list->topLevelItem(7));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, delegate->starRect(opt).center());
        QTRY_COMPARE_WITH_TIMEOUT(gmail.batches.size(), 2, 10000);
        QCOMPARE(gmail.batches[1].count, 1);
        QCOMPARE(gmail.batches[1].add, QStringList{"STARRED"});
        QVERIFY(view->message().id.isEmpty());

        // Menu de sélection : « Suivis » coche m5 et m7
        auto *pickButton = bar->findChild<QToolButton *>("selectionPick");
        QVERIFY(pickButton);
        // Une seule flèche : la nôtre, sans l'indicateur de menu ajouté par le style
        QCOMPARE(pickButton->arrowType(), Qt::DownArrow);
        QVERIFY(!pickButton->menu());
        QCOMPARE(pickButton->popupMode(), QToolButton::DelayedPopup);
        QMenu *pick = pickButton->findChild<QMenu *>();
        QVERIFY(pick);
        for (QAction *a : pick->actions())
            if (a->text() == "Suivis")
                a->trigger();
        QCOMPARE(checked(), (QStringList{"m5", "m7"}));

        // Supprimer la sélection : les deux partent à la corbeille et disparaissent de la liste
        if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty())
            win.grab().save(out + "/selection.png");
        button("Supprimer")->click();
        QTRY_COMPARE_WITH_TIMEOUT(gmail.trashed.size(), 2, 10000);
        QCOMPARE(QSet<QString>(gmail.trashed.begin(), gmail.trashed.end()), (QSet<QString>{"m5", "m7"}));
        QCOMPARE(list->topLevelItemCount(), 10);
        QCOMPARE(checked().size(), 0);
        QVERIFY(!button("Non lu")->isVisible());

        GmailApi::setBaseUrlForTesting("https://gmail.googleapis.com/gmail/v1/users/me/");
        GoogleAuth::setAccessTokenForTesting({});
    }
    void lazyWebEngine()
    {
        MessageView view;
        view.resize(600, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(!view.hasEngine());
        QVERIFY(!view.findChild<QWebEngineView *>());

        MailMessage m;
        m.id = "a";
        m.subject = "Test";
        m.text = "Bonjour";
        view.showMessage(m, false);
        QVERIFY(view.hasEngine());
        QSignalSpy loaded(view.findChild<QWebEngineView *>(), &QWebEngineView::loadFinished);
        QVERIFY(loaded.wait(15000));

        // Fenêtre cachée : le moteur est libéré (ici immédiatement plutôt qu'après 1 minute)
        view.hide();
        view.releaseEngine();
        QVERIFY(!view.hasEngine());
        QVERIFY(!view.findChild<QWebEngineView *>());
        // Réaffichée : le message est redessiné avec un nouveau moteur
        view.show();
        QVERIFY(view.hasEngine());
        QCOMPARE(view.message().id, QString("a"));
    }

    void incrementalNewMail()
    {
        FakeGmail gmail;
        for (int i = 0; i < 12; ++i)
            gmail.add(QString("m%1").arg(i), {"INBOX"});
        GmailApi::setBaseUrlForTesting(gmail.base());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());

        MainWindow win;
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        QTreeWidget *list = nullptr;
        for (QTreeWidget *t : win.findChildren<QTreeWidget *>())
            if (qobject_cast<MailListDelegate *>(t->itemDelegate()))
                list = t;
        win.folderMenu("INBOX")->actions().first()->trigger(); // charge la réception
        QTRY_COMPARE_WITH_TIMEOUT(gmail.metadataRequests.size(), 12, 10000);
        QVERIFY(gmail.listFields.first().contains("messages/id")); // réponse partielle demandée

        // Deux nouveaux messages arrivent
        gmail.add("new1", {"INBOX", "UNREAD"}, true);
        gmail.add("new2", {"INBOX", "UNREAD"}, true);
        gmail.metadataRequests.clear();
        win.insertNewMessages();
        QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 14, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(gmail.metadataRequests.size(), 2, 10000);
        QCOMPARE(QSet<QString>(gmail.metadataRequests.begin(), gmail.metadataRequests.end()),
                 (QSet<QString>{"new1", "new2"})); // seuls les nouveaux sont demandés
        QCOMPARE(list->topLevelItem(0)->data(0, MailRoles::Id).toString(), QString("new2"));
        QCOMPARE(list->topLevelItem(1)->data(0, MailRoles::Id).toString(), QString("new1"));
        QCOMPARE(list->topLevelItem(2)->data(0, MailRoles::Id).toString(), QString("m0"));
        // Les anciennes lignes, cochées ou non, sont conservées telles quelles
        QCOMPARE(list->topLevelItem(13)->data(0, MailRoles::Id).toString(), QString("m11"));

        GmailApi::setBaseUrlForTesting("https://gmail.googleapis.com/gmail/v1/users/me/");
        GoogleAuth::setAccessTokenForTesting({});
    }
    // Régression : au premier message ouvert, la fenêtre principale clignotait. Ajouter le moteur
    // web (rendu RHI) à une fenêtre déjà affichée obligeait Qt à la détruire puis la recréer.
    // (Ne se reproduit que sur un vrai écran : lancer ce test sans QT_QPA_PLATFORM=offscreen.)
    void noWindowRecreationOnFirstMessage()
    {
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        MainWindow win;
        QVERIFY(win.findChild<QWidget *>("rhiSurface"));
        win.resize(1000, 700);
        qobject_cast<QStackedWidget *>(win.centralWidget())->setCurrentIndex(1);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        struct SurfaceWatch : QObject {
            int destroyed = 0;
            bool eventFilter(QObject *, QEvent *e) override
            {
                if (e->type() == QEvent::PlatformSurface
                    && static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType()
                           == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
                    ++destroyed;
                return false;
            }
        } watch;
        win.windowHandle()->installEventFilter(&watch);

        auto *view = win.findChild<MessageView *>();
        QVERIFY(view && !view->hasEngine());
        MailMessage m;
        m.id = "a";
        m.text = "Bonjour";
        view->showMessage(m, false);
        QSignalSpy loaded(view->findChild<QWebEngineView *>(), &QWebEngineView::loadFinished);
        QVERIFY(loaded.wait(15000));
        // Libération puis recréation (retour depuis la barre système) : toujours sans clignotement
        view->releaseEngine();
        view->showMessage(m, false);
        QTest::qWait(300);
        QCOMPARE(watch.destroyed, 0);
        QVERIFY(!view->findChild<QWebEngineView *>()->isWindow());
        win.windowHandle()->removeEventFilter(&watch);
    }
    // Même régression par le vrai chemin : clic sur le premier message de la liste, avec une
    // fenêtre enregistrée maximisée. Aucune autre fenêtre ne doit apparaître, même brièvement.
    void noExtraWindowOnFirstClick()
    {
        FakeGmail gmail;
        for (int i = 0; i < 3; ++i)
            gmail.add(QString("m%1").arg(i), {"INBOX"});
        GmailApi::setBaseUrlForTesting(gmail.base());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        { // fenêtre enregistrée maximisée : restoreGeometry crée la fenêtre native dès le constructeur
            QWidget w;
            w.setWindowState(Qt::WindowMaximized);
            QSettings("gdesk", "gdesk").setValue("geometry", w.saveGeometry());
        }

        MainWindow win;
        qobject_cast<QStackedWidget *>(win.centralWidget())->setCurrentIndex(1);
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QTreeWidget *list = nullptr;
        for (QTreeWidget *t : win.findChildren<QTreeWidget *>())
            if (qobject_cast<MailListDelegate *>(t->itemDelegate()))
                list = t;
        win.folderMenu("INBOX")->actions().first()->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(gmail.metadataRequests.size(), 3, 10000);
        QTest::qWait(200);

        struct Watch : QObject {
            QWindow *main = nullptr;
            int destroyed = 0;
            QStringList others; // fenêtres de premier niveau affichées en dehors de la principale
            bool eventFilter(QObject *o, QEvent *e) override
            {
                auto *w = qobject_cast<QWindow *>(o);
                if (!w)
                    return false;
                if (w == main && e->type() == QEvent::PlatformSurface
                    && static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType()
                           == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
                    ++destroyed;
                if (w != main && !w->parent() && (e->type() == QEvent::Expose || e->type() == QEvent::Show)
                    && w->isVisible() && w->type() != Qt::ToolTip && w->type() != Qt::Popup)
                    others << QString("%1 (%2)").arg(w->objectName(), QString::number(e->type()));
                return false;
            }
        } watch;
        watch.main = win.windowHandle();
        qApp->installEventFilter(&watch);

        const QRect r = list->visualItemRect(list->topLevelItem(0));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, r.center());
        auto *view = win.findChild<MessageView *>();
        QTRY_VERIFY_WITH_TIMEOUT(view->hasEngine(), 10000);
        QSignalSpy loaded(view->findChild<QWebEngineView *>(), &QWebEngineView::loadFinished);
        loaded.wait(15000);
        QTest::qWait(500);
        qApp->removeEventFilter(&watch);
        QCOMPARE(watch.destroyed, 0);
        QVERIFY2(watch.others.isEmpty(), qPrintable(watch.others.join(", ")));

        GmailApi::setBaseUrlForTesting("https://gmail.googleapis.com/gmail/v1/users/me/");
        GoogleAuth::setAccessTokenForTesting({});
    }
    void settingsApplyButton()
    {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("gdesk.conf"), QSettings::IniFormat);
        SettingsDialog dlg(settings, "moi@gmail.com");
        QPushButton *apply = dlg.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
        QVERIFY(!apply->isEnabled()); // rien de modifié
        auto *snippet = [&] {
            for (QCheckBox *c : dlg.findChildren<QCheckBox *>())
                if (c->text().startsWith("Afficher le début"))
                    return c;
            return static_cast<QCheckBox *>(nullptr);
        }();
        QVERIFY(snippet);
        snippet->toggle();
        QVERIFY(apply->isEnabled());
        snippet->toggle(); // retour à l'état enregistré
        QVERIFY(!apply->isEnabled());

        QTreeWidget *badge = dlg.findChild<QTreeWidget *>("badgeTree");
        badge->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
        QVERIFY(apply->isEnabled());
        apply->click();
        QVERIFY(!apply->isEnabled()); // enregistré
        QVERIFY(settings.value("badge_folders").toStringList().isEmpty());

        QLineEdit *name = nullptr;
        for (QLineEdit *e : dlg.findChildren<QLineEdit *>())
            if (!qobject_cast<QComboBox *>(e->parentWidget()))
                name = name ? name : e;
        QVERIFY(name);
        name->setText("Gabriel");
        QVERIFY(apply->isEnabled());
    }
    void badgeFolders()
    {
        // Recherche des non-lus comptés : une seule requête, chaque message compté une fois
        const QMap<QString, QString> names{{"Label_1", "Factures"}, {"Label_3", "Voyages/Japon 2026"}};
        QCOMPARE(MainWindow::badgeQuery({}, names), QString());
        QCOMPARE(MainWindow::badgeQuery({"INBOX"}, names), QString("is:unread (in:inbox)"));
        QCOMPARE(MainWindow::badgeQuery({"CATEGORY_PERSONAL", "STARRED", "Label_3"}, names),
                 QString("is:unread ((in:inbox category:primary) OR is:starred OR label:\"Voyages-Japon-2026\")"));
        QCOMPARE(MainWindow::badgeQuery({"Label_inconnu"}, names), QString()); // libellé supprimé

        // Paramètres : arbre séparé de celui des notifications, réception par défaut
        QTemporaryDir dir;
        QSettings settings(dir.filePath("gdesk.conf"), QSettings::IniFormat);
        const QList<LabelChoice> labels{{"Label_1", "Factures", QColor("#fb4c2f")}};
        auto itemsOf = [](QTreeWidget *tree) {
            QHash<QString, QTreeWidgetItem *> items;
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                items.insert((*it)->data(0, Qt::UserRole).toString(), *it);
            return items;
        };
        {
            SettingsDialog dlg(settings, "moi@gmail.com", labels);
            auto *tree = dlg.findChild<QTreeWidget *>("badgeTree");
            QVERIFY(tree);
            auto items = itemsOf(tree);
            QCOMPARE(items["INBOX"]->checkState(0), Qt::Checked);
            items["INBOX"]->setCheckState(0, Qt::Unchecked);
            items["CATEGORY_PERSONAL"]->setCheckState(0, Qt::Checked);
            items["Label_1"]->setCheckState(0, Qt::Checked);
            dlg.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        }
        QStringList badge = settings.value("badge_folders").toStringList();
        badge.sort();
        QCOMPARE(badge, (QStringList{"CATEGORY_PERSONAL", "Label_1"}));
        QCOMPARE(settings.value("notify_folders").toStringList(), QStringList{"INBOX"}); // inchangé
        {
            SettingsDialog dlg(settings, "moi@gmail.com", labels);
            auto items = itemsOf(dlg.findChild<QTreeWidget *>("badgeTree"));
            QCOMPARE(items["INBOX"]->checkState(0), Qt::PartiallyChecked);
            QCOMPARE(items["Label_1"]->checkState(0), Qt::Checked);
            for (auto *it : items) // plus rien : jamais de pastille
                if (it->childCount() == 0)
                    it->setCheckState(0, Qt::Unchecked);
            dlg.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        }
        QVERIFY(settings.contains("badge_folders"));
        QVERIFY(settings.value("badge_folders").toStringList().isEmpty());
    }
    void dateStyles()
    {
        QLocale::setDefault(QLocale(QLocale::French, QLocale::France));
        const QDateTime now(QDate(2026, 9, 25), QTime(15, 0)); // vendredi
        auto at = [](int y, int m, int d, int h = 9, int min = 5) { return QDateTime(QDate(y, m, d), QTime(h, min)); };
        const QDateTime today = at(2026, 9, 25, 14, 32), yesterday = at(2026, 9, 24), monday = at(2026, 9, 21),
                        earlier = at(2026, 9, 1), old = at(2025, 3, 7);
        auto list = [&](const QString &style, bool time = false) {
            QStringList out;
            for (const QDateTime &d : {today, yesterday, monday, earlier, old})
                out << Mime::shortDate(d, style, time, now);
            return out.join(" | ");
        };
        QCOMPARE(list("short"), QString("14:32 | 24 sept. | 21 sept. | 1 sept. | 07/03/2025"));
        QCOMPARE(list("numeric"), QString("14:32 | 24/09 | 21/09 | 01/09 | 07/03/25"));
        QCOMPARE(list("medium"), QString("14:32 | hier | lun. | mar. 1 sept. | 7 mars 2025"));
        QCOMPARE(list("long"), QString("aujourd'hui à 14:32 | hier | lundi 21 septembre | mardi 1 septembre | 7 mars 2025"));
        QCOMPARE(list("relative"), QString("il y a 28 min | hier | il y a 4 jours | 1 sept. | mars 2025"));
        QCOMPARE(list("short", true), QString("14:32 | 24 sept. 09:05 | 21 sept. 09:05 | 1 sept. 09:05 | 07/03/2025 09:05"));
        QCOMPARE(Mime::shortDate(yesterday, "long", true, now), QString("hier à 09:05"));
        QCOMPARE(Mime::shortDate(now.addSecs(-20), "relative", false, now), QString("à l'instant"));
        QCOMPARE(Mime::shortDate(at(2026, 9, 25, 10, 0), "relative", false, now), QString("il y a 5 h"));

        QCOMPARE(Mime::longDate(today, "long"), QString("vendredi 25 septembre 2026 à 14:32"));
        QCOMPARE(Mime::longDate(today, "abbreviated"), QString("ven. 25 sept. 2026, 14:32"));
        QCOMPARE(Mime::longDate(today, "numeric"), QString("25/09/2026 14:32"));
        // Style choisi dans les paramètres, appliqué partout
        Mime::setDateStyles("numeric", "abbreviated", false);
        QCOMPARE(Mime::longDate(today), QString("ven. 25 sept. 2026, 14:32"));
        QCOMPARE(Mime::shortDate(old), QString("07/03/25"));
        Mime::setDateStyles("short", "long", false);
        QLocale::setDefault(QLocale::system());
    }
    void unsubscribeHeaders()
    {
        // Désabonnement en un clic (RFC 8058) : https et List-Unsubscribe-Post exigés
        auto m = UnsubscribeMethods::parse("<mailto:quit@news.fr?subject=stop>, <https://news.fr/u?id=42>",
                                           "List-Unsubscribe=One-Click");
        QCOMPARE(m.oneClick, QUrl("https://news.fr/u?id=42"));
        QCOMPARE(m.mailto.path(), QString("quit@news.fr"));
        QCOMPARE(m.description(), QString("En un clic"));
        m = UnsubscribeMethods::parse("<https://news.fr/u?id=42>", {}); // sans l'en-tête Post : page web
        QVERIFY(m.oneClick.isEmpty());
        QCOMPARE(m.web, QUrl("https://news.fr/u?id=42"));
        QCOMPARE(m.description(), QString("Page web"));
        m = UnsubscribeMethods::parse("<http://news.fr/u>", "List-Unsubscribe=One-Click"); // pas de POST en http
        QVERIFY(m.oneClick.isEmpty());
        QCOMPARE(m.web, QUrl("http://news.fr/u"));
        m = UnsubscribeMethods::parse("<mailto:quit@news.fr>", {});
        QCOMPARE(m.description(), QString("Par e-mail"));
        QVERIFY(!UnsubscribeMethods::parse({}, {}).isValid());
        QVERIFY(!UnsubscribeMethods::parse("<javascript:alert(1)>, <file:///etc/passwd>", {}).isValid());
        QVERIFY(!UnsubscribeMethods::parse("https://sans-chevrons.fr", {}).isValid());
    }
    void unsubscribeDialog()
    {
        FakeGmail gmail;
        const QString oneClickUrl = QString("http://127.0.0.1:%1/unsub/promo").arg(gmail.server.serverPort());
        auto add = [&](const QString &id, const QString &from, const QString &list, const QString &post = {}) {
            gmail.add(id, {"INBOX", "CATEGORY_PROMOTIONS"});
            QJsonArray h = headers({{"From", from}, {"Subject", "Offre " + id}});
            if (!list.isEmpty())
                h.append(QJsonObject{{"name", "List-Unsubscribe"}, {"value", list}});
            if (!post.isEmpty())
                h.append(QJsonObject{{"name", "List-Unsubscribe-Post"}, {"value", post}});
            gmail.customHeaders.insert(id, h);
        };
        add("p1", "Promo Shop <promo@shop.fr>", "<" + oneClickUrl + ">", "List-Unsubscribe=One-Click");
        add("p2", "Promo Shop <PROMO@shop.fr>", "<" + oneClickUrl + ">", "List-Unsubscribe=One-Click");
        add("p3", "Promo Shop <promo@shop.fr>", "<" + oneClickUrl + ">", "List-Unsubscribe=One-Click");
        add("n1", "=?UTF-8?Q?La_Lettre_=C3=A9t=C3=A9?= <lettre@journal.fr>", "<mailto:quit@journal.fr?subject=STOP>");
        add("w1", "Club <club@club.fr>", "<https://club.fr/desinscription>");
        add("x1", "Ami <ami@perso.fr>", {}); // message ordinaire : pas une newsletter
        GmailApi::setBaseUrlForTesting(gmail.base());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        UnsubscribeDialog::setAllowHttpForTesting(true);
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        QNetworkAccessManager nam;
        GoogleAuth auth(&nam);
        GmailApi api(&auth, &nam);

        {
            UnsubscribeDialog dlg(&api, "Moi <moi@exemple.fr>");
            dlg.show();
            QTreeWidget *list = dlg.list();
            QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 3, 10000);
            // Regroupées par expéditeur (adresse sans casse), triées par nombre de messages
            QTreeWidgetItem *promo = list->topLevelItem(0);
            QCOMPARE(promo->text(UnsubscribeDialog::AddressColumn), QString("promo@shop.fr"));
            QCOMPARE(promo->text(UnsubscribeDialog::CountColumn), QString("3"));
            QCOMPARE(promo->text(UnsubscribeDialog::MethodColumn), QString("En un clic"));
            QTreeWidgetItem *lettre = nullptr, *club = nullptr;
            for (int i = 0; i < 3; ++i) {
                QTreeWidgetItem *it = list->topLevelItem(i);
                QCOMPARE(it->checkState(UnsubscribeDialog::NameColumn), Qt::Unchecked); // rien de coché d'office
                if (it->text(UnsubscribeDialog::AddressColumn) == "lettre@journal.fr")
                    lettre = it;
                if (it->text(UnsubscribeDialog::AddressColumn) == "club@club.fr")
                    club = it;
            }
            QVERIFY(lettre && club);
            QCOMPARE(lettre->text(UnsubscribeDialog::NameColumn), QString("La Lettre été")); // nom décodé
            QCOMPARE(club->text(UnsubscribeDialog::MethodColumn), QString("Page web"));

            auto *button = dlg.findChild<QPushButton *>("unsubscribeButton");
            QVERIFY(!button->isEnabled());
            promo->setCheckState(UnsubscribeDialog::NameColumn, Qt::Checked);
            lettre->setCheckState(UnsubscribeDialog::NameColumn, Qt::Checked);
            QVERIFY(button->isEnabled());
            QCOMPARE(button->text(), QString("Se désabonner (2)"));

            // Confirmation, puis désabonnement : POST en un clic et e-mail de désabonnement
            QTimer::singleShot(0, &dlg, [] {
                for (QWidget *w : QApplication::topLevelWidgets())
                    if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible())
                        box->button(QMessageBox::Yes)->click();
            });
            button->click();
            QTRY_COMPARE_WITH_TIMEOUT(gmail.unsubscribePosts.size(), 1, 10000);
            QCOMPARE(gmail.unsubscribePosts.first(), QString("POST /unsub/promo List-Unsubscribe=One-Click"));
            QTRY_COMPARE_WITH_TIMEOUT(gmail.sent.size(), 1, 10000);
            const QByteArray mail = gmail.sent.first();
            QVERIFY(mail.contains("To: quit@journal.fr"));
            QVERIFY(mail.contains("Subject: STOP"));
            QVERIFY(mail.contains("From: Moi <moi@exemple.fr>"));
            QTRY_COMPARE_WITH_TIMEOUT(promo->text(UnsubscribeDialog::MethodColumn), QString("Désabonné"), 10000);
            QTRY_COMPARE_WITH_TIMEOUT(lettre->text(UnsubscribeDialog::MethodColumn), QString("Demande envoyée par e-mail"), 10000);
            QVERIFY(!(promo->flags() & Qt::ItemIsUserCheckable)); // traité : plus de case
            QVERIFY(!button->isEnabled());
            QVERIFY(club->flags() & Qt::ItemIsUserCheckable);
            if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty()) {
                dlg.resize(860, 420);
                QTest::qWait(200);
                dlg.grab().save(out + "/unsubscribe.png");
            }
        }
        // Listes quittées : plus proposées à la recherche suivante
        {
            UnsubscribeDialog dlg(&api, "moi@exemple.fr");
            QTRY_COMPARE_WITH_TIMEOUT(dlg.list()->topLevelItemCount(), 1, 10000);
            QCOMPARE(dlg.list()->topLevelItem(0)->text(UnsubscribeDialog::AddressColumn), QString("club@club.fr"));
        }

        UnsubscribeDialog::setAllowHttpForTesting(false);
        GmailApi::setBaseUrlForTesting("https://gmail.googleapis.com/gmail/v1/users/me/");
        GoogleAuth::setAccessTokenForTesting({});
    }
    void driveHelpers()
    {
        DriveFile doc;
        doc.name = "Budget";
        doc.mimeType = "application/vnd.google-apps.spreadsheet";
        QVERIFY(doc.isGoogleFile() && !doc.isFolder());
        QCOMPARE(DriveApi::downloadName(doc), QString("Budget.xlsx"));
        QCOMPARE(DriveApi::exportFormat(doc.mimeType).second, QString("xlsx"));
        QVERIFY(DriveApi::exportFormat("application/vnd.google-apps.form").first.isEmpty()); // non exportable
        DriveFile pdf;
        pdf.name = "Facture.pdf";
        pdf.mimeType = "application/pdf";
        QCOMPARE(DriveApi::downloadName(pdf), QString("Facture.pdf"));
        QVERIFY(!pdf.isGoogleFile());
        DriveFile shortcut; // raccourci vers un dossier
        shortcut.mimeType = "application/vnd.google-apps.shortcut";
        shortcut.targetMimeType = DriveApi::FolderMime;
        QVERIFY(shortcut.isFolder());

        // Apostrophes et barres obliques échappées dans les requêtes
        QCOMPARE(DriveApi::searchQuery("l'été"), QString("(name contains 'l\\'été' or fullText contains 'l\\'été') and trashed = false"));
        QCOMPARE(DriveApi::folderQuery("root", true),
                 QString("'root' in parents and trashed = false and mimeType = 'application/vnd.google-apps.folder'"));

        const QJsonObject scope = QJsonDocument::fromJson(
            R"({"error":{"code":403,"message":"Request had insufficient authentication scopes.","errors":[{"reason":"insufficientPermissions"}]}})").object();
        const QJsonObject disabled = QJsonDocument::fromJson(
            R"({"error":{"code":403,"message":"Google Drive API has not been used in project 1 before or it is disabled.","errors":[{"reason":"accessNotConfigured"}]}})").object();
        QVERIFY(DriveApi::needsConsent(scope) && !DriveApi::apiDisabled(scope));
        QVERIFY(DriveApi::apiDisabled(disabled) && !DriveApi::needsConsent(disabled));
        QVERIFY(QString(GoogleAuth::Scope).contains("https://www.googleapis.com/auth/drive"));
        QVERIFY(QString(GoogleAuth::Scope).contains("gmail.modify"));
    }

    void driveUploadDownload()
    {
        FakeDrive drive;
        DriveApi::setBaseUrlsForTesting(drive.api(), drive.upload());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QNetworkAccessManager nam;
        GoogleAuth auth(&nam);
        DriveApi api(&auth, &nam);
        QSignalSpy changed(&api, &DriveApi::filesChanged);

        // Envoi en deux temps (session puis contenu), ici une pièce jointe gardée en mémoire
        QByteArray content;
        for (int i = 0; i < 20000; ++i)
            content += QByteArray::number(i) + ",";
        QJsonObject created;
        QString error = "attente";
        qint64 lastProgress = 0;
        api.uploadData("relevé.csv", "text/csv", content, "root",
                       [&](const QJsonObject &o, const QString &e) { created = o; error = e; },
                       [&](qint64 done, qint64) { lastProgress = done; });
        QTRY_VERIFY_WITH_TIMEOUT(error != "attente", 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const DriveFile uploaded = DriveFile::fromJson(created);
        QCOMPARE(uploaded.name, QString("relevé.csv"));
        QCOMPARE(drive.files[uploaded.id].data, content);
        QCOMPARE(drive.files[uploaded.id].mime, QString("text/csv"));
        QCOMPARE(lastProgress, qint64(content.size()));
        QCOMPARE(changed.size(), 1);

        // Téléchargement en mémoire et vers un fichier
        QByteArray downloaded;
        error = "attente";
        api.download(uploaded, [&](const QByteArray &d, const QString &e) { downloaded = d; error = e; });
        QTRY_VERIFY_WITH_TIMEOUT(error != "attente", 10000);
        QVERIFY(error.isEmpty());
        QCOMPARE(downloaded, content);
        QTemporaryDir dir;
        error = "attente";
        api.downloadToFile(uploaded, dir.filePath("copie.csv"), [&](const QJsonObject &, const QString &e) { error = e; });
        QTRY_VERIFY_WITH_TIMEOUT(error != "attente", 10000);
        QFile copy(dir.filePath("copie.csv"));
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), content);

        // Document Google : exporté au format Office
        DriveFile doc = DriveFile::fromJson(QJsonObject{{"id", drive.add("Notes", "application/vnd.google-apps.document", "root")},
                                                        {"name", "Notes"}, {"mimeType", "application/vnd.google-apps.document"}});
        error = "attente";
        api.download(doc, [&](const QByteArray &d, const QString &e) { downloaded = d; error = e; });
        QTRY_VERIFY_WITH_TIMEOUT(error != "attente", 10000);
        QCOMPARE(downloaded, QByteArray("EXPORT Notes application/vnd.openxmlformats-officedocument.wordprocessingml.document"));

        // Jeton sans l'autorisation Drive : erreur reconnue
        drive.denyScope = true;
        QJsonObject details;
        error = "attente";
        api.listFiles(DriveApi::placeQuery(DriveApi::MyDrive), {}, {}, 10,
                      [&](const QJsonObject &o, const QString &e) { details = o; error = e; });
        QTRY_VERIFY_WITH_TIMEOUT(error != "attente", 10000);
        QVERIFY(DriveApi::needsConsent(details));
        QVERIFY(error.contains("autorisation"));

        DriveApi::setBaseUrlsForTesting("https://www.googleapis.com/drive/v3/", "https://www.googleapis.com/upload/drive/v3/");
        GoogleAuth::setAccessTokenForTesting({});
    }

    void driveView()
    {
        FakeDrive drive;
        const QString factures = drive.add("Factures", DriveApi::FolderMime, "root");
        drive.add("Vide", DriveApi::FolderMime, "root");
        drive.add("zèbre.pdf", "application/pdf", "root", "PDF");
        drive.add("Budget", "application/vnd.google-apps.spreadsheet", "root");
        drive.add("Facture 2.pdf", "application/pdf", factures, "F2");
        drive.add("Facture 10.pdf", "application/pdf", factures, "F10");
        DriveApi::setBaseUrlsForTesting(drive.api(), drive.upload());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QNetworkAccessManager nam;
        GoogleAuth auth(&nam);
        DriveApi api(&auth, &nam);

        DriveView view(&api);
        view.resize(1100, 650);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.activate();
        DriveBrowser *browser = view.browser();
        QTreeWidget *list = browser->list();
        auto names = [&] {
            QStringList n;
            for (int i = 0; i < list->topLevelItemCount(); ++i)
                n << list->topLevelItem(i)->data(0, DriveBrowser::FileRole).value<DriveFile>().name;
            return n;
        };
        QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 4, 10000);
        QVERIFY(drive.queries.last().startsWith("'root' in parents"));
        QVERIFY(browser->canAddHere());
        const QList<QLabel *> quota = view.findChildren<QLabel *>();
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(quota.begin(), quota.end(),
                                             [](QLabel *l) { return l->text().contains("utilisés sur"); }), 10000);

        // Tri par nom : dossiers d'abord, puis ordre naturel (2 avant 10)
        list->header()->sectionClicked(DriveBrowser::NameColumn);
        QCOMPARE(names(), (QStringList{"Factures", "Vide", "Budget", "zèbre.pdf"}));
        if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty())
            view.grab().save(out + "/drive-view.png");

        // Ouverture d'un dossier (double-clic / Entrée)
        emit list->itemActivated(list->topLevelItem(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 2, 10000);
        list->header()->sectionClicked(DriveBrowser::NameColumn);
        QCOMPARE(names(), (QStringList{"Facture 2.pdf", "Facture 10.pdf"}));
        QCOMPARE(browser->currentFolder().id, factures);

        // Import (comme un glisser-déposer) dans le dossier ouvert
        QTemporaryDir dir;
        QFile local(dir.filePath("scan.png"));
        QVERIFY(local.open(QIODevice::WriteOnly));
        local.write("PNG");
        local.close();
        view.uploadPaths({local.fileName()}, browser->currentFolder());
        QTRY_VERIFY_WITH_TIMEOUT(drive.namesIn(factures).contains("scan.png"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 3, 10000); // la liste se met à jour seule

        // Mise à la corbeille (Suppr)
        list->clearSelection();
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->text(0) == "scan.png")
                list->topLevelItem(i)->setSelected(true);
        auto *trash = view.findChild<QAction *>("driveTrash");
        QVERIFY(trash && trash->isEnabled());
        trash->trigger();
        QCOMPARE(list->topLevelItemCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!drive.namesIn(factures).contains("scan.png"), 10000);

        // Retour à Mon Drive puis dossier vide
        browser->goUp();
        QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 4, 10000);
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->text(0) == "Vide")
                emit list->itemActivated(list->topLevelItem(i), 0);
        auto *notice = view.findChild<QLabel *>("driveNotice");
        QTRY_VERIFY_WITH_TIMEOUT(notice->isVisible() && notice->text().contains("Ce dossier est vide"), 10000);

        // Jeton sans l'autorisation Drive : message et bouton pour la donner
        drive.denyScope = true;
        QSignalSpy authorize(&api, &DriveApi::authorizationRequested);
        browser->reload();
        auto *action = view.findChild<QPushButton *>("driveNoticeAction");
        QTRY_VERIFY_WITH_TIMEOUT(action->isVisible() && notice->text().contains("pas encore accès"), 10000);
        if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty())
            view.grab().save(out + "/drive-consent.png");
        action->click();
        QCOMPARE(authorize.size(), 1);

        DriveApi::setBaseUrlsForTesting("https://www.googleapis.com/drive/v3/", "https://www.googleapis.com/upload/drive/v3/");
        GoogleAuth::setAccessTokenForTesting({});
    }

    void drivePickerAndComposer()
    {
        FakeDrive drive;
        const QString archives = drive.add("Archives", DriveApi::FolderMime, "root");
        const QString pdf = drive.add("devis.pdf", "application/pdf", "root", "DEVIS");
        drive.add("Planning", "application/vnd.google-apps.spreadsheet", "root");
        DriveApi::setBaseUrlsForTesting(drive.api(), drive.upload());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        QNetworkAccessManager nam;
        GoogleAuth auth(&nam);
        DriveApi api(&auth, &nam);

        // Choix d'un dossier : seuls les dossiers sont listés ; l'emplacement est mémorisé
        {
            DrivePicker picker(&api, DrivePicker::Folder);
            picker.show();
            QVERIFY(QTest::qWaitForWindowExposed(&picker));
            QTreeWidget *list = picker.findChild<QTreeWidget *>("driveList");
            QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 1, 10000);
            QCOMPARE(picker.folder().id, QString("root"));
            emit list->itemActivated(list->topLevelItem(0), 0);
            QTRY_COMPARE_WITH_TIMEOUT(picker.folder().id, archives, 10000);
            if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty()) {
                QTest::qWait(300); // fil d'Ariane et contenu du dossier affichés
                picker.grab().save(out + "/drive-picker-folder.png");
            }
            picker.accept();
            QCOMPARE(picker.result(), int(QDialog::Accepted));
        }
        {
            DrivePicker picker(&api, DrivePicker::Folder);
            QTRY_COMPARE_WITH_TIMEOUT(picker.folder().id, archives, 10000); // rouvert au même endroit
        }

        // Rédaction : joindre un PDF et une feuille Google (exportée en .xlsx)
        Composer c(nullptr, "moi@gmail.com", {}, {});
        c.prepare(Composer::New);
        c.setDriveApi(&api);
        QVERIFY(c.findChild<QAction *>("attachFromDrive")->isVisible());
        QList<DriveFile> files;
        for (const DriveFile &f : {DriveFile::fromJson(QJsonObject{{"id", pdf}, {"name", "devis.pdf"}, {"mimeType", "application/pdf"}, {"size", "5"}}),
                                   DriveFile::fromJson(QJsonObject{{"id", "f3"}, {"name", "Planning"}, {"mimeType", "application/vnd.google-apps.spreadsheet"}})})
            files << f;
        c.attachFromDrive(files);
        auto *attachments = c.findChild<QListWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(attachments->count(), 2, 10000);
        QStringList labels;
        for (int i = 0; i < attachments->count(); ++i)
            labels << attachments->item(i)->text();
        labels.sort();
        QVERIFY2(labels[0].startsWith("Planning.xlsx") && labels[1].startsWith("devis.pdf"), qPrintable(labels.join(" | ")));

        DriveApi::setBaseUrlsForTesting("https://www.googleapis.com/drive/v3/", "https://www.googleapis.com/upload/drive/v3/");
        GoogleAuth::setAccessTokenForTesting({});
    }

    void navigationRail()
    {
        FakeDrive drive;
        drive.add("Photos", DriveApi::FolderMime, "root");
        DriveApi::setBaseUrlsForTesting(drive.api(), drive.upload());
        GoogleAuth::setAccessTokenForTesting("jeton-de-test");
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
        MainWindow win;
        win.resize(1200, 750);
        qobject_cast<QStackedWidget *>(win.centralWidget())->setCurrentIndex(1);
        win.populateFolders(QJsonObject{{"labels", QJsonArray{}}});
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        auto *rail = win.findChild<QToolBar *>("navigationRail");
        QVERIFY(rail);
        QAction *mail = nullptr, *driveAction = nullptr;
        for (QAction *a : rail->actions()) {
            if (a->text() == "Courrier")
                mail = a;
            if (a->text() == "Drive")
                driveAction = a;
        }
        QVERIFY(mail && driveAction && mail->isChecked());
        QAction *refresh = nullptr;
        for (QAction *a : win.actions())
            if (a->text() == "Actualiser")
                refresh = a;
        QVERIFY(refresh && refresh->isEnabled());
        QVERIFY(!win.findChild<DriveView *>()); // créée seulement au premier affichage

        driveAction->trigger();
        auto *view = win.findChild<DriveView *>();
        QVERIFY(view && view->isVisible());
        QVERIFY(driveAction->isChecked());
        QVERIFY(!refresh->isEnabled()); // raccourcis du courrier inactifs dans Drive
        QTRY_COMPARE_WITH_TIMEOUT(view->browser()->list()->topLevelItemCount(), 1, 10000);
        if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty())
            win.grab().save(out + "/rail-drive.png");

        mail->trigger();
        QVERIFY(!view->isVisible());
        QVERIFY(refresh->isEnabled());
        if (const QString out = qEnvironmentVariable("GDESK_TEST_OUT"); !out.isEmpty())
            win.grab().save(out + "/rail-mail.png");

        DriveApi::setBaseUrlsForTesting("https://www.googleapis.com/drive/v3/", "https://www.googleapis.com/upload/drive/v3/");
        GoogleAuth::setAccessTokenForTesting({});
    }
};

int main(int argc, char *argv[])
{
    // Dossiers de test séparés (~/.qttest) : les tests ne touchent jamais la configuration réelle
    // (réglages, démarrage automatique, portefeuille de secours…)
    QStandardPaths::setTestModeEnabled(true);
    MessageView::registerScheme();
    QApplication app(argc, argv);
    // Sans écran (QT_QPA_PLATFORM=offscreen), Qt ne cherche pas les icônes du système
    QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << "/usr/share/icons");
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName("breeze");
    Theme::enableColorEmoji();

    // Traductions de Qt (boutons Oui/Non, Annuler, sélecteur de fichiers…) dans la langue du système
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale::system(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);
    Tests t;
    return QTest::qExec(&t, argc, argv);
}

#include "tests.moc"
