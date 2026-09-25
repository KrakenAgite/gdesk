#include "setupdialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

SetupDialog::SetupDialog(const QString &clientId, const QString &clientSecret, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Identifiants Google Cloud");
    resize(640, 0);
    auto *v = new QVBoxLayout(this);

    auto *help = new QLabel(
        "<p>G-Desk utilise les API officielles de Gmail et de Google Drive. Google exige pour cela un « client OAuth » "
        "créé dans <b>votre propre</b> projet Google Cloud (gratuit, à faire une seule fois) :</p>"
        "<ol>"
        "<li>Ouvrez <a href='https://console.cloud.google.com/projectcreate'>console.cloud.google.com</a> "
        "et créez un projet (par ex. « G-Desk »).</li>"
        "<li>Activez <a href='https://console.cloud.google.com/apis/library/gmail.googleapis.com'>l'API Gmail</a> "
        "et <a href='https://console.cloud.google.com/apis/library/drive.googleapis.com'>l'API Google Drive</a> "
        "pour ce projet.</li>"
        "<li>Dans <a href='https://console.cloud.google.com/auth/overview'>Google Auth Platform</a>, cliquez sur "
        "« Commencer » : nom de l'appli, votre e-mail, audience <b>Externe</b>. Dans « Branding », vous pouvez "
        "indiquer la page d'accueil <i>https://krakenagite.github.io/gdesk/</i> et la politique de confidentialité "
        "<i>https://krakenagite.github.io/gdesk/confidentialite.html</i>.</li>"
        "<li>Dans <a href='https://console.cloud.google.com/auth/audience'>Audience</a>, ajoutez votre adresse "
        "Gmail comme <b>utilisateur test</b>. Astuce : cliquez ensuite sur « Publier l'application », sinon "
        "Google vous déconnecte tous les 7 jours.</li>"
        "<li>Dans <a href='https://console.cloud.google.com/auth/clients'>Clients</a>, créez un client de type "
        "<b>Application de bureau</b>, puis téléchargez le fichier JSON.</li>"
        "</ol>"
        "<p>Importez ce fichier ci-dessous (ou copiez l'ID client et le code secret).</p>");
    help->setWordWrap(true);
    help->setOpenExternalLinks(true);
    help->setTextInteractionFlags(Qt::TextBrowserInteraction);
    v->addWidget(help);

    auto *importBtn = new QPushButton(QIcon::fromTheme("document-import"), "Importer le fichier JSON téléchargé…");
    connect(importBtn, &QPushButton::clicked, this, &SetupDialog::importJson);
    v->addWidget(importBtn);

    auto *form = new QFormLayout;
    m_id = new QLineEdit(clientId);
    m_id->setPlaceholderText("xxxxxxxx.apps.googleusercontent.com");
    m_secret = new QLineEdit(clientSecret);
    m_secret->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_secret->setPlaceholderText("GOCSPX-…");
    form->addRow("ID client :", m_id);
    form->addRow("Code secret du client :", m_secret);
    v->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText("Enregistrer");
    auto updateOk = [this, ok] {
        ok->setEnabled(m_id->text().trimmed().endsWith(".apps.googleusercontent.com")
                       && !m_secret->text().trimmed().isEmpty());
    };
    connect(m_id, &QLineEdit::textChanged, this, updateOk);
    connect(m_secret, &QLineEdit::textChanged, this, updateOk);
    updateOk();
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

QString SetupDialog::clientId() const
{
    return m_id->text().trimmed();
}

QString SetupDialog::clientSecret() const
{
    return m_secret->text().trimmed();
}

void SetupDialog::importJson()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Fichier d'identifiants Google",
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), "JSON (*.json)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Importer", "Impossible de lire le fichier.");
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject c = root.contains("installed") ? root.value("installed").toObject() : root.value("web").toObject();
    if (root.contains("web"))
        QMessageBox::warning(this, "Importer",
                             "Ce client est de type « Application Web ». Créez plutôt un client "
                             "« Application de bureau », sinon la connexion échouera.");
    if (c.value("client_id").toString().isEmpty()) {
        QMessageBox::warning(this, "Importer", "Ce fichier ne contient pas d'identifiants OAuth.");
        return;
    }
    m_id->setText(c.value("client_id").toString());
    m_secret->setText(c.value("client_secret").toString());
}
