#include "settingsdialog.h"
#include "theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

// =============================================================================
//  Miniatures
// =============================================================================
namespace {

// Dessine une fenêtre G-Desk miniature : barre d'outils, dossiers, liste, aperçu
void drawMiniWindow(QPainter &p, const QRectF &r, const QPalette &pal, const QString &layout, const QString &density,
                    double listShare = 0.45)
{
    const QColor window = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor mid = pal.color(QPalette::Mid);
    QColor text = pal.color(QPalette::Text);
    text.setAlphaF(0.45);
    QColor strongText = pal.color(QPalette::Text);
    strongText.setAlphaF(0.75);

    QPainterPath clip;
    clip.addRoundedRect(r, 6, 6);
    p.save();
    p.setClipPath(clip, Qt::IntersectClip); // conserve un éventuel découpage (vignette « Comme KDE »)
    p.fillRect(r, window);

    // Barre d'outils
    const QRectF toolbar(r.left(), r.top(), r.width(), r.height() * 0.14);
    p.fillRect(toolbar, window.darker(window.lightness() < 128 ? 80 : 104));
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(QRectF(toolbar.left() + 6, toolbar.center().y() - 2.5, 14, 5), 2, 2);
    p.setBrush(text);
    for (int i = 0; i < 3; ++i)
        p.drawRoundedRect(QRectF(toolbar.left() + 26 + i * 12, toolbar.center().y() - 2, 8, 4), 2, 2);

    // Dossiers
    const QRectF content(r.left(), toolbar.bottom(), r.width(), r.bottom() - toolbar.bottom());
    const QRectF sidebar(content.left(), content.top(), content.width() * 0.22, content.height());
    for (int i = 0; i < 5; ++i) {
        p.setBrush(i == 0 ? accent : text);
        p.drawRoundedRect(QRectF(sidebar.left() + 5, sidebar.top() + 6 + i * 9, sidebar.width() * (i == 0 ? 0.7 : 0.55), 3.5), 1.5, 1.5);
    }

    // Liste et aperçu
    const QRectF area(sidebar.right(), content.top(), content.right() - sidebar.right(), content.height());
    p.fillRect(area, base);
    QRectF list, preview;
    if (layout == "right") {
        list = QRectF(area.left(), area.top(), area.width() * listShare, area.height());
        preview = QRectF(list.right() + 1, area.top(), area.right() - list.right() - 1, area.height());
        p.fillRect(QRectF(list.right(), area.top(), 1, area.height()), mid);
    } else {
        list = QRectF(area.left(), area.top(), area.width(), area.height() * listShare);
        preview = QRectF(area.left(), list.bottom() + 1, area.width(), area.bottom() - list.bottom() - 1);
        p.fillRect(QRectF(area.left(), list.bottom(), area.width(), 1), mid);
    }
    p.fillRect(QRectF(sidebar.right(), content.top(), 1, content.height()), mid);

    const double rowH = density == "compact" ? 5.5 : density == "spacious" ? 12 : 8.5;
    int row = 0;
    for (double y = list.top() + 2; y + rowH <= list.bottom() - 1; y += rowH, ++row) {
        const QRectF rowRect(list.left() + 2, y, list.width() - 4, rowH - 1);
        if (row == 0) {
            QColor sel = accent;
            sel.setAlphaF(0.35);
            p.setBrush(sel);
            p.drawRoundedRect(rowRect, 2, 2);
        }
        const double lineH = qMin(2.6, rowH * 0.4);
        p.setBrush(row == 0 ? strongText : text);
        p.drawRoundedRect(QRectF(rowRect.left() + 3, rowRect.center().y() - lineH / 2,
                                 rowRect.width() * (0.28 + 0.07 * (row % 3)), lineH), 1, 1);
        p.drawRoundedRect(QRectF(rowRect.left() + rowRect.width() * 0.45, rowRect.center().y() - lineH / 2,
                                 rowRect.width() * 0.4, lineH), 1, 1);
    }

    // Aperçu : objet puis lignes de texte
    p.setBrush(strongText);
    p.drawRoundedRect(QRectF(preview.left() + 5, preview.top() + 5, preview.width() * 0.55, 4), 2, 2);
    p.setBrush(text);
    const double widths[] = {0.85, 0.78, 0.9, 0.6, 0.82, 0.7, 0.88, 0.5};
    int i = 0;
    for (double y = preview.top() + 14; y + 3 <= preview.bottom() - 3 && i < 8; y += 6, ++i)
        p.drawRoundedRect(QRectF(preview.left() + 5, y, (preview.width() - 10) * widths[i], 2.2), 1, 1);

    p.restore();
    p.setPen(QPen(mid, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 6, 6);
}

} // namespace

PreviewCard::PreviewCard(Kind kind, const QString &value, const QString &label, QWidget *parent)
    : QAbstractButton(parent), m_kind(kind), m_value(value)
{
    setText(label);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(label);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void PreviewCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(6, 6, -6, -30);
    const QPalette current = palette();

    switch (m_kind) {
    case ThemeCard:
        if (m_value == "system") {
            // Moitié claire, moitié sombre
            drawMiniWindow(p, frame, Theme::palette(false), "below", "comfortable");
            QPainterPath half;
            half.moveTo(frame.center().x() + 18, frame.top());
            half.lineTo(frame.right() + 1, frame.top());
            half.lineTo(frame.right() + 1, frame.bottom() + 1);
            half.lineTo(frame.center().x() - 18, frame.bottom() + 1);
            half.closeSubpath();
            p.save();
            p.setClipPath(half);
            drawMiniWindow(p, frame, Theme::palette(true), "below", "comfortable");
            p.restore();
        } else {
            drawMiniWindow(p, frame, Theme::palette(m_value == "dark"), "below", "comfortable");
        }
        break;
    case DensityCard:
        drawMiniWindow(p, frame, current, "below", m_value, 0.62);
        break;
    case LayoutCard:
        drawMiniWindow(p, frame, current, m_value, "comfortable");
        break;
    }

    // Cadre de sélection
    if (isChecked() || underMouse()) {
        QColor c = current.color(QPalette::Highlight);
        if (!isChecked())
            c.setAlphaF(0.45);
        p.setPen(QPen(c, isChecked() ? 2.5 : 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame.adjusted(-3.5, -3.5, 3.5, 3.5), 8, 8);
    }
    if (hasFocus()) {
        p.setPen(QPen(current.color(QPalette::Highlight), 1, Qt::DotLine));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 9, 9);
    }

    // Libellé
    QFont f = font();
    f.setBold(isChecked());
    p.setFont(f);
    p.setPen(current.color(QPalette::WindowText));
    p.drawText(QRectF(0, frame.bottom() + 6, width(), 22), Qt::AlignCenter, text());
}

// =============================================================================
//  Fenêtre des paramètres
// =============================================================================
SettingsDialog::SettingsDialog(QSettings &settings, const QString &email, QWidget *parent)
    : QDialog(parent), m_settings(settings)
{
    setWindowTitle("Paramètres — G-Desk");
    resize(820, 620);

    auto *nav = new QListWidget;
    nav->setIconSize(QSize(22, 22));
    nav->setFixedWidth(180);
    nav->setSpacing(2);
    auto *pages = new QStackedWidget;
    auto addPage = [&](const char *icon, const QString &name, QWidget *page) {
        nav->addItem(new QListWidgetItem(QIcon::fromTheme(icon), name));
        auto *scroll = new QScrollArea;
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        pages->addWidget(scroll);
    };
    addPage("preferences-desktop-theme", "Affichage", displayPage());
    addPage("user-identity", "Compte", accountPage(email));
    addPage("preferences-system", "Général", generalPage());
    addPage("security-high", "Confidentialité", privacyPage());
    connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(0);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::save);

