#include "driveview.h"
#include "drivebrowser.h"
#include "mime.h"
#include "sidebar.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

static QString safeFileName(QString name)
{
    name.replace('/', '_').replace('\\', '_');
    if (name.startsWith('.'))
        name.prepend('_');
    return name.isEmpty() ? QString("fichier") : name;
}

static int percent(qint64 done, qint64 total)
{
    return total > 0 ? int(done * 100 / total) : -1;
}

DriveView::DriveView(DriveApi *api, QWidget *parent) : QWidget(parent), m_api(api)
{
    m_browser = new DriveBrowser(api, DriveBrowser::Browse);
    m_browser->setDropHandler([this](const QStringList &paths, const DriveFile &folder) { uploadPaths(paths, folder); });

    auto make = [this](const char *icon, const QString &text, const QKeySequence &key, auto slot) {
        auto *a = new QAction(QIcon::fromTheme(icon), text, this);
        if (!key.isEmpty()) {
            a->setShortcut(key);
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            a->setToolTip(QString("%1 (%2)").arg(text, key.toString(QKeySequence::NativeText)));
        }
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    m_actOpen = make("document-open", "Ouvrir", {}, [this] {
        if (const QList<DriveFile> sel = m_browser->selectedFiles(); sel.size() == 1)
            openFile(sel.first());
    });
    m_actOpenWeb = make("internet-web-browser", "Ouvrir dans le navigateur", {}, [this] {
        if (const QList<DriveFile> sel = m_browser->selectedFiles(); sel.size() == 1)
            QDesktopServices::openUrl(QUrl(sel.first().webViewLink));
    });
    m_actDownload = make("download", "Télécharger…", {}, [this] { downloadSelected(); });
    m_actMail = make("mail-send", "Envoyer par e-mail", {}, [this] { emit sendByMailRequested(downloadableSelection()); });
    m_actLink = make("edit-copy", "Copier le lien", {}, [this] {
        QStringList links;
        for (const DriveFile &f : m_browser->selectedFiles())
            links << f.webViewLink;
        QApplication::clipboard()->setText(links.join('\n'));
        emit statusMessage(links.size() > 1 ? QString("Liens copiés.") : QString("Lien copié."), 4000);
    });
    m_actRename = make("edit-rename", "Renommer…", QKeySequence(Qt::Key_F2), [this] { rename(); });
    m_actStar = make("rating", "Suivi", QKeySequence("S"), [this] { toggleStar(); });
    m_actTrash = make("edit-delete", "Supprimer", QKeySequence::Delete, [this] { setTrashed(true); });
    m_actRestore = make("edit-undo", "Restaurer", {}, [this] { setTrashed(false); });
    m_actNewFolder = make("folder-new", "Nouveau dossier…", QKeySequence("Ctrl+Shift+N"), [this] { newFolder(); });
    m_actUpload = make("document-import", "Importer des fichiers…", QKeySequence("Ctrl+U"), [this] { importFiles(); });
    m_actRefresh = make("view-refresh", "Actualiser", {}, [this] {
        m_browser->reload();
        refreshQuota();
    });
    m_actRefresh->setToolTip("Actualiser (F5)"); // raccourci porté par la liste
    m_actRename->setObjectName("driveRename");
    m_actTrash->setObjectName("driveTrash");

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildToolBar());
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(buildSidebar());
    splitter->addWidget(m_browser);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({250, 1050});
    splitter->setChildrenCollapsible(false);
    v->addWidget(splitter, 1);

    QTreeWidget *list = m_browser->list();
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QWidget::customContextMenuRequested, this, &DriveView::showContextMenu);
    connect(m_browser, &DriveBrowser::fileActivated, this, &DriveView::openFile);
    connect(m_browser, &DriveBrowser::selectionChanged, this, [this] {
        const int n = m_browser->selectedFiles().size();
        if (n > 1)
            emit statusMessage(QString("%L1 éléments sélectionnés").arg(n), 0);
        updateActions();
    });
    connect(m_browser, &DriveBrowser::locationChanged, this, [this] {
        const bool searching = !m_browser->searchText().isEmpty();
        if (!searching)
            m_lastPlace = m_browser->place();
        m_places->setCurrentPlace(searching ? -1 : m_browser->place());
        updateActions();
    });
    connect(m_places, &DrivePlaces::placeChosen, this, [this](int place) {
        QSignalBlocker b(m_search);
        m_search->clear();
        m_browser->setPlace(place);
    });

    // Fichier ajouté (import, pièce jointe enregistrée depuis le courrier…) : la liste se met à jour
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(400);
    connect(m_reloadTimer, &QTimer::timeout, this, [this] {
        if (m_loaded) {
            m_browser->reload();
            refreshQuota();
        }
    });
    connect(m_api, &DriveApi::filesChanged, m_reloadTimer, qOverload<>(&QTimer::start));
    updateActions();
}

