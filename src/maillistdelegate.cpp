#include "maillistdelegate.h"

#include <QApplication>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextLayout>

namespace {
constexpr int CardMarginH = 8;  // espace entre la carte et les bords de la liste
constexpr int CardMarginV = 3;  // demi-espace entre deux cartes
constexpr int DotSpace = 30;    // colonne du point « non lu » / de la case à cocher
constexpr int CheckSize = 18;
constexpr int StarSize = 16;
constexpr int LineGap = 2;

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

// Fond des cartes : un peu plus clair que la fenêtre en thème sombre, blanc (Base) en thème clair
QColor cardColor(const QPalette &pal)
{
    const QColor window = pal.color(QPalette::Window);
    return window.lightness() < 128 ? window.lighter(118) : pal.color(QPalette::Base);
}

QFont boldFont(QFont f)
{
    f.setBold(true);
    return f;
}

QFont smallFont(QFont f)
{
    f.setPointSizeF(f.pointSizeF() * 0.9);
    return f;
}
} // namespace

int MailListDelegate::padding() const
{
    return density == "compact" ? 5 : density == "spacious" ? 12 : 8;
}

int MailListDelegate::snippetLines() const
{
    if (!showSnippet || density == "compact")
        return 0; // en compact, le début du message suit l'objet sur la même ligne
    return density == "spacious" ? 2 : 1;
}

QSize MailListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    const QFontMetrics bold(boldFont(option.font));
    const QFontMetrics normal(option.font);
    const QFontMetrics small(smallFont(option.font));
    int h = bold.height() + LineGap + normal.height();
    if (snippetLines() > 0)
        h += LineGap + snippetLines() * small.height();
    return {200, h + 2 * padding() + 2 * CardMarginV};
}

QRect MailListDelegate::cardRect(const QRect &row) const
{
    return row.adjusted(CardMarginH, CardMarginV, -CardMarginH, -CardMarginV);
}

QRect MailListDelegate::checkRect(const QRect &row) const
{
    const QRect card = cardRect(row);
    return {card.left() + (DotSpace - CheckSize) / 2 + 1, card.center().y() - CheckSize / 2, CheckSize, CheckSize};
}

void MailListDelegate::attachTo(QAbstractItemView *view)
{
    m_view = view;
    view->viewport()->installEventFilter(this);
}

bool MailListDelegate::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type t = event->type();
    if (!m_view || watched != m_view->viewport()
        || (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonRelease && t != QEvent::MouseButtonDblClick))
        return false;
    auto *me = static_cast<QMouseEvent *>(event);
    if (me->button() != Qt::LeftButton)
        return false;
    const QPoint pos = me->position().toPoint();
    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid())
        return false;
    QStyleOptionViewItem opt;
    opt.initFrom(m_view->viewport());
    opt.font = m_view->font();
    opt.rect = m_view->visualRect(index);
    const bool onCheck = checkRect(opt.rect).adjusted(-6, -8, 6, 8).contains(pos);
    const bool onStar = !index.data(MailRoles::Subject).toString().isEmpty()
                        && starRect(opt).adjusted(-5, -5, 5, 5).contains(pos);
    if (!onCheck && !onStar)
        return false;
    if (t == QEvent::MouseButtonRelease) {
        if (onCheck)
            emit checkClicked(index, me->modifiers());
        else
            emit starClicked(index);
    }
    return true; // ni sélection, ni ouverture du message
}

QRect MailListDelegate::starRect(const QStyleOptionViewItem &option) const
{
    const QRect card = cardRect(option.rect);
    const QFontMetrics bold(boldFont(option.font));
    const QFontMetrics normal(option.font);
    const int line2Top = card.top() + padding() + bold.height() + LineGap;
    return {card.right() - padding() - StarSize + 1, line2Top + (normal.height() - StarSize) / 2, StarSize, StarSize};
}

void MailListDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QPalette &pal = option.palette;
    const QStringList labels = index.data(MailRoles::Labels).toStringList();
    const bool unread = labels.contains("UNREAD");
    const bool starred = labels.contains("STARRED");
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor text = pal.color(QPalette::Text);
    const QColor muted = mix(text, cardColor(pal), 0.42);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    // Carte
    const QRectF card = QRectF(cardRect(option.rect)).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor bg = cardColor(pal);
    if (selected)
        bg = mix(bg, accent, 0.22);
    else if (index.data(MailRoles::Checked).toBool())
        bg = mix(bg, accent, 0.10);
    else if (hovered)
        bg = mix(bg, text, 0.04);
    QColor border = selected ? accent : mix(bg, text, 0.12);
    p->setPen(QPen(border, selected ? 1.5 : 1));
    p->setBrush(bg);
    p->drawRoundedRect(card, 8, 8);
    if (selected) {
        // Barre d'accent à gauche pour repérer le message ouvert
        QPainterPath clip;
        clip.addRoundedRect(card, 8, 8);
        p->setClipPath(clip);
        p->fillRect(QRectF(card.left(), card.top(), 3.5, card.height()), accent);
        p->setClipping(false);
    }

    const QRect content = cardRect(option.rect).adjusted(DotSpace, padding(), -padding(), -padding());
    const QFont normalFont = option.font;
    const QFont bold = boldFont(option.font);
    const QFont small = smallFont(option.font);
    const QFontMetrics fmBold(bold), fmNormal(normalFont), fmSmall(small);

    // Case à cocher (au survol, ou partout dès qu'un message est coché), sinon point « non lu »
    const bool checked = index.data(MailRoles::Checked).toBool();
    if (checked || hovered || selectionMode) {
        QStyleOptionButton box;
        box.rect = checkRect(option.rect);
        box.state = QStyle::State_Enabled | (checked ? QStyle::State_On : QStyle::State_Off);
        if (hovered)
            box.state |= QStyle::State_MouseOver;
        box.palette = pal;
        const QWidget *w = option.widget;
        (w ? w->style() : QApplication::style())->drawPrimitive(QStyle::PE_IndicatorCheckBox, &box, p, w);
    } else if (unread) {
        p->setPen(Qt::NoPen);
        p->setBrush(accent);
        const double d = 8;
        const QRect check = checkRect(option.rect);
        p->drawEllipse(QRectF(check.center().x() - d / 2 + 0.5, check.center().y() - d / 2 + 0.5, d, d));
    }

    const QString subject = index.data(MailRoles::Subject).toString();
    if (subject.isEmpty()) { // métadonnées pas encore reçues
        p->setFont(normalFont);
        p->setPen(muted);
        p->drawText(content, Qt::AlignLeft | Qt::AlignVCenter, "Chargement…");
        p->restore();
        return;
    }

    // Ligne 1 : expéditeur en gras, date à droite
    const QString date = index.data(MailRoles::Date).toString();
    QFont dateFont = small;
    dateFont.setBold(unread);
    const QFontMetrics fmDate(dateFont);
    const int dateWidth = fmDate.horizontalAdvance(date);
    const QRect line1(content.left(), content.top(), content.width(), fmBold.height());
    p->setFont(dateFont);
    p->setPen(unread ? accent : muted);
    p->drawText(line1, Qt::AlignRight | Qt::AlignVCenter, date);
    p->setFont(bold);
    p->setPen(text);
    p->drawText(line1.adjusted(0, 0, -(dateWidth + 10), 0), Qt::AlignLeft | Qt::AlignVCenter,
                fmBold.elidedText(index.data(MailRoles::Who).toString(), Qt::ElideRight, line1.width() - dateWidth - 10));

    // Ligne 2 : objet (et, en compact, début du message à la suite), étoile à droite
    const QRect star = starRect(option);
    const bool showStar = starred || hovered;
    const QRect line2(content.left(), line1.bottom() + 1 + LineGap,
                      content.width() - (showStar ? StarSize + 6 : 0), fmNormal.height());
    const QFont subjectFont = unread ? boldFont(normalFont) : normalFont;
    const QFontMetrics fmSubject(subjectFont);
    const QString subjectText = fmSubject.elidedText(subject, Qt::ElideRight, line2.width());
    p->setFont(subjectFont);
    p->setPen(text);
    p->drawText(line2, Qt::AlignLeft | Qt::AlignVCenter, subjectText);

    const QString snippet = index.data(MailRoles::Snippet).toString().simplified();
    if (density == "compact" && showSnippet && !snippet.isEmpty()) {
        const int used = fmSubject.horizontalAdvance(subjectText) + 12;
        const int remaining = line2.width() - used;
        if (remaining > 60) { // « si il y a la place »
            p->setFont(small);
            p->setPen(muted);
            p->drawText(QRect(line2.left() + used, line2.top(), remaining, line2.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, fmSmall.elidedText(snippet, Qt::ElideRight, remaining));
        }
    }

    if (showStar) {
        const QIcon icon = QIcon::fromTheme(starred ? "rating" : "rating-unrated");
        if (!icon.isNull()) {
            p->setOpacity(starred ? 1.0 : 0.45);
            icon.paint(p, star);
            p->setOpacity(1.0);
        } else { // thème d'icônes sans étoile : dessin simple
            p->setFont(normalFont);
            p->setPen(starred ? QColor("#f4b400") : muted);
            p->drawText(star, Qt::AlignCenter, starred ? "★" : "☆");
        }
    }

    // Lignes 3 (et 4) : début du message, coupé proprement
    const int lines = snippetLines();
    if (lines > 0 && !snippet.isEmpty()) {
        p->setFont(small);
        p->setPen(muted);
        QTextLayout layout(snippet, small);
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(opt);
        layout.beginLayout();
        int y = line2.bottom() + 1 + LineGap;
        for (int i = 0; i < lines; ++i) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(content.width());
            const bool last = i == lines - 1;
            QString part = snippet.mid(line.textStart(), last ? -1 : line.textLength());
            if (last)
                part = fmSmall.elidedText(part, Qt::ElideRight, content.width());
            p->drawText(QRect(content.left(), y, content.width(), fmSmall.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        part.trimmed());
            y += fmSmall.height();
        }
        layout.endLayout();
    }
    p->restore();
}