    auto *top = new QHBoxLayout;
    top->addWidget(nav);
    top->addWidget(pages, 1);
    auto *v = new QVBoxLayout(this);
    v->addLayout(top, 1);
    v->addWidget(buttons);

    load();
}

QButtonGroup *SettingsDialog::addCards(QLayout *layout, PreviewCard::Kind kind,
                                       const QList<QPair<QString, QString>> &options)
{
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    auto *row = new QHBoxLayout;
    for (const auto &[value, label] : options) {
        auto *card = new PreviewCard(kind, value, label);
        group->addButton(card);
        row->addWidget(card);
    }
    row->addStretch(1);
    static_cast<QBoxLayout *>(layout)->addLayout(row);
    return group;
}

static QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setEnabled(false);
    return l;
}

QWidget *SettingsDialog::displayPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *themeBox = new QGroupBox("Thème");
    auto *tv = new QVBoxLayout(themeBox);
    m_theme = addCards(tv, PreviewCard::ThemeCard,
                       {{"system", "Comme KDE"}, {"light", "Clair"}, {"dark", "Sombre"}});
    m_darkMessages = new QCheckBox("Assombrir aussi le contenu des e-mails en thème sombre");
    tv->addWidget(m_darkMessages);
    tv->addWidget(hint("La plupart des e-mails sont conçus sur fond blanc : cette option les convertit "
                       "automatiquement en couleurs sombres. Le rendu peut varier selon les messages."));
    v->addWidget(themeBox);

    auto *densityBox = new QGroupBox("Taille des lignes de la liste");
    auto *dv = new QVBoxLayout(densityBox);
    m_density = addCards(dv, PreviewCard::DensityCard,
                         {{"compact", "Compacte"}, {"comfortable", "Aérée"}, {"spacious", "Espacée"}});
    m_showSnippet = new QCheckBox("Afficher le début du message après l'objet");
    dv->addWidget(m_showSnippet);
    v->addWidget(densityBox);

    auto *layoutBox = new QGroupBox("Disposition de la liste et de l'aperçu");
    auto *lv = new QVBoxLayout(layoutBox);
    m_layout = addCards(lv, PreviewCard::LayoutCard,
                        {{"below", "Aperçu en dessous"}, {"right", "Aperçu à droite"}});
    lv->addWidget(hint("« Aperçu à droite » convient aux écrans larges : la liste et le message s'affichent côte à côte."));
    v->addWidget(layoutBox);

    auto *readBox = new QGroupBox("Lecture");
    auto *rv = new QHBoxLayout(readBox);
    rv->addWidget(new QLabel("Taille du texte des messages :"));
    m_zoom = new QSlider(Qt::Horizontal);
    m_zoom->setRange(7, 20); // ×10 %
    m_zoom->setPageStep(1);
    m_zoom->setTickPosition(QSlider::TicksBelow);
    m_zoomLabel = new QLabel;
    m_zoomLabel->setMinimumWidth(48);
    connect(m_zoom, &QSlider::valueChanged, this, [this](int v) { m_zoomLabel->setText(QString("%1 %").arg(v * 10)); });
    rv->addWidget(m_zoom, 1);
    rv->addWidget(m_zoomLabel);
    v->addWidget(readBox);
    v->addStretch(1);
    return page;
}

