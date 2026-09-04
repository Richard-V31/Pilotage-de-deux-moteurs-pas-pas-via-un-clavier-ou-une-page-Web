// =====================================================
// Tourne-BP - Impulsion avec MCP23008 + Interface Web
// Version : Arduino UNO R4 WiFi
// =====================================================
// - Boutons physiques (MCP23008) : fonctionnement identique à l'original
// - Interface Web : reprend les commandes Marche/Arrêt, 1 Pas, Inversion
//                    pour les 2 moteurs, accessible depuis un navigateur
// =====================================================
//  - Au démarrage sans clavier : 
//    L'Arduino démarre instantanément, active le WiFi et gère les moteurs via la page Web.
//  - Dès que vous branchez le clavier : 
//    Dans les 2 secondes qui suivent, l'Arduino le détecte, le configure et les boutons 
//    physiques deviennent instantanément fonctionnels.
//  - Si vous débranchez le clavier : 
//    L'Arduino s'en aperçoit au premier appui (ou à la première lecture), bascule en mode Web pur,
//    et attend qu'on le rebranche sans jamais faire bugger ou ralentir vos moteurs.
// =====================================================

// ---- Bibliothèques utilisées ----
#include <AccelStepper.h>       // Pilotage des moteurs pas à pas (gère l'accélération/la vitesse)
#include <Wire.h>               // Bus I2C, utilisé pour dialoguer avec le module MCP23008
#include <Adafruit_MCP23X08.h>  // Pilote du port d'extension I2C MCP23008 (les boutons physiques)
#include <WiFiS3.h>             // Bibliothèque WiFi native du R4 WiFi
#include "FspTimer.h"           // Timer matériel du R4 (Renesas) - découple les pas moteur du reste du code

// =====================
// IDENTIFIANTS WIFI  ⚠️ A MODIFIER
// =====================
const char* ssid     = "Votre_SSID";
const char* password = "Votre_Mot_De_Passe";

WiFiServer server(80);  // Serveur web écoutant sur le port HTTP standard (80)

Adafruit_MCP23X08 mcp;         // Objet pilotant le module d'extension I2C (boutons physiques)
#define AdrMCP 0x24             // ⚠️ Adresse I2C du module MCP
bool mcpDisponible = false;    // 🟢 Pour tester la présence du module (true = clavier branché et détecté)

// =====================
// MOTEURS
// =====================
// Broches numériques reliées au driver du moteur 1 (ordre IN1/IN2/IN3/IN4 du driver ULN2003 par ex.)
#define Moteur1_IN1 6
#define Moteur1_IN2 7
#define Moteur1_IN3 8
#define Moteur1_IN4 9

// Broches numériques reliées au driver du moteur 2
#define Moteur2_IN1 10
#define Moteur2_IN2 11
#define Moteur2_IN3 12
#define Moteur2_IN4 13

// Pour Moteur 28BYJ-48 (mode 4 fils)
// Remarque : l'ordre IN1,IN3,IN2,IN4 est celui attendu par AccelStepper pour ce câblage précis
AccelStepper Mt1(AccelStepper::FULL4WIRE, Moteur1_IN1, Moteur1_IN3, Moteur1_IN2, Moteur1_IN4);
AccelStepper Mt2(AccelStepper::FULL4WIRE, Moteur2_IN1, Moteur2_IN3, Moteur2_IN2, Moteur2_IN4);
// Variante "demi-pas" (couple/douceur différents) - laissée en commentaire, non utilisée
//AccelStepper Mt1(AccelStepper::HALF4WIRE, Moteur1_IN1, Moteur1_IN3, Moteur1_IN2, Moteur1_IN4);
//AccelStepper Mt2(AccelStepper::HALF4WIRE, Moteur2_IN1, Moteur2_IN3, Moteur2_IN2, Moteur2_IN4);

// Pour Nema 17 (mode 4 fils) - variante alternative avec un câblage/ordre de broches différent
//AccelStepper Mt1(AccelStepper::FULL4WIRE, Moteur1_IN1, Moteur1_IN2, Moteur1_IN3, Moteur1_IN4);
//AccelStepper Mt2(AccelStepper::FULL4WIRE, Moteur2_IN1, Moteur2_IN2, Moteur2_IN3, Moteur2_IN4);

// =====================
// LED MOTEUR 1
// =====================
#define Moteur1_LED_STEP 3    // LED allumée pendant un "pas à pas" manuel du moteur 1
#define Moteur1_LED_INV 4     // LED indiquant que le sens du moteur 1 est inversé
#define Moteur1_LED_ONOFF 5   // LED indiquant que le moteur 1 tourne en continu (mode auto)

// =====================
// LED MOTEUR 2
// =====================
#define Moteur2_LED_STEP A2   // LED "pas à pas" du moteur 2 (utilise une broche analogique en sortie tout-ou-rien)
#define Moteur2_LED_INV A3    // LED "inversion de sens" du moteur 2
#define Moteur2_LED_ONOFF 2   // LED "marche continue" du moteur 2

// =====================
// ENTREES
// =====================
#define Vitesse_POT A0   // Broche analogique reliée au potentiomètre de vitesse

// =====================
// PLAGE DE VITESSE (pas/s) - à modifier ICI uniquement
// =====================
#define VITESSE_MIN 5       // Vitesse minimale autorisée (pas par seconde)
#define VITESSE_MAX 700      // Vitesse maximale autorisée (pas par seconde)
#define VITESSE_DEFAUT 350   // Vitesse par défaut au démarrage / valeur initiale du slider web

// Macros utilitaires pour insérer VITESSE_MIN/MAX/DEFAUT (des nombres)
// directement dans le texte HTML au moment de la compilation.
#define STR_HELPER(x) #x   // Transforme un nombre/mot en chaîne de caractères ("stringification" du préprocesseur)
#define STR(x) STR_HELPER(x)  // Étage intermédiaire nécessaire pour que x soit d'abord remplacé par sa valeur

// =====================
// ETATS
// =====================
bool invM1 = false;   // true = le moteur 1 tourne en sens inversé
bool invM2 = false;   // true = le moteur 2 tourne en sens inversé

bool autoM1 = false;  // true = le moteur 1 tourne en continu (mode "Marche")
bool autoM2 = false;  // true = le moteur 2 tourne en continu (mode "Marche")

