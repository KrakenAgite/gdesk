# G-Desk

Client mail natif (C++ / Qt6) pour Gmail, sous KDE, utilisant l'**API officielle Gmail**.

**Site et téléchargement : <https://krakenagite.github.io/gdesk/>** · [Politique de confidentialité](https://krakenagite.github.io/gdesk/confidentialite.html) · [Conditions d'utilisation](https://krakenagite.github.io/gdesk/conditions-utilisation.html) · [Mentions légales](https://krakenagite.github.io/gdesk/mentions-legales.html)

- Dossiers, catégories et libellés Gmail avec compteurs de non-lus
- Lecture des messages HTML dans un bac à sable : pas de JavaScript, images distantes
  (pisteurs) bloquées sauf autorisation, images intégrées affichées
- Répondre, répondre à tous, transférer (avec pièces jointes), nouveau message,
  pièces jointes, brouillons enregistrés dans Gmail, autocomplétion des adresses
- Archiver, supprimer, restaurer, spam, lu/non lu, suivi — sur plusieurs messages à la fois
- Recherche avec la syntaxe Gmail (`from:`, `has:attachment`, `after:2026/01/01`…)
- Relève toutes les minutes, notifications KDE, pastille de non-lus, liens `mailto:`
- Connexion OAuth 2.0 (PKCE) dans le navigateur ; jeton rangé dans KWallet

## Première utilisation : identifiants Google Cloud

Google impose que chaque application utilisant l'API Gmail ait son « client OAuth ».
L'assistant intégré (bouton « Identifiants Google Cloud… ») détaille les étapes :

1. Créer un projet sur <https://console.cloud.google.com/>
2. Activer l'**API Gmail**
3. Google Auth Platform → configurer l'écran de consentement (audience **Externe**)
4. Audience → s'ajouter comme **utilisateur test**, puis **Publier l'application**
   (sinon Google demande de se reconnecter tous les 7 jours)
5. Clients → créer un client **Application de bureau** → télécharger le JSON
6. Importer ce JSON dans G-Desk, puis « Se connecter avec Google »

Google affichera « Google n'a pas validé cette application » : c'est normal, c'est votre
propre projet. Cliquez sur « Paramètres avancés » → « Accéder à G-Desk ».

## Installer
Téléchargez le `.deb` depuis la [page des versions](https://github.com/KrakenAgite/gdesk/releases/latest), puis :

    sudo apt install ./gdesk_2.0.1_amd64.deb

## Compiler
    sudo apt install build-essential cmake qt6-base-dev qt6-webengine-dev qtkeychain-qt6-dev qt6-svg-plugins dpkg-dev
    ./build-deb.sh          # produit build/gdesk_<version>_amd64.deb

Tests (sans compte Google) :

    cmake -S . -B build -DGDESK_BUILD_TESTS=ON && cmake --build build
    QT_QPA_PLATFORM=offscreen build/gdesk-tests

## Raccourcis
Ctrl+N nouveau · Ctrl+R répondre · Ctrl+Maj+R répondre à tous · Ctrl+L transférer ·
A archiver · Suppr supprimer · S suivi · M lu/non lu · J spam · Ctrl+F rechercher · F5 actualiser ·
Ctrl+Entrée envoyer (fenêtre de rédaction)

## Site web
Le site (dossier `docs/`) est publié par GitHub Pages. Il est statique, sans cookie ni ressource externe.

## Licence
[MIT](LICENSE) © 2026 Gabriel Arthus. Gmail et Google sont des marques de Google LLC ;
G-Desk n'est ni affilié à Google ni approuvé par Google.
