#include "sidebar.h"

#include <QPainter>
#include <QPainterPath>
#include <QTreeWidget>

namespace {
QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

int depthOf(const QModelIndex &index)
{
    int depth = -1; // les dossiers sous une section sont au niveau 0
    for (QModelIndex p = index.parent(); p.isValid(); p = p.parent())
        ++depth;
    return qMax(depth, 0);
}

constexpr int SideMargin = 8;
constexpr int IconSize = 18;
} // namespace

QPixmap tintedIcon(const QIcon &icon, int size, const QColor &color, qreal dpr)
{
    QPixmap pm = icon.pixmap(QSize(size, size), dpr);
    if (pm.isNull())
        return pm;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), color);
    return pm;
}

// =============================================================================
//  Dossiers et sections
// =============================================================================
QSize FolderDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QFontMetrics fm(option.font);
    if (!index.data(FolderRoles::SectionKey).toString().isEmpty())
        return {100, fm.height() + (index.row() == 0 && !index.parent().isValid() ? 10 : 22)};
    return {100, qMax(fm.height() + 14, 34)};
}

void FolderDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QPalette &pal = option.palette;
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor muted = mix(text, window, 0.45);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    // --- En-tête de section : petites capitales + chevron -----------------------
    const QString section = index.data(FolderRoles::SectionKey).toString();
    if (!section.isEmpty()) {
        QFont f = option.font;
        f.setPointSizeF(f.pointSizeF() * 0.8);
        f.setBold(true);
        f.setLetterSpacing(QFont::PercentageSpacing, 108);
        f.setCapitalization(QFont::AllUppercase);
        p->setFont(f);
        p->setPen(muted);
        const QRect r = option.rect.adjusted(SideMargin + 10, 0, -SideMargin - 6, -3);
        p->drawText(r, Qt::AlignLeft | Qt::AlignBottom, index.data(FolderRoles::Name).toString());

        const bool expanded = option.state & QStyle::State_Open;
        const QPointF c(r.right() - 4, r.bottom() - QFontMetrics(f).height() / 2.0);
        QPainterPath chevron;
        if (expanded) {
            chevron.moveTo(c + QPointF(-3.5, -1.5));
            chevron.lineTo(c + QPointF(0, 2));
            chevron.lineTo(c + QPointF(3.5, -1.5));
        } else {
            chevron.moveTo(c + QPointF(-1.5, -3.5));
            chevron.lineTo(c + QPointF(2, 0));
            chevron.lineTo(c + QPointF(-1.5, 3.5));
        }
        p->setPen(QPen(muted, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        if (option.state & QStyle::State_MouseOver || !expanded)
            p->drawPath(chevron);
        p->restore();
        return;
    }

    // --- Dossier -------------------------------------------------------------------
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const int indent = depthOf(index) * 16;
    const QRectF pill = QRectF(option.rect).adjusted(SideMargin + indent, 1.5, -SideMargin, -1.5);
    if (selected || hovered) {
        p->setPen(Qt::NoPen);
        p->setBrush(selected ? mix(window, accent, window.lightness() < 128 ? 0.32 : 0.20) : mix(window, text, 0.07));
        p->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    }

    // Icône teintée (couleur de la catégorie ou du libellé)
    QColor iconColor = index.data(FolderRoles::Color).value<QColor>();
    if (!iconColor.isValid())
        iconColor = selected ? (window.lightness() < 128 ? accent.lighter(130) : accent.darker(115)) : muted;
    const QRect iconRect(int(pill.left()) + 12, int(pill.center().y()) - IconSize / 2, IconSize, IconSize);
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const QPixmap pm = tintedIcon(icon, IconSize, iconColor, p->device()->devicePixelRatioF());
    if (!pm.isNull()) {
        p->drawPixmap(iconRect, pm);
    } else { // pas d'icône dans le thème : pastille de couleur
        p->setPen(Qt::NoPen);
        p->setBrush(iconColor);
        p->drawEllipse(QRectF(iconRect).adjusted(5, 5, -5, -5));
    }

    // Compteur : pastille à droite
    const int count = index.data(FolderRoles::Count).toInt();
    const bool isDraft = index.data(FolderRoles::Id).toString() == "DRAFT";
    int right = int(pill.right()) - 10;
    if (count > 0) {
        QFont cf = option.font;
        cf.setPointSizeF(cf.pointSizeF() * 0.85);
        cf.setBold(!isDraft);
        const QFontMetrics cfm(cf);
        const QString label = count > 9999 ? QString("9999+") : QLocale().toString(count);
        const int w = qMax(cfm.horizontalAdvance(label) + 12, cfm.height() + 4);
        const QRectF badge(right - w, pill.center().y() - (cfm.height() + 2) / 2.0, w, cfm.height() + 2);
        if (!isDraft) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? accent : mix(window, accent, window.lightness() < 128 ? 0.35 : 0.18));
            p->drawRoundedRect(badge, badge.height() / 2, badge.height() / 2);
        }
        p->setFont(cf);
        p->setPen(isDraft ? muted : selected ? pal.color(QPalette::HighlightedText)
                                             : (window.lightness() < 128 ? accent.lighter(135) : accent.darker(125)));
        p->drawText(badge, Qt::AlignCenter, label);
        right = int(badge.left()) - 6;
    }

    // Nom
    QFont nf = option.font;
    nf.setBold(selected || (count > 0 && !isDraft));
    p->setFont(nf);
    p->setPen(text);
    const QRect nameRect(iconRect.right() + 12, int(pill.top()), right - iconRect.right() - 12, int(pill.height()));
    p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(nf).elidedText(index.data(FolderRoles::Name).toString(), Qt::ElideRight, nameRect.width()));
    p->restore();
}

