#!/usr/bin/env python3
"""Mulhouse-Basel radar: simulated screen fed with live ADS-B data.

Run:  python3 radar.py [--port 8080] [--host 127.0.0.1] [--no-browser]
No dependencies. The APIs are called from here because not all of them
allow direct calls from a browser (CORS). The page itself stays in French.
"""
import argparse, json, re, threading, time, urllib.error, urllib.request, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LAT, LON, RADIUS_NM = 47.67, 7.47, 30
UA = "radar-mulhouse-bale/1.0"
PROVIDERS = [
    ("adsb.lol", f"https://api.adsb.lol/v2/point/{LAT}/{LON}/{RADIUS_NM}"),
    ("adsb.fi", f"https://opendata.adsb.fi/api/v2/lat/{LAT}/lon/{LON}/dist/{RADIUS_NM}"),
    ("airplanes.live", f"https://api.airplanes.live/v2/point/{LAT}/{LON}/{RADIUS_NM}"),
]
ROUTE_URL = "https://api.adsbdb.com/v0/callsign/"
CACHE_S, STALE_S, COOLDOWN_S, ROUTE_TTL_S, ROUTE_MISS_TTL_S = 4, 20, 30, 6 * 3600, 3600

lock = threading.Lock()
cache = {"t": 0.0, "body": None}
cooldown = {}
strikes = {}
routes = {}


def get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=6) as r:
        return json.load(r)


def retry_after(e):
    v = e.headers.get("Retry-After", "")
    return min(int(v), 3600) if v.isdigit() else 0


def aircraft():
    with lock:
        now = time.time()
        if cache["body"] is not None and now - cache["t"] < CACHE_S:
            return 200, cache["body"]
        errors = []
        for name, url in PROVIDERS:
            if cooldown.get(name, 0) > now:
                continue
            try:
                d = get_json(url)
                ac = d.get("ac", d.get("aircraft"))
                if not isinstance(ac, list):
                    raise ValueError("réponse inattendue")
                cooldown.pop(name, None)
                strikes.pop(name, None)
                cache.update(t=now, body={"source": name, "ac": ac})
                return 200, cache["body"]
            except urllib.error.HTTPError as e:
                if e.code == 429:
                    strikes[name] = strikes.get(name, 0) + 1
                    wait = retry_after(e) or min(600, 60 * 2 ** (strikes[name] - 1))
                    print(f"[radar] {name} is rate limiting: pausing {wait} s, switching to the next provider", flush=True)
                else:
                    wait = COOLDOWN_S
                    print(f"[radar] {name}: HTTP error {e.code}, pausing {wait} s", flush=True)
                cooldown[name] = now + wait
                errors.append(f"{name}: HTTP {e.code}")
            except Exception as e:
                cooldown[name] = now + COOLDOWN_S
                errors.append(f"{name}: {e}")
                print(f"[radar] {name}: {e}, pausing {COOLDOWN_S} s", flush=True)
        if cache["body"] is not None and now - cache["t"] < STALE_S:
            return 200, cache["body"]
        return 503, {"error": "; ".join(errors) or "all providers paused, retrying soon"}


def airport(a):
    return {"iata": a.get("iata_code") or a.get("icao_code"), "city": a.get("municipality") or a.get("name"),
            "lat": a.get("latitude"), "lon": a.get("longitude")}


def route(call):
    now = time.time()
    with lock:
        hit = routes.get(call)
        if hit and hit[0] > now:
            return hit[1]
    try:
        fr = get_json(ROUTE_URL + call)["response"]["flightroute"]
        out = {"route": {"from": airport(fr["origin"]), "to": airport(fr["destination"]),
                         "airline": (fr.get("airline") or {}).get("name"), "flight": fr.get("callsign_iata")}}
        ttl = ROUTE_TTL_S
    except urllib.error.HTTPError as e:
        if e.code != 404:
            return {"route": None, "retry": True}
        out, ttl = {"route": None}, ROUTE_MISS_TTL_S
    except Exception:
        return {"route": None, "retry": True}
    with lock:
        routes[call] = (now + ttl, out)
    return out


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            return self.send(200, PAGE.encode(), "text/html; charset=utf-8")
        if path == "/api/aircraft":
            return self.send(*aircraft())
        m = re.fullmatch(r"/api/route/([A-Z0-9]{2,8})", path)
        if m:
            return self.send(200, route(m.group(1)))
        self.send(404, {"error": "not found"})

    def log_message(self, *a):
        pass


