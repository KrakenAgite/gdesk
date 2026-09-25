// Tests sans compte Google : MIME, connexion OAuth (jusqu'au serveur de Google) et visionneuse.
#include "googleauth.h"
#include "mainwindow.h"
#include "messageview.h"
#include "mime.h"
#include "settingsdialog.h"
#include "theme.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QImage>
#include <QJsonArray>
#include <QListWidget>
#include <QSplitter>
#include <QStackedWidget>
#include <QSettings>
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
};

int main(int argc, char *argv[])
{
    MessageView::registerScheme();
    QApplication app(argc, argv);

    // Traductions de Qt (boutons Oui/Non, Annuler, sélecteur de fichiers…) dans la langue du système
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale::system(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);
    Tests t;
    return QTest::qExec(&t, argc, argv);
}

#include "tests.moc"
