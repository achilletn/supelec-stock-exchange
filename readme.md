# Supélec Stock Exchange

Jeu de simulation boursière multijoueur en temps réel. Les joueurs démarrent avec **100 000 $** virtuels et tradent des actifs réels (actions, cryptos) dont les cours sont récupérés via l'API TwelveData — ou simulés avec un modèle de marché stochastique si aucune clé n'est configurée.

![Stack](https://img.shields.io/badge/backend-C%2B%2B20-blue) ![DB](https://img.shields.io/badge/database-SQLite3-lightgrey) ![Frontend](https://img.shields.io/badge/frontend-Vanilla%20JS-yellow)

---

## Fonctionnalités

- **Marché en temps réel** — cours mis à jour en continu depuis TwelveData (polling serveur), OHLC + variation 24h
- **50+ actifs** — Big Tech, semiconducteurs, finance, santé, énergie, retail + cryptos (BTC, ETH)
- **Achat / vente** avec frais de 0.2%, quantités entières uniquement
- **Portefeuille** — positions ouvertes, PnL depuis achat, PnL 24h, historique de trades
- **Classement** — leaderboard temps réel avec consultation du portefeuille de chaque joueur
- **Graphiques** — historique de prix sur 1h / 3h / 24h / 7j / 1m / 4m / 1a / 5a (Chart.js)
- **Mode simulation** — si `TWELVEDATA_API_KEY` absent, tous les actifs sont générés par un mouvement brownien géométrique (5 ans d'historique cohérent)
- **Interface admin** — gestion des joueurs, reset de comptes, injection de liquidités, consultation globale
- **Dark mode** — thème sombre Discord-like, persisté par `localStorage`
- **Mobile-first** — panneaux détail et achat/vente en slide-up natif avec swipe-to-dismiss

---

## Stack technique

| Couche | Technologie |
|---|---|
| Backend | C++20, [cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only), OpenSSL |
| Base de données | SQLite3 |
| Frontend | HTML5 / CSS3 / Vanilla JS |
| Graphiques | Chart.js (CDN) |
| Prix | API REST TwelveData |
| Auth | Sessions par cookie (PBKDF2-SHA256, tokens 64 hex) |

---

## Prérequis

```bash
# Arch / CachyOS
sudo pacman -S gcc sqlite openssl

# Debian / Ubuntu
sudo apt install g++ libsqlite3-dev libssl-dev

# Optionnel : accélérer les recompilations
sudo pacman -S ccache   # ou apt install ccache
```

Les dépendances C++ header-only (`httplib.h`, `nlohmann/json.hpp`) sont incluses dans le dépôt.

---

## Installation & lancement

### 1. Cloner et configurer l'environnement

```bash
git clone <url-du-repo>
cd bourse
```

Créer un fichier `.env` à la racine :

```env
ADMIN_USER=admin
ADMIN_PASS=motdepasse_secret
TWELVEDATA_API_KEY=votre_clé_ici   # optionnel — mode simulation si absent
```

> **Sans `TWELVEDATA_API_KEY`**, le serveur génère automatiquement un historique de 5 ans simulé pour chaque actif et met les cours à jour via mouvement brownien. Idéal pour un déploiement hors-ligne ou pour tester.

### 2. Compiler et lancer

```bash
make          # compile serveur.cpp → ./serveur, puis lance sur :8080
```

Ou étape par étape :

```bash
make compile  # compile uniquement
./serveur     # lance manuellement (charge .env automatiquement)
```

L'interface joueur est accessible sur `http://localhost:8080/`.

### 3. Interface admin

```
http://localhost:8080/admin/
```

Identifiants = `ADMIN_USER` / `ADMIN_PASS` du `.env`.

---

## Structure du projet

```
serveur.cpp          # serveur C++ unique — toute la logique backend
htdocs/
  index.html         # interface joueur
  script.js          # logique frontend (~1000 lignes)
  styles.css         # CSS variables light/dark
  admin/
    index.html       # interface admin
    script.js
    style.css
httplib.h            # cpp-httplib (header-only)
Makefile
bourse.db            # SQLite — créée automatiquement au premier lancement
.env                 # variables d'environnement (non commité)
```

---

## API — endpoints principaux

| Méthode | Route | Description |
|---|---|---|
| `GET` | `/api/marche` | Tous les actifs (prix, OHLC, variation 24h) |
| `GET` | `/api/stats?symbole=X` | Stats détaillées + volatilité d'un actif |
| `GET` | `/api/historique?symbole=X&periode=24h` | Historique de prix |
| `GET` | `/api/dashboard` | Solde, PnL, portefeuille du joueur connecté |
| `GET` | `/api/classement` | Classement tous joueurs |
| `GET` | `/api/portefeuille` | Positions ouvertes du joueur connecté |
| `GET` | `/api/portefeuille/joueur?username=X` | Portefeuille d'un autre joueur |
| `POST` | `/api/trade` | Passer un ordre (`{ symbole, action, quantite }`) |
| `POST` | `/api/login` | Connexion |
| `POST` | `/api/register` | Inscription |
| `POST` | `/api/logout` | Déconnexion |

---

## Règles du jeu

- Capital de départ : **100 000 $** par joueur
- Frais de transaction : **0.2%** par ordre (achat et vente)
- Quantités entières uniquement (pas de fractions d'actions)
- Un compte par personne
- Classement basé sur la **valeur totale du portefeuille** (liquidités + positions valorisées au prix actuel)

---

## Sécurité

- Mots de passe hashés avec **PBKDF2-SHA256** (100 000 itérations, sel aléatoire 16 bytes)
- Protection brute-force : blocage de l'IP pendant **15 minutes** après 5 échecs de connexion sur 5 minutes
- Sessions expirables stockées en base (`user_sessions`)
- Admin accessible uniquement par credentials `.env`, jamais en base

---

## Développement

```bash
# Recompiler sans relancer Apache ni le serveur
make compile

# Nettoyer le binaire
make clean
```

Les cours en RAM (`marcheMondial`) sont mutex-protégés et mis à jour par un worker thread dédié. Le frontend se synchronise via `lastMarketUpdateMs` (atomic) pour interpoler l'horloge de marché côté client sans dépendre du clock du serveur.
