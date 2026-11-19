# HomeAssistant7 — panneau Home Assistant sur Waveshare ESP32-S3-Touch-LCD-7

Firmware Arduino (sans ESPHome) pour un écran tactile 800×480 relié à Home Assistant, avec une page web de configuration servie par l'écran lui-même.

## Fonctionnement
1. **Démarrage** : aplats de couleurs, mire, arc-en-ciel animé (en double tampon), puis écran d'accueil avec la résolution, le taux de rafraîchissement mesuré et un test du tactile.
2. **Wi-Fi** : réseau mémorisé, sinon scan → clavier tactile → connexion.
3. **Écran « Connecté »** : affiche l'IP et l'adresse de configuration (`http://<ip>/` ou `http://ha-panel-7.local/`).
4. **Tableau de bord** : s'ouvre automatiquement si un layout existe, après 1, 5, 10 ou 15 s (réglage « Démarrage » de l'écran « Connecté », 5 s par défaut). Un **appui long de 3 s sur le fond** d'une page revient à l'écran « Connecté ».

## Page de configuration (navigateur)
Ouvrez `http://<ip de l'écran>/` :
- **Home Assistant** : adresse (ex. `http://192.168.1.10:8123`) et jeton d'accès longue durée (Profil → Sécurité dans HA). Boutons Tester / Enregistrer. Le jeton est stocké en NVS sur l'écran et n'est jamais renvoyé au navigateur.
- **Designer WYSIWYG 800×480** : panneaux, textes, valeurs, boutons bascule, boutons d'action (service HA ou changement de page), interrupteurs, curseurs, jauges et barres. Les entités sont proposées en autocomplétion depuis Home Assistant.
- **Enregistrer sur l'écran** : le layout est stocké dans `/layout.json` (FFat) et l'écran se met à jour immédiatement.

L'aperçu est fidèle à l'écran : même police (Montserrat Medium), mêmes tailles, mêmes hauteurs de ligne LVGL, mêmes géométries. Les accents s'affichent sur l'écran (polices Latin-1 générées, voir `tools/gen_fonts.ps1`) ; un caractère absent de la police s'affiche « ? », sur l'écran comme dans l'aperçu.

### API REST de l'écran
| Méthode | Chemin | Rôle |
|---|---|---|
| GET | `/api/info` | IP, résolution, tailles de police |
| GET/POST | `/api/layout` | Lire / enregistrer le tableau de bord |
| GET/POST | `/api/ha` | Lire / enregistrer `{url, token}` |
| POST | `/api/ha/test` | Tester `{url, token}` |
| GET | `/api/ha/entities` | Liste `[{id, name}]` des entités HA |

Toutes ces routes exigent une session ouverte avec le mot de passe affiché sur l'écran. **Il n'y a pas de clé API** : Home Assistant (ou tout autre système) ne peut pas piloter l'écran. C'est l'écran qui interroge Home Assistant, jamais l'inverse.

### Lien avec Home Assistant
- États : `GET /api/states/<entité>` toutes les ~1,5 s pour chaque entité utilisée.
- Actions : `POST /api/services/<domaine>/<service>` avec `entity_id`. Pour les curseurs, `brightness` (lumières) ou `value` (autres entités).
- Une pastille rouge ou orange apparaît en haut à droite si Home Assistant est injoignable ou non configuré.

## Caméras
Élément **Caméra** du designer : choisissez une entité `camera.…` de Home Assistant.
- L'écran télécharge en boucle l'image de `GET /api/camera_proxy/<entité>?width=L&height=H`. Home Assistant la réduit à peu près à la taille de l'élément. L'écran la décode en JPEG (esp_jpeg), puis la met à l'échelle.
- **Rafraîchissement** : délai entre deux images, 100 ms au minimum. Comptez environ 1 à 5 images par seconde selon la caméra, la taille et le Wi-Fi.
- **Cadrage** : image entière (bandes de la couleur de fond), remplir (bords coupés) ou étirer.
- **Plein écran au toucher** : affiche la caméra en 800 × 480. Un autre toucher la referme.
- Seules les caméras de la page affichée sont téléchargées, et aucune ne l'est quand le rétroéclairage est éteint.
- En cas de problème (caméra indisponible, jeton refusé), un message s'affiche par-dessus la dernière image.
- Dans le designer, l'aperçu montre une vraie image de la caméra, obtenue par `GET /api/ha/camera?id=…&w=…&h=…`.

