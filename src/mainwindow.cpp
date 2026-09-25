#include "mainwindow.h"
#include "gmailapi.h"
#include "googleauth.h"
#include "messageview.h"
#include "setupdialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <memory>

namespace {
enum Roles { IdRole = Qt::UserRole, LabelsRole, NameRole };
enum Columns { ColStar, ColWho, ColSubject, ColDate };

struct SystemLabel {
    const char *id, *name, *icon;
};
const SystemLabel systemLabels[] = {
    {"INBOX", "Boîte de réception", "mail-folder-inbox"},
    {"STARRED", "Suivis", "rating"},
    {"IMPORTANT", "Importants", "mail-mark-important"},
    {"SENT", "Messages envoyés", "mail-folder-sent"},
    {"DRAFT", "Brouillons", "document-edit"},
    {"", "Tous les messages", "mail-folder-outbox"},
    {"SPAM", "Spam", "mail-mark-junk"},
    {"TRASH", "Corbeille", "user-trash"},
};
const SystemLabel categories[] = {
    {"CATEGORY_PERSONAL", "Principale", "mail-folder-inbox"},
    {"CATEGORY_SOCIAL", "Réseaux sociaux", "system-users"},
    {"CATEGORY_PROMOTIONS", "Promotions", "tag"},
    {"CATEGORY_UPDATES", "Notifications", "dialog-information"},
    {"CATEGORY_FORUMS", "Forums", "im-user"},
};
} // namespace

MainWindow::MainWindow()
    : m_settings("gdesk", "gdesk"),
      m_nam(new QNetworkAccessManager(this))
{
    m_auth = new GoogleAuth(m_nam, this);
    m_api = new GmailApi(m_auth, m_nam, this);
    m_baseIcon = QIcon::fromTheme("gdesk", QIcon(":/gdesk.svg"));
    m_knownAddresses = m_settings.value("known_addresses").toStringList();

    setWindowTitle("G-Desk");
    setWindowIcon(m_baseIcon);
    resize(1300, 820);
    if (m_settings.contains("geometry"))
        restoreGeometry(m_settings.value("geometry").toByteArray());

    buildActions();
    m_pages = new QStackedWidget;
    m_pages->addWidget(buildLoginPage());
    m_pages->addWidget(buildMailPage());
    setCentralWidget(m_pages);
    setupTray();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(60 * 1000);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::checkNewMail);
    m_countsTimer = new QTimer(this);
    m_countsTimer->setSingleShot(true);
    m_countsTimer->setInterval(800);
    connect(m_countsTimer, &QTimer::timeout, this, &MainWindow::refreshCounts);

    connect(m_auth, &GoogleAuth::loggedIn, this, &MainWindow::onLoggedIn);
    connect(m_auth, &GoogleAuth::loginFailed, this, [this](const QString &err) {
        showLoginPage("La connexion a échoué : " + err);
        bringToFront();
    });
    connect(m_auth, &GoogleAuth::sessionExpired, this, [this] {
        m_pollTimer->stop();
        showLoginPage("Votre session Google a expiré. Reconnectez-vous.");
    });

    m_auth->setClient(m_settings.value("client_id").toString(), m_settings.value("client_secret").toString());
    if (!m_auth->hasClient()) {
        showLoginPage("Bienvenue ! Avant la première connexion, G-Desk a besoin de vos identifiants Google Cloud.\n"
                      "Cliquez sur « Se connecter avec Google » pour suivre l'assistant.");
    } else {
        showLoginPage("Connexion…", true);
        m_auth->loadStoredToken([this](bool found) {
            if (found)
                onLoggedIn();
            else
                showLoginPage({});
        });
    }
}