PAGE = r'''<!doctype html><html lang="fr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"></head><body><title>Radar Mulhouse–Bâle</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Montserrat:wght@500;600;700&family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@500&display=swap">
<style>
/* Concept: the device on a workbench. The printed case and its cable in the middle, a quiet page around it. */
:root{
  --bg:#e9ebe6;--fg:#20282b;--muted:#596569;--line:#c6cbc4;--btn:#f6f7f3;--btn-on:#20282b;--btn-on-fg:#f6f7f3;--ring:#1b6fa8;
  --case:#32393c;--case-hi:#454e52;--cable:#23282a;
  --font-ui:"IBM Plex Sans",system-ui,sans-serif;--font-data:"IBM Plex Mono",ui-monospace,monospace;--font-lv:"Montserrat",system-ui,sans-serif;
  /* screen: always dark, like the device */
  --s-bg:#0f1c22;--s-panel:#14252c;--s-raise:#1d333c;--s-line:#2c4650;--s-fg:#e6eff1;--s-dim:#93aab2;--s-accent:#ffc857;--s-ok:#6fdc9c;--s-err:#ff8a7a;
}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#161b1d;--fg:#e4e9e8;--muted:#9aa7a9;--line:#323b3e;--btn:#222a2d;--btn-on:#e4e9e8;--btn-on-fg:#161b1d;--ring:#6cc0ee;--case:#3a4246;--case-hi:#4e585d;--cable:#0b0e0f;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#161b1d;--fg:#e4e9e8;--muted:#9aa7a9;--line:#323b3e;--btn:#222a2d;--btn-on:#e4e9e8;--btn-on-fg:#161b1d;--ring:#6cc0ee;--case:#3a4246;--case-hi:#4e585d;--cable:#0b0e0f;color-scheme:dark}
*{box-sizing:border-box}
[hidden]{display:none!important}
body{background:var(--bg);color:var(--fg);font:15px/1.5 var(--font-ui);margin:0}
.wrap{max-width:1100px;margin:0 auto;padding-block:20px 40px;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px));display:flex;flex-direction:column;align-items:center;gap:20px}
.head{width:100%;display:flex;flex-wrap:wrap;gap:4px 16px;align-items:baseline;justify-content:center;text-align:center}
.head h1{font:600 20px/1.25 var(--font-ui);margin:0;text-wrap:balance}
.head p{margin:0;color:var(--muted)}
#stage{width:100%;display:flex;flex-direction:column;align-items:center;min-width:0}
.device{padding:14px;border-radius:14px;background:repeating-linear-gradient(0deg,rgba(255,255,255,.035) 0 1px,transparent 1px 3px),linear-gradient(160deg,var(--case-hi),var(--case) 45%);box-shadow:0 18px 34px -16px rgba(0,0,0,.55),inset 0 0 0 1px rgba(255,255,255,.06);max-width:100%}
.glass{overflow:hidden;border-radius:3px;box-shadow:0 0 0 2px #06090a;background:var(--s-bg)}
.cable{width:9px;height:34px;background:var(--cable);border-radius:0 0 3px 3px;position:relative}
.cable::before{content:"";position:absolute;left:-7px;top:0;width:23px;height:9px;border-radius:0 0 4px 4px;background:var(--cable)}
.controls{display:flex;flex-wrap:wrap;gap:12px 28px;justify-content:center}
.ctl{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.ctl>span{color:var(--muted);font-size:14px}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:7px;overflow:hidden}
.seg button{font:500 14px var(--font-ui);padding:7px 13px;min-height:36px;border:0;background:var(--btn);color:var(--fg);cursor:pointer}
.seg button+button{border-left:1px solid var(--line)}
.seg button[aria-pressed="true"]{background:var(--btn-on);color:var(--btn-on-fg)}
button:focus-visible{outline:2px solid var(--ring);outline-offset:2px}
.notes{width:100%;max-width:820px;display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:18px 28px;border-top:1px solid var(--line);padding-top:18px}
.notes h2{font:600 14px var(--font-ui);margin:0 0 4px}
.notes p{margin:0;font-size:14px;color:var(--muted)}
.credit{width:100%;max-width:820px;font-size:12.5px;color:var(--muted);margin:0}
.credit a{color:inherit}
@media (max-width:680px){.notes{grid-template-columns:minmax(0,1fr)}}

/* 800×480 SCREEN */
#screen{width:800px;height:480px;position:relative;transform-origin:0 0;overflow:hidden;background:var(--s-bg);color:var(--s-fg);font:500 14px/1.25 var(--font-lv);user-select:none;-webkit-user-select:none;font-variant-numeric:tabular-nums}
#screen button{font-family:var(--font-lv);color:inherit;background:none;border:0;padding:0;cursor:pointer}
#screen button:focus-visible{outline:2px solid var(--s-accent);outline-offset:-2px}
#map{position:absolute;inset:0;width:800px;height:480px;cursor:pointer;touch-action:manipulation}
.bar{position:absolute;left:0;right:0;top:0;height:40px;background:var(--s-panel);border-bottom:1px solid var(--s-line);display:flex;align-items:center;gap:4px;padding:0 4px 0 12px;z-index:3}
.bar .zone{font-weight:700;font-size:14px}
.sim{font-size:10px;font-weight:700;letter-spacing:.08em;color:#14252c;background:var(--s-accent);border-radius:3px;padding:2px 5px;margin-left:8px}
#age{font-size:11px;color:var(--s-dim);margin-left:8px;font-weight:600}
.sim.live{background:var(--s-ok)}.sim.down{background:var(--s-err)}
.bar .sp{flex:1}
#screen .barbtn{height:40px;min-width:44px;padding:0 10px;display:inline-flex;align-items:center;justify-content:center;gap:6px;font-weight:600;font-size:13px}
#screen .barbtn:active{background:var(--s-raise)}
#clock{font-weight:600;font-size:13px;padding:0 8px;color:var(--s-dim)}
svg.i{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;display:block}
#panel,#list{position:absolute;right:0;top:40px;bottom:0;width:276px;background:var(--s-panel);border-left:1px solid var(--s-line);z-index:2;display:flex;flex-direction:column}
.p-head{display:flex;align-items:flex-start;padding:10px 4px 8px 14px;gap:8px}
.p-head>div{flex:1;min-width:0}
#pCall{font-size:23px;font-weight:700;letter-spacing:.01em;line-height:1.1}
#pLine{font-size:12px;color:var(--s-dim);margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#screen .close{width:44px;height:44px;display:flex;align-items:center;justify-content:center;flex:none;margin-top:-6px}
.route{padding:0 14px 10px;border-bottom:1px solid var(--s-line)}
.route .ends{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.route b{font-size:18px;font-weight:700}
.route small{font-size:11px;color:var(--s-dim);display:flex;justify-content:space-between;gap:8px}
.track{height:4px;background:var(--s-raise);border-radius:2px;margin:6px 0;position:relative}
#pProg{position:absolute;left:0;top:0;height:4px;background:var(--s-fg);border-radius:2px;width:0}
.chip{font-size:11px;font-weight:700;padding:2px 6px;border-radius:3px;background:var(--s-raise)}
.data{display:grid;grid-template-columns:1fr 1fr;gap:9px 12px;padding:10px 14px;flex:none}
.data div>span{display:block;font-size:10.5px;color:var(--s-dim);font-weight:600;letter-spacing:.02em}
.data b{font-size:15px;font-weight:600;white-space:nowrap}
.data i{font-style:normal;font-size:11px;color:var(--s-dim);font-weight:500;display:block}
.profile{padding:0 14px 8px;flex:1;min-height:0;display:flex;flex-direction:column;border-top:1px solid var(--s-line);padding-top:7px}
.profile span{font-size:10.5px;color:var(--s-dim);font-weight:600}
#pSvg{flex:1;width:100%;min-height:0;margin-top:3px}
.l-head{display:flex;align-items:center;padding:0 4px 0 14px;height:44px;font-weight:700;border-bottom:1px solid var(--s-line);flex:none}
.l-head span{flex:1}
#rows{overflow-y:auto;flex:1;scrollbar-width:thin;scrollbar-color:var(--s-line) transparent}
#screen .row{display:grid;grid-template-columns:12px 1fr auto;gap:2px 8px;width:100%;text-align:left;align-items:center;padding:7px 12px;min-height:48px;border-bottom:1px solid var(--s-raise)}
#screen .row:active{background:var(--s-raise)}
.row em{width:10px;height:10px;border-radius:50%;grid-row:1/3}
.row b{font-size:13.5px;font-weight:700}
.row u{text-decoration:none;font-size:12.5px;font-weight:600;text-align:right}
.row small{font-size:11px;color:var(--s-dim);grid-column:2/4}
#offline{position:absolute;inset:40px 0 0;background:rgba(15,28,34,.72);display:flex;align-items:center;justify-content:center;z-index:2}
#offline div{background:var(--s-panel);border:1px solid var(--s-line);border-radius:6px;padding:18px 22px;text-align:center;display:grid;gap:10px;max-width:340px}
#offline b{font-size:16px}#offline p{margin:0;color:var(--s-dim);font-size:13px}
#toast{position:absolute;left:12px;top:50px;z-index:4;background:var(--s-raise);border:1px solid var(--s-line);border-radius:5px;padding:8px 12px;font-size:13px;font-weight:600}
#hint{position:absolute;left:50%;top:50px;transform:translateX(-50%);z-index:1;background:rgba(20,37,44,.92);border:1px solid var(--s-line);border-radius:5px;padding:6px 12px;font-size:12.5px;font-weight:600;pointer-events:none;transition:opacity .4s;white-space:nowrap}
.primary{background:var(--s-accent)!important;color:#14252c!important;border-radius:5px!important;font-weight:700!important;min-height:44px;padding:0 16px!important;font-size:14px}
.primary:disabled{opacity:.45;cursor:default}
.ghost{border:1px solid var(--s-line)!important;border-radius:5px!important;min-height:44px;padding:0 14px!important;font-weight:600;font-size:13px}
.full{position:absolute;inset:0;background:var(--s-bg);z-index:5;display:flex;flex-direction:column}
.topbar{height:52px;background:var(--s-panel);border-bottom:1px solid var(--s-line);display:flex;align-items:center;gap:8px;padding:0 8px 0 2px;flex:none}
.topbar h2{font:700 17px var(--font-lv);margin:0;flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#screen .back{width:48px;height:52px;display:flex;align-items:center;justify-content:center;flex:none}
.wifi-body{flex:1;min-height:0;display:grid;grid-template-columns:minmax(0,1fr) 256px}
#nets{overflow-y:auto;scrollbar-width:thin;scrollbar-color:var(--s-line) transparent;border-right:1px solid var(--s-line)}
#screen .net{display:flex;width:100%;align-items:center;gap:12px;min-height:56px;padding:6px 14px;text-align:left;border-bottom:1px solid var(--s-raise)}
#screen .net:active{background:var(--s-raise)}
.net .nm{flex:1;min-width:0;font-weight:600;font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.net small{display:block;font-weight:500;font-size:11.5px;color:var(--s-dim);margin-top:2px}
.net small.on{color:var(--s-ok)}
.bars{display:flex;align-items:flex-end;gap:2px;height:16px;flex:none}
.bars i{width:4px;background:var(--s-line);border-radius:1px;display:block}
.bars i.on{background:var(--s-fg)}
.scan{padding:18px 14px;color:var(--s-dim);display:flex;align-items:center;gap:10px}
.spin{width:18px;height:18px;border:2px solid var(--s-line);border-top-color:var(--s-accent);border-radius:50%;animation:sp 1s linear infinite;flex:none}
@keyframes sp{to{transform:rotate(360deg)}}
.side{padding:14px;display:flex;flex-direction:column;gap:10px;font-size:13px}
.side h3{font:700 11px var(--font-lv);letter-spacing:.06em;color:var(--s-dim);margin:0;text-transform:uppercase}
.side .big{font-size:17px;font-weight:700;overflow-wrap:anywhere}
.side dl{display:grid;grid-template-columns:auto 1fr;gap:5px 12px;margin:0}
.side dt{color:var(--s-dim)}.side dd{margin:0;text-align:right;font-weight:600}
.side p{margin:0;color:var(--s-dim);line-height:1.4}
.side .push{margin-top:auto}
.field{display:flex;align-items:center;margin:10px 12px 2px;border:1px solid var(--s-line);background:var(--s-panel);border-radius:5px;height:50px;padding-left:12px;flex:none}
.field.err{border-color:var(--s-err)}
#pwVal{flex:1;min-width:0;font-size:18px;font-weight:600;letter-spacing:.04em;white-space:nowrap;overflow:hidden;display:flex;justify-content:flex-start}
#pwVal.empty{color:var(--s-dim);font-size:14px;letter-spacing:0;font-weight:500}
#pwVal:not(.empty)::after{content:"";width:2px;height:22px;background:var(--s-accent);margin-left:2px;animation:bl 1s steps(2) infinite}
@keyframes bl{50%{opacity:0}}
#screen .eye{width:50px;height:48px;display:flex;align-items:center;justify-content:center;color:var(--s-dim);flex:none}
#pwMsg{font-size:12px;color:var(--s-dim);padding:3px 14px;height:22px;flex:none}
#pwMsg.err{color:var(--s-err)}
#kbd{margin-top:auto;padding:6px;background:var(--s-panel);border-top:1px solid var(--s-line);display:grid;gap:5px}
.kr{display:flex;gap:5px;justify-content:center}
#screen .k{flex:1 1 0;min-width:0;height:53px;background:var(--s-raise);border-radius:4px;font-size:18px;font-weight:600;display:flex;align-items:center;justify-content:center}
#screen .k:active{background:var(--s-line)}
#screen .k.w15{flex-grow:1.5;font-size:13px}
#screen .k.w5{flex-grow:5}
#screen .k.on{background:var(--s-fg);color:#14252c}
#busy{position:absolute;inset:52px 0 0;background:var(--s-bg);display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px;font-size:15px;font-weight:600}
#busy .spin{width:34px;height:34px;border-width:3px}
@media (prefers-reduced-motion:reduce){.spin,#pwVal::after{animation:none!important}}
</style>

<main class="wrap">
  <header class="head">
    <h1>Radar Mulhouse–Bâle</h1>
    <p id="subtitle">Simulation de l'écran tactile 800×480. Les avions sont fictifs.</p>
  </header>

  <div id="stage">
    <div class="device" id="device">
      <div class="glass" id="glass">
        <div id="screen">
          <canvas id="map" role="img" aria-label="Carte de Mulhouse à Bâle avec les avions en vol. Utilisez le bouton de liste pour y accéder au clavier."></canvas>
          <div class="bar">
            <span class="zone">Mulhouse – Bâle</span><span class="sim" id="chip">SIMULÉ</span><span id="age"></span>
            <span class="sp"></span>
            <button class="barbtn" id="btnList" type="button"><svg class="i" viewBox="0 0 24 24"><path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/></svg><span id="count">0 avion</span></button>
            <span id="clock"></span>
            <button class="barbtn" id="btnWifi" type="button" aria-label="Réglages Wi-Fi"><svg class="i" viewBox="0 0 24 24" id="wifiIcon"><path d="M2 8.8a15 15 0 0 1 20 0M5 12.6a10 10 0 0 1 14 0M8.5 16.2a5 5 0 0 1 7 0"/><path d="M12 20h.01"/></svg></button>
          </div>
          <div id="hint">Touchez un avion pour voir ses données</div>
          <div id="toast" hidden></div>

          <aside id="panel" hidden>
            <div class="p-head"><div><div id="pCall"></div><div id="pLine"></div></div>
              <button class="close" id="pClose" type="button" aria-label="Fermer la fiche"><svg class="i" viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg></button></div>
            <div class="route">
              <div class="ends"><b id="pFrom"></b><span class="chip" id="pPhase"></span><b id="pTo"></b></div>
              <div class="track"><div id="pProg"></div></div>
              <small><span id="pFromN"></span><span id="pToN"></span></small>
            </div>
            <div class="data">
              <div><span>Altitude</span><b id="dAlt"></b><i id="dAlt2"></i></div>
              <div><span>Vitesse sol</span><b id="dGs"></b><i id="dGs2"></i></div>
              <div><span>Cap</span><b id="dTrk"></b><i id="dTrk2"></i></div>
              <div><span>Taux vertical</span><b id="dVs"></b><i id="dVs2"></i></div>
              <div><span>Immatriculation</span><b id="dReg"></b><i id="dType"></i></div>
              <div><span>Squawk · Hex</span><b id="dSq"></b><i id="dHex"></i></div>
            </div>
            <div class="profile"><span id="pProfLabel">Profil d'altitude</span><svg id="pSvg" viewBox="0 0 248 40" preserveAspectRatio="none"><path id="pArea" fill="currentColor" opacity=".18"/><path id="pPath" fill="none" stroke="currentColor" stroke-width="1.6" vector-effect="non-scaling-stroke"/></svg></div>
          </aside>

          <aside id="list" hidden>
            <div class="l-head"><span id="lTitle">Avions dans la zone</span><button class="close" id="lClose" type="button" aria-label="Fermer la liste" style="margin:0"><svg class="i" viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg></button></div>
            <div id="rows"></div>
          </aside>

          <div id="offline" hidden><div><b>Pas de connexion Wi-Fi</b><p>Les avions reviendront dès que l'écran sera connecté.</p><button class="primary" id="offBtn" type="button">Choisir un réseau</button></div></div>

          <section class="full" id="settings" hidden>
            <div class="topbar"><button class="back" id="setBack" type="button" aria-label="Retour à la carte"><svg class="i" viewBox="0 0 24 24"><path d="M15 5l-7 7 7 7"/></svg></button><h2>Wi-Fi</h2><button class="ghost" id="scanBtn" type="button">Actualiser</button></div>
            <div class="wifi-body"><div id="nets"></div><div class="side" id="side"></div></div>
          </section>

          <section class="full" id="pw" hidden>
            <div class="topbar"><button class="back" id="pwBack" type="button" aria-label="Annuler"><svg class="i" viewBox="0 0 24 24"><path d="M15 5l-7 7 7 7"/></svg></button><h2 id="pwTitle"></h2><button class="primary" id="pwGo" type="button" disabled>Se connecter</button></div>
            <div class="field" id="pwField"><div id="pwVal" class="empty"></div><button class="eye" id="pwEye" type="button" aria-label="Afficher le mot de passe"><svg class="i" viewBox="0 0 24 24"><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7-10-7-10-7z"/><circle cx="12" cy="12" r="3"/></svg></button></div>
            <div id="pwMsg">8 caractères minimum</div>
            <div id="kbd"></div>
            <div id="busy" hidden><div class="spin"></div><span id="busyT"></span></div>
          </section>
        </div>
      </div>
    </div>
    <div class="cable" aria-hidden="true"></div>
  </div>

  <div class="controls">
    <div class="ctl" id="ctlSpeed"><span>Vitesse du trafic</span><div class="seg" id="segSpeed"><button id="sp1" type="button" data-v="1" aria-pressed="true">×1</button><button id="sp5" type="button" data-v="5" aria-pressed="false">×5</button><button id="sp20" type="button" data-v="20" aria-pressed="false">×20</button></div></div>
    <div class="ctl"><span>Taille réelle</span><div class="seg" id="segSize"><button id="szFit" type="button" data-v="fit" aria-pressed="true">Agrandi</button><button id="sz43" type="button" data-v="4.3" aria-pressed="false">4,3"</button><button id="sz5" type="button" data-v="5" aria-pressed="false">5"</button><button id="sz6" type="button" data-v="6" aria-pressed="false">6"</button><button id="sz7" type="button" data-v="7" aria-pressed="false">7"</button></div></div>
  </div>

  <section class="notes">
    <div><h2>Ce qui est réel</h2><p id="nReal">Frontières, Rhin, Ill, autoroutes, zones urbaines, villes et position de l'EuroAirport. L'axe de piste et l'approche sont approximatifs. Pour de vrais avions, lancez radar.py sur votre ordinateur.</p></div>
    <div><h2>Ce qui est simulé</h2><p id="nSim">Tous les vols, indicatifs et immatriculations, et les réseaux Wi-Fi. Pour tester, n'importe quel mot de passe de 8 caractères fonctionne.</p></div>
    <div><h2>Sur le vrai appareil</h2><p>Le microcontrôleur appellera les mêmes API sans passer par un serveur. L'interface reste réalisable avec LVGL, dont Montserrat est la police par défaut.</p></div>
  </section>
  <p class="credit">Carte : Natural Earth, france-geojson, swiss-maps (OFS/swisstopo), deutschlandGeoJSON. Version en direct : <a href="https://adsb.lol">adsb.lol</a>, <a href="https://adsb.fi">adsb.fi</a>, <a href="https://airplanes.live">airplanes.live</a>, <a href="https://www.adsbdb.com">adsbdb</a>.</p>
</main>

<script>
const GEO={"fr":[[[7.5819,47.8965],[7.5769,47.8915],[7.5657,47.8867],[7.5601,47.8834],[7.5567,47.8799],[7.5555,47.8735],[7.5559,47.868],[7.5572,47.8638],[7.5636,47.8521],[7.5631,47.8438],[7.5615,47.8387],[7.5497,47.8226],[7.5453,47.8113],[7.5313,47.7868],[7.53,47.7822],[7.5301,47.7729],[7.5481,47.7396],[7.5487,47.7367],[7.5477,47.7299],[7.5435,47.7219],[7.5403,47.7184],[7.5137,47.7028],[7.5125,47.7014],[7.5118,47.697],[7.5195,47.6847],[7.5193,47.6702],[7.5211,47.6637],[7.5384,47.6488],[7.5587,47.6384],[7.5665,47.6329],[7.5703,47.6271],[7.5736,47.6167],[7.5761,47.6142],[7.5891,47.6068],[7.5928,47.6018],[7.593,47.5986],[7.5922,47.5954],[7.5861,47.5842],[7.5846,47.5786],[7.5849,47.5756],[7.5753,47.5761],[7.5671,47.5776],[7.5567,47.5723],[7.5563,47.571],[7.5591,47.5692],[7.5572,47.565],[7.5478,47.5618],[7.5358,47.5551],[7.5307,47.5542],[7.525,47.5515],[7.5255,47.551],[7.5193,47.5461],[7.5061,47.5443],[7.5,47.5398],[7.498,47.536],[7.5017,47.5322],[7.5008,47.5301],[7.5014,47.5287],[7.5074,47.5281],[7.5111,47.5291],[7.5193,47.5346],[7.5245,47.5319],[7.5274,47.5326],[7.5314,47.5283],[7.5312,47.5266],[7.5277,47.5215],[7.5239,47.5189],[7.524,47.5155],[7.5229,47.5142],[7.5205,47.5156],[7.5191,47.5148],[7.5176,47.517],[7.5113,47.5165],[7.5057,47.5142],[7.5035,47.5146],[7.4993,47.5208],[7.4978,47.5212],[7.4988,47.5167],[7.5036,47.5139],[7.5057,47.5116],[7.5082,47.5109],[7.5097,47.5084],[7.5109,47.5026],[7.5087,47.5021],[7.511,47.4967],[7.5023,47.4938],[7.5013,47.4928],[7.5025,47.4908],[7.4999,47.491],[7.4981,47.4889],[7.4939,47.4883],[7.4911,47.484],[7.4869,47.4817],[7.4761,47.48],[7.4709,47.4804],[7.4621,47.4891],[7.4579,47.4886],[7.4509,47.4901],[7.4464,47.4935],[7.4348,47.498],[7.4318,47.4961],[7.4257,47.4886],[7.422,47.4838],[7.4211,47.4804],[7.4261,47.4809],[7.4295,47.4824],[7.4318,47.4807],[7.4361,47.4806],[7.4417,47.4779],[7.4427,47.4787],[7.4462,47.4769],[7.4483,47.4738],[7.4502,47.4753],[7.4556,47.473],[7.4557,47.4705],[7.4496,47.4669],[7.4454,47.4617],[7.4336,47.4591],[7.4301,47.4593],[7.4279,47.4571],[7.4284,47.4533],[7.4228,47.4505],[7.4242,47.4496],[7.421,47.4461],[7.4142,47.4421],[7.4065,47.44],[7.3401,47.44],[7.339,47.4409],[7.3307,47.4416],[7.327,47.44],[7.1876,47.44],[7.1795,47.4421],[7.1708,47.443],[7.1705,47.4439],[7.1729,47.4463],[7.1732,47.4488],[7.1782,47.4585],[7.1774,47.4595],[7.1787,47.4605],[7.1793,47.4634],[7.1784,47.4667],[7.1772,47.4676],[7.1847,47.4774],[7.1872,47.483],[7.1907,47.4864],[7.1914,47.4888],[7.1957,47.4892],[7.2023,47.4917],[7.2009,47.4928],[7.2011,47.4939],[7.1716,47.4895],[7.1617,47.49],[7.1589,47.4908],[7.1522,47.4963],[7.1379,47.5021],[7.128,47.5035],[7.1161,47.4965],[7.1111,47.4949],[7.1012,47.4943],[7.0941,47.4954],[7.0793,47.4889],[7.0759,47.4882],[7.072,47.49],[7.072,47.492],[7.0608,47.4951],[7.0519,47.495],[7.0439,47.4982],[7.0366,47.4975],[7.0362,47.499],[7.0287,47.5016],[7.0274,47.5012],[7.0247,47.5041],[7.021,47.5044],[7.0001,47.4998],[6.9858,47.4933],[6.9834,47.4942],[6.9825,47.4935],[6.9859,47.4922],[6.9887,47.4867],[6.9886,47.4814],[6.9864,47.4768],[6.9885,47.473],[6.9923,47.4717],[6.9929,47.4705],[6.991,47.4684],[6.9918,47.4664],[7.0005,47.4671],[6.9997,47.4653],[7.0011,47.4618],[6.9981,47.4594],[6.9975,47.4563],[7.0014,47.456],[7.0019,47.4539],[6.996,47.4498],[6.9891,47.4477],[6.9813,47.449],[6.97,47.4469],[6.9673,47.44],[6.9,47.44],[6.9,47.9],[7.583,47.9],[7.5819,47.8965]]],"de":[[[8.04,47.5595],[8.0195,47.5565],[8.0071,47.5585],[8.005,47.561],[7.9995,47.5624],[7.9785,47.561],[7.9582,47.5642],[7.9554,47.5634],[7.9523,47.5611],[7.9488,47.5514],[7.9466,47.5499],[7.9371,47.5508],[7.9331,47.553],[7.9192,47.5534],[7.912,47.5564],[7.9077,47.5627],[7.9067,47.5676],[7.9095,47.571],[7.9105,47.574],[7.9093,47.5783],[7.9027,47.5864],[7.8935,47.5925],[7.8865,47.5949],[7.8702,47.5955],[7.8599,47.5944],[7.8488,47.5898],[7.8401,47.5889],[7.8356,47.5891],[7.8341,47.5916],[7.8301,47.5937],[7.8208,47.5947],[7.8171,47.5927],[7.8139,47.5883],[7.8123,47.5856],[7.8101,47.5775],[7.8087,47.5756],[7.7994,47.5702],[7.7937,47.5641],[7.7837,47.5606],[7.7705,47.5591],[7.7572,47.5557],[7.7531,47.5519],[7.7191,47.5489],[7.7136,47.5469],[7.7071,47.543],[7.6974,47.5398],[7.6958,47.5385],[7.696,47.5374],[7.6869,47.5361],[7.6695,47.5371],[7.661,47.5408],[7.6545,47.5478],[7.6427,47.552],[7.6376,47.5581],[7.6324,47.5624],[7.6356,47.5635],[7.644,47.5632],[7.6543,47.5673],[7.6671,47.5679],[7.6734,47.5724],[7.6783,47.5705],[7.6803,47.5713],[7.6817,47.5735],[7.6815,47.5747],[7.6787,47.5753],[7.6766,47.5772],[7.6719,47.5838],[7.6721,47.585],[7.6643,47.5873],[7.6645,47.5893],[7.6706,47.5933],[7.6563,47.5943],[7.6503,47.5961],[7.6392,47.597],[7.636,47.5952],[7.6349,47.5922],[7.6307,47.5903],[7.6189,47.5809],[7.6078,47.581],[7.6072,47.5883],[7.6028,47.5897],[7.6027,47.5927],[7.5965,47.5963],[7.5914,47.5962],[7.593,47.6008],[7.5743,47.6158],[7.5674,47.6317],[7.5247,47.6602],[7.52,47.6663],[7.5198,47.6834],[7.5121,47.6961],[7.5138,47.7028],[7.5349,47.7147],[7.5439,47.7226],[7.5486,47.7354],[7.5334,47.766],[7.5302,47.7834],[7.5458,47.8127],[7.5624,47.841],[7.5634,47.8512],[7.5556,47.8705],[7.5577,47.8809],[7.5776,47.8921],[7.5825,47.8983],[7.5824,47.9],[8.04,47.9],[8.04,47.5595]]],"ch":[[[6.9703,47.4471],[6.9814,47.449],[6.9903,47.4479],[7.0016,47.4537],[7.0009,47.456],[6.9975,47.4566],[6.9981,47.4601],[7.0011,47.4618],[7.0004,47.4669],[6.9918,47.4664],[6.9929,47.4712],[6.9865,47.4762],[6.9885,47.4873],[6.9829,47.4943],[6.9863,47.4936],[7.0009,47.5],[7.0244,47.5042],[7.0272,47.5013],[7.037,47.4975],[7.0448,47.4982],[7.0513,47.4952],[7.0604,47.4952],[7.0713,47.4923],[7.0749,47.4882],[7.0917,47.4948],[7.1119,47.4951],[7.128,47.5036],[7.1381,47.5021],[7.1505,47.4971],[7.1594,47.4908],[7.1693,47.4893],[7.2011,47.4939],[7.2023,47.4917],[7.1909,47.4882],[7.1776,47.4681],[7.1788,47.4642],[7.1783,47.4581],[7.1704,47.4435],[7.1865,47.44],[6.9662,47.44],[6.9703,47.4471]],[[7.4066,47.4402],[7.4139,47.442],[7.4211,47.4464],[7.4233,47.4508],[7.4285,47.4535],[7.4301,47.4593],[7.445,47.4617],[7.4552,47.4704],[7.4558,47.4728],[7.4505,47.4753],[7.4478,47.4742],[7.4438,47.4785],[7.4294,47.4825],[7.4212,47.4802],[7.4255,47.4884],[7.4348,47.498],[7.4546,47.4894],[7.4621,47.4891],[7.4709,47.4804],[7.4876,47.4823],[7.4975,47.4893],[7.502,47.4904],[7.5019,47.4935],[7.511,47.4968],[7.5087,47.5019],[7.5109,47.5026],[7.5092,47.5096],[7.4992,47.516],[7.4978,47.5212],[7.5035,47.5146],[7.5168,47.517],[7.5188,47.5151],[7.5239,47.5152],[7.5238,47.5187],[7.5315,47.5277],[7.5278,47.5323],[7.5244,47.532],[7.5192,47.5345],[7.5098,47.5285],[7.5024,47.5285],[7.499,47.5352],[7.4992,47.5398],[7.5079,47.5448],[7.5192,47.5461],[7.5252,47.5516],[7.5552,47.5646],[7.5589,47.5694],[7.5564,47.5712],[7.5575,47.5726],[7.5663,47.5774],[7.5848,47.5757],[7.589,47.5899],[7.6049,47.5848],[7.6045,47.5778],[7.6191,47.5768],[7.6432,47.5913],[7.6419,47.5942],[7.6458,47.597],[7.6673,47.592],[7.6751,47.592],[7.6828,47.5985],[7.6934,47.6006],[7.6718,47.5874],[7.672,47.5851],[7.6807,47.5827],[7.6837,47.574],[7.6895,47.5715],[7.686,47.5656],[7.6834,47.5712],[7.6752,47.5654],[7.6774,47.5638],[7.6723,47.5638],[7.6702,47.5659],[7.6547,47.5624],[7.6482,47.5599],[7.6362,47.564],[7.6341,47.5611],[7.6515,47.547],[7.6614,47.5448],[7.6668,47.5366],[7.6753,47.5336],[7.6862,47.5325],[7.697,47.5329],[7.7138,47.5394],[7.7241,47.5422],[7.7527,47.545],[7.7579,47.5484],[7.7833,47.5533],[7.7957,47.5574],[7.7995,47.5626],[7.812,47.5708],[7.8188,47.5871],[7.8327,47.5866],[7.8376,47.5826],[7.8472,47.5824],[7.8605,47.5874],[7.8688,47.5885],[7.885,47.5889],[7.8941,47.5864],[7.9065,47.5769],[7.9115,47.569],[7.9074,47.5608],[7.9085,47.5554],[7.9172,47.5477],[7.9323,47.5469],[7.9456,47.544],[7.9486,47.5455],[7.9554,47.5571],[7.96,47.5583],[7.978,47.5552],[7.9992,47.5564],[8.0072,47.555],[8.0195,47.5505],[8.04,47.5542],[8.04,47.44],[7.4066,47.4402]]],"urban":[[[6.9079,47.4893],[6.9048,47.4798],[6.9,47.4747],[6.9,47.4904],[6.9079,47.4893]],[[7.9497,47.5399],[7.9271,47.554],[7.9189,47.5782],[7.9856,47.556],[7.9737,47.5393],[7.9497,47.5399]],[[8.0239,47.5466],[8.0189,47.5671],[8.04,47.5729],[8.04,47.5438],[8.0239,47.5466]],[[7.5539,47.6621],[7.5523,47.6337],[7.5608,47.634],[7.5673,47.6321],[7.5718,47.6283],[7.5746,47.6227],[7.6044,47.6372],[7.719,47.656],[7.719,47.6337],[7.7165,47.6287],[7.7078,47.6226],[7.7094,47.5956],[7.7078,47.5893],[7.7009,47.5863],[7.6797,47.5823],[7.6745,47.5781],[7.6772,47.5694],[7.6939,47.5545],[7.6967,47.5448],[7.71,47.5444],[7.7182,47.5515],[7.7242,47.5607],[7.73,47.5671],[7.7437,47.5706],[7.7713,47.5725],[7.8046,47.5876],[7.816,47.5866],[7.8301,47.5781],[7.8184,47.5582],[7.8093,47.5491],[7.7832,47.5413],[7.7517,47.5228],[7.7412,47.5115],[7.7489,47.4988],[7.7778,47.513],[7.7855,47.5004],[7.7863,47.4794],[7.7702,47.4778],[7.7411,47.4892],[7.7327,47.4805],[7.7265,47.4777],[7.7189,47.4781],[7.7154,47.4819],[7.7105,47.4963],[7.7077,47.5003],[7.6825,47.5063],[7.6717,47.5111],[7.6633,47.5225],[7.6483,47.5154],[7.6377,47.4815],[7.6301,47.467],[7.6105,47.4645],[7.5982,47.4725],[7.5879,47.4833],[7.5605,47.4929],[7.5573,47.5016],[7.5572,47.5152],[7.5522,47.5337],[7.5481,47.5366],[7.53,47.5448],[7.5293,47.5552],[7.5321,47.5795],[7.53,47.5893],[7.4996,47.6272],[7.4634,47.6559],[7.4665,47.6596],[7.4745,47.6782],[7.4799,47.6769],[7.49,47.6677],[7.4968,47.6671],[7.518,47.6853],[7.5189,47.6893],[7.5354,47.6848],[7.5476,47.6774],[7.5539,47.6621]],[[7.5189,47.7781],[7.5155,47.8022],[7.5351,47.821],[7.5967,47.8448],[7.5647,47.8046],[7.5452,47.788],[7.5189,47.7781]],[[7.2907,47.8707],[7.3078,47.8781],[7.3189,47.8893],[7.3298,47.8909],[7.3406,47.8902],[7.3489,47.8918],[7.3529,47.9],[7.3603,47.8941],[7.3632,47.8828],[7.3604,47.872],[7.3524,47.867],[7.3477,47.8605],[7.3332,47.8527],[7.3301,47.8448],[7.3329,47.8338],[7.3423,47.8293],[7.3539,47.8271],[7.3635,47.8227],[7.3635,47.8116],[7.37,47.8137],[7.3866,47.8234],[7.3968,47.8227],[7.4019,47.8133],[7.4169,47.8089],[7.4353,47.7961],[7.4505,47.777],[7.4524,47.7782],[7.4476,47.766],[7.4214,47.7426],[7.398,47.7293],[7.3635,47.7225],[7.3212,47.7014],[7.3078,47.7003],[7.3044,47.7036],[7.2967,47.7225],[7.2856,47.7226],[7.2856,47.7336],[7.2745,47.7337],[7.2634,47.7559],[7.2264,47.7606],[7.219,47.767],[7.2199,47.7857],[7.219,47.7892],[7.2117,47.7907],[7.1856,47.7892],[7.1799,47.7918],[7.1703,47.7989],[7.1634,47.8004],[7.1717,47.8144],[7.1776,47.8197],[7.1856,47.8227],[7.2126,47.8136],[7.2249,47.8119],[7.23,47.8226],[7.2272,47.8329],[7.2232,47.84],[7.2256,47.8468],[7.2412,47.856],[7.256,47.8581],[7.2614,47.8602],[7.2634,47.867],[7.258,47.8758],[7.2314,47.8759],[7.219,47.8781],[7.2181,47.8818],[7.2189,47.9],[7.2742,47.9],[7.267,47.8927],[7.2696,47.8887],[7.2775,47.8822],[7.2857,47.8671],[7.2907,47.8707]]],"roads":[[[7.3304,47.7826],[7.3407,47.765],[7.3538,47.749],[7.4295,47.6743],[7.4373,47.6681],[7.4435,47.6647],[7.4504,47.6621],[7.4643,47.6579],[7.5065,47.6418],[7.5212,47.6391]],[[7.6047,47.9],[7.5624,47.8157],[7.5554,47.7959],[7.5511,47.7766],[7.5496,47.757],[7.5511,47.7374],[7.5557,47.7182],[7.5626,47.7019],[7.5946,47.6501],[7.5974,47.6424],[7.6005,47.6262]],[[7.3293,47.7619],[7.0121,47.6897],[6.9453,47.6678],[6.9271,47.6596],[6.9098,47.6498],[6.9,47.6431]],[[8.04,47.5413],[8.0307,47.5457],[8.0121,47.5518],[7.9943,47.5556],[7.9582,47.5593],[7.8441,47.5622],[7.8246,47.5653],[7.806,47.5707],[7.7889,47.578],[7.7043,47.625],[7.6749,47.6364],[7.6688,47.638],[7.6559,47.6394],[7.6496,47.6391],[7.6382,47.6372],[7.6158,47.6314],[7.5986,47.6294]],[[7.6005,47.6262],[7.6321,47.5786],[7.6421,47.566],[7.6458,47.5623],[7.6609,47.5509],[7.6692,47.5462],[7.6781,47.5426],[7.6969,47.5379],[7.7064,47.5366],[7.7161,47.5362],[7.7446,47.5385],[7.8276,47.5606]],[[7.08,47.4445],[7.0874,47.44]],[[7.7544,47.5374],[7.8119,47.4794],[7.8276,47.4581],[7.8307,47.4509],[7.833,47.44]],[[7.5261,47.6393],[7.4655,47.7173],[7.4487,47.7443],[7.4259,47.7915],[7.3881,47.9]]],"ill":[[[7.2197,47.4617],[7.2048,47.501],[7.1903,47.5312],[7.1763,47.5487],[7.1585,47.5648],[7.135,47.5915],[7.1172,47.6209],[7.1133,47.643],[7.126,47.6576],[7.1487,47.6689],[7.1763,47.6738],[7.2007,47.6727],[7.2275,47.6758],[7.2611,47.6907],[7.2927,47.7159],[7.3147,47.7437],[7.3301,47.7671],[7.3455,47.7814],[7.3566,47.7968],[7.3618,47.8227],[7.3591,47.8511],[7.3513,47.8725],[7.3513,47.8959],[7.3535,47.9]]],"rhine":[[[7.588,47.6049],[7.5743,47.6158],[7.5674,47.6317],[7.5247,47.6602],[7.52,47.6663],[7.5198,47.6834],[7.5121,47.6961],[7.5138,47.7028],[7.5349,47.7147],[7.5439,47.7226],[7.5486,47.7354],[7.5398,47.7543],[7.5334,47.766],[7.5302,47.7834],[7.5458,47.8127],[7.5624,47.841],[7.5634,47.8512],[7.5556,47.8705],[7.5577,47.8809],[7.5776,47.8921],[7.5823,47.898]],[[7.588,47.6049],[7.5905,47.575],[7.5905,47.564],[7.5895,47.5603],[7.5985,47.5567],[7.6165,47.56],[7.6285,47.559],[7.6448,47.5509],[7.6545,47.5478],[7.6569,47.5461],[7.661,47.5408],[7.6695,47.5371],[7.6869,47.5361],[7.696,47.5374],[7.6958,47.5385],[7.6974,47.5398],[7.7071,47.543],[7.7136,47.5469],[7.7191,47.5489],[7.7531,47.5519],[7.7572,47.5557],[7.7705,47.5591],[7.7837,47.5606],[7.7931,47.5638],[7.7994,47.5702],[7.8087,47.5756],[7.8101,47.5775],[7.8123,47.5856],[7.8139,47.5883],[7.8171,47.5927],[7.8208,47.5947],[7.8301,47.5937],[7.8341,47.5916],[7.8356,47.5891],[7.8401,47.5889],[7.8488,47.5898],[7.8599,47.5944],[7.8702,47.5955],[7.8865,47.5949],[7.8935,47.5925],[7.9027,47.5864],[7.9093,47.5783],[7.9105,47.574],[7.9095,47.571],[7.9067,47.5676],[7.9077,47.5627],[7.909,47.5598],[7.912,47.5564],[7.9192,47.5534],[7.9331,47.553],[7.9371,47.5508],[7.9466,47.5499],[7.9488,47.5514],[7.9523,47.5611],[7.9554,47.5634],[7.9582,47.5642],[7.9705,47.5616],[7.9785,47.561],[7.9925,47.5625],[7.9995,47.5624],[8.005,47.561],[8.0071,47.5585],[8.0195,47.5565],[8.038,47.5592]]]};
(function(){
"use strict";
const $=id=>document.getElementById(id);
const LON0=6.93,LON1=8.01,LAT0=47.47,LAT1=47.87,W=800,H=480,TOP=40,MH=440;
const X=lon=>(lon-LON0)/(LON1-LON0)*W, Y=lat=>TOP+(LAT1-lat)/(LAT1-LAT0)*MH;
const KM_LAT=111.32,COS=Math.cos(47.67*Math.PI/180),KM_LON=KM_LAT*COS;
const dist=(a,b,c,d)=>Math.hypot((c-a)*KM_LAT,(d-b)*KM_LON);
const rnd=(a,b)=>a+Math.random()*(b-a), pick=a=>a[Math.floor(Math.random()*a.length)];
const num=n=>Math.round(n).toLocaleString('fr-FR').replace(/ | /g,' ');
const MUL=[47.7508,7.3359];

/* ---------- altitude scale ---------- */
const STOPS=[[0,[255,157,92]],[5000,[255,209,102]],[15000,[127,227,161]],[30000,[108,201,255]],[42000,[185,167,255]]];
function altColor(a){a=Math.max(0,Math.min(42000,a));for(let i=1;i<STOPS.length;i++){if(a<=STOPS[i][0]){const[p,c]=STOPS[i-1],[q,d]=STOPS[i],t=(a-p)/(q-p);return`rgb(${c.map((v,j)=>Math.round(v+(d[j]-v)*t)).join(',')})`}}return'rgb(185,167,255)'}

/* ---------- base map ---------- */
const base=document.createElement('canvas');const cv=$('map'),ctx=cv.getContext('2d');let RS=1;
const CITIES=[
 ['Mulhouse',7.3359,47.7508,1,'r'],['Bâle',7.5886,47.5596,1,'l'],
 ['Saint-Louis',7.5600,47.5900,2,'r'],['Lörrach',7.6610,47.6150,2,'r'],
 ['Altkirch',7.2397,47.6236,2,'l'],['Thann',7.1030,47.8070,2,'l'],['Cernay',7.1760,47.8090,3,'r'],['Ensisheim',7.3530,47.8660,3,'r'],
 ['Sierentz',7.4550,47.6600,3,'l'],['Neuenburg',7.5600,47.8130,3,'l'],['Müllheim',7.6300,47.8080,2,'r'],['Schopfheim',7.8220,47.6500,3,'r'],
 ['Liestal',7.7340,47.4840,2,'r'],['Rheinfelden',7.7940,47.5540,3,'r'],['Dannemarie',7.1210,47.6300,3,'l'],
 ['Masevaux',6.9960,47.7740,3,'r']];
const AREAS=[['VOSGES',7.03,47.845,'#8fb3a4'],['SUNDGAU',7.20,47.545,'#8fb3a4'],['JURA',7.50,47.487,'#8fb3a4'],['FORÊT-NOIRE',7.86,47.76,'#8fb3a4'],['Forêt de la Hardt',7.47,47.80,'#7fa58f']];
const THR15=[47.606,7.518],THR33=[47.574,7.540],BSL=[47.590,7.529];
function poly(c,rings){c.beginPath();for(const r of rings){r.forEach((p,i)=>i?c.lineTo(X(p[0]),Y(p[1])):c.moveTo(X(p[0]),Y(p[1])));c.closePath()}}
function line(c,l){c.beginPath();l.forEach((p,i)=>i?c.lineTo(X(p[0]),Y(p[1])):c.moveTo(X(p[0]),Y(p[1])))}
function glow(c,lon,lat,r,a){const x=X(lon),y=Y(lat),g=c.createRadialGradient(x,y,0,x,y,r);g.addColorStop(0,`rgba(136,170,150,${a})`);g.addColorStop(1,'rgba(136,170,150,0)');c.fillStyle=g;c.fillRect(0,0,W,H)}
function text(c,s,x,y,font,fill,align,halo){c.font=font;c.textAlign=align||'left';c.textBaseline='middle';c.lineJoin='round';c.lineWidth=3;c.strokeStyle=halo||'rgba(15,28,34,.85)';c.strokeText(s,x,y);c.fillStyle=fill;c.fillText(s,x,y)}
function drawBase(){
 base.width=Math.round(W*RS);base.height=Math.round(H*RS);const c=base.getContext('2d');c.setTransform(RS,0,0,RS,0,0);
 c.fillStyle='#14252c';c.fillRect(0,0,W,H);
 [[GEO.fr,'#14252c'],[GEO.de,'#172a30'],[GEO.ch,'#1a2d33']].forEach(([g,f])=>{poly(c,g);c.fillStyle=f;c.fill()});
 glow(c,6.95,47.88,250,.17);glow(c,7.0,47.70,140,.09);glow(c,7.92,47.80,300,.17);glow(c,7.35,47.40,170,.14);glow(c,7.72,47.40,190,.15);
 poly(c,GEO.urban);c.fillStyle='rgba(148,178,186,.13)';c.fill();
 c.lineCap='round';c.lineJoin='round';
 c.strokeStyle='#34515b';c.lineWidth=1.4;GEO.roads.forEach(l=>{line(c,l);c.stroke()});
 c.strokeStyle='#3b7f9c';c.lineWidth=1.2;GEO.ill.forEach(l=>{line(c,l);c.stroke()});
 c.setLineDash([2,4]);c.strokeStyle='#6d8c97';c.lineWidth=1;[GEO.fr,GEO.ch].forEach(g=>{poly(c,g);c.stroke()});c.setLineDash([]);
 c.strokeStyle='#4fa3c7';c.lineWidth=2.4;GEO.rhine.forEach(l=>{line(c,l);c.stroke()});
 /* rings around the airport (NM) */
 const bx=X(BSL[1]),by=Y(BSL[0]);c.strokeStyle='rgba(180,205,212,.13)';c.lineWidth=1;
 [10,20].forEach(nm=>{c.beginPath();c.arc(bx,by,nm*1.852/KM_LON/(LON1-LON0)*W,0,7);c.stroke()});
 text(c,'10 NM',bx,by-(10*1.852/KM_LON/(LON1-LON0)*W),'600 9.5px Montserrat,sans-serif','#7f9aa3','center');
 text(c,'20 NM',bx,by-(20*1.852/KM_LON/(LON1-LON0)*W),'600 9.5px Montserrat,sans-serif','#7f9aa3','center');
 /* approach path */
 c.setLineDash([5,5]);c.strokeStyle='rgba(255,200,87,.28)';c.lineWidth=1;c.beginPath();c.moveTo(X(THR15[1]),Y(THR15[0]));c.lineTo(X(7.394),Y(47.786));c.stroke();c.setLineDash([]);
 /* runway */
 c.strokeStyle='#e6eff1';c.lineWidth=3;c.lineCap='butt';c.beginPath();c.moveTo(X(THR15[1]),Y(THR15[0]));c.lineTo(X(THR33[1]),Y(THR33[0]));c.stroke();c.lineCap='round';
 text(c,'BSL · MLH',bx-9,by+11,'700 11px Montserrat,sans-serif','#ffc857','right');
 /* Habsheim */
 c.strokeStyle='#9fb7bf';c.lineWidth=1.2;c.beginPath();c.arc(X(7.432),Y(47.738),3,0,7);c.stroke();
 text(c,'LFGB',X(7.432)+6,Y(47.738)+1,'600 9.5px Montserrat,sans-serif','#9fb7bf');
 /* names */
 AREAS.forEach(([s,lo,la,col])=>{const it=s===s.toUpperCase()?'600 10.5px':'italic 500 10px';if(c.letterSpacing!==undefined)c.letterSpacing=s===s.toUpperCase()?'2.5px':'0px';text(c,s,X(lo),Y(la),it+' Montserrat,sans-serif',col,'center')});
 if(c.letterSpacing!==undefined)c.letterSpacing='3px';
 [['FRANCE',7.09,47.70],['ALLEMAGNE',7.77,47.685],['SUISSE',7.40,47.515]].forEach(([s,lo,la])=>text(c,s,X(lo),Y(la),'700 11.5px Montserrat,sans-serif','rgba(205,222,227,.42)','center','rgba(15,28,34,0)'));
 if(c.letterSpacing!==undefined)c.letterSpacing='0px';
 CITIES.forEach(([s,lo,la,t,side])=>{const x=X(lo),y=Y(la),r=t===1?3.6:t===2?2.6:2;
  c.fillStyle=t===1?'#ffffff':'#c5d6db';c.beginPath();c.arc(x,y,r,0,7);c.fill();
  const f=t===1?'700 14.5px':t===2?'600 11.5px':'500 10.5px',col=t===1?'#ffffff':t===2?'#d5e2e6':'#aec2c9';
  text(c,s,side==='l'?x-r-4:x+r+4,y,f+' Montserrat,sans-serif',col,side==='l'?'right':'left')});
 /* scale + legend */
 const lx=10,ly=H-34,lw=172;c.fillStyle='rgba(15,28,34,.82)';c.fillRect(lx-4,ly-8,lw+8,38);
 const gr=c.createLinearGradient(lx,0,lx+lw,0);STOPS.forEach(([a,col])=>gr.addColorStop(a/42000,`rgb(${col})`));c.fillStyle=gr;c.fillRect(lx,ly-3,lw,6);
 [[0,'0'],[10000,'10k'],[20000,'20k'],[30000,'30k'],[40000,'40k ft']].forEach(([a,s])=>{const x=lx+a/42000*lw;c.fillStyle='#aec2c9';c.fillRect(x,ly+3,1,3);text(c,s,a===40000?x+8:x,ly+12,'600 9.5px Montserrat,sans-serif','#aec2c9',a===0?'left':'center','rgba(0,0,0,0)')});
 const k10=10/KM_LON/(LON1-LON0)*W,sx=lx+lw+16,sy=H-14;c.fillStyle='rgba(15,28,34,.82)';c.fillRect(sx-5,sy-20,k10+10,27);c.strokeStyle='#d5e2e6';c.lineWidth=1.5;c.beginPath();c.moveTo(sx,sy-4);c.lineTo(sx,sy);c.lineTo(sx+k10,sy);c.lineTo(sx+k10,sy-4);c.stroke();text(c,'10 km',sx+k10/2,sy-11,'600 10px Montserrat,sans-serif','#d5e2e6','center','rgba(0,0,0,0)');
}

/* ---------- fake flight data ---------- */
const AP={BSL:['Bâle-Mulhouse',47.59,7.53],ZRH:['Zurich',47.46,8.55],GVA:['Genève',46.24,6.11],CDG:['Paris',49.01,2.55],LHR:['Londres',51.47,-.46],LGW:['Londres',51.15,-.19],AMS:['Amsterdam',52.31,4.76],FRA:['Francfort',50.03,8.57],MUC:['Munich',48.35,11.79],VIE:['Vienne',48.11,16.57],BRU:['Bruxelles',50.9,4.48],MXP:['Milan',45.63,8.72],FCO:['Rome',41.8,12.25],ATH:['Athènes',37.94,23.95],BCN:['Barcelone',41.3,2.08],LYS:['Lyon',45.73,5.08],MRS:['Marseille',43.44,5.22],LIS:['Lisbonne',38.77,-9.13],OPO:['Porto',41.24,-8.68],PMI:['Palma',39.55,2.74],AGP:['Malaga',36.67,-4.5],BER:['Berlin',52.36,13.5],PRN:['Pristina',42.57,21.04],SKP:['Skopje',41.96,21.62],IST:['Istanbul',41.26,28.74],LFGB:['Habsheim',47.738,7.432]};
const AL={EZS:['U2','easyJet Switzerland','HB-J',['A320','A20N','A319'],'4B'],EJU:['U2','easyJet Europe','OE-I',['A320','A20N'],'44'],WZZ:['W6','Wizz Air','HA-L',['A321','A21N'],'47'],KLM:['KL','KLM','PH-E',['E190','E295'],'48'],AFR:['AF','Air France','F-H',['A20N','A223','A320'],'39'],SWR:['LX','Swiss','HB-J',['A20N','BCS3','A21N','A333'],'4B'],DLH:['LH','Lufthansa','D-A',['A320','A20N','A321','B748'],'3C'],BAW:['BA','British Airways','G-E',['A320','A20N','A21N'],'40'],AUA:['OS','Austrian','OE-L',['A320','E195'],'44'],THY:['TK','Turkish Airlines','TC-J',['A321','B738','A333'],'4B'],RYR:['FR','Ryanair','EI-',['B738','B38M'],'4C'],VLG:['VY','Vueling','EC-M',['A320','A20N'],'34'],FDX:['FX','FedEx','N8',['B77L'],'A1'],ITY:['AZ','ITA Airways','EI-D',['A320','A20N'],'4C']};
const OVER=[['CDG','ZRH','SWR',-1],['LHR','ZRH','SWR',-1],['ZRH','CDG','AFR',1],['ZRH','LHR','BAW',1],['AMS','MXP','KLM',0],['BRU','MXP','ITY',0],['LHR','FCO','BAW',0],['LHR','ATH','BAW',0],['CDG','ATH','AFR',0],['FRA','BCN','DLH',0],['FRA','LYS','DLH',0],['FRA','MRS','DLH',0],['MUC','CDG','AFR',0],['VIE','CDG','AUA',0],['IST','LHR','THY',0],['BCN','FRA','VLG',0],['MXP','LHR','RYR',0],['LIS','MUC','DLH',0],['AGP','VIE','AUA',0],['MRS','FRA','FDX',0],['FCO','AMS','KLM',0],['ATH','LHR','BAW',0]];
const BSLDEST=[['PMI','EZS','W'],['LIS','EZS','W'],['OPO','EJU','W'],['AGP','EZS','W'],['LGW','EZS','W'],['CDG','AFR','W'],['AMS','KLM','N'],['BER','EZS','N'],['PRN','WZZ','E'],['SKP','WZZ','E'],['IST','THY','E']];
const EXIT={W:[[47.455,7.26,10500,270],[47.34,6.62,19000,330]],N:[[47.50,7.74,10000,270],[47.72,7.92,15000,320],[48.12,7.98,20000,350]],E:[[47.49,7.78,10500,270],[47.41,8.42,20000,340]]};
const ENTRY=[[[47.98,7.26,10000,270],[47.786,7.394,4400,170]],[[47.78,6.80,12000,290],[47.86,7.14,7500,220],[47.80,7.385,4600,175]],[[47.82,8.15,12000,290],[47.91,7.60,7500,220],[47.84,7.33,5500,190],[47.77,7.405,4300,170]]];
function bearing(a,b){const[φ1,λ1,φ2,λ2]=[a[1],a[2],b[1],b[2]].map(v=>v*Math.PI/180);const y=Math.sin(λ2-λ1)*Math.cos(φ2),x=Math.cos(φ1)*Math.sin(φ2)-Math.sin(φ1)*Math.cos(φ2)*Math.cos(λ2-λ1);return(Math.atan2(y,x)*180/Math.PI+360)%360}
const hex=n=>Array.from({length:n},()=>'0123456789ABCDEF'[Math.floor(Math.random()*16)]).join('');
const ltr=n=>Array.from({length:n},()=>'ABCDEFGHJKLMNPQRSTUVWXYZ'[Math.floor(Math.random()*24)]).join('');
const squawk=()=>Array.from({length:4},()=>Math.floor(Math.random()*8)).join('');
let planes=[],uid=0,simT=0;
function mk(o){return Object.assign({id:++uid,trail:[],prof:[],ts:0,ps:0,vs:0,visible:false,labelPos:0},o)}
function airlineBits(code){const a=AL[code],n=Math.floor(rnd(100,9800));return{call:code+n,flight:a[0]+' '+n,airline:a[1],reg:a[2]+ltr(a[2].endsWith('-')?3:(a[2].length>=4?2:3)),type:pick(a[3]),hex:a[4]+hex(4),sq:squawk()}}
function spawnOver(){
 const[o,d,al,mode]=pick(OVER),tr=bearing(AP[o],AP[d]),clat=rnd(47.50,47.84),clon=rnd(7.05,7.9),rad=tr*Math.PI/180,back=115;
 const lat=clat-back*Math.cos(rad)/KM_LAT,lon=clon-back*Math.sin(rad)/KM_LON,eLat=clat+back*Math.cos(rad)/KM_LAT,eLon=clon+back*Math.sin(rad)/KM_LON;
 const fl=pick([31000,33000,35000,36000,37000,38000,39000]);
 let a0=fl,a1=fl,gs=rnd(415,490);
 if(mode===-1){a0=rnd(24000,29000);a1=a0-rnd(9000,12000);gs=rnd(360,410)} if(mode===1){a0=rnd(17000,24000);a1=a0+rnd(10000,13000);gs=rnd(360,420)}
 planes.push(mk(Object.assign(airlineBits(al),{kind:'over',from:o,to:d,lat,lon,trk:tr,alt:a0,gs,path:[[eLat,eLon,a1,gs]]})));
}
function spawnArr(){
 const[o,al]=pick(BSLDEST),p=pick(ENTRY),s=p[0],rest=p.slice(1).concat([[THR15[0],THR15[1],1200,138]]);
 planes.push(mk(Object.assign(airlineBits(al),{kind:'arr',from:o,to:'BSL',lat:s[0],lon:s[1],trk:bearing([0,s[0],s[1]],[0,rest[0][0],rest[0][1]]),alt:s[2],gs:s[3],path:rest})));
}
function spawnDep(){
 const[d,al,dir]=pick(BSLDEST);
 planes.push(mk(Object.assign(airlineBits(al),{kind:'dep',from:'BSL',to:d,lat:THR15[0]-.004,lon:THR15[1]+.003,trk:155,alt:1100,gs:140,path:[[47.552,7.556,4800,215]].concat(EXIT[dir])})));
}
function spawnGA(){
 const reg='F-'+pick(['GT','GN','BX','HA'])+ltr(2);
 const lp=[[47.72,7.47,2500,100],[47.68,7.33,3000,105],[47.71,7.14,3400,105],[47.80,7.17,3400,105],[47.83,7.33,3000,105],[47.78,7.46,2600,100],[47.738,7.432,1900,85]];
 planes.push(mk({kind:'ga',call:reg.replace('-',''),flight:'Vol local VFR',airline:'Aviation légère',reg,type:pick(['DR40','C172','P28A']),hex:'39'+hex(4),sq:'7000',from:'LFGB',to:'LFGB',lat:47.738,lon:7.432,trk:50,alt:1900,gs:90,path:lp.slice()}));
}
function spawnHeli(){
 const reg='HB-Z'+ltr(2);
 planes.push(mk({kind:'heli',call:'HB'+reg.slice(3),flight:'Vol hélicoptère',airline:'Transport héliporté',reg,type:'EC35',hex:'4B'+hex(4),sq:'7000',from:'',to:'',lat:47.50,lon:7.95,trk:300,alt:2400,gs:115,path:[[47.54,7.68,2600,120],[47.58,7.60,2000,95],[47.68,7.42,2700,120],[47.84,7.20,3200,120],[48.0,7.0,3500,120]]}));
}
const ROUTE_NAMES=c=>AP[c]?AP[c][0]:'';

/* ---------- simulation ---------- */
function step(p,dt){
 const wp=p.path[0];
 if(wp){
  const d=dist(p.lat,p.lon,wp[0],wp[1]),want=bearing([0,p.lat,p.lon],[0,wp[0],wp[1]]);
  /* bearing over short distances, corrected for local longitude */
  const turnRate=p.kind==='over'?1.2:p.kind==='ga'||p.kind==='heli'?4:2.6;
  let diff=((want-p.trk+540)%360)-180;p.trk=(p.trk+Math.max(-turnRate*dt,Math.min(turnRate*dt,diff))+360)%360;
  const ms=p.gs*0.514444/1000,t=Math.max(dt,d/ms);
  const rate=(wp[2]-p.alt)/t;p.alt+=rate*dt;p.vs=p.vs*.9+rate*60*.1;p.gs+=(wp[3]-p.gs)/t*dt;
  if(d<(p.kind==='arr'&&p.path.length===1?.6:p.kind==='over'?2:1.3)){
    p.path.shift();
    if(!p.path.length&&p.kind==='ga'&&simT<1e9){p.path=[[47.72,7.47,2500,100],[47.68,7.33,3000,105],[47.71,7.14,3400,105],[47.80,7.17,3400,105],[47.83,7.33,3000,105],[47.78,7.46,2600,100],[47.738,7.432,1900,85]]}
  }
 }
 const r=p.trk*Math.PI/180,km=p.gs*0.514444/1000*dt;p.lat+=km*Math.cos(r)/KM_LAT;p.lon+=km*Math.sin(r)/KM_LON;
 p.ts+=dt;if(p.ts>=8){p.ts=0;p.trail.push([p.lat,p.lon,p.alt]);if(p.trail.length>26)p.trail.shift()}
 p.ps+=dt;if(p.ps>=15){p.ps=0;p.prof.push(p.alt);if(p.prof.length>80)p.prof.shift()}
 const x=X(p.lon),y=Y(p.lat);p.visible=x>-20&&x<W+20&&y>TOP-20&&y<H+20;if(p.visible)p.seen=true;
}
let nextOver=0,nextArr=0,nextDep=0,nextHeli=rnd(200,600),nextGA=0;
function sim(dt){
 simT+=dt;
 if(simT>=nextOver){spawnOver();nextOver=simT+rnd(38,78)}
 if(simT>=nextArr){spawnArr();nextArr=simT+rnd(260,460)}
 if(simT>=nextDep){spawnDep();nextDep=simT+rnd(240,430)}
 if(simT>=nextHeli){spawnHeli();nextHeli=simT+rnd(900,1800)}
 if(simT>=nextGA&&!planes.some(p=>p.kind==='ga')){spawnGA();nextGA=simT+60}
 for(const p of planes)step(p,dt);
 planes=planes.filter(p=>{
  const done=!p.path.length;
  const far=p.lat<46.3||p.lat>49||p.lon<5.6||p.lon>9.4;
  const gone=p.seen&&!p.visible&&(p.kind==='over'||p.kind==='dep'||p.kind==='heli');
  if(done||far||gone){
    if(selected===p){const msg=p.kind==='arr'&&done?p.call+' a atterri à Bâle-Mulhouse':p.call+' a quitté la zone';closePanel();toast(msg)}
    return false}
  return true});
}

/* ---------- rendering ---------- */
/* Top-view silhouettes, nose up: only the right half is described, the left is its mirror. */
function mirrored(h){const p=new Path2D();p.moveTo(0,h[0][1]);for(const[x,y]of h.slice(1))p.lineTo(x,y);for(const[x,y]of h.slice(1,-1).reverse())p.lineTo(-x,y);p.closePath();return p}
const JET=mirrored([[0,-13],[.8,-12],[1.5,-10],[1.7,-7],[1.7,-3.2],[12,3.6],[12,5.6],[1.7,2.8],[1.4,8],[5,11],[5,12.6],[.7,11.4],[0,13]]);
const LIGHT=mirrored([[0,-10],[1,-9],[1.4,-6],[1.5,-4.5],[11.5,-3.6],[11.5,-.8],[1.3,-.2],[.8,6.6],[4.2,7.2],[4.2,9.2],[.5,9.4],[0,10]]);
function drawHeli(c){c.beginPath();c.moveTo(-.7,3);c.lineTo(-.7,11);c.lineTo(-3,11);c.lineTo(-3,13);c.lineTo(3,13);c.lineTo(3,11);c.lineTo(.7,11);c.lineTo(.7,3);c.closePath();c.stroke();c.fill();c.beginPath();c.ellipse(0,-1.5,3.2,5.5,0,0,7);c.stroke();c.fill();const col=c.fillStyle;c.lineCap='round';c.lineWidth=3.2;c.beginPath();c.moveTo(-9,-8.5);c.lineTo(9,5.5);c.moveTo(9,-8.5);c.lineTo(-9,5.5);c.stroke();c.lineWidth=1.5;c.strokeStyle=col;c.stroke()}
const LBL=[[13,-12],[13,12],[-13,-12],[-13,12]];
function labelBox(p,i){const x=X(p.lon),y=Y(p.lat),[dx,dy]=LBL[i],w=p.lw||58;return{x:dx>0?x+dx:x+dx-w,y:y+dy-8,w,h:16}}
function overlap(a,b){return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h}
let selected=null;
function draw(){
 ctx.setTransform(RS,0,0,RS,0,0);ctx.drawImage(base,0,0,W,H);
 if(!wifi.connected)return;
 const vis=planes.filter(p=>p.visible);
 ctx.lineCap='round';
 for(const p of vis){const n=p.trail.length;if(n<1)continue;const sel=p===selected;
  for(let i=0;i<n;i++){const a=p.trail[i],b=p.trail[i+1]||[p.lat,p.lon,p.alt];ctx.strokeStyle=altColor(a[2]);ctx.globalAlpha=(sel?.95:.55)*((i+1)/n);ctx.lineWidth=sel?2.6:1.6;ctx.beginPath();ctx.moveTo(X(a[1]),Y(a[0]));ctx.lineTo(X(b[1]),Y(b[0]));ctx.stroke()}}
 ctx.globalAlpha=1;
 ctx.font='600 12px Montserrat,sans-serif';
 const placed=[];
 const order=vis.slice().sort((a,b)=>(a===selected?-1:b===selected?1:a.id-b.id));
 for(const p of order){p.lw=ctx.measureText(p.call).width+10;let i=p.labelPos,box=labelBox(p,i);
  if(placed.some(q=>overlap(box,q))){for(let k=0;k<4;k++){const b=labelBox(p,k);if(!placed.some(q=>overlap(b,q))){i=k;box=b;break}}}
  p.labelPos=i;placed.push(box);
  ctx.fillStyle=p===selected?'#e6eff1':'rgba(15,28,34,.82)';ctx.fillRect(box.x,box.y,box.w,box.h);
  ctx.fillStyle=p===selected?'#14252c':'#e6eff1';ctx.textAlign='left';ctx.textBaseline='middle';ctx.fillText(p.call,box.x+5,box.y+8.5)}
 for(const p of vis){const x=X(p.lon),y=Y(p.lat),col=altColor(p.alt);ctx.save();ctx.translate(x,y);
  if(p===selected){ctx.strokeStyle='#ffffff';ctx.lineWidth=2;ctx.beginPath();ctx.arc(0,0,17,0,7);ctx.stroke()}
  ctx.rotate(p.trk*Math.PI/180);ctx.fillStyle=col;ctx.strokeStyle='#0b1519';ctx.lineWidth=1.3;ctx.lineJoin='round';
  if(p.kind==='heli')drawHeli(ctx);else{const sh=p.kind==='ga'?LIGHT:JET;ctx.stroke(sh);ctx.fill(sh)}
  ctx.restore()}
}

/* ---------- flight card ---------- */
const PH=p=>p.live?(Math.abs(p.vs)<150?(p.alt>=20000?'Croisière':'Palier'):p.vs>0?'Montée':'Descente'):p.kind==='ga'?'VFR':p.kind==='heli'?'Hélico':Math.abs(p.vs)<150?'Croisière':p.vs>0?'Montée':(p.kind==='arr'&&p.alt<6000?'Approche':'Descente');
function fmtVs(v){v=Math.round(v/50)*50;return(v>0?'+':v<0?'−':'')+num(Math.abs(v))}
const CARD=['N','NE','E','SE','S','SO','O','NO'];
function updatePanel(){const p=selected;if(!p)return;
 $('dAlt').textContent=num(Math.round(p.alt/10)*10)+' ft';const fl=p.alt>=8000?'FL'+String(Math.round(p.alt/100)).padStart(3,'0')+' · ':'';$('dAlt2').textContent=fl+num(Math.round(p.alt*.3048/10)*10)+' m';
 $('dGs').textContent=Math.round(p.gs)+' kt';$('dGs2').textContent=num(p.gs*1.852)+' km/h';
 $('dTrk').textContent=String(Math.round(p.trk)%360).padStart(3,'0')+'°';$('dTrk2').textContent='vers '+CARD[Math.round(p.trk/45)%8];
 $('dVs').textContent=fmtVs(p.vs)+' ft/min';const d=dist(p.lat,p.lon,MUL[0],MUL[1]);$('dVs2').textContent=Math.round(d)+' km de Mulhouse';
 $('pPhase').textContent=PH(p);$('pPhase').style.color=altColor(p.alt);
 if(p.from&&p.to&&p.from!==p.to){const a=AP[p.from],b=AP[p.to],da=Math.hypot((p.lat-a[1])*KM_LAT,(p.lon-a[2])*KM_LON),db=Math.hypot((p.lat-b[1])*KM_LAT,(p.lon-b[2])*KM_LON);$('pProg').style.width=(da/(da+db)*100).toFixed(1)+'%'}else $('pProg').style.width='0';
 const v=p.prof.concat([p.alt]);const W2=248,H2=40;let mn=Math.min(...v),mx=Math.max(...v);if(mx-mn<1000){const c=(mx+mn)/2;mn=c-500;mx=c+500}
 const n=Math.max(v.length-1,1),pts=v.map((a,i)=>[i/n*W2,H2-3-(a-mn)/(mx-mn)*(H2-6)]);if(pts.length===1)pts.unshift([0,pts[0][1]]);
 const dl='M'+pts.map(q=>q[0].toFixed(1)+' '+q[1].toFixed(1)).join('L');$('pPath').setAttribute('d',dl);$('pArea').setAttribute('d',dl+'L'+W2+' '+H2+'L0 '+H2+'Z');
 $('pSvg').style.color=altColor(p.alt);
 $('pProfLabel').textContent="Profil d'altitude · "+num(Math.round(mn/100)*100)+' à '+num(Math.round(mx/100)*100)+' ft';
}
function openPanel(p){selected=p;$('list').hidden=true;$('panel').hidden=false;$('hint').style.opacity=0;
 $('pCall').textContent=p.call;$('pLine').textContent=[p.flight,p.airline].filter(Boolean).join(' · ');
 const hasR=p.from&&p.to&&p.from!==p.to;
 const gaL=!p.live&&p.kind==='ga';$('pFrom').textContent=hasR?p.from:(gaL?'LFGB':'—');$('pTo').textContent=hasR?p.to:(gaL?'LFGB':'—');if(p.live)wantRoute(p);
 $('pFromN').textContent=hasR?ROUTE_NAMES(p.from):(p.live?'Route inconnue':p.kind==='ga'?'Vol local':'Route inconnue');$('pToN').textContent=hasR?ROUTE_NAMES(p.to):'';
 $('dReg').textContent=p.reg;$('dType').textContent=p.type;$('dSq').textContent=p.sq;$('dHex').textContent=p.hex;updatePanel()}
function closePanel(){selected=null;$('panel').hidden=true}
let tt=0;function toast(s){const t=$('toast');t.textContent=s;t.hidden=false;clearTimeout(tt);tt=setTimeout(()=>t.hidden=true,3500)}
function renderList(){const r=$('rows');const v=planes.filter(p=>p.visible).sort((a,b)=>dist(a.lat,a.lon,MUL[0],MUL[1])-dist(b.lat,b.lon,MUL[0],MUL[1]));
 r.replaceChildren(...v.map(p=>{const b=document.createElement('button');b.type='button';b.className='row';
  const hasR=p.from&&p.to&&p.from!==p.to;
  b.innerHTML='<em></em><b></b><u></u><small></small>';b.children[0].style.background=altColor(p.alt);b.children[1].textContent=p.call;b.children[2].textContent=p.alt>=8000?'FL'+String(Math.round(p.alt/100)).padStart(3,'0'):num(Math.round(p.alt/100)*100)+' ft';
  b.children[3].textContent=(hasR?p.from+' → '+p.to:p.live?p.type:p.type+' · '+(p.kind==='ga'?'vol local':'hélico'))+' · '+Math.round(dist(p.lat,p.lon,MUL[0],MUL[1]))+' km';
  b.addEventListener('click',()=>openPanel(p));return b}))}

/* ---------- map interactions ---------- */
cv.addEventListener('click',e=>{if(!wifi.connected)return;const rc=cv.getBoundingClientRect(),x=(e.clientX-rc.left)/rc.width*W,y=(e.clientY-rc.top)/rc.height*H;
 let best=null,bd=30;for(const p of planes){if(!p.visible)continue;let d=Math.hypot(X(p.lon)-x,Y(p.lat)-y);const b=p.lw?labelBox(p,p.labelPos):null;if(b&&x>=b.x&&x<=b.x+b.w&&y>=b.y&&y<=b.y+b.h)d=Math.min(d,8);if(d<bd){bd=d;best=p}}
 if(best)openPanel(best);else{closePanel();$('list').hidden=true}});
$('pClose').addEventListener('click',closePanel);
$('lClose').addEventListener('click',()=>$('list').hidden=true);
$('btnList').addEventListener('click',()=>{const l=$('list');if(l.hidden){closePanel();renderList();l.hidden=false;$('hint').style.opacity=0}else l.hidden=true});

/* ---------- Wi-Fi ---------- */
const wifi={connected:true,current:'Maison_2G',ip:'192.168.1.42',dbm:-54};
let nets=[['Maison_2G',-54,1],['Freebox-7A3C1E',-61,1],['Livebox-21F0',-68,1],['SFR_BOX_8842',-73,1],['Bbox-4D21A9',-79,1],['Invités',-82,0],['Imprimante-HP-3320',-86,0]];
const lvl=d=>d>-58?4:d>-68?3:d>-78?2:1;
function barsEl(d){const n=lvl(d);return'<span class="bars" aria-label="Signal '+n+' sur 4">'+[6,9,12,16].map((h,i)=>'<i class="'+(i<n?'on':'')+'" style="height:'+h+'px"></i>').join('')+'</span>'}
const LOCK='<svg class="i" viewBox="0 0 24 24" style="width:15px;height:15px;color:var(--s-dim);flex:none"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg>';
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML}
function renderNets(){const box=$('nets');box.innerHTML='';
 nets.slice().sort((a,b)=>(b[0]===wifi.current&&wifi.connected)-(a[0]===wifi.current&&wifi.connected)||b[1]-a[1]).forEach(n=>{const on=wifi.connected&&wifi.current===n[0],b=document.createElement('button');b.type='button';b.className='net';
  b.innerHTML=barsEl(n[1])+'<span class="nm">'+esc(n[0])+(on?'<small class="on">Connecté</small>':'<small>'+(n[2]?'Sécurisé (WPA2)':'Ouvert')+'</small>')+'</span>'+(n[2]?LOCK:'');
  b.addEventListener('click',()=>{if(on)return;if(n[2])openPw(n[0]);else connect(n[0])});box.appendChild(b)});
 const s=$('side');
 if(wifi.connected){s.innerHTML='<h3>Connexion</h3><div class="big">'+esc(wifi.current)+'</div><dl><dt>Adresse IP</dt><dd>'+wifi.ip+'</dd><dt>Signal</dt><dd>'+wifi.dbm+' dBm</dd><dt>Bande</dt><dd>2,4 GHz</dd></dl><p>Seuls les réseaux 2,4 GHz sont visibles.</p><button class="ghost push" id="forgetBtn" type="button">Oublier ce réseau</button>';$('forgetBtn').addEventListener('click',()=>{wifi.connected=false;closePanel();$('list').hidden=true;$('offline').hidden=false;setWifiIcon();renderNets()})}
 else s.innerHTML='<h3>Connexion</h3><div class="big">Non connecté</div><p>Choisissez un réseau dans la liste. Le mot de passe restera enregistré sur l\'appareil.</p><p>Seuls les réseaux 2,4 GHz sont visibles.</p>'}
function setWifiIcon(){$('wifiIcon').style.color=wifi.connected?'':'var(--s-err)'}
function scan(){$('nets').innerHTML='<div class="scan"><span class="spin"></span>Recherche des réseaux…</div>';setTimeout(()=>{nets=nets.map(n=>[n[0],Math.max(-90,Math.min(-48,n[1]+Math.round(rnd(-3,3)))),n[2]]);renderNets()},1100)}
$('btnWifi').addEventListener('click',openSettings);$('offBtn').addEventListener('click',openSettings);
function openSettings(){$('settings').hidden=false;scan()}
$('setBack').addEventListener('click',()=>$('settings').hidden=true);$('scanBtn').addEventListener('click',scan);

const ROWS={a:['azertyuiop','qsdfghjklm','⇧wxcvbn\'⌫','⇄ .'],n:['1234567890','@#&_-+()/*','⇧!?,:;"=⌫','⇄ .']};
const pw={net:'',val:'',show:false,mode:'a',shift:false};
function renderPw(){const v=$('pwVal');v.classList.toggle('empty',!pw.val);v.textContent=pw.val?(pw.show?pw.val:'•'.repeat(pw.val.length)):'Mot de passe';
 const g=pw.val.length>=8;$('pwGo').disabled=!g;const m=$('pwMsg');m.textContent=pw.val.length?(g?pw.val.length+' caractères':'Encore '+(8-pw.val.length)+' caractère'+(8-pw.val.length>1?'s':'')+' (8 minimum)'):'8 caractères minimum';m.className=''}
function renderKbd(){const K=$('kbd');K.innerHTML='';ROWS[pw.mode].forEach((row,ri)=>{const r=document.createElement('div');r.className='kr';
 [...row].forEach(ch=>{const b=document.createElement('button');b.type='button';b.className='k';
  if(ch==='⇧'){b.textContent='⇧';b.classList.add('w15');if(pw.shift)b.classList.add('on');b.setAttribute('aria-label','Majuscule');b.onclick=()=>{pw.shift=!pw.shift;renderKbd()}}
  else if(ch==='⌫'){b.textContent='⌫';b.classList.add('w15');b.setAttribute('aria-label','Effacer');b.onclick=()=>{pw.val=pw.val.slice(0,-1);renderPw()}}
  else if(ch==='⇄'){b.textContent=pw.mode==='a'?'123':'ABC';b.classList.add('w15');b.onclick=()=>{pw.mode=pw.mode==='a'?'n':'a';renderKbd()}}
  else if(ch===' '){b.textContent='espace';b.classList.add('w5');b.style.fontSize='13px';b.onclick=()=>add(' ')}
  else{const c=pw.shift?ch.toUpperCase():ch;b.textContent=c;b.onclick=()=>{add(c);if(pw.shift){pw.shift=false;renderKbd()}}}
  r.appendChild(b)});K.appendChild(r)})}
function add(c){if(pw.val.length<63){pw.val+=c;renderPw()}}
function openPw(n){pw.net=n;pw.val='';pw.show=false;pw.mode='a';pw.shift=false;$('pwTitle').textContent=n;$('pw').hidden=false;$('busy').hidden=true;renderPw();renderKbd()}
$('pwBack').addEventListener('click',()=>$('pw').hidden=true);
$('pwEye').addEventListener('click',()=>{pw.show=!pw.show;renderPw()});
$('pwGo').addEventListener('click',()=>connect(pw.net));
document.addEventListener('keydown',e=>{if($('pw').hidden||!$('busy').hidden)return;if(e.ctrlKey||e.metaKey||e.altKey)return;
 if(e.key==='Backspace'){pw.val=pw.val.slice(0,-1);renderPw();e.preventDefault()}
 else if(e.key==='Enter'){if(document.activeElement&&document.activeElement.tagName==='BUTTON'&&document.activeElement.closest('#pw'))return;if(!$('pwGo').disabled)connect(pw.net)}
 else if(e.key==='Escape')$('pw').hidden=true;
 else if(e.key.length===1&&!(e.key===' '&&document.activeElement&&document.activeElement.tagName==='BUTTON')){add(e.key);e.preventDefault()}});
function connect(n){const onPw=!$('pw').hidden;if(!onPw){$('pw').hidden=false;$('pwTitle').textContent=n;renderPw();renderKbd()}
 $('busyT').textContent='Connexion à '+n+'…';$('busy').hidden=false;
 setTimeout(()=>{const net=nets.find(x=>x[0]===n);wifi.connected=true;wifi.current=n;wifi.dbm=net?net[1]:-60;wifi.ip='192.168.1.'+Math.floor(rnd(20,200));
  $('pw').hidden=true;$('busy').hidden=true;$('offline').hidden=true;$('settings').hidden=true;setWifiIcon();renderNets();toast('Connecté à '+n)},1500)}

/* ---------- outer controls ---------- */
let speed=1,sizeMode='fit';
function seg(id,fn){$(id).addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;[...$(id).children].forEach(x=>x.setAttribute('aria-pressed',x===b));fn(b.dataset.v)})}
seg('segSpeed',v=>speed=+v);seg('segSize',v=>{sizeMode=v;layout()});
const REAL=d=>d*96*Math.cos(Math.atan2(H,W))/W; /* diagonal in inches, at 96 CSS dpi */
function layout(){const st=$('stage').clientWidth,pad=28;let s=Math.min((st-pad)/W,Math.max(.4,(window.innerHeight-250)/H),1.4);
 if(sizeMode!=='fit')s=Math.min(REAL(+sizeMode),(st-pad)/W);s=Math.max(.2,s);
 const sc=$('screen'),g=$('glass');sc.style.transform='scale('+s+')';g.style.width=W*s+'px';g.style.height=H*s+'px';
 const dpr=window.devicePixelRatio||1,nr=Math.min(3,Math.max(1,s*dpr));
 if(Math.abs(nr-RS)>.01||cv.width!==Math.round(W*nr)){RS=nr;cv.width=Math.round(W*RS);cv.height=Math.round(H*RS);drawBase();draw()}}
window.addEventListener('resize',layout);

/* ---------- live data ---------- */
let MODE='sim',lastOk=0,polling=false,source='';
const routeCache={},routeQ=[];let routeBusy=false;
function applyRoute(p){const r=routeCache[p.call];if(!r)return;AP[r.from.iata]=[r.from.city,r.from.lat,r.from.lon];AP[r.to.iata]=[r.to.city,r.to.lat,r.to.lon];p.from=r.from.iata;p.to=r.to.iata;if(r.flight)p.flight=r.flight;if(r.airline)p.airline=r.airline}
function wantRoute(p){if(MODE!=='live'||!/^[A-Z0-9]{2,8}$/.test(p.call)||p.call===p.reg.replace('-',''))return;if(p.call in routeCache){applyRoute(p);return}if(!routeQ.includes(p.call))routeQ.push(p.call);pumpRoutes()}
async function pumpRoutes(){if(routeBusy)return;routeBusy=true;while(routeQ.length){const call=routeQ.shift();try{const d=await(await fetch('/api/route/'+call)).json();if(!d.retry)routeCache[call]=d.route;for(const p of planes)if(p.call===call)applyRoute(p);if(selected&&selected.call===call)openPanel(selected);if(!$('list').hidden)renderList()}catch(e){}await new Promise(r=>setTimeout(r,400))}routeBusy=false}
function setChip(cls,txt){const c=$('chip');c.className='sim '+cls;c.textContent=txt}
function applyLive(d){const now=performance.now(),seen=new Set();source=d.source;
 for(const a of d.ac){if(a.lat==null||a.lon==null||a.alt_baro==='ground'||(a.seen_pos||0)>30||!a.hex)continue;seen.add(a.hex);
  let p=planes.find(q=>q.hex===a.hex.toUpperCase());const alt=typeof a.alt_baro==='number'?a.alt_baro:(a.alt_geom||0);
  if(!p){const cat=a.category||'',call=(a.flight||'').trim()||a.r||a.hex.toUpperCase();
   p=mk({live:true,path:[],hex:a.hex.toUpperCase(),call,flight:a.desc||'',airline:a.ownOp||'',reg:a.r||'—',type:a.t||'—',sq:a.squawk||'—',from:'',to:'',lat:a.lat,lon:a.lon,alt,gs:a.gs||0,trk:a.track??a.true_heading??0,kind:(cat==='A7'?'heli':cat==='A1'||cat==='A2'||cat==='B1'?'ga':'over'),lastProf:0,dlat:0,dlon:0});planes.push(p);if(routeCache[p.call])applyRoute(p)}
  else{const dl=a.lat-p.lat,dn=a.lon-p.lon;if(Math.hypot(dl*KM_LAT,dn*KM_LON)>5){p.lat=a.lat;p.lon=a.lon;p.dlat=p.dlon=0}else{p.dlat=dl;p.dlon=dn}}
  p.alt=alt;p.gs=a.gs??p.gs;p.trk=a.track??a.true_heading??p.trk;p.vs=a.baro_rate??a.geom_rate??0;p.sq=a.squawk||p.sq;p.t=now;p.seenPos=a.seen_pos||0;
  const last=p.trail[p.trail.length-1];if(!last||dist(last[0],last[1],a.lat,a.lon)>.8){p.trail.push([a.lat,a.lon,alt]);if(p.trail.length>40)p.trail.shift()}
  if(now-p.lastProf>15000){p.lastProf=now;p.prof.push(alt);if(p.prof.length>80)p.prof.shift()}}
 dropLive(p=>!seen.has(p.hex)&&now-p.t>30000);
 for(const p of planes){p.visible=visibleXY(p);if(p.visible)wantRoute(p)}}
const visibleXY=p=>{const x=X(p.lon),y=Y(p.lat);return x>-20&&x<W+20&&y>TOP-20&&y<H+20};
function dropLive(test){planes=planes.filter(p=>{if(!test(p))return true;if(selected===p){closePanel();toast(p.call+' a quitté la zone')}return false})}
function stepLive(dt){const now=performance.now();for(const p of planes){const c=Math.min(1,dt/2);p.lat+=p.dlat*c;p.lon+=p.dlon*c;p.dlat*=1-c;p.dlon*=1-c;
  if((now-p.t)/1000+p.seenPos<20){const r=p.trk*Math.PI/180,km=p.gs*0.514444/1000*dt;p.lat+=km*Math.cos(r)/KM_LAT;p.lon+=km*Math.sin(r)/KM_LON}
  const was=p.visible;p.visible=visibleXY(p);if(was&&!p.visible&&selected===p){closePanel();toast(p.call+' a quitté la zone')}}}
async function poll(){if(polling)return false;polling=true;try{const r=await fetch('/api/aircraft');if(!r.ok)throw 0;const d=await r.json();if(!Array.isArray(d.ac))throw 0;if(MODE==='live'||MODE==='probe'){MODE='live';applyLive(d)}lastOk=Date.now();setChip('live','EN DIRECT');polling=false;return true}catch(e){polling=false;if(MODE==='live'){setChip('down','API INJOIGNABLE');dropLive(p=>performance.now()-p.t>30000)}return false}}
async function startLive(){const ok=await(MODE='probe',poll());if(!ok){MODE='sim';warm();draw();ui();return}
 $('ctlSpeed').hidden=true;$('subtitle').textContent="Écran tactile 800×480 alimenté par de vrais avions.";
 $('nReal').textContent="Vols, positions, altitudes et vitesses reçus par ADS-B, ainsi que la carte. Les routes viennent d'adsbdb d'après l'indicatif et peuvent être fausses ou absentes.";
 $('nSim').textContent="Seul le menu Wi-Fi reste simulé, car un navigateur ne peut pas scanner les réseaux. N'importe quel mot de passe de 8 caractères fonctionne.";
 $('hint').textContent='Touchez un avion pour voir ses données';ui();setInterval(poll,5000)}

/* ---------- loop ---------- */
function warm(){let t=0;nextOver=0;nextArr=760;nextDep=850;nextGA=0;while(t<900){sim(2);t+=2}for(const p of planes)p.labelPos=0;selected=null}
const WANT_LIVE=location.protocol==='http:';
if(!WANT_LIVE)warm();
const REDUCED=window.matchMedia&&matchMedia('(prefers-reduced-motion:reduce)').matches;
let last=performance.now(),lastUi=0,lastDraw=0;
function ui(now){const v=planes.filter(p=>p.visible).length;$('age').textContent=MODE==='live'&&lastOk?source+' · il y a '+Math.round((Date.now()-lastOk)/1000)+' s':'';$('count').textContent=v+(v>1?' avions':' avion');
 const d=new Date();$('clock').textContent=String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');
 updatePanel();if(!$('list').hidden&&!$('rows').matches(':active'))renderList()}
function loop(now){const dt=Math.min(.25,(now-last)/1000);last=now;
 if(MODE==='probe'){}else if(MODE==='live'){stepLive(dt);if(!REDUCED||now-lastDraw>5000){lastDraw=now;draw()}}else if(REDUCED){if(now-lastDraw>5000){sim(5*speed);lastDraw=now;draw()}}else{sim(dt*speed);draw()}
 if(now-lastUi>1000){lastUi=now;ui(now)}
 requestAnimationFrame(loop)}
layout();drawBase();draw();ui(0);
if(document.fonts&&document.fonts.ready)document.fonts.ready.then(()=>{drawBase();draw()});
setTimeout(()=>{$('hint').style.opacity=0},9000);
if(WANT_LIVE)startLive();
requestAnimationFrame(loop);
})();
</script>
</body></html>'''


def main():
    ap = argparse.ArgumentParser(description="Live Mulhouse-Basel radar")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    url = f"http://{'127.0.0.1' if a.host == '0.0.0.0' else a.host}:{a.port}/"
    print(f"Live radar at {url} (Ctrl+C to stop)", flush=True)
    if not a.no_browser:
        threading.Timer(0.4, webbrowser.open, [url]).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
