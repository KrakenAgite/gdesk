#pragma once
#include "mime.h"

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QWebEngineProfile;
class QWebEngineView;
class MailSchemeHandler;
class RemoteBlocker;

// Affichage d'un message : en-têtes, pièces jointes et corps HTML.
// Le HTML est rendu sans JavaScript et les contenus distants (images de pistage)
// sont bloqués tant que l'utilisateur ne les autorise pas.
class MessageView : public QWidget
{
    Q_OBJECT
public:
    static constexpr const char *Scheme = "gdesk-mail";
    static void registerScheme(); // à appeler avant la création de QApplication

    explicit MessageView(QWidget *parent = nullptr);
    ~MessageView() override;

    void showMessage(const MailMessage &message, bool allowRemote);
    void clear();
    const MailMessage &message() const { return m_message; }
    void setZoom(double factor);
    void setDarkContent(bool dark); // convertit les e-mails en couleurs sombres
    QWebEngineView *ensureEngine();  // crée le moteur web à la demande
    void releaseEngine();            // libère le moteur web (processus Chromium)
    bool hasEngine() const { return m_web != nullptr; }

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

signals:
    void saveAttachmentRequested(int index);
    void openAttachmentRequested(int index);
    void remoteContentAllowed(bool alwaysForSender);
    void mailtoClicked(const QUrl &url);

private:
    void render();

    MailMessage m_message;
    bool m_allowRemote = false;
    bool m_darkContent = false;
    double m_zoom = 1.0;
    int m_loadCounter = 0;

    QStackedWidget *m_stack;
    QLabel *m_subject, *m_from, *m_details, *m_date;
    QWidget *m_attachmentBar;
    QHBoxLayout *m_attachmentLayout;
    QWidget *m_remoteBar;
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineView *m_web = nullptr;
    class QVBoxLayout *m_bodyLayout;
    class QTimer *m_releaseTimer;
    MailSchemeHandler *m_handler;
    RemoteBlocker *m_blocker;
};
