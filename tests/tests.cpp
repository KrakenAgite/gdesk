// Tests sans compte Google : MIME, connexion OAuth (jusqu'au serveur de Google) et visionneuse.
#include "googleauth.h"
#include "messageview.h"
#include "mime.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QImage>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QStringEncoder>
#include <QApplication>
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
        m.html = "<p id=t>Bonjour éàü</p><img id=cid src=\"cid:img1@x\">"
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
        qInfo() << "Images distantes bloquées :" << blocked;
        QVERIFY(blocked.contains("\"text\":\"Bonjour éàü\""));
        QVERIFY(blocked.contains("\"cid\":10"));
        QVERIFY(blocked.contains("\"remote\":0"));
        QVERIFY(!blocked.contains("js-a-tourne"));
        QVERIFY(!view.findChild<QWidget *>("remoteBar")->isHidden());

        const QString allowed = inspect(true);
        qInfo() << "Images distantes autorisées :" << allowed;
        QVERIFY(allowed.contains("\"remote\":92"));
        QVERIFY(view.findChild<QWidget *>("remoteBar")->isHidden());
    }
};

int main(int argc, char *argv[])
{
    MessageView::registerScheme();
    QApplication app(argc, argv);
    Tests t;
    return QTest::qExec(&t, argc, argv);
}

#include "tests.moc"