// =============================================================================
//  Bouton « Nouveau message »
// =============================================================================
ComposeButton::ComposeButton(QWidget *parent) : QAbstractButton(parent)
{
    setText("Nouveau message");
    setIcon(QIcon(":/sidebar/compose.svg"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setToolTip("Nouveau message (Ctrl+N)");
}

QSize ComposeButton::sizeHint() const
{
    QFont f = font();
    f.setBold(true);
    return {QFontMetrics(f).horizontalAdvance(text()) + 70, qMax(QFontMetrics(f).height() + 22, 44)};
}

void ComposeButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    QColor bg = accent;
    if (isDown())
        bg = accent.darker(115);
    else if (underMouse())
        bg = accent.lighter(110);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -3);
    // Ombre douce
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, underMouse() ? 50 : 30));
    p.drawRoundedRect(r.translated(0, 2), r.height() / 2, r.height() / 2);
    p.setBrush(bg);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    if (hasFocus()) {
        p.setPen(QPen(pal.color(QPalette::HighlightedText), 1.5, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(3, 3, -3, -3), r.height() / 2 - 3, r.height() / 2 - 3);
    }

    const QColor fg = pal.color(QPalette::HighlightedText);
    const QPixmap pm = tintedIcon(icon(), 20, fg, devicePixelRatioF());
    int x = int(r.left()) + 18;
    if (!pm.isNull()) {
        p.drawPixmap(QRect(x, int(r.center().y()) - 10, 20, 20), pm);
        x += 30;
    }
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(QRectF(x, r.top(), r.right() - x - 12, r.height()), Qt::AlignLeft | Qt::AlignVCenter, text());
}

// =============================================================================
//  Compte connecté
// =============================================================================
AccountChip::AccountChip(QWidget *parent) : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setToolTip("Compte, paramètres et déconnexion");
}

void AccountChip::setEmail(const QString &email)
{
    m_email = email;
    setAccessibleName(email.isEmpty() ? QString("Compte") : email);
    update();
}

QSize AccountChip::sizeHint() const
{
    return {180, qMax(fontMetrics().height() * 2 + 12, 48)};
}

void AccountChip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QRectF r = QRectF(rect()).adjusted(SideMargin, 4, -SideMargin, -4);
    if (underMouse() || isDown()) {
        p.setPen(Qt::NoPen);
        p.setBrush(mix(window, text, isDown() ? 0.12 : 0.07));
        p.drawRoundedRect(r, 10, 10);
    }

    // Avatar : initiale sur une couleur tirée de l'adresse
    static const QColor palette[] = {QColor("#1a73e8"), QColor("#188038"), QColor("#e37400"), QColor("#9334e6"),
                                     QColor("#d93025"), QColor("#00897b"), QColor("#c2185b"), QColor("#5f6368")};
    const QColor avatarColor = palette[qHash(m_email) % 8];
    const double d = qMin(r.height() - 8, 34.0);
    const QRectF avatar(r.left() + 8, r.center().y() - d / 2, d, d);
    p.setPen(Qt::NoPen);
    p.setBrush(avatarColor);
    p.drawEllipse(avatar);
    QFont af = font();
    af.setBold(true);
    af.setPointSizeF(af.pointSizeF() * 1.1);
    p.setFont(af);
    p.setPen(Qt::white);
    p.drawText(avatar, Qt::AlignCenter, m_email.isEmpty() ? QString("?") : m_email.left(1).toUpper());

    // Adresse + « Compte et paramètres »
    const double x = avatar.right() + 10;
    const QRectF textRect(x, r.top(), r.right() - x - 22, r.height());
    QFont bf = font();
    bf.setBold(true);
    p.setFont(bf);
    p.setPen(text);
    const QFontMetrics bfm(bf);
    p.drawText(QRectF(textRect.left(), r.center().y() - bfm.height(), textRect.width(), bfm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               bfm.elidedText(m_email.isEmpty() ? QString("Non connecté") : m_email, Qt::ElideMiddle, int(textRect.width())));
    QFont sf = font();
    sf.setPointSizeF(sf.pointSizeF() * 0.88);
    p.setFont(sf);
    p.setPen(mix(text, window, 0.45));
    p.drawText(QRectF(textRect.left(), r.center().y() + 1, textRect.width(), QFontMetrics(sf).height()),
               Qt::AlignLeft | Qt::AlignVCenter, "Compte et paramètres");

    // Chevron vers le haut (le menu s'ouvre au-dessus)
    const QPointF c(r.right() - 12, r.center().y());
    QPainterPath chevron;
    chevron.moveTo(c + QPointF(-4, 2));
    chevron.lineTo(c + QPointF(0, -2));
    chevron.lineTo(c + QPointF(4, 2));
    p.setPen(QPen(mix(text, window, 0.45), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(chevron);
}
