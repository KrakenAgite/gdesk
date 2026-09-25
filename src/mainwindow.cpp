#include "mainwindow.h"
#include "drivebrowser.h"
#include "driveview.h"
#include "gmailapi.h"
#include "maillistdelegate.h"
#include "googleauth.h"
#include "messageview.h"
#include "settingsdialog.h"
#include "setupdialog.h"
#include "sidebar.h"
#include "theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPushButton>
#include <QRhiWidget>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QMap>
#include <memory>
#include <utility>

namespace {
} // namespace

MainWindow::MainWindow()
    : m_settings("gdesk", "gdesk"),
      m_nam(new QNetworkAccessManager(this))
{
    // Le moteur web dessine via la carte graphique (RHI). L'ajouter à une fenêtre déjà affichée
    // oblige Qt à détruire puis recréer la fenêtre : elle clignotait à l'ouverture du premier
    // message. Ce minuscule composant RHI caché prépare la fenêtre dès sa création (≈ 5 Mo,
    // contre ≈ 90 Mo pour un moteur web démarré en permanence). Il doit exister avant tout ce qui
    // crée la fenêtre native, comme restoreGeometry() d'une fenêtre enregistrée maximisée.
    auto *rhiSurface = new QRhiWidget(this);
    rhiSurface->setObjectName("rhiSurface");
    rhiSurface->setFixedSize(1, 1);
    rhiSurface->hide();

    m_auth = new GoogleAuth(m_nam, this);
    m_api = new GmailApi(m_auth, m_nam, this);
    m_drive = new DriveApi(m_auth, m_nam, this);
    m_baseIcon = QIcon::fromTheme("gdesk", QIcon(":/gdesk.svg"));
    setWindowTitle("G-Desk");
    setWindowIcon(m_baseIcon);
    resize(1300, 820);
    if (m_settings.contains("geometry"))
        restoreGeometry(m_settings.value("geometry").toByteArray());

    buildActions();
    m_pages = new QStackedWidget;
    m_pages->addWidget(buildLoginPage());
    m_pages->addWidget(buildConnectedPage());
    setCentralWidget(m_pages);
    setupTray();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(60 * 1000);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::checkNewMail);
    m_addressSaveTimer = new QTimer(this);
    m_addressSaveTimer->setSingleShot(true);
    m_addressSaveTimer->setInterval(3000);
    connect(m_addressSaveTimer, &QTimer::timeout, this,
            [this] { m_settings.setValue("known_addresses", m_knownAddresses); });
    m_countsTimer = new QTimer(this);
    m_countsTimer->setSingleShot(true);
    m_countsTimer->setInterval(800);
    connect(m_countsTimer, &QTimer::timeout, this, [this] { refreshCounts(); });
    applySettings();

    connect(m_auth, &GoogleAuth::loggedIn, this, &MainWindow::onLoggedIn);
    connect(m_auth, &GoogleAuth::loginFailed, this, [this](const QString &err) {
        showLoginPage("La connexion a échoué : " + err);
        bringToFront();
    });
    // Vue Drive ou sélecteur sans l'autorisation Drive (compte connecté avant la 2.4) : nouveau consentement
    connect(m_drive, &DriveApi::authorizationRequested, this, [this] {
        QTimer::singleShot(0, this, [this] { // après la fermeture éventuelle du sélecteur
            bringToFront();
            startLogin();
        });
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
    make("mail-message-new", "Nouveau", QKeySequence("Ctrl+N"), [this] { compose(Composer::New); });
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

    m_mailOnlyActions = {m_actReply, m_actReplyAll, m_actForward, m_actArchive, m_actDelete, m_actRestore,
                         m_actSpam, m_actRead, m_actStar, m_actRefresh};

    auto *settings = new QAction(QIcon::fromTheme("configure"), "Paramètres…", this);
    settings->setShortcut(QKeySequence("Ctrl+,"));
    connect(settings, &QAction::triggered, this, &MainWindow::openSettings);
    addAction(settings);

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
    auto *prefs = new QPushButton(QIcon::fromTheme("preferences-desktop-theme"), "Paramètres d'affichage…");
    prefs->setFlat(true);
    connect(prefs, &QPushButton::clicked, this, &MainWindow::openSettings);
    buttons->addWidget(prefs);
    v->addLayout(buttons);
    v->addStretch(3);
    return page;
}

QWidget *MainWindow::buildConnectedPage()
{
    auto *page = new QWidget;
    auto *h = new QHBoxLayout(page);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Barre de navigation verticale, comme dans Kontact : Courrier, Drive… et les paramètres en bas
    m_rail = new QToolBar("Navigation");
    m_rail->setObjectName("navigationRail");
    m_rail->setOrientation(Qt::Vertical);
    m_rail->setMovable(false);
    m_rail->setFloatable(false);
    m_rail->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_rail->setIconSize(QSize(32, 32));
    const QPixmap mailFallback = tintedIcon(folderIcon("inbox"), 32, palette().color(QPalette::WindowText), devicePixelRatioF());
    const QPixmap driveFallback = tintedIcon(folderIcon("drive"), 32, palette().color(QPalette::WindowText), devicePixelRatioF());
    auto *group = new QActionGroup(this);
    m_railMail = m_rail->addAction(QIcon::fromTheme("internet-mail", QIcon::fromTheme("mail-message", QIcon(mailFallback))),
                                   "Courrier");
    m_railMail->setShortcut(QKeySequence("Ctrl+1"));
    m_railMail->setToolTip("Courrier (Ctrl+1)");
    m_railDrive = m_rail->addAction(QIcon::fromTheme("folder-gdrive", QIcon::fromTheme("folder-cloud", QIcon(driveFallback))),
                                    "Drive");
    m_railDrive->setShortcut(QKeySequence("Ctrl+2"));
    m_railDrive->setToolTip("Google Drive (Ctrl+2)");
    for (QAction *a : {m_railMail, m_railDrive}) {
        a->setCheckable(true);
        group->addAction(a);
    }
    m_railMail->setChecked(true);
    connect(m_railMail, &QAction::triggered, this, [this] { showView(false); });
    connect(m_railDrive, &QAction::triggered, this, [this] { showView(true); });
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_rail->addWidget(spacer);
    QAction *settings = m_rail->addAction(QIcon::fromTheme("preferences-system", QIcon::fromTheme("configure")),
                                          "Paramètres", this, &MainWindow::openSettings);
    settings->setToolTip("Paramètres (Ctrl+,)");
    for (QAction *a : {m_railMail, m_railDrive, settings})
        if (QWidget *w = m_rail->widgetForAction(a)) {
            w->setMinimumWidth(76);
            w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
    h->addWidget(m_rail);
    auto *line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    h->addWidget(line);

    m_views = new QStackedWidget;
    m_mailPage = buildMailPage();
    m_views->addWidget(m_mailPage);
    h->addWidget(m_views, 1);
    return page;
}

void MainWindow::showView(bool drive)
{
    if (drive && !m_driveView) {
        m_driveView = new DriveView(m_drive);
        connect(m_driveView, &DriveView::statusMessage, this,
                [this](const QString &text, int timeout) { statusBar()->showMessage(text, timeout); });
        connect(m_driveView, &DriveView::sendByMailRequested, this, &MainWindow::composeWithDriveFiles);
        m_views->addWidget(m_driveView);
    }
    (drive ? m_railDrive : m_railMail)->setChecked(true);
    m_views->setCurrentWidget(drive ? static_cast<QWidget *>(m_driveView) : m_mailPage);
    // Les raccourcis du courrier (Suppr, A, S…) ne doivent pas agir sur les messages depuis Drive
    for (QAction *a : std::as_const(m_mailOnlyActions))
        a->setEnabled(!drive);
    statusBar()->clearMessage();
    if (drive) {
        m_driveView->activate();
    } else {
        updateActions();
        m_list->setFocus();
    }
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
    auto *checkAll = new QAction("Tout sélectionner", this);
    checkAll->setShortcut(QKeySequence::SelectAll);
    connect(checkAll, &QAction::triggered, this, [this] { checkWhere([](const QStringList &) { return true; }); });
    addAction(checkAll);
    auto *uncheck = new QAction(this);
    uncheck->setShortcut(Qt::Key_Escape);
    connect(uncheck, &QAction::triggered, this, &MainWindow::clearChecks);
    addAction(uncheck);
    auto *focusSearch = new QAction(this);
    focusSearch->setShortcut(QKeySequence::Find);
    connect(focusSearch, &QAction::triggered, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    addAction(focusSearch);
    m_mailOnlyActions << checkAll << uncheck << focusSearch;
    bar->addWidget(m_search);

    v->addWidget(bar);

    // --- barre latérale : nouveau message, dossiers, compte ---
    auto *sidebar = new QWidget;
    sidebar->setMinimumWidth(200);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 10, 0, 6);
    sideLayout->setSpacing(6);
    auto *composeBtn = new ComposeButton;
    connect(composeBtn, &QAbstractButton::clicked, this, [this] { compose(Composer::New); });
    auto *composeRow = new QHBoxLayout;
    composeRow->setContentsMargins(10, 0, 10, 4);
    composeRow->addWidget(composeBtn);
    composeRow->addStretch(1);
    sideLayout->addLayout(composeRow);

    m_folders = new QTreeWidget;
    m_folders->setHeaderHidden(true);
    m_folders->setRootIsDecorated(false);
    m_folders->setIndentation(0); // l'indentation des sous-libellés est dessinée par le délégué
    m_folders->setItemsExpandable(true);
    m_folders->setExpandsOnDoubleClick(false);
    m_folders->setFrameShape(QFrame::NoFrame);
    m_folders->setMouseTracking(true);
    m_folders->setFocusPolicy(Qt::StrongFocus);
    m_folders->viewport()->setBackgroundRole(QPalette::Window);
    m_folders->setItemDelegate(new FolderDelegate(m_folders));
    m_folders->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_folders, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur && cur->data(0, FolderRoles::SectionKey).toString().isEmpty() && cur->data(0, FolderRoles::Id).isValid())
            onFolderChanged();
    });
    connect(m_folders, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const QString key = item->data(0, FolderRoles::SectionKey).toString();
        if (key.isEmpty())
            return;
        item->setExpanded(!item->isExpanded()); // clic sur un en-tête : replier / déplier
        QStringList collapsed = m_settings.value("sidebar_collapsed").toStringList();
        collapsed.removeAll(key);
        if (!item->isExpanded())
            collapsed << key;
        m_settings.setValue("sidebar_collapsed", collapsed);
        if (QTreeWidgetItem *cur = m_folderItems.value(m_currentLabel)) { // la sélection reste sur le dossier
            QSignalBlocker b(m_folders);
            m_folders->setCurrentItem(cur);
        }
    });
    m_folders->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_folders, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_folders->itemAt(pos);
        if (!item || !item->data(0, FolderRoles::SectionKey).toString().isEmpty()
            || !item->data(0, FolderRoles::Id).isValid())
            return;
        folderMenu(item->data(0, FolderRoles::Id).toString())->popup(m_folders->viewport()->mapToGlobal(pos));
    });
    sideLayout->addWidget(m_folders, 1);

    m_accountChip = new AccountChip;
    m_accountMenu = buildAccountMenu();
    connect(m_accountChip, &QAbstractButton::clicked, this, [this] {
        const QSize size = m_accountMenu->sizeHint();
        m_accountMenu->popup(m_accountChip->mapToGlobal(QPoint(8, -size.height())));
    });
    sideLayout->addWidget(m_accountChip);

    // --- liste ---
    m_list = new QTreeWidget;
    m_list->setColumnCount(1);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setIndentation(0);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setMouseTracking(true); // survol : étoile cliquable
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->viewport()->setBackgroundRole(QPalette::Window); // les cartes se détachent du fond
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMinimumWidth(260);
    m_listDelegate = new MailListDelegate(m_list);
    m_list->setItemDelegate(m_listDelegate);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
    m_listDelegate->attachTo(m_list);
    connect(m_listDelegate, &MailListDelegate::starClicked, this, [this](const QModelIndex &index) {
        if (QTreeWidgetItem *item = m_list->itemFromIndex(index)) { // étoile de cette carte uniquement
            const bool starred = item->data(0, MailRoles::Labels).toStringList().contains("STARRED");
            applyLabelsTo({item}, starred ? QStringList() : QStringList{"STARRED"},
                          starred ? QStringList{"STARRED"} : QStringList(),
                          starred && m_currentLabel == "STARRED" && m_query.isEmpty(), {});
        }
    });
    connect(m_listDelegate, &MailListDelegate::checkClicked, this, &MainWindow::onCheckClicked);
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
    connect(m_view, &MessageView::saveAttachmentToDriveRequested, this, &MainWindow::saveAttachmentToDrive);
    connect(m_view, &MessageView::mailtoClicked, this, &MainWindow::composeMailto);
    connect(m_view, &MessageView::remoteContentAllowed, this, [this](bool always) {
        MailMessage m = m_view->message();
        if (always) {
            m_trustedSenders.insert(Mime::emailOnly(m.from).toLower());
            m_settings.setValue("trusted_senders", QStringList(m_trustedSenders.begin(), m_trustedSenders.end()));
        }
        m_view->showMessage(m, true);
    });

    m_rightSplitter = new QSplitter(Qt::Vertical);
    auto *listPane = new QWidget;
    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);
    listLayout->addWidget(buildSelectionBar());
    listLayout->addWidget(m_list, 1);
    listPane->installEventFilter(this); // boutons compacts quand la liste est étroite
    m_rightSplitter->addWidget(listPane);
    m_rightSplitter->addWidget(m_view);
    m_rightSplitter->setStretchFactor(0, 2);
    m_rightSplitter->setStretchFactor(1, 3);
    m_rightSplitter->setChildrenCollapsible(false);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(sidebar);
    m_splitter->addWidget(m_rightSplitter);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({250, 1050});
    if (m_settings.contains("splitter"))
        m_splitter->restoreState(m_settings.value("splitter").toByteArray());
    v->addWidget(m_splitter, 1);

    statusBar()->setSizeGripEnabled(false);
    updateActions();
    return page;
}

