#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QIcon>
#include <QStandardPaths>
#include <QStyleHints>
#include <QWidget>

namespace {
QString g_systemIconTheme;
bool g_initialized = false;
bool g_customApplied = false; // une palette personnalisée a été posée

bool iconThemeExists(const QString &name)
{
    for (const QString &dir : QIcon::themeSearchPaths())
        if (QFileInfo::exists(dir + "/" + name + "/index.theme"))
            return true;
    return false;
}

// Variante claire ou sombre du thème d'icônes de l'utilisateur (ex. Tela → Tela-dark)
QString iconThemeFor(bool dark)
{
    QString base = g_systemIconTheme;
    for (const char *suffix : {"-dark", "-Dark", "_dark", "-light", "-Light", "_light"})
        if (base.endsWith(QLatin1String(suffix)))
            base.chop(qstrlen(suffix));
    if (dark) {
        for (const QString &candidate : {base + "-dark", base + "-Dark", base + "_dark"})
            if (iconThemeExists(candidate))
                return candidate;
        return iconThemeExists("breeze-dark") ? QString("breeze-dark") : g_systemIconTheme;
    }
    if (iconThemeExists(base))
        return base;
    return iconThemeExists("breeze") ? QString("breeze") : g_systemIconTheme;
}

bool isDarkPalette(const QPalette &p)
{
    return p.color(QPalette::Window).lightness() < 128;
}
} // namespace

namespace Theme {

void init()
{
    g_systemIconTheme = QIcon::themeName();
    g_initialized = true;
}

void enableColorEmoji()
{
    // Qt 6.8 ne choisit pas de lui-même la police emoji en couleur : sans cela, les emojis
    // s'affichent en carrés vides ou en noir et blanc (Symbola), et les drapeaux en lettres.
    QString emojiFamily;
    for (const char *family : {"Noto Color Emoji", "Twemoji", "JoyPixels", "Apple Color Emoji", "Segoe UI Emoji"})
        if (QFontDatabase::hasFamily(family)) {
            emojiFamily = family;
            break;
        }
    if (emojiFamily.isEmpty())
        return;
    QFont font = QApplication::font();
    // Nom réel de la police (et non un alias comme « Sans Serif ») : sinon la police emoji,
    // qui contient aussi chiffres et espaces, remplacerait une partie du texte normal.
    const QString resolved = QFontInfo(font).family();
    if (resolved.isEmpty() || resolved == emojiFamily)
        return;
    font.setFamilies({resolved, emojiFamily}); // la police emoji ne sert qu'aux caractères absents
    QApplication::setFont(font);
}

QPalette palette(bool dark)
{
    QPalette p;
    auto set = [&p](QPalette::ColorRole role, const char *active, const char *disabled = nullptr) {
        p.setColor(QPalette::All, role, QColor(active));
        if (disabled)
            p.setColor(QPalette::Disabled, role, QColor(disabled));
    };
    if (dark) {
        set(QPalette::Window, "#202326");
        set(QPalette::WindowText, "#fcfcfc", "#6e7175");
        set(QPalette::Base, "#141618");
        set(QPalette::AlternateBase, "#1d1f22");
        set(QPalette::Text, "#fcfcfc", "#6e7175");
        set(QPalette::PlaceholderText, "#a1a9b1");
        set(QPalette::Button, "#292c30");
        set(QPalette::ButtonText, "#fcfcfc", "#6e7175");
        set(QPalette::BrightText, "#ffffff");
        set(QPalette::Highlight, "#3daee9", "#2e3236");
        set(QPalette::HighlightedText, "#fcfcfc");
        set(QPalette::ToolTipBase, "#292c30");
        set(QPalette::ToolTipText, "#fcfcfc");
        set(QPalette::Link, "#1d99f3");
        set(QPalette::LinkVisited, "#9b59b6");
        set(QPalette::Light, "#3b3f45");
        set(QPalette::Midlight, "#33373c");
        set(QPalette::Mid, "#3b3f45");
        set(QPalette::Dark, "#0f1011");
        set(QPalette::Shadow, "#000000");
    } else {
        set(QPalette::Window, "#eff0f1");
        set(QPalette::WindowText, "#232627", "#a0a1a3");
        set(QPalette::Base, "#ffffff");
        set(QPalette::AlternateBase, "#f7f7f7");
        set(QPalette::Text, "#232627", "#a0a1a3");
        set(QPalette::PlaceholderText, "#707d8a");
        set(QPalette::Button, "#fcfcfc");
        set(QPalette::ButtonText, "#232627", "#a0a1a3");
        set(QPalette::BrightText, "#ffffff");
        set(QPalette::Highlight, "#3daee9", "#e3e5e7");
        set(QPalette::HighlightedText, "#ffffff");
        set(QPalette::ToolTipBase, "#f7f7f7");
        set(QPalette::ToolTipText, "#232627");
        set(QPalette::Link, "#2980b9");
        set(QPalette::LinkVisited, "#7f8c8d");
        set(QPalette::Light, "#ffffff");
        set(QPalette::Midlight, "#e3e5e7");
        set(QPalette::Mid, "#bdc3c7");
        set(QPalette::Dark, "#888e93");
        set(QPalette::Shadow, "#474a4c");
    }
    return p;
}

void apply(const QString &mode)
{
    if (!g_initialized)
        init();
    if (mode == "light" || mode == "dark") {
        const bool dark = mode == "dark";
        QApplication::setPalette(palette(dark));
        QIcon::setThemeName(iconThemeFor(dark));
        g_customApplied = true;
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    } else {
        if (!g_customApplied)
            return; // déjà sur l'apparence de KDE
        // Palette vide : Qt revient à celle du système et suit ses changements en direct
        QApplication::setPalette(QPalette());
        QIcon::setThemeName(g_systemIconTheme);
        QGuiApplication::styleHints()->unsetColorScheme();
        g_customApplied = false;
    }
    // Les feuilles de style qui utilisent palette(…) doivent être réévaluées
    for (QWidget *w : QApplication::allWidgets())
        if (!w->styleSheet().isEmpty())
            w->setStyleSheet(w->styleSheet());
}

bool isDark()
{
    return isDarkPalette(QApplication::palette());
}

} // namespace Theme
