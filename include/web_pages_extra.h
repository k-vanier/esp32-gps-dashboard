// Pages /metar et /sats, et la feuille de style commune /style.css.
#pragma once

#include <pgmspace.h>

static const char COMMON_CSS[] PROGMEM = R"rawliteral(
:root{--bg:#0b1020;--line:rgba(255,255,255,.07);--text:#edf3ff;--muted:#9eb0d1;--accent:#66e3ff;
  --good:#59f2a5;--warn:#ffcc66;--bad:#ff6b81;--violet:#c792ea;--blue:#7aa7ff;--radius:18px;
  --shadow:0 10px 30px rgba(0,0,0,.28)}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{font-family:Arial,Helvetica,sans-serif;color:var(--text);min-height:100vh;
  background:radial-gradient(circle at top right,rgba(102,227,255,.12),transparent 30%),
  radial-gradient(circle at top left,rgba(89,242,165,.08),transparent 22%),linear-gradient(180deg,#0b1020,#0d1424)}
a{color:var(--accent);text-decoration:none}
.wrap{max-width:1100px;margin:0 auto;padding:20px}
nav{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:16px}
nav a{padding:8px 14px;border-radius:999px;border:1px solid var(--line);background:rgba(255,255,255,.03);color:var(--muted);font-size:13px}
nav a.on{color:#06202a;background:var(--accent);border-color:var(--accent);font-weight:700}
.hero{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:14px;padding:20px;margin-bottom:16px;
  border:1px solid var(--line);border-radius:24px;background:linear-gradient(180deg,rgba(255,255,255,.05),rgba(255,255,255,.02));box-shadow:var(--shadow)}
.hero h1{margin:0;font-size:28px}
.sub{margin:6px 0 0;color:var(--muted);font-size:14px}
.pill{display:inline-flex;align-items:center;gap:8px;padding:9px 14px;border-radius:999px;font-weight:700;font-size:14px;
  background:rgba(255,255,255,.06);border:1px solid var(--line)}
.dot{width:10px;height:10px;border-radius:50%;background:currentColor;box-shadow:0 0 12px currentColor}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}
.card{grid-column:span 12;padding:18px;border:1px solid var(--line);border-radius:var(--radius);
  background:linear-gradient(180deg,rgba(255,255,255,.04),rgba(255,255,255,.02));box-shadow:var(--shadow)}
.card h2{margin:0 0 14px;font-size:13px;color:var(--muted);font-weight:600;letter-spacing:.5px;text-transform:uppercase}
.big{font-size:38px;font-weight:800;line-height:1}
.unit{font-size:15px;color:var(--muted);font-weight:400;margin-left:4px}
.muted{color:var(--muted)}
.row{display:flex;justify-content:space-between;gap:12px;padding:9px 0;border-bottom:1px solid var(--line);font-size:14px}
.row:last-child{border-bottom:0}
.row .v{font-weight:700;text-align:right}
.mono{font-family:Consolas,Menlo,monospace;font-size:14px;background:rgba(0,0,0,.25);border:1px solid var(--line);
  border-radius:12px;padding:12px 14px;word-break:break-word}
table{width:100%;border-collapse:collapse;font-size:14px}
th{text-align:left;color:var(--muted);font-weight:600;font-size:12px;text-transform:uppercase;padding:6px 8px;border-bottom:1px solid var(--line)}
td{padding:8px;border-bottom:1px solid var(--line)}
.banner{padding:12px 14px;border-radius:12px;margin-bottom:16px;font-size:14px;background:rgba(255,204,102,.1);
  border:1px solid rgba(255,204,102,.4);color:var(--warn)}
.ok{color:var(--good)}.warn{color:var(--warn)}.bad{color:var(--bad)}
@media (min-width:760px){.s3{grid-column:span 3}.s4{grid-column:span 4}.s5{grid-column:span 5}.s6{grid-column:span 6}
  .s7{grid-column:span 7}.s8{grid-column:span 8}}
@media (max-width:759px){.hero h1{font-size:23px}.big{font-size:32px}}
)rawliteral";

static const char METAR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Météo METAR</title>
  <link rel="stylesheet" href="/style.css">
  <style>
    .rose{width:150px;height:150px;flex:none}
    .windbox{display:flex;align-items:center;gap:20px;flex-wrap:wrap}
    .layers{position:relative;height:190px;border-left:1px solid var(--line);margin-left:44px}
    .layer{position:absolute;left:0;right:0;display:flex;align-items:center;gap:10px}
    .layer .bar{height:14px;border-radius:7px;background:linear-gradient(90deg,rgba(237,243,255,.85),rgba(237,243,255,.25))}
    .layer .lbl{font-size:13px;white-space:nowrap}
    .layer .alt{position:absolute;left:-46px;font-size:11px;color:var(--muted);width:40px;text-align:right}
    .ground{position:absolute;left:0;right:0;bottom:0;border-top:2px solid var(--line)}
    .tags{display:flex;flex-wrap:wrap;gap:8px}
    .tag{padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.06);border:1px solid var(--line);font-size:14px}
    .trend{font-size:16px;line-height:1.5;color:var(--warn)}
    .legend{display:flex;flex-wrap:wrap;gap:14px;font-size:13px;color:var(--muted)}
    .legend span{display:inline-flex;align-items:center;gap:6px}
  </style>
