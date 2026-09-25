// Tests sans compte Google : MIME, connexion OAuth (jusqu'au serveur de Google) et visionneuse.
#include "composer.h"
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
#include <QTest>
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
    void add(const QString &id, const QSet<QString> &labels)
    {
        messages.insert(id, labels);
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
        if (method == "GET" && path.startsWith("messages/")) {
            const QString id = path.section('/', 1, 1);
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
        const QByteArray out = QJsonDocument(route(requestLine.value(0), url.path().section("/users/me/", 1),
                                                   QUrlQuery(url), buf.mid(headerEnd + 4, length)))
                                   .toJson(QJsonDocument::Compact);
        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                 + QByteArray::number(out.size()) + "\r\n\r\n" + out);
        s->disconnectFromHost();
    }
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
        auto *web = view.findChild<QWebEngineView *>();
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
                if (t->topLevelItemCount() > 0 && t->topLevelItem(0)->flags() & Qt::ItemIsUserCheckable)
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
        QMenu *pick = nullptr;
        for (QToolButton *b : bar->findChildren<QToolButton *>())
            if (b->arrowType() == Qt::DownArrow)
                pick = b->menu();
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
