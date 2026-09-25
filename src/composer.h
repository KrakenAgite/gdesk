#pragma once
#include "mime.h"

#include <QLineEdit>
#include <QMainWindow>
#include <functional>

class GmailApi;
class QCompleter;
class QListWidget;
class QPlainTextEdit;

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

signals:
    void sent();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    OutgoingMail collect() const;
    void send();
    void saveDraft(std::function<void()> then = {});
    void addFiles();
    void addAttachment(const Attachment &a);
    void setBusy(bool busy, const QString &status = {});
    QString signatureBlock() const;

    GmailApi *m_api;
    QString m_myAddress, m_signature, m_threadId, m_draftId;
    QString m_inReplyTo, m_references;
    AddressEdit *m_to, *m_cc, *m_bcc;
    QLineEdit *m_subject;
    QPlainTextEdit *m_body;
    QListWidget *m_attachmentList;
    QList<Attachment> m_attachments;
    QList<QAction *> m_actions;
    bool m_done = false;
};