int ancienBouton = 0;  // Mémorise le dernier bouton physique lu, pour détecter un "front montant" (nouvel appui)

// Etats déclenchés depuis l'interface Web (bouton "1 Pas" maintenu)
volatile bool webStepM1 = false;  // true tant que l'utilisateur maintient le bouton "1 Pas" du moteur 1 sur la page web
volatile bool webStepM2 = false;  // idem pour le moteur 2

// Contrôle de la vitesse en mode auto : potentiomètre (par défaut) ou web
// Indépendant pour chaque moteur.
volatile bool vitesseSourceWebM1 = false;  // false = potentiomètre, true = curseur web (moteur 1)
volatile bool vitesseSourceWebM2 = false;  // false = potentiomètre, true = curseur web (moteur 2)
volatile float vitesseWebM1 = VITESSE_DEFAUT;  // valeur définie via le curseur web (Moteur 1)
volatile float vitesseWebM2 = VITESSE_DEFAUT;  // valeur définie via le curseur web (Moteur 2)

// =====================
// TIMER MATERIEL - pilotage des moteurs indépendant de loop()
// =====================
// C'est ce timer qui appelle runSpeed() en continu, à fréquence fixe,
// quoi qu'il arrive dans loop() (WiFi, I2C, etc.) : les moteurs restent
// donc fluides même si le traitement web prend quelques millisecondes.
FspTimer motorTimer;              // Objet représentant le timer matériel du R4 (Renesas)
volatile bool activeM1 = false;   // true = le moteur 1 doit tourner (lu par l'interruption du timer)
volatile bool activeM2 = false;   // true = le moteur 2 doit tourner (lu par l'interruption du timer)

// Fonction appelée automatiquement par le timer matériel, à haute fréquence (4000 Hz, voir setup())
void timerMoteurs(timer_callback_args_t __attribute__((unused)) *args) {
  if (activeM1) Mt1.runSpeed();  // Si le moteur 1 est actif, avance d'un pas si le délai de vitesse est écoulé
  if (activeM2) Mt2.runSpeed();  // Idem pour le moteur 2
}

// Configure et démarre le timer matériel qui appellera timerMoteurs() en boucle
bool demarrerTimerMoteurs(float frequenceHz) {
  uint8_t timerType = GPT_TIMER;                                   // Type de timer matériel visé (General PWM Timer)
  int8_t indexTimer = FspTimer::get_available_timer(timerType);    // Cherche un timer GPT libre
  if (indexTimer < 0) {
    // Aucun timer "normal" libre : on tente avec les timers habituellement réservés au PWM
    indexTimer = FspTimer::get_available_timer(timerType, true);
  }
  if (indexTimer < 0) return false;   // Vraiment aucun timer disponible : échec

  FspTimer::force_use_of_pwm_reserved_timer();  // Autorise l'utilisation d'un timer normalement dédié au PWM

  // Initialise le timer en mode périodique à la fréquence demandée, avec timerMoteurs() comme callback
  if (!motorTimer.begin(TIMER_MODE_PERIODIC, timerType, indexTimer, frequenceHz, 0.0f, timerMoteurs)) return false;
  if (!motorTimer.setup_overflow_irq()) return false;  // Active l'interruption de dépassement (déclenche le callback)
  if (!motorTimer.open()) return false;                // Ouvre/réserve le timer matériel
  if (!motorTimer.start()) return false;               // Démarre effectivement le comptage
  return true;   // Tout s'est bien passé
}

// =====================
// LECTURE BOUTONS PHYSIQUES (MCP23008)
// =====================
/*  GP0 → BP1 1pas Moteur 1
    GP1 → BP2 1pas Moteur 2
    GP2 → BP3 Rotation Moteur 1
    GP3 → BP4 Rotation Moteur 2
    GP4 → BP5 Inversion Rotation Moteur 1
    GP5 → BP6 Inversion Rotation Moteur 2
*/
// Ancienne version de lireBouton(), conservée en commentaire pour référence :
// ne testait pas la présence du MCP avant de lire ses broches.
/*int lireBouton() {
  for (int i = 0; i < 6; i++) {
    if (mcp.digitalRead(i) == LOW) {
      if (mcp.digitalRead(i) == LOW)
        return i + 1;
    }
  }
  return 0;
}
*/
// Version actuelle : vérifie d'abord que le module I2C répond toujours avant de lire les boutons
int lireBouton() {
  // On vérifie rapidement si le MCP répond toujours sur le bus
  Wire.beginTransmission(AdrMCP);           // Démarre une transmission I2C "à vide" vers le MCP
  if (Wire.endTransmission() != 0) {        // Si la transmission échoue, le module ne répond plus
    mcpDisponible = false;                  // Il a été débranché !
    Serial.println("-> Clavier physique débranché !");
    return 0;                               // Aucun bouton à traiter ce tour-ci
  }

  for (int i = 0; i < 6; i++) {                 // Parcourt les 6 entrées utilisées (GP0 à GP5)
    if (mcp.digitalRead(i) == LOW) {            // Une entrée à LOW = bouton appuyé (pull-up interne)
      delay(5);                                 // Petit anti-rebond matériel rapide
      if (mcp.digitalRead(i) == LOW)            // On revérifie après le délai pour confirmer l'appui
        return i + 1;                           // Retourne le numéro de bouton (1 à 6)
    }
  }
  return 0;   // Aucun bouton appuyé
}