// =============================================================================
//  Construction de l'interface
// =============================================================================
void MainWindow::buildActions()
{
    auto make = [this](const char *icon, const QString &text, const QKeySequence &key, auto slot) {
        auto *a = new QAction(QIcon::fromTheme(icon), text, this);
        if (!key.isEmpty()) {
            a->setShortcut(key);
            a->setToolTip(QString("%1 (%2)").arg(text, key.toString(QKeySequence::NativeText)));
        }
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    m_actNew = make("mail-message-new", "Nouveau", QKeySequence("Ctrl+N"), [this] { compose(Composer::New); });
    m_actReply = make("mail-reply-sender", "Répondre", QKeySequence("Ctrl+R"), [this] { compose(Composer::Reply); });
    m_actReplyAll = make("mail-reply-all", "Répondre à tous", QKeySequence("Ctrl+Shift+R"),
                         [this] { compose(Composer::ReplyAll); });
    m_actForward = make("mail-forward", "Transférer", QKeySequence("Ctrl+L"), [this] { compose(Composer::Forward); });
    m_actArchive = make("archive-insert", "Archiver", QKeySequence("A"), [this] { archive(); });
    m_actDelete = make("edit-delete", "Supprimer", QKeySequence::Delete, [this] { trash(); });
    m_actRestore = make("edit-undo", "Restaurer", QKeySequence(), [this] { restore(); });
    m_actSpam = make("mail-mark-junk", "Spam", QKeySequence("J"), [this] { toggleSpam(); });
    m_actRead = make("mail-mark-read", "Lu / non lu", QKeySequence("M"), [this] { toggleRead(); });
    m_actStar = make("rating", "Suivi", QKeySequence("S"), [this] { toggleStar(); });
    m_actRefresh = make("view-refresh", "Actualiser", QKeySequence::Refresh, [this] {
        loadLabels();
        reloadList(m_openId);
    });

    m_actCloseToTray = new QAction("Garder dans la barre système à la fermeture", this);
    m_actCloseToTray->setCheckable(true);
    m_actCloseToTray->setChecked(m_settings.value("close_to_tray", true).toBool());
    connect(m_actCloseToTray, &QAction::toggled, this, [this](bool v) { m_settings.setValue("close_to_tray", v); });
    m_actNotifications = new QAction("Notifications des nouveaux messages", this);
    m_actNotifications->setCheckable(true);
    m_actNotifications->setChecked(m_settings.value("notifications", true).toBool());
    connect(m_actNotifications, &QAction::toggled, this, [this](bool v) { m_settings.setValue("notifications", v); });

    auto *quit = new QAction(this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::quitApp);
    addAction(quit);
}

QWidget *MainWindow::buildLoginPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addStretch(2);
    m_loginIcon = new QLabel;
    m_loginIcon->setPixmap(m_baseIcon.pixmap(96, 96));
    m_loginIcon->setAlignment(Qt::AlignCenter);
    v->addWidget(m_loginIcon);
    auto *title = new QLabel("<h1>G-Desk</h1><p>Client mail pour Gmail</p>");
    title->setAlignment(Qt::AlignCenter);
    v->addWidget(title);
    m_loginStatus = new QLabel;
    m_loginStatus->setAlignment(Qt::AlignCenter);
    m_loginStatus->setWordWrap(true);
    v->addWidget(m_loginStatus);
    v->addSpacing(12);

    auto *buttons = new QVBoxLayout;
    buttons->setAlignment(Qt::AlignHCenter);
    m_loginButton = new QPushButton(QIcon::fromTheme("im-google", QIcon::fromTheme("go-next")),
                                    "Se connecter avec Google");
    m_loginButton->setMinimumSize(280, 40);
    m_loginButton->setDefault(true);
    connect(m_loginButton, &QPushButton::clicked, this, [this] {
        if (m_auth->isLoggedIn())
            onLoggedIn(); // « Réessayer » après une erreur réseau
        else
            startLogin();
    });
    m_cancelButton = new QPushButton("Annuler");
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        m_auth->cancelLogin();
        showLoginPage({});
    });
    m_setupButton = new QPushButton(QIcon::fromTheme("configure"), "Identifiants Google Cloud…");
    connect(m_setupButton, &QPushButton::clicked, this, &MainWindow::configureClient);
    buttons->addWidget(m_loginButton);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_setupButton);
    v->addLayout(buttons);
    v->addStretch(3);
    return page;
}

QWidget *MainWindow::buildMailPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *bar = new QToolBar;
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->setIconSize(QSize(20, 20));
    bar->addAction(m_actNew);
    bar->addSeparator();
    for (QAction *a : {m_actReply, m_actReplyAll, m_actForward})
        bar->addAction(a);
    bar->addSeparator();
    for (QAction *a : {m_actArchive, m_actDelete, m_actRestore, m_actSpam, m_actRead, m_actStar})
        bar->addAction(a);
    for (QAction *a : {m_actReplyAll, m_actSpam, m_actRead, m_actStar})
        if (auto *w = qobject_cast<QToolButton *>(bar->widgetForAction(a)))
            w->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->addSeparator();
    bar->addAction(m_actRefresh);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    m_search = new QLineEdit;
    m_search->setPlaceholderText("Rechercher (ex. : from:paul has:attachment)");
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(280);
    m_search->addAction(QIcon::fromTheme("search"), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        m_query = m_search->text().trimmed();
        reloadList();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (t.isEmpty() && !m_query.isEmpty()) {
            m_query.clear();
            reloadList();
        }
    });
    auto *focusSearch = new QAction(this);
    focusSearch->setShortcut(QKeySequence::Find);
    connect(focusSearch, &QAction::triggered, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    addAction(focusSearch);
    bar->addWidget(m_search);

    m_accountButton = new QToolButton;
    m_accountButton->setIcon(QIcon::fromTheme("user-identity"));
    m_accountButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_accountButton->setPopupMode(QToolButton::InstantPopup);
    m_accountButton->setMenu(buildAccountMenu());
    bar->addWidget(m_accountButton);
    v->addWidget(bar);

    // --- dossiers ---
    m_folders = new QTreeWidget;
    m_folders->setHeaderHidden(true);
    m_folders->setIconSize(QSize(18, 18));
    m_folders->setRootIsDecorated(true);
    m_folders->setMinimumWidth(170);
    connect(m_folders, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur && cur->data(0, IdRole).isValid())
            onFolderChanged();
    });

    // --- liste ---
    m_list = new QTreeWidget;
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({"", "De", "Objet", "Date"});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAllColumnsShowFocus(true);
    m_list->setTextElideMode(Qt::ElideRight);
    QHeaderView *hdr = m_list->header();
    hdr->setStretchLastSection(false);
    hdr->setSectionResizeMode(ColStar, QHeaderView::Fixed);
    hdr->resizeSection(ColStar, 28);
    hdr->resizeSection(ColWho, 180);
    hdr->setSectionResizeMode(ColSubject, QHeaderView::Stretch);
    hdr->setSectionResizeMode(ColDate, QHeaderView::ResizeToContents);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column == ColStar) {
            m_list->setCurrentItem(item);
            toggleStar();
        }
    });
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value >= m_list->verticalScrollBar()->maximum() - 5 && !m_loadingList && !m_nextPageToken.isEmpty())
            fetchPage(m_generation, {});
    });
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);
    for (QAction *a : {m_actReply, m_actReplyAll, m_actForward, m_actArchive, m_actDelete, m_actRestore,
                       m_actSpam, m_actRead, m_actStar})
        m_list->addAction(a);

    // --- lecture ---
    m_view = new MessageView;
    connect(m_view, &MessageView::saveAttachmentRequested, this, &MainWindow::saveAttachment);
    connect(m_view, &MessageView::openAttachmentRequested, this, &MainWindow::openAttachment);
    connect(m_view, &MessageView::mailtoClicked, this, &MainWindow::composeMailto);
    connect(m_view, &MessageView::remoteContentAllowed, this, [this](bool always) {
        MailMessage m = m_view->message();
        if (always) {
            QStringList trusted = m_settings.value("trusted_senders").toStringList();
            trusted << Mime::emailOnly(m.from).toLower();
            trusted.removeDuplicates();
            m_settings.setValue("trusted_senders", trusted);
        }
        m_view->showMessage(m, true);
    });

    auto *right = new QSplitter(Qt::Vertical);
    right->addWidget(m_list);
    right->addWidget(m_view);
    right->setStretchFactor(0, 2);
    right->setStretchFactor(1, 3);
    right->setObjectName("rightSplitter");

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(m_folders);
    m_splitter->addWidget(right);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({220, 1080});
    if (m_settings.contains("splitter"))
        m_splitter->restoreState(m_settings.value("splitter").toByteArray());
    if (m_settings.contains("splitter_right"))
        right->restoreState(m_settings.value("splitter_right").toByteArray());
    v->addWidget(m_splitter, 1);

    statusBar()->setSizeGripEnabled(false);
    updateActions();
    return page;
}