QMenu *MainWindow::buildAccountMenu()
{
    auto *menu = new QMenu(this);
    auto *settings = menu->addAction(QIcon::fromTheme("configure"), "Paramètres…", this, &MainWindow::openSettings);
    settings->setShortcut(QKeySequence("Ctrl+,"));
    menu->addAction(QIcon::fromTheme("internet-web-browser"), "Ouvrir Gmail dans le navigateur", this,
                    [] { QDesktopServices::openUrl(QUrl("https://mail.google.com/")); });
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme("system-switch-user"), "Changer de compte…", this, &MainWindow::switchAccount);
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
    menu->addAction(QIcon::fromTheme("configure"), "Paramètres…", this, [this] {
        bringToFront();
        openSettings();
    });
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
        if (auto *box = m_folderItems.value(m_notifyFolder, m_folderItems.value("INBOX")))
            m_folders->setCurrentItem(box);
    });
    m_tray->show();
}

void MainWindow::updateActions()
{
    if (m_views && m_views->currentWidget() != m_mailPage)
        return; // vue Drive : actions du courrier désactivées (voir showView)
    const int selected = m_list ? int(targetItems().size()) : 0;
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
//  Paramètres
// =============================================================================
void MainWindow::openSettings()
{
    auto *dlg = new SettingsDialog(m_settings, m_email, userLabelChoices(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &SettingsDialog::applied, this, &MainWindow::applySettings);
    connect(dlg, &SettingsDialog::logoutRequested, this, [this, dlg] {
        dlg->close();
        logout();
    });
    connect(dlg, &SettingsDialog::switchAccountRequested, this, [this, dlg] {
        dlg->close();
        switchAccount();
    });
    connect(dlg, &SettingsDialog::configureClientRequested, this, &MainWindow::configureClient);
    connect(dlg, &SettingsDialog::testNotificationRequested, this, [this] {
        notify("G-Desk", "Les notifications fonctionnent. Vous serez prévenu à chaque nouveau message.");
    });
    dlg->open();
}

// Dimensions de la liste et de l'aperçu, mémorisées séparément pour chaque disposition.
// (La 2.0 utilisait déjà la clé « splitter_right » pour tout autre chose : d'où le préfixe distinct.)
static QString previewSplitterKey(const QString &layout)
{
    return "preview_splitter_" + layout;
}

void MainWindow::saveSplitters()
{
    m_settings.setValue("splitter", m_splitter->saveState());
    m_settings.setValue(previewSplitterKey(m_layout), m_rightSplitter->saveState());
}

void MainWindow::applySettings()
{
    // Thème
    Theme::apply(m_settings.value("theme", "system").toString());
    m_view->setDarkContent(Theme::isDark() && m_settings.value("dark_messages", false).toBool());
    m_view->setZoom(m_settings.value("message_zoom", 100).toInt() / 100.0);

    // Densité de la liste
    const QString density = m_settings.value("density", "comfortable").toString();
    m_listDelegate->density = density;
    m_listDelegate->showSnippet = m_settings.value("show_snippet", true).toBool();
    m_list->setUniformRowHeights(false); // force le recalcul de la hauteur des cartes
    m_list->setUniformRowHeights(true);
    m_list->doItemsLayout();
    m_list->viewport()->update();

    // Disposition : aperçu en dessous ou à droite
    const QString layout = m_settings.value("layout", "below").toString();
    if (layout != m_layout || m_rightSplitter->property("initialized").isNull()) {
        if (!m_rightSplitter->property("initialized").isNull())
            m_settings.setValue(previewSplitterKey(m_layout), m_rightSplitter->saveState());
        m_settings.remove("splitter_right"); // ancienne clé de la 2.0
        m_settings.remove("splitter_below");  // ancienne clé de la 2.1.0
        m_layout = layout;
        const Qt::Orientation orientation = layout == "right" ? Qt::Horizontal : Qt::Vertical;
        const QByteArray state = m_settings.value(previewSplitterKey(layout)).toByteArray();
        // Un état enregistré contient aussi l'orientation : on ne le garde que s'il correspond
        const bool restored = !state.isEmpty() && m_rightSplitter->restoreState(state)
                              && m_rightSplitter->orientation() == orientation;
        m_rightSplitter->setOrientation(orientation);
        if (!restored) {
            const int total = orientation == Qt::Horizontal ? m_rightSplitter->width() : m_rightSplitter->height();
            if (total > 0) // sinon (fenêtre pas encore affichée) : répartition par défaut 2/5 – 3/5
                m_rightSplitter->setSizes({total * 2 / 5, total * 3 / 5});
        }
        m_rightSplitter->setProperty("initialized", true);
    }

    // Général et confidentialité
    m_closeToTray = m_settings.value("close_to_tray", true).toBool();
    m_notificationsOn = m_settings.value("notifications", true).toBool();
    m_pollTimer->setInterval(m_settings.value("poll_minutes", 1).toInt() * 60 * 1000);
    m_markReadMode = m_settings.value("mark_read", "immediate").toString();
    m_remoteMode = m_settings.value("remote_images", "ask").toString();
    const QStringList notifyFolders = m_settings.value("notify_folders", QStringList{"INBOX"}).toStringList();
    if (notifyFolders != m_notifyFolders) {
        // Nouvelles boîtes surveillées : la prochaine relève mémorise leurs non-lus sans notifier
        m_notifyFolders = notifyFolders;
        m_knownUnread.clear();
        m_unreadSeeded = false;
        ++m_pollGeneration;
    }
    if (!m_addressSaveTimer->isActive()) // sinon des adresses non encore enregistrées seraient perdues
        setKnownAddresses(m_settings.value("known_addresses").toStringList());
    const QStringList trusted = m_settings.value("trusted_senders").toStringList();
    m_trustedSenders = QSet<QString>(trusted.begin(), trusted.end());

    // Réaffiche le message ouvert avec les nouveaux réglages d'images
    if (!m_view->message().id.isEmpty())
        displayMessage(m_view->message());
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
        const QString email = profile.value("emailAddress").toString();
        if (!m_email.isEmpty() && email != m_email)
            clearMailbox(); // nouvelle autorisation donnée avec un autre compte
        m_email = email;
        m_accountChip->setEmail(m_email);
        if (m_driveView)
            m_driveView->reloadAll();
        m_pages->setCurrentIndex(1);
        m_unreadSeeded = false;
        m_knownUnread.clear();
        restoreStartFolder();
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
    m_auth->logout();
    clearMailbox();
    showLoginPage("Vous êtes déconnecté.");
}

void MainWindow::clearMailbox()
{
    m_pollTimer->stop();
    m_email.clear();
    m_accountChip->setEmail({});
    ++m_generation;
    m_list->clear();
    m_rows.clear();
    m_folders->clear();
    m_folderItems.clear();
    m_openId.clear();
    m_view->clear();
    m_knownUnread.clear();
    m_unreadSeeded = false;
    setUnread(0);
    if (m_driveView) { // les fichiers du compte ne restent pas affichés
        showView(false);
        m_views->removeWidget(m_driveView);
        m_driveView->deleteLater();
        m_driveView = nullptr;
    }
}

void MainWindow::switchAccount()
{
    if (QMessageBox::question(this, "Changer de compte",
                              "Se déconnecter de " + m_email + " et se connecter avec un autre compte Google ?")
        != QMessageBox::Yes)
        return;
    m_auth->logout();
    clearMailbox();
    startLogin();
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
        populateFolders(obj);
    });
}