QWidget *SettingsDialog::accountPage(const QString &email)
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *header = new QHBoxLayout;
    auto *avatar = new QLabel;
    avatar->setPixmap(QIcon::fromTheme("user-identity").pixmap(48, 48));
    header->addWidget(avatar);
    auto *who = new QLabel(email.isEmpty()
                               ? QString("<b>Non connecté</b>")
                               : QString("<b style='font-size:13pt'>%1</b><br>Connecté avec l'API officielle de Gmail")
                                     .arg(email.toHtmlEscaped()));
    header->addWidget(who, 1);
    v->addLayout(header);

    auto *idBox = new QGroupBox("Identité");
    auto *form = new QFormLayout(idBox);
    m_senderName = new QLineEdit;
    m_senderName->setPlaceholderText("ex. Marie Dupont");
    form->addRow("Nom affiché :", m_senderName);
    form->addRow(hint(QString("Vos destinataires verront « Nom affiché &lt;%1&gt; ». Laissez vide pour n'afficher "
                              "que l'adresse.").arg(email.isEmpty() ? QString("adresse") : email.toHtmlEscaped())));
    v->addWidget(idBox);

    auto *sigBox = new QGroupBox("Signature");
    auto *sv = new QVBoxLayout(sigBox);
    m_signature = new QPlainTextEdit;
    m_signature->setPlaceholderText("ex.\nMarie Dupont\n06 12 34 56 78");
    m_signature->setMaximumHeight(110);
    sv->addWidget(m_signature);
    sv->addWidget(hint("Ajoutée automatiquement à vos nouveaux messages, réponses et transferts, "
                       "au-dessus du message cité."));
    v->addWidget(sigBox);

    auto *accessBox = new QGroupBox("Connexion");
    auto *av = new QVBoxLayout(accessBox);
    auto *row = new QHBoxLayout;
    auto *switchBtn = new QPushButton(QIcon::fromTheme("system-switch-user"), "Changer de compte…");
    auto *logoutBtn = new QPushButton(QIcon::fromTheme("system-log-out"), "Se déconnecter");
    auto *clientBtn = new QPushButton(QIcon::fromTheme("configure"), "Identifiants Google Cloud…");
    switchBtn->setEnabled(!email.isEmpty());
    logoutBtn->setEnabled(!email.isEmpty());
    connect(switchBtn, &QPushButton::clicked, this, &SettingsDialog::switchAccountRequested);
    connect(logoutBtn, &QPushButton::clicked, this, &SettingsDialog::logoutRequested);
    connect(clientBtn, &QPushButton::clicked, this, &SettingsDialog::configureClientRequested);
    row->addWidget(switchBtn);
    row->addWidget(logoutBtn);
    row->addWidget(clientBtn);
    row->addStretch(1);
    av->addLayout(row);
    auto *links = new QLabel("<a href='https://myaccount.google.com/connections'>Voir les applications ayant accès à "
                             "votre compte Google</a> · <a href='https://mail.google.com/'>Ouvrir Gmail dans le "
                             "navigateur</a>");
    links->setOpenExternalLinks(true);
    links->setWordWrap(true);
    av->addWidget(links);
    av->addWidget(hint("La déconnexion révoque l'accès de G-Desk auprès de Google et efface le jeton de KWallet."));
    v->addWidget(accessBox);
    v->addStretch(1);
    return page;
}

