#include "messageview.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMimeDatabase>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>

// Sert le HTML du message et les images intégrées (cid:) depuis la mémoire
class MailSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;
    QByteArray html;
    QHash<QString, QPair<QByteArray, QByteArray>> parts; // content-id → (type, données)

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrl url = job->requestUrl();
        QByteArray type, data;
        if (url.host() == "msg") {
            type = "text/html;charset=utf-8";
            data = html;
        } else if (url.host() == "cid") {
            const auto it = parts.constFind(QUrl::fromPercentEncoding(url.path().mid(1).toUtf8()));
            if (it == parts.constEnd()) {
                job->fail(QWebEngineUrlRequestJob::UrlNotFound);
                return;
            }
            type = it->first;
            data = it->second;
        } else {
            job->fail(QWebEngineUrlRequestJob::UrlInvalid);
            return;
        }
        auto *buffer = new QBuffer(job);
        buffer->setData(data);
        buffer->open(QIODevice::ReadOnly);
        job->reply(type, buffer);
    }
};

// Bloque tout accès réseau depuis le message, sauf autorisation explicite
class RemoteBlocker : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    bool allowRemote = false;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QString scheme = info.requestUrl().scheme();
        if (scheme == MessageView::Scheme || scheme == "data")
            return;
        if (!allowRemote || (scheme != "http" && scheme != "https"))
            info.block(true);
    }
};

// Les liens cliqués s'ouvrent dans le navigateur, jamais dans la visionneuse
class MailPage : public QWebEnginePage
{
public:
    MailPage(QWebEngineProfile *profile, MessageView *view) : QWebEnginePage(profile, view), m_view(view) {}

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override
    {
        if (url.scheme() == MessageView::Scheme)
            return isMainFrame;
        openLink(url);
        return false;
    }

    QWebEnginePage *createWindow(WebWindowType) override
    {
        // Lien target=_blank : page temporaire qui transmet l'adresse puis disparaît
        auto *catcher = new MailPage(profile(), m_view);
        catcher->m_catcher = true;
        return catcher;
    }

private:
    void openLink(const QUrl &url)
    {
        if (url.scheme() == "mailto")
            emit m_view->mailtoClicked(url);
        else if (url.scheme() == "http" || url.scheme() == "https")
            QDesktopServices::openUrl(url);
        if (m_catcher)
            deleteLater();
    }

    MessageView *m_view;
    bool m_catcher = false;
};

void MessageView::registerScheme()
{
    QWebEngineUrlScheme scheme(Scheme);
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::ContentSecurityPolicyIgnored);
    QWebEngineUrlScheme::registerScheme(scheme);
}