</head>
<body>
<div class="wrap">
  <nav><a href="/">Dashboard</a><a class="on" href="/metar">Météo</a><a href="/sats">Satellites &amp; NTP</a><a href="/config">Configuration</a></nav>

  <section class="hero">
    <div>
      <h1>METAR <span id="station">----</span></h1>
      <p class="sub" id="obs">Chargement…</p>
    </div>
    <div class="pill" id="rules"><span class="dot"></span><span id="rulesTxt">--</span></div>
  </section>
  <div class="banner" id="err" hidden></div>

  <section class="grid">
    <article class="card s7">
      <h2>Vent</h2>
      <div class="windbox">
        <svg class="rose" viewBox="-80 -80 160 160" aria-label="Rose des vents">
          <circle r="70" fill="rgba(255,255,255,.03)" stroke="rgba(255,255,255,.12)"/>
          <circle r="46" fill="none" stroke="rgba(255,255,255,.06)"/>
          <g id="ticks"></g>
          <text y="-55" text-anchor="middle" fill="#9eb0d1" font-size="12">N</text>
          <text x="58" y="4" text-anchor="middle" fill="#9eb0d1" font-size="12">E</text>
          <text y="63" text-anchor="middle" fill="#9eb0d1" font-size="12">S</text>
          <text x="-58" y="4" text-anchor="middle" fill="#9eb0d1" font-size="12">O</text>
          <path id="varArc" fill="none" stroke="rgba(102,227,255,.35)" stroke-width="8"/>
          <g id="arrow"><line y1="40" y2="-38" stroke="#66e3ff" stroke-width="4" stroke-linecap="round"/>
            <path d="M0,-50 L-10,-30 L10,-30 Z" fill="#66e3ff"/></g>
        </svg>
        <div>
          <div class="big" id="windSpd">--</div>
          <div class="sub" id="windDir">--</div>
          <div class="sub" id="windExtra"></div>
        </div>
      </div>
    </article>

    <article class="card s5">
      <h2>Température</h2>
      <div class="big" id="temp">--</div>
      <div class="row"><span class="muted">Point de rosée</span><span class="v" id="dew">--</span></div>
      <div class="row"><span class="muted">Humidité relative</span><span class="v" id="hum">--</span></div>
      <div class="row"><span class="muted">Écart T / rosée</span><span class="v" id="spread">--</span></div>
    </article>

    <article class="card s4">
      <h2>Pression (QNH)</h2>
      <div class="big" id="qnh">--</div>
      <p class="sub" id="qnhHint"></p>
    </article>

    <article class="card s4">
      <h2>Visibilité</h2>
      <div class="big" id="vis">--</div>
      <p class="sub" id="visHint"></p>
    </article>

    <article class="card s4">
      <h2>Phénomènes</h2>
      <div class="tags" id="wx"><span class="muted">Aucun</span></div>
    </article>

    <article class="card s6">
      <h2>Nuages</h2>
      <div class="layers" id="layers"><div class="ground"></div></div>
    </article>

    <article class="card s6">
      <h2>Tendance et remarques</h2>
      <div class="trend" id="trend">--</div>
      <h2 style="margin-top:22px">METAR brut</h2>
      <div class="mono" id="raw">--</div>
    </article>

    <article class="card">
      <h2>Règles de vol</h2>
      <div class="legend">
        <span><span class="dot" style="color:var(--good)"></span>VFR : plafond &gt; 3000 ft et visi &gt; 8 km</span>
        <span><span class="dot" style="color:var(--blue)"></span>MVFR : plafond 1000-3000 ft ou visi 5-8 km</span>
        <span><span class="dot" style="color:var(--bad)"></span>IFR : plafond 500-1000 ft ou visi 1,6-5 km</span>
        <span><span class="dot" style="color:var(--violet)"></span>LIFR : plafond &lt; 500 ft ou visi &lt; 1,6 km</span>
      </div>
    </article>
  </section>