void MainWindow::populateFolders(const QJsonObject &obj)
{
    {
        QSignalBlocker block(m_folders);
        m_folders->clear();
        m_folderItems.clear();
        const QStringList collapsed = m_settings.value("sidebar_collapsed").toStringList();

        auto addSection = [this, &collapsed](const QString &key, const QString &title) {
            auto *item = new QTreeWidgetItem(m_folders);
            item->setData(0, FolderRoles::SectionKey, key);
            item->setData(0, FolderRoles::Name, title);
            item->setFlags(Qt::ItemIsEnabled); // en-tête : cliquable mais pas sélectionnable
            item->setExpanded(!collapsed.contains(key));
            return item;
        };
        auto addFolder = [this](QTreeWidgetItem *parent, const QString &id, const QString &name, const QString &icon,
                                const QColor &color) {
            auto *item = new QTreeWidgetItem(parent);
            item->setData(0, FolderRoles::Id, id);
            item->setData(0, FolderRoles::Name, name);
            item->setIcon(0, folderIcon(icon));
            if (color.isValid())
                item->setData(0, FolderRoles::Color, color);
            item->setToolTip(0, name);
            m_folderItems.insert(id, item);
            return item;
        };

        for (const SectionDef &section : sidebarSections()) {
            QTreeWidgetItem *header = addSection(section.key, section.title);
            for (const FolderDef &f : section.folders)
                addFolder(header, f.id, f.name, f.icon, QColor(f.color));
            header->setExpanded(!collapsed.contains(section.key)); // après ajout des enfants
        }

        // Libellés personnels (avec leur couleur Gmail) et sous-libellés « Parent/Enfant »
        struct UserLabel { QString path, id; QColor color; };
        QList<UserLabel> userLabels;
        for (const QJsonValue &v : obj.value("labels").toArray()) {
            const QJsonObject l = v.toObject();
            if (l.value("type").toString() == "user")
                userLabels.append({l.value("name").toString(), l.value("id").toString(),
                                   QColor(l.value("color").toObject().value("backgroundColor").toString())});
        }
        std::sort(userLabels.begin(), userLabels.end(), [](const UserLabel &a, const UserLabel &b) {
            return QString::localeAwareCompare(a.path, b.path) < 0;
        });
        if (!userLabels.isEmpty()) {
            QTreeWidgetItem *root = addSection("labels", "Libellés");
            QHash<QString, QTreeWidgetItem *> byPath;
            for (const UserLabel &l : userLabels) {
                QTreeWidgetItem *parent = byPath.value(l.path.section('/', 0, -2), root);
                byPath.insert(l.path, addFolder(parent, l.id, l.path.section('/', -1), "label", l.color));
                parent->setExpanded(parent != root || !collapsed.contains("labels"));
            }
            root->setExpanded(!collapsed.contains("labels"));
        }

        QTreeWidgetItem *current = m_folderItems.value(m_currentLabel, m_folderItems.value("INBOX"));
        for (QTreeWidgetItem *p = current ? current->parent() : nullptr; p; p = p->parent())
            p->setExpanded(true); // boîte de démarrage dans une section repliée
        m_folders->setCurrentItem(current);
    }
    if (m_email.isEmpty())
        return; // aperçu hors connexion (tests)
    refreshCounts();
    if (m_list->topLevelItemCount() == 0)
        onFolderChanged();
}

