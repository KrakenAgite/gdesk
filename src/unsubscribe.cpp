#include "unsubscribe.h"
#include "gmailapi.h"
#include "mime.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QVBoxLayout>

static bool s_allowHttp = false;
static constexpr int MaxMessages = 500;  // messages analysés
static constexpr int Parallel = 8;       // lectures d'en-têtes simultanées
static constexpr const char *UnsubscribedKey = "unsubscribed_senders";

enum { NewsletterRole = Qt::UserRole + 1 };

// Élément trié par valeur (nombre, date) plutôt que par texte
class NewsletterItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == UnsubscribeDialog::CountColumn || col == UnsubscribeDialog::DateColumn) {
            const QVariant a = data(col, Qt::UserRole), b = other.data(col, Qt::UserRole);
            return col == UnsubscribeDialog::CountColumn ? a.toInt() < b.toInt() : a.toDateTime() < b.toDateTime();
        }
        return text(col).localeAwareCompare(other.text(col)) < 0;
    }
};

// =============================================================================
//  En-têtes List-Unsubscribe
// =============================================================================

UnsubscribeMethods UnsubscribeMethods::parse(const QString &listUnsubscribe, const QString &listUnsubscribePost)
{
    UnsubscribeMethods m;
    // « <mailto:x@y.fr?subject=unsubscribe>, <https://exemple.fr/u?id=1> » : adresses entre chevrons
    const bool oneClickAllowed = listUnsubscribePost.simplified().compare("List-Unsubscribe=One-Click", Qt::CaseInsensitive) == 0;
    qsizetype pos = 0;
    while ((pos = listUnsubscribe.indexOf('<', pos)) >= 0) {
        const qsizetype end = listUnsubscribe.indexOf('>', pos);
        if (end < 0)
            break;
        const QUrl url(listUnsubscribe.mid(pos + 1, end - pos - 1).simplified().remove(' '), QUrl::StrictMode);
        pos = end + 1;
        if (!url.isValid())
            continue;
        const QString scheme = url.scheme().toLower();
        if (scheme == "mailto" && m.mailto.isEmpty() && url.path().contains('@')) {
            m.mailto = url;
        } else if ((scheme == "https" || scheme == "http") && !url.host().isEmpty()) {
            // Désabonnement en un clic : https exigé par la RFC 8058 (la page web peut être en http)
            if (oneClickAllowed && m.oneClick.isEmpty() && (scheme == "https" || s_allowHttp))
                m.oneClick = url;
            if (m.web.isEmpty())
                m.web = url;
        }
    }
    return m;
}

QString UnsubscribeMethods::description() const
{
    if (!oneClick.isEmpty())
        return "En un clic";
    if (!mailto.isEmpty())
        return "Par e-mail";
    return "Page web";
}

void UnsubscribeDialog::setAllowHttpForTesting(bool allow)
{
    s_allowHttp = allow;
}

QString UnsubscribeDialog::scanQuery()
{
    return "newer_than:1y -in:sent -in:drafts -in:chats "
           "(category:promotions OR category:updates OR category:social OR category:forums "
           "OR unsubscribe OR newsletter OR désabonner OR désinscrire OR désabonnement OR désinscription)";
}

QList<Newsletter> UnsubscribeDialog::group(const QList<QJsonObject> &messages, const QSet<QString> &ignored)
{
    QHash<QString, Newsletter> bySender;
    for (const QJsonObject &json : messages) {
        QString listUnsubscribe, listPost;
        for (const QJsonValue &h : json.value("payload").toObject().value("headers").toArray()) {
            const QString name = h.toObject().value("name").toString();
            if (name.compare("List-Unsubscribe", Qt::CaseInsensitive) == 0)
                listUnsubscribe = h.toObject().value("value").toString();
            else if (name.compare("List-Unsubscribe-Post", Qt::CaseInsensitive) == 0)
                listPost = h.toObject().value("value").toString();
        }
        const UnsubscribeMethods methods = UnsubscribeMethods::parse(listUnsubscribe, listPost);
        if (!methods.isValid())
            continue;
        const MailMessage m = Mime::parseMessage(json);
        const QString address = Mime::emailOnly(m.from).toLower();
        if (address.isEmpty() || ignored.contains(address))
            continue;
        Newsletter &n = bySender[address];
        n.address = address;
        ++n.count;
        if (!n.last.isValid() || m.date > n.last) { // le message le plus récent fait foi
            n.last = m.date;
            n.lastSubject = m.subject;
            n.methods = methods;
            const QString name = Mime::displayName(m.from);
            n.name = name.isEmpty() ? address : name;
        }
    }
    QList<Newsletter> list = bySender.values();
    std::sort(list.begin(), list.end(), [](const Newsletter &a, const Newsletter &b) {
        return a.count != b.count ? a.count > b.count : a.last > b.last;
    });
    return list;
}