QWidget *SettingsDialog::generalPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *startBox = new QGroupBox("Démarrage et barre système");
    auto *sv = new QVBoxLayout(startBox);
    m_closeToTray = new QCheckBox("Garder G-Desk dans la barre système quand on ferme la fenêtre");
    m_autostart = new QCheckBox("Lancer G-Desk à l'ouverture de la session");
    m_startMinimized = new QCheckBox("… directement réduit dans la barre système");
    m_startMinimized->setContentsMargins(24, 0, 0, 0);
    auto *minRow = new QHBoxLayout;
    minRow->addSpacing(24);
    minRow->addWidget(m_startMinimized);
    connect(m_autostart, &QCheckBox::toggled, m_startMinimized, &QWidget::setEnabled);
    sv->addWidget(m_closeToTray);
    sv->addWidget(m_autostart);
    sv->addLayout(minRow);
    v->addWidget(startBox);

    auto *mailBox = new QGroupBox("Nouveaux messages");
    auto *form = new QFormLayout(mailBox);
    m_poll = new QComboBox;
    for (int m : {1, 2, 5, 10, 15, 30})
        m_poll->addItem(m == 1 ? QString("Toutes les minutes") : QString("Toutes les %1 minutes").arg(m), m);
    form->addRow("Relever le courrier :", m_poll);
    m_notifications = new QCheckBox("Afficher une notification pour chaque nouveau message");
    auto *test = new QPushButton(QIcon::fromTheme("preferences-desktop-notification"), "Tester");
    connect(test, &QPushButton::clicked, this, &SettingsDialog::testNotificationRequested);
    auto *notifRow = new QHBoxLayout;
    notifRow->addWidget(m_notifications, 1);
    notifRow->addWidget(test);
    form->addRow(notifRow);
    v->addWidget(mailBox);

    auto *readBox = new QGroupBox("Lecture");
    auto *rf = new QFormLayout(readBox);
    m_markRead = new QComboBox;
    m_markRead->addItem("Dès l'ouverture", "immediate");
    m_markRead->addItem("Après 3 secondes d'affichage", "delay");
    m_markRead->addItem("Jamais (touche M pour le faire soi-même)", "never");
    rf->addRow("Marquer un message comme lu :", m_markRead);
    v->addWidget(readBox);
    v->addStretch(1);
    return page;
}

