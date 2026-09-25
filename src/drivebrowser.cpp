#include "drivebrowser.h"
#include "mime.h"
#include "sidebar.h"

#include <QCollator>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
enum { IsFolderRole = DriveBrowser::FileRole + 1, SortRole };

DriveFile rootFolder()
{
    DriveFile f;
    f.id = "root";
    f.name = DrivePlaces::title(DriveApi::MyDrive);
    f.mimeType = DriveApi::FolderMime;
    f.canAddChildren = true;
    f.ownedByMe = true;
    return f;
}

// Ligne de la liste : les dossiers restent en tête, quel que soit le tri
class DriveItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const QTreeWidget *tree = treeWidget();
        const int column = tree ? tree->sortColumn() : 0;
        const bool ascending = !tree || tree->header()->sortIndicatorOrder() == Qt::AscendingOrder;
        const bool a = data(0, IsFolderRole).toBool(), b = other.data(0, IsFolderRole).toBool();
        if (a != b)
            return ascending ? a : b;
        const QVariant x = data(column, SortRole), y = other.data(column, SortRole);
        if (x.typeId() == QMetaType::QString) {
            static const QCollator collator = [] {
                QCollator c;
                c.setNumericMode(true);
                c.setCaseSensitivity(Qt::CaseInsensitive);
                return c;
            }();
            return collator.compare(x.toString(), y.toString()) < 0;
        }
        return x.toLongLong() < y.toLongLong();
    }
};
} // namespace

// =============================================================================
//  Lieux
// =============================================================================
QString DrivePlaces::title(int place)
{
    switch (place) {
    case DriveApi::MyDrive: return "Mon Drive";
    case DriveApi::Recent: return "Récents";
    case DriveApi::Starred: return "Suivis";
    case DriveApi::Shared: return "Partagés avec moi";
    case DriveApi::Trash: return "Corbeille";
    }
    return {};
}

DrivePlaces::DrivePlaces(bool foldersOnly, QWidget *parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setBackgroundRole(QPalette::Window);
    setItemDelegate(new FolderDelegate(this));

    struct Def { int place; const char *icon, *color; };
    for (const Def &d : {Def{DriveApi::MyDrive, "drive", ""}, Def{DriveApi::Recent, "recent", ""},
                         Def{DriveApi::Starred, "star", "#f4b400"}, Def{DriveApi::Shared, "social", ""},
                         Def{DriveApi::Trash, "trash", ""}}) {
        if (foldersOnly && (d.place == DriveApi::Recent || d.place == DriveApi::Trash))
            continue;
        auto *item = new QTreeWidgetItem(this);
        item->setData(0, FolderRoles::Id, QString::number(d.place));
        item->setData(0, FolderRoles::Name, title(d.place));
        item->setIcon(0, folderIcon(d.icon));
        if (*d.color)
            item->setData(0, FolderRoles::Color, QColor(d.color));
        item->setToolTip(0, title(d.place));
    }
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current) {
        if (current)
            emit placeChosen(current->data(0, FolderRoles::Id).toInt());
    });
}

void DrivePlaces::setCurrentPlace(int place)
{
    QSignalBlocker block(this);
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (topLevelItem(i)->data(0, FolderRoles::Id).toInt() == place) {
            setCurrentItem(topLevelItem(i));
            return;
        }
    setCurrentItem(nullptr);
}

void DrivePlaces::mousePressEvent(QMouseEvent *event)
{
    // Clic sur le lieu déjà choisi (depuis un sous-dossier) : retour à sa racine
    QTreeWidgetItem *before = currentItem();
    QTreeWidget::mousePressEvent(event);
    if (QTreeWidgetItem *item = itemAt(event->position().toPoint()); item && item == before)
        emit placeChosen(item->data(0, FolderRoles::Id).toInt());
}