void MainWindow::scheduleCountsRefresh()
{
    m_countsTimer->start();
}

void MainWindow::refreshCounts(const QStringList &only)
{
    for (auto it = m_folderItems.cbegin(); it != m_folderItems.cend(); ++it) {
        const QString id = it.key();
        if (id.isEmpty() || id == "SENT" || id == "TRASH" || id == "STARRED" || id == "IMPORTANT")
            continue;
        if (!only.isEmpty() && !only.contains(id))
            continue;
        m_api->getLabel(id, [this, id](const QJsonObject &l, const QString &err) {
            QTreeWidgetItem *item = m_folderItems.value(id);
            if (!err.isEmpty() || !item)
                return;
            const int count = (id == "DRAFT" ? l.value("messagesTotal") : l.value("messagesUnread")).toInt();
            item->setData(0, FolderRoles::Count, count);
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
    m_currentLabel = item->data(0, FolderRoles::Id).toString();
    m_settings.setValue("last_folder", m_currentLabel);
    m_query.clear();
    {
        QSignalBlocker b(m_search);
        m_search->clear();
    }
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
    m_checked.clear();
    m_lastCheckedRow = -1;
    updateSelectionBar();
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
    m_api->listMessages(currentListLabels(), m_query, m_nextPageToken, 50,
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
        const bool selectAllAfter = std::exchange(m_selectAllPending, false);
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
            item->setData(0, MailRoles::Id, id);
            m_rows.insert(id, item);
            if (id == keepSelected) {
                m_restoringSelection = true;
                m_list->setCurrentItem(item);
                m_restoringSelection = false;
            }
            loadRowMetadata(id, generation);
        }
        if (selectAllAfter)
            checkWhere([](const QStringList &) { return true; });
        updateSelectionBar();
    });
}

QStringList MainWindow::currentListLabels() const
{
    return (!m_query.isEmpty() || m_currentLabel.isEmpty()) ? QStringList() : QStringList{m_currentLabel};
}

void MainWindow::loadRowMetadata(const QString &id, int generation)
{
    m_api->getMessage(id, false, [this, generation, id](const QJsonObject &o, const QString &e) {
        if (generation != m_generation || !e.isEmpty())
            return;
        if (QTreeWidgetItem *it = m_rows.value(id))
            fillRow(it, Mime::parseMessage(o));
    });
}

// Nouveaux messages : on les insère en haut de la liste au lieu de tout recharger
// (1 requête + 1 par nouveau message, au lieu de 51 ; sélection et défilement conservés)
void MainWindow::insertNewMessages()
{
    const int generation = m_generation;
    m_api->listMessages(currentListLabels(), m_query, {}, 50,
                        [this, generation](const QJsonObject &obj, const QString &err) {
        if (generation != m_generation || !err.isEmpty())
            return;
        int insertAt = 0;
        bool added = false;
        for (const QJsonValue &v : obj.value("messages").toArray()) {
            const QString id = v.toObject().value("id").toString();
            if (QTreeWidgetItem *known = m_rows.value(id)) {
                insertAt = m_list->indexOfTopLevelItem(known) + 1;
                continue;
            }
            auto *item = new QTreeWidgetItem;
            item->setData(0, MailRoles::Id, id);
            m_list->insertTopLevelItem(insertAt++, item);
            m_rows.insert(id, item);
            loadRowMetadata(id, generation);
            added = true;
        }
        if (added) {
            m_lastCheckedRow = -1; // les numéros de ligne ont changé
            updateSelectionBar();
        }
    });
}

void MainWindow::fillRow(QTreeWidgetItem *item, const MailMessage &m)
{
    const bool outgoing = m_currentLabel == "SENT" || m_currentLabel == "DRAFT";
    QStringList names;
    for (const QString &a : Mime::splitAddresses(outgoing ? m.to : m.from))
        names << Mime::displayName(a);
    QString who = names.isEmpty() ? QString("(inconnu)") : names.join(", ");
    if (outgoing)
        who = "À : " + who;
    const QString subject = m.subject.isEmpty() ? QString("(sans objet)") : m.subject;
    item->setData(0, MailRoles::Labels, m.labelIds);
    item->setData(0, MailRoles::Who, who);
    item->setData(0, MailRoles::Date, Mime::shortDate(m.date));
    item->setData(0, MailRoles::Snippet, m.snippet);
    item->setData(0, MailRoles::Subject, subject);
    item->setToolTip(0, QString("<b>%1</b><br>%2<br><i>%3</i>")
                            .arg((outgoing ? m.to : m.from).toHtmlEscaped(), subject.toHtmlEscaped(),
                                 Mime::longDate(m.date)));
    rememberAddresses(m.from);
}

void MainWindow::removeRows(const QList<QTreeWidgetItem *> &items)
{
    if (items.isEmpty())
        return;
    int nextIndex = m_list->indexOfTopLevelItem(items.first());
    {
        QSignalBlocker b(m_list);
        for (QTreeWidgetItem *it : items) {
            m_rows.remove(it->data(0, MailRoles::Id).toString());
            m_checked.remove(it->data(0, MailRoles::Id).toString());
            delete it;
        }
    }
    m_lastCheckedRow = -1;
    updateSelectionBar();
    m_openId.clear();
    m_view->clear();
    if (m_list->topLevelItemCount() > 0 && m_checked.isEmpty()) { // après une action groupée, rien n'est ouvert
        nextIndex = qMin(nextIndex, m_list->topLevelItemCount() - 1);
        m_list->setCurrentItem(m_list->topLevelItem(nextIndex));
    }
    updateActions();
}

void MainWindow::onSelectionChanged()
{
    const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
    if (selected.size() > 1)
        statusBar()->showMessage(QString("%L1 messages sélectionnés").arg(selected.size()));
    if (selected.size() == 1 && !m_restoringSelection) {
        const QString id = selected.first()->data(0, MailRoles::Id).toString();
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
            if (m_markReadMode == "immediate") {
                markRead(id);
                m.labelIds.removeAll("UNREAD");
            } else if (m_markReadMode == "delay") {
                QTimer::singleShot(3000, this, [this, id] {
                    if (id == m_openId)
                        markRead(id);
                });
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
                                 [this, id, i, shared, remaining](const QByteArray &bytes, const QString &) {
                                     shared->attachments[i].data = bytes;
                                     if (--*remaining == 0 && id == m_openId)
                                         displayMessage(*shared);
                                 });
        }
    });
}

