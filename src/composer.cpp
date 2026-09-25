#include "composer.h"
#include "gmailapi.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QCompleter>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QProcess>
#include <QStandardPaths>
#include <QWidgetAction>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QStringListModel>
#include <QToolBar>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>

// --- AddressEdit ----------------------------------------------------------------
AddressEdit::AddressEdit(const QStringList &known, QWidget *parent)
    : QLineEdit(parent), m_completer(new QCompleter(known, this))
{
    m_completer->setWidget(this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, &AddressEdit::insertCompletion);
    connect(this, &QLineEdit::textEdited, this, [this](const QString &text) {
        const QString last = text.section(',', -1).trimmed();
        if (last.size() < 2) {
            m_completer->popup()->hide();
            return;
        }
        m_completer->setCompletionPrefix(last);
        m_completer->complete();
    });
}

void AddressEdit::insertCompletion(const QString &completion)
{
    QStringList parts = text().split(',');
    parts.removeLast();
    for (QString &p : parts)
        p = p.trimmed();
    parts << completion;
    setText(parts.join(", ") + ", ");
}

void AddressEdit::keyPressEvent(QKeyEvent *e)
{
    if (m_completer->popup()->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
            e->ignore(); // géré par le complèteur
            return;
        default:
            break;
        }
    }
    QLineEdit::keyPressEvent(e);
}

// --- Composer -----------------------------------------------------------------
Composer::Composer(GmailApi *api, const QString &myAddress, const QString &signature,
                   const QStringList &knownAddresses, QWidget *parent)
    : QMainWindow(parent), m_api(api), m_myAddress(myAddress), m_signature(signature)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("Nouveau message");
    resize(760, 620);

    auto *bar = addToolBar("Actions");
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_actions << bar->addAction(QIcon::fromTheme("mail-send"), "Envoyer", QKeySequence("Ctrl+Return"), this, &Composer::send);
    m_actions << bar->addAction(QIcon::fromTheme("mail-attachment"), "Joindre…", this, &Composer::addFiles);
    m_actions << bar->addAction(QIcon::fromTheme("document-save"), "Enregistrer le brouillon", QKeySequence::Save,
                                this, [this] { saveDraft(); });
    bar->addWidget(buildEmojiButton());
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    QAction *showBcc = bar->addAction("Cci");
    showBcc->setCheckable(true);

    auto *central = new QWidget;
    auto *v = new QVBoxLayout(central);
    auto *form = new QFormLayout;
    m_to = new AddressEdit(knownAddresses);
    m_cc = new AddressEdit(knownAddresses);
    m_bcc = new AddressEdit(knownAddresses);
    m_subject = new QLineEdit;
    m_to->setPlaceholderText("destinataire@exemple.fr, …");
    form->addRow("De :", new QLabel(myAddress));
    form->addRow("À :", m_to);
    form->addRow("Cc :", m_cc);
    form->addRow("Cci :", m_bcc);
    form->addRow("Objet :", m_subject);
    form->setRowVisible(m_bcc, false);
    connect(showBcc, &QAction::toggled, this, [form, this](bool on) { form->setRowVisible(m_bcc, on); });
    connect(m_subject, &QLineEdit::textChanged, this, [this](const QString &s) {
        setWindowTitle(s.isEmpty() ? QString("Nouveau message") : s);
    });
    v->addLayout(form);

    m_body = new QPlainTextEdit;
    m_body->setTabChangesFocus(true);
    v->addWidget(m_body, 1);

    m_attachmentList = new QListWidget;
    m_attachmentList->setFlow(QListView::LeftToRight);
    m_attachmentList->setWrapping(true);
    m_attachmentList->setMaximumHeight(70);
    m_attachmentList->setVisible(false);
    m_attachmentList->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto *remove = new QAction(QIcon::fromTheme("list-remove"), "Retirer la pièce jointe", m_attachmentList);
    remove->setShortcut(QKeySequence::Delete);
    remove->setShortcutContext(Qt::WidgetShortcut);
    connect(remove, &QAction::triggered, this, [this] {
        const int row = m_attachmentList->currentRow();
        if (row < 0)
            return;
        m_attachments.removeAt(row);
        delete m_attachmentList->takeItem(row);
        m_attachmentList->setVisible(!m_attachments.isEmpty());
    });
    m_attachmentList->addAction(remove);
    v->addWidget(m_attachmentList);
    setCentralWidget(central);
    setAcceptDrops(false);
    statusBar();
    m_to->setFocus();
}