// =============================================================================
//  Fenêtre
// =============================================================================

UnsubscribeDialog::UnsubscribeDialog(GmailApi *api, const QString &myAddress, QWidget *parent)
    : QDialog(parent),
      m_api(api),
      m_nam(new QNetworkAccessManager(this)),
      m_myAddress(myAddress)
{
    setWindowTitle("Se désabonner des listes de diffusion");
    setWindowIcon(QIcon::fromTheme("news-unsubscribe"));
    resize(860, 560);
    auto *v = new QVBoxLayout(this);

    m_stack = new QStackedWidget;
    v->addWidget(m_stack, 1);

    // Page 0 : analyse
    auto *scanning = new QWidget;
    auto *sv = new QVBoxLayout(scanning);
    sv->addStretch();
    m_progressLabel = new QLabel("Recherche des listes de diffusion…");
    m_progressLabel->setAlignment(Qt::AlignCenter);
    sv->addWidget(m_progressLabel);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 0);
    m_progress->setMaximumWidth(420);
    sv->addWidget(m_progress, 0, Qt::AlignHCenter);
    sv->addStretch();
    m_stack->addWidget(scanning);

    // Page 1 : résultats
    auto *results = new QWidget;
    auto *rv = new QVBoxLayout(results);
    rv->setContentsMargins(0, 0, 0, 0);
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    rv->addWidget(m_summary);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Filtrer par nom ou adresse");
    m_filter->setClearButtonEnabled(true);
    m_filter->addAction(QIcon::fromTheme("search"), QLineEdit::LeadingPosition);
    rv->addWidget(m_filter);
    m_list = new QTreeWidget;
    m_list->setObjectName("newsletterList");
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setHeaderLabels({"Expéditeur", "Adresse", "Messages", "Dernier envoi", "Désabonnement"});
    m_list->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(AddressColumn, QHeaderView::Stretch);
    for (int c : {CountColumn, DateColumn, MethodColumn})
        m_list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_list->setSortingEnabled(true);
    rv->addWidget(m_list, 1);
    m_stack->addWidget(results);

    // Page 2 : rien trouvé ou erreur
    m_emptyLabel = new QLabel;
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_stack->addWidget(m_emptyLabel);

    auto *buttons = new QHBoxLayout;
    m_checkAll = new QPushButton(QIcon::fromTheme("edit-select-all"), "Tout cocher");
    m_checkNone = new QPushButton(QIcon::fromTheme("edit-select-none"), "Tout décocher");
    m_unsubscribe = new QPushButton(QIcon::fromTheme("news-unsubscribe"), "Se désabonner");
    m_unsubscribe->setObjectName("unsubscribeButton");
    m_unsubscribe->setDefault(true);
    auto *close = new QPushButton(QIcon::fromTheme("dialog-close"), "Fermer");
    buttons->addWidget(m_checkAll);
    buttons->addWidget(m_checkNone);
    buttons->addStretch();
    buttons->addWidget(m_unsubscribe);
    buttons->addWidget(close);
    v->addLayout(buttons);

    auto setAll = [this](bool checked) {
        for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = m_list->topLevelItem(i);
            if (!it->isHidden() && (it->flags() & Qt::ItemIsUserCheckable))
                it->setCheckState(NameColumn, checked ? Qt::Checked : Qt::Unchecked);
        }
    };
    connect(m_checkAll, &QPushButton::clicked, this, [setAll] { setAll(true); });
    connect(m_checkNone, &QPushButton::clicked, this, [setAll] { setAll(false); });
    connect(m_unsubscribe, &QPushButton::clicked, this, &UnsubscribeDialog::unsubscribeChecked);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_list, &QTreeWidget::itemChanged, this, &UnsubscribeDialog::updateButtons);
    // Espace ou double-clic : coche ou décoche les lignes sélectionnées
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *it) {
        if (it->flags() & Qt::ItemIsUserCheckable)
            it->setCheckState(NameColumn, it->checkState(NameColumn) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = m_list->topLevelItem(i);
            it->setHidden(!text.isEmpty() && !it->text(NameColumn).contains(text, Qt::CaseInsensitive)
                          && !it->text(AddressColumn).contains(text, Qt::CaseInsensitive));
        }
    });
    connect(this, &QDialog::finished, this, [this] { m_cancelled = true; });

    updateButtons();
    scan();
}