QMenu *MainWindow::buildAccountMenu()
{
    auto *menu = new QMenu(this);
    menu->addAction(m_actCloseToTray);
    menu->addAction(m_actNotifications);
    auto *autostart = menu->addAction("Lancer à l'ouverture de session");
    autostart->setCheckable(true);
    autostart->setChecked(QFile::exists(autostartPath()));
    connect(autostart, &QAction::toggled, this, &MainWindow::setAutostart);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme("internet-web-browser"), "Ouvrir Gmail dans le navigateur", this,
                    [] { QDesktopServices::openUrl(QUrl("https://mail.google.com/")); });
    menu->addAction(QIcon::fromTheme("configure"), "Identifiants Google Cloud…", this, &MainWindow::configureClient);
    menu->addAction(QIcon::fromTheme("system-log-out"), "Se déconnecter", this, &MainWindow::logout);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme("help-about"), "À propos", this, &MainWindow::about);
    menu->addAction(QIcon::fromTheme("application-exit"), "Quitter", this, &MainWindow::quitApp);
    return menu;
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    m_tray = new QSystemTrayIcon(m_baseIcon, this);
    m_tray->setToolTip("G-Desk");
    auto *menu = new QMenu(this);
    menu->addAction("Afficher", this, &MainWindow::bringToFront);
    menu->addAction(QIcon::fromTheme("mail-message-new"), "Nouveau message", this, [this] { compose(Composer::New); });
    menu->addAction(QIcon::fromTheme("view-refresh"), "Relever le courrier", this, &MainWindow::checkNewMail);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme("application-exit"), "Quitter", this, &MainWindow::quitApp);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger)
            return;
        if (isVisible() && isActiveWindow())
            hide();
        else
            bringToFront();
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
        bringToFront();
        if (auto *inbox = m_folderItems.value("INBOX"))
            m_folders->setCurrentItem(inbox);
    });
    m_tray->show();
}

void MainWindow::updateActions()
{
    const int selected = m_list ? m_list->selectedItems().size() : 0;
    const bool single = selected == 1 && !m_view->message().id.isEmpty();
    const bool inTrash = m_currentLabel == "TRASH" && m_query.isEmpty();
    const bool inSpam = m_currentLabel == "SPAM" && m_query.isEmpty();
    m_actReply->setEnabled(single);
    m_actReplyAll->setEnabled(single);
    m_actForward->setEnabled(single);
    m_actArchive->setEnabled(selected > 0 && !inTrash && !inSpam);
    m_actDelete->setEnabled(selected > 0 && !inTrash);
    m_actRestore->setVisible(inTrash);
    m_actRestore->setEnabled(selected > 0);
    m_actSpam->setEnabled(selected > 0 && !inTrash);
    m_actSpam->setText(inSpam ? "Non spam" : "Spam");
    m_actSpam->setIcon(QIcon::fromTheme(inSpam ? "mail-mark-notjunk" : "mail-mark-junk"));
    m_actRead->setEnabled(selected > 0);
    m_actStar->setEnabled(selected > 0);
}

