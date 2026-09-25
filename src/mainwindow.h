#pragma once
#include "composer.h"
#include "mime.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QSettings>
#include <QSystemTrayIcon>
#include <functional>

class GmailApi;
class GoogleAuth;
class MessageView;
class QAction;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QSplitter;
class RowDelegate;
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

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    // Interface
    void buildActions();
    QWidget *buildLoginPage();
    QWidget *buildMailPage();
    QMenu *buildAccountMenu();
    void setupTray();
    void updateActions();
    void openSettings();
    void applySettings();
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
    void updateRowStyle(QTreeWidgetItem *item);
    void updateRowText(QTreeWidgetItem *item);
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
    RowDelegate *m_rowDelegate;
    QTreeWidget *m_folders;
    QTreeWidget *m_list;
    MessageView *m_view;
    QLineEdit *m_search;
    QToolButton *m_accountButton;

    QAction *m_actNew, *m_actReply, *m_actReplyAll, *m_actForward;
    QAction *m_actArchive, *m_actDelete, *m_actRestore, *m_actSpam, *m_actRead, *m_actStar, *m_actRefresh;

    // Réglages (voir SettingsDialog)
    QString m_layout = "below";
    QString m_markReadMode = "immediate";
    QString m_remoteMode = "ask";
    bool m_closeToTray = true;
    bool m_notificationsOn = true;
    bool m_showSnippet = true;

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
