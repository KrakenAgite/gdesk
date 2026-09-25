#pragma once
#include "composer.h"
#include "mime.h"
#include "settingsdialog.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QSettings>
#include <QSystemTrayIcon>
#include <functional>
#include <memory>

class GmailApi;
class GoogleAuth;
class MessageView;
class QAction;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QSplitter;
class AccountChip;
class MailListDelegate;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();

    void composeMailto(const QUrl &mailto);
    void bringToFront();
    bool hasTray() const { return m_tray != nullptr; }
    void applySettings(); // relit les réglages et les applique à la fenêtre
    void populateFolders(const QJsonObject &labels); // barre latérale à partir de labels.list
    void restoreStartFolder(); // boîte à ouvrir au démarrage, d'après les paramètres
    QMenu *folderMenu(const QString &folderId); // clic droit sur une boîte

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Interface
    void buildActions();
    QWidget *buildLoginPage();
    QWidget *buildMailPage();
    QMenu *buildAccountMenu();
    void setupTray();
    void updateActions();
    void openSettings();
    void saveSplitters();

    // Connexion
    void configureClient();
    void startLogin();
    void onLoggedIn();
    void showLoginPage(const QString &message, bool busy = false);
    void logout();
    void switchAccount();
    void clearMailbox();

    // Dossiers
    void loadLabels();
    void refreshCounts();
    void scheduleCountsRefresh();
    void onFolderChanged();

    // Liste des messages
    void reloadList(const QString &keepSelected = {});
    void fetchPage(int generation, const QString &keepSelected);
    void fillRow(QTreeWidgetItem *item, const MailMessage &m);
    void removeRows(const QList<QTreeWidgetItem *> &items);
    void onSelectionChanged();

    // Lecture
    void openMessage(const QString &id);
    void markRead(const QString &id);
    void displayMessage(const MailMessage &m);
    void fetchAttachment(int index, std::function<void(const QByteArray &)> cb);
    void saveAttachment(int index);
    void openAttachment(int index);
    void rememberAddresses(const QString &addresses);

    // Actions
    void applyLabels(const QStringList &add, const QStringList &remove, bool removesFromView, const QString &done);
    void applyLabelsTo(const QList<QTreeWidgetItem *> &items, const QStringList &add, const QStringList &remove,
                       bool removesFromView, const QString &done);
    QList<QTreeWidgetItem *> targetItems() const; // messages cochés, sinon message(s) sélectionné(s)
    void markTargets(bool read);

    // Sélection par cases à cocher
    QWidget *buildSelectionBar();
    void setChecked(QTreeWidgetItem *item, bool on);
    void onCheckClicked(const QModelIndex &index, Qt::KeyboardModifiers modifiers);
    void checkWhere(const std::function<bool(const QStringList &labels)> &predicate);
    void clearChecks();
    void updateSelectionBar();
    void archive();
    void trash();
    void restore();
    void toggleSpam();
    void toggleRead();
    void toggleStar();
    void compose(Composer::Mode mode);
    Composer *newComposer();

    // Nouveaux messages, barre système
    void checkNewMail();
    QString folderName(const QString &id) const;
    void selectAllMessages(const QString &folderId);
    void markAllInFolder(const QString &folderId, bool read);
    void emptyFolder(const QString &folderId);
    void collectMessageIds(const QStringList &labels, const QString &query, const QString &pageToken,
                           std::shared_ptr<QStringList> ids,
                           std::function<void(const QStringList &, const QString &)> done);
    void batchModifyAll(const QStringList &ids, const QStringList &add, const QStringList &remove,
                        std::function<void(int done, const QString &error)> finished, int offset = 0);
    void applyLabelsToRows(const QStringList &ids, const QStringList &add, const QStringList &remove);
    QList<LabelChoice> userLabelChoices() const;
    void notify(const QString &title, const QString &text);
    void setUnread(int count);
    QIcon badgeIcon(int count) const;
    void updateLauncherBadge(int count);
    void about();
    void quitApp();
    void showError(const QString &what, const QString &err);

    QSettings m_settings;
    QNetworkAccessManager *m_nam;
    GoogleAuth *m_auth;
    GmailApi *m_api;
    QString m_email;

    QStackedWidget *m_pages;
    QLabel *m_loginIcon, *m_loginStatus;
    QPushButton *m_loginButton, *m_setupButton, *m_cancelButton;

    QSplitter *m_splitter;
    QSplitter *m_rightSplitter;
    MailListDelegate *m_listDelegate;
    QTreeWidget *m_folders;
    QTreeWidget *m_list;
    MessageView *m_view;
    QLineEdit *m_search;
    AccountChip *m_accountChip;
    QMenu *m_accountMenu;

    QAction *m_actNew, *m_actReply, *m_actReplyAll, *m_actForward;
    QAction *m_actArchive, *m_actDelete, *m_actRestore, *m_actSpam, *m_actRead, *m_actStar, *m_actRefresh;

    // Réglages (voir SettingsDialog)
    QString m_layout = "below";
    QString m_markReadMode = "immediate";
    QString m_remoteMode = "ask";
    bool m_closeToTray = true;
    bool m_notificationsOn = true;
    QStringList m_notifyFolders;      // boîtes dont les nouveaux messages sont notifiés
    QString m_notifyFolder = "INBOX"; // boîte ouverte au clic sur la dernière notification
    int m_pollGeneration = 0;
    bool m_bulkBusy = false;
    QSet<QString> m_checked;          // messages cochés
    int m_lastCheckedRow = -1;        // pour Maj+clic
    class QCheckBox *m_masterCheck = nullptr;
    QLabel *m_selectionLabel = nullptr;
    QWidget *m_selectionActions = nullptr;
    QList<QToolButton *> m_selectionButtons;
    QToolButton *m_selArchive = nullptr, *m_selSpam = nullptr, *m_selDelete = nullptr, *m_selRestore = nullptr;         // opération en masse en cours (tout marquer, vider)
    bool m_selectAllPending = false; // tout sélectionner dès que la liste est chargée

    QString m_currentLabel = "INBOX";
    QString m_query;
    QString m_nextPageToken;
    QString m_openId;
    int m_generation = 0;
    bool m_loadingList = false;
    bool m_restoringSelection = false;
    QHash<QString, QTreeWidgetItem *> m_rows;
    QHash<QString, QTreeWidgetItem *> m_folderItems;

    QStringList m_knownAddresses;
    QSet<QString> m_knownUnread;
    bool m_unreadSeeded = false;
    int m_unread = -1;
    QTimer *m_pollTimer;
    QTimer *m_countsTimer;

    QSystemTrayIcon *m_tray = nullptr;
    QIcon m_baseIcon;
    bool m_quitting = false;
};