// =============================================================================
//  Liste des fichiers
// =============================================================================
QIcon DriveBrowser::fileIcon(const DriveFile &file)
{
    static QHash<QString, QIcon> cache;
    const QString name = DriveApi::iconName(file);
    if (const auto it = cache.constFind(name); it != cache.constEnd())
        return *it;
    QString generic = "text-x-generic";
    if (file.isFolder())
        generic = "folder";
    else if (const QMimeType t = QMimeDatabase().mimeTypeForName(file.contentMimeType()); t.isValid())
        generic = t.genericIconName();
    const QIcon icon = QIcon::fromTheme(name, QIcon::fromTheme(generic, QIcon::fromTheme("text-x-generic")));
    cache.insert(name, icon);
    return icon;
}

DriveBrowser::DriveBrowser(DriveApi *api, Mode mode, QWidget *parent)
    : QWidget(parent), m_api(api), m_mode(mode)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // Fil d'Ariane : Mon Drive › Factures › 2026
    auto *crumbBar = new QWidget;
    crumbBar->setObjectName("driveCrumbs");
    crumbBar->setStyleSheet("#driveCrumbs { border-bottom: 1px solid palette(mid); }");
    auto *ch = new QHBoxLayout(crumbBar);
    ch->setContentsMargins(6, 3, 6, 3);
    ch->setSpacing(2);
    m_upButton = new QToolButton;
    m_upButton->setIcon(QIcon::fromTheme("go-up"));
    m_upButton->setToolTip("Dossier parent (Retour arrière)");
    m_upButton->setAutoRaise(true);
    connect(m_upButton, &QToolButton::clicked, this, &DriveBrowser::goUp);
    ch->addWidget(m_upButton);
    m_crumbs = new QHBoxLayout;
    m_crumbs->setSpacing(0);
    ch->addLayout(m_crumbs);
    ch->addStretch(1);
    v->addWidget(crumbBar);

    m_stack = new QStackedWidget;
    v->addWidget(m_stack, 1);

    m_list = new QTreeWidget;
    m_list->setObjectName("driveList");
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({"Nom", "Propriétaire", "Modifié", "Taille"});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIconSize(QSize(22, 22));
    m_list->setSelectionMode(mode == PickFolder ? QAbstractItemView::SingleSelection
                                                : QAbstractItemView::ExtendedSelection);
    m_list->headerItem()->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
    QHeaderView *header = m_list->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    header->resizeSection(OwnerColumn, 150);
    header->resizeSection(ModifiedColumn, 120);
    header->resizeSection(SizeColumn, 90);
    header->setSectionsClickable(true);
    if (mode != Browse)
        m_list->hideColumn(OwnerColumn);
    // Tri seulement à la demande : l'ordre de Drive (récents, date de partage…) est conservé sinon
    connect(header, &QHeaderView::sectionClicked, this, [this, header](int column) {
        const Qt::SortOrder order = m_sorted && header->sortIndicatorSection() == column
                                            && header->sortIndicatorOrder() == Qt::AscendingOrder
                                        ? Qt::DescendingOrder
                                        : Qt::AscendingOrder;
        m_sorted = true;
        header->setSortIndicatorShown(true);
        header->setSortIndicator(column, order);
        m_list->sortItems(column, order);
    });
    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const DriveFile f = item->data(0, FileRole).value<DriveFile>();
        if (f.isFolder() && !f.trashed)
            openFolder(f);
        else
            emit fileActivated(f);
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &DriveBrowser::selectionChanged);
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value >= m_list->verticalScrollBar()->maximum() - 5 && !m_loading && !m_nextPageToken.isEmpty())
            fetch();
    });
    m_stack->addWidget(m_list);

    // Message à la place de la liste : dossier vide, autorisation manquante, erreur…
    auto *notice = new QWidget;
    notice->setBackgroundRole(QPalette::Base); // même fond que la liste
    notice->setAutoFillBackground(true);
    auto *nv = new QVBoxLayout(notice);
    nv->addStretch(2);
    m_noticeIcon = new QLabel;
    m_noticeIcon->setAlignment(Qt::AlignCenter);
    nv->addWidget(m_noticeIcon);
    m_noticeText = new QLabel;
    m_noticeText->setObjectName("driveNotice");
    m_noticeText->setAlignment(Qt::AlignCenter);
    m_noticeText->setWordWrap(true);
    nv->addWidget(m_noticeText);
    nv->addSpacing(8);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_noticeAction = new QPushButton;
    m_noticeAction->setObjectName("driveNoticeAction");
    m_noticeAction->setDefault(true);
    connect(m_noticeAction, &QPushButton::clicked, this, [this] {
        if (m_noticeHandler)
            m_noticeHandler();
    });
    m_noticeRetry = new QPushButton(QIcon::fromTheme("view-refresh"), "Réessayer");
    connect(m_noticeRetry, &QPushButton::clicked, this, &DriveBrowser::reload);
    buttons->addWidget(m_noticeAction);
    buttons->addWidget(m_noticeRetry);
    buttons->addStretch(1);
    nv->addLayout(buttons);
    nv->addStretch(3);
    m_stack->addWidget(notice);

    auto addShortcut = [this](const QList<QKeySequence> &keys, auto slot) {
        auto *a = new QAction(this);
        a->setShortcuts(keys);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
    };
    addShortcut({QKeySequence(Qt::Key_Backspace), QKeySequence(Qt::ALT | Qt::Key_Up)}, [this] { goUp(); });
    addShortcut({QKeySequence::Refresh}, [this] { reload(); });

    updateCrumbs();
}