</div>
<script>
const $ = id => document.getElementById(id);
const COVER = {FEW:['Peu nombreux',.25],SCT:['Épars',.45],BKN:['Fragmenté',.75],OVC:['Couvert',1],VV:['Ciel invisible',1]};
const RULES = {VFR:'var(--good)',MVFR:'var(--blue)',IFR:'var(--bad)',LIFR:'var(--violet)'};

(function ticks(){
  let s='';
  for(let a=0;a<360;a+=30){const r=a*Math.PI/180;
    s+=`<line x1="${64*Math.sin(r)}" y1="${-64*Math.cos(r)}" x2="${70*Math.sin(r)}" y2="${-70*Math.cos(r)}" stroke="rgba(255,255,255,.25)"/>`;}
  $('ticks').innerHTML=s;
})();

function arc(from,to,r){
  const p=a=>{const t=a*Math.PI/180;return [r*Math.sin(t),-r*Math.cos(t)];};
  const [x1,y1]=p(from),[x2,y2]=p(to);
  const span=((to-from)+360)%360;
  return `M${x1},${y1} A${r},${r} 0 ${span>180?1:0} 1 ${x2},${y2}`;
}

function render(m){
  $('station').textContent = m.station || '----';
  $('err').hidden = !m.error;
  $('err').textContent = m.error ? 'Dernière récupération en erreur : ' + m.error : '';
  if(!m.valid){ $('obs').textContent = 'En attente du premier METAR…'; return; }

  const t = m.time || '';
  $('obs').textContent = t.length>=6 ? `Observé le ${t.slice(0,2)} à ${t.slice(2,4)}h${t.slice(4,6)} UTC · mis à jour il y a ${m.updatedMin} min` : '';
  $('rulesTxt').textContent = m.flightRules || '--';
  $('rules').style.color = RULES[m.flightRules] || 'var(--muted)';

  const w = m.wind, u = w.unit || 'kt';
  if(w.speed === 0){ $('windSpd').innerHTML = 'Calme'; $('windDir').textContent = 'Vent nul'; }
  else {
    $('windSpd').innerHTML = `${w.speed ?? '--'}<span class="unit">${u}</span>`;
    $('windDir').textContent = w.dir == null ? 'Direction variable' : `Du ${w.dirText}° (${w.compass}) · ${Math.round((w.speed||0)*1.852)} km/h`;
  }
  const extra=[];
  if(w.gust) extra.push(`Rafales ${w.gust} ${u}`);
  if(w.varFrom!=null) extra.push(`Variable entre ${w.varFrom}° et ${w.varTo}°`);
  $('windExtra').textContent = extra.join(' · ');
  $('arrow').style.display = w.dir == null || w.speed === 0 ? 'none' : '';
  $('arrow').setAttribute('transform', `rotate(${(w.dir||0)+180})`);
  $('varArc').setAttribute('d', w.varFrom!=null ? arc(w.varFrom,w.varTo,70) : '');

  $('temp').innerHTML = m.temperature==null ? '--' : `${m.temperature}<span class="unit">°C</span>`;
  $('dew').textContent = m.dewpoint==null ? '--' : `${m.dewpoint} °C`;
  $('hum').textContent = m.humidity==null ? '--' : `${m.humidity} %`;
  const sp = m.temperature!=null && m.dewpoint!=null ? m.temperature-m.dewpoint : null;
  $('spread').textContent = sp==null ? '--' : `${sp} °C${sp<=2?' · risque de brouillard':''}`;
  $('spread').className = 'v' + (sp!=null && sp<=2 ? ' warn' : '');

  $('qnh').innerHTML = m.qnh==null ? '--' : `${m.qnh}<span class="unit">${m.qnhUnit}</span>`;
  $('qnhHint').textContent = m.qnh==null ? '' : (m.qnh>=1020 ? 'Hautes pressions' : m.qnh<=1005 ? 'Basses pressions' : 'Pression normale');

  if(m.visibilityRepr==='CAVOK'){ $('vis').textContent='CAVOK'; $('visHint').textContent='Visibilité 10 km ou plus, pas de nuage sous 5000 ft, pas de phénomène'; }
  else if(m.visibility==null){ $('vis').textContent='--'; $('visHint').textContent=''; }
  else if(m.visibility>=9999){ $('vis').innerHTML='10<span class="unit">km+</span>'; $('visHint').textContent='Visibilité de 10 km ou plus'; }
  else { $('vis').innerHTML = m.visibility>=5000 ? `${(m.visibility/1000).toFixed(0)}<span class="unit">km</span>` : `${m.visibility}<span class="unit">m</span>`; $('visHint').textContent=''; }

  $('wx').innerHTML = m.weather ? m.weather.split(', ').map(x=>`<span class="tag">${x}</span>`).join('') : '<span class="muted">Aucun</span>';

  const L = $('layers');
  L.querySelectorAll('.layer,.empty').forEach(e=>e.remove());
  if(!m.clouds.length){
    const d=document.createElement('div'); d.className='empty muted'; d.style.padding='70px 12px';
    d.textContent = m.visibilityRepr==='CAVOK' ? 'Pas de nuage significatif (CAVOK)' : 'Pas de nuage signalé';
    L.appendChild(d);
  }
  const maxAlt = Math.max(5000, ...m.clouds.map(c=>c.altitudeFt||0)) * 1.15;
  m.clouds.forEach(c=>{
    const [name,cov] = COVER[c.type] || [c.type,.5];
    const y = 170 - (Math.max(c.altitudeFt||0,0)/maxAlt)*160;
    const d=document.createElement('div'); d.className='layer'; d.style.top=y+'px';
    const mod = c.modifier==='CB' ? ' · cumulonimbus' : c.modifier==='TCU' ? ' · cumulus bourgeonnant' : '';
    d.innerHTML=`<span class="alt">${c.altitudeFt>=0?c.altitudeFt+' ft':'?'}</span><span class="bar" style="width:${cov*70}%"></span><span class="lbl">${name}${mod}</span>`;
    L.appendChild(d);
  });

  $('trend').textContent = m.trend || 'Rien à signaler';
  $('raw').textContent = m.raw || '--';
}