// =============================================================================
//  Connexion
// =============================================================================
void MainWindow::configureClient()
{
    SetupDialog dlg(m_settings.value("client_id").toString(), m_settings.value("client_secret").toString(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const bool changed = dlg.clientId() != m_settings.value("client_id").toString();
    m_settings.setValue("client_id", dlg.clientId());
    m_settings.setValue("client_secret", dlg.clientSecret());
    m_auth->setClient(dlg.clientId(), dlg.clientSecret());
    if (changed && m_auth->isLoggedIn())
        m_auth->logout(); // les jetons sont liés à l'ancien client
    if (!m_auth->isLoggedIn())
        startLogin();
}

void MainWindow::startLogin()
{
    if (!m_auth->hasClient()) {
        configureClient();
        return;
    }
    showLoginPage("Terminez la connexion dans votre navigateur…\n"
                  "(Si Google affiche « Google n'a pas validé cette application », cliquez sur "
                  "« Paramètres avancés » puis « Accéder à G-Desk » : c'est votre propre projet.)",
                  true);
    m_cancelButton->setVisible(true);
    m_auth->login();
}

void MainWindow::showLoginPage(const QString &message, bool busy)
{
    m_pages->setCurrentIndex(0);
    m_loginStatus->setText(message);
    m_loginButton->setVisible(!busy);
    // Sans identifiants, le bouton ouvre l'assistant de configuration (voir startLogin)
    m_loginButton->setText(m_auth->isLoggedIn() ? "Réessayer" : "Se connecter avec Google");
    m_setupButton->setVisible(!busy);
    m_cancelButton->setVisible(false);
    setUnread(0);
}

void MainWindow::onLoggedIn()
{
    showLoginPage("Connexion à Gmail…", true);
    m_api->getProfile([this](const QJsonObject &profile, const QString &err) {
        if (!err.isEmpty()) {
            showLoginPage("Impossible de contacter Gmail : " + err);
            return;
        }
        m_email = profile.value("emailAddress").toString();
        m_accountButton->setText(m_email);
        m_pages->setCurrentIndex(1);
        m_unreadSeeded = false;
        m_knownUnread.clear();
        m_currentLabel = "INBOX";
        loadLabels();
        checkNewMail();
        m_pollTimer->start();
    });
}

void MainWindow::logout()
{
    if (QMessageBox::question(this, "Se déconnecter",
                              "Se déconnecter de " + m_email + " ?\nG-Desk n'aura plus accès à vos messages.")
        != QMessageBox::Yes)
        return;
    m_pollTimer->stop();
    m_auth->logout();
    m_email.clear();
    m_list->clear();
    m_rows.clear();
    m_folders->clear();
    m_folderItems.clear();
    m_view->clear();
    showLoginPage("Vous êtes déconnecté.");
}

// =============================================================================
//  Dossiers
// =============================================================================
void MainWindow::loadLabels()
{
    m_api->listLabels([this](const QJsonObject &obj, const QString &err) {
        if (!err.isEmpty()) {
            showError("Impossible de charger les dossiers", err);
            return;
        }
        QSignalBlocker block(m_folders);
        m_folders->clear();
        m_folderItems.clear();

        auto addItem = [this](QTreeWidgetItem *parent, const QString &id, const QString &name, const QIcon &icon) {
            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_folders);
            item->setText(0, name);
            item->setIcon(0, icon);
            item->setData(0, IdRole, id);
            item->setData(0, NameRole, name);
            m_folderItems.insert(id, item);
            return item;
        };
        for (const SystemLabel &l : systemLabels)
            addItem(nullptr, l.id, l.name, QIcon::fromTheme(l.icon, QIcon::fromTheme("folder")));

        auto *catRoot = new QTreeWidgetItem(m_folders, {"Catégories"});
        catRoot->setIcon(0, QIcon::fromTheme("folder-tag", QIcon::fromTheme("folder")));
        catRoot->setFlags(Qt::ItemIsEnabled);
        for (const SystemLabel &l : categories)
            addItem(catRoot, l.id, l.name, QIcon::fromTheme(l.icon, QIcon::fromTheme("folder")));

        // Libellés personnels, avec sous-libellés « Parent/Enfant »
        QList<QPair<QString, QString>> userLabels;
        for (const QJsonValue &v : obj.value("labels").toArray()) {
            const QJsonObject l = v.toObject();
            if (l.value("type").toString() == "user")
                userLabels.append({l.value("name").toString(), l.value("id").toString()});
        }
        std::sort(userLabels.begin(), userLabels.end(), [](const auto &a, const auto &b) {
            return QString::localeAwareCompare(a.first, b.first) < 0;
        });
        if (!userLabels.isEmpty()) {
            auto *root = new QTreeWidgetItem(m_folders, {"Libellés"});
            root->setIcon(0, QIcon::fromTheme("tag", QIcon::fromTheme("folder")));
            root->setFlags(Qt::ItemIsEnabled);
            QHash<QString, QTreeWidgetItem *> byPath;
            for (const auto &[path, id] : userLabels) {
                const QString parentPath = path.section('/', 0, -2);
                QTreeWidgetItem *parent = byPath.value(parentPath, root);
                byPath.insert(path, addItem(parent, id, path.section('/', -1), QIcon::fromTheme("tag")));
            }
            root->setExpanded(true);
        }

        QTreeWidgetItem *current = m_folderItems.value(m_currentLabel, m_folderItems.value("INBOX"));
        m_folders->setCurrentItem(current);
        block.unblock();
        refreshCounts();
        if (m_list->topLevelItemCount() == 0)
            onFolderChanged();
    });
}

void MainWindow::scheduleCountsRefresh()
{
    m_countsTimer->start();
}

void MainWindow::refreshCounts()
{
    for (auto it = m_folderItems.cbegin(); it != m_folderItems.cend(); ++it) {
        const QString id = it.key();
        if (id.isEmpty() || id == "SENT" || id == "TRASH" || id == "STARRED" || id == "IMPORTANT")
            continue;
        m_api->getLabel(id, [this, id](const QJsonObject &l, const QString &err) {
            QTreeWidgetItem *item = m_folderItems.value(id);
            if (!err.isEmpty() || !item)
                return;
            const int count = (id == "DRAFT" ? l.value("messagesTotal") : l.value("messagesUnread")).toInt();
            const QString name = item->data(0, NameRole).toString();
            item->setText(0, count > 0 ? QString("%1 (%2)").arg(name).arg(count) : name);
            QFont f = item->font(0);
            f.setBold(count > 0 && id != "DRAFT");
            item->setFont(0, f);
            if (id == "INBOX")
                setUnread(count);
        });
    }
}

void MainWindow::onFolderChanged()
{
    QTreeWidgetItem *item = m_folders->currentItem();
    if (!item)
        return;
    m_currentLabel = item->data(0, IdRole).toString();
    m_query.clear();
    {
        QSignalBlocker b(m_search);
        m_search->clear();
    }
    m_list->headerItem()->setText(ColWho, (m_currentLabel == "SENT" || m_currentLabel == "DRAFT") ? "À" : "De");
    reloadList();
}

// =============================================================================
//  Liste des messages
// =============================================================================
void MainWindow::reloadList(const QString &keepSelected)
{
    ++m_generation;
    m_nextPageToken.clear();
    m_loadingList = false;
    {
        QSignalBlocker b(m_list);
        m_list->clear();
    }
    m_rows.clear();
    if (keepSelected.isEmpty()) {
        m_openId.clear();
        m_view->clear();
    }
    updateActions();
    fetchPage(m_generation, keepSelected);
}

void MainWindow::fetchPage(int generation, const QString &keepSelected)
{
    m_loadingList = true;
    statusBar()->showMessage("Chargement…");
    const QStringList labels = (!m_query.isEmpty() || m_currentLabel.isEmpty()) ? QStringList() : QStringList{m_currentLabel};
    m_api->listMessages(labels, m_query, m_nextPageToken, 50,
                        [this, generation, keepSelected](const QJsonObject &obj, const QString &err) {
        if (generation != m_generation)
            return;
        m_loadingList = false;
        if (!err.isEmpty()) {
            showError("Impossible de charger les messages", err);
            return;
        }
        m_nextPageToken = obj.value("nextPageToken").toString();
        const QJsonArray messages = obj.value("messages").toArray();
        if (m_list->topLevelItemCount() == 0 && messages.isEmpty())
            statusBar()->showMessage(m_query.isEmpty() ? "Aucun message." : "Aucun résultat.");
        else if (!m_query.isEmpty())
            statusBar()->showMessage(QString("Résultats pour « %1 »").arg(m_query));
        else
            statusBar()->clearMessage();

        for (const QJsonValue &v : messages) {
            const QString id = v.toObject().value("id").toString();
            if (m_rows.contains(id))
                continue;
            auto *item = new QTreeWidgetItem(m_list);
            item->setData(0, IdRole, id);
            item->setText(ColSubject, "…");
            m_rows.insert(id, item);
            if (id == keepSelected) {
                m_restoringSelection = true;
                m_list->setCurrentItem(item);
                m_restoringSelection = false;
            }
            m_api->getMessage(id, false, [this, generation, id](const QJsonObject &o, const QString &e) {
                if (generation != m_generation || !e.isEmpty())
                    return;
                if (QTreeWidgetItem *it = m_rows.value(id))
                    fillRow(it, Mime::parseMessage(o));
            });
        }
    });
}

void MainWindow::fillRow(QTreeWidgetItem *item, const MailMessage &m)
{
    item->setData(0, LabelsRole, m.labelIds);
    const bool outgoing = m_currentLabel == "SENT" || m_currentLabel == "DRAFT";
    QStringList names;
    for (const QString &a : Mime::splitAddresses(outgoing ? m.to : m.from))
        names << Mime::displayName(a);
    item->setText(ColWho, names.isEmpty() ? QString("(inconnu)") : names.join(", "));
    item->setToolTip(ColWho, outgoing ? m.to : m.from);
    const QString subject = m.subject.isEmpty() ? QString("(sans objet)") : m.subject;
    item->setText(ColSubject, m.snippet.isEmpty() ? subject : subject + "  —  " + m.snippet);
    item->setToolTip(ColSubject, "<b>" + subject.toHtmlEscaped() + "</b><br>" + m.snippet.toHtmlEscaped());
    item->setText(ColDate, Mime::shortDate(m.date));
    item->setToolTip(ColDate, QLocale().toString(m.date.toLocalTime(), QLocale::LongFormat));
    updateRowStyle(item);
    rememberAddresses(m.from);
}

void MainWindow::updateRowStyle(QTreeWidgetItem *item)
{
    const QStringList labels = item->data(0, LabelsRole).toStringList();
    const bool unread = labels.contains("UNREAD");
    for (int c = ColWho; c <= ColDate; ++c) {
        QFont f = item->font(c);
        f.setBold(unread);
        item->setFont(c, f);
    }
    item->setIcon(ColStar, labels.contains("STARRED") ? QIcon::fromTheme("rating") : QIcon());
    item->setToolTip(ColStar, labels.contains("STARRED") ? "Suivi" : "Cliquer pour suivre");
}

void MainWindow::removeRows(const QList<QTreeWidgetItem *> &items)
{
    if (items.isEmpty())
        return;
    int nextIndex = m_list->indexOfTopLevelItem(items.first());
    {
        QSignalBlocker b(m_list);
        for (QTreeWidgetItem *it : items) {
            m_rows.remove(it->data(0, IdRole).toString());
            delete it;
        }
    }
    m_openId.clear();
    m_view->clear();
    if (m_list->topLevelItemCount() > 0) {
        nextIndex = qMin(nextIndex, m_list->topLevelItemCount() - 1);
        m_list->setCurrentItem(m_list->topLevelItem(nextIndex));
    }
    updateActions();
}

void MainWindow::onSelectionChanged()
{
    const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
    if (selected.size() == 1 && !m_restoringSelection) {
        const QString id = selected.first()->data(0, IdRole).toString();
        if (id != m_openId)
            openMessage(id);
    }
    updateActions();
}

// =============================================================================
//  Lecture d'un message
// =============================================================================
void MainWindow::openMessage(const QString &id)
{
    m_openId = id;
    m_api->getMessage(id, true, [this, id](const QJsonObject &obj, const QString &err) {
        if (id != m_openId)
            return;
        if (!err.isEmpty()) {
            showError("Impossible d'ouvrir le message", err);
            return;
        }
        MailMessage m = Mime::parseMessage(obj);
        if (QTreeWidgetItem *item = m_rows.value(id))
            fillRow(item, m);

        if (m.isUnread()) {
            m_api->modifyMessages({id}, {}, {"UNREAD"}, [this](const QJsonObject &, const QString &e) {
                if (e.isEmpty())
                    scheduleCountsRefresh();
            });
            m.labelIds.removeAll("UNREAD");
            if (QTreeWidgetItem *item = m_rows.value(id)) {
                item->setData(0, LabelsRole, m.labelIds);
                updateRowStyle(item);
            }
        }

        // Images intégrées (cid:) à télécharger avant l'affichage
        QList<int> pending;
        for (int i = 0; i < m.attachments.size(); ++i) {
            const Attachment &a = m.attachments.at(i);
            if (!a.contentId.isEmpty() && a.data.isEmpty() && !a.attachmentId.isEmpty()
                && m.html.contains("cid:" + a.contentId))
                pending << i;
        }
        if (pending.isEmpty()) {
            displayMessage(m);
            return;
        }
        auto shared = std::make_shared<MailMessage>(m);
        auto remaining = std::make_shared<int>(pending.size());
        for (int i : pending) {
            m_api->getAttachment(id, m.attachments.at(i).attachmentId,
                                 [this, id, i, shared, remaining](const QByteArray &data, const QString &) {
                                     shared->attachments[i].data = data;
                                     if (--*remaining == 0 && id == m_openId)
                                         displayMessage(*shared);
                                 });
        }
    });
}

void MainWindow::displayMessage(const MailMessage &m)
{
    const QStringList trusted = m_settings.value("trusted_senders").toStringList();
    m_view->showMessage(m, trusted.contains(Mime::emailOnly(m.from).toLower()));
    rememberAddresses(m.from + "," + m.to + "," + m.cc);
    updateActions();
}

void MainWindow::rememberAddresses(const QString &addresses)
{
    const QString me = m_email.toLower();
    bool changed = false;
    for (const QString &a : Mime::splitAddresses(addresses)) {
        const QString email = Mime::emailOnly(a).toLower();
        if (email.isEmpty() || email == me || !email.contains('@') || email.contains("noreply")
            || email.contains("no-reply"))
            continue;
        bool known = false;
        for (const QString &k : std::as_const(m_knownAddresses))
            if (Mime::emailOnly(k).toLower() == email) {
                known = true;
                break;
            }
        if (!known) {
            m_knownAddresses.prepend(a.trimmed());
            changed = true;
        }
    }
    if (changed) {
        while (m_knownAddresses.size() > 1000)
            m_knownAddresses.removeLast();
        m_settings.setValue("known_addresses", m_knownAddresses);
    }
}

void MainWindow::fetchAttachment(int index, std::function<void(const QByteArray &)> cb)
{
    const MailMessage &m = m_view->message();
    if (index < 0 || index >= m.attachments.size())
        return;
    const Attachment &a = m.attachments.at(index);
    if (!a.data.isEmpty()) {
        cb(a.data);
        return;
    }
    statusBar()->showMessage("Téléchargement de " + a.filename + "…");
    m_api->getAttachment(m.id, a.attachmentId, [this, cb](const QByteArray &data, const QString &err) {
        statusBar()->clearMessage();
        if (!err.isEmpty())
            showError("Téléchargement impossible", err);
        else
            cb(data);
    });
}

static QString safeFileName(QString name)
{
    name.replace('/', '_').replace('\\', '_');
    if (name.startsWith('.'))
        name.prepend('_');
    return name.isEmpty() ? QString("piece-jointe") : name;
}

void MainWindow::saveAttachment(int index)
{
    const QString name = safeFileName(m_view->message().attachments.value(index).filename);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getSaveFileName(this, "Enregistrer la pièce jointe", dir + "/" + name);
    if (path.isEmpty())
        return;
    fetchAttachment(index, [this, path](const QByteArray &data) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            showError("Enregistrement impossible", f.errorString());
        else
            statusBar()->showMessage("Enregistré : " + path, 5000);
    });
}