void MainWindow::markRead(const QString &id)
{
    m_api->modifyMessages({id}, {}, {"UNREAD"}, [this](const QJsonObject &, const QString &e) {
        if (e.isEmpty())
            scheduleCountsRefresh();
    });
    if (QTreeWidgetItem *item = m_rows.value(id)) {
        QStringList labels = item->data(0, MailRoles::Labels).toStringList();
        labels.removeAll("UNREAD");
        item->setData(0, MailRoles::Labels, labels);
    }
}

void MainWindow::displayMessage(const MailMessage &m)
{
    m_view->showMessage(m, m_remoteMode == "always" || m_trustedSenders.contains(Mime::emailOnly(m.from).toLower()));
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
            || email.contains("no-reply") || m_knownEmails.contains(email))
            continue;
        m_knownEmails.insert(email);
        m_knownAddresses.prepend(a.trimmed());
        changed = true;
    }
    if (!changed)
        return;
    while (m_knownAddresses.size() > 1000)
        m_knownEmails.remove(Mime::emailOnly(m_knownAddresses.takeLast()).toLower());
    m_addressSaveTimer->start(); // une seule écriture pour toute une page de messages
}

void MainWindow::setKnownAddresses(const QStringList &addresses)
{
    m_knownAddresses = addresses;
    m_knownEmails.clear();
    for (const QString &a : addresses)
        m_knownEmails.insert(Mime::emailOnly(a).toLower());
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
    m_api->getAttachment(m.id, a.attachmentId, [this, cb](const QByteArray &bytes, const QString &err) {
        statusBar()->clearMessage();
        if (!err.isEmpty())
            showError("Téléchargement impossible", err);
        else
            cb(bytes);
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
    fetchAttachment(index, [this, path](const QByteArray &bytes) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
            showError("Enregistrement impossible", f.errorString());
        else
            statusBar()->showMessage("Enregistré : " + path, 5000);
    });
}

void MainWindow::openAttachment(int index)
{
    const QString name = safeFileName(m_view->message().attachments.value(index).filename);
    fetchAttachment(index, [this, name](const QByteArray &bytes) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                            + QString("/gdesk-%1").arg(QCoreApplication::applicationPid());
        QDir().mkpath(dir);
        QFile f(dir + "/" + name);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
            showError("Ouverture impossible", f.errorString());
            return;
        }
        f.close();
        QDesktopServices::openUrl(QUrl::fromLocalFile(f.fileName()));
    });
}

void MainWindow::saveAttachmentToDrive(int index)
{
    const Attachment a = m_view->message().attachments.value(index);
    const QString name = a.filename.isEmpty() ? QString("piece-jointe") : a.filename;
    DrivePicker picker(m_drive, DrivePicker::Folder, this);
    picker.setWindowTitle(QString("Enregistrer « %1 » dans Google Drive").arg(name));
    if (picker.exec() != QDialog::Accepted)
        return;
    const DriveFile folder = picker.folder();
    fetchAttachment(index, [this, a, name, folder](const QByteArray &bytes) {
        statusBar()->showMessage(QString("Enregistrement de « %1 » dans Google Drive…").arg(name));
        m_drive->uploadData(name, a.mimeType, bytes, folder.contentId(),
            [this, name, folder](const QJsonObject &, const QString &err) {
                if (!err.isEmpty())
                    showError("Enregistrement dans Google Drive impossible", err);
                else
                    statusBar()->showMessage(QString("« %1 » enregistré dans Google Drive (%2).").arg(name, folder.name), 6000);
            },
            [this, name](qint64 done, qint64 total) {
                statusBar()->showMessage(QString("Enregistrement de « %1 » dans Google Drive… %2 %")
                                             .arg(name).arg(done * 100 / total));
            });
    });
}

void MainWindow::composeWithDriveFiles(const QList<DriveFile> &files)
{
    if (m_email.isEmpty() || files.isEmpty())
        return;
    Composer *c = newComposer();
    c->prepare(Composer::New);
    c->show();
    c->attachFromDrive(files);
}

// =============================================================================
//  Sélection par cases à cocher
// =============================================================================
QWidget *MainWindow::buildSelectionBar()
{
    auto *bar = new QWidget;
    bar->setObjectName("selectionBar");
    bar->setStyleSheet("#selectionBar { border-bottom: 1px solid palette(mid); }");
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(17, 3, 8, 3);
    h->setSpacing(2);

    m_masterCheck = new QCheckBox;
    m_masterCheck->setTristate(true);
    m_masterCheck->setToolTip("Tout sélectionner / tout désélectionner (Ctrl+A, Échap)");
    connect(m_masterCheck, &QCheckBox::clicked, this, [this] {
        if (m_checked.isEmpty())
            checkWhere([](const QStringList &) { return true; });
        else
            clearChecks();
    });
    h->addWidget(m_masterCheck);

    auto *pick = new QToolButton;
    pick->setObjectName("selectionPick");
    pick->setAutoRaise(true);
    pick->setArrowType(Qt::DownArrow);
    pick->setToolTip("Sélectionner…");
    auto *pickMenu = new QMenu(pick);
    pickMenu->addAction("Tous", this, [this] { checkWhere([](const QStringList &) { return true; }); });
    pickMenu->addAction("Aucun", this, &MainWindow::clearChecks);
    pickMenu->addSeparator();
    pickMenu->addAction("Lus", this, [this] { checkWhere([](const QStringList &l) { return !l.contains("UNREAD"); }); });
    pickMenu->addAction("Non lus", this, [this] { checkWhere([](const QStringList &l) { return l.contains("UNREAD"); }); });
    pickMenu->addAction("Suivis", this, [this] { checkWhere([](const QStringList &l) { return l.contains("STARRED"); }); });
    pickMenu->addAction("Non suivis", this,
                        [this] { checkWhere([](const QStringList &l) { return !l.contains("STARRED"); }); });
    // Menu ouvert à la main : un bouton « à menu » recevrait en plus l'indicateur du style (double flèche)
    connect(pick, &QToolButton::clicked, this, [pick, pickMenu] {
        pickMenu->popup(pick->mapToGlobal(QPoint(0, pick->height())));
    });
    h->addWidget(pick);
    h->addSpacing(6);

    m_selectionLabel = new QLabel;
    QFont f = m_selectionLabel->font();
    f.setBold(true);
    m_selectionLabel->setFont(f);
    m_selectionLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    h->addWidget(m_selectionLabel, 1);

    // Actions sur les messages cochés
    m_selectionActions = new QWidget;
    auto *ah = new QHBoxLayout(m_selectionActions);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(0);
    auto addButton = [&](const char *icon, const QString &text, auto slot) {
        auto *b = new QToolButton;
        b->setIcon(QIcon::fromTheme(icon));
        b->setText(text);
        b->setToolTip(text);
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(b, &QToolButton::clicked, this, slot);
        ah->addWidget(b);
        m_selectionButtons << b;
        return b;
    };
    addButton("mail-mark-read", "Lu", [this] { markTargets(true); });
    addButton("mail-mark-unread", "Non lu", [this] { markTargets(false); });
    m_selArchive = addButton("archive-insert", "Archiver", [this] { archive(); });
    m_selSpam = addButton("mail-mark-junk", "Spam", [this] { toggleSpam(); });
    m_selDelete = addButton("edit-delete", "Supprimer", [this] { trash(); });
    m_selRestore = addButton("edit-undo", "Restaurer", [this] { restore(); });
    addButton("rating", "Suivi", [this] { toggleStar(); });
    auto *labelBtn = addButton("tag", "Libellé", [] {});
    labelBtn->setPopupMode(QToolButton::InstantPopup);
    auto *labelMenu = new QMenu(labelBtn);
    connect(labelMenu, &QMenu::aboutToShow, this, [this, labelMenu] {
        labelMenu->clear();
        const QList<LabelChoice> labels = userLabelChoices();
        if (labels.isEmpty())
            labelMenu->addAction("Aucun libellé dans ce compte")->setEnabled(false);
        for (const LabelChoice &l : labels)
            labelMenu->addAction(QIcon(tintedIcon(folderIcon("label"), 16, l.color.isValid() ? l.color
                                                     : palette().color(QPalette::Text), devicePixelRatioF())),
                                 l.name, this, [this, id = l.id, name = l.name] {
                                     applyLabels({id}, {}, false, QString("Libellé « %1 » ajouté.").arg(name));
                                 });
    });
    labelBtn->setMenu(labelMenu);
    ah->addSpacing(4);
    auto *close = new QToolButton;
    close->setIcon(QIcon::fromTheme("dialog-close", QIcon::fromTheme("edit-clear")));
    close->setToolTip("Annuler la sélection (Échap)");
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &MainWindow::clearChecks);
    ah->addWidget(close);
    h->addWidget(m_selectionActions);
    m_selectionActions->setVisible(false);
    return bar;
}