MessageView::MessageView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget;
    layout->addWidget(m_stack);

    auto *empty = new QLabel("Sélectionnez un message pour le lire");
    empty->setAlignment(Qt::AlignCenter);
    empty->setEnabled(false);
    m_stack->addWidget(empty);

    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // --- en-têtes ---
    auto *headerBox = new QWidget;
    headerBox->setObjectName("mailHeader");
    headerBox->setStyleSheet("#mailHeader { border-bottom: 1px solid palette(mid); }");
    auto *h = new QVBoxLayout(headerBox);
    h->setContentsMargins(14, 10, 14, 10);
    h->setSpacing(4);
    m_subject = new QLabel;
    QFont f = m_subject->font();
    f.setPointSizeF(f.pointSizeF() * 1.35);
    f.setBold(true);
    m_subject->setFont(f);
    m_subject->setWordWrap(true);
    m_subject->setTextInteractionFlags(Qt::TextSelectableByMouse);
    h->addWidget(m_subject);
    auto *fromRow = new QHBoxLayout;
    m_from = new QLabel;
    m_from->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_date = new QLabel;
    m_date->setEnabled(false);
    fromRow->addWidget(m_from, 1);
    fromRow->addWidget(m_date);
    h->addLayout(fromRow);
    m_details = new QLabel;
    m_details->setWordWrap(true);
    m_details->setEnabled(false);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    h->addWidget(m_details);

    m_attachmentBar = new QWidget;
    m_attachmentLayout = new QHBoxLayout(m_attachmentBar);
    m_attachmentLayout->setContentsMargins(0, 4, 0, 0);
    auto *attachScroll = new QScrollArea;
    attachScroll->setWidget(m_attachmentBar);
    attachScroll->setWidgetResizable(true);
    attachScroll->setFrameShape(QFrame::NoFrame);
    attachScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    attachScroll->setFixedHeight(46);
    attachScroll->setObjectName("attachScroll");
    h->addWidget(attachScroll);
    v->addWidget(headerBox);

    // --- bandeau « images bloquées » ---
    m_remoteBar = new QWidget;
    m_remoteBar->setObjectName("remoteBar");
    m_remoteBar->setStyleSheet("#remoteBar { background: palette(alternate-base); border-bottom: 1px solid palette(mid); }");
    auto *rb = new QHBoxLayout(m_remoteBar);
    rb->setContentsMargins(14, 4, 14, 4);
    auto *rbIcon = new QLabel;
    rbIcon->setPixmap(QIcon::fromTheme("security-medium").pixmap(16, 16));
    rb->addWidget(rbIcon);
    rb->addWidget(new QLabel("Les images distantes sont bloquées pour protéger votre vie privée."), 1);
    auto *showOnce = new QPushButton("Afficher les images");
    auto *showAlways = new QPushButton("Toujours pour cet expéditeur");
    rb->addWidget(showOnce);
    rb->addWidget(showAlways);
    connect(showOnce, &QPushButton::clicked, this, [this] { emit remoteContentAllowed(false); });
    connect(showAlways, &QPushButton::clicked, this, [this] { emit remoteContentAllowed(true); });
    v->addWidget(m_remoteBar);

    // --- corps : le moteur web (Chromium, plusieurs processus) n'est créé qu'à l'affichage
    // d'un message, et libéré quand la fenêtre reste cachée (voir ensureEngine / releaseEngine)
    m_handler = new MailSchemeHandler(this);
    m_blocker = new RemoteBlocker(this);
    m_bodyLayout = v;
    m_stack->addWidget(content);

    m_releaseTimer = new QTimer(this);
    m_releaseTimer->setSingleShot(true);
    m_releaseTimer->setInterval(60 * 1000);
    connect(m_releaseTimer, &QTimer::timeout, this, &MessageView::releaseEngine);
}

MessageView::~MessageView()
{
    releaseEngine();
}

QWebEngineView *MessageView::ensureEngine()
{
    if (m_web)
        return m_web;
    m_profile = new QWebEngineProfile(this); // hors ligne, sans cookies persistants
    m_profile->installUrlSchemeHandler(Scheme, m_handler);
    m_profile->setUrlRequestInterceptor(m_blocker);
    QWebEngineSettings *s = m_profile->settings();
    s->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
    s->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    s->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    s->setAttribute(QWebEngineSettings::AutoLoadIconsForPage, false);
    s->setAttribute(QWebEngineSettings::ForceDarkMode, m_darkContent);

    m_web = new QWebEngineView;
    m_web->setPage(new MailPage(m_profile, this));
    m_web->page()->setParent(m_web);
    m_web->setContextMenuPolicy(Qt::NoContextMenu);
    m_web->setZoomFactor(m_zoom);
    m_bodyLayout->addWidget(m_web, 1);
    return m_web;
}

void MessageView::releaseEngine()
{
    if (!m_web)
        return;
    delete m_web; // la page doit disparaître avant son profil
    delete m_profile;
    m_web = nullptr;
    m_profile = nullptr;
}

void MessageView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_releaseTimer->stop();
    if (!m_web && !m_message.id.isEmpty())
        render(); // moteur libéré pendant que la fenêtre était cachée
}

void MessageView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_releaseTimer->start();
}

void MessageView::setZoom(double factor)
{
    m_zoom = factor;
    if (m_web)
        m_web->setZoomFactor(factor);
}

