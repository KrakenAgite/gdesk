// G-Desk : client mail Gmail natif pour KDE, basé sur l'API officielle de Google.
#include "mainwindow.h"
#include "messageview.h"
#include "theme.h"

#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <QLocalServer>
#include <QLocalSocket>
#include <unistd.h>

int main(int argc, char *argv[])
{
    MessageView::registerScheme();
    QApplication app(argc, argv);
    QApplication::setApplicationName("gdesk");
    QApplication::setApplicationDisplayName("G-Desk");
    QApplication::setApplicationVersion(GDESK_VERSION);
    QApplication::setDesktopFileName("gdesk");
    QApplication::setQuitOnLastWindowClosed(false);
    Theme::init(); // mémorise l'apparence KDE avant toute personnalisation

    // Traductions de Qt (boutons Oui/Non, Annuler, sélecteur de fichiers…) dans la langue du système
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale::system(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);

    const QStringList args = app.arguments().mid(1);
    QString mailto;
    for (const QString &a : args)
        if (a.startsWith("mailto:"))
            mailto = a;

    // Une seule instance : une 2e ouverture réactive la fenêtre existante
    const QString serverName = QString("gdesk-%1").arg(getuid());
    {
        QLocalSocket sock;
        sock.connectToServer(serverName);
        if (sock.waitForConnected(300)) {
            sock.write((mailto.isEmpty() ? QString("show") : mailto).toUtf8());
            sock.waitForBytesWritten(500);
            sock.disconnectFromServer();
            return 0;
        }
    }
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(serverName);

    MainWindow win;
    QObject::connect(&server, &QLocalServer::newConnection, &win, [&server, &win] {
        QLocalSocket *conn = server.nextPendingConnection();
        conn->waitForReadyRead(500);
        const QString msg = QString::fromUtf8(conn->readAll());
        conn->deleteLater();
        if (msg.startsWith("mailto:"))
            win.composeMailto(QUrl(msg));
        else
            win.bringToFront();
    });

    if (!mailto.isEmpty())
        win.composeMailto(QUrl(mailto));
    else if (!(args.contains("--minimized") && win.hasTray()))
        win.show();
    return app.exec();
}
