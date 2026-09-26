#pragma once
#include <QDateTime>
#include <QDialog>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

class GmailApi;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

// Moyens de désabonnement annoncés par l'expéditeur : en-têtes List-Unsubscribe (RFC 2369)
// et List-Unsubscribe-Post (RFC 8058, désabonnement « en un clic »)
struct UnsubscribeMethods
{
    QUrl oneClick; // requête POST directe, sans navigateur (https uniquement)
    QUrl mailto;   // e-mail de désabonnement à envoyer
    QUrl web;      // page à ouvrir dans le navigateur

    bool isValid() const { return !oneClick.isEmpty() || !mailto.isEmpty() || !web.isEmpty(); }
    QString description() const; // « En un clic », « Par e-mail », « Page web »
    static UnsubscribeMethods parse(const QString &listUnsubscribe, const QString &listUnsubscribePost);
};

// Expéditeur de newsletters trouvé dans la boîte
struct Newsletter
{
    QString address, name, lastSubject;
    int count = 0;
    QDateTime last;
    UnsubscribeMethods methods; // ceux du message le plus récent
};

// Recherche les listes de diffusion dans les messages reçus, les affiche avec des cases à cocher
// et désabonne des listes cochées.
class UnsubscribeDialog : public QDialog
{
    Q_OBJECT
public:
    enum Column { NameColumn, AddressColumn, CountColumn, DateColumn, MethodColumn };

    UnsubscribeDialog(GmailApi *api, const QString &myAddress, QWidget *parent = nullptr);
    ~UnsubscribeDialog() override;

    // Messages analysés : reçus depuis un an, dans les catégories Gmail ou mentionnant un désabonnement
    static QString scanQuery();
    // Regroupe par expéditeur les messages (format « metadata ») qui ont un en-tête List-Unsubscribe
    static QList<Newsletter> group(const QList<QJsonObject> &messages, const QSet<QString> &ignored = {});
    static void setAllowHttpForTesting(bool allow); // faux serveur des tests (http://127.0.0.1)

    QTreeWidget *list() const { return m_list; }

private:
    void scan();
    void fetchIds(const QString &pageToken);
    void fetchNextHeaders();
    void showResults();
    void updateButtons();
    void unsubscribeChecked();
    void unsubscribe(QTreeWidgetItem *item);
    void oneClick(QTreeWidgetItem *item, const Newsletter &n);
    void sendMail(QTreeWidgetItem *item, const Newsletter &n);
    void openWeb(QTreeWidgetItem *item, const Newsletter &n);
    void done(QTreeWidgetItem *item, bool ok, const QString &status, const QString &icon);
    QSet<QString> unsubscribedSenders() const;

    GmailApi *m_api;
    QNetworkAccessManager *m_nam; // propre à la fenêtre : aucun cookie partagé avec le reste
    QString m_myAddress;
    QList<Newsletter> m_found;
    QStringList m_pending;        // identifiants dont les en-têtes restent à lire
    QList<QJsonObject> m_messages;
    int m_total = 0, m_inFlight = 0, m_running = 0;
    bool m_cancelled = false, m_destroying = false;

    QStackedWidget *m_stack;
    QLabel *m_progressLabel, *m_summary, *m_emptyLabel;
    QProgressBar *m_progress;
    QLineEdit *m_filter;
    QTreeWidget *m_list;
    QPushButton *m_checkAll, *m_checkNone, *m_unsubscribe;
};
