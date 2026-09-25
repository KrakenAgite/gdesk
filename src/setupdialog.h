#pragma once
#include <QDialog>

class QLineEdit;

// Saisie des identifiants OAuth (projet Google Cloud de l'utilisateur)
class SetupDialog : public QDialog
{
    Q_OBJECT
public:
    SetupDialog(const QString &clientId, const QString &clientSecret, QWidget *parent = nullptr);
    QString clientId() const;
    QString clientSecret() const;

private:
    void importJson();

    QLineEdit *m_id;
    QLineEdit *m_secret;
};
