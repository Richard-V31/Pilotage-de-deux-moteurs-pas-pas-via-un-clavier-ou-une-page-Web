# Pilotage de deux moteurs pas à pas 
#(Moteur 1 / Moteur 2) avec Arduino UNO R4 WiFi

Pilotage de deux moteurs pas à pas via :
- des **boutons physiques** branchés sur un module d'extension I2C **MCP23008** ;
- une **page web** intégrée (servie directement par l'Arduino), accessible depuis n'importe quel navigateur sur le même réseau WiFi.

Les deux modes de commande fonctionnent **simultanément** et peuvent être utilisés indifféremment, y compris en branchant/débranchant le clavier physique en cours de fonctionnement.

## Fonctionnalités

**Pour chaque moteur (1 et 2), indépendamment :**
- Marche / Arrêt (rotation continue)
- Inversion du sens de rotation
- Mode « 1 Pas » : le moteur avance lentement tant qu'on maintient le bouton
- Réglage de la vitesse en mode Marche, au choix :
  - via le **potentiomètre** physique (commun aux deux moteurs)
  - via un **curseur (slider) web** propre à chaque moteur
- Un interrupteur « **Synchroniser vitesse (M1 ↔ M2)** » permet d'aligner automatiquement la vitesse et la source (potentiomètre/web) du Moteur 2 sur celles du Moteur 1

**Détection automatique du clavier physique :**
- Démarrage possible sans clavier branché (mode Web pur)
- Détection du branchement en moins de 2 secondes
- Détection du débranchement au premier appui / à la première lecture, sans jamais bloquer ni ralentir les moteurs

**Pilotage moteur en temps réel :**
- Un **timer matériel** dédié (Renesas FSP, 4000 Hz) génère les pas des moteurs indépendamment de la boucle principale (`loop()`), pour un mouvement fluide même pendant les traitements WiFi/I2C.

## Matériel nécessaire

| Élément | Rôle |
|---|---|
| Arduino UNO R4 WiFi | Carte principale (WiFi natif + timer matériel Renesas) |
| 2 × moteur pas à pas 28BYJ-48 (+ driver ULN2003) | Moteurs pilotés (Nema 17 également supporté, voir câblage) |
| Module MCP23008 (I2C) | Lecture des 6 boutons physiques |
| 6 × bouton-poussoir | Commandes physiques |
| 1 × potentiomètre | Réglage de vitesse physique |
| 6 × LED (+ résistances) | Retour visuel d'état |

## Câblage / Broches

### Moteurs

| Signal | Moteur 1 | Moteur 2 |
|---|---|---|
| IN1 | 6 | 10 |
| IN2 | 7 | 11 |
| IN3 | 8 | 12 |
| IN4 | 9 | 13 |

> Câblage prévu pour un **28BYJ-48** en mode 4 fils (`FULL4WIRE`, ordre IN1-IN3-IN2-IN4). Les lignes pour le mode demi-pas et pour un **Nema 17** (ordre de broches différent) sont présentes en commentaire dans le code — à décommenter/adapter selon le moteur utilisé.

### LEDs

| LED | Moteur 1 | Moteur 2 |
|---|---|---|
| Pas à pas | 3 | A2 |
| Inversion | 4 | A3 |
| Marche/Arrêt | 5 | 2 |

### Entrées

| Élément | Broche |
|---|---|
| Potentiomètre de vitesse | A0 |
| Module MCP23008 | Bus I2C (SDA/SCL), adresse `0x24` |

### Boutons physiques (via MCP23008)

| Entrée MCP | Bouton | Action |
|---|---|---|
| GP0 | BP1 | 1 Pas Moteur 1 |
| GP1 | BP2 | 1 Pas Moteur 2 |
| GP2 | BP3 | Marche/Arrêt Moteur 1 |
| GP3 | BP4 | Marche/Arrêt Moteur 2 |
| GP4 | BP5 | Inversion sens Moteur 1 |
| GP5 | BP6 | Inversion sens Moteur 2 |

## Bibliothèques Arduino requises

À installer via le gestionnaire de bibliothèques de l'IDE Arduino :

- `AccelStepper`
- `Adafruit MCP23017 Arduino Library` (fournit `Adafruit_MCP23X08.h`)
- `WiFiS3` (incluse avec le core Arduino UNO R4)
- `FspTimer` (incluse avec le core Arduino UNO R4)

## Configuration avant téléversement

Dans le fichier `.ino`, modifier les identifiants WiFi :

```cpp
const char* ssid     = "Votre_SSID";
const char* password = "Votre_Mot_De_Passe";
```

La plage de vitesse (en pas/seconde) peut aussi être ajustée à un seul endroit :

```cpp
#define VITESSE_MIN 5
#define VITESSE_MAX 700
#define VITESSE_DEFAUT 350
```

## Utilisation

1. Téléverser le programme sur l'Arduino UNO R4 WiFi.
2. Ouvrir le moniteur série (115200 bauds) pour suivre la connexion WiFi et récupérer l'**adresse IP** attribuée par le routeur.
3. Ouvrir cette adresse IP dans un navigateur (smartphone, PC…) connecté au **même réseau WiFi**.
4. La page affiche les commandes des deux moteurs ; elle se rafraîchit automatiquement toutes les 700 ms.

Le clavier physique (boutons + MCP23008) peut être branché ou débranché à tout moment sans avoir à redémarrer l'Arduino.

## Interface web — routes HTTP internes

La page web communique avec l'Arduino via de petites requêtes HTTP (utilisées en interne par le JavaScript embarqué) :

| Route | Effet |
|---|---|
| `GET /` | Renvoie la page HTML complète |
| `GET /status` | Renvoie l'état courant (JSON) : marche/arrêt, inversion, source et valeur de vitesse |
| `GET /m1/toggle`, `/m2/toggle` | Bascule Marche/Arrêt |
| `GET /m1/inv`, `/m2/inv` | Bascule l'inversion de sens |
| `GET /m1/step/on`, `/m1/step/off` | Démarre/arrête le mode « 1 Pas » (idem `/m2/...`) |
| `GET /m1/speed/mode?val=web\|pot` | Choisit la source de vitesse (idem `/m2/...`) |
| `GET /m1/speed/set?val=<nombre>` | Définit la vitesse web, bornée à `[VITESSE_MIN, VITESSE_MAX]` (idem `/m2/...`) |

## Structure du code

- **Partie C++** : configuration matérielle, timer moteur, lecture des boutons physiques, serveur web, `setup()`/`loop()`.
- **Page web (HTML + CSS + JS)** : stockée en mémoire programme (`PROGMEM`) sous forme de chaîne brute, servie directement par l'Arduino — aucun fichier externe requis.

Une version du fichier `.ino` avec des commentaires détaillés ligne par ligne est également disponible (`tourne_BP_Impulsion_R4WiFi_commente.ino`).

## Limitations connues

- Les identifiants WiFi sont stockés en clair dans le code source.
- Le serveur web traite une requête à la fois par tour de boucle (suffisant pour un usage local à quelques utilisateurs).