void DriveBrowser::setDropHandler(std::function<void(const QStringList &, const DriveFile &)> handler)
{
    m_dropHandler = std::move(handler);
    setAcceptDrops(bool(m_dropHandler));
}

void DriveBrowser::setPlace(int place)
{
    m_place = place;
    m_search.clear();
    m_path.clear();
    reload();
}

void DriveBrowser::search(const QString &text)
{
    m_search = text.trimmed();
    m_path.clear();
    reload();
}

void DriveBrowser::openFolder(const DriveFile &folder)
{
    m_path.append(folder);
    reload();
}

void DriveBrowser::goUp()
{
    if (m_path.isEmpty())
        return;
    m_path.removeLast();
    reload();
}

void DriveBrowser::reload()
{
    ++m_generation;
    m_nextPageToken.clear();
    m_loading = false;
    m_sorted = false;
    m_list->header()->setSortIndicatorShown(false); // les nouvelles lignes gardent l'ordre de Drive
    {
        QSignalBlocker b(m_list);
        m_list->clear();
    }
    showList();
    updateCrumbs();
    emit locationChanged();
    emit selectionChanged();
    fetch();

    // Chargement long : on l'indique (mais pas pour une réponse rapide, qui ferait clignoter)
    const int generation = m_generation;
    QTimer::singleShot(400, this, [this, generation] {
        if (generation == m_generation && m_loading && m_list->topLevelItemCount() == 0)
            showNotice("", "Chargement…");
    });
}

void DriveBrowser::fetch()
{
    QString query, order;
    const bool foldersOnly = m_mode == PickFolder;
    if (!m_path.isEmpty()) {
        query = DriveApi::folderQuery(m_path.last().contentId(), foldersOnly);
        order = DriveApi::placeOrder(DriveApi::MyDrive);
    } else if (!m_search.isEmpty()) {
        query = DriveApi::searchQuery(m_search, foldersOnly);
    } else if (m_place == DriveApi::MyDrive) {
        query = DriveApi::folderQuery("root", foldersOnly);
        order = DriveApi::placeOrder(DriveApi::MyDrive);
    } else {
        query = DriveApi::placeQuery(DriveApi::Place(m_place));
        if (foldersOnly)
            query += QString(" and mimeType = '%1'").arg(DriveApi::FolderMime);
        order = DriveApi::placeOrder(DriveApi::Place(m_place));
    }

    m_loading = true;
    const int generation = m_generation;
    QPointer<DriveBrowser> self(this);
    m_api->listFiles(query, order, m_nextPageToken, 200,
                     [self, generation](const QJsonObject &obj, const QString &err) {
        if (!self || generation != self->m_generation)
            return;
        self->m_loading = false;
        if (!err.isEmpty()) {
            self->showError(err, obj);
            return;
        }
        self->m_nextPageToken = obj.value("nextPageToken").toString();
        for (const QJsonValue &v : obj.value("files").toArray())
            self->addFile(DriveFile::fromJson(v.toObject()));
        if (self->m_sorted)
            self->m_list->sortItems(self->m_list->sortColumn(), self->m_list->header()->sortIndicatorOrder());
        if (self->m_list->topLevelItemCount() == 0 && self->m_nextPageToken.isEmpty()) {
            self->showEmpty();
            return;
        }
        self->showList();
        // Page pas assez longue pour faire défiler : on charge la suivante tout de suite
        if (!self->m_nextPageToken.isEmpty() && self->m_list->verticalScrollBar()->maximum() == 0)
            self->fetch();
    });
}