void MainWindow::openAttachment(int index)
{
    const QString name = safeFileName(m_view->message().attachments.value(index).filename);
    fetchAttachment(index, [this, name](const QByteArray &data) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                            + QString("/gdesk-%1").arg(QCoreApplication::applicationPid());
        QDir().mkpath(dir);
        QFile f(dir + "/" + name);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            showError("Ouverture impossible", f.errorString());
            return;
        }
        f.close();
        QDesktopServices::openUrl(QUrl::fromLocalFile(f.fileName()));
    });
}

// =============================================================================
//  Actions
// =============================================================================
void MainWindow::applyLabels(const QStringList &add, const QStringList &remove, bool removesFromView,
                             const QString &done)
{
    const QList<QTreeWidgetItem *> items = m_list->selectedItems();
    if (items.isEmpty())
        return;
    QStringList ids;
    for (QTreeWidgetItem *it : items)
        ids << it->data(0, IdRole).toString();
    m_api->modifyMessages(ids, add, remove, [this, done](const QJsonObject &, const QString &err) {
        if (!err.isEmpty()) {
            showError("L'opération a échoué", err);
            reloadList();
            return;
        }
        if (!done.isEmpty())
            statusBar()->showMessage(done, 4000);
        scheduleCountsRefresh();
    });
    if (removesFromView) {
        removeRows(items);
        return;
    }
    for (QTreeWidgetItem *it : items) {
        QStringList labels = it->data(0, LabelsRole).toStringList();
        for (const QString &l : remove)
            labels.removeAll(l);
        for (const QString &l : add)
            if (!labels.contains(l))
                labels << l;
        it->setData(0, LabelsRole, labels);
        updateRowStyle(it);
    }
}