QSet<QString> UnsubscribeDialog::unsubscribedSenders() const
{
    const QStringList list = QSettings("gdesk", "gdesk").value(UnsubscribedKey).toStringList();
    return QSet<QString>(list.begin(), list.end());
}

void UnsubscribeDialog::scan()
{
    m_stack->setCurrentIndex(0);
    m_pending.clear();
    m_messages.clear();
    fetchIds({});
}

void UnsubscribeDialog::fetchIds(const QString &pageToken)
{
    m_api->listMessages({}, scanQuery(), pageToken, MaxMessages - int(m_pending.size()),
                        [this, self = QPointer(this)](const QJsonObject &r, const QString &err) {
        if (!self || m_cancelled)
            return;
        if (!err.isEmpty()) {
            m_emptyLabel->setText("Impossible de rechercher les listes de diffusion :\n" + err);
            m_stack->setCurrentIndex(2);
            return;
        }
        for (const QJsonValue &v : r.value("messages").toArray())
            m_pending << v.toObject().value("id").toString();
        const QString next = r.value("nextPageToken").toString();
        if (!next.isEmpty() && m_pending.size() < MaxMessages) {
            fetchIds(next);
            return;
        }
        m_total = int(m_pending.size());
        m_progress->setRange(0, qMax(1, m_total));
        m_progress->setValue(0);
        if (m_total == 0) {
            showResults();
            return;
        }
        for (int i = 0; i < Parallel; ++i)
            fetchNextHeaders();
    });
}

void UnsubscribeDialog::fetchNextHeaders()
{
    if (m_cancelled)
        return;
    if (m_pending.isEmpty()) {
        if (m_inFlight == 0)
            showResults();
        return;
    }
    const QString id = m_pending.takeFirst();
    ++m_inFlight;
    m_api->getMessageHeaders(id, {"From", "Subject", "List-Unsubscribe", "List-Unsubscribe-Post"},
                             [this, self = QPointer(this)](const QJsonObject &r, const QString &err) {
        if (!self)
            return;
        --m_inFlight;
        if (err.isEmpty())
            m_messages << r; // un message illisible est simplement ignoré
        const int done = m_total - int(m_pending.size()) - m_inFlight;
        m_progress->setValue(done);
        m_progressLabel->setText(QString("Analyse des messages reçus… %1 / %2").arg(done).arg(m_total));
        fetchNextHeaders();
    });
}

