# ESP32 GPS Dashboard

Tableau de bord GPS sur ESP32-C6 avec un u-blox NEO-M8N :

- **TFT ST7789 240×240** : latitude, longitude, altitude, vitesse, satellites, état du fix
- **OLED SSD1306 128×32** : heure UTC et heure de France en alternance (toutes les 5 s)
- **Serveur web** : page dashboard sur `/` et données brutes sur `/json`

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