QToolButton *Composer::buildEmojiButton()
{
    static const QStringList emojis = {
        "😀", "😂", "😊", "😍", "😘", "😉", "🙂", "🤔", "😅", "😢", "😭", "😡",
        "👍", "👎", "👏", "🙏", "💪", "👋", "🤝", "✌️", "👌", "❤️", "💙", "💚",
        "🎉", "🎂", "🎁", "☀️", "🌧️", "❄️", "🔥", "⭐", "✅", "❌", "⚠️", "📎",
        "📅", "📞", "📷", "☕", "🍕", "🍷", "🏠", "🚗", "✈️", "💼", "💡", "🇫🇷"};
    auto *button = new QToolButton;
    button->setText("😀");
    button->setToolTip("Insérer un emoji");
    QFont f = button->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    button->setFont(f);
    button->setPopupMode(QToolButton::InstantPopup);

    auto *menu = new QMenu(button);
    auto *grid = new QWidget;
    auto *layout = new QGridLayout(grid);
    layout->setSpacing(2);
    layout->setContentsMargins(6, 6, 6, 6);
    QFont big = font();
    big.setPointSizeF(big.pointSizeF() * 1.45);
    for (int i = 0; i < emojis.size(); ++i) {
        auto *b = new QToolButton;
        b->setText(emojis[i]);
        b->setFont(big);
        b->setAutoRaise(true);
        b->setFixedSize(38, 38);
        connect(b, &QToolButton::clicked, this, [this, menu, e = emojis[i]] {
            menu->close();
            insertEmoji(e);
        });
        layout->addWidget(b, i / 12, i % 12);
    }
    auto *gridAction = new QWidgetAction(menu);
    gridAction->setDefaultWidget(grid);
    menu->addAction(gridAction);
    const QString emojier = QStandardPaths::findExecutable("plasma-emojier");
    if (!emojier.isEmpty()) {
        menu->addSeparator();
        menu->addAction(QIcon::fromTheme("preferences-desktop-emoticons"), "Plus d'emojis… (Meta+.)", this, [emojier] {
            QProcess::startDetached(emojier, {}); // l'emoji choisi est copié : Ctrl+V pour le coller
        });
    }
    // Mémorise le champ où insérer avant que le menu ne prenne le focus
    connect(menu, &QMenu::aboutToShow, this, [this] {
        QWidget *w = QApplication::focusWidget();
        m_emojiTarget = (qobject_cast<QLineEdit *>(w) || qobject_cast<QPlainTextEdit *>(w)) ? w : m_body;
    });
    button->setMenu(menu);
    return button;
}

void Composer::insertEmoji(const QString &emoji)
{
    QWidget *target = m_emojiTarget ? m_emojiTarget.data() : m_body;
    if (auto *line = qobject_cast<QLineEdit *>(target))
        line->insert(emoji);
    else if (auto *edit = qobject_cast<QPlainTextEdit *>(target))
        edit->insertPlainText(emoji);
    target->setFocus();
}

QString Composer::signatureBlock() const
{
    // « -- » suivi d'un espace : séparateur standard reconnu par les logiciels de messagerie
    return m_signature.isEmpty() ? QString() : "\n\n-- \n" + m_signature;
}

void Composer::prepare(Mode mode, const MailMessage &o)
{
    if (mode == New) {
        m_body->setPlainText(signatureBlock());
        m_body->moveCursor(QTextCursor::Start);
        m_to->setFocus();
        return;
    }
    m_threadId = mode == Forward ? QString() : o.threadId;
    const QString original = o.text.isEmpty() ? Mime::htmlToText(o.html) : o.text;
    const QString when = Mime::longDate(o.date);

    if (mode == Reply || mode == ReplyAll) {
        m_inReplyTo = o.messageId;
        m_references = (o.references + " " + o.messageId).trimmed();
        const QString me = Mime::emailOnly(m_myAddress).toLower();
        QStringList to{o.replyTo.isEmpty() ? o.from : o.replyTo};
        QStringList cc;
        if (mode == ReplyAll) {
            for (const QString &a : Mime::splitAddresses(o.to))
                if (Mime::emailOnly(a).toLower() != me)
                    to << a;
            for (const QString &a : Mime::splitAddresses(o.cc))
                if (Mime::emailOnly(a).toLower() != me)
                    cc << a;
        }
        // Réponse à son propre message envoyé : on répond aux destinataires d'origine
        if (Mime::emailOnly(o.from).toLower() == me && mode == Reply)
            to = Mime::splitAddresses(o.to);
        to.removeDuplicates();
        m_to->setText(to.join(", "));
        m_cc->setText(cc.join(", "));
        m_subject->setText(o.subject.startsWith("Re:", Qt::CaseInsensitive) ? o.subject : "Re: " + o.subject);

        QString quoted;
        for (const QString &line : original.split('\n'))
            quoted += "> " + line + "\n";
        m_body->setPlainText(signatureBlock() + QString("\n\nLe %1, %2 a écrit :\n%3").arg(when, o.from, quoted));
        m_body->moveCursor(QTextCursor::Start);
        m_body->setFocus();
    } else if (mode == Forward) {
        const bool alreadyFwd = o.subject.startsWith("Fwd:", Qt::CaseInsensitive)
                                || o.subject.startsWith("TR:", Qt::CaseInsensitive);
        m_subject->setText(alreadyFwd ? o.subject : "Fwd: " + o.subject);
        m_body->setPlainText(signatureBlock() + QString("\n\n---------- Message transféré ---------\nDe : %1\nDate : %2\n"
                                     "Objet : %3\nÀ : %4\n%5\n%6")
                                 .arg(o.from, when, o.subject, o.to,
                                      o.cc.isEmpty() ? QString() : "Cc : " + o.cc + "\n", original));
        for (const Attachment &a : o.attachments)
            if (!a.isInline && !a.data.isEmpty())
                addAttachment(a);
        m_body->moveCursor(QTextCursor::Start);
        m_to->setFocus();
    }
}