void DriveBrowser::addFile(const DriveFile &file)
{
    auto *item = new DriveItem(m_list);
    fillItem(item, file);
}

void DriveBrowser::fillItem(QTreeWidgetItem *item, const DriveFile &f)
{
    item->setData(0, FileRole, QVariant::fromValue(f));
    item->setData(0, IsFolderRole, f.isFolder());
    item->setIcon(NameColumn, fileIcon(f));
    item->setText(NameColumn, f.starred ? f.name + "  ★" : f.name);
    item->setData(NameColumn, SortRole, f.name);
    item->setToolTip(NameColumn, f.name);
    const QString owner = f.ownedByMe ? QString("moi") : f.owner;
    item->setText(OwnerColumn, owner);
    item->setData(OwnerColumn, SortRole, owner);
    item->setText(ModifiedColumn, Mime::shortDate(f.modified));
    item->setToolTip(ModifiedColumn, Mime::longDate(f.modified));
    item->setData(ModifiedColumn, SortRole, f.modified.toMSecsSinceEpoch());
    item->setText(SizeColumn, f.size >= 0 ? Mime::humanSize(f.size) : QString("—"));
    item->setData(SizeColumn, SortRole, f.size);
    item->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
    for (int c : {OwnerColumn, ModifiedColumn, SizeColumn})
        item->setForeground(c, m_list->palette().brush(QPalette::Disabled, QPalette::Text));
}

void DriveBrowser::updateFile(const DriveFile &file)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (item->data(0, FileRole).value<DriveFile>().id == file.id) {
            fillItem(item, file);
            break;
        }
    }
    emit selectionChanged();
}

void DriveBrowser::removeFiles(const QStringList &ids)
{
    {
        QSignalBlocker b(m_list);
        for (int i = m_list->topLevelItemCount() - 1; i >= 0; --i)
            if (ids.contains(m_list->topLevelItem(i)->data(0, FileRole).value<DriveFile>().id))
                delete m_list->takeTopLevelItem(i);
    }
    if (m_list->topLevelItemCount() == 0 && m_nextPageToken.isEmpty())
        showEmpty();
    emit selectionChanged();
}

QList<DriveFile> DriveBrowser::selectedFiles() const
{
    QList<DriveFile> files;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) // dans l'ordre de la liste
        if (m_list->topLevelItem(i)->isSelected())
            files << m_list->topLevelItem(i)->data(0, FileRole).value<DriveFile>();
    return files;
}

DriveFile DriveBrowser::currentFolder() const
{
    if (!m_path.isEmpty())
        return m_path.last();
    if (m_search.isEmpty() && m_place == DriveApi::MyDrive)
        return rootFolder();
    return {};
}

bool DriveBrowser::canAddHere() const
{
    const DriveFile f = currentFolder();
    return f.isValid() && m_place != DriveApi::Trash && (f.id == "root" || f.canAddChildren);
}