void MainWindow::setChecked(QTreeWidgetItem *item, bool on)
{
    const QString id = item->data(0, MailRoles::Id).toString();
    if (on)
        m_checked.insert(id);
    else
        m_checked.remove(id);
    item->setData(0, MailRoles::Checked, on);
}

void MainWindow::onCheckClicked(const QModelIndex &index, Qt::KeyboardModifiers modifiers)
{
    QTreeWidgetItem *item = m_list->itemFromIndex(index);
    if (!item)
        return;
    const int row = index.row();
    const bool on = !item->data(0, MailRoles::Checked).toBool();
    if ((modifiers & Qt::ShiftModifier) && m_lastCheckedRow >= 0 && m_lastCheckedRow < m_list->topLevelItemCount()) {
        // Maj+clic : toute la plage depuis la dernière case cliquée
        for (int r = qMin(row, m_lastCheckedRow); r <= qMax(row, m_lastCheckedRow); ++r)
            setChecked(m_list->topLevelItem(r), on);
    } else {
        setChecked(item, on);
    }
    m_lastCheckedRow = row;
    updateSelectionBar();
}

void MainWindow::checkWhere(const std::function<bool(const QStringList &)> &predicate)
{
    // Carte encore en chargement : libellés inconnus. « Tous » la coche quand même
    // (le prédicat accepte une liste vide), « Lus », « Suivis »… non.
    const bool takeLoading = predicate({}) && predicate({"UNREAD", "STARRED"});
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        const bool loaded = !item->data(0, MailRoles::Subject).toString().isEmpty();
        setChecked(item, loaded ? predicate(item->data(0, MailRoles::Labels).toStringList()) : takeLoading);
    }
    m_lastCheckedRow = -1;
    updateSelectionBar();
}

void MainWindow::clearChecks()
{
    if (m_checked.isEmpty())
        return;
    for (const QString &id : std::as_const(m_checked))
        if (QTreeWidgetItem *item = m_rows.value(id))
            item->setData(0, MailRoles::Checked, false);
    m_checked.clear();
    m_lastCheckedRow = -1;
    updateSelectionBar();
}

void MainWindow::updateSelectionBar()
{
    const int n = m_checked.size();
    const int total = m_list->topLevelItemCount();
    m_listDelegate->selectionMode = n > 0;
    m_list->viewport()->update();
    {
        QSignalBlocker b(m_masterCheck);
        m_masterCheck->setCheckState(n == 0 ? Qt::Unchecked : n >= total ? Qt::Checked : Qt::PartiallyChecked);
    }
    m_masterCheck->setEnabled(total > 0);
    if (n > 0)
        m_selectionLabel->setText(n == 1 ? QString("1 sélectionné") : QString("%L1 sélectionnés").arg(n));
    else if (!m_query.isEmpty())
        m_selectionLabel->setText("Résultats de recherche");
    else
        m_selectionLabel->setText(folderName(m_currentLabel));
    m_selectionActions->setVisible(n > 0);
    const bool inTrash = m_currentLabel == "TRASH" && m_query.isEmpty();
    const bool inSpam = m_currentLabel == "SPAM" && m_query.isEmpty();
    m_selRestore->setVisible(inTrash);
    m_selDelete->setVisible(!inTrash);
    m_selArchive->setVisible(!inTrash && !inSpam);
    m_selSpam->setText(inSpam ? "Non spam" : "Spam");
    m_selSpam->setToolTip(m_selSpam->text());
    updateActions();
}

void MainWindow::markTargets(bool read)
{
    if (read)
        applyLabels({}, {"UNREAD"}, false, {});
    else
        applyLabels({"UNREAD"}, {}, false, {});
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Liste étroite (aperçu à droite) : boutons de la barre de sélection en icônes seules
    if (event->type() == QEvent::Resize && watched->isWidgetType()) {
        const bool narrow = static_cast<QWidget *>(watched)->width() < 620;
        for (QToolButton *b : std::as_const(m_selectionButtons))
            b->setToolButtonStyle(narrow ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
    return QMainWindow::eventFilter(watched, event);
}

QList<QTreeWidgetItem *> MainWindow::targetItems() const
{
    if (m_checked.isEmpty())
        return m_list->selectedItems();
    QList<QTreeWidgetItem *> items;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) // dans l'ordre de la liste
        if (m_checked.contains(m_list->topLevelItem(i)->data(0, MailRoles::Id).toString()))
            items << m_list->topLevelItem(i);
    return items;
}

void MainWindow::applyLabels(const QStringList &add, const QStringList &remove, bool removesFromView,
                             const QString &done)
{
    applyLabelsTo(targetItems(), add, remove, removesFromView, done);
}

void MainWindow::applyLabelsTo(const QList<QTreeWidgetItem *> &items, const QStringList &add,
                               const QStringList &remove, bool removesFromView, const QString &done)
{
    if (items.isEmpty())
        return;
    QStringList ids;
    for (QTreeWidgetItem *it : items)
        ids << it->data(0, MailRoles::Id).toString();
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
        QStringList labels = it->data(0, MailRoles::Labels).toStringList();
        for (const QString &l : remove)
            labels.removeAll(l);
        for (const QString &l : add)
            if (!labels.contains(l))
                labels << l;
        it->setData(0, MailRoles::Labels, labels);
    }
}

void MainWindow::archive()
{
    const int n = targetItems().size();
    applyLabels({}, {"INBOX"}, m_currentLabel == "INBOX" || m_currentLabel.startsWith("CATEGORY_"),
                n > 1 ? QString("%1 messages archivés.").arg(n) : QString("Message archivé."));
}