QWidget *DriveView::buildToolBar()
{
    auto *bar = new QToolBar;
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->setIconSize(QSize(20, 20));
    bar->addAction(m_actNewFolder);
    bar->addAction(m_actUpload);
    bar->addSeparator();
    for (QAction *a : {m_actDownload, m_actMail, m_actRename, m_actStar, m_actTrash, m_actRestore})
        bar->addAction(a);
    for (QAction *a : {m_actNewFolder, m_actUpload, m_actRename, m_actStar}) // aussi dans le bouton « Nouveau »
        if (auto *w = qobject_cast<QToolButton *>(bar->widgetForAction(a)))
            w->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->addSeparator();
    bar->addAction(m_actRefresh);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    m_search = new QLineEdit;
    m_search->setObjectName("driveSearch");
    m_search->setPlaceholderText("Rechercher dans Drive");
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(280);
    m_search->addAction(QIcon::fromTheme("search"), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (!m_search->text().trimmed().isEmpty())
            m_browser->search(m_search->text());
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (t.isEmpty() && !m_browser->searchText().isEmpty())
            m_browser->setPlace(m_lastPlace);
    });
    auto *focusSearch = new QAction(this);
    focusSearch->setShortcut(QKeySequence::Find);
    focusSearch->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(focusSearch, &QAction::triggered, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    addAction(focusSearch);
    bar->addWidget(m_search);
    return bar;
}

QWidget *DriveView::buildSidebar()
{
    auto *sidebar = new QWidget;
    sidebar->setMinimumWidth(200);
    auto *v = new QVBoxLayout(sidebar);
    v->setContentsMargins(0, 10, 0, 8);
    v->setSpacing(6);

    auto *newButton = new ComposeButton;
    newButton->setObjectName("driveNew");
    newButton->setText("Nouveau");
    newButton->setIcon(QIcon(":/sidebar/new.svg"));
    newButton->setToolTip("Nouveau dossier, importer des fichiers");
    auto *newMenu = new QMenu(newButton);
    newMenu->addAction(m_actNewFolder);
    newMenu->addAction(m_actUpload);
    connect(newButton, &QAbstractButton::clicked, this, [newButton, newMenu] {
        newMenu->popup(newButton->mapToGlobal(QPoint(0, newButton->height())));
    });
    auto *row = new QHBoxLayout;
    row->setContentsMargins(10, 0, 10, 4);
    row->addWidget(newButton);
    row->addStretch(1);
    v->addLayout(row);

    m_places = new DrivePlaces(false);
    m_places->setCurrentPlace(DriveApi::MyDrive);
    v->addWidget(m_places, 1);

    // Espace de stockage
    auto *quota = new QWidget;
    auto *q = new QVBoxLayout(quota);
    q->setContentsMargins(18, 4, 14, 0);
    q->setSpacing(4);
    m_quotaBar = new QProgressBar;
    m_quotaBar->setRange(0, 1000);
    m_quotaBar->setTextVisible(false);
    m_quotaBar->setFixedHeight(6);
    m_quotaBar->setVisible(false);
    m_quotaLabel = new QLabel;
    m_quotaLabel->setEnabled(false);
    m_quotaLabel->setWordWrap(true);
    q->addWidget(m_quotaBar);
    q->addWidget(m_quotaLabel);
    v->addWidget(quota);
    return sidebar;
}

void DriveView::activate()
{
    if (!m_loaded) {
        m_loaded = true;
        m_browser->setPlace(DriveApi::MyDrive);
        refreshQuota();
    }
    m_browser->list()->setFocus();
}

void DriveView::reloadAll()
{
    if (!m_loaded)
        return;
    m_browser->reload();
    refreshQuota();
}

void DriveView::refreshQuota()
{
    QPointer<DriveView> self(this);
    m_api->about([self](const QJsonObject &obj, const QString &err) {
        if (!self || !err.isEmpty())
            return;
        const QJsonObject quota = obj.value("storageQuota").toObject();
        const qint64 usage = quota.value("usage").toString().toLongLong();
        const qint64 limit = quota.value("limit").toString().toLongLong(); // absent : illimité
        self->m_quotaBar->setVisible(limit > 0);
        if (limit > 0) {
            self->m_quotaBar->setValue(int(qMin<qint64>(1000, usage * 1000 / limit)));
            self->m_quotaLabel->setText(QString("%1 utilisés sur %2").arg(Mime::humanSize(usage), Mime::humanSize(limit)));
        } else {
            self->m_quotaLabel->setText(QString("%1 utilisés").arg(Mime::humanSize(usage)));
        }
    });
}