void UnsubscribeDialog::showResults()
{
    const QSet<QString> ignored = unsubscribedSenders();
    m_found = group(m_messages, ignored);
    m_messages.clear();
    if (m_found.isEmpty()) {
        m_emptyLabel->setText("Aucune liste de diffusion trouvée dans les messages reçus depuis un an.");
        m_stack->setCurrentIndex(2);
        updateButtons();
        return;
    }
    const QSignalBlocker block(m_list);
    m_list->setSortingEnabled(false);
    m_list->clear();
    for (int i = 0; i < m_found.size(); ++i) {
        const Newsletter &n = m_found[i];
        auto *it = new NewsletterItem(m_list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(NameColumn, Qt::Unchecked);
        it->setData(NameColumn, NewsletterRole, i);
        it->setText(NameColumn, n.name);
        it->setToolTip(NameColumn, n.lastSubject.isEmpty() ? n.name : QString("Dernier message : %1").arg(n.lastSubject));
        it->setText(AddressColumn, n.address);
        it->setToolTip(AddressColumn, n.address);
        it->setText(CountColumn, QString::number(n.count));
        it->setData(CountColumn, Qt::UserRole, n.count);
        it->setTextAlignment(CountColumn, Qt::AlignRight | Qt::AlignVCenter);
        it->setText(DateColumn, Mime::shortDate(n.last));
        it->setData(DateColumn, Qt::UserRole, n.last);
        it->setText(MethodColumn, n.methods.description());
        it->setToolTip(MethodColumn, !n.methods.oneClick.isEmpty()
                                         ? "Désabonnement immédiat, sans ouvrir le navigateur"
                                         : !n.methods.mailto.isEmpty()
                                               ? QString("Un e-mail de désabonnement sera envoyé à %1").arg(n.methods.mailto.path())
                                               : "La page de désabonnement s'ouvrira dans votre navigateur pour confirmer");
    }
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(CountColumn, Qt::DescendingOrder);
    m_summary->setText(QString("<b>%1 listes de diffusion</b> trouvées dans les %2 derniers messages reçus. "
                               "Cochez celles dont vous voulez vous désabonner.")
                           .arg(m_found.size())
                           .arg(m_total));
    m_stack->setCurrentIndex(1);
    updateButtons();
    m_list->setFocus();
}

void UnsubscribeDialog::updateButtons()
{
    int checked = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        checked += m_list->topLevelItem(i)->checkState(NameColumn) == Qt::Checked
                   && (m_list->topLevelItem(i)->flags() & Qt::ItemIsUserCheckable);
    const bool ready = m_stack->currentIndex() == 1 && m_running == 0;
    m_unsubscribe->setEnabled(ready && checked > 0);
    m_unsubscribe->setText(checked > 0 ? QString("Se désabonner (%1)").arg(checked) : QString("Se désabonner"));
    m_checkAll->setEnabled(ready);
    m_checkNone->setEnabled(ready);
}

// =============================================================================
//  Désabonnement
// =============================================================================

void UnsubscribeDialog::unsubscribeChecked()
{
    QList<QTreeWidgetItem *> items;
    int web = 0, mail = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_list->topLevelItem(i);
        if (it->checkState(NameColumn) != Qt::Checked || !(it->flags() & Qt::ItemIsUserCheckable))
            continue;
        items << it;
        const UnsubscribeMethods &m = m_found[it->data(NameColumn, NewsletterRole).toInt()].methods;
        if (m.oneClick.isEmpty())
            (m.mailto.isEmpty() ? web : mail)++;
    }
    if (items.isEmpty())
        return;
    QString details;
    if (mail > 0)
        details += QString("\n• %1 par un e-mail de désabonnement envoyé depuis votre adresse").arg(mail);
    if (web > 0)
        details += QString("\n• %1 par une page web ouverte dans votre navigateur, où il faudra peut-être confirmer").arg(web);
    const QString question = items.size() == 1
        ? QString("Se désabonner de « %1 » ?").arg(items.first()->text(NameColumn))
        : QString("Se désabonner de %1 listes de diffusion ?").arg(items.size());
    if (QMessageBox::question(this, "Se désabonner", question + (details.isEmpty() ? QString() : "\n" + details))
        != QMessageBox::Yes)
        return;
    for (QTreeWidgetItem *it : std::as_const(items))
        unsubscribe(it);
}

