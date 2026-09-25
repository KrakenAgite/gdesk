#pragma once
#include "driveapi.h"

#include <QWidget>

class DriveBrowser;
class DrivePlaces;
class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTimer;

// Vue « Drive » de la fenêtre principale : lieux et espace de stockage à gauche,
// fichiers à droite. Ouvrir, télécharger, importer (aussi par glisser-déposer),
// renommer, suivre, supprimer, envoyer par e-mail.
class DriveView : public QWidget
{
    Q_OBJECT
public:
    explicit DriveView(DriveApi *api, QWidget *parent = nullptr);

    void activate();  // à chaque affichage : premier chargement, focus sur la liste
    void reloadAll(); // après une nouvelle autorisation Google
    DriveBrowser *browser() const { return m_browser; }
    void uploadPaths(const QStringList &paths, const DriveFile &folder);

signals:
    void statusMessage(const QString &text, int timeout = 0);
    void sendByMailRequested(const QList<DriveFile> &files);

private:
    QWidget *buildSidebar();
    QWidget *buildToolBar();
    void updateActions();
    void showContextMenu(const QPoint &pos);
    void refreshQuota();

    void openFile(const DriveFile &file);
    void downloadSelected();
    void downloadNext(const QList<QPair<DriveFile, QString>> &queue, int index);
    void uploadNext(const QStringList &queue, const DriveFile &folder, int index);
    void importFiles();
    void newFolder();
    void rename();
    void toggleStar();
    void setTrashed(bool trashed);
    QList<DriveFile> downloadableSelection() const;

    DriveApi *m_api;
    DriveBrowser *m_browser;
    DrivePlaces *m_places;
    QLineEdit *m_search;
    QLabel *m_quotaLabel;
    QProgressBar *m_quotaBar;
    QTimer *m_reloadTimer;
    bool m_loaded = false;
    int m_lastPlace = DriveApi::MyDrive;

    QAction *m_actOpen, *m_actOpenWeb, *m_actDownload, *m_actMail, *m_actLink, *m_actRename, *m_actStar;
    QAction *m_actTrash, *m_actRestore, *m_actNewFolder, *m_actUpload, *m_actRefresh;
};
