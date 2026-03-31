/*
  Project: BroCommand - ESP-01 LED Driver
  Version: 1.0 (Final)

  Developed by: MD Saimum Hassan Arafat
  AKA: Saimum / BroCommand
  E-mail: saimumhassan26@gmail.com

  Description:
    LED control system for ESP-01 with multiple lighting modes:
      1) Solid Color (all LEDs: fan + cooler)
      2) Chaser (FAN ONLY: 6 LEDs)
      3) Mirage/Strobe (all LEDs: fan + cooler)

  Features:
    - Web UI control panel
    - Mode selection system
    - EEPROM save (persistent config)
    - Real-time state sync via /state endpoint

  UI:
    - Heading: BroCommand
    - 3 divisions (Solid / Chaser / Mirage)
    - One Active Mode selector
    - Save Config button

  Network:
    SSID: (Your WiFi Name)
    PASS: (Your WiFi Password)
    Static IP: 192.168.10.50

  Hardware:
    - ESP-01 (ESP8266)
    - WS2812 LEDs (GRB)
    - TOTAL_LEDS = 14
    - FAN_LEDS   = 6
    - Data Pin   = GPIO2

  License:
    © 2025 MD Saimum Hassan Arafat. All rights reserved.

  Note:
    Unauthorized redistribution(selling) is not permitted without permission.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ---------------- WiFi ----------------
const char* SSID     = "My Wifi";
const char* PASSWORD = "abc123xyz";

IPAddress localIP(192, 168, 10, 50); // Note: Ensure this IP is outside your router's DHCP range to avoid conflicts
IPAddress gateway(192, 168, 10, 1); // default gateway (usually your router's IP)
IPAddress subnet(255, 255, 255, 0); // dont change this for a typical home network
IPAddress dns(192, 168, 10, 1); // DNS server (usually your router's IP) aslo dont change this for a typical home network

// ---------------- LEDs ----------------
#define LED_PIN    2 // GPIO2 (D4 on NodeMCU, ESP-01S) - Ensure this pin is connected to the data line of the first LED in the chain
#define TOTAL_LEDS 14 // in my fan I have 6 LEDs for the fan and 8 for the cooler, all in series (total 14)
#define FAN_LEDS   6 // these are the argb leds under the fan blades, the other 8 are beside the liquid pump

Adafruit_NeoPixel strip(TOTAL_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------------- Web ----------------
ESP8266WebServer server(80);

// ---------------- Modes ----------------
enum MainMode : uint8_t { MODE_SOLID = 0, MODE_CHASER = 1, MODE_MIRAGE = 2 };
enum ChaseMode : uint8_t { CHASE_SINGLE = 0, CHASE_DUAL_HALF = 1 };

// ---------------- Active State ----------------
MainMode mainMode = MODE_SOLID;

// Solid (all LEDs)
uint8_t solidR = 255, solidG = 0, solidB = 0;
uint8_t solidBrightness = 255; // 0..255

// Chaser (fan only)
ChaseMode chaseMode = CHASE_SINGLE;
uint8_t chaseSingleR = 255, chaseSingleG = 0, chaseSingleB = 0;
uint8_t chaseAR = 255, chaseAG = 0, chaseAB = 0;
uint8_t chaseBR = 0,   chaseBG = 0, chaseBB = 255;
uint8_t chaseBrightness = 80;      // 0..255
uint16_t chaseDelayMs = 120;       // ms/step
int8_t chaseDir = +1;              // +1 / -1
uint16_t chasePos = 0;             // 0..FAN_LEDS-1
uint32_t chaseLastStepMs = 0;

// Mirage (all LEDs)
uint8_t mirageR = 255, mirageG = 0, mirageB = 0;
uint16_t mirageHz = 0;             // 0..1300
bool mirageOnPhase = false;
uint32_t mirageLastToggleUs = 0;
uint32_t frameOn[TOTAL_LEDS];
uint32_t frameOff[TOTAL_LEDS];

// Dirty flag (apply changes outside HTTP handler)
volatile bool dirty = true;

// ---------------- EEPROM ----------------
static const uint32_t CFG_MAGIC = 0xB0C0AA01;
static const uint16_t EEPROM_SIZE = 1024;

struct Config {
  uint32_t magic;
  uint8_t version;

  uint8_t mainMode;

  // solid
  uint8_t solidR, solidG, solidB;
  uint8_t solidBrightness;

  // chaser
  uint8_t chaseMode;
  int8_t  chaseDir;
  uint16_t chaseDelayMs;
  uint8_t chaseBrightness;
  uint8_t chaseSingleR, chaseSingleG, chaseSingleB;
  uint8_t chaseAR, chaseAG, chaseAB;
  uint8_t chaseBR, chaseBG, chaseBB;

  // mirage
  uint16_t mirageHz;
  uint8_t  mirageR, mirageG, mirageB;

  uint8_t pad[16];

  uint16_t crc;
};

static uint16_t simpleCrc16(const uint8_t* data, size_t n) {
  uint16_t c = 0xA5A5;
  for (size_t i = 0; i < n; i++) c = (uint16_t)((c << 5) ^ (c >> 11) ^ data[i]);
  return c;
}

// ---------------- Helpers ----------------
static bool parseHexColor(const String& s, uint8_t &r, uint8_t &g, uint8_t &bl) {
  int idx = (s.length() == 7 && s[0] == '#') ? 1 : 0;
  if ((int)s.length() - idx != 6) return false;

  auto hx = [](char c)->int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  auto hex2 = [&](char a, char b)->int {
    int hi = hx(a), lo = hx(b);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
  };

  int rr = hex2(s[idx+0], s[idx+1]);
  int gg = hex2(s[idx+2], s[idx+3]);
  int bb = hex2(s[idx+4], s[idx+5]);
  if (rr < 0 || gg < 0 || bb < 0) return false;

  r = (uint8_t)rr; g = (uint8_t)gg; bl = (uint8_t)bb;
  return true;
}

static inline uint16_t wrapFan(int32_t i) {
  while (i < 0) i += FAN_LEDS;
  while (i >= (int32_t)FAN_LEDS) i -= FAN_LEDS;
  return (uint16_t)i;
}

static inline uint32_t packScaled(uint8_t r, uint8_t g, uint8_t b, uint8_t br) {
  uint8_t rr = (uint8_t)((uint16_t)r * br / 255);
  uint8_t gg = (uint8_t)((uint16_t)g * br / 255);
  uint8_t bb = (uint8_t)((uint16_t)b * br / 255);
  return strip.Color(rr, gg, bb);
}

static void buildMirageFrames() {
  uint32_t cOn = strip.Color(mirageR, mirageG, mirageB);
  for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
    frameOn[i]  = cOn;
    frameOff[i] = 0;
  }
}

static void showFrame(const uint32_t* frame) {
  for (uint16_t i = 0; i < TOTAL_LEDS; i++) strip.setPixelColor(i, frame[i]);
  strip.show();
}

// ---------------- Renderers ----------------
static void renderSolid() {
  uint32_t c = packScaled(solidR, solidG, solidB, solidBrightness);
  for (uint16_t i = 0; i < TOTAL_LEDS; i++) strip.setPixelColor(i, c);
  strip.show();
}

static void renderChaserFrame() {
  // Chaser affects ONLY fan LEDs (0..5). Ensure cooler (6..13) is OFF.
  for (uint16_t i = FAN_LEDS; i < TOTAL_LEDS; i++) strip.setPixelColor(i, 0);

  if (chaseMode == CHASE_SINGLE) {
    for (uint16_t i = 0; i < FAN_LEDS; i++) strip.setPixelColor(i, 0);
    uint32_t c = packScaled(chaseSingleR, chaseSingleG, chaseSingleB, chaseBrightness);
    strip.setPixelColor(wrapFan(chasePos), c);
    strip.show();
    return;
  }

  // Dual half-ring on FAN only (6 LEDs => 3/3), rotating boundary
  const uint16_t halfA = FAN_LEDS / 2; // 3
  uint32_t cA = packScaled(chaseAR, chaseAG, chaseAB, chaseBrightness);
  uint32_t cB = packScaled(chaseBR, chaseBG, chaseBB, chaseBrightness);

  uint16_t base = wrapFan(chasePos);
  for (uint16_t k = 0; k < FAN_LEDS; k++) {
    uint16_t idx = (uint16_t)((base + k) % FAN_LEDS);
    strip.setPixelColor(idx, (k < halfA) ? cA : cB);
  }
  strip.show();
}

static void renderMirageImmediate() {
  if (mirageHz == 0) {
    mirageOnPhase = false;
    showFrame(frameOff);
  } else {
    showFrame(mirageOnPhase ? frameOn : frameOff);
  }
}

static void renderActiveNow() {
  if (mainMode == MODE_SOLID) renderSolid();
  else if (mainMode == MODE_CHASER) renderChaserFrame();
  else renderMirageImmediate();
}

// ---------------- EEPROM load/save ----------------
static void loadConfig() {
  Config cfg;
  EEPROM.get(0, cfg);

  uint16_t want = simpleCrc16((const uint8_t*)&cfg, sizeof(Config) - sizeof(cfg.crc));
  if (cfg.magic != CFG_MAGIC || cfg.version != 1 || cfg.crc != want) {
    // Defaults
    mainMode = MODE_SOLID;

    solidR = 255; solidG = 0; solidB = 0;
    solidBrightness = 255;

    chaseMode = CHASE_SINGLE;
    chaseDir = +1;
    chaseDelayMs = 120;
    chaseBrightness = 80;
    chaseSingleR = 255; chaseSingleG = 0; chaseSingleB = 0;
    chaseAR = 255; chaseAG = 0; chaseAB = 0;
    chaseBR = 0; chaseBG = 0; chaseBB = 255;
    chasePos = 0;

    mirageHz = 0;
    mirageR = 255; mirageG = 0; mirageB = 0;

    buildMirageFrames();
    dirty = true;
    return;
  }

  mainMode = (MainMode)cfg.mainMode;

  solidR = cfg.solidR; solidG = cfg.solidG; solidB = cfg.solidB;
  solidBrightness = cfg.solidBrightness;

  chaseMode = (ChaseMode)cfg.chaseMode;
  chaseDir = cfg.chaseDir;
  chaseDelayMs = cfg.chaseDelayMs;
  chaseBrightness = cfg.chaseBrightness;
  chaseSingleR = cfg.chaseSingleR; chaseSingleG = cfg.chaseSingleG; chaseSingleB = cfg.chaseSingleB;
  chaseAR = cfg.chaseAR; chaseAG = cfg.chaseAG; chaseAB = cfg.chaseAB;
  chaseBR = cfg.chaseBR; chaseBG = cfg.chaseBG; chaseBB = cfg.chaseBB;
  chasePos = 0;

  mirageHz = cfg.mirageHz;
  mirageR = cfg.mirageR; mirageG = cfg.mirageG; mirageB = cfg.mirageB;

  buildMirageFrames();
  dirty = true;
}

static void saveConfig() {
  Config cfg{};
  cfg.magic = CFG_MAGIC;
  cfg.version = 1;

  cfg.mainMode = (uint8_t)mainMode;

  cfg.solidR = solidR; cfg.solidG = solidG; cfg.solidB = solidB;
  cfg.solidBrightness = solidBrightness;

  cfg.chaseMode = (uint8_t)chaseMode;
  cfg.chaseDir = chaseDir;
  cfg.chaseDelayMs = chaseDelayMs;
  cfg.chaseBrightness = chaseBrightness;
  cfg.chaseSingleR = chaseSingleR; cfg.chaseSingleG = chaseSingleG; cfg.chaseSingleB = chaseSingleB;
  cfg.chaseAR = chaseAR; cfg.chaseAG = chaseAG; cfg.chaseAB = chaseAB;
  cfg.chaseBR = chaseBR; cfg.chaseBG = chaseBG; cfg.chaseBB = chaseBB;

  cfg.mirageHz = mirageHz;
  cfg.mirageR = mirageR; cfg.mirageG = mirageG; cfg.mirageB = mirageB;

  cfg.crc = simpleCrc16((const uint8_t*)&cfg, sizeof(Config) - sizeof(cfg.crc));

  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------- State JSON ----------------
static void handleStateJson() {
  String j;
  j.reserve(520);
  j += "{";

  j += "\"mainMode\":"; j += (int)mainMode; j += ",";

  char buf[8];

  snprintf(buf, sizeof(buf), "%02X%02X%02X", solidR, solidG, solidB);
  j += "\"solid\":\""; j += buf; j += "\",";
  j += "\"solidBri\":"; j += (int)solidBrightness; j += ",";

  j += "\"chaseMode\":"; j += (int)chaseMode; j += ",";
  j += "\"chaseDir\":"; j += (int)chaseDir; j += ",";
  j += "\"chaseSpeed\":"; j += (int)chaseDelayMs; j += ",";
  j += "\"chaseBri\":"; j += (int)chaseBrightness; j += ",";

  snprintf(buf, sizeof(buf), "%02X%02X%02X", chaseSingleR, chaseSingleG, chaseSingleB);
  j += "\"chSingle\":\""; j += buf; j += "\",";
  snprintf(buf, sizeof(buf), "%02X%02X%02X", chaseAR, chaseAG, chaseAB);
  j += "\"chA\":\""; j += buf; j += "\",";
  snprintf(buf, sizeof(buf), "%02X%02X%02X", chaseBR, chaseBG, chaseBB);
  j += "\"chB\":\""; j += buf; j += "\",";

  j += "\"mirageHz\":"; j += (int)mirageHz; j += ",";
  snprintf(buf, sizeof(buf), "%02X%02X%02X", mirageR, mirageG, mirageB);
  j += "\"mirage\":\""; j += buf; j += "\"";

  j += "}";

  server.send(200, "application/json", j);
}

// ---------------- UI ----------------
static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BroCommand</title>
<style>
  :root{
    --bg:#0b0c10; --card:#11131a; --card2:#0f1117; --txt:#e9eef6; --mut:#9aa4b2;
    --b:#232634; --acc:#6ae4ff;
  }
  body{margin:0;font-family:system-ui,Arial;background:var(--bg);color:var(--txt)}
  .wrap{max-width:1040px;margin:0 auto;padding:18px}
  .top{display:flex;align-items:center;justify-content:space-between;gap:12px}
  h1{margin:0;font-size:22px;letter-spacing:.2px}
  .btn{padding:10px 12px;border-radius:12px;border:1px solid var(--b);background:#151827;color:var(--txt);cursor:pointer}
  .btn:hover{border-color:#2f3350}
  .grid{display:grid;grid-template-columns:1fr;gap:14px;margin-top:14px}
  @media(min-width:920px){ .grid{grid-template-columns:1fr 1fr 1fr} }
  .card{background:linear-gradient(180deg,var(--card),var(--card2));border:1px solid var(--b);
        border-radius:16px;padding:14px}
  .head{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
  .title{font-weight:700}
  .mut{color:var(--mut);font-family:ui-monospace, Menlo, monospace; font-size:12px}
  .modeSel{display:flex;gap:10px;align-items:center;color:var(--mut);font-size:12px;flex-wrap:wrap}
  .pill{display:flex;gap:8px;align-items:center;padding:8px 10px;border-radius:14px;border:1px solid var(--b);background:#0f1220}
  input[type=color]{width:100%;height:56px;border:none;background:none;padding:0}
  input[type=range]{width:100%}
  input[type=number]{width:120px;padding:8px 10px;border-radius:12px;border:1px solid var(--b);
                     background:#0b0d16;color:var(--txt)}
  .row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px}
  .seg{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
  .smallbtn{padding:8px 10px;border-radius:12px;border:1px solid var(--b);background:#0f1220;color:var(--txt);cursor:pointer}
  .smallbtn:hover{border-color:#2f3350}
  .kv{display:flex;gap:10px;align-items:center;justify-content:space-between;margin-top:10px}
  .accent{color:var(--acc)}
</style>
</head><body>
<div class="wrap">
  <div class="top">
    <h1>BroCommand</h1>
    <button class="btn" onclick="saveCfg()">Save Config</button>
  </div>

  <div class="modeSel" style="margin-top:12px">
    <span class="mut">Active Mode:</span>
    <label class="pill"><input id="mSolid" type="radio" name="main" value="solid" onchange="setMain('solid')"> Solid</label>
    <label class="pill"><input id="mChaser" type="radio" name="main" value="chaser" onchange="setMain('chaser')"> Chaser</label>
    <label class="pill"><input id="mMirage" type="radio" name="main" value="mirage" onchange="setMain('mirage')"> Mirage</label>
    <span class="mut" id="status"></span>
  </div>

  <div class="grid">

    <div class="card">
      <div class="head">
        <div>
          <div class="title">Simple Color</div>
          <div class="mut">All LEDs (14)</div>
        </div>
      </div>
      <input id="solidColor" type="color">
      <div class="kv">
        <span class="mut">Brightness</span><span class="mut"><span id="solidBriV" class="accent"></span></span>
      </div>
      <input id="solidBri" type="range" min="0" max="255">
    </div>

    <div class="card">
      <div class="head">
        <div>
          <div class="title">Chaser</div>
          <div class="mut">Fan only (6)</div>
        </div>
      </div>

      <div class="seg">
        <label class="pill"><input id="chmSingle" type="radio" name="chm" value="single" onchange="setChaseMode('single')"> Single</label>
        <label class="pill"><input id="chmDual" type="radio" name="chm" value="dual" onchange="setChaseMode('dual')"> Dual</label>
        <button class="smallbtn" id="dirBtn" onclick="toggleDir()">CW</button>
      </div>

      <div class="kv">
        <span class="mut">Speed</span><span class="mut"><span id="spdV" class="accent"></span> ms</span>
      </div>
      <input id="speed" type="range" min="20" max="800">

      <div class="kv">
        <span class="mut">Brightness</span><span class="mut"><span id="briV" class="accent"></span></span>
      </div>
      <input id="bri" type="range" min="0" max="255">

      <div class="row">
        <div style="flex:1">
          <div class="mut">Single Color</div>
          <input id="chSingle" type="color">
        </div>
      </div>

      <div class="row">
        <div style="flex:1">
          <div class="mut">Color A</div>
          <input id="chA" type="color">
        </div>
        <div style="flex:1">
          <div class="mut">Color B</div>
          <input id="chB" type="color">
        </div>
      </div>
    </div>

    <div class="card">
      <div class="head">
        <div>
          <div class="title">Mirage</div>
          <div class="mut">All LEDs (14) 0–1300 Hz</div>
        </div>
      </div>

      <div class="mut">Color</div>
      <input id="miColor" type="color">

      <div class="row">
        <div class="mut">Hz</div>
        <input id="miHzNum" type="number" min="0" max="400" step="1">
      </div>
      <input id="miHz" type="range" min="0" max="400">
    </div>

  </div>
</div>

<script>
  const statusEl = document.getElementById('status');

  const mSolid=document.getElementById('mSolid');
  const mChaser=document.getElementById('mChaser');
  const mMirage=document.getElementById('mMirage');

  const solidColor = document.getElementById('solidColor');
  const solidBri = document.getElementById('solidBri');
  const solidBriV = document.getElementById('solidBriV');

  const speed = document.getElementById('speed');
  const bri = document.getElementById('bri');
  const spdV = document.getElementById('spdV');
  const briV = document.getElementById('briV');

  const chSingle = document.getElementById('chSingle');
  const chA = document.getElementById('chA');
  const chB = document.getElementById('chB');
  const dirBtn = document.getElementById('dirBtn');
  const chmSingle = document.getElementById('chmSingle');
  const chmDual = document.getElementById('chmDual');

  const miColor = document.getElementById('miColor');
  const miHz = document.getElementById('miHz');
  const miHzNum = document.getElementById('miHzNum');

  let mainMode = 'solid';
  let chaseMode = 'single';
  let dir = 1;

  function updLabels(){
    spdV.textContent = speed.value;
    briV.textContent = bri.value;
    solidBriV.textContent = solidBri.value;
  }

  function clampHz(v){
    v = parseInt(v);
    if (isNaN(v)) v = 0;
    if (v < 0) v = 0;
    if (v > 400) v = 400;
    return v;
  }
  function syncHzFromSlider(){ miHzNum.value = miHz.value; }
  function syncHzFromNum(){
    const v = clampHz(miHzNum.value);
    miHzNum.value = v;
    miHz.value = v;
  }

  // ---- Request limiting (coalesce + hard throttle) ----
  let pendingQuery = null;
  let timer = null;
  const SEND_EVERY_MS = 80;

  function scheduleSend(params){
    pendingQuery = params.toString();
    if (timer) return;
    timer = setTimeout(async () => {
      const q = pendingQuery;
      pendingQuery = null;
      timer = null;
      try { await fetch('/set?' + q, {cache:'no-store'}); statusEl.textContent=''; }
      catch(e){ statusEl.textContent='offline'; }
      if (pendingQuery) scheduleSend(new URLSearchParams(pendingQuery));
    }, SEND_EVERY_MS);
  }

  function pushAll(){
    const p = new URLSearchParams();
    p.set('main', mainMode);

    p.set('solid', solidColor.value);
    p.set('solidBri', solidBri.value);

    p.set('chmode', chaseMode);
    p.set('dir', dir);
    p.set('speed', speed.value);
    p.set('bri', bri.value);
    p.set('chsingle', chSingle.value);
    p.set('cha', chA.value);
    p.set('chb', chB.value);

    p.set('mihz', miHz.value);
    p.set('micolor', miColor.value);

    scheduleSend(p);
  }

  function setMain(m){ mainMode = m; pushAll(); }
  function setChaseMode(m){ chaseMode = m; pushAll(); }

  function toggleDir(){
    dir = (dir === 1) ? -1 : 1;
    dirBtn.textContent = (dir === 1) ? 'CW' : 'CCW';
    pushAll();
  }

  async function saveCfg(){
    try{
      await fetch('/save', {cache:'no-store'});
      statusEl.textContent = 'saved';
      setTimeout(()=>statusEl.textContent='', 900);
    }catch(e){
      statusEl.textContent = 'save-fail';
    }
  }

  // ---- Events ----
  solidColor.addEventListener('input', pushAll);
  solidBri.addEventListener('input', ()=>{ updLabels(); pushAll(); });

  speed.addEventListener('input', ()=>{ updLabels(); pushAll(); });
  bri.addEventListener('input', ()=>{ updLabels(); pushAll(); });

  chSingle.addEventListener('input', pushAll);
  chA.addEventListener('input', pushAll);
  chB.addEventListener('input', pushAll);

  miColor.addEventListener('input', pushAll);
  miHz.addEventListener('input', ()=>{ syncHzFromSlider(); pushAll(); });
  miHzNum.addEventListener('change', ()=>{ syncHzFromNum(); pushAll(); });
  miHzNum.addEventListener('keydown', (e)=>{ if(e.key==='Enter'){ syncHzFromNum(); pushAll(); } });

  // ---- Load state from ESP ----
  function hex6ToCss(h){ return '#'+h.toLowerCase(); }

  async function loadState(){
    try{
      const r = await fetch('/state', {cache:'no-store'});
      const s = await r.json();

      if (s.mainMode === 0){ mainMode='solid'; mSolid.checked=true; }
      else if (s.mainMode === 1){ mainMode='chaser'; mChaser.checked=true; }
      else { mainMode='mirage'; mMirage.checked=true; }

      solidColor.value = hex6ToCss(s.solid);
      solidBri.value = s.solidBri;

      chaseMode = (s.chaseMode === 1) ? 'dual' : 'single';
      chmSingle.checked = (chaseMode==='single');
      chmDual.checked = (chaseMode==='dual');

      dir = (s.chaseDir < 0) ? -1 : 1;
      dirBtn.textContent = (dir === 1) ? 'CW' : 'CCW';

      speed.value = s.chaseSpeed;
      bri.value = s.chaseBri;
      chSingle.value = hex6ToCss(s.chSingle);
      chA.value = hex6ToCss(s.chA);
      chB.value = hex6ToCss(s.chB);

      miHz.value = s.mirageHz;
      miHzNum.value = s.mirageHz;
      miColor.value = hex6ToCss(s.mirage);

      updLabels();

      // sync once
      pushAll();
    }catch(e){
      statusEl.textContent = 'state-fail';
    }
  }

  dirBtn.textContent='CW';
  loadState();
</script>
<footer>
  Developed by Saimum (BroCommand)
</footer>
</body></html>
)HTML";

// ---------------- Handlers ----------------
void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleSet() {
  if (server.hasArg("main")) {
    String m = server.arg("main");
    if (m == "solid") mainMode = MODE_SOLID;
    else if (m == "chaser") mainMode = MODE_CHASER;
    else if (m == "mirage") mainMode = MODE_MIRAGE;
    dirty = true;
  }

  // Solid
  if (server.hasArg("solid")) {
    uint8_t rr, gg, bb;
    if (parseHexColor(server.arg("solid"), rr, gg, bb)) {
      solidR = rr; solidG = gg; solidB = bb;
      dirty = true;
    }
  }
  if (server.hasArg("solidBri")) {
    int v = server.arg("solidBri").toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    solidBrightness = (uint8_t)v;
    dirty = true;
  }

  // Chaser
  if (server.hasArg("chmode")) {
    String cm = server.arg("chmode");
    chaseMode = (cm == "dual") ? CHASE_DUAL_HALF : CHASE_SINGLE;
    dirty = true;
  }
  if (server.hasArg("dir")) {
    int v = server.arg("dir").toInt();
    chaseDir = (v < 0) ? -1 : +1;
    dirty = true;
  }
  if (server.hasArg("speed")) {
    int v = server.arg("speed").toInt();
    if (v < 10) v = 10;
    if (v > 2000) v = 2000;
    chaseDelayMs = (uint16_t)v;
    dirty = true;
  }
  if (server.hasArg("bri")) {
    int v = server.arg("bri").toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    chaseBrightness = (uint8_t)v;
    dirty = true;
  }

  if (server.hasArg("chsingle")) {
    uint8_t rr, gg, bb;
    if (parseHexColor(server.arg("chsingle"), rr, gg, bb)) {
      chaseSingleR = rr; chaseSingleG = gg; chaseSingleB = bb;
      dirty = true;
    }
  }
  if (server.hasArg("cha")) {
    uint8_t rr, gg, bb;
    if (parseHexColor(server.arg("cha"), rr, gg, bb)) {
      chaseAR = rr; chaseAG = gg; chaseAB = bb;
      dirty = true;
    }
  }
  if (server.hasArg("chb")) {
    uint8_t rr, gg, bb;
    if (parseHexColor(server.arg("chb"), rr, gg, bb)) {
      chaseBR = rr; chaseBG = gg; chaseBB = bb;
      dirty = true;
    }
  }

  // Mirage
  if (server.hasArg("mihz")) {
    long v = server.arg("mihz").toInt();
    if (v < 0) v = 0;
    if (v > 400) v = 400;
    mirageHz = (uint16_t)v;
    dirty = true;
  }
  if (server.hasArg("micolor")) {
    uint8_t rr, gg, bb;
    if (parseHexColor(server.arg("micolor"), rr, gg, bb)) {
      mirageR = rr; mirageG = gg; mirageB = bb;
      buildMirageFrames();
      dirty = true;
    }
  }

  server.send(200, "text/plain", "OK");
}

void handleSave() {
  saveConfig();
  server.send(200, "text/plain", "SAVED");
}

// ---------------- WiFi ----------------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  WiFi.config(localIP, gateway, subnet, dns);
  WiFi.begin(SSID, PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
    delay(250);
  }
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(50);

  EEPROM.begin(EEPROM_SIZE);

  strip.begin();
  strip.setBrightness(255);
  strip.show();

  for (uint16_t i = 0; i < TOTAL_LEDS; i++) frameOff[i] = 0;

  loadConfig();
  buildMirageFrames();

  chasePos = wrapFan(chasePos);

  renderActiveNow();

  connectWiFi();

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/save", handleSave);
  server.on("/state", handleStateJson);
  server.begin();

  Serial.println("BroCommand running at http://192.168.10.50");
}

void loop() {
  server.handleClient();

  if (dirty) {
    dirty = false;

    if (mainMode == MODE_SOLID) {
      renderSolid();
    } else if (mainMode == MODE_CHASER) {
      chasePos = wrapFan(chasePos);
      chaseLastStepMs = millis();
      renderChaserFrame();
    } else {
      mirageLastToggleUs = micros();
      renderMirageImmediate();
    }
  }

  if (mainMode == MODE_CHASER) {
    uint32_t now = millis();
    if ((uint32_t)(now - chaseLastStepMs) >= chaseDelayMs) {
      chaseLastStepMs = now;
      chasePos = wrapFan((int32_t)chasePos + (int32_t)chaseDir);
      renderChaserFrame();
    }
  } else if (mainMode == MODE_MIRAGE) {
    uint16_t f = mirageHz;
    if (f == 0) { yield(); return; }

    uint32_t halfPeriodUs = 500000UL / (uint32_t)f;
    if (halfPeriodUs < 150) halfPeriodUs = 150;

    uint32_t nowUs = micros();
    if ((uint32_t)(nowUs - mirageLastToggleUs) >= halfPeriodUs) {
      mirageLastToggleUs = nowUs;
      mirageOnPhase = !mirageOnPhase;
      showFrame(mirageOnPhase ? frameOn : frameOff);
    }
  }

  yield();
}