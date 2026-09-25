#!/bin/sh
# Compile G-Desk et construit build/gdesk_<version>_<arch>.deb
# Prérequis : sudo apt install build-essential cmake qt6-base-dev qt6-webengine-dev qtkeychain-qt6-dev dpkg-dev
set -e
umask 022
cd "$(dirname "$0")"

VERSION=$(sed -n 's/^project(gdesk VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
ARCH=$(dpkg --print-architecture)
STAGE=build/pkg
rm -rf "$STAGE"

cmake -S . -B build -DGDESK_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr $CMAKE_EXTRA_ARGS
cmake --build build -j"$(nproc)"
DESTDIR="$PWD/$STAGE" cmake --install build
strip --strip-unneeded "$STAGE/usr/bin/gdesk"

mkdir -p "$STAGE/usr/share/doc/gdesk"
printf 'G-Desk %s\nCopyright (c) 2026 Gabriel Arthus\nLicence : MIT (voir https://github.com/KrakenAgite/gdesk/blob/main/LICENSE)\n' "$VERSION" > "$STAGE/usr/share/doc/gdesk/copyright"

# Dépendances calculées à partir des bibliothèques réellement utilisées
mkdir -p build/shlibs/debian
printf 'Source: gdesk\n\nPackage: gdesk\nArchitecture: any\n' > build/shlibs/debian/control
DEPENDS=$(cd build/shlibs && dpkg-shlibdeps -O "../../$STAGE/usr/bin/gdesk" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
[ -n "$DEPENDS" ] || DEPENDS="libqt6webenginewidgets6 (>= 6.8), libqt6webenginecore6 (>= 6.8), libqt6widgets6, libqt6gui6, libqt6network6, libqt6dbus6, libqt6core6t64"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<CTRL
Package: gdesk
Version: $VERSION
Section: mail
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Recommends: breeze-icon-theme, kwalletmanager
Installed-Size: $(du -sk "$STAGE/usr" | cut -f1)
Maintainer: Gabriel Arthus <120360026+KrakenAgite@users.noreply.github.com>
Homepage: https://krakenagite.github.io/gdesk/
Description: Client mail natif pour Gmail (API officielle)
 G-Desk est un client mail Qt pour KDE qui utilise l'API Gmail de Google :
 dossiers et libellés, lecture sécurisée des messages HTML (images
 distantes bloquées), réponse, transfert, pièces jointes, brouillons,
 recherche, notifications et compteur de non-lus dans la barre système.
 La connexion OAuth se fait dans le navigateur ; le jeton est conservé
 dans KWallet.
CTRL

cat > "$STAGE/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -f /usr/share/icons/hicolor || true
exit 0
POST
cp "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

OUT="build/gdesk_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
echo "Paquet créé : $OUT"
