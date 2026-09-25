#pragma once
#include <QStyledItemDelegate>

// Données d'une ligne de la liste des messages (colonne 0 du QTreeWidget)
namespace MailRoles {
enum {
    Id = Qt::UserRole,  // identifiant Gmail
    Labels,             // QStringList des libellés (UNREAD, STARRED…)
    Who,                // expéditeur (ou « À : … » dans Envoyés)
    Date,               // date courte (« 14:32 », « 3 sept. »…)
    Subject,
    Snippet,
};
}

// Affiche chaque message comme une carte :
//   Nom de l'expéditeur (gras)                     14:32
//   Objet du message                                  ★
//   Début du message, si la place le permet…
class MailListDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit MailListDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QString density = "comfortable"; // compact, comfortable, spacious
    bool showSnippet = true;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // Zone cliquable de l'étoile « suivi » dans une carte
    QRect starRect(const QStyleOptionViewItem &option) const;

signals:
    void starClicked(const QModelIndex &index);

protected:
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

private:
    int padding() const;
    int snippetLines() const;
    QRect cardRect(const QRect &row) const;
};
