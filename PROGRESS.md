# Journal de progression — HomeAssistant7

Dernière mise à jour : 2026-11-17

## Ce qui fonctionne (testé sur la carte)
- **Écran RGB 800×480** à **12 MHz**, soit environ 29 Hz, stable et sans scintillement.
- **Tactile GT911**, rétroéclairage par le CH422G.
- **Wi-Fi** : scan, clavier tactile, identifiants mémorisés, reconnexion au démarrage.
- **Démarrage direct** : écran « Connecté » pendant 5 s, puis tableau de bord.
- **Page web de configuration** `http://<ip>/` :
  - protégée par un mot de passe à 6 chiffres, nouveau à chaque démarrage et affiché sur l'écran « Connecté » ;
  - designer WYSIWYG, connexion Home Assistant, réglage de la luminosité.
- **Tableau de bord LVGL** relié à Home Assistant par l'API REST (lecture des états, appels d'actions).

## Fonctions livrées mais non vérifiées sur la carte
Elles ont été testées dans le navigateur ou seulement compilées.
- **Designer** : copier, couper, coller ; sélection multiple ; rotation par 90°.
  - Testé dans Edge en mode headless.
- **Actions HA selon l'entité**, ajustement automatique de min/max, paramètre et facteur des curseurs.
  - Testé avec un faux Home Assistant, pas avec le vrai.
- **Luminosité** : voile logiciel, et 0 % = rétroéclairage éteint. Entités `screen.brightness` et `screen.backlight`.
- **Bouton d'action « Ouvrir l'écran de configuration »**.
- **Clé API** : accès à `/api/brightness` et `/api/battery` pour Home Assistant.
- **Caméras HA** (2026-11-18) : élément « Caméra » avec rafraîchissement, cadrage et plein écran au toucher. Aperçu réel dans le designer.
  - Designer testé dans Edge headless. Firmware compilé et téléversé, **pas encore vérifié avec une vraie caméra**.
  - À surveiller : l'ordre des couleurs (rouge et bleu inversés ?), la vitesse en images par seconde et un éventuel scintillement dû à la PSRAM.
- **Mesure de batterie optionnelle** sur GPIO6 : entités `screen.battery` et `screen.battery_voltage`.
  - Demande un pont diviseur à câbler, et est désactivée par défaut.

## Essais matériels et décisions
| Sujet | Résultat |
|---|---|
| Horloge pixel (PCLK) | 8 MHz : écran blanc. **12 MHz : OK**. 16 MHz : léger scintillement. 21 MHz : scintillement marqué. |
| Chaîne d'affichage | Un seul framebuffer en PSRAM, LVGL en mode PARTIAL dans un tampon en SRAM interne, tampons bounce de 10 lignes. Le double framebuffer en mode DIRECT saturait la PSRAM et faisait scintiller l'image. |
| Accès flash périodiques | À éviter. La flash et la PSRAM partagent le même bus, donc chaque accès FFat ou NVS fait sauter l'image. L'état est gardé en RAM et les écritures NVS sont différées. |
| CH422G EXIO5 (USB_SEL) | Laissé à **0 (mode USB)** comme ESPHome. La démo Waveshare le met à 1, ce qui correspond au mode CAN. |
| Rétroéclairage | Marche/arrêt seulement (EXIO2), sans PWM. Luminosité réglée par un voile LVGL. |
| Batterie | La charge par le CS8501 est entièrement matérielle. D'après le schéma, **seul le port « UART » alimente le chargeur** : le 5 V du port « USB » va directement sur le rail 5 V. **Reste à vérifier** que le voyant CHG s'allume quand le panneau est alimenté par le port UART. La carte consomme environ 450 mA et la charge en demande 580 : une source USB de 500 mA (port de PC) ne suffit pas pour charger. Aucune broche de l'ESP32 ni du CH422G n'est reliée au CS8501. |
| Mesure de batterie | Aucune entrée de l'ESP32 n'est câblée d'origine. Il faut un pont diviseur sur le connecteur Sensor AD (GPIO6). |

## Programmation de la carte
- Téléverser par le **port « USB » (COM14)** : le port « UART » (CH343, COM15) ne répond pas.
- Après un téléversement, **appuyer sur RESET** : le reset automatique ne relance pas la carte.
- Pour reflasher sans recompiler, utiliser le dossier `firmware/` (voir `firmware/FLASH.txt`).
- Après une modification de `web/*.html`, lancer `tools\embed_web.ps1`.

## Points ouverts / prochaines étapes
- Vérifier la charge en alimentant le panneau par le port UART.
- Remplacer la lecture périodique de HA (toutes les 2 s) par l'API WebSocket de HA, plus réactive et moins chargée.
- Ajouter la clé API dans les exemples `rest_command` du README.
- Polices avec accents : générer une police Montserrat Latin-1 pour LVGL.