QVariant DriveBrowser::saveLocation() const
{
    QVariantList path;
    for (const DriveFile &f : m_path)
        path << QVariantMap{{"id", f.id}, {"name", f.name}, {"mime", f.mimeType},
                            {"target", f.targetId}, {"targetMime", f.targetMimeType},
                            {"canAdd", f.canAddChildren}};
    return QVariantMap{{"place", m_place}, {"path", path}};
}

void DriveBrowser::restoreLocation(const QVariant &state)
{
    const QVariantMap map = state.toMap();
    m_place = map.value("place", int(DriveApi::MyDrive)).toInt();
    m_search.clear();
    m_path.clear();
    for (const QVariant &v : map.value("path").toList()) {
        const QVariantMap m = v.toMap();
        DriveFile f;
        f.id = m.value("id").toString();
        f.name = m.value("name").toString();
        f.mimeType = m.value("mime").toString();
        f.targetId = m.value("target").toString();
        f.targetMimeType = m.value("targetMime").toString();
        f.canAddChildren = m.value("canAdd").toBool();
        if (f.isValid())
            m_path << f;
    }
    reload();
}

void DriveBrowser::updateCrumbs()
{
    while (QLayoutItem *item = m_crumbs->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QStringList names{m_search.isEmpty() ? DrivePlaces::title(m_place)
                                         : QString("Résultats pour « %1 »").arg(m_search)};
    for (const DriveFile &f : std::as_const(m_path))
        names << f.name;
    for (int i = 0; i < names.size(); ++i) {
        if (i > 0) {
            auto *sep = new QLabel("›");
            sep->setEnabled(false);
            sep->setContentsMargins(2, 0, 2, 0);
            m_crumbs->addWidget(sep);
        }
        auto *b = new QToolButton;
        b->setAutoRaise(true);
        b->setText(b->fontMetrics().elidedText(names[i], Qt::ElideMiddle, 220).replace('&', "&&"));
        b->setToolTip(names[i]);
        const bool last = i == names.size() - 1;
        if (last) {
            QFont f = b->font();
            f.setBold(true);
            b->setFont(f);
        }
        connect(b, &QToolButton::clicked, this, [this, i] {
            if (i == m_path.size())
                return; // dossier actuel
            m_path = m_path.mid(0, i);
            reload();
        });
        m_crumbs->addWidget(b);
    }
    m_upButton->setEnabled(!m_path.isEmpty());
}

void DriveBrowser::showList()
{
    m_stack->setCurrentIndex(0);
}

void DriveBrowser::showNotice(const QString &icon, const QString &text, const QString &actionText,
                              std::function<void()> action, bool retry)
{
    m_noticeIcon->setPixmap(icon.isEmpty() ? QPixmap() : QIcon::fromTheme(icon).pixmap(64, 64));
    m_noticeIcon->setVisible(!icon.isEmpty());
    m_noticeText->setText(text);
    m_noticeAction->setText(actionText);
    m_noticeAction->setVisible(!actionText.isEmpty());
    m_noticeHandler = std::move(action);
    m_noticeRetry->setVisible(retry);
    m_stack->setCurrentIndex(1);
}

void DriveBrowser::showError(const QString &error, const QJsonObject &details)
{
    if (DriveApi::needsConsent(details)) {
        showNotice("folder-gdrive",
                   "G-Desk n'a pas encore accès à votre Google Drive.\n"
                   "Autorisez-le pour parcourir vos fichiers, joindre des fichiers Drive à vos messages\n"
                   "et y enregistrer vos pièces jointes. Google vous redemandera votre accord dans le navigateur.",
                   "Autoriser l'accès à Google Drive…", [this] { m_api->requestAuthorization(); });
    } else if (DriveApi::apiDisabled(details)) {
        showNotice("dialog-warning",
                   "L'API Google Drive n'est pas activée dans votre projet Google Cloud.\n"
                   "Activez-la, patientez une minute, puis réessayez.",
                   "Activer l'API Google Drive…",
                   [] { QDesktopServices::openUrl(QUrl(DriveApi::EnableApiUrl)); }, true);
    } else {
        showNotice("dialog-error", "Impossible de charger les fichiers :\n" + error, {}, {}, true);
    }
}

void DriveBrowser::showEmpty()
{
    QString text;
    if (!m_path.isEmpty())
        text = m_mode == PickFolder ? "Aucun sous-dossier." : "Ce dossier est vide.";
    else if (!m_search.isEmpty())
        text = QString("Aucun résultat pour « %1 ».").arg(m_search);
    else if (m_place == DriveApi::MyDrive)
        text = m_mode == PickFolder ? "Aucun dossier dans votre Drive." : "Votre Drive est vide.";
    else if (m_place == DriveApi::Recent)
        text = "Aucun fichier récent.";
    else if (m_place == DriveApi::Starred)
        text = "Aucun élément suivi.";
    else if (m_place == DriveApi::Shared)
        text = "Aucun fichier n'a été partagé avec vous.";
    else
        text = "La corbeille est vide.";
    if (m_dropHandler && canAddHere())
        text += "\nGlissez des fichiers ici pour les importer.";
    showNotice(m_place == DriveApi::Trash && m_path.isEmpty() && m_search.isEmpty() ? "user-trash" : "folder",
               text);
}

// --- Dépôt de fichiers depuis le gestionnaire de fichiers -------------------------
QTreeWidgetItem *DriveBrowser::folderItemAt(const QPoint &pos) const
{
    if (m_stack->currentIndex() != 0)
        return nullptr;
    QTreeWidgetItem *item = m_list->itemAt(m_list->viewport()->mapFrom(this, pos));
    if (!item)
        return nullptr;
    const DriveFile f = item->data(0, FileRole).value<DriveFile>();
    return f.isFolder() && f.canAddChildren && !f.trashed ? item : nullptr;
}

static bool localFiles(const QMimeData *data)
{
    if (!data->hasUrls())
        return false;
    for (const QUrl &url : data->urls())
        if (!url.isLocalFile())
            return false;
    return true;
}

void DriveBrowser::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_dropHandler && localFiles(event->mimeData()))
        event->acceptProposedAction();
}

