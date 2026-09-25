#pragma once
#include <QPalette>
#include <QString>

// Thème de l'application : « system » (suit KDE), « light » ou « dark »
namespace Theme {
void init();                       // mémorise la palette et les icônes du système (au démarrage)
void enableColorEmoji();           // police emoji en couleur en secours de la police de l'interface
void apply(const QString &mode);
bool isDark();                     // thème effectivement affiché
QPalette palette(bool dark);       // palettes façon Breeze, aussi utilisées pour les aperçus
}