void MainWindow::archive()
{
    const int n = m_list->selectedItems().size();
    applyLabels({}, {"INBOX"}, m_currentLabel == "INBOX" || m_currentLabel.startsWith("CATEGORY_"),
                n > 1 ? QString("%1 messages archivés.").arg(n) : QString("Message archivé."));
}

void MainWindow::trash()
{
    const QList<QTreeWidgetItem *> items = m_list->selectedItems();
    if (items.isEmpty() || m_currentLabel == "TRASH")
        return;
    for (QTreeWidgetItem *it : items)
        m_api->trashMessage(it->data(0, IdRole).toString(), [this](const QJsonObject &, const QString &err) {
            if (!err.isEmpty())
                showError("Suppression impossible", err);
            scheduleCountsRefresh();
        });
    statusBar()->showMessage(items.size() > 1 ? QString("%1 messages placés dans la corbeille.").arg(items.size())
                                              : QString("Message placé dans la corbeille."),
                             4000);
    removeRows(items);
}

void MainWindow::restore()
{
    const QList<QTreeWidgetItem *> items = m_list->selectedItems();
    for (QTreeWidgetItem *it : items)
        m_api->untrashMessage(it->data(0, IdRole).toString(), [this](const QJsonObject &, const QString &err) {
            if (!err.isEmpty())
                showError("Restauration impossible", err);
            scheduleCountsRefresh();
        });
    removeRows(items);
}