## Luminosité de l'écran
Le rétroéclairage de cette carte (CH422G EXIO2) ne fait que marche/arrêt. Les valeurs fonctionnent donc ainsi :
- **0 %** : rétroéclairage éteint. Un toucher le rallume, et ce toucher n'est pas transmis à l'interface.
- **1 à 100 %** : atténuation logicielle par un voile noir semi-transparent.

Où la régler :
- **Écran « Connecté »** : curseur de 5 à 100 %.
- **Page web** : curseur dans la barre du haut.
- **Designer** : entités locales `screen.brightness` (curseur, jauge, barre, valeur) et `screen.backlight` (bouton bascule, interrupteur, bouton d'action).
- **API web** (session de la page de configuration seulement) : `GET/POST /api/brightness` avec `{"value": 0..100}` ou `{"on": true|false}`.

La valeur est conservée au redémarrage (écriture NVS différée de 3 s).

## Batterie
- **Charge** : la puce CS8501 de la carte la gère entièrement (580 mA par défaut, réglable avec R45), dès que la carte reçoit du 5 V par USB. Aucune broche de l'ESP32 ne la commande : le firmware ne peut ni l'activer ni la bloquer. Les voyants CHG (en charge) et DONE (terminée) indiquent son état.
- **Mesure** : aucune entrée de l'ESP32 ne reçoit la tension de la batterie (vérifié sur le schéma Waveshare). Pour l'afficher, il faut un pont diviseur sur le connecteur **Sensor AD (GPIO6)** : 100 kΩ entre BAT+ et AD, 100 kΩ entre AD et GND (rapport 2).
- **Activation** : bouton **Batterie** de la page web, où l'on règle aussi le rapport du pont. La mesure est désactivée par défaut, parce qu'une entrée non câblée donne des valeurs fausses.
- **Entités** : `screen.battery` (%) et `screen.battery_voltage` (V), en lecture seule.

## Accès à la configuration depuis le tableau de bord
- Un **bouton d'action** réglé sur « Ouvrir l'écran de configuration » mène à l'écran IP / Wi-Fi / luminosité.
- Un **appui long de 3 s** sur le fond d'une page fait la même chose.

## Modifier la page web
La source est [web/index.html](web/index.html). Après modification, régénérez `web_page.h` :
```
powershell -ExecutionPolicy Bypass -File tools\embed_web.ps1
```
Toute modification de rendu doit être faite à la fois dans `web/index.html` (aperçu) et dans `dashboard.cpp` (écran).

## Prérequis
- Core **esp32 by Espressif 3.3.x** (testé avec 3.3.11)
- Bibliothèques : **lvgl 9.2.2**, **ArduinoJson 7**
- Copier [extras/lv_conf.h](extras/lv_conf.h) dans `Documents/Arduino/libraries/lv_conf.h`

## Réglages de la carte
| Option | Valeur |
|---|---|
| Carte | ESP32S3 Dev Module |
| Flash Size | 16MB |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB CDC On Boot | **Enabled** (console sur le port USB natif) |

```
arduino-cli compile -u -p COM14 --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CPUFreq=240,CDCOnBoot=cdc" .
```
Téléversez par le port **USB** natif de la carte. Sur cette carte, le port UART (CH343) ne répondait pas.

## Fichiers
| Fichier | Rôle |
|---|---|
| `board_config.h` | Brochage et timings de l'écran (PCLK 12 MHz ≈ 29 Hz, voir les essais en commentaire) |
| `backlight.*` | Luminosité (rétroéclairage + voile logiciel) |
| `ch422g.*`, `gt911.*` | Extension d'E/S et tactile |
| `lcd_panel.*` | Panneau RGB, 2 framebuffers PSRAM, mesure du rafraîchissement |
| `boot_anim.*` | Mires de démarrage |
| `lvgl_port.*` | LVGL en mode direct sur les framebuffers |
| `app_ui.*` | Accueil, Wi-Fi, écran « Connecté », machine d'états |
| `ha_client.*` | Client API REST Home Assistant (tâche de fond) |
| `camera.*` | Images des caméras HA : téléchargement, décodage JPEG, double tampon PSRAM |
| `web_config.*` | Serveur web, API REST, stockage FFat |
| `dashboard.*` | Rendu LVGL du layout et liaison avec HA |
| `web/index.html` → `web_page.h` | Page de configuration |

## Remarques
- `LCD_PCLK_HZ` est réglé à 12 MHz après essais : 8 MHz donne un écran blanc, et 16 MHz ou plus fait scintiller l'image quand le Wi-Fi et le tableau de bord sollicitent la PSRAM.
- Les écritures en flash (enregistrement du layout ou du Wi-Fi) peuvent provoquer un bref saut d'image.