async function refresh(){
  try{ render(await (await fetch('/metar.json',{cache:'no-store'})).json()); }
  catch(e){ $('err').hidden=false; $('err').textContent='ESP32 injoignable'; }
}
refresh(); setInterval(refresh, 30000);
</script>
</body>
</html>
)rawliteral";

static const char SATS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Satellites &amp; NTP</title>
  <link rel="stylesheet" href="/style.css">
  <style>
    .sky{width:100%;max-width:360px;display:block;margin:0 auto}
    .legend{display:flex;flex-wrap:wrap;gap:12px;justify-content:center;margin-top:10px;font-size:13px;color:var(--muted)}
    .legend span{display:inline-flex;align-items:center;gap:6px}
    .snr{display:grid;grid-template-columns:62px 1fr 34px;align-items:center;gap:8px;font-size:13px;margin:5px 0}
    .snr .track{height:12px;border-radius:6px;background:rgba(255,255,255,.06);overflow:hidden}
    .snr .fill{height:100%;border-radius:6px}
    .snr .id{display:flex;align-items:center;gap:6px}
    .snr.unused{opacity:.45}
    .stats{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}
    .stat .lbl{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.4px;margin-bottom:4px}
    .stat .val{font-size:26px;font-weight:800}
    .cmd{display:flex;gap:8px;align-items:center}
    .cmd .mono{flex:1;font-size:13px}
    .cmd button{padding:10px 12px;border:0;border-radius:10px;background:var(--accent);color:#06202a;font-weight:700;cursor:pointer}
  </style>
</head>
<body>
<div class="wrap">
  <nav><a href="/">Dashboard</a><a href="/metar">Météo</a><a class="on" href="/sats">Satellites &amp; NTP</a><a href="/config">Configuration</a></nav>

  <section class="hero">
    <div>
      <h1>Satellites &amp; serveur NTP</h1>
      <p class="sub" id="clock">--</p>
    </div>
    <div class="pill" id="ntpPill"><span class="dot"></span><span id="ntpPillTxt">NTP</span></div>
  </section>
  <div class="banner" id="noGsv" hidden>
    Pas de trame GSV reçue : le détail des satellites n'est pas disponible. Relie le TX de l'ESP32 (GPIO2) au RX du GPS
    pour que l'ESP active GSV/GSA au démarrage, ou relance <b>tools/gps_config.ps1</b> sur le module.
  </div>

  <section class="grid">
    <article class="card s5">
      <h2>Ciel</h2>
      <svg class="sky" id="sky" viewBox="-110 -110 220 220" aria-label="Position des satellites"></svg>
      <div class="legend" id="legend"></div>
    </article>

    <article class="card s7">
      <h2>Signal (dB-Hz)</h2>
      <div id="snr"><span class="muted">--</span></div>
    </article>

    <article class="card s5">
      <h2>Réception</h2>
      <div class="stats">
        <div class="stat"><div class="lbl">En vue</div><div class="val" id="inView">--</div></div>
        <div class="stat"><div class="lbl">Utilisés</div><div class="val" id="used">--</div></div>
        <div class="stat"><div class="lbl">Fix</div><div class="val" id="fix">--</div></div>
        <div class="stat"><div class="lbl">HDOP</div><div class="val" id="hdop">--</div></div>
      </div>
      <div style="margin-top:14px" id="perSys"></div>
    </article>

    <article class="card s7">
      <h2>Serveur NTP</h2>
      <div class="row"><span class="muted">État</span><span class="v" id="ntpState">--</span></div>
      <div class="row"><span class="muted">Stratum</span><span class="v" id="stratum">--</span></div>
      <div class="row"><span class="muted">Référence</span><span class="v" id="source">--</span></div>
      <div class="row"><span class="muted">Dernière seconde GPS</span><span class="v" id="syncAge">--</span></div>
      <div class="row"><span class="muted">Requêtes servies</span><span class="v" id="requests">--</span></div>
      <div class="row"><span class="muted">Adresse</span><span class="v" id="addr">--</span></div>
      <h2 style="margin-top:18px">Tester depuis Windows</h2>
      <div class="cmd"><div class="mono" id="cmd">--</div><button id="copy">Copier</button></div>
    </article>

    <article class="card">
      <h2>Clients NTP récents</h2>
      <table><thead><tr><th>Adresse</th><th>Requêtes</th><th>Dernière</th></tr></thead>
        <tbody id="clients"><tr><td colspan="3" class="muted">Aucun client pour l'instant</td></tr></tbody></table>
    </article>
  </section>
</div>
<script>
const $ = id => document.getElementById(id);
const SYS = {P:['GPS','#66e3ff'],L:['GLONASS','#ff6b81'],A:['Galileo','#59f2a5'],B:['BeiDou','#ffcc66'],Q:['QZSS','#c792ea']};

function ago(s){ if(s==null) return '--'; if(s<60) return s+' s'; if(s<3600) return Math.floor(s/60)+' min'; return Math.floor(s/3600)+' h'; }

function drawSky(sats){
  let s='';
  [90,60,30].forEach((e,i)=>{ const r=(90-e)/90*100; if(r>0) s+=`<circle r="${r}" fill="none" stroke="rgba(255,255,255,${i?0.08:0.15})"/>`; });
  s+='<circle r="100" fill="rgba(255,255,255,.025)" stroke="rgba(255,255,255,.18)"/>';
  s+='<line x1="-100" x2="100" stroke="rgba(255,255,255,.06)"/><line y1="-100" y2="100" stroke="rgba(255,255,255,.06)"/>';
  [['N',0,-104],['E',106,4],['S',0,112],['O',-106,4]].forEach(([t,x,y])=>s+=`<text x="${x}" y="${y}" text-anchor="middle" font-size="10" fill="#9eb0d1">${t}</text>`);
  s+='<text x="3" y="-64" font-size="7" fill="rgba(158,176,209,.6)">30°</text><text x="3" y="-31" font-size="7" fill="rgba(158,176,209,.6)">60°</text>';
  sats.filter(x=>x.elev>=0&&x.az>=0).forEach(x=>{
    const r=(90-x.elev)/90*100, a=x.az*Math.PI/180, cx=r*Math.sin(a), cy=-r*Math.cos(a);
    const c=(SYS[x.sys]||['?','#fff'])[1], rad=x.snr>0?4+Math.min(x.snr,50)/12:3.5;
    s+=`<g><title>${(SYS[x.sys]||['?'])[0]} ${x.prn} · élév ${x.elev}° · az ${x.az}° · ${x.snr>0?x.snr+' dB-Hz':'pas de signal'}${x.used?' · utilisé':''}</title>`+
       `<circle cx="${cx}" cy="${cy}" r="${rad}" fill="${x.used?c:'none'}" stroke="${c}" stroke-width="1.5" opacity="${x.snr>0?1:.45}"/>`+
       `<text x="${cx}" y="${cy-rad-2}" text-anchor="middle" font-size="6.5" fill="#edf3ff">${x.prn}</text></g>`;
  });
  $('sky').innerHTML=s;
}

function render(d){
  const sats=d.sats||[];
  $('noGsv').hidden = d.gsv;
  $('clock').textContent = `UTC ${d.gps.utc} · France ${d.gps.france} · allumé depuis ${ago(d.uptimeS)}`;

  drawSky(sats);
  const present=[...new Set(sats.map(s=>s.sys))];
  $('legend').innerHTML = present.map(k=>`<span><span class="dot" style="color:${SYS[k][1]}"></span>${SYS[k][0]}</span>`).join('') +
    (present.length?'<span>● utilisé · ○ en vue</span>':'');

  const sorted=[...sats].sort((a,b)=>(b.used-a.used)||(b.snr-a.snr));
  $('snr').innerHTML = sorted.length ? sorted.map(x=>{const [n,c]=SYS[x.sys]||['?','#fff'];
    return `<div class="snr${x.used?'':' unused'}"><span class="id"><span class="dot" style="color:${c}"></span>${n.slice(0,3)} ${x.prn}</span>`+
      `<span class="track"><span class="fill" style="display:block;width:${Math.max(0,Math.min(x.snr,55))/55*100}%;background:${c}"></span></span>`+
      `<span>${x.snr>0?x.snr:'--'}</span></div>`;}).join('') : '<span class="muted">Aucun satellite en vue</span>';

  $('inView').textContent = d.gsv ? sats.length : '--';
  $('used').textContent = d.gps.used;
  $('fix').textContent = d.gps.error ? 'Erreur' : d.gps.fix;
  $('fix').className = 'val ' + (d.gps.fix==='FIX 3D'?'ok':d.gps.fix==='FIX 2D'?'warn':'bad');
  $('hdop').textContent = d.gps.hdop==null ? '--' : d.gps.hdop.toFixed(1);
  $('perSys').innerHTML = present.map(k=>{const l=sats.filter(s=>s.sys===k);
    return `<div class="row"><span><span class="dot" style="color:${SYS[k][1]};display:inline-block;margin-right:8px"></span>${SYS[k][0]}</span><span class="v">${l.filter(s=>s.used).length} / ${l.length}</span></div>`;}).join('');

  const n=d.ntp;
  $('ntpPillTxt').textContent = n.synced ? 'NTP synchronisé' : 'NTP en attente du GPS';
  $('ntpPill').style.color = n.synced ? 'var(--good)' : 'var(--warn)';
  $('ntpState').textContent = n.synced ? 'Synchronisé sur le GPS' : 'Non synchronisé (les clients ignorent l\'heure)';
  $('ntpState').className = 'v ' + (n.synced?'ok':'warn');
  $('stratum').textContent = n.stratum + (n.synced ? ' (référence primaire)' : ' (non synchronisé)');
  $('source').textContent = n.source==='PPS' ? 'PPS (impulsion à la µs)' : 'Trame NMEA RMC (≈ ±50 ms)';
  $('syncAge').textContent = n.syncAgeMs==null ? 'jamais' : `il y a ${(n.syncAgeMs/1000).toFixed(1)} s`;
  $('requests').textContent = n.requests;
  $('addr').textContent = `${n.address} · UDP 123`;
  $('cmd').textContent = `w32tm /stripchart /computer:${n.address} /samples:5 /dataonly`;
  $('clients').innerHTML = n.clients.length ? n.clients.sort((a,b)=>a.lastSeenS-b.lastSeenS)
    .map(c=>`<tr><td>${c.ip}</td><td>${c.requests}</td><td>il y a ${ago(c.lastSeenS)}</td></tr>`).join('')
    : '<tr><td colspan="3" class="muted">Aucun client pour l\'instant</td></tr>';
}

$('copy').onclick = () => navigator.clipboard && navigator.clipboard.writeText($('cmd').textContent);

async function refresh(){
  try{ render(await (await fetch('/sats.json',{cache:'no-store'})).json()); }
  catch(e){ $('clock').textContent='ESP32 injoignable'; }
}
refresh(); setInterval(refresh, 2000);
</script>
</body>
</html>
)rawliteral";