void MainWindow::toggleSpam()
{
    if (m_currentLabel == "SPAM" && m_query.isEmpty())
        applyLabels({"INBOX"}, {"SPAM"}, true, "Message remis dans la boîte de réception.");
    else
        applyLabels({"SPAM"}, {"INBOX"}, true, "Signalé comme spam.");
}

void MainWindow::toggleRead()
{
    bool anyUnread = false;
    for (QTreeWidgetItem *it : m_list->selectedItems())
        anyUnread |= it->data(0, LabelsRole).toStringList().contains("UNREAD");
    if (anyUnread)
        applyLabels({}, {"UNREAD"}, false, {});
    else
        applyLabels({"UNREAD"}, {}, false, {});
}

void MainWindow::toggleStar()
{
    bool anyUnstarred = false;
    for (QTreeWidgetItem *it : m_list->selectedItems())
        anyUnstarred |= !it->data(0, LabelsRole).toStringList().contains("STARRED");
    if (anyUnstarred)
        applyLabels({"STARRED"}, {}, false, {});
    else
        applyLabels({}, {"STARRED"}, m_currentLabel == "STARRED" && m_query.isEmpty(), {});
}

Composer *MainWindow::newComposer()
{
    auto *c = new Composer(m_api, m_email, m_knownAddresses, this);
    connect(c, &Composer::sent, this, [this] {
        statusBar()->showMessage("Message envoyé.", 5000);
        if (m_currentLabel == "SENT" || m_currentLabel == "DRAFT")
            reloadList();
        scheduleCountsRefresh();
    });
    return c;
}

void MainWindow::compose(Composer::Mode mode)
{
    if (m_email.isEmpty())
        return;
    if (mode == Composer::New) {
        newComposer()->show();
        return;
    }
    const MailMessage m = m_view->message();
    if (m.id.isEmpty())
        return;
    if (mode != Composer::Forward) {
        Composer *c = newComposer();
        c->prepare(mode, m);
        c->show();
        return;
    }
    // Transfert : on récupère d'abord les pièces jointes
    auto shared = std::make_shared<MailMessage>(m);
    QList<int> pending;
    for (int i = 0; i < m.attachments.size(); ++i)
        if (!m.attachments.at(i).isInline && m.attachments.at(i).data.isEmpty())
            pending << i;
    auto open = [this, shared] {
        statusBar()->clearMessage();
        Composer *c = newComposer();
        c->prepare(Composer::Forward, *shared);
        c->show();
    };
    if (pending.isEmpty()) {
        open();
        return;
    }
    statusBar()->showMessage("Récupération des pièces jointes…");
    auto remaining = std::make_shared<int>(pending.size());
    for (int i : pending)
        m_api->getAttachment(m.id, m.attachments.at(i).attachmentId,
                             [shared, remaining, i, open](const QByteArray &data, const QString &) {
                                 shared->attachments[i].data = data;
                                 if (--*remaining == 0)
                                     open();
                             });
}

void MainWindow::composeMailto(const QUrl &mailto)
{
    bringToFront();
    if (m_email.isEmpty()) {
        QMessageBox::information(this, "G-Desk", "Connectez-vous d'abord à votre compte Google.");
        return;
    }
    Composer *c = newComposer();
    c->prepareMailto(mailto);
    c->show();
}