void MessageView::setDarkContent(bool dark)
{
    if (m_darkContent == dark)
        return;
    m_darkContent = dark;
    if (m_profile)
        m_profile->settings()->setAttribute(QWebEngineSettings::ForceDarkMode, dark);
    if (!m_message.id.isEmpty() && m_web)
        render();
}

void MessageView::clear()
{
    m_message = {};
    m_stack->setCurrentIndex(0);
}

void MessageView::showMessage(const MailMessage &message, bool allowRemote)
{
    m_message = message;
    m_allowRemote = allowRemote;

    m_subject->setText(message.subject.isEmpty() ? "(sans objet)" : message.subject);
    m_from->setText(QString("<b>%1</b> &lt;%2&gt;")
                        .arg(Mime::displayName(message.from).toHtmlEscaped(),
                             Mime::emailOnly(message.from).toHtmlEscaped()));
    m_date->setText(Mime::longDate(message.date));
    QString details = "À : " + message.to;
    if (!message.cc.isEmpty())
        details += "\nCc : " + message.cc;
    m_details->setText(details);

    // Pièces jointes (hors images intégrées au texte)
    while (QLayoutItem *item = m_attachmentLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    int shown = 0;
    QMimeDatabase mimeDb;
    for (int i = 0; i < message.attachments.size(); ++i) {
        const Attachment &a = message.attachments.at(i);
        if (a.isInline && message.html.contains("cid:" + a.contentId))
            continue;
        auto *btn = new QToolButton;
        btn->setText(QString("%1  (%2)").arg(a.filename, Mime::humanSize(a.size)));
        btn->setIcon(QIcon::fromTheme(mimeDb.mimeTypeForName(a.mimeType).iconName(),
                                      QIcon::fromTheme("mail-attachment")));
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(btn);
        menu->addAction(QIcon::fromTheme("document-open"), "Ouvrir", this, [this, i] { emit openAttachmentRequested(i); });
        menu->addAction(QIcon::fromTheme("document-save-as"), "Enregistrer sous…", this,
                        [this, i] { emit saveAttachmentRequested(i); });
        menu->addAction(QIcon::fromTheme("folder-gdrive", QIcon::fromTheme("folder-cloud")),
                        "Enregistrer dans Google Drive…", this, [this, i] { emit saveAttachmentToDriveRequested(i); });
        btn->setMenu(menu);
        m_attachmentLayout->addWidget(btn);
        ++shown;
    }
    m_attachmentLayout->addStretch(1);
    m_attachmentBar->parentWidget()->parentWidget()->setVisible(shown > 0);

    render();
    m_stack->setCurrentIndex(1);
}

void MessageView::render()
{
    static const QRegularExpression remoteRe(
        R"((src|background|srcset)\s*=\s*["']?\s*(https?:)?//|url\(\s*["']?\s*(https?:)?//)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cidRe(R"(cid:([^"'\s)>]+))", QRegularExpression::CaseInsensitiveOption);

    QString body = m_message.html.isEmpty() ? Mime::textToHtml(m_message.text) : m_message.html;
    body.replace(cidRe, QString(Scheme) + "://cid/\\1");
    const bool hasRemote = remoteRe.match(body).hasMatch();

    m_handler->parts.clear();
    for (const Attachment &a : m_message.attachments)
        if (!a.contentId.isEmpty())
            m_handler->parts.insert(a.contentId, {a.mimeType.toUtf8(), a.data});
    m_handler->html = "<!DOCTYPE html><meta charset=\"utf-8\">"
                      "<style>body{margin:14px;overflow-wrap:anywhere;background:#fff;color:#202124}"
                      "img{max-width:100%;height:auto}</style>"
                      + body.toUtf8();
    m_blocker->allowRemote = m_allowRemote;
    m_remoteBar->setVisible(hasRemote && !m_allowRemote);
    ensureEngine()->setUrl(QUrl(QString("%1://msg/%2?n=%3").arg(Scheme, m_message.id).arg(++m_loadCounter)));
}