QWidget *SettingsDialog::privacyPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *imgBox = new QGroupBox("Images distantes");
    auto *iv = new QVBoxLayout(imgBox);
    m_remoteImages = new QComboBox;
    m_remoteImages->addItem("Bloquer et proposer de les afficher (recommandé)", "ask");
    m_remoteImages->addItem("Toujours afficher", "always");
    iv->addWidget(m_remoteImages);
    iv->addWidget(hint("Beaucoup d'e-mails contiennent des images invisibles qui indiquent à l'expéditeur quand et où "
                       "vous avez ouvert le message. Les bloquer empêche ce pistage."));
    v->addWidget(imgBox);

    auto *trustBox = new QGroupBox("Expéditeurs dont les images sont toujours affichées");
    auto *tv = new QVBoxLayout(trustBox);
    m_trusted = new QListWidget;
    m_trusted->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_trusted->setMaximumHeight(140);
    tv->addWidget(m_trusted);
    auto *tRow = new QHBoxLayout;
    auto *removeBtn = new QPushButton(QIcon::fromTheme("list-remove"), "Retirer");
    auto *clearBtn = new QPushButton(QIcon::fromTheme("edit-clear-all"), "Tout retirer");
    connect(removeBtn, &QPushButton::clicked, this, [this] { qDeleteAll(m_trusted->selectedItems()); });
    connect(clearBtn, &QPushButton::clicked, m_trusted, &QListWidget::clear);
    tRow->addWidget(removeBtn);
    tRow->addWidget(clearBtn);
    tRow->addStretch(1);
    tv->addLayout(tRow);
    v->addWidget(trustBox);

    auto *addrBox = new QGroupBox("Adresses mémorisées pour l'autocomplétion");
    auto *ah = new QHBoxLayout(addrBox);
    m_addressCount = new QLabel;
    auto *forget = new QPushButton(QIcon::fromTheme("edit-clear-history"), "Oublier ces adresses");
    connect(forget, &QPushButton::clicked, this, [this] {
        m_clearAddresses = true;
        m_addressCount->setText("Les adresses seront effacées à l'enregistrement.");
    });
    ah->addWidget(m_addressCount, 1);
    ah->addWidget(forget);
    v->addWidget(addrBox);

    auto *policy = new QLabel("<a href='https://krakenagite.github.io/gdesk/confidentialite.html'>Politique de "
                              "confidentialité de G-Desk</a>");
    policy->setOpenExternalLinks(true);
    v->addWidget(policy);
    v->addStretch(1);
    return page;
}

// =============================================================================
//  Lecture / écriture des réglages
// =============================================================================
static void checkValue(QButtonGroup *group, const QString &value)
{
    for (QAbstractButton *b : group->buttons())
        if (static_cast<PreviewCard *>(b)->value() == value)
            b->setChecked(true);
}

static QString checkedValue(QButtonGroup *group, const QString &fallback)
{
    auto *b = static_cast<PreviewCard *>(group->checkedButton());
    return b ? b->value() : fallback;
}