void Composer::prepareMailto(const QUrl &mailto)
{
    prepare(New);
    m_to->setText(QUrl::fromPercentEncoding(mailto.path(QUrl::FullyEncoded).toUtf8()));
    const QUrlQuery q(mailto);
    for (const auto &[key, value] : q.queryItems(QUrl::FullyDecoded)) {
        const QString k = key.toLower();
        if (k == "subject")
            m_subject->setText(value);
        else if (k == "body")
            m_body->setPlainText(value + signatureBlock());
        else if (k == "cc")
            m_cc->setText(value);
        else if (k == "bcc")
            m_bcc->setText(value);
    }
}

void Composer::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, "Joindre des fichiers");
    QMimeDatabase db;
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Pièce jointe", "Impossible de lire " + path);
            continue;
        }
        Attachment a;
        a.filename = QFileInfo(path).fileName();
        a.mimeType = db.mimeTypeForFile(path).name();
        a.data = f.readAll();
        a.size = a.data.size();
        addAttachment(a);
    }
}

void Composer::addAttachment(const Attachment &a)
{
    m_attachments << a;
    auto *item = new QListWidgetItem(QIcon::fromTheme(QMimeDatabase().mimeTypeForName(a.mimeType).iconName(),
                                                      QIcon::fromTheme("mail-attachment")),
                                     QString("%1 (%2)").arg(a.filename, Mime::humanSize(a.data.size())));
    m_attachmentList->addItem(item);
    m_attachmentList->setVisible(true);

    qint64 total = 0;
    for (const Attachment &x : m_attachments)
        total += x.data.size();
    if (total > 25 * 1024 * 1024)
        statusBar()->showMessage("Attention : Gmail refuse les pièces jointes de plus de 25 Mo au total.");
}

OutgoingMail Composer::collect() const
{
    OutgoingMail m;
    m.from = m_myAddress;
    m.to = m_to->text();
    m.cc = m_cc->text();
    m.bcc = m_bcc->text();
    m.subject = m_subject->text();
    m.body = m_body->toPlainText();
    m.inReplyTo = m_inReplyTo;
    m.references = m_references;
    m.attachments = m_attachments;
    return m;
}

void Composer::setBusy(bool busy, const QString &status)
{
    for (QAction *a : std::as_const(m_actions))
        a->setEnabled(!busy);
    centralWidget()->setEnabled(!busy);
    statusBar()->showMessage(status);
}

void Composer::send()
{
    if (Mime::splitAddresses(m_to->text() + "," + m_cc->text() + "," + m_bcc->text()).isEmpty()) {
        QMessageBox::warning(this, "Envoyer", "Indiquez au moins un destinataire.");
        m_to->setFocus();
        return;
    }
    if (m_subject->text().trimmed().isEmpty()
        && QMessageBox::question(this, "Envoyer", "Envoyer ce message sans objet ?") != QMessageBox::Yes)
        return;

    setBusy(true, "Envoi en cours…");
    auto done = [this](const QJsonObject &, const QString &err) {
        if (!err.isEmpty()) {
            setBusy(false);
            QMessageBox::critical(this, "Envoi impossible", err);
            return;
        }
        m_done = true;
        emit sent();
        close();
    };
    const QByteArray raw = Mime::build(collect());
    if (m_draftId.isEmpty())
        m_api->sendMessage(raw, m_threadId, done);
    else
        m_api->sendDraft(m_draftId, raw, m_threadId, done);
}

void Composer::saveDraft(std::function<void()> then)
{
    setBusy(true, "Enregistrement…");
    m_api->saveDraft(m_draftId, Mime::build(collect()), m_threadId,
                     [this, then](const QJsonObject &result, const QString &err) {
                         setBusy(false, err.isEmpty() ? QString("Brouillon enregistré dans Gmail.") : QString());
                         if (!err.isEmpty()) {
                             QMessageBox::critical(this, "Brouillon", err);
                             return;
                         }
                         m_draftId = result.value("id").toString();
                         m_body->document()->setModified(false);
                         if (then)
                             then();
                     });
}

void Composer::closeEvent(QCloseEvent *event)
{
    const bool hasContent = !m_body->toPlainText().trimmed().isEmpty() || !m_attachments.isEmpty();
    if (!m_done && hasContent && m_body->document()->isModified()) {
        const auto answer = QMessageBox::question(this, "Fermer",
            "Ce message n'a pas été envoyé. Que voulez-vous faire ?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save) {
            event->ignore();
            saveDraft([this] {
                m_done = true;
                close();
            });
            return;
        }
    }
    event->accept();
}