// =============================================================================
//  Nouveaux messages et barre système
// =============================================================================
void MainWindow::checkNewMail()
{
    if (m_email.isEmpty())
        return;
    m_api->listMessages({"INBOX", "UNREAD"}, {}, {}, 30, [this](const QJsonObject &obj, const QString &err) {
        if (!err.isEmpty())
            return;
        QStringList fresh;
        for (const QJsonValue &v : obj.value("messages").toArray()) {
            const QString id = v.toObject().value("id").toString();
            if (!m_knownUnread.contains(id)) {
                m_knownUnread.insert(id);
                if (m_unreadSeeded)
                    fresh << id;
            }
        }
        m_unreadSeeded = true;
        refreshCounts();
        if (fresh.isEmpty())
            return;

        if (m_currentLabel == "INBOX" && m_query.isEmpty() && m_list->verticalScrollBar()->value() < 20)
            reloadList(m_openId);
        if (!m_actNotifications->isChecked())
            return;
        if (fresh.size() > 3) {
            notify(QString("%1 nouveaux messages").arg(fresh.size()), m_email);
            return;
        }
        for (const QString &id : fresh)
            m_api->getMessage(id, false, [this](const QJsonObject &o, const QString &e) {
                if (!e.isEmpty())
                    return;
                const MailMessage m = Mime::parseMessage(o);
                notify(Mime::displayName(m.from), m.subject.isEmpty() ? m.snippet : m.subject);
            });
    });
}

void MainWindow::notify(const QString &title, const QString &text)
{
    if (m_tray)
        m_tray->showMessage(title, text, m_baseIcon, 8000);
    else
        QApplication::alert(this);
}

void MainWindow::setUnread(int count)
{
    if (count == m_unread)
        return;
    m_unread = count;
    setWindowTitle(count > 0 ? QString("(%1) G-Desk").arg(count) : QString("G-Desk"));
    if (m_tray) {
        m_tray->setIcon(badgeIcon(count));
        m_tray->setToolTip(count > 0 ? QString("G-Desk — %1 message(s) non lu(s)").arg(count) : QString("G-Desk"));
    }
    updateLauncherBadge(count);
}

QIcon MainWindow::badgeIcon(int count) const
{
    if (count <= 0)
        return m_baseIcon;
    QPixmap pix = m_baseIcon.pixmap(64, 64);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor("#d93025"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(28, 0, 36, 36);
    p.setPen(Qt::white);
    QFont f;
    f.setBold(true);
    f.setPixelSize(count < 100 ? 22 : 15);
    p.setFont(f);
    p.drawText(QRect(28, 0, 36, 36), Qt::AlignCenter, count < 1000 ? QString::number(count) : "999+");
    p.end();
    return QIcon(pix);
}

// Pastille sur l'icône de la barre des tâches KDE (protocole Unity LauncherEntry)
void MainWindow::updateLauncherBadge(int count)
{
    QDBusMessage msg = QDBusMessage::createSignal("/gdesk", "com.canonical.Unity.LauncherEntry", "Update");
    QVariantMap props;
    props.insert("count", qint64(qMax(count, 0)));
    props.insert("count-visible", count > 0);
    msg << QString("application://gdesk.desktop") << props;
    QDBusConnection::sessionBus().send(msg);
}

void MainWindow::bringToFront()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

QString MainWindow::autostartPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/gdesk.desktop";
}

void MainWindow::setAutostart(bool enabled)
{
    const QString path = autostartPath();
    if (!enabled) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write("[Desktop Entry]\nType=Application\nName=G-Desk\nExec=gdesk --minimized\nIcon=gdesk\n"
                "X-GNOME-Autostart-enabled=true\n");
}

void MainWindow::about()
{
    QMessageBox::about(this, "À propos de G-Desk",
        "<h3>G-Desk " GDESK_VERSION "</h3>"
        "<p>Client mail pour Gmail, basé sur l'API officielle de Google.</p>"
        "<p>La connexion se fait dans votre navigateur ; G-Desk ne voit jamais votre mot de passe. "
        "Le jeton d'accès est rangé dans le portefeuille KDE (KWallet).</p>"
        "<p><a href='https://krakenagite.github.io/gdesk/'>Site web</a> · "
        "<a href='https://krakenagite.github.io/gdesk/confidentialite.html'>Confidentialité</a> · "
        "<a href='https://github.com/KrakenAgite/gdesk'>Code source</a> — Licence MIT, © 2026 Gabriel Arthus</p>"
        "<p>Gmail et Google sont des marques de Google LLC. G-Desk n'est ni affilié à Google ni approuvé par Google.</p>"
        "<p>Raccourcis : Ctrl+N nouveau · Ctrl+R répondre · Ctrl+Maj+R répondre à tous · Ctrl+L transférer · "
        "A archiver · Suppr supprimer · S suivi · M lu/non lu · J spam · Ctrl+F rechercher · F5 actualiser</p>");
}

void MainWindow::showError(const QString &what, const QString &err)
{
    statusBar()->showMessage(what + " : " + err, 10000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_settings.setValue("geometry", saveGeometry());
    m_settings.setValue("splitter", m_splitter->saveState());
    if (auto *right = m_splitter->findChild<QSplitter *>("rightSplitter"))
        m_settings.setValue("splitter_right", right->saveState());
    if (!m_quitting && m_tray && m_actCloseToTray->isChecked()) {
        event->ignore();
        hide();
        return;
    }
    event->accept();
    quitApp();
}

void MainWindow::quitApp()
{
    if (!m_quitting) {
        m_quitting = true;
        close();
    }
    QApplication::quit();
}