static void selectData(QComboBox *combo, const QVariant &value)
{
    const int i = combo->findData(value);
    combo->setCurrentIndex(i >= 0 ? i : 0);
}

QString SettingsDialog::autostartPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/gdesk.desktop";
}

void SettingsDialog::load()
{
    checkValue(m_theme, m_settings.value("theme", "system").toString());
    m_darkMessages->setChecked(m_settings.value("dark_messages", false).toBool());
    checkValue(m_density, m_settings.value("density", "comfortable").toString());
    m_showSnippet->setChecked(m_settings.value("show_snippet", true).toBool());
    checkValue(m_layout, m_settings.value("layout", "below").toString());
    m_zoom->setValue(qRound(m_settings.value("message_zoom", 100).toInt() / 10.0));
    m_zoomLabel->setText(QString("%1 %").arg(m_zoom->value() * 10));

    m_senderName->setText(m_settings.value("sender_name").toString());
    m_signature->setPlainText(m_settings.value("signature").toString());

    m_closeToTray->setChecked(m_settings.value("close_to_tray", true).toBool());
    m_autostart->setChecked(QFile::exists(autostartPath()));
    m_startMinimized->setChecked(m_settings.value("start_minimized", true).toBool());
    m_startMinimized->setEnabled(m_autostart->isChecked());
    selectData(m_poll, m_settings.value("poll_minutes", 1).toInt());
    m_notifications->setChecked(m_settings.value("notifications", true).toBool());
    selectData(m_markRead, m_settings.value("mark_read", "immediate").toString());

    selectData(m_remoteImages, m_settings.value("remote_images", "ask").toString());
    m_trusted->addItems(m_settings.value("trusted_senders").toStringList());
    const int known = m_settings.value("known_addresses").toStringList().size();
    m_addressCount->setText(known ? QString("%1 adresse(s) mémorisée(s) à partir des messages lus.").arg(known)
                                  : QString("Aucune adresse mémorisée."));
}

void SettingsDialog::save()
{
    m_settings.setValue("theme", checkedValue(m_theme, "system"));
    m_settings.setValue("dark_messages", m_darkMessages->isChecked());
    m_settings.setValue("density", checkedValue(m_density, "comfortable"));
    m_settings.setValue("show_snippet", m_showSnippet->isChecked());
    m_settings.setValue("layout", checkedValue(m_layout, "below"));
    m_settings.setValue("message_zoom", m_zoom->value() * 10);

    m_settings.setValue("sender_name", m_senderName->text().trimmed());
    m_settings.setValue("signature", m_signature->toPlainText().trimmed());

    m_settings.setValue("close_to_tray", m_closeToTray->isChecked());
    m_settings.setValue("start_minimized", m_startMinimized->isChecked());
    writeAutostart(m_autostart->isChecked(), m_startMinimized->isChecked());
    m_settings.setValue("poll_minutes", m_poll->currentData().toInt());
    m_settings.setValue("notifications", m_notifications->isChecked());
    m_settings.setValue("mark_read", m_markRead->currentData().toString());

    m_settings.setValue("remote_images", m_remoteImages->currentData().toString());
    QStringList trusted;
    for (int i = 0; i < m_trusted->count(); ++i)
        trusted << m_trusted->item(i)->text();
    m_settings.setValue("trusted_senders", trusted);
    if (m_clearAddresses) {
        m_settings.remove("known_addresses");
        m_clearAddresses = false;
        m_addressCount->setText("Aucune adresse mémorisée.");
    }
    m_settings.sync();
    emit applied();
}

void SettingsDialog::writeAutostart(bool enabled, bool minimized)
{
    const QString path = autostartPath();
    if (!enabled) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QString("[Desktop Entry]\nType=Application\nName=G-Desk\nExec=gdesk%1\nIcon=gdesk\n"
                        "X-GNOME-Autostart-enabled=true\n")
                    .arg(minimized ? " --minimized" : "")
                    .toUtf8());
}
