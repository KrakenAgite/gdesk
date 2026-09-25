// Outil de compilation : génère les icônes PNG (thème hicolor) à partir de l'icône SVG,
// avec le moteur SVG de Qt (le même que celui de KDE).
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc < 4) {
        qCritical("usage : rasterize-icon <icone.svg> <dossier-sortie> <taille>...");
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        const int size = QString(argv[i]).toInt();
        QImageReader reader(QString::fromLocal8Bit(argv[1]));
        reader.setScaledSize(QSize(size, size));
        const QImage image = reader.read();
        const QString dir = QString("%1/%2x%2/apps").arg(QString::fromLocal8Bit(argv[2])).arg(size);
        if (image.isNull() || !QDir().mkpath(dir) || !image.save(dir + "/gdesk.png")) {
            qCritical("échec pour la taille %d : %s", size, qPrintable(reader.errorString()));
            return 1;
        }
    }
    return 0;
}
