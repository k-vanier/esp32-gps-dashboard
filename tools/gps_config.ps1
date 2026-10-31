<#
.SYNOPSIS
  Configure un u-blox NEO-M8N (protocole 18) pour le firmware ESP32 GPS Dashboard.

.DESCRIPTION
  - GNSS : GPS + Galileo + GLONASS (BeiDou, IMES, QZSS coupés)
  - SBAS : EGNOS uniquement (PRN 123, 126, 136)
  - NAV5 : modèle Portable, référence UTC Galileo
  - NMEA : RMC + GGA + GSA + GSV + ZDA sur UART1 et USB, GLL et VTG coupés
  - TP5  : PPS 1 Hz, 100 ms, front montant, aligné UTC, actif seulement avec fix
  - UART1 : 38400 bauds 8N1 (= GPS_BAUD du firmware)
  - Sauvegarde en BBR + flash, puis relecture

  Par l'USB du module, le débit n'a pas d'importance.
  Par un adaptateur série sur l'UART1, ajouter -ViaUart1 et -Baud 9600 (module neuf)
  ou -Baud 38400 (module déjà configuré).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\gps_config.ps1 -Port COM20
#>
param(
  [Parameter(Mandatory = $true)][string]$Port,
  [int]$Baud = 9600,
  [switch]$ViaUart1
)

$ErrorActionPreference = "Stop"

function Hex([string]$s) { [byte[]]($s -split ' ' | ? { $_ } | % { [Convert]::ToByte($_, 16) }) }

function Ubx([byte]$cls, [byte]$id, [byte[]]$pl) {
  $len = $pl.Length
  $body = [byte[]](@($cls, $id, ($len -band 0xFF), ($len -shr 8)) + $pl)
  $a = 0; $b = 0
  foreach ($x in $body) { $a = ($a + $x) -band 0xFF; $b = ($b + $a) -band 0xFF }
  [byte[]](@(0xB5, 0x62) + $body + @($a, $b))
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud
$sp.Open(); Start-Sleep -Milliseconds 300

# Envoie un message UBX et attend une trame (classe, id) en retour
function Xfer($cls, $id, $pl, $wantCls, $wantId) {
  $m = Ubx $cls $id $pl
  $sp.DiscardInBuffer(); $sp.Write($m, 0, $m.Length)
  $buf = New-Object System.Collections.Generic.List[byte]
  $end = (Get-Date).AddSeconds(2)
  while ((Get-Date) -lt $end) {
    while ($sp.BytesToRead -gt 0) { $buf.Add([byte]$sp.ReadByte()) }
    for ($i = 0; $i -le $buf.Count - 8; $i++) {
      if ($buf[$i] -eq 0xB5 -and $buf[$i+1] -eq 0x62 -and $buf[$i+2] -eq $wantCls -and $buf[$i+3] -eq $wantId) {
        $n = $buf[$i+4] + 256 * $buf[$i+5]
        if ($buf.Count -ge $i + 8 + $n) { return ,$buf.GetRange($i + 6, $n).ToArray() }
      }
    }
    Start-Sleep -Milliseconds 50
  }
  $null
}

$script:failed = 0
function Send($name, $cls, $id, $pl) {
  $r = Xfer $cls $id $pl 0x05 0x01
  $ok = $r -and $r[0] -eq $cls -and $r[1] -eq $id
  if (-not $ok) { $script:failed++ }
  "{0,-30} {1}" -f $name, $(if ($ok) { "ACK" } else { "NAK/TIMEOUT" })
}

# --- GNSS : blocs gnssId, resTrkCh, maxTrkCh, res, flags (enable + L1) ---
Send "CFG-GNSS" 0x06 0x3E (Hex @"
00 00 FF 07
00 08 10 00 01 00 01 01
01 01 03 00 01 00 01 01
02 04 08 00 01 00 01 01
03 08 10 00 00 00 01 01
04 00 08 00 00 00 01 01
05 00 03 00 00 00 01 01
06 08 0E 00 01 00 01 01
"@.Replace("`r", " ").Replace("`n", " "))

# --- SBAS : actif, range+corr+intégrité, 3 max, masque PRN 123/126/136 ---
Send "CFG-SBAS (EGNOS)" 0x06 0x16 (Hex "01 07 03 00 48 00 01 00")

# --- NAV5 : masque dyn + utc, Portable (0), utcStandard Galileo (5) ---
$nav5 = New-Object byte[] 36
$nav5[0] = 0x01; $nav5[1] = 0x04; $nav5[2] = 0; $nav5[30] = 5
Send "CFG-NAV5 (Portable)" 0x06 0x24 $nav5

# --- NMEA : débit par port I2C, UART1, UART2, USB, SPI, res ---
foreach ($m in @(@("GGA", 0x00, 1), @("GLL", 0x01, 0), @("GSA", 0x02, 1), @("GSV", 0x03, 1),
                 @("RMC", 0x04, 1), @("VTG", 0x05, 0), @("ZDA", 0x08, 1))) {
  $r = [byte]$m[2]
  Send ("CFG-MSG " + $m[0] + "=" + $r) 0x06 0x01 ([byte[]]@(0xF0, $m[1], 0, $r, 0, $r, 0, 0))
}

# --- TP5 : période 1 s, 0 % sans fix, 100 ms avec fix, flags 0x77 ---
Send "CFG-TP5 (PPS)" 0x06 0x31 (Hex "00 01 00 00 00 00 00 00 40 42 0F 00 40 42 0F 00 00 00 00 00 A0 86 01 00 00 00 00 00 77 00 00 00")

# --- UART1 : 38400 8N1, entrée UBX+NMEA+RTCM, sortie UBX+NMEA ---
$prt = Hex "01 00 00 00 D0 08 00 00 00 96 00 00 07 00 03 00 00 00 00 00"
if ($ViaUart1 -and $Baud -ne 38400) {
  # Le module change de débit avant de répondre : pas d'ACK lisible, on rouvre à 38400.
  $m = Ubx 0x06 0x00 $prt; $sp.Write($m, 0, $m.Length); Start-Sleep -Milliseconds 300
  $sp.Close(); $sp.BaudRate = 38400; $sp.Open(); Start-Sleep -Milliseconds 300
  "{0,-30} {1}" -f "CFG-PRT UART1 38400", "envoyé (reprise à 38400)"
} else {
  Send "CFG-PRT UART1 38400" 0x06 0x00 $prt
}

# --- Sauvegarde BBR + flash ---
Send "CFG-CFG (save)" 0x06 0x09 (Hex "00 00 00 00 1F 06 00 00 00 00 00 00 17")

# --- Relecture ---
""
$p = Xfer 0x06 0x00 ([byte[]]@(1)) 0x06 0x00
if ($p) { "UART1     : {0} bauds" -f [BitConverter]::ToUInt32($p, 8) }
$n = Xfer 0x06 0x24 ([byte[]]@()) 0x06 0x24
if ($n) { "dynModel  : {0} (0 = Portable)" -f $n[2] }
foreach ($m in @(@("GGA", 0), @("RMC", 4), @("ZDA", 8), @("GSV", 3))) {
  $r = Xfer 0x06 0x01 ([byte[]]@(0xF0, $m[1])) 0x06 0x01
  if ($r) { "NMEA {0}  : UART1={1} USB={2}" -f $m[0], $r[3], $r[5] }
}

$sp.Close()
if ($script:failed) { Write-Warning "$($script:failed) commande(s) sans ACK"; exit 1 }
