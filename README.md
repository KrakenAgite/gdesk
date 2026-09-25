# G-Desk

Client mail natif (C++ / Qt6) pour Gmail, sous KDE, utilisant l'**API officielle Gmail**.

**Site et téléchargement : <https://krakenagite.github.io/gdesk/>** · [Politique de confidentialité](https://krakenagite.github.io/gdesk/confidentialite.html) · [Conditions d'utilisation](https://krakenagite.github.io/gdesk/conditions-utilisation.html) · [Mentions légales](https://krakenagite.github.io/gdesk/mentions-legales.html)

- Barre latérale en sections repliables : messagerie (réception, suivis, importants, envoyés, brouillons),
  catégories Gmail (principale, réseaux sociaux, promotions, notifications, forums), spam, corbeille,
  libellés avec leur couleur Gmail, pastilles de non-lus
- Messages présentés en cartes : expéditeur en gras et date, objet, puis début du message
- Lecture des messages HTML dans un bac à sable : pas de JavaScript, images distantes
  (pisteurs) bloquées sauf autorisation, images intégrées affichées
- Emojis en couleur partout et sélecteur d'emojis dans la rédaction ; tous les jeux de caractères
  (UTF-8, Windows-1252, ISO-8859-x, cyrillique, japonais, chinois…)
- Répondre, répondre à tous, transférer (avec pièces jointes), nouveau message,
  pièces jointes, brouillons enregistrés dans Gmail, autocomplétion des adresses
- Cases à cocher sur les messages et barre de sélection (Tous, Lus, Non lus, Suivis… ; Maj+clic pour une plage) :
  lu / non lu, archiver, spam, supprimer, suivi, libellé — sans ouvrir les messages
- Clic droit sur une boîte : tout sélectionner, tout marquer comme lu / non lu, vider la boîte (vers la corbeille)
- **Google Drive** : barre de navigation à gauche pour passer du courrier à Drive (Ctrl+1 / Ctrl+2) ;
  Mon Drive, récents, suivis, partagés, corbeille, recherche ; ouvrir, télécharger, importer (aussi par
  glisser-déposer), nouveau dossier, renommer, suivre, supprimer, envoyer par e-mail, espace utilisé
- Joindre un fichier Google Drive à un message (Docs, Sheets, Slides exportés en .docx, .xlsx, .pptx ; lien
  proposé au-delà de 25 Mo) et enregistrer une pièce jointe dans Google Drive
- Recherche avec la syntaxe Gmail (`from:`, `has:attachment`, `after:2026/01/01`…)
- Relève réglable, notifications KDE par boîte ou catégorie au choix, pastille de non-lus, liens `mailto:`
- Connexion OAuth 2.0 (PKCE) dans le navigateur ; jeton rangé dans KWallet
- Paramètres (Ctrl+,) : thème clair / sombre / KDE, taille des cartes (compacte, aérée, espacée),
  aperçu en dessous ou à droite, taille du texte, nom affiché, signature, boîte au démarrage, fréquence de relève,
  marquage comme lu, gestion des images distantes et des adresses mémorisées

## Première utilisation : identifiants Google Cloud

Google impose que chaque application utilisant l'API Gmail ait son « client OAuth ».
L'assistant intégré (bouton « Identifiants Google Cloud… ») détaille les étapes :

1. Créer un projet sur <https://console.cloud.google.com/>
2. Activer l'**API Gmail** et l'**API Google Drive**
3. Google Auth Platform → configurer l'écran de consentement (audience **Externe**)
4. Audience → s'ajouter comme **utilisateur test**, puis **Publier l'application**
   (sinon Google demande de se reconnecter tous les 7 jours)
5. Clients → créer un client **Application de bureau** → télécharger le JSON
6. Importer ce JSON dans G-Desk, puis « Se connecter avec Google »

Déjà connecté avec une version précédente ? Activez l'API Google Drive dans votre projet, puis ouvrez
la vue Drive : G-Desk vous proposera d'autoriser l'accès à Drive (une seule fois, dans le navigateur).

Google affichera « Google n'a pas validé cette application » : c'est normal, c'est votre
propre projet. Cliquez sur « Paramètres avancés » → « Accéder à G-Desk ».

## Installer
Téléchargez le `.deb` depuis la [page des versions](https://github.com/KrakenAgite/gdesk/releases/latest), puis :

    sudo apt install ./gdesk_2.3.1_amd64.deb

## Compiler
    sudo apt install build-essential cmake qt6-base-dev qt6-webengine-dev qtkeychain-qt6-dev qt6-svg-plugins dpkg-dev
    ./build-deb.sh          # produit build/gdesk_<version>_amd64.deb

Tests (sans compte Google) :

    cmake -S . -B build -DGDESK_BUILD_TESTS=ON && cmake --build build
    QT_QPA_PLATFORM=offscreen build/gdesk-tests

## Raccourcis
Ctrl+N nouveau · Ctrl+R répondre · Ctrl+Maj+R répondre à tous · Ctrl+L transférer ·
A archiver · Suppr supprimer · S suivi · M lu/non lu · J spam · Ctrl+F rechercher · F5 actualiser ·
Ctrl+A tout cocher · Échap décocher · Ctrl+1 courrier · Ctrl+2 Google Drive · Ctrl+, paramètres · Ctrl+Entrée envoyer (fenêtre de rédaction)

## Site web
Le site (dossier `docs/`) est publié par GitHub Pages. Il est statique, sans cookie ni ressource externe.

## Licence
[MIT](LICENSE) © 2026 Gabriel Arthus. Gmail et Google sont des marques de Google LLC ;
G-Desk n'est ni affilié à Google ni approuvé par Google.
