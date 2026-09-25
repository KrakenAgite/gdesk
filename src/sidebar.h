#pragma once
#include <QAbstractButton>
#include <QStyledItemDelegate>

// Données des éléments de la barre latérale (QTreeWidget des dossiers, colonne 0)
namespace FolderRoles {
enum {
    Id = Qt::UserRole,   // identifiant du libellé Gmail ("" = tous les messages)
    Name,                // nom affiché
    Count,               // nombre de non-lus (ou de brouillons)
    Color,               // couleur de l'icône (QColor), invalide = couleur du texte
    SectionKey,          // clé d'une section (en-tête repliable) ; vide pour un dossier
};
}

// Dessine les en-têtes de section, les dossiers (icône teintée, nom, pastille de non-lus)
// et la sélection arrondie.
class FolderDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// Grand bouton arrondi « Nouveau message »
class ComposeButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit ComposeButton(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
};

// Compte connecté en bas de la barre : pastille avec l'initiale + adresse
class AccountChip : public QAbstractButton
{
    Q_OBJECT
public:
    explicit AccountChip(QWidget *parent = nullptr);
    void setEmail(const QString &email);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    QString m_email;
};

// Icône monochrome recolorée (rend les thèmes d'icônes colorés homogènes dans la barre)
QPixmap tintedIcon(const QIcon &icon, int size, const QColor &color, qreal dpr);
