#pragma once
#include "mime.h"

#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <functional>

class DriveApi;
struct DriveFile;
class GmailApi;
class QCompleter;
class QListWidget;
class QPlainTextEdit;
class QToolButton;

// Champ d'adresses avec autocomplétion sur la dernière adresse saisie
class AddressEdit : public QLineEdit
{
    Q_OBJECT
public:
    AddressEdit(const QStringList &known, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void insertCompletion(const QString &completion);
    QCompleter *m_completer;
};

class Composer : public QMainWindow
{
    Q_OBJECT
public:
    enum Mode { New, Reply, ReplyAll, Forward };

    // myAddress : « Nom <adresse> » ou adresse seule ; signature : texte brut, éventuellement vide
    Composer(GmailApi *api, const QString &myAddress, const QString &signature, const QStringList &knownAddresses,
             QWidget *parent = nullptr);

    // Préremplit la fenêtre (signature, et pour une réponse ou un transfert, le message d'origine)
    void prepare(Mode mode, const MailMessage &original = {});
    void prepareMailto(const QUrl &mailto);
    // Google Drive : sans lui, les commandes « Drive » du bouton Joindre sont masquées
    void setDriveApi(DriveApi *drive);
    void attachFromDrive(const QList<DriveFile> &files); // télécharge et joint (ou propose un lien si > 25 Mo)

signals:
    void sent();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    OutgoingMail collect() const;
    void send();
    void saveDraft(std::function<void()> then = {});
    void addFiles();
    void pickFromDrive();
    void insertDriveLinks();
    void insertDriveLink(const DriveFile &file);
    void updateDriveStatus();
    void addAttachment(const Attachment &a);
    void setBusy(bool busy, const QString &status = {});
    QString signatureBlock() const;
    QToolButton *buildEmojiButton();
    void insertEmoji(const QString &emoji);

    GmailApi *m_api;
    DriveApi *m_drive = nullptr;
    QList<QAction *> m_driveActions;
    int m_driveDownloads = 0; // pièces jointes en cours de téléchargement depuis Drive
    QString m_myAddress, m_signature, m_threadId, m_draftId;
    QString m_inReplyTo, m_references;
    AddressEdit *m_to, *m_cc, *m_bcc;
    QLineEdit *m_subject;
    QPlainTextEdit *m_body;
    QListWidget *m_attachmentList;
    QList<Attachment> m_attachments;
    QList<QAction *> m_actions;
    bool m_done = false;
    QPointer<QWidget> m_emojiTarget; // champ qui recevra l'emoji (objet, destinataire ou corps)
};