QList<DriveFile> DriveView::downloadableSelection() const
{
    QList<DriveFile> files;
    for (const DriveFile &f : m_browser->selectedFiles())
        if (!f.isFolder() && (!f.isGoogleFile() || !DriveApi::exportFormat(f.contentMimeType()).first.isEmpty()))
            files << f;
    return files;
}

void DriveView::updateActions()
{
    const QList<DriveFile> sel = m_browser->selectedFiles();
    const bool inTrash = m_browser->place() == DriveApi::Trash && m_browser->searchText().isEmpty();
    const int downloadable = downloadableSelection().size();
    bool canTrash = !sel.isEmpty(), canRename = sel.size() == 1;
    for (const DriveFile &f : sel) {
        canTrash &= f.canTrash;
        canRename &= f.canRename;
    }
    m_actOpen->setEnabled(sel.size() == 1);
    m_actOpenWeb->setEnabled(sel.size() == 1 && !sel.first().webViewLink.isEmpty());
    m_actDownload->setEnabled(downloadable > 0 && downloadable == sel.size());
    m_actMail->setEnabled(downloadable > 0 && downloadable == sel.size() && !inTrash);
    m_actLink->setEnabled(!sel.isEmpty());
    m_actRename->setEnabled(canRename && !inTrash);
    m_actStar->setEnabled(!sel.isEmpty() && !inTrash);
    m_actTrash->setVisible(!inTrash);
    m_actTrash->setEnabled(canTrash && !inTrash);
    m_actRestore->setVisible(inTrash);
    m_actRestore->setEnabled(!sel.isEmpty() && inTrash);
    m_actNewFolder->setEnabled(m_browser->canAddHere());
    m_actUpload->setEnabled(m_browser->canAddHere());
}

void DriveView::showContextMenu(const QPoint &pos)
{
    QTreeWidget *list = m_browser->list();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (!list->itemAt(pos)) {
        list->clearSelection();
        menu->addAction(m_actNewFolder);
        menu->addAction(m_actUpload);
        menu->addSeparator();
        menu->addAction(m_actRefresh);
    } else {
        menu->addAction(m_actOpen);
        menu->addAction(m_actOpenWeb);
        menu->addSeparator();
        menu->addAction(m_actDownload);
        menu->addAction(m_actMail);
        menu->addAction(m_actLink);
        menu->addSeparator();
        menu->addAction(m_actRename);
        menu->addAction(m_actStar);
        menu->addAction(m_actTrash);
        menu->addAction(m_actRestore);
    }
    menu->popup(list->viewport()->mapToGlobal(pos));
}