// =====================
// PAGE WEB (HTML + JS) - ESPACES CORRIGÉS
// =====================
// Toute la page servie au navigateur est stockée en mémoire programme (PROGMEM)
// sous forme de chaîne brute R"HTML( ... )HTML" pour ne pas consommer de RAM.
const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>Commande Moteurs</title>
<style>
    /* Mise en page générale de la page (fond sombre, centrage vertical en colonne) */
    body {
    font-family: Arial, Helvetica, sans-serif;
    background: #111;
    color: #eee;
    text-align: center;
    margin: 0;
    padding: 5px 10px;
    height: 100vh;
    box-sizing: border-box;
    display: flex;
    flex-direction: column;
    justify-content: flex-around; /* 👈 Modifié ici (au lieu de space-around) */
  }

  /* Titre principal en haut de page */
  h1 {  /* 👈 Titre */
    font-size: 1.1em;
    margin: 25px 0 30px 0; /* 👈 Force une marge de 10px en haut de l'ecran et 30px entre le titre et la suite*/
  }
  /* Carte/bloc englobant chaque moteur (fond légèrement plus clair, coins arrondis) */
  .moteur {
    background: #1e1e1e;
    border-radius: 8px;
    padding: 8px 12px;
    margin: 20px auto; /* 👈 Modifié l'eccart entre les deux blocs Moteur 1 et 2*/
    width: 100%;
    max-width: 360px;
    box-sizing: border-box;
  }
  /* Titre "Moteur 1" / "Moteur 2" à l'intérieur de chaque carte */
  .moteur h2 {
    margin: 0 0 8px 0;
    font-size: 1.2em;/* 👈 Modifie la taille des caractéres pour les titre Moteur 1 et Moteur 2 */
    color: #9cf;
  }
  /* Style de base commun à tous les boutons */
  button {
    display: block;
    width: 100%;
    padding: 20px; /*👈 Modifie la taille des boutons */
    margin: 4px 0;
    font-size: 0.95em;
    font-weight: bold;
    border: none;
    border-radius: 6px;
    background: #333;
    color: #eee;
    cursor: pointer;
    -webkit-user-select: none;
    user-select: none;
    transition: background .15s;
  }
  button:active { background: #555; }        /* Effet visuel pendant l'appui (tactile/souris) */
  button.on { background: #C22B2B; }          /* Bouton "Marche/Arrêt" en rouge quand le moteur tourne */
  button.inv { background: #C97124; }         /* Bouton "Inversion" en orange quand le sens est inversé */
  .step-btn { background: #444; }             /* Style de base du bouton "Pas à Pas" */
  .step-btn.active { background: #2B58C0; }   /* Bouton "Pas à Pas" en bleu tant qu'il est maintenu */

  .btn-row { display: flex; gap: 8px; }       /* Aligne côte à côte les boutons Marche/Arrêt et Inversion */
  .btn-row button { margin: 4px 0; }

  /* Bloc de réglage de vitesse (séparé du reste par une ligne fine) */
  .vitesse-bloc {
    margin-top: 6px;
    padding-top: 6px;
    border-top: 1px solid #333;
  }
  /* Ligne contenant le switch Potentiomètre/Web */
  .switch-row {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 10px;
    margin: 30px 0;/*👈 30px Modifie l'ecart  entre le slider et le switch Potentiometre-Web*/
    font-size: 0.85em;
  }
  /* Conteneur visuel de l'interrupteur à bascule (case à cocher stylée en "switch") */
  .switch {
    position: relative;
    display: inline-block;
    width: 42px;
    height: 22px;
    flex-shrink: 0;
  }
  .switch input { opacity: 0; width: 0; height: 0; }   /* Case à cocher réelle rendue invisible */
  /* Piste du switch (le fond du bouton à bascule) */
  .slider-switch {
    position: absolute;
    cursor: pointer;
    top: 0; left: 0; right: 0; bottom: 0;
    background: #555;
    border-radius: 22px;
    transition: .2s;
  }
  /* Le rond (curseur) du switch */
  .slider-switch:before {
    position: absolute;
    content: "";
    height: 16px;
    width: 16px;
    left: 3px;
    bottom: 3px;
    background: #eee;
    border-radius: 50%;
    transition: .2s;
  }
  input:checked + .slider-switch { background: #2a8f4a; }              /* Piste verte quand activé */
  input:checked + .slider-switch:before { transform: translateX(20px); } /* Le rond glisse à droite */
  
  /* Ligne regroupant le slider de vitesse et la valeur numérique affichée */
  .slider-container {
    display: flex;
    align-items: center;
    gap: 10px;
    margin: 2px 0;/*👈 30px Modifie l'ecart  entre le slider et le switch Potentiometre-Web*/
  }
  /* Style du curseur de vitesse (input range) */
  input[type=range] {
    flex: 1;
    margin: 0;
    accent-color: #2a8f4a;
    height: 20px;
  }
  /* Affichage numérique de la valeur de vitesse à droite du slider */
  .vitesseValeur {
    font-size: 1.1em;
    color: #e67e22;
    font-weight: bold;
    min-width: 45px;
    text-align: right;
  }

  /* Bandeau tout en haut avec le switch "Synchroniser vitesse" */
  .sync-row {
    display: flex;/* Active le mode Flexbox. Cela aligne tous les éléments enfants (à l'intérieur de cette ligne) horizontalement par défaut.*/
    align-items: center;
    justify-content: center;
    gap: 1px; /* Crée un espace de 1 pixels entre chaque élément*/
    max-width: 360px;
    width: 100%;
    margin: 0 auto -4px; /* 👈 Modifié l'espace entre Synchroniser vitesse et bloc Moteur 1 */
    padding: 6px;
    background: #1e1e1e;
    border-radius: 8px;
    font-size: 1.05em;
    box-sizing: border-box;
  }
</style>
</head>
<body>
<h1>Commande des moteurs</h1>

<!-- Interrupteur global : synchronise la vitesse des deux moteurs quand il est activé -->
<div class="sync-row">
  <label class="switch">
    <input type="checkbox" id="syncSwitch" onchange="syncChange(this.checked)">
    <span class="slider-switch"></span>
  </label>
  <span>Synchroniser vitesse (M1 ↔ M2)</span>
</div>

<!-- ===================== Bloc de commande du Moteur 1 ===================== -->
<div class="moteur">
  <h2>Moteur 1</h2>
  <div class="btn-row">
    <!-- Bouton Marche/Arrêt : bascule autoM1 côté Arduino -->
    <button id="m1onoff" onclick="toggle('m1')">Marche / Arrêt</button>
    <!-- Bouton Inversion : bascule invM1 côté Arduino -->
    <button id="m1inv" onclick="toggle('m1inv')">Inversion sens</button>
  </div>
  <!-- Bouton "Pas à Pas" : fait tourner le moteur uniquement tant qu'il est maintenu appuyé -->
  <button id="m1step" class="step-btn"
    ontouchstart="stepStart('m1')" ontouchend="stepStop('m1')"
    onmousedown="stepStart('m1')" onmouseup="stepStop('m1')" onmouseleave="stepStop('m1')">
    Pas à Pas (maintenir)
  </button>

  <div class="vitesse-bloc">
    <!-- Switch pour choisir la source de vitesse : potentiomètre physique ou slider web -->
    <div class="switch-row">
      <span>Potentiomètre</span>
      <label class="switch">
        <input type="checkbox" id="m1vitesseSwitch" onchange="modeVitesse('m1', this.checked)">
        <span class="slider-switch"></span>
      </label>
      <span>Web</span>
    </div>
    <!-- Curseur de vitesse web : min/max/valeur générés depuis les macros C++ (VITESSE_MIN/MAX/DEFAUT) -->
    <div class="slider-container">
      <input type="range" id="m1vitesseSlider" min=")HTML" STR(VITESSE_MIN) R"HTML(" max=")HTML" STR(VITESSE_MAX) R"HTML(" value=")HTML" STR(VITESSE_DEFAUT) R"HTML("
        oninput="onSlide('m1', this.value)"
        onchange="onSlide('m1', this.value)" disabled>
      <!-- Valeur numérique affichée en face du slider, mise à jour par onSlide() -->
      <div class="vitesseValeur" id="m1vitesseValeur">)HTML" STR(VITESSE_DEFAUT) R"HTML(</div>
    </div>
  </div>
</div>

<!-- ===================== Bloc de commande du Moteur 2 (identique au Moteur 1) ===================== -->
<div class="moteur">
  <h2>Moteur 2</h2>
  <div class="btn-row">
    <button id="m2onoff" onclick="toggle('m2')">Marche / Arrêt</button>
    <button id="m2inv" onclick="toggle('m2inv')">Inversion sens</button>
  </div>
  <button id="m2step" class="step-btn"
    ontouchstart="stepStart('m2')" ontouchend="stepStop('m2')"
    onmousedown="stepStart('m2')" onmouseup="stepStop('m2')" onmouseleave="stepStop('m2')">
    Pas à Pas (maintenir)
  </button>

  <div class="vitesse-bloc">
    <div class="switch-row">
      <span>Potentiomètre</span>
      <label class="switch">
        <input type="checkbox" id="m2vitesseSwitch" onchange="modeVitesse('m2', this.checked)">
        <span class="slider-switch"></span>
      </label>
      <span>Web</span>
    </div>
    <div class="slider-container">
      <input type="range" id="m2vitesseSlider" min=")HTML" STR(VITESSE_MIN) R"HTML(" max=")HTML" STR(VITESSE_MAX) R"HTML(" value=")HTML" STR(VITESSE_DEFAUT) R"HTML("
        oninput="onSlide('m2', this.value)"
        onchange="onSlide('m2', this.value)" disabled>
      <div class="vitesseValeur" id="m2vitesseValeur">)HTML" STR(VITESSE_DEFAUT) R"HTML(</div>
    </div>
  </div>
</div>

<script>
// true quand le switch "Synchroniser vitesse (M1 ↔ M2)" est activé
let syncMode = false;
// Petite fonction utilitaire : renvoie "m2" si on lui donne "m1", et inversement
function autreMoteur(m){ return m == "m1" ? "m2" : "m1"; }

// Appelée quand l'utilisateur active/désactive le switch de synchronisation
function syncChange(checked){
  syncMode = checked;                 // Mémorise le nouvel état
  if (!syncMode) return;              // Si on désactive la synchro, rien d'autre à faire
  // On aligne immédiatement M2 sur l'état actuel de M1 (mode + valeur du slider)
  const webMode = document.getElementById("m1vitesseSwitch").checked;
  const val = document.getElementById("m1vitesseSlider").value;
  document.getElementById("m2vitesseSwitch").checked = webMode;   // Coche/décoche le switch de M2
  document.getElementById("m2vitesseSlider").disabled = !webMode; // Active/désactive le slider de M2
  document.getElementById("m2vitesseSlider").value = val;         // Copie la position du slider
  document.getElementById("m2vitesseValeur").textContent = val;   // Copie la valeur affichée
  fetch("/m2/speed/mode?val=" + (webMode ? "web" : "pot"));        // Informe l'Arduino du mode de M2
  fetch("/m2/speed/set?val=" + val);                                // Informe l'Arduino de la vitesse de M2
}

// Gère les boutons "Marche/Arrêt" et "Inversion sens" pour M1 et M2
function toggle(what){
  let url = "";
  if(what=="m1") url="/m1/toggle";      // Marche/Arrêt moteur 1
  if(what=="m2") url="/m2/toggle";      // Marche/Arrêt moteur 2
  if(what=="m1inv") url="/m1/inv";      // Inversion sens moteur 1
  if(what=="m2inv") url="/m2/inv";      // Inversion sens moteur 2
  fetch(url).then(refresh);             // Envoie la commande puis rafraîchit l'affichage des boutons
}
// Appelée quand l'utilisateur appuie sur le bouton "Pas à Pas" (m = "m1" ou "m2")
function stepStart(m){
  fetch("/"+m+"/step/on");                              // Démarre le pas à pas côté Arduino
  document.getElementById(m+"step").classList.add("active"); // Colore le bouton en bleu (retour visuel)
}
// Appelée quand l'utilisateur relâche le bouton "Pas à Pas"
function stepStop(m){
  fetch("/"+m+"/step/off");                                // Arrête le pas à pas côté Arduino
  document.getElementById(m+"step").classList.remove("active"); // Retire la couleur active
}
// Appelée quand on bascule le switch Potentiomètre/Web pour un moteur donné
function modeVitesse(m, webMode){
  fetch("/"+m+"/speed/mode?val=" + (webMode ? "web" : "pot")); // Informe l'Arduino de la source choisie
  document.getElementById(m+"vitesseSlider").disabled = !webMode; // Le slider n'est utilisable qu'en mode "Web"
  if (syncMode) {
    // Si la synchro est active, on répercute le même changement sur l'autre moteur
    const o = autreMoteur(m);
    document.getElementById(o+"vitesseSwitch").checked = webMode;
    document.getElementById(o+"vitesseSlider").disabled = !webMode;
    fetch("/"+o+"/speed/mode?val=" + (webMode ? "web" : "pot"));
  }
}
// Variables pour bloquer l'accumulation de requêtes HTTP
// 🔧 Un flag "en cours d'envoi" INDÉPENDANT par moteur, pour que bouger
//    le slider M1 ne mette plus en attente les envois de M2 (et inversement)
//    quand on n'est pas en mode synchronisé.
let enCoursEnvoi = { m1: false, m2: false };   // true tant qu'une requête réseau est en vol pour ce moteur
let valeurEnAttente = { m1: null, m2: null };  // Dernière valeur de slider non encore envoyée (si occupé)

// Appelée à chaque déplacement du slider (oninput) et au relâchement (onchange)
function onSlide(m, val){
  // 1. Mise à jour visuelle INSTANTANÉE sur l'écran
  document.getElementById(m+"vitesseValeur").textContent = val;   // Affiche tout de suite la nouvelle valeur
  if (syncMode) {
    // En mode synchro, on répercute visuellement la même valeur sur l'autre slider
    const o = autreMoteur(m);
    document.getElementById(o+"vitesseSlider").value = val;
    document.getElementById(o+"vitesseValeur").textContent = val;
  }

  // 2. Gestion de l'envoi HTTP intelligent (anti-latence)
  if (enCoursEnvoi[m]) {
    // Si ce moteur est déjà occupé à traiter une requête, on mémorise juste la dernière valeur
    valeurEnAttente[m] = val;
    if (syncMode) valeurEnAttente[autreMoteur(m)] = val;
    return; // On quitte sans envoyer pour ne pas créer de bouchon
  }

  // Si le réseau est libre, on envoie la valeur immédiatement
  envoyerVitesseReseau(m, val);
}

// Envoie réellement la nouvelle vitesse à l'Arduino via HTTP
function envoyerVitesseReseau(m, val) {
  enCoursEnvoi[m] = true;   // Marque ce moteur comme "occupé" le temps de la requête

  if (syncMode) {
    // 🔧 Envoi PARALLÈLE des deux requêtes (au lieu de chaînées) pour ne pas
    //    doubler la latence perçue sur le slider en mode synchronisé.
    const o = autreMoteur(m);
    enCoursEnvoi[o] = true;                 // L'autre moteur est aussi occupé pendant l'envoi
    Promise.all([
      fetch("/"+m+"/speed/set?val=" + val), // Envoie la vitesse au moteur courant
      fetch("/"+o+"/speed/set?val=" + val)  // Envoie la même vitesse à l'autre moteur
    ]).then(() => {
      enCoursEnvoi[o] = false;              // Libère le flag de l'autre moteur une fois répondu
      verifierAttente(m);                   // Vérifie si une nouvelle valeur est arrivée entre-temps
    });
    return;
  }

  // Cas simple (pas de synchro) : une seule requête pour le moteur concerné
  fetch("/"+m+"/speed/set?val=" + val).then(() => verifierAttente(m));
}

// Appelée après chaque requête réseau terminée, pour envoyer la valeur la plus récente si besoin
function verifierAttente(m) {
  enCoursEnvoi[m] = false;   // Ce moteur n'est plus occupé
  // Une fois la réponse reçue, on regarde si l'utilisateur a bougé le curseur entre-temps
  if (valeurEnAttente[m] !== null) {
    let derniereValeur = valeurEnAttente[m];
    valeurEnAttente[m] = null; // On vide l'attente
    // 🔧 En mode synchro, la valeur de l'autre moteur est forcément identique :
    //    on la vide aussi pour éviter une requête fantôme superflue plus tard.
    if (syncMode) valeurEnAttente[autreMoteur(m)] = null;
    envoyerVitesseReseau(m, derniereValeur); // On envoie directement la toute dernière position
  } else if (syncMode && valeurEnAttente[autreMoteur(m)] !== null) {
    // Cas particulier : seule la file de l'autre moteur contient encore une valeur en attente
    let o = autreMoteur(m);
    let derniereValeur = valeurEnAttente[o];
    valeurEnAttente[o] = null;
    envoyerVitesseReseau(m, derniereValeur);
  }
}


// true uniquement lors du tout premier appel à refresh(), pour initialiser les sliders sans les "sauter"
let premierRefresh = true;
// Interroge périodiquement l'Arduino pour resynchroniser l'affichage (boutons, LEDs, etc.)
function refresh(){
  fetch("/status").then(r=>r.json()).then(s=>{
    document.getElementById("m1onoff").classList.toggle("on", s.autoM1);   // Colore le bouton si M1 tourne
    document.getElementById("m2onoff").classList.toggle("on", s.autoM2);   // Colore le bouton si M2 tourne
    document.getElementById("m1inv").classList.toggle("inv", s.invM1);     // Colore si M1 est inversé
    document.getElementById("m2inv").classList.toggle("inv", s.invM2);     // Colore si M2 est inversé
    if (premierRefresh) {
      // On ne met à jour les sliders qu'une seule fois au chargement de la page,
      // pour ne pas "arracher" le curseur des mains de l'utilisateur ensuite.
      document.getElementById("m1vitesseSwitch").checked = s.vitesseWebM1;
      document.getElementById("m1vitesseSlider").disabled = !s.vitesseWebM1;
      document.getElementById("m1vitesseSlider").value = s.vitesseValM1;
      document.getElementById("m1vitesseValeur").textContent = s.vitesseValM1;

      document.getElementById("m2vitesseSwitch").checked = s.vitesseWebM2;
      document.getElementById("m2vitesseSlider").disabled = !s.vitesseWebM2;
      document.getElementById("m2vitesseSlider").value = s.vitesseValM2;
      document.getElementById("m2vitesseValeur").textContent = s.vitesseValM2;
      premierRefresh = false;   // Les prochains refresh() n'y toucheront plus
    }
  });
}
setInterval(refresh, 700);  // Rafraîchit l'état affiché toutes les 700 ms
refresh();                  // Premier appel immédiat au chargement de la page
</script>
</body>
</html>
)HTML";


// =====================
// SERVEUR WEB - traitement d'un client
// =====================
// Traite UNE requête HTTP entrante, si un client (navigateur) est connecté.
// Appelée à chaque tour de loop() : reste très rapide grâce au timeout court.
void traiterClientWeb() {
  WiFiClient client = server.available();   // Vérifie si un client attend d'être servi
  if (!client) return;                      // Aucun client : on ne fait rien ce tour-ci

  // Le timer matériel pilote maintenant les moteurs indépendamment de loop(),
  // donc un blocage ici (même de 100ms) n'affecte plus la régularité des pas.
  // On privilégie la fiabilité de réception des commandes (1 Pas, etc.)
  // plutôt qu'un timeout ultra-court qui pouvait faire perdre des requêtes.
client.setTimeout(5); // 👈 Réduit le temps d'attente maximum à 5ms pour libérer le serveur plus vite
  String requestLine = client.readStringUntil('\r');   // Lit la 1ère ligne de la requête HTTP (ex: "GET /path HTTP/1.1")

  // Vide le reste de la requête (en-têtes HTTP) sans attente supplémentaire
  while (client.available()) client.read();

  // ---- Extraction du chemin (ex: "/m1/toggle") depuis la ligne de requête ----
  String path = "";
  int i1 = requestLine.indexOf(' ');              // Position du 1er espace (après "GET")
  int i2 = requestLine.indexOf(' ', i1 + 1);       // Position du 2e espace (avant "HTTP/1.1")
  if (i1 >= 0 && i2 > i1) path = requestLine.substring(i1 + 1, i2);  // Le chemin est entre les deux

  // Séparation chemin / paramètre (ex : /speed/set?val=120)
  String query = "";
  int iq = path.indexOf('?');            // Cherche un éventuel "?" (début des paramètres)
  if (iq >= 0) {
    query = path.substring(iq + 1);      // Tout ce qui suit le "?" est la chaîne de paramètres
    path = path.substring(0, iq);        // Le chemin ne garde que la partie avant le "?"
  }
  String valParam = "";
  int iv = query.indexOf("val=");        // Cherche le paramètre "val="
  if (iv >= 0) valParam = query.substring(iv + 4);  // Récupère tout ce qui suit "val="

  // ---- Actions ----
  // Chaque bloc ci-dessous correspond à une route appelée par le JavaScript de la page web
  if (path == "/m1/toggle") {
    autoM1 = !autoM1;                                  // Bascule Marche/Arrêt du moteur 1
    digitalWrite(Moteur1_LED_ONOFF, autoM1);            // Met à jour la LED correspondante
  } else if (path == "/m2/toggle") {
    autoM2 = !autoM2;                                  // Bascule Marche/Arrêt du moteur 2
    digitalWrite(Moteur2_LED_ONOFF, autoM2);
  } else if (path == "/m1/inv") {
    invM1 = !invM1;                                    // Bascule le sens de rotation du moteur 1
    digitalWrite(Moteur1_LED_INV, invM1);
  } else if (path == "/m2/inv") {
    invM2 = !invM2;                                    // Bascule le sens de rotation du moteur 2
    digitalWrite(Moteur2_LED_INV, invM2);
  } else if (path == "/m1/step/on") {
    webStepM1 = true;                                  // Démarre le pas à pas web du moteur 1
    autoM1 = false;                                    // Coupe le mode auto pendant le pas à pas
    digitalWrite(Moteur1_LED_ONOFF, LOW);
  } else if (path == "/m1/step/off") {
    webStepM1 = false;                                 // Arrête le pas à pas web du moteur 1
  } else if (path == "/m2/step/on") {
    webStepM2 = true;                                  // Démarre le pas à pas web du moteur 2
    autoM2 = false;
    digitalWrite(Moteur2_LED_ONOFF, LOW);
  } else if (path == "/m2/step/off") {
    webStepM2 = false;                                 // Arrête le pas à pas web du moteur 2
  } else if (path == "/m1/speed/mode") {
    vitesseSourceWebM1 = (valParam == "web");          // Choisit la source de vitesse du moteur 1 (pot/web)
  } else if (path == "/m1/speed/set") {
    float v = valParam.toFloat();                      // Convertit le paramètre texte en nombre
    if (v < VITESSE_MIN) v = VITESSE_MIN;               // Sécurité : borne basse
    if (v > VITESSE_MAX) v = VITESSE_MAX;               // Sécurité : borne haute
    vitesseWebM1 = v;                                   // Mémorise la nouvelle vitesse web du moteur 1
  } else if (path == "/m2/speed/mode") {
    vitesseSourceWebM2 = (valParam == "web");          // Choisit la source de vitesse du moteur 2 (pot/web)
  } else if (path == "/m2/speed/set") {
    float v = valParam.toFloat();
    if (v < VITESSE_MIN) v = VITESSE_MIN;
    if (v > VITESSE_MAX) v = VITESSE_MAX;
    vitesseWebM2 = v;                                   // Mémorise la nouvelle vitesse web du moteur 2
  }

  // ---- Réponses ----
  if (path == "/status") {
    // Construit un petit objet JSON avec tous les états utiles à la page web
    String json = "{";
    json += "\"autoM1\":" + String(autoM1 ? 1 : 0) + ",";
    json += "\"autoM2\":" + String(autoM2 ? 1 : 0) + ",";
    json += "\"invM1\":" + String(invM1 ? 1 : 0) + ",";
    json += "\"invM2\":" + String(invM2 ? 1 : 0) + ",";
    json += "\"vitesseWebM1\":" + String(vitesseSourceWebM1 ? 1 : 0) + ",";
    json += "\"vitesseValM1\":" + String((int)vitesseWebM1) + ",";
    json += "\"vitesseWebM2\":" + String(vitesseSourceWebM2 ? 1 : 0) + ",";
    json += "\"vitesseValM2\":" + String((int)vitesseWebM2);
    json += "}";
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");  // En-têtes HTTP JSON
    client.print(json);                                                                              // Corps de la réponse
  } else if (path == "/") {
    // Page d'accueil : on renvoie la page HTML complète stockée en PROGMEM
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
    client.print(PAGE_HTML);
  } else {
    // Toute autre route (ou route déjà traitée ci-dessus) reçoit une simple réponse "OK"
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
    client.print("OK");
  }

  client.stop();   // Ferme la connexion avec ce client (requêtes en "Connection: close")
}

// =====================
// SETUP
// =====================
void setup() {
  // ---- Configuration des broches LED en sortie ----
  pinMode(Moteur1_LED_STEP, OUTPUT);
  pinMode(Moteur1_LED_INV, OUTPUT);
  pinMode(Moteur1_LED_ONOFF, OUTPUT);

  pinMode(Moteur2_LED_STEP, OUTPUT);
  pinMode(Moteur2_LED_INV, OUTPUT);
  pinMode(Moteur2_LED_ONOFF, OUTPUT);

  // ---- Vitesse maximale absolue autorisée par AccelStepper (sécurité logicielle) ----
  Mt1.setMaxSpeed(2000);
  Mt2.setMaxSpeed(2000);

  // ---- Toutes les LEDs éteintes au démarrage ----
  digitalWrite(Moteur1_LED_STEP, LOW);
  digitalWrite(Moteur1_LED_INV, LOW);
  digitalWrite(Moteur1_LED_ONOFF, LOW);

  digitalWrite(Moteur2_LED_STEP, LOW);
  digitalWrite(Moteur2_LED_INV, LOW);
  digitalWrite(Moteur2_LED_ONOFF, LOW);

  // ---- Coupe l'alimentation des bobines des moteurs (économie d'énergie / évite l'échauffement à l'arrêt) ----
  Mt1.disableOutputs();
  Mt2.disableOutputs();

  // Démarrage du timer matériel qui pilote les moteurs en continu,
  // indépendamment du WiFi et du reste de loop(). 4000 Hz couvre
  // largement les vitesses utilisées ici (jusqu'à 2000 pas/s max).
  if (!demarrerTimerMoteurs(4000.0f)) {
    Serial.println("Erreur : impossible de démarrer le timer moteur");
  }

  Serial.begin(115200);   // Démarre la liaison série (pour le débogage via le moniteur série)
  delay(1500);             // Laisse le temps au port série de s'initialiser correctement

  Wire.begin();            // Initialise le bus I2C (broches SDA/SCL)

/*  if (!mcp.begin_I2C(AdrMCP)) {
// 🟢 On teste la présence du MCP sans bloquer la carte
  if (!mcp.begin_I2C(AdrMCP)) {
    Serial.println("Erreur : MCP23008 non détecté. Fonctionnement en mode Web uniquement.");
    mcpDisponible = false;
  } else {
    Serial.println("MCP23008 détecté avec succès ! Clavier physique actif.");
    mcpDisponible = true;
*/
  // ↑ Ancienne version conservée en commentaire (bloquait en cas d'échec) ; version active ci-dessous :
  Wire.begin();   // (Second appel, sans effet néfaste - Wire.begin() est déjà initialisé)

  if (mcp.begin_I2C(AdrMCP)) {
    // Le module MCP23008 répond : le clavier physique est présent
    Serial.println("MCP23008 detecte au démarrage.");
    mcpDisponible = true;
    for (int i = 0; i < 6; i++) {
      mcp.pinMode(i, INPUT_PULLUP);   // Configure chaque entrée bouton avec résistance de tirage interne
    }
  } else {
    // Pas de réponse du module : on continue quand même, en mode Web uniquement
    Serial.println("MCP23008 absent au démarrage. Mode Web actif.");
    mcpDisponible = false;
  }

  // ---- Connexion WiFi ----
  Serial.print("Connexion au WiFi ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);        // Lance la connexion au réseau WiFi configuré plus haut
  int essais = 0;
  while (WiFi.status() != WL_CONNECTED && essais < 40) {   // Attend la connexion, max 40 x 500ms = 20s
    delay(500);
    Serial.print(".");
    essais++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connecté, attente de l'adresse IP (DHCP)");

    // WL_CONNECTED peut être vrai avant que le bail DHCP soit obtenu :
    // on attend que l'IP ne soit plus 0.0.0.0
    IPAddress ip = WiFi.localIP();
    int essaisIP = 0;
    while (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0 && essaisIP < 20) {  // Max 20 x 500ms = 10s
      delay(500);
      Serial.print(".");
      ip = WiFi.localIP();
      essaisIP++;
    }

    Serial.println();
    Serial.print("Adresse IP : ");
    Serial.println(ip);              // Affiche l'adresse IP obtenue, à saisir dans le navigateur
    Serial.println("Ouvrez cette adresse dans un navigateur sur le même réseau.");
    server.begin();                  // Démarre effectivement le serveur web (écoute sur le port 80)
  } else {
    Serial.println();
    Serial.println("Echec de connexion WiFi - vérifier SSID / mot de passe.");
  }
}

// =====================
// LOOP
// =====================
void loop() {
  const float vitesseManuelle = 5;   // Vitesse fixe et lente utilisée pendant le mode "1 Pas" manuel

  // ---- Lecture et filtrage du potentiomètre de vitesse ----
  int pot = analogRead(Vitesse_POT);                                      // Lecture brute 0-1023
  float potMappe = (float)map(pot, 0, 1023, VITESSE_MIN, VITESSE_MAX);    // Conversion en pas/s

  // Filtre anti-bruit potentiomètre
  static float dernierPotFiltre = VITESSE_DEFAUT;   // Dernière valeur "stable" retenue (persiste entre appels)
  float ecartPot = potMappe - dernierPotFiltre;      // Écart entre la nouvelle lecture et la valeur retenue
  if (ecartPot < 0) ecartPot = -ecartPot;            // Valeur absolue de l'écart
  if (ecartPot >= 2.0) dernierPotFiltre = potMappe;  // On ne met à jour que si l'écart est significatif (anti-bruit)

  // Vitesse indépendante par moteur : selon la source choisie (potentiomètre ou slider web)
  float vitesseAutoM1 = vitesseSourceWebM1 ? vitesseWebM1 : dernierPotFiltre;
  float vitesseAutoM2 = vitesseSourceWebM2 ? vitesseWebM2 : dernierPotFiltre;

  // ---- Lecture des boutons physiques (avec re-détection automatique du MCP) ----
  int bouton = 0;
  
  if (mcpDisponible) {
    bouton = lireBouton();   // Lecture normale si le clavier physique est disponible
  } else {
    // Le clavier n'est pas disponible : on retente une détection toutes les 2 secondes,
    // sans jamais bloquer loop() plus longtemps que nécessaire.
    static unsigned long dernierEssaiMCP = 0;
    if (millis() - dernierEssaiMCP > 2000) {
      dernierEssaiMCP = millis();
      Wire.beginTransmission(AdrMCP);          // Test rapide de présence sur le bus I2C
      if (Wire.endTransmission() == 0) {       // Le module répond de nouveau
        if (mcp.begin_I2C(AdrMCP)) {           // On réinitialise pleinement le module
          for (int i = 0; i < 6; i++) {
            mcp.pinMode(i, INPUT_PULLUP);      // Reconfigure les entrées boutons
          }
          mcpDisponible = true;
          Serial.println("-> Clavier physique branché et activé !");
        }
      }
    }
  }

  // FRONT MONTANT (boutons physiques)
  // On ne traite l'action qu'au moment précis où le bouton passe de "relâché" à "appuyé",
  // pour éviter de répéter l'action tant que le bouton reste maintenu.
  if (bouton != 0 && ancienBouton == 0) {
    switch (bouton) {
      case 1: autoM1 = false; digitalWrite(Moteur1_LED_ONOFF, LOW); break;         // BP1 : coupe le mode auto M1 (pas à pas prend le relais)
      case 2: autoM2 = false; digitalWrite(Moteur2_LED_ONOFF, LOW); break;         // BP2 : coupe le mode auto M2
      case 3: autoM1 = !autoM1; digitalWrite(Moteur1_LED_ONOFF, autoM1); break;    // BP3 : bascule Marche/Arrêt M1
      case 4: autoM2 = !autoM2; digitalWrite(Moteur2_LED_ONOFF, autoM2); break;    // BP4 : bascule Marche/Arrêt M2
      case 5: invM1 = !invM1; digitalWrite(Moteur1_LED_INV, invM1); break;         // BP5 : bascule inversion M1
      case 6: invM2 = !invM2; digitalWrite(Moteur2_LED_INV, invM2); break;         // BP6 : bascule inversion M2
    }
  }

  // Le pas à pas est actif soit via le bouton physique 1/2, soit via le bouton web maintenu
  bool step1 = (bouton == 1) || webStepM1;
  bool step2 = (bouton == 2) || webStepM2;

  // =====================
  // MOTEUR 1 (Optimisé)
  // =====================
  static bool precActifM1 = false;        // Mémorise si M1 était actif au tour de boucle précédent
  static float derniereVitesseM1 = 0.0;   // 🟢 Création de la variable pour détecter les vrais changements
  bool actifM1 = step1 || autoM1;         // M1 est actif s'il est en pas à pas OU en mode auto

  if (actifM1 && !precActifM1) {
    Mt1.enableOutputs();                  // Vient de démarrer : ré-alimente les bobines du moteur
  }
  if (!actifM1 && precActifM1) {
    // Vient de s'arrêter : coupe tout proprement
    activeM1 = false;                     // Le timer matériel arrête d'appeler runSpeed() pour M1
    Mt1.disableOutputs();                 // Coupe l'alimentation des bobines
    derniereVitesseM1 = 0.0;              // Réinitialise pour forcer une mise à jour au prochain démarrage
  }
  precActifM1 = actifM1;                  // Mémorise l'état actuel pour le tour suivant

  if (step1) {
    // ---- Mode "1 Pas" (manuel, vitesse fixe et lente) ----
    digitalWrite(Moteur1_LED_STEP, HIGH);
    float vManM1 = invM1 ? -vitesseManuelle : vitesseManuelle;   // Applique le sens (inversé ou non)
    if (vManM1 != derniereVitesseM1) {
      // On ne touche à AccelStepper que si la vitesse a réellement changé (évite les accès inutiles)
      noInterrupts();            // Coupe brièvement les interruptions pour un accès sûr à Mt1
      Mt1.setSpeed(vManM1);
      interrupts();              // Réautorise les interruptions
      derniereVitesseM1 = vManM1;
    }
    activeM1 = true;             // Autorise le timer matériel à faire avancer M1
  } else if (autoM1) {
    // ---- Mode "Marche" (continu, vitesse = potentiomètre ou slider web) ----
    digitalWrite(Moteur1_LED_STEP, LOW);
    digitalWrite(Moteur1_LED_ONOFF, HIGH);
    
    float vAutoM1 = invM1 ? -vitesseAutoM1 : vitesseAutoM1;   // Applique le sens
    // 🟢 On n'applique la vitesse que si le slider ou le potard a bougé !
    if (vAutoM1 != derniereVitesseM1) { 
      noInterrupts();
      Mt1.setSpeed(vAutoM1);
      interrupts();
      derniereVitesseM1 = vAutoM1;
    }
    activeM1 = true;
  } else {
    // ---- Moteur 1 à l'arrêt : LEDs éteintes ----
    digitalWrite(Moteur1_LED_STEP, LOW);
    digitalWrite(Moteur1_LED_ONOFF, LOW);
  }

  // =====================
  // MOTEUR 2 (Optimisé)
  // =====================
  // Logique strictement identique au Moteur 1, appliquée au Moteur 2
  static bool precActifM2 = false;
  static float derniereVitesseM2 = 0.0; // 🟢 Création de la variable pour le moteur 2
  bool actifM2 = step2 || autoM2;

  if (actifM2 && !precActifM2) {
    Mt2.enableOutputs();
  }
  if (!actifM2 && precActifM2) {
    activeM2 = false;
    Mt2.disableOutputs();
    derniereVitesseM2 = 0.0;
  }
  precActifM2 = actifM2;

  if (step2) {
    digitalWrite(Moteur2_LED_STEP, HIGH);
    float vManM2 = invM2 ? -vitesseManuelle : vitesseManuelle;
    if (vManM2 != derniereVitesseM2) {
      noInterrupts();
      Mt2.setSpeed(vManM2);
      interrupts();
      derniereVitesseM2 = vManM2;
    }
    activeM2 = true;
  } else if (autoM2) {
    digitalWrite(Moteur2_LED_STEP, LOW);
    digitalWrite(Moteur2_LED_ONOFF, HIGH);
    
    float vAutoM2 = invM2 ? -vitesseAutoM2 : vitesseAutoM2;
    // 🟢 On n'applique la vitesse que si le slider ou le potard a bougé !
    if (vAutoM2 != derniereVitesseM2) { 
      noInterrupts();
      Mt2.setSpeed(vAutoM2);
      interrupts();
      derniereVitesseM2 = vAutoM2;
    }
    activeM2 = true;
  } else {
    digitalWrite(Moteur2_LED_STEP, LOW);
    // Remarque : contrairement au Moteur 1, la LED Moteur2_LED_ONOFF n'est pas explicitement remise à LOW ici
  }

  ancienBouton = bouton;      // Mémorise le bouton lu ce tour-ci, pour la détection du front montant au tour suivant
  traiterClientWeb();         // Traite une éventuelle requête HTTP en attente (page, actions, /status)
}
