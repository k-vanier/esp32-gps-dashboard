# ESP32 GPS Dashboard

Tableau de bord GPS sur ESP32-C6 avec un u-blox NEO-M8N :

- **TFT ST7789 240×240** : latitude, longitude, altitude, vitesse, satellites, état du fix
- **OLED SSD1306 128×32** : heure UTC et heure de France en alternance (toutes les 5 s)
- **Serveur web** : page dashboard sur `/`, données brutes sur `/json`, configuration réseau sur `/config`

Le TFT alterne toutes les 10 s entre la page Position (coordonnées en grand, altitude, vitesse, satellites, état NTP) et une page « Réseau & GPS » (SSID, IP, passerelle, signal, dernières trames RMC/GGA, qualité du fix, HDOP, âge de la position).

## METAR (page 3)

La page 3 du TFT affiche le METAR de la station `METAR_STATION_VALUE` (LFPG par défaut), récupéré sur [avwx.rest](https://avwx.rest) toutes les 10 minutes, puis réessayé chaque minute en cas d'erreur. Les infos sont traduites en français :

- **Règle de vol :** VFR, MVFR, IFR ou LIFR, en pastille de couleur.
- **Vent :** direction et force, rose des vents, rafales et secteur variable.
- **Mesures :** température et point de rosée, humidité, QNH.
- **Ciel :** visibilité, nuages (Peu, Epars, Fragmente, Couvert), phénomènes (pluie, brouillard, orage...).
- **Tendance :** TEMPO ou BECMG traduits, par exemple `TEMPO 0500 FG VV///` donne « Temporaire: visi 500m, brouillard, ciel invisible ».

Le jeton API se met dans `include/secrets.h` (`AVWX_TOKEN_VALUE`). Le METAR décodé figure aussi dans `/json`. Le certificat HTTPS d'avwx.rest n'est pas vérifié (`setInsecure`).

## Serveur NTP

L'ESP32 répond aux requêtes NTP (UDP/123) avec l'heure GPS, en stratum 1.

- **Sans fix :** il répond « non synchronisé » (LI = 3, stratum 16), donc les clients ignorent son heure.
- **Sans PPS (par défaut) :** le début de chaque seconde est estimé à l'arrivée de la trame RMC, moins une latence fixe (`NMEA_LATENCY_US` dans `src/gps_clock.cpp`, 60 ms). La précision est de l'ordre de ±50 ms. On peut ajuster cette latence en comparant avec un serveur NTP de référence.
- **Avec PPS :** relier la broche TIMEPULSE du GPS à une GPIO libre et renseigner `GPS_PPS_PIN` dans `src/main.cpp`. Le front montant donne alors le début exact de la seconde (refid `PPS`).
- **Hors synchro :** si plus aucune trame RMC valide n'arrive pendant 30 s, le serveur repasse en « non synchronisé ».

Test depuis Windows :

```powershell
w32tm /stripchart /computer:192.168.0.32 /samples:5 /dataonly
```

L'état NTP (synchro, source, nombre de requêtes) figure dans `/json` et en bas de la page 1 du TFT.

## Configuration réseau

La page `/config` permet de changer le SSID, le mot de passe, le DHCP ou l'IP fixe, le masque, la passerelle et les DNS. Les réglages sont enregistrés en NVS et l'ESP32 redémarre. `include/secrets.h` ne fournit que les valeurs par défaut.

Si le WiFi est injoignable au démarrage, l'ESP32 ouvre le point d'accès **ESP32-GPS-Setup** (mot de passe `gpsconfig`). La config est alors sur http://192.168.4.1/config. Le point d'accès se ferme dès que le WiFi revient.

## Matériel et câblage

| Fonction | GPIO ESP32 |
|---|---|
| GPS TX → RX ESP (UART1) | 1 |
| GPS RX ← TX ESP (facultatif) | 2 |
| TFT SCLK / MOSI | 7 / 6 |
| TFT CS / DC / RST / BL | 14 / 15 / 21 / 22 |
| OLED SDA / SCL (I²C, adresse 0x3C) | 23 / 20 |

Le GPS est alimenté en 3,3 V, avec la masse en commun avec l'ESP32.

## Configuration du GPS

Le firmware attend sur l'UART1 du module :

| Réglage | Valeur |
|---|---|
| Débit | 38400 bauds, 8N1 |
| Trames NMEA | RMC + GGA + ZDA (GSV, GSA, VTG, GLL coupés) |
| Constellations | GPS + Galileo + GLONASS |
| SBAS | EGNOS uniquement |
| Modèle dynamique | Portable |
| PPS | 1 Hz, 100 ms, aligné UTC, seulement avec fix |

Pour appliquer cette config, brancher le module en USB sur le PC, puis lancer :

```powershell
powershell -ExecutionPolicy Bypass -File tools\gps_config.ps1 -Port COM20
```

Pour passer par un adaptateur USB-série branché sur l'UART1 du module, ajouter `-ViaUart1`, avec `-Baud 9600` pour un module neuf ou `-Baud 38400` pour un module déjà configuré.

Le script sauvegarde la config en flash, puis la relit sur le module.

## Compilation

Projet [PlatformIO](https://platformio.org/). Le support Arduino de l'ESP32-C6 passe par la plateforme pioarduino.

1. Copier `include/secrets.example.h` en `include/secrets.h` et y mettre le SSID et le mot de passe WiFi.
2. Vérifier l'IP fixe (`LOCAL_IP`, `GATEWAY`) dans `src/main.cpp`.
3. Compiler et flasher :

```bash
pio run -t upload
pio device monitor
```

## Heure

- L'heure n'est affichée qu'avec un fix GPS valide. Avant ça, le u-blox peut envoyer l'heure de son horloge interne, souvent fausse sans pile de sauvegarde.
- L'heure de France est calculée à partir de l'UTC : UTC+2 du dernier dimanche de mars au dernier dimanche d'octobre, à 01:00 UTC, et UTC+1 le reste de l'année.
