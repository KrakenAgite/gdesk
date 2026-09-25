#pragma once
#include <QAbstractItemView>
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
    Checked,            // bool : case cochée (sélection pour une action groupée)
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
    bool selectionMode = false; // au moins un message coché : cases visibles sur toutes les cartes

    // Intercepte les clics sur la case et l'étoile avant la liste (pour ne pas ouvrir le message)
    void attachTo(QAbstractItemView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // Zones cliquables d'une carte (rect = rectangle de la ligne)
    QRect starRect(const QStyleOptionViewItem &option) const;
    QRect checkRect(const QRect &row) const;

signals:
    void starClicked(const QModelIndex &index);
    void checkClicked(const QModelIndex &index, Qt::KeyboardModifiers modifiers);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QAbstractItemView *m_view = nullptr;
    int padding() const;
    int snippetLines() const;
    QRect cardRect(const QRect &row) const;
};