void DriveBrowser::dragMoveEvent(QDragMoveEvent *event)
{
    if (folderItemAt(event->position().toPoint()) || canAddHere())
        event->acceptProposedAction();
    else
        event->ignore();
}

void DriveBrowser::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls())
        paths << url.toLocalFile();
    QTreeWidgetItem *item = folderItemAt(event->position().toPoint());
    const DriveFile target = item ? item->data(0, FileRole).value<DriveFile>() : currentFolder();
    if (!target.isValid() || paths.isEmpty())
        return;
    event->acceptProposedAction();
    m_dropHandler(paths, target);
}

// =============================================================================
//  Sélecteur
// =============================================================================
DrivePicker::DrivePicker(DriveApi *api, Kind kind, QWidget *parent)
    : QDialog(parent), m_api(api), m_kind(kind)
{
    setWindowTitle(kind == Files ? "Joindre des fichiers depuis Google Drive" : "Choisir un dossier Google Drive");
    resize(880, 560);
    auto *v = new QVBoxLayout(this);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(kind == Files ? "Rechercher dans Drive" : "Rechercher un dossier");
    m_search->setClearButtonEnabled(true);
    m_search->addAction(QIcon::fromTheme("search"), QLineEdit::LeadingPosition);
    v->addWidget(m_search);

    auto *h = new QHBoxLayout;
    h->setSpacing(0);
    m_places = new DrivePlaces(kind == Folder);
    m_places->setFixedWidth(210);
    h->addWidget(m_places);
    auto *line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    h->addWidget(line);
    m_browser = new DriveBrowser(api, kind == Files ? DriveBrowser::PickFiles : DriveBrowser::PickFolder);
    h->addWidget(m_browser, 1);
    v->addLayout(h, 1);

    auto *bottom = new QHBoxLayout;
    if (kind == Folder) {
        auto *newFolder = new QPushButton(QIcon::fromTheme("folder-new"), "Nouveau dossier…");
        newFolder->setObjectName("pickerNewFolder");
        connect(newFolder, &QPushButton::clicked, this, [this] {
            const DriveFile parentFolder = m_browser->currentFolder();
            const QString name = QInputDialog::getText(this, "Nouveau dossier", "Nom du dossier :").trimmed();
            if (name.isEmpty())
                return;
            QPointer<DrivePicker> self(this);
            m_api->createFolder(name, parentFolder.contentId(), [self](const QJsonObject &obj, const QString &err) {
                if (!self)
                    return;
                if (!err.isEmpty())
                    QMessageBox::warning(self, "Nouveau dossier", err);
                else
                    self->m_browser->openFolder(DriveFile::fromJson(obj));
            });
        });
        connect(m_browser, &DriveBrowser::locationChanged, newFolder,
                [this, newFolder] { newFolder->setEnabled(m_browser->canAddHere()); });
        bottom->addWidget(newFolder);
        m_destination = new QLabel;
        bottom->addWidget(m_destination, 1);
    } else {
        bottom->addStretch(1);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(kind == Files ? "Joindre" : "Enregistrer ici");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottom->addWidget(buttons);
    v->addLayout(bottom);

    connect(m_places, &DrivePlaces::placeChosen, this, [this](int place) {
        QSignalBlocker b(m_search);
        m_search->clear();
        m_browser->setPlace(place);
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (!m_search->text().trimmed().isEmpty())
            m_browser->search(m_search->text());
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (t.isEmpty() && !m_browser->searchText().isEmpty())
            m_browser->setPlace(m_browser->place() < 0 ? int(DriveApi::MyDrive) : m_browser->place());
    });
    connect(m_browser, &DriveBrowser::locationChanged, this, [this] {
        m_places->setCurrentPlace(m_browser->searchText().isEmpty() ? m_browser->place() : -1);
        updateButtons();
    });
    connect(m_browser, &DriveBrowser::selectionChanged, this, &DrivePicker::updateButtons);
    connect(m_browser, &DriveBrowser::fileActivated, this, [this](const DriveFile &f) {
        if (m_kind == Files && !f.isFolder())
            accept();
    });
    // L'autorisation se donne dans le navigateur : le sélecteur se ferme
    connect(api, &DriveApi::authorizationRequested, this, &QDialog::reject);

    m_browser->restoreLocation(QSettings("gdesk", "gdesk").value(kind == Files ? "drive_picker_files"
                                                                               : "drive_picker_folder"));
    m_browser->list()->setFocus();
}

void DrivePicker::setAcceptText(const QString &text)
{
    m_ok->setText(text);
}

QList<DriveFile> DrivePicker::files() const
{
    QList<DriveFile> files;
    for (const DriveFile &f : m_browser->selectedFiles())
        if (!f.isFolder())
            files << f;
    return files;
}

DriveFile DrivePicker::folder() const
{
    return m_browser->currentFolder();
}

void DrivePicker::updateButtons()
{
    if (m_kind == Files) {
        const int n = files().size();
        m_ok->setEnabled(n > 0);
        m_ok->setText(n > 1 ? QString("Joindre (%1)").arg(n) : QString("Joindre"));
        return;
    }
    const DriveFile f = folder();
    m_ok->setEnabled(m_browser->canAddHere());
    m_destination->setText(m_browser->canAddHere()
                               ? QString("Destination : <b>%1</b>").arg(f.name.toHtmlEscaped())
                               : QString("Ouvrez le dossier où enregistrer."));
}

void DrivePicker::accept()
{
    if (m_kind == Files ? files().isEmpty() : !m_browser->canAddHere())
        return;
    QSettings("gdesk", "gdesk").setValue(m_kind == Files ? "drive_picker_files" : "drive_picker_folder",
                                         m_browser->saveLocation());
    QDialog::accept();
}