// --- Ouvrir, télécharger ------------------------------------------------------------
void DriveView::openFile(const DriveFile &f)
{
    if (f.trashed) {
        emit statusMessage("Cet élément est dans la corbeille : restaurez-le pour l'ouvrir.", 5000);
        return;
    }
    if (f.isFolder()) {
        m_browser->openFolder(f);
        return;
    }
    if (f.isGoogleFile()) { // Docs, Sheets… s'ouvrent dans leur éditeur en ligne
        QDesktopServices::openUrl(QUrl(f.webViewLink));
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                        + QString("/gdesk-%1/drive-%2").arg(QCoreApplication::applicationPid()).arg(f.id);
    QDir().mkpath(dir);
    const QString path = dir + "/" + safeFileName(DriveApi::downloadName(f));
    QPointer<DriveView> self(this);
    emit statusMessage(QString("Ouverture de « %1 »…").arg(f.name));
    m_api->downloadToFile(f, path,
        [self, path, name = f.name](const QJsonObject &, const QString &err) {
            if (!self)
                return;
            if (!err.isEmpty()) {
                emit self->statusMessage(QString("Impossible d'ouvrir « %1 » : %2").arg(name, err), 10000);
                return;
            }
            emit self->statusMessage({}, 1);
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        },
        [self, name = f.name](qint64 done, qint64 total) {
            if (self && percent(done, total) >= 0)
                emit self->statusMessage(QString("Ouverture de « %1 »… %2 %").arg(name).arg(percent(done, total)));
        });
}

void DriveView::downloadSelected()
{
    const QList<DriveFile> files = downloadableSelection();
    if (files.isEmpty())
        return;
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QList<QPair<DriveFile, QString>> queue;
    if (files.size() == 1) {
        const QString path = QFileDialog::getSaveFileName(this, "Télécharger",
                                                          downloads + "/" + safeFileName(DriveApi::downloadName(files.first())));
        if (path.isEmpty())
            return;
        queue.append({files.first(), path});
    } else {
        const QString dir = QFileDialog::getExistingDirectory(this, "Télécharger dans", downloads);
        if (dir.isEmpty())
            return;
        QSet<QString> used;
        for (const DriveFile &f : files) {
            const QFileInfo info(safeFileName(DriveApi::downloadName(f)));
            QString name = info.fileName();
            // Noms en double dans Drive, ou fichier déjà présent : « rapport (2).pdf »
            for (int n = 2; used.contains(name) || QFileInfo::exists(dir + "/" + name); ++n)
                name = QString("%1 (%2)%3").arg(info.completeBaseName()).arg(n)
                           .arg(info.suffix().isEmpty() ? QString() : "." + info.suffix());
            used.insert(name);
            queue.append({f, dir + "/" + name});
        }
    }
    downloadNext(queue, 0);
}

void DriveView::downloadNext(const QList<QPair<DriveFile, QString>> &queue, int index)
{
    if (index >= queue.size()) {
        emit statusMessage(queue.size() == 1
                               ? QString("Téléchargé : %1").arg(queue.first().second)
                               : QString("%1 fichiers téléchargés dans %2")
                                     .arg(queue.size())
                                     .arg(QFileInfo(queue.first().second).absolutePath()),
                           8000);
        return;
    }
    const DriveFile f = queue.at(index).first;
    const QString prefix = queue.size() > 1
                               ? QString("Téléchargement %1/%2 : « %3 »").arg(index + 1).arg(queue.size()).arg(f.name)
                               : QString("Téléchargement de « %1 »").arg(f.name);
    emit statusMessage(prefix + "…");
    QPointer<DriveView> self(this);
    m_api->downloadToFile(f, queue.at(index).second,
        [self, queue, index, name = f.name](const QJsonObject &, const QString &err) {
            if (!self)
                return;
            if (!err.isEmpty()) {
                emit self->statusMessage({}, 1);
                QMessageBox::warning(self, "Télécharger", QString("Impossible de télécharger « %1 » :\n%2").arg(name, err));
                return;
            }
            self->downloadNext(queue, index + 1);
        },
        [self, prefix](qint64 done, qint64 total) {
            if (self && percent(done, total) >= 0)
                emit self->statusMessage(QString("%1… %2 %").arg(prefix).arg(percent(done, total)));
        });
}

// --- Importer -----------------------------------------------------------------------
void DriveView::importFiles()
{
    if (!m_browser->canAddHere())
        return;
    const QStringList files = QFileDialog::getOpenFileNames(this, "Importer dans Google Drive",
                                                            QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    uploadPaths(files, m_browser->currentFolder());
}

void DriveView::uploadPaths(const QStringList &paths, const DriveFile &folder)
{
    QStringList files;
    int folders = 0;
    for (const QString &p : paths) {
        if (QFileInfo(p).isFile())
            files << p;
        else
            ++folders;
    }
    if (folders > 0)
        emit statusMessage("Les dossiers ne peuvent pas être importés : glissez plutôt les fichiers qu'ils contiennent.",
                           8000);
    if (!files.isEmpty())
        uploadNext(files, folder, 0);
}

void DriveView::uploadNext(const QStringList &queue, const DriveFile &folder, int index)
{
    if (index >= queue.size()) {
        emit statusMessage(queue.size() == 1
                               ? QString("« %1 » importé dans « %2 ».").arg(QFileInfo(queue.first()).fileName(), folder.name)
                               : QString("%1 fichiers importés dans « %2 ».").arg(queue.size()).arg(folder.name),
                           6000);
        return;
    }
    const QString name = QFileInfo(queue.at(index)).fileName();
    const QString prefix = queue.size() > 1
                               ? QString("Importation %1/%2 : « %3 »").arg(index + 1).arg(queue.size()).arg(name)
                               : QString("Importation de « %1 »").arg(name);
    emit statusMessage(prefix + "…");
    QPointer<DriveView> self(this);
    m_api->uploadFile(queue.at(index), folder.contentId(),
        [self, queue, folder, index, name](const QJsonObject &, const QString &err) {
            if (!self)
                return;
            if (!err.isEmpty()) {
                emit self->statusMessage({}, 1);
                QMessageBox::warning(self, "Importer", QString("Impossible d'importer « %1 » :\n%2").arg(name, err));
                return;
            }
            self->uploadNext(queue, folder, index + 1);
        },
        [self, prefix](qint64 done, qint64 total) {
            if (self && percent(done, total) >= 0)
                emit self->statusMessage(QString("%1… %2 %").arg(prefix).arg(percent(done, total)));
        });
}

// --- Organiser --------------------------------------------------------------------
void DriveView::newFolder()
{
    if (!m_browser->canAddHere())
        return;
    const DriveFile parentFolder = m_browser->currentFolder();
    const QString name = QInputDialog::getText(this, "Nouveau dossier", "Nom du dossier :").trimmed();
    if (name.isEmpty())
        return;
    QPointer<DriveView> self(this);
    m_api->createFolder(name, parentFolder.contentId(), [self, name](const QJsonObject &, const QString &err) {
        if (!self)
            return;
        if (!err.isEmpty())
            QMessageBox::warning(self, "Nouveau dossier", err);
        else
            emit self->statusMessage(QString("Dossier « %1 » créé.").arg(name), 4000);
    });
}

void DriveView::rename()
{
    const QList<DriveFile> sel = m_browser->selectedFiles();
    if (sel.size() != 1 || !sel.first().canRename)
        return;
    const DriveFile f = sel.first();
    QInputDialog dlg(this);
    dlg.setWindowTitle("Renommer");
    dlg.setLabelText("Nouveau nom :");
    dlg.setTextValue(f.name);
    // Comme dans Dolphin : seul le nom est sélectionné, pas l'extension
    QTimer::singleShot(0, &dlg, [&dlg, f] {
        if (auto *edit = dlg.findChild<QLineEdit *>()) {
            const int dot = f.isFolder() || f.isGoogleFile() ? -1 : int(f.name.lastIndexOf('.'));
            edit->setSelection(0, dot > 0 ? dot : int(f.name.size()));
        }
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.textValue().trimmed();
    if (name.isEmpty() || name == f.name)
        return;
    QPointer<DriveView> self(this);
    m_api->updateFile(f.id, {{"name", name}}, [self](const QJsonObject &obj, const QString &err) {
        if (!self)
            return;
        if (!err.isEmpty())
            QMessageBox::warning(self, "Renommer", err);
        else
            self->m_browser->updateFile(DriveFile::fromJson(obj));
    });
}

void DriveView::toggleStar()
{
    const QList<DriveFile> sel = m_browser->selectedFiles();
    if (sel.isEmpty())
        return;
    const bool star = std::any_of(sel.begin(), sel.end(), [](const DriveFile &f) { return !f.starred; });
    QPointer<DriveView> self(this);
    for (const DriveFile &f : sel)
        m_api->updateFile(f.id, {{"starred", star}}, [self, star](const QJsonObject &obj, const QString &err) {
            if (!self)
                return;
            if (!err.isEmpty()) {
                emit self->statusMessage("Opération impossible : " + err, 8000);
                return;
            }
            const DriveFile updated = DriveFile::fromJson(obj);
            if (!star && self->m_browser->place() == DriveApi::Starred && self->m_browser->searchText().isEmpty())
                self->m_browser->removeFiles({updated.id});
            else
                self->m_browser->updateFile(updated);
        });
}

void DriveView::setTrashed(bool trashed)
{
    QList<DriveFile> sel = m_browser->selectedFiles();
    if (trashed)
        sel.removeIf([](const DriveFile &f) { return !f.canTrash; });
    if (sel.isEmpty())
        return;
    QStringList ids;
    for (const DriveFile &f : std::as_const(sel))
        ids << f.id;
    m_browser->removeFiles(ids);
    const int n = sel.size();
    emit statusMessage(trashed ? (n > 1 ? QString("%1 éléments placés dans la corbeille.").arg(n)
                                        : QString("« %1 » placé dans la corbeille.").arg(sel.first().name))
                               : (n > 1 ? QString("%1 éléments restaurés.").arg(n)
                                        : QString("« %1 » restauré.").arg(sel.first().name)),
                       5000);
    QPointer<DriveView> self(this);
    for (const DriveFile &f : std::as_const(sel))
        m_api->updateFile(f.id, {{"trashed", trashed}}, [self](const QJsonObject &, const QString &err) {
            if (!self || err.isEmpty())
                return;
            emit self->statusMessage("Opération impossible : " + err, 10000);
            self->m_browser->reload();
        });
    refreshQuota();
}
