// Page HTML du dashboard, servie sur "/". Les données viennent de "/json".
#pragma once

#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 GPS Dashboard</title>
  <style>
    :root{
      --bg:#0b1020;
      --bg2:#121a2b;
      --card:#151f34;
      --card2:#1a2742;
      --line:#263555;
      --text:#edf3ff;
      --muted:#9eb0d1;
      --accent:#66e3ff;
      --good:#59f2a5;
      --warn:#ffcc66;
      --bad:#ff6b81;
      --shadow:0 10px 30px rgba(0,0,0,.28);
      --radius:18px;
    }

    *{box-sizing:border-box}
    html,body{margin:0;padding:0}
    body{
      font-family:Arial,Helvetica,sans-serif;
      background:
        radial-gradient(circle at top right, rgba(102,227,255,.12), transparent 30%),
        radial-gradient(circle at top left, rgba(89,242,165,.10), transparent 22%),
        linear-gradient(180deg, #0b1020 0%, #0d1424 100%);
      color:var(--text);
      min-height:100vh;
    }

    .wrap{
      max-width:1100px;
      margin:0 auto;
      padding:20px;
    }

    .hero{
      display:flex;
      align-items:flex-start;
      justify-content:space-between;
      gap:16px;
      margin-bottom:18px;
      padding:20px;
      border:1px solid rgba(255,255,255,.06);
      border-radius:24px;
      background:linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
      backdrop-filter:blur(8px);
      box-shadow:var(--shadow);
    }

    .title{
      margin:0;
      font-size:28px;
      line-height:1.1;
      color:#fff;
      letter-spacing:.3px;
    }

    .subtitle{
      margin:8px 0 0;
      color:var(--muted);
      font-size:14px;
    }

    .top-badges{
      display:flex;
      flex-wrap:wrap;
      gap:10px;
      justify-content:flex-end;
    }

    .badge{
      display:inline-flex;
      align-items:center;
      gap:8px;
      padding:10px 14px;
      border-radius:999px;
      background:rgba(255,255,255,.05);
      border:1px solid rgba(255,255,255,.08);
      color:#fff;
      font-size:13px;
      white-space:nowrap;
    }

    .dot{
      width:10px;
      height:10px;
      border-radius:50%;
      background:var(--muted);
      box-shadow:0 0 12px currentColor;
    }

    .ok{color:var(--good)}
    .warn{color:var(--warn)}
    .bad{color:var(--bad)}

    .grid{
      display:grid;
      grid-template-columns:repeat(12,1fr);
      gap:16px;
    }

    .card{
      grid-column:span 12;
      background:linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
      border:1px solid rgba(255,255,255,.06);
      border-radius:var(--radius);
      padding:18px;
      box-shadow:var(--shadow);
    }

    .card h2{
      margin:0 0 14px;
      font-size:15px;
      color:var(--muted);
      font-weight:600;
      letter-spacing:.4px;
      text-transform:uppercase;
    }

    .metric{
      display:flex;
      align-items:flex-end;
      gap:10px;
    }

    .metric .value{
      font-size:34px;
      line-height:1;
      font-weight:800;
      color:#fff;
      letter-spacing:.3px;
    }

    .metric .unit{
      font-size:14px;
      color:var(--muted);
      padding-bottom:4px;
    }

    .mini{
      margin-top:10px;
      color:var(--muted);
      font-size:13px;
    }

    .kv{
      display:grid;
      grid-template-columns:1fr;
      gap:10px;
    }

    .row{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:12px;
      padding:10px 0;
      border-bottom:1px solid rgba(255,255,255,.06);
    }

    .row:last-child{border-bottom:none}
    .k{color:var(--muted);font-size:14px}
    .v{color:#fff;font-weight:700;font-size:15px;text-align:right}

    .coords{
      display:grid;
      grid-template-columns:1fr;
      gap:12px;
    }

    .coordBox{
      padding:14px;
      border-radius:14px;
      background:rgba(255,255,255,.04);
      border:1px solid rgba(255,255,255,.05);
    }

    .coordLabel{
      color:var(--muted);
      font-size:12px;
      text-transform:uppercase;
      letter-spacing:.5px;
      margin-bottom:6px;
    }

    .coordValue{
      color:#fff;
      font-size:24px;
      font-weight:800;
      word-break:break-all;
    }

    .footer{
      margin-top:16px;
      text-align:center;
      color:var(--muted);
      font-size:13px;
    }

    .footer a{
      color:var(--accent);
      text-decoration:none;
    }

    @media (min-width:700px){
      .span-3{grid-column:span 3}
      .span-4{grid-column:span 4}
      .span-5{grid-column:span 5}
      .span-6{grid-column:span 6}
      .span-7{grid-column:span 7}
      .coords{grid-template-columns:1fr 1fr}
      .kv{grid-template-columns:1fr 1fr}
    }

    @media (max-width:699px){
      .hero{flex-direction:column}
      .top-badges{justify-content:flex-start}
      .title{font-size:24px}
      .metric .value{font-size:28px}
      .coordValue{font-size:20px}
    }
  </style>
</head>
<body>
  <div class="wrap">
    <section class="hero">
      <div>
        <h1 class="title">ESP32 GPS Dashboard</h1>
        <p class="subtitle">Position, fix, heure GPS et état Wi‑Fi en direct</p>
      </div>

      <div class="top-badges">
        <div class="badge"><span id="wifiDot" class="dot"></span><span id="wifiText">WiFi</span></div>
        <div class="badge"><span id="gpsDot" class="dot"></span><span id="gpsText">GPS</span></div>
        <div class="badge">Fix: <strong id="fixText">--</strong></div>
      </div>
    </section>

    <section class="grid">
      <article class="card span-3">
        <h2>Satellites</h2>
        <div class="metric">
          <div class="value" id="sats">--</div>
          <div class="unit">sat</div>
        </div>
        <div class="mini">Qualité de réception GNSS</div>
      </article>

      <article class="card span-4">
        <h2>Vitesse</h2>
        <div class="metric">
          <div class="value" id="speed">--</div>
          <div class="unit">km/h</div>
        </div>
        <div class="mini">Vitesse sol issue du GPS</div>
      </article>

      <article class="card span-5">
        <h2>Altitude</h2>
        <div class="metric">
          <div class="value" id="altitude">--</div>
          <div class="unit">m</div>
        </div>
        <div class="mini">Altitude GPS</div>
      </article>

      <article class="card span-7">
        <h2>Coordonnées</h2>
        <div class="coords">
          <div class="coordBox">
            <div class="coordLabel">Latitude</div>
            <div class="coordValue" id="lat">--</div>
          </div>
          <div class="coordBox">
            <div class="coordLabel">Longitude</div>
            <div class="coordValue" id="lon">--</div>
          </div>
        </div>
      </article>

      <article class="card span-5">
        <h2>Temps GPS</h2>
        <div class="kv">
          <div class="row"><div class="k">UTC</div><div class="v" id="utc">--:--:--</div></div>
          <div class="row"><div class="k">France</div><div class="v" id="france">--:--:--</div></div>
          <div class="row"><div class="k">Port GPS</div><div class="v" id="port">--</div></div>
          <div class="row"><div class="k">Chars processed</div><div class="v" id="charsProcessed">--</div></div>
        </div>
      </article>

      <article class="card span-6">
        <h2>État GPS</h2>
        <div class="kv">
          <div class="row"><div class="k">Status</div><div class="v" id="status">--</div></div>
          <div class="row"><div class="k">Erreur GPS</div><div class="v" id="gpsError">--</div></div>
          <div class="row"><div class="k">Location valid</div><div class="v" id="locationValid">--</div></div>
          <div class="row"><div class="k">Altitude valid</div><div class="v" id="altitudeValid">--</div></div>
        </div>
      </article>

      <article class="card span-6">
        <h2>Réseau</h2>
        <div class="kv">
          <div class="row"><div class="k">WiFi</div><div class="v" id="wifiConnected">--</div></div>
          <div class="row"><div class="k">SSID</div><div class="v" id="ssid">--</div></div>
          <div class="row"><div class="k">IP</div><div class="v" id="ip">--</div></div>
          <div class="row"><div class="k">Fix courant</div><div class="v" id="fix2">--</div></div>
        </div>
      </article>
    </section>

    <div class="footer">
      Données JSON disponibles sur <a href="/json">/json</a>
    </div>
  </div>

  <script>
    function setText(id, value){
      const el = document.getElementById(id);
      if(el) el.textContent = value;
    }

    function setDot(id, state){
      const el = document.getElementById(id);
      if(!el) return;
      el.classList.remove('ok','warn','bad');
      el.classList.add(state);
    }

    function onlyNumberPrefix(text){
      if(typeof text !== 'string') return text;
      const m = text.match(/-?[0-9]+(\.[0-9]+)?/);
      return m ? m[0] : text;
    }

    async function refreshData(){
      try{
        const res = await fetch('/json', {cache:'no-store'});
        const data = await res.json();

        const wifiOk = !!(data.wifi && data.wifi.connected);
        const gpsErr = !!(data.gps && data.gps.error);
        const fix = data.gps?.fix ?? '--';
        const status = data.gps?.status ?? '--';

        setText('wifiText', wifiOk ? 'WiFi connecté' : 'WiFi coupé');
        setText('gpsText', gpsErr ? 'GPS erreur' : 'GPS actif');
        setText('fixText', fix);

        setDot('wifiDot', wifiOk ? 'ok' : 'bad');
        if (gpsErr) setDot('gpsDot', 'bad');
        else if (fix === 'FIX 2D') setDot('gpsDot', 'warn');
        else if (fix === 'FIX 3D') setDot('gpsDot', 'ok');
        else setDot('gpsDot', 'bad');

        setText('sats', String(data.gps?.satellites ?? '--'));
        setText('speed', onlyNumberPrefix(data.gps?.speed ?? '--'));
        setText('altitude', onlyNumberPrefix(data.gps?.altitude ?? '--'));

        setText('lat', data.gps?.latitude ?? '--');
        setText('lon', data.gps?.longitude ?? '--');

        setText('utc', data.gps?.utc ?? '--:--:--');
        setText('france', data.gps?.france ?? '--:--:--');
        setText('port', data.port ?? '--');
        setText('charsProcessed', String(data.gps?.charsProcessed ?? '--'));

        setText('status', status);
        setText('gpsError', gpsErr ? 'YES' : 'NO');
        setText('locationValid', data.gps?.locationValid ? 'YES' : 'NO');
        setText('altitudeValid', data.gps?.altitudeValid ? 'YES' : 'NO');

        setText('wifiConnected', wifiOk ? 'YES' : 'NO');
        setText('ssid', data.wifi?.ssid ?? '--');
        setText('ip', data.wifi?.ip ?? '--');
        setText('fix2', fix);
      } catch (e) {
        setText('wifiText', 'Erreur HTTP');
        setText('gpsText', 'Données indisponibles');
        setDot('wifiDot', 'bad');
        setDot('gpsDot', 'bad');
      }
    }

    refreshData();
    setInterval(refreshData, 1000);
  </script>
</body>
</html>
)rawliteral";