void MainWindow::trash()
{
    const QList<QTreeWidgetItem *> items = targetItems();
    if (items.isEmpty() || m_currentLabel == "TRASH")
        return;
    for (QTreeWidgetItem *it : items)
        m_api->trashMessage(it->data(0, MailRoles::Id).toString(), [this](const QJsonObject &, const QString &err) {
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
    const QList<QTreeWidgetItem *> items = targetItems();
    for (QTreeWidgetItem *it : items)
        m_api->untrashMessage(it->data(0, MailRoles::Id).toString(), [this](const QJsonObject &, const QString &err) {
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
    for (QTreeWidgetItem *it : targetItems())
        anyUnread |= it->data(0, MailRoles::Labels).toStringList().contains("UNREAD");
    if (anyUnread)
        applyLabels({}, {"UNREAD"}, false, {});
    else
        applyLabels({"UNREAD"}, {}, false, {});
}

void MainWindow::toggleStar()
{
    bool anyUnstarred = false;
    for (QTreeWidgetItem *it : targetItems())
        anyUnstarred |= !it->data(0, MailRoles::Labels).toStringList().contains("STARRED");
    if (anyUnstarred)
        applyLabels({"STARRED"}, {}, false, {});
    else
        applyLabels({}, {"STARRED"}, m_currentLabel == "STARRED" && m_query.isEmpty(), {});
}

Composer *MainWindow::newComposer()
{
    const QString name = m_settings.value("sender_name").toString().trimmed();
    const QString from = name.isEmpty() ? m_email : QString("%1 <%2>").arg(name, m_email);
    auto *c = new Composer(m_api, from, m_settings.value("signature").toString(), m_knownAddresses, this);
    c->setDriveApi(m_drive);
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
        Composer *c = newComposer();
        c->prepare(Composer::New);
        c->show();
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
                             [shared, remaining, i, open](const QByteArray &bytes, const QString &) {
                                 shared->attachments[i].data = bytes;
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
    // Boîtes interrogées : celles choisies pour les notifications, plus la réception
    // (pour rafraîchir la liste et la pastille même si on n'en veut pas les notifications)
    QStringList polled = m_notifyFolders;
    if (!polled.contains("INBOX"))
        polled.prepend("INBOX");
    const int generation = ++m_pollGeneration;
    auto remaining = std::make_shared<int>(polled.size());
    auto fresh = std::make_shared<QMap<QString, QStringList>>(); // message → boîtes où il est apparu

    for (const QString &box : std::as_const(polled)) {
        m_api->listMessages({box, "UNREAD"}, {}, {}, 25,
                            [this, generation, remaining, fresh, box](const QJsonObject &obj, const QString &err) {
            if (generation != m_pollGeneration)
                return; // réglages modifiés entre-temps
            if (err.isEmpty())
                for (const QJsonValue &v : obj.value("messages").toArray()) {
                    const QString id = v.toObject().value("id").toString();
                    if (!m_knownUnread.contains(id))
                        (*fresh)[id] << box;
                }
            if (--*remaining > 0)
                return;

            // Toutes les boîtes ont répondu
            const bool seeded = m_unreadSeeded;
            for (auto it = fresh->cbegin(); it != fresh->cend(); ++it)
                m_knownUnread.insert(it.key());
            m_unreadSeeded = true; // la première relève ne fait que mémoriser l'existant
            // Compteurs : la réception (pastille) à chaque relève ; les ~15 boîtes seulement
            // s'il y a du nouveau ou toutes les 5 relèves (lectures faites ailleurs)
            if (!fresh->isEmpty() || ++m_pollsSinceCounts >= 5) {
                m_pollsSinceCounts = 0;
                refreshCounts();
            } else {
                refreshCounts({"INBOX"});
            }
            if (!seeded || fresh->isEmpty())
                return;

            bool currentHit = false;
            for (const QStringList &boxes : std::as_const(*fresh))
                currentHit |= boxes.contains(m_currentLabel);
            if (currentHit && m_query.isEmpty())
                insertNewMessages();

            if (!m_notificationsOn)
                return;
            QList<QPair<QString, QString>> toNotify; // (message, boîte surveillée)
            for (auto it = fresh->cbegin(); it != fresh->cend(); ++it)
                for (const QString &b : it.value())
                    if (m_notifyFolders.contains(b)) {
                        toNotify.append({it.key(), b});
                        break;
                    }
            if (toNotify.isEmpty())
                return;
            m_notifyFolder = toNotify.first().second;
            if (toNotify.size() > 3) {
                QStringList names;
                for (const auto &[id, b] : std::as_const(toNotify))
                    if (!names.contains(folderName(b)))
                        names << folderName(b);
                notify(QString("%1 nouveaux messages").arg(toNotify.size()), names.join(", "));
                return;
            }
            for (const auto &[id, b] : std::as_const(toNotify))
                m_api->getMessage(id, false, [this, b](const QJsonObject &o, const QString &e) {
                    if (!e.isEmpty())
                        return;
                    const MailMessage m = Mime::parseMessage(o);
                    // Précise la boîte (ou la catégorie) d'où vient le message
                    QString where = b == "INBOX" ? QString() : folderName(b);
                    if (where.isEmpty())
                        for (const char *cat : {"CATEGORY_SOCIAL", "CATEGORY_PROMOTIONS", "CATEGORY_UPDATES",
                                                "CATEGORY_FORUMS"})
                            if (m.labelIds.contains(cat))
                                where = folderName(cat);
                    notify(where.isEmpty() ? Mime::displayName(m.from) : Mime::displayName(m.from) + " · " + where,
                           m.subject.isEmpty() ? m.snippet : m.subject);
                });
        });
    }
}

void MainWindow::restoreStartFolder()
{
    QString start = m_settings.value("start_folder", "INBOX").toString();
    if (start == "__last__")
        start = m_settings.value("last_folder", "INBOX").toString();
    m_currentLabel = start; // si la boîte n'existe plus, la barre latérale revient à la réception
}

// =============================================================================
//  Menu contextuel d'une boîte et opérations en masse
// =============================================================================
QMenu *MainWindow::folderMenu(const QString &id)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setToolTipsVisible(true);
    const QString name = folderName(id);
    const bool isDraft = id == "DRAFT";

    auto *select = menu->addAction(QIcon::fromTheme("edit-select-all"), "Sélectionner tous les messages", this,
                                   [this, id] { selectAllMessages(id); });
    select->setObjectName("selectAll");
    menu->addSeparator();
    auto *read = menu->addAction(QIcon::fromTheme("mail-mark-read"), "Tout marquer comme lu", this,
                                 [this, id] { markAllInFolder(id, true); });
    read->setObjectName("markRead");
    auto *unread = menu->addAction(QIcon::fromTheme("mail-mark-unread"), "Tout marquer comme non lu", this,
                                   [this, id] { markAllInFolder(id, false); });
    unread->setObjectName("markUnread");
    read->setEnabled(!isDraft);
    unread->setEnabled(!isDraft);
    menu->addSeparator();

    if (id == "TRASH") {
        // La suppression définitive exige une autorisation Google plus large que celle de G-Desk
        auto *web = menu->addAction(QIcon::fromTheme("trash-empty"), "Vider la corbeille dans Gmail…", this,
                                    [] { QDesktopServices::openUrl(QUrl("https://mail.google.com/mail/u/0/#trash")); });
        web->setObjectName("emptyTrashWeb");
        web->setToolTip("Ouvre la corbeille de Gmail dans le navigateur (Gmail la vide aussi seul après 30 jours).");
    } else {
        auto *empty = menu->addAction(QIcon::fromTheme("edit-delete"),
                                      id == "SPAM" ? QString("Vider le spam…") : QString("Vider « %1 »…").arg(name),
                                      this, [this, id] { emptyFolder(id); });
        empty->setObjectName("empty");
        if (id.isEmpty()) {
            empty->setEnabled(false);
            empty->setToolTip("Par sécurité, « Tous les messages » ne peut pas être vidé d'un coup.");
        }
    }
    if (m_bulkBusy)
        for (QAction *a : menu->actions())
            a->setEnabled(false);
    return menu;
}

void MainWindow::selectAllMessages(const QString &id)
{
    if (id != m_currentLabel || !m_query.isEmpty() || m_list->topLevelItemCount() == 0) {
        m_selectAllPending = true; // appliqué à la fin du chargement de la liste
        if (QTreeWidgetItem *item = m_folderItems.value(id); item && id != m_currentLabel)
            m_folders->setCurrentItem(item); // déclenche le chargement
        else
            reloadList();
        return;
    }
    checkWhere([](const QStringList &) { return true; });
}

void MainWindow::collectMessageIds(const QStringList &labels, const QString &query, const QString &pageToken,
                                   std::shared_ptr<QStringList> ids,
                                   std::function<void(const QStringList &, const QString &)> done)
{
    m_api->listMessages(labels, query, pageToken, 500,
                        [this, labels, query, ids, done](const QJsonObject &obj, const QString &err) {
        if (!err.isEmpty()) {
            done(*ids, err);
            return;
        }
        for (const QJsonValue &v : obj.value("messages").toArray())
            ids->append(v.toObject().value("id").toString());
        statusBar()->showMessage(QString("Recherche des messages… %L1").arg(ids->size()));
        const QString next = obj.value("nextPageToken").toString();
        if (next.isEmpty())
            done(*ids, {});
        else
            collectMessageIds(labels, query, next, ids, done);
    });
}

// L'API accepte au plus 1 000 messages par appel : on enchaîne les paquets
void MainWindow::batchModifyAll(const QStringList &ids, const QStringList &add, const QStringList &remove,
                                std::function<void(int, const QString &)> finished, int offset)
{
    if (offset >= ids.size()) {
        finished(ids.size(), {});
        return;
    }
    const QStringList chunk = ids.mid(offset, 1000);
    statusBar()->showMessage(QString("Mise à jour… %L1 / %L2").arg(offset).arg(ids.size()));
    m_api->modifyMessages(chunk, add, remove,
                          [this, ids, add, remove, finished, offset, n = chunk.size()](const QJsonObject &,
                                                                                        const QString &err) {
        if (!err.isEmpty()) {
            finished(offset, err);
            return;
        }
        batchModifyAll(ids, add, remove, finished, offset + n);
    });
}

void MainWindow::applyLabelsToRows(const QStringList &ids, const QStringList &add, const QStringList &remove)
{
    for (const QString &id : ids)
        if (QTreeWidgetItem *item = m_rows.value(id)) {
            QStringList labels = item->data(0, MailRoles::Labels).toStringList();
            for (const QString &l : remove)
                labels.removeAll(l);
            for (const QString &l : add)
                if (!labels.contains(l))
                    labels << l;
            item->setData(0, MailRoles::Labels, labels);
        }
}

void MainWindow::markAllInFolder(const QString &id, bool read)
{
    if (m_bulkBusy)
        return;
    m_bulkBusy = true;
    const QString name = folderName(id);
    QStringList labels = id.isEmpty() ? QStringList() : QStringList{id};
    if (read)
        labels << "UNREAD"; // seuls les non-lus sont à modifier
    collectMessageIds(labels, read ? QString() : QString("is:read"), {}, std::make_shared<QStringList>(),
                      [this, name, read](const QStringList &ids, const QString &err) {
        if (!err.isEmpty() || ids.isEmpty()) {
            m_bulkBusy = false;
            if (!err.isEmpty())
                showError("Impossible de lister les messages", err);
            else
                statusBar()->showMessage(QString("Tous les messages de « %1 » sont déjà %2.")
                                             .arg(name, read ? "lus" : "non lus"), 5000);
            return;
        }
        const QStringList add = read ? QStringList() : QStringList{"UNREAD"};
        const QStringList remove = read ? QStringList{"UNREAD"} : QStringList();
        batchModifyAll(ids, add, remove, [this, ids, add, remove, name, read](int done, const QString &e) {
            m_bulkBusy = false;
            applyLabelsToRows(ids.mid(0, done), add, remove);
            scheduleCountsRefresh();
            if (!e.isEmpty())
                showError(QString("Arrêt après %L1 messages").arg(done), e);
            else
                statusBar()->showMessage(QString("%L1 message(s) de « %2 » marqué(s) comme %3.")
                                             .arg(done).arg(name, read ? "lu(s)" : "non lu(s)"), 6000);
        });
    });
}

void MainWindow::emptyFolder(const QString &id)
{
    if (m_bulkBusy || id.isEmpty() || id == "TRASH")
        return;
    m_bulkBusy = true;
    const QString name = folderName(id);
    collectMessageIds({id}, {}, {}, std::make_shared<QStringList>(),
                      [this, id, name](const QStringList &ids, const QString &err) {
        if (!err.isEmpty() || ids.isEmpty()) {
            m_bulkBusy = false;
            if (!err.isEmpty())
                showError("Impossible de lister les messages", err);
            else
                statusBar()->showMessage(QString("« %1 » est déjà vide.").arg(name), 5000);
            return;
        }
        statusBar()->clearMessage();
        QMessageBox box(QMessageBox::Warning, QString("Vider « %1 »").arg(name),
                        QString("Placer les %L1 message(s) de « %2 » dans la corbeille ?").arg(ids.size()).arg(name),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setInformativeText("Vous pourrez les restaurer depuis la corbeille pendant 30 jours ; "
                               "Gmail les supprimera ensuite définitivement.");
        box.button(QMessageBox::Yes)->setText("Placer dans la corbeille");
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Yes) {
            m_bulkBusy = false;
            return;
        }
        const QStringList remove = id == "SPAM" ? QStringList{"SPAM"} : QStringList();
        batchModifyAll(ids, {"TRASH"}, remove, [this, id, name](int done, const QString &e) {
            m_bulkBusy = false;
            if (id == m_currentLabel)
                reloadList();
            scheduleCountsRefresh();
            if (!e.isEmpty())
                showError(QString("Arrêt après %L1 messages").arg(done), e);
            else
                statusBar()->showMessage(QString("%L1 message(s) de « %2 » placé(s) dans la corbeille.")
                                             .arg(done).arg(name), 6000);
        });
    });
}

QString MainWindow::folderName(const QString &id) const
{
    if (QTreeWidgetItem *item = m_folderItems.value(id))
        return item->data(0, FolderRoles::Name).toString();
    for (const SectionDef &section : sidebarSections())
        for (const FolderDef &f : section.folders)
            if (id == f.id)
                return f.name;
    return id;
}

QList<LabelChoice> MainWindow::userLabelChoices() const
{
    QList<LabelChoice> labels;
    for (QTreeWidgetItemIterator it(m_folders); *it; ++it) {
        QTreeWidgetItem *item = *it;
        if (!item->data(0, FolderRoles::SectionKey).toString().isEmpty())
            continue;
        QTreeWidgetItem *top = item;
        while (top->parent())
            top = top->parent();
        if (top->data(0, FolderRoles::SectionKey).toString() != "labels")
            continue;
        QStringList path;
        for (QTreeWidgetItem *p = item; p && p != top; p = p->parent())
            path.prepend(p->data(0, FolderRoles::Name).toString());
        labels.append({item->data(0, FolderRoles::Id).toString(), path.join(" / "),
                       item->data(0, FolderRoles::Color).value<QColor>()});
    }
    return labels;
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
        "A archiver · Suppr supprimer · S suivi · M lu/non lu · J spam · Ctrl+F rechercher · F5 actualiser · "
        "Ctrl+1 courrier · Ctrl+2 Google Drive</p>");
}

void MainWindow::showError(const QString &what, const QString &err)
{
    statusBar()->showMessage(what + " : " + err, 10000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_settings.setValue("geometry", saveGeometry());
    saveSplitters();
    if (!m_quitting && m_tray && m_closeToTray) {
        event->ignore();
        hide();
        return;
    }
    event->accept();
    quitApp();
}

void MainWindow::quitApp()
{
    if (m_addressSaveTimer->isActive()) {
        m_addressSaveTimer->stop();
        m_settings.setValue("known_addresses", m_knownAddresses);
    }
    if (!m_quitting) {
        m_quitting = true;
        close();
    }
    QApplication::quit();
}
