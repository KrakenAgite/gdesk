#pragma once
#include "driveapi.h"

#include <QDialog>
#include <QTreeWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QHBoxLayout;

// Lieux de Drive (Mon Drive, Récents, Suivis…) dans le style de la barre latérale du courrier
class DrivePlaces : public QTreeWidget
{
    Q_OBJECT
public:
    explicit DrivePlaces(bool foldersOnly, QWidget *parent = nullptr);
    void setCurrentPlace(int place); // -1 : aucun (recherche)
    static QString title(int place);

signals:
    void placeChosen(int place);

protected:
    void mousePressEvent(QMouseEvent *event) override;
};

// Liste des fichiers d'un lieu ou d'un dossier Drive, avec fil d'Ariane, pagination et
// messages d'erreur (autorisation manquante, API non activée…). Utilisée par la vue Drive
// et par le sélecteur de fichiers.
class DriveBrowser : public QWidget
{
    Q_OBJECT
public:
    enum Mode { Browse, PickFiles, PickFolder };
    enum Column { NameColumn, OwnerColumn, ModifiedColumn, SizeColumn };
    enum { FileRole = Qt::UserRole + 1 }; // DriveFile de la ligne

    DriveBrowser(DriveApi *api, Mode mode, QWidget *parent = nullptr);

    void setPlace(int place);
    int place() const { return m_place; }
    void search(const QString &text);
    QString searchText() const { return m_search; }
    void openFolder(const DriveFile &folder);
    void goUp();
    void reload();

    QList<DriveFile> selectedFiles() const;
    // Dossier affiché (« root » pour Mon Drive) ; vide pour une liste comme Récents ou une recherche
    DriveFile currentFolder() const;
    bool canAddHere() const; // on peut créer ou importer des fichiers dans ce dossier
    void updateFile(const DriveFile &file); // après un renommage, un ajout aux suivis…
    void removeFiles(const QStringList &ids); // placés dans la corbeille, restaurés…
    QTreeWidget *list() const { return m_list; }

    // Emplacement mémorisé (lieu + chemin), pour rouvrir le sélecteur au même endroit
    QVariant saveLocation() const;
    void restoreLocation(const QVariant &state);

    // Fichiers déposés depuis le gestionnaire de fichiers (vue Drive uniquement)
    void setDropHandler(std::function<void(const QStringList &paths, const DriveFile &folder)> handler);

    static QIcon fileIcon(const DriveFile &file);

signals:
    void fileActivated(const DriveFile &file); // double-clic ou Entrée sur un fichier
    void selectionChanged();
    void locationChanged();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void fetch();
    void addFile(const DriveFile &file);
    void fillItem(QTreeWidgetItem *item, const DriveFile &file);
    void updateCrumbs();
    void showList();
    void showNotice(const QString &icon, const QString &text, const QString &actionText = {},
                    std::function<void()> action = {}, bool retry = false);
    void showError(const QString &error, const QJsonObject &details);
    void showEmpty();
    QTreeWidgetItem *folderItemAt(const QPoint &pos) const;

    DriveApi *m_api;
    Mode m_mode;
    int m_place = DriveApi::MyDrive;
    QString m_search;
    QList<DriveFile> m_path; // dossiers ouverts sous le lieu
    QString m_nextPageToken;
    int m_generation = 0;
    bool m_loading = false;
    bool m_sorted = false; // l'utilisateur a trié la liste en cliquant un en-tête

    QHBoxLayout *m_crumbs;
    QToolButton *m_upButton;
    QStackedWidget *m_stack;
    QTreeWidget *m_list;
    QLabel *m_noticeIcon, *m_noticeText;
    QPushButton *m_noticeAction, *m_noticeRetry;
    std::function<void()> m_noticeHandler;
    std::function<void(const QStringList &, const DriveFile &)> m_dropHandler;
};

// Sélecteur Drive : des fichiers à joindre, ou un dossier où enregistrer
class DrivePicker : public QDialog
{
    Q_OBJECT
public:
    enum Kind { Files, Folder };
    DrivePicker(DriveApi *api, Kind kind, QWidget *parent = nullptr);

    QList<DriveFile> files() const; // fichiers choisis (hors dossiers)
    DriveFile folder() const;       // dossier de destination
    void setAcceptText(const QString &text);

    void accept() override;

private:
    void updateButtons();

    DriveApi *m_api;
    Kind m_kind;
    DriveBrowser *m_browser;
    DrivePlaces *m_places;
    QLineEdit *m_search;
    QLabel *m_destination = nullptr;
    QPushButton *m_ok;
};