void UnsubscribeDialog::unsubscribe(QTreeWidgetItem *item)
{
    const Newsletter &n = m_found[item->data(NameColumn, NewsletterRole).toInt()];
    item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    item->setData(NameColumn, Qt::CheckStateRole, QVariant()); // plus de case : traité
    item->setIcon(MethodColumn, QIcon::fromTheme("view-refresh"));
    item->setText(MethodColumn, "En cours…");
    ++m_running;
    updateButtons();
    if (!n.methods.oneClick.isEmpty())
        oneClick(item, n);
    else if (!n.methods.mailto.isEmpty())
        sendMail(item, n);
    else
        openWeb(item, n);
}

UnsubscribeDialog::~UnsubscribeDialog()
{
    m_destroying = true;
    delete m_nam; // requêtes en cours annulées tant que la liste existe encore
}

void UnsubscribeDialog::oneClick(QTreeWidgetItem *item, const Newsletter &n)
{
    QNetworkRequest req(n.methods.oneClick);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setHeader(QNetworkRequest::UserAgentHeader, "G-Desk");
    req.setTransferTimeout(20000);
    QNetworkReply *reply = m_nam->post(req, "List-Unsubscribe=One-Click");
    connect(reply, &QNetworkReply::finished, this, [this, reply, item, n] {
        reply->deleteLater();
        if (m_destroying)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 200 && status < 300) {
            done(item, true, "Désabonné", "dialog-ok-apply");
        } else if (!n.methods.mailto.isEmpty()) { // l'expéditeur propose aussi un e-mail
            sendMail(item, n);
        } else if (!n.methods.web.isEmpty()) {
            openWeb(item, n);
        } else {
            done(item, false, status ? QString("Refusé (erreur %1)").arg(status) : reply->errorString(), "dialog-error");
        }
    });
}

void UnsubscribeDialog::sendMail(QTreeWidgetItem *item, const Newsletter &n)
{
    const QUrlQuery q(n.methods.mailto);
    auto value = [&q](const char *key) {
        for (const auto &[k, v] : q.queryItems(QUrl::FullyDecoded))
            if (k.compare(key, Qt::CaseInsensitive) == 0)
                return v;
        return QString();
    };
    OutgoingMail mail;
    mail.from = m_myAddress;
    mail.to = n.methods.mailto.path(QUrl::FullyDecoded);
    mail.subject = value("subject").isEmpty() ? QString("unsubscribe") : value("subject");
    mail.body = value("body").isEmpty() ? QString("unsubscribe") : value("body");
    m_api->sendMessage(Mime::build(mail), {}, [this, self = QPointer(this), item, n](const QJsonObject &, const QString &err) {
        if (!self)
            return;
        if (err.isEmpty())
            done(item, true, "Demande envoyée par e-mail", "mail-sent");
        else if (!n.methods.web.isEmpty())
            openWeb(item, n);
        else
            done(item, false, "Échec : " + err, "dialog-error");
    });
}

void UnsubscribeDialog::openWeb(QTreeWidgetItem *item, const Newsletter &n)
{
    if (QDesktopServices::openUrl(n.methods.web))
        done(item, true, "À confirmer dans le navigateur", "internet-web-browser");
    else
        done(item, false, "Impossible d'ouvrir le navigateur", "dialog-error");
}

void UnsubscribeDialog::done(QTreeWidgetItem *item, bool ok, const QString &status, const QString &icon)
{
    const Newsletter &n = m_found[item->data(NameColumn, NewsletterRole).toInt()];
    item->setIcon(MethodColumn, QIcon::fromTheme(icon));
    item->setText(MethodColumn, status);
    item->setToolTip(MethodColumn, status);
    if (ok) { // n'est plus proposé lors des prochaines recherches
        QSettings settings("gdesk", "gdesk");
        QStringList list = settings.value(UnsubscribedKey).toStringList();
        if (!list.contains(n.address)) {
            list << n.address;
            settings.setValue(UnsubscribedKey, list);
        }
        for (int c = 0; c < m_list->columnCount(); ++c)
            item->setForeground(c, palette().brush(QPalette::Disabled, QPalette::Text));
    } else { // on peut réessayer
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(NameColumn, Qt::Unchecked);
    }
    --m_running;
    updateButtons();
}
