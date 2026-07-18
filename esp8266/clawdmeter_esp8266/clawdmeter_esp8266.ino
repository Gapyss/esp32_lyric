#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <WiFiManager.h>          // install "WiFiManager" by tzapu via Library Manager
#include <Arduino_GFX_Library.h>  // install "GFX Library for Arduino" by moononournation
#include <time.h>                 // NTP clock (so the wait screen has time before any push)
#include "tend.h"                 // shared palette / screen registry / helpers
#include "thai_font.h"            // Thai+Latin GFXfont tables for the MUSIC screen (PROGMEM)

ESP8266WebServer server(80);

// --- GeekMagic HelloCubic Lite / SmallTV-Ultra: ESP8266 + ST7789 240x240 ---
// Pins/SPI mirror the GeekMagic open firmware. CS is tied to GND, the backlight
// is ACTIVE LOW (GPIO5 LOW = on), and the panel needs SPI mode 3.
#define LCD_DC   0
#define LCD_RST  2
#define LCD_BL   5
#define LCD_ROT  0    // change to 2/4/6 if the image is rotated or mirrored
// Backlight brightness 0..255. Was hardwired full-on (255), which runs the panel
// hot; the LED string behind the glass is the main "screen is hot" heat source.
// ~90 is plenty indoors. Lower = cooler + less current (also eases the regulator).
#define LCD_BRIGHTNESS 90
#define LCD_MAX_BRIGHTNESS 120
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, GFX_NOT_DEFINED /* CS -> GND */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, LCD_ROT, true /* IPS */, 240, 240);

// A MUSIC-screen text face: a Latin+Thai GFXfont pair (thai_font.h) plus the
// baseline ascent for its band. Defined up here for the same reason as FaceExpr:
// Arduino injects auto-prototypes for the functions that take it at the top of
// the file, so the type must already be visible there.
// `latinSize` > 0 renders ASCII (English) with the chip's built-in 5x7 font at
// that integer scale instead of the bundled Ayuthaya Latin GFXfont (`latin`);
// Thai always blits from `thai`. Set `latinSize` to 0 to fall back to `latin`.
struct MusicFace { const GFXfont *latin; const GFXfont *thai; int16_t ascent; uint8_t latinSize; };

// EEPROM layout v2 (see tend.h): brightness + water config + pet counters.
// v1 (marker 0xC1) carried only brightness; loadBrightness migrates it.
static const uint8_t EEPROM_MARKER_V1 = 0xC1;
static const uint8_t EEPROM_MARKER = 0xC2;
uint8_t lcdBrightness = LCD_BRIGHTNESS;

// Backlight is ACTIVE LOW and the pin supports software PWM. analogWrite sets the
// HIGH duty, and HIGH = off here, so invert: brightness 255 -> duty 0 (full on),
// brightness 0 -> duty 255 (off). The PWM is a software waveform on an IRAM timer
// ISR (~2 edges/period), so its CPU cost scales with frequency. Keep it LOW: at
// 20 kHz the ISR starved the WiFi/TCP stack and crash-rebooted the device under any
// HTTP load. 1 kHz (1/20th the interrupt rate) is well above flicker fusion and
// leaves the radio/server responsive. Extremes (brightness 0/255 -> duty 255/0)
// stop the waveform entirely, which is what makes a flash write safe (see OTA).
#define LCD_PWM_FREQ 1000
static void setBacklight(uint8_t brightness) {
  analogWriteRange(255);
  analogWriteFreq(LCD_PWM_FREQ);
  analogWrite(LCD_BL, 255 - brightness);
}

// Silence the PWM waveform ISR before any SPI-flash erase/write (OTA, EEPROM): a
// timer interrupt firing mid-flash resets the ESP8266 (deterministic crash). duty 0
// detaches the pin from the waveform generator and leaves the backlight full-on.
static void backlightStopForFlash() {
  analogWrite(LCD_BL, 0);
}

// Commit wrapped in the flash guard, shared with the screen modules (water
// config, pet counters). See backlightStopForFlash above for why.
void tendEepromCommit() {
  backlightStopForFlash();
  EEPROM.commit();
  setBacklight(lcdBrightness);
}

// Migrate v1 (brightness only) to v2 and default the new fields; on a fresh
// chip default everything. Runs once in setup before any screen reads config.
static void eepromInitLayout() {
  const uint8_t marker = EEPROM.read(EE_MARKER_ADDR);
  if (marker == EEPROM_MARKER) return;
  if (marker != EEPROM_MARKER_V1) {
    EEPROM.write(EE_BRIGHTNESS_ADDR, LCD_BRIGHTNESS);
  }
  EEPROM.write(EE_MARKER_ADDR, EEPROM_MARKER);
  EEPROM.put(EE_WATER_INTERVAL, (uint16_t)45);
  EEPROM.put(EE_WATER_START, (uint16_t)(9 * 60));
  EEPROM.put(EE_WATER_END, (uint16_t)(18 * 60));
  EEPROM.put(EE_PET_TOTAL, (uint32_t)0);
  EEPROM.put(EE_PET_ADOPT, (uint32_t)0);
  tendEepromCommit();
}

static uint8_t loadBrightness() {
  if (EEPROM.read(EE_MARKER_ADDR) != EEPROM_MARKER) return LCD_BRIGHTNESS;
  return min((uint8_t)EEPROM.read(EE_BRIGHTNESS_ADDR), (uint8_t)LCD_MAX_BRIGHTNESS);
}

static void saveBrightness(uint8_t brightness) {
  if (EEPROM.read(EE_BRIGHTNESS_ADDR) == brightness) {
    return;
  }
  EEPROM.write(EE_BRIGHTNESS_ADDR, brightness);
  tendEepromCommit();
}

static void applyBrightness(uint8_t brightness, bool persist) {
  lcdBrightness = min(brightness, (uint8_t)LCD_MAX_BRIGHTNESS);
  setBacklight(lcdBrightness);
  if (persist) saveBrightness(lcdBrightness);
}

// Latest usage, pushed by the daemon. -1 = no data yet.
int sessionPct = -1;            // 5-hour utilization %
int weeklyPct  = -1;            // 7-day utilization %
unsigned long sessionTokens = 0;
unsigned long weeklyTokens = 0;
unsigned long lastUpdateMs = 0;

// api-mode extras (see daemon push). 0 = not provided.
unsigned long sessReset = 0;    // unix epoch when the 5h window resets
unsigned long weekReset = 0;    // unix epoch when the 7d window resets
String unifiedStatus = "";      // "allowed" / "rejected" / ... from the API
int bindingLimit = 0;           // 1 = session is binding, 2 = weekly, 0 = unknown
int macCpuPct = -1;             // macOS CPU use, pushed by daemon
int macMemPct = -1;             // macOS memory use
int macDiskPct = -1;            // macOS home volume disk use
int macBatteryPct = -1;         // Mac battery level, -1 if unavailable
uint8_t lcdScreen = SCREEN_CLOCK;   // screen ids live in tend.h
String npTitle = "";             // YouTube Music now-playing title (UTF-8, may be Thai)
String npArtist = "";            // now-playing artist (UTF-8)
int npPos = -1;                  // current playback position, seconds
int npDur = -1;                  // total track duration, seconds
int npPaused = -1;               // -1 unknown, 0 playing, 1 paused
unsigned long npPosBaseMs = 0;   // millis() when npPos was received
String npLyric = "";             // current lyric line (UTF-8, daemon-pushed from lrclib)
String npLyric2 = "";            // upcoming lyric line (shown dim below the current one)
int npLyricAt = -1;              // playback pos (s) at which npLyric2 promotes to current; -1 = none
bool musicChromeReady = false;   // MUSIC chrome is static; pushes repaint title/artist only
bool otaInProgress = false;     // true while /update is writing firmware
bool otaUpdateOk = false;
String otaError = "";
String bootReason = "";         // why the chip last reset (captured once at boot)
String bootInfo = "";           // detailed reset info (exception cause/stack on crash)

// UTC clock, synced from the daemon's pushed server time (no RTC/NTP needed).
unsigned long timeBaseEpoch = 0;   // server epoch at the moment of the last push
unsigned long timeBaseMillis = 0;  // millis() at that same moment

// Current UTC epoch. Prefer the daemon-pushed server time (extrapolated via
// millis()); before the first push, fall back to NTP. 0 = no time source yet.
unsigned long nowEpoch() {
  if (timeBaseEpoch != 0)
    return timeBaseEpoch + (millis() - timeBaseMillis) / 1000UL;
  time_t t = time(nullptr);
  return (t > 1700000000) ? (unsigned long)t : 0;   // >2023 => NTP has synced
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>clawdmeter</title>
<style>
  :root{color-scheme:light;--paper:#F8F3E1;--soft:#FDFBF2;--ink:#1F1D11;--mut:rgba(31,29,17,.55);--line:rgba(31,29,17,.16);--ember:#E85D3C}
  *{box-sizing:border-box}
  body{font-family:Roboto,-apple-system,"Segoe UI",system-ui,sans-serif;background:var(--paper);color:var(--ink);margin:0;min-height:100vh}
  main{width:min(920px,100%);margin:0 auto;padding:22px;display:grid;gap:14px}
  header{display:flex;align-items:flex-end;justify-content:space-between;gap:14px;border-bottom:1px solid var(--line);padding-bottom:12px}
  h1{font-family:"Roboto Condensed",Roboto,sans-serif;font-size:26px;line-height:1;margin:2px 0 0;font-weight:700}
  .eb{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:var(--mut)}
  .mut{color:var(--mut);font-size:13px}
  .mono{font-family:"JetBrains Mono",ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}
  .status{display:flex;align-items:center;gap:8px}
  .dot{width:10px;height:10px;border-radius:50%;background:var(--mut)}
  .dot.live{background:var(--ink)}
  .dot.hot{background:var(--ember)}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
  .card{background:var(--soft);border:1px solid var(--line);border-radius:16px;padding:16px}
  .label{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin-bottom:10px}
  .pct{font-size:34px;line-height:1;font-weight:700}
  .track{height:14px;border:1px solid var(--line);border-radius:999px;overflow:hidden;background:var(--paper)}
  .fill{height:100%;width:0;background:var(--ember);transition:width .35s ease}
  .row{display:flex;justify-content:space-between;gap:12px;margin-top:10px;font-size:13px;color:var(--mut)}
  .chip{font-size:11px;letter-spacing:.12em;text-transform:uppercase;background:var(--ink);color:var(--paper);border-radius:999px;padding:2px 8px}
  .stats{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}
  .stat{background:var(--soft);border:1px solid var(--line);border-radius:16px;padding:12px 14px}
  .v{font-size:18px;font-weight:600;margin-top:5px;white-space:nowrap}
  .acts{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
  .btn{font:inherit;font-size:13px;color:var(--ink);background:transparent;border:1px solid var(--line);border-radius:10px;padding:8px 12px;cursor:pointer;text-decoration:none}
  .btn:hover{border-color:var(--ink)}
  .btn.on{background:var(--ink);color:var(--paper);border-color:var(--ink)}
  .btn.warn{color:var(--ember)}
  .btn.warn:hover{border-color:var(--ember)}
  .ctx{display:none}
  .big{font-size:30px;font-weight:700;line-height:1.1}
  .cfg{display:grid;grid-template-columns:1fr 1fr 1fr auto;gap:8px;margin-top:10px}
  input[type=text],input[type=number]{font:inherit;font-size:13px;width:100%;color:var(--ink);background:var(--paper);border:1px solid var(--line);border-radius:10px;padding:8px 10px}
  input[type=range]{width:100%;accent-color:var(--ember)}
  .ctl{display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:12px;margin-top:12px}
  .stale main{opacity:.6}
  @media(max-width:720px){main{padding:14px}.grid{grid-template-columns:1fr}.stats{grid-template-columns:1fr 1fr}.cfg{grid-template-columns:1fr 1fr}}
</style></head><body>
<main>
<header>
  <div><div class="eb">tend &middot; desk cube</div><h1>clawdmeter</h1></div>
  <div class="status"><span class="dot" id="dot"></span><span class="mut" id="status">connecting</span></div>
</header>
<section class="grid">
  <div class="card">
    <div class="label"><span><span class="eb">session window</span> <span class="chip" id="sB" hidden>binding</span></span><span class="pct mono" id="sp">--</span></div>
    <div class="track"><div class="fill" id="sf"></div></div>
    <div class="row"><span id="st">reset --</span><span class="mono" id="sc">--</span></div>
  </div>
  <div class="card">
    <div class="label"><span><span class="eb">weekly window</span> <span class="chip" id="wB" hidden>binding</span></span><span class="pct mono" id="wp">--</span></div>
    <div class="track"><div class="fill" id="wf"></div></div>
    <div class="row"><span id="wt">reset --</span><span class="mono" id="wc">7 days</span></div>
  </div>
</section>
<section class="stats">
  <div class="stat"><div class="eb">clock &middot; utc+7</div><div class="v mono" id="clock">--:--:--</div></div>
  <div class="stat"><div class="eb">last push</div><div class="v mono" id="age">--</div></div>
  <div class="stat"><div class="eb">session tokens</div><div class="v mono" id="stok">--</div></div>
  <div class="stat"><div class="eb">weekly tokens</div><div class="v mono" id="wtok">--</div></div>
</section>
<section class="card">
  <div class="label"><span class="eb">screen</span><span class="mut" id="scrName">--</span></div>
  <div class="acts">
    <button class="btn" data-scr="clock" data-get="/mode?screen=clock">clock</button>
    <button class="btn" data-scr="music" data-get="/mode?screen=music">music</button>
    <button class="btn" data-scr="pomodoro" data-get="/mode?screen=pomodoro">pomodoro</button>
    <button class="btn" data-scr="water" data-get="/mode?screen=water">water</button>
    <button class="btn" data-scr="stats" data-get="/mode?screen=stats">stats</button>
    <button class="btn" data-scr="pet" data-get="/mode?screen=pet">pet</button>
    <button class="btn" data-scr="sand" data-get="/mode?screen=sand">sand</button>
    <button class="btn" data-scr="swarm" data-get="/mode?screen=swarm">swarm</button>
    <button class="btn" data-scr="comic" data-get="/mode?screen=comic">comic</button>
    <button class="btn" data-scr="apod" data-get="/mode?screen=apod">apod</button>
  </div>
  <div class="ctx" id="cx-clock"><div class="mut" style="margin-top:12px">time flows from the daemon &middot; nothing to tend here</div></div>
  <div class="ctx" id="cx-music">
    <div class="label" style="margin-top:12px"><span class="big" id="mT">nothing playing</span><span class="mono mut" id="mP">--</span></div>
    <div class="mut" id="mA"></div><div class="mut" id="mL" style="margin-top:6px"></div>
  </div>
  <div class="ctx" id="cx-pomodoro">
    <div class="label" style="margin-top:12px"><span class="big mono" id="pR">25:00</span><span class="mut" id="pS">idle</span></div>
    <div class="acts"><button class="btn" id="pBtn" data-get="/pomodoro">start</button><button class="btn" data-get="/pomodoro?action=reset">reset</button></div>
  </div>
  <div class="ctx" id="cx-water">
    <div class="label" style="margin-top:12px"><span class="big"><span class="mono" id="wD">0</span> <span style="font-size:15px;font-weight:400">drinks today</span></span><span class="mut">next &middot; <span class="mono" id="wN">--</span></span></div>
    <div class="acts"><button class="btn" data-get="/hydrate/log">log a drink</button><button class="btn" data-get="/hydrate/now">remind now</button><button class="btn" data-get="/hydrate/snooze?min=10">snooze 10m</button></div>
    <div class="cfg"><input id="wI" type="number" min="5" max="480" title="interval min"><input id="wS" type="text" maxlength="5" title="start hh:mm"><input id="wE" type="text" maxlength="5" title="end hh:mm"><button class="btn" id="wApply">apply</button></div>
    <div class="mut" style="margin-top:6px">interval minutes &middot; active start &middot; active end</div>
  </div>
  <div class="ctx" id="cx-stats">
    <div class="stats" style="margin-top:12px">
      <div class="stat"><div class="eb">mac cpu</div><div class="v mono" id="xC">--</div></div>
      <div class="stat"><div class="eb">memory</div><div class="v mono" id="xM">--</div></div>
      <div class="stat"><div class="eb">disk</div><div class="v mono" id="xD">--</div></div>
      <div class="stat"><div class="eb">battery</div><div class="v mono" id="xB">--</div></div>
    </div>
  </div>
  <div class="ctx" id="cx-pet">
    <div class="label" style="margin-top:12px"><span class="big"><span class="mono" id="petD">0</span> <span style="font-size:15px;font-weight:400">pets today</span></span><span class="mut">all time &middot; <span class="mono" id="petT">0</span></span></div>
    <div class="acts"><button class="btn" data-get="/pet">pet the pet</button></div>
  </div>
  <div class="ctx" id="cx-sand">
    <div class="acts" style="margin-top:12px"><button class="btn" data-get="/sand?action=pour">pour sand</button><button class="btn" data-get="/sand?action=clear">clear</button></div>
  </div>
  <div class="ctx" id="cx-swarm">
    <div class="mut" style="margin-top:12px">roaming &middot; <span id="swR">off</span></div>
    <div class="acts"><button class="btn" data-get="/swarm?action=scatter">scatter</button><button class="btn" data-get="/swarm?action=roam">toggle roam</button></div>
  </div>
  <div class="ctx" id="cx-comic">
    <div class="label" style="margin-top:12px"><span class="big" id="cT">--</span><span class="mut" id="cS">--</span></div>
    <div class="acts"><button class="btn" data-get="/refresh?screen=comic">refresh comic</button></div>
  </div>
  <div class="ctx" id="cx-apod">
    <div class="label" style="margin-top:12px"><span class="big" id="aT">--</span><span class="mut" id="aS">--</span></div>
    <div class="mut" id="aX"></div>
    <div class="acts"><button class="btn" data-get="/refresh?screen=apod">refresh apod</button></div>
  </div>
</section>
<section class="card">
  <div class="label"><span class="eb">device</span><span class="mut"><span class="mono" id="heap">--</span> heap &middot; up <span class="mono" id="up">--</span> &middot; boot <span id="rst">--</span></span></div>
  <div class="acts"><a class="btn" href="/usage.json">usage json</a><a class="btn" href="/update">ota update</a><a class="btn" href="/restart" id="rBtn">restart</a><a class="btn warn" href="/factory-reset" id="fBtn">reset settings</a></div>
  <div class="ctl"><span class="eb">backlight</span><input id="bl" type="range" min="0" max="120" value="90"><span class="v mono" id="blV">90</span></div>
</section>
</main>
<script>
function $(i){return document.getElementById(i)}
var TZ=25200; // Asia/Bangkok UTC+7
function pad2(n){return (n<10?'0':'')+n}
function lt(e){return new Date((e+TZ)*1000)} // local UTC+7 Date via UTC getters
function hm(e){var d=lt(e);return pad2(d.getUTCHours())+':'+pad2(d.getUTCMinutes())}
function clk(e){return hm(e)+':'+pad2(lt(e).getUTCSeconds())}
var DOW=['sun','mon','tue','wed','thu','fri','sat'];
function cd(s){if(s<=0)return 't-0m';var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h>0?'t-'+h+'h'+pad2(m)+'m':'t-'+m+'m'}
function mmss(s){s=s>0?Math.floor(s):0;return Math.floor(s/60)+':'+pad2(s%60)}
function m2h(m){return pad2(Math.floor(m/60))+':'+pad2(m%60)}
function commas(n){return n>0?String(n).replace(/\B(?=(\d{3})+(?!\d))/g,','):'--'}
function ageText(a){return a<0?'--':a<60?a+'s':Math.floor(a/60)+'m '+pad2(a%60)+'s'}
function pct(v){return v>=0?v+'%':'--'}
function upText(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h>0?h+'h'+pad2(m)+'m':m+'m'}
function meter(pId,fId,v){$(pId).textContent=v>=0?v+'%':'--';$(fId).style.width=(v>=0?Math.min(v,100):0)+'%'}
var gs=document.querySelectorAll('[data-get]');
for(var gi=0;gi<gs.length;gi++)gs[gi].onclick=function(e){e.preventDefault();fetch(this.getAttribute('data-get'),{cache:'no-store'}).then(tick).catch(function(_){})};
$('wApply').onclick=function(e){e.preventDefault();
  fetch('/hydrate/config?interval='+encodeURIComponent($('wI').value)+'&start='+encodeURIComponent($('wS').value)+'&end='+encodeURIComponent($('wE').value),{cache:'no-store'}).then(tick).catch(function(_){})};
$('fBtn').onclick=function(e){if(!confirm('reset wifi and settings, then reboot?'))e.preventDefault()};
$('rBtn').onclick=function(e){if(!confirm('restart clawdmeter?'))e.preventDefault()};
var blBusy=false,blTimer=0;
$('bl').oninput=function(){var v=parseInt(this.value,10)||0;$('blV').textContent=v;blBusy=true;
  clearTimeout(blTimer);blTimer=setTimeout(function(){fetch('/brightness?value='+$('bl').value,{cache:'no-store',method:'POST'}).catch(function(_){});blBusy=false},120)};
function setIf(id,v){var a=document.activeElement&&document.activeElement.id;if(a!=id)$(id).value=v}
var ticking=false;
async function tick(){
  if(ticking)return;
  ticking=true;
  var st=$('status'),dot=$('dot');
  try{
    var d=await (await fetch('/usage.json',{cache:'no-store'})).json();
    var now=d.now||0,live=d.age>=0&&d.age<=120;
    var hot=(d.stat&&d.stat!='allowed')||d.s>=90||d.walert==1;
    document.body.className=live?'':'stale';
    dot.className='dot'+(hot?' hot':live?' live':'');
    st.textContent=d.s<0?'waiting for daemon':(d.stat||'local')+' · updated '+ageText(d.age)+' ago';
    meter('sp','sf',d.s);meter('wp','wf',d.w);
    $('sB').hidden=d.bind!=1;$('wB').hidden=d.bind!=2;
    $('st').textContent=d.sr?'reset '+hm(d.sr):'reset --';
    $('sc').textContent=d.sr&&now?cd(d.sr-now):'--';
    $('wt').textContent=d.wr?'reset '+DOW[lt(d.wr).getUTCDay()]+' '+hm(d.wr):'reset --';
    $('clock').textContent=now?clk(now):'--:--:--';
    $('age').textContent=ageText(d.age);
    $('stok').textContent=commas(d.st);
    $('wtok').textContent=commas(d.wt);
    var bs=document.querySelectorAll('[data-scr]');
    for(var i=0;i<bs.length;i++)bs[i].className='btn'+(bs[i].getAttribute('data-scr')==d.screen?' on':'');
    $('scrName').textContent='on the lcd · '+d.screen;
    var cs=document.querySelectorAll('.ctx');
    for(var ci=0;ci<cs.length;ci++)cs[ci].style.display=cs[ci].id=='cx-'+d.screen?'block':'none';
    $('mT').textContent=(d.title||'').trim()||'nothing playing';
    $('mA').textContent=d.artist||'';
    $('mL').textContent=(d.lyric||'')+(d.lyric2?' / '+d.lyric2:'');
    $('mP').textContent=d.dur>0?mmss(d.pos)+' / '+mmss(d.dur)+(d.paused==1?' · paused':''):'--';
    $('pR').textContent=mmss(d.pomor);
    $('pS').textContent=d.pomo;
    $('pBtn').textContent=d.pomo=='running'?'pause':'start';
    $('wD').textContent=d.drinks;
    $('wN').textContent=d.walert==1?'drink now':d.nextin<0?'--':d.nextin<60?d.nextin+'s':Math.ceil(d.nextin/60)+'m';
    setIf('wI',d.winterval);setIf('wS',m2h(d.wstart));setIf('wE',m2h(d.wend));
    $('xC').textContent=pct(d.cpu);$('xM').textContent=pct(d.mem);
    $('xD').textContent=pct(d.disk);$('xB').textContent=pct(d.bat);
    $('petD').textContent=d.pets;$('petT').textContent=d.petstotal;
    $('swR').textContent=d.swroam==1?'on':'off';
    $('cT').textContent=d.ctitle||'--';$('cS').textContent=d.cstate;
    $('aT').textContent=d.atitle||'--';$('aS').textContent=d.astate;$('aX').textContent=d.ax||'';
    $('heap').textContent=Math.round(d.heap/1024)+'k';
    $('up').textContent=upText(d.up);
    $('rst').textContent=d.rst||'--';
    if(!blBusy&&d.bl>=0){$('bl').value=d.bl;$('blV').textContent=d.bl}
  }catch(e){st.textContent='device unreachable';dot.className='dot hot'}
  ticking=false;
}
tick();setInterval(tick,3000);
</script></body></html>
)HTML";

// Gzipped form of INDEX_HTML above, served by handleRoot(). Regenerate with
// esp8266/tools/gen_index_gz.py whenever INDEX_HTML changes.
#include "index_html_gz.h"

const char UPDATE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>clawdmeter · ota</title>
<style>
  :root{color-scheme:light;--paper:#F8F3E1;--soft:#FDFBF2;--ink:#1F1D11;--mut:rgba(31,29,17,.55);--line:rgba(31,29,17,.16);--ember:#E85D3C}
  *{box-sizing:border-box}
  body{font-family:Roboto,-apple-system,"Segoe UI",system-ui,sans-serif;background:var(--paper);color:var(--ink);margin:0;min-height:100vh;display:grid;place-items:center;padding:20px}
  main{width:min(520px,100%);background:var(--soft);border:1px solid var(--line);border-radius:16px;padding:18px;display:grid;gap:14px}
  h1{font-family:"Roboto Condensed",Roboto,sans-serif;font-size:22px;margin:0}
  p{color:var(--mut);font-size:14px;line-height:1.45;margin:0}
  form{display:grid;gap:12px}
  input,button{font:inherit}
  input[type=file]{border:1px solid var(--line);border-radius:10px;padding:10px;width:100%}
  button{color:var(--ink);background:transparent;border:1px solid var(--line);border-radius:10px;padding:10px 12px;cursor:pointer}
  button:hover{border-color:var(--ember)}
  code{color:var(--ink)}
</style></head><body><main>
<h1>firmware update</h1>
<p>upload only <code>clawdmeter_esp8266.ino.bin</code> · close dashboard tabs and stop the daemon while updating</p>
<form method="POST" action="/update" enctype="multipart/form-data">
  <input type="file" name="firmware" accept=".bin,.bin.gz" required>
  <button type="submit">update firmware</button>
</form>
<p>the device reboots after a successful upload · if the browser disconnects, wait 20 seconds and reopen the dashboard</p>
</main></body></html>
)HTML";

void handleRoot() {
  // Serve the dashboard gzipped (~15 KB -> ~4.4 KB, see index_html_gz.h). The
  // uncompressed page took multiple TCP segments and a single blocking send_P
  // stalled mid-write ~25% of the time when the WiFi link couldn't drain the
  // send buffer fast enough — Chrome then showed a failed/partial load. The
  // gzipped body fits in one send-buffer fill so the write completes in one go.
  // Keep the radio awake for the brief send as cheap insurance against a lossy
  // link; it's invisible (unlike parking the backlight PWM, which flashed the
  // screen). INDEX_HTML stays the editable source; regenerate the header with
  // esp8266/tools/gen_index_gz.py after editing it.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  // Drain the TX buffer before "Connection: close" tears the socket down. On a
  // lossy link the final segment was racing the close and arriving truncated at
  // the client (page came through as 2920/4424); flush() blocks until the
  // outgoing data is actually sent.
  server.client().flush();
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
}

// Daemon pushes the numbers here:
// POST /usage?s=&w=&st=&wt=&sr=&wr=&stat=&bind=&t=&cpu=&mem=&disk=&bat=
void handleUsage() {
  // The pusher is the Mac that also runs the lyrics daemon — remember its IP
  // so the MUSIC screen can dial back for the frame stream (no config needed).
  lyricsStreamNoteHost(server.client().remoteIP());
  if (server.hasArg("s")) sessionPct = server.arg("s").toInt();
  if (server.hasArg("w")) weeklyPct  = server.arg("w").toInt();
  if (server.hasArg("st")) sessionTokens = strtoul(server.arg("st").c_str(), NULL, 10);
  if (server.hasArg("wt")) weeklyTokens = strtoul(server.arg("wt").c_str(), NULL, 10);
  if (server.hasArg("sr")) sessReset = strtoul(server.arg("sr").c_str(), NULL, 10);
  if (server.hasArg("wr")) weekReset = strtoul(server.arg("wr").c_str(), NULL, 10);
  if (server.hasArg("stat")) unifiedStatus = server.arg("stat");
  if (server.hasArg("bind")) bindingLimit = server.arg("bind").toInt();
  if (server.hasArg("cpu")) macCpuPct = server.arg("cpu").toInt();
  if (server.hasArg("mem")) macMemPct = server.arg("mem").toInt();
  if (server.hasArg("disk")) macDiskPct = server.arg("disk").toInt();
  if (server.hasArg("bat")) macBatteryPct = server.arg("bat").toInt();
  if (server.hasArg("t")) {
    unsigned long t = strtoul(server.arg("t").c_str(), NULL, 10);
    if (t > 0) { timeBaseEpoch = t; timeBaseMillis = millis(); }
  }
  lastUpdateMs = millis();
  statsOnUsagePush();   // fold the fresh Mac metrics into the stats trend
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

// Screen id <-> name, mirroring the ESP32 app_mode names (plus music).
static const char *const SCREEN_NAMES[] = {
  "music", "clock", "pomodoro", "water", "stats",
  "pet", "sand", "swarm", "comic", "apod",
};
static const uint8_t SCREEN_NAME_COUNT = sizeof(SCREEN_NAMES) / sizeof(SCREEN_NAMES[0]);

// The dashboard page polls this.
static String screenName() {
  if (lcdScreen < SCREEN_NAME_COUNT) return SCREEN_NAMES[lcdScreen];
  return "clock";
}

// Escape a UTF-8 string for embedding in the hand-built /usage.json. Song titles
// are arbitrary text, so a stray quote/backslash would otherwise break the JSON;
// raw multibyte UTF-8 is valid inside a JSON string and passes through untouched.
static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;   // drop control chars, keep UTF-8 bytes
  }
  return o;
}

void handleUsageJson() {
  long age = (sessionPct < 0) ? -1 : (long)((millis() - lastUpdateMs) / 1000);
  String j = "{\"s\":" + String(sessionPct) +
             ",\"w\":" + String(weeklyPct) +
             ",\"st\":" + String(sessionTokens) +
             ",\"wt\":" + String(weeklyTokens) +
             ",\"sr\":" + String(sessReset) +
             ",\"wr\":" + String(weekReset) +
             ",\"stat\":\"" + unifiedStatus + "\"" +
             ",\"bind\":" + String(bindingLimit) +
             ",\"cpu\":" + String(macCpuPct) +
             ",\"mem\":" + String(macMemPct) +
             ",\"disk\":" + String(macDiskPct) +
             ",\"bat\":" + String(macBatteryPct) +
             ",\"screen\":\"" + screenName() + "\"" +
             ",\"title\":\"" + jsonEscape(npTitle) + "\"" +
             ",\"artist\":\"" + jsonEscape(npArtist) + "\"" +
             ",\"lyric\":\"" + jsonEscape(npLyric) + "\"" +
             ",\"lyric2\":\"" + jsonEscape(npLyric2) + "\"" +
             ",\"pos\":" + String(musicDisplayPos()) +
             ",\"dur\":" + String(npDur) +
             ",\"paused\":" + String(npPaused) +
             ",\"pomo\":\"" + String(pomodoroStateName()) + "\"" +
             ",\"pomor\":" + String(pomodoroRemainingSec()) +
             ",\"drinks\":" + String(waterDrinksToday()) +
             ",\"nextin\":" + String(waterNextInSec()) +
             ",\"walert\":" + String(waterAlerting() ? 1 : 0) +
             ",\"winterval\":" + String(waterIntervalMin()) +
             ",\"wstart\":" + String(waterStartMin()) +
             ",\"wend\":" + String(waterEndMin()) +
             ",\"pets\":" + String(petPetsToday()) +
             ",\"petstotal\":" + String(petPetsTotal()) +
             ",\"swroam\":" + String(swarmRoaming() ? 1 : 0) +
             ",\"cstate\":\"" + String(dailyStateName(false)) + "\"" +
             ",\"astate\":\"" + String(dailyStateName(true)) + "\"" +
             ",\"ctitle\":\"" + jsonEscape(dailyTitle(false)) + "\"" +
             ",\"atitle\":\"" + jsonEscape(dailyTitle(true)) + "\"" +
             ",\"cx\":\"" + jsonEscape(dailyExtra(false)) + "\"" +
             ",\"ax\":\"" + jsonEscape(dailyExtra(true)) + "\"" +
             ",\"bl\":" + String(lcdBrightness) +
             ",\"heap\":" + String(ESP.getFreeHeap()) +
             ",\"now\":" + String(nowEpoch()) +
             ",\"rst\":\"" + bootReason + "\"" +
             ",\"rinfo\":\"" + jsonEscape(bootInfo) + "\"" +
             ",\"up\":" + String(millis() / 1000) +
             ",\"age\":" + String(age) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", j);
}

void handleBrightness() {
  if (server.hasArg("value")) {
    String raw = server.arg("value");
    bool valid = raw.length() > 0;
    for (unsigned int i = 0; i < raw.length(); i++) {
      if (!isDigit(raw[i])) valid = false;
    }
    if (valid) {
      int value = raw.toInt();
      value = constrain(value, 0, 255);
      applyBrightness((uint8_t)value, true);
    }
  }
  String j = "{\"brightness\":" + String(lcdBrightness) + "}";
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", j);
}

void handleMode() {
  if (server.hasArg("screen")) {
    String screen = server.arg("screen");
    screen.toLowerCase();
    for (uint8_t i = 0; i < SCREEN_NAME_COUNT; i++) {
      if (screen == SCREEN_NAMES[i]) {
        if (i != lcdScreen) tendShowScreen(i);
        break;
      }
    }
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", screenName());
}

// Daemon pushes the current YouTube Music song here:
// POST /nowplaying?title=<urlenc UTF-8>&artist=<urlenc UTF-8>&pos=&dur=&paused=&lyric=&lyric2=&lt=
// Stores the song WITHOUT stealing focus: now-playing is a web-only feature
// (the dashboard's Now Playing panel reads it from /usage.json). The physical
// MUSIC screen is manual-only — selected via /mode?screen=music. If MUSIC happens
// to be the current screen, track/pause changes repaint the content; position/lyric
// resyncs repaint just the progress/time footer and lyric band.
void handleNowPlaying() {
  lyricsStreamNoteHost(server.client().remoteIP());
  String oldTitle = npTitle;
  String oldArtist = npArtist;
  int oldDur = npDur;
  int oldPaused = npPaused;
  String oldLyric = npLyric;
  String oldLyric2 = npLyric2;
  if (server.hasArg("title")) npTitle = server.arg("title");
  if (server.hasArg("artist")) npArtist = server.arg("artist");
  if (server.hasArg("pos")) {
    npPos = server.arg("pos").toInt();
    npPosBaseMs = millis();
  }
  if (server.hasArg("dur")) npDur = server.arg("dur").toInt();
  if (server.hasArg("paused")) npPaused = constrain(server.arg("paused").toInt(), -1, 1);
  bool trackChanged = oldTitle != npTitle || oldArtist != npArtist;
  bool durationChanged = oldDur != npDur;
  bool identityChanged = trackChanged || durationChanged;
  if (trackChanged) {
    npPos = 0;
    npPosBaseMs = millis();
  }
  if (server.hasArg("lyric")) {
    npLyric = server.arg("lyric");
    npLyric2 = server.hasArg("lyric2") ? server.arg("lyric2") : String("");
    npLyricAt = server.hasArg("lt") ? server.arg("lt").toInt() : -1;
  } else if (identityChanged) {
    npLyric = "";
    npLyric2 = "";
    npLyricAt = -1;
  }
  // While the frame stream owns the panel the daemon's frames carry all of
  // this; painting the on-device widgets would scribble over them.
  if (lcdScreen == SCREEN_MUSIC && !lyricsStreamActive()) {
    bool pausedChanged = oldPaused != npPaused;
    if (identityChanged || pausedChanged) drawMusic();
    else {
      musicDrawFooter();
      if (oldLyric != npLyric || oldLyric2 != npLyric2) musicDrawLyrics();
    }
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

// Per-screen action endpoints (this box has no physical buttons; the ESP32's
// BOOT/action button gestures become dashboard buttons). Every action jumps to
// its screen so the effect is visible immediately.
void handleHydrateLog() {
  waterLogDrink();
  tendShowScreen(SCREEN_WATER);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

void handleHydrateNow() {
  waterFireNow();
  tendShowScreen(SCREEN_WATER);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

void handleHydrateSnooze() {
  int minutes = server.hasArg("min") ? server.arg("min").toInt() : 10;
  if (minutes <= 0) {
    server.send(400, "text/plain", "min must be positive");
    return;
  }
  waterSnooze(minutes);
  tendShowScreen(SCREEN_WATER);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

// /hydrate/config?interval=45&start=09:00&end=18:00 (any subset).
static bool parseHhmmArg(const char *name, int *minutes) {
  if (!server.hasArg(name)) return true;   // absent = keep current
  int h = -1, m = -1;
  if (sscanf(server.arg(name).c_str(), "%d:%d", &h, &m) != 2 ||
      h < 0 || h > 23 || m < 0 || m > 59) {
    return false;
  }
  *minutes = h * 60 + m;
  return true;
}

void handleHydrateConfig() {
  int interval = server.hasArg("interval") ? server.arg("interval").toInt()
                                           : waterIntervalMin();
  int start = waterStartMin();
  int end = waterEndMin();
  if (!parseHhmmArg("start", &start) || !parseHhmmArg("end", &end)) {
    server.send(400, "text/plain", "start/end must be HH:MM");
    return;
  }
  if (!waterConfigure(interval, start, end)) {
    server.send(400, "text/plain", "bad hydration config");
    return;
  }
  tendShowScreen(SCREEN_WATER);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

void handlePomodoro() {
  String action = server.hasArg("action") ? server.arg("action") : String("toggle");
  if (action == "reset") pomodoroReset();
  else pomodoroToggleStartPause();
  tendShowScreen(SCREEN_POMODORO);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", pomodoroStateName());
}

void handlePet() {
  petPet();
  tendShowScreen(SCREEN_PET);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

void handleSand() {
  String action = server.hasArg("action") ? server.arg("action") : String("pour");
  if (action == "clear") sandClear();
  else sandPour();
  if (lcdScreen != SCREEN_SAND) tendShowScreen(SCREEN_SAND);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

void handleSwarm() {
  String action = server.hasArg("action") ? server.arg("action") : String("scatter");
  if (action == "roam") swarmToggleRoam();
  else swarmScatter();
  if (lcdScreen != SCREEN_SWARM) tendShowScreen(SCREEN_SWARM);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

// Daily-image metadata push from the Mac daemon (this chip cannot afford the
// BearSSL heap to talk to the HTTPS xkcd/NASA APIs itself):
// POST /daily?comicimg=&comictitle=&comicnum=&apodimg=&apodtitle=&apoddate=
void handleDaily() {
  if (server.hasArg("comicimg")) {
    dailySetMeta(false, server.arg("comicimg"), server.arg("comictitle"),
                 server.arg("comicnum"));
  }
  if (server.hasArg("apodimg")) {
    dailySetMeta(true, server.arg("apodimg"), server.arg("apodtitle"),
                 server.arg("apoddate"));
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

// Manual comic/APOD refetch from the dashboard.
void handleRefresh() {
  bool apod = server.hasArg("screen") && server.arg("screen") == "apod";
  dailyRefresh(apod);
  tendShowScreen(apod ? SCREEN_APOD : SCREEN_COMIC);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
}

static void setOtaError() {
  StreamString message;
  Update.printError(message);
  otaError = message.c_str();
  Serial.print("OTA error: ");
  Serial.println(otaError);
}

void handleUpdatePage() {
  server.sendHeader("Connection", "close");
  server.send_P(200, "text/html", UPDATE_HTML);
}

void handleRestart() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "Restarting Clawdmeter...");
  delay(500);
  ESP.restart();
}

void handleFactoryReset() {
  WiFiManager wm;
  wm.resetSettings();
  EEPROM.write(EE_MARKER_ADDR, 0x00);   // invalid marker -> defaults on reboot
  EEPROM.commit();

  server.sendHeader("Connection", "close");
  server.send(200, "text/plain",
              "Factory reset OK. Rebooting to Clawdmeter-setup...");
  delay(500);
  ESP.restart();
}

// ---- ST7789 rendering ----
// The palette lives in tend.h (shared with the ported screen modules).

int textWidth(const String &s, uint8_t size) {
  return (int)s.length() * 6 * size;
}

void printCentered(int y, uint8_t size, const String &s,
                   uint16_t fg, uint16_t bg) {
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor((240 - textWidth(s, size)) / 2, y);
  gfx->print(s);
}

String pctText(int pct) {
  return (pct < 0) ? String("--") : String(pct) + "%";
}

static void drawOtaStatus(const String &line) {
  gfx->fillScreen(C_BLACK);
  gfx->fillRect(0, 0, 240, 34, C_BLUE);
  gfx->setTextColor(C_BLACK, C_BLUE);
  gfx->setTextSize(2);
  gfx->setCursor(14, 10);
  gfx->print("OTA UPDATE");

  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(16, 84);
  gfx->print("firmware");
  gfx->setTextColor(C_GRAY, C_BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(16, 116);
  gfx->print(line);
  String ip = "IP " + WiFi.localIP().toString();
  gfx->setCursor(236 - textWidth(ip, 1), 230);
  gfx->print(ip);
}

String pad2(int v) {
  return (v < 10) ? "0" + String(v) : String(v);
}

// All times are UTC epochs; the caller adds TZ_OFFSET before formatting.
String hhmmss(unsigned long e) {
  unsigned long s = e % 86400UL;
  return pad2(s / 3600) + ":" + pad2((s % 3600) / 60) + ":" + pad2(s % 60);
}
String hhmm(unsigned long e) {
  unsigned long s = e % 86400UL;
  return pad2(s / 3600) + ":" + pad2((s % 3600) / 60);
}

// Right-align text ending at rightX (opaque, so it overprints cleanly).
void printRight(int rightX, int y, uint8_t size, const String &s,
                uint16_t fg, uint16_t bg) {
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(rightX - (int)s.length() * 6 * size, y);
  gfx->print(s);
}

// Tend's hearth mark: a tiny ember flame, drawn from primitives. Shared by the
// screen headers — the fire/hearth metaphor at the heart of the design.
// (cx, cy) is the flame's visual center.
void drawTendFlame(int cx, int cy) {
  gfx->fillCircle(cx, cy + 3, 5, C_TND_EMBER);                          // rounded base
  gfx->fillTriangle(cx - 5, cy + 3, cx + 5, cy + 3, cx, cy - 7, C_TND_EMBER); // tapered tip
  gfx->fillCircle(cx, cy + 4, 2, C_TND_WARN);                          // warm inner glow
}

// Shared screen chrome: paper field, hearth mark, eyebrow, hairline rule.
// Each screen's tick paints the clock at top right via tendHeaderClock.
void tendHeader(const char *eyebrow) {
  gfx->fillScreen(C_TND_PAPER);
  drawTendFlame(16, 13);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(30, 8);
  gfx->print(eyebrow);
  gfx->drawFastHLine(14, 30, 212, C_TND_LINE);
}

void tendHeaderClock() {
  unsigned long e = nowEpoch();
  String t = e ? hhmmss(e + TZ_OFFSET) : String("--:--:--");
  gfx->fillRect(178, 8, 48, 8, C_TND_PAPER);
  printRight(226, 8, 1, t, C_TND_MUTE, C_TND_PAPER);
}

// ---- MUSIC screen: YouTube Music now-playing (scrolling title + artist) --------
// A cream "now playing" card in the same System-7 spirit as the desk sign. The
// title is a millis()-driven marquee (the face/desk poll pattern, NOT a timer
// ISR, so zero IRAM and no WiFi starvation); long artists use the same pattern.
// Two-phase repaint like mac/desk: chrome once on switch-in, then only the title
// strip + artist band repaint (no fillScreen per push).
//
// Text is rendered with bundled Ayuthaya GFXfonts (thai_font.h) so Thai titles
// show real glyphs — the built-in 5x7 font is ASCII-only and Arduino_GFX's
// drawChar() can't index code points > 255. We blit glyphs ourselves from the
// PROGMEM tables, indexing by full UTF-8 code point. Thai combining marks carry
// xAdvance==0 with negative xOffsets, so a faithful per-glyph blit stacks them
// over the base consonant for free (no special combining logic).

// Fallback lever (plan option C): set to 0 to stop scrolling Thai/long titles
// (draw left-aligned, clipped at the edge) if the marquee ever looks wrong.
#define MUSIC_TITLE_SCROLL 1

// English/ASCII on the MUSIC screen renders with the built-in 5x7 font (the
// blocky retro look preferred over the proportional Ayuthaya Latin) scaled by
// these per-face sizes; Thai still blits from thai_font.h. 6*size px per ASCII
// char, so bigger = chunkier and scrolls sooner. Tune on-device; set a size to
// 0 to revert that face to its Ayuthaya Latin GFXfont (`.latin`).
#define MUSIC_TITLE_LATIN_SIZE   3
#define MUSIC_ARTIST_LATIN_SIZE  2
// A built-in glyph fills 7 rows above the baseline (rows 0..6 of the 8-row cell),
// so its top sits at baseline-(7*size-1). Knob if the baseline needs nudging.
#define MUSIC_LATIN_CAP_ROWS     7

// ascent = max px a glyph rises above the baseline (used to seat the baseline in
// the band); the values are the per-size maxima measured across both ranges.
static const MusicFace MUSIC_FACE_TITLE  = { &MusicTitleLatin,  &MusicTitleThai,  28, MUSIC_TITLE_LATIN_SIZE };
static const MusicFace MUSIC_FACE_ARTIST = { &MusicArtistLatin, &MusicArtistThai, 22, MUSIC_ARTIST_LATIN_SIZE };

// "Tend" Now Playing (claude.ai/design): a horizontal header row, a vinyl-disc
// album art on the LEFT with the title + artist meta column to its right, a thin
// ember progress bar, and — per the user's deviation from the source design — a
// two-line LYRIC band filling the bottom where the transport controls would be.

#define MUSIC_PAD            16    // outer paper margin

// header (eyebrow "NOW PLAYING" left, clock right)
#define MUSIC_HDR_Y          14    // baseline of the header row

// vinyl disc art (left). The art + meta + progress block is pulled up to butt
// against the header (no gap below the status bar) so the lyric band can grow.
#define MUSIC_ART_X          16
#define MUSIC_ART_Y          24
#define MUSIC_ART_W          72
#define MUSIC_ART_H          72
#define MUSIC_ART_R          12
#define MUSIC_LABEL_R        11    // ember center label (22 px dia)

// meta column (title + artist marquees), right of the art. The marquee canvases
// live in this column, not the full width, so they never erase the disc.
#define MUSIC_META_X         100
#define MUSIC_META_W         124   // 240 - META_X - PAD
#define MUSIC_TITLE_BAND_Y   30
#define MUSIC_TITLE_BAND_H   34
#define MUSIC_ARTIST_BAND_Y  64
#define MUSIC_ARTIST_BAND_H  30

// progress + elapsed/-remaining times
#define MUSIC_PROGRESS_Y     104
#define MUSIC_TIME_Y         114

// two-line lyric band (bottom): current line in ink, upcoming line dim below.
// Grown taller (was 150..224 / 74 px) now that the meta block sits up top.
#define MUSIC_LYRIC_Y        126   // top of the band
#define MUSIC_LYRIC_H        98    // 126..224
#define MUSIC_LYRIC1_BASE    162   // current-line baseline (artist face)
#define MUSIC_LYRIC2_BASE    200   // upcoming-line baseline

// marquee
#define MUSIC_SCROLL_PXPS    42    // marquee speed, px/sec
#define MUSIC_SCROLL_STEP_PX 2     // 2px frames ~=21 FPS, leaves WiFi loop headroom
#define MUSIC_TITLE_PAD      4
#define MUSIC_SCROLL_HOLD_MS 1000  // readable pause at the beginning/end of a long title
#define MUSIC_SCROLL_GAP     60    // blank px between the title's tail and its wrap-around head

int  musicScrollX = 0;             // px scrolled left from the readable start position
unsigned long musicScrollLastMs = 0;
unsigned long musicScrollHoldUntilMs = 0; // millis() deadline for the start/end hold pause
unsigned long musicTimeLastMs = 0;
int  musicTitleW = 0;              // measured title width (sum of glyph advances)
int  musicArtistW = 0;
bool musicTitleScrolls = false;    // title wider than the panel -> animate
int  musicClockMin = -1;           // last header-clock minute drawn (-1 = none yet)
Arduino_Canvas_Indexed *musicTitleCanvas = nullptr;
bool musicTitleCanvasOk = false;
Arduino_Canvas_Indexed *musicArtistCanvas = nullptr;
bool musicArtistCanvasOk = false;
bool musicArtistScrolls = false;   // artist wider than panel -> animate
int  musicArtistScrollX = 0;
unsigned long musicArtistScrollLastMs = 0;
unsigned long musicArtistScrollHoldUntilMs = 0;
String musicLyricShown1 = "\x01";  // last lyric lines painted (sentinel forces first draw)
String musicLyricShown2 = "\x01";
int  musicLyricScrollX1 = 0;
int  musicLyricScrollX2 = 0;
int  musicLyricW1 = 0;
int  musicLyricW2 = 0;
bool musicLyricScrolls1 = false;
bool musicLyricScrolls2 = false;
unsigned long musicLyricScrollLastMs = 0;
unsigned long musicLyricScrollHoldUntilMs = 0;
// Offscreen buffer for one lyric line. Scrolling Thai lyrics repaint every
// marquee frame; clear+draw straight to the panel flashes white. Reusing one
// 240x40 strip keeps each line atomic without pinning a 240x98 band buffer.
#define MUSIC_LYRIC_SLOT_H   40
#define MUSIC_LYRIC_TOP_PAD  27
Arduino_Canvas_Indexed *musicLyricCanvas = nullptr;
bool musicLyricCanvasOk = false;
bool musicStreamWas = false;       // last lyricsStreamActive() seen by musicTick

// Free the marquee/lyric canvases (~17 KB) while the daemon's frame stream
// owns the panel — the stream needs two 7.2 KB framebuffers instead, and the
// two render paths must not hold heap at the same time. The ensure* helpers
// lazily rebuild the canvases when the on-device fallback returns.
static void musicReleaseCanvases() {
  delete musicTitleCanvas;  musicTitleCanvas = nullptr;  musicTitleCanvasOk = false;
  delete musicArtistCanvas; musicArtistCanvas = nullptr; musicArtistCanvasOk = false;
  delete musicLyricCanvas;  musicLyricCanvas = nullptr;  musicLyricCanvasOk = false;
}

static String musicTitleStr() {
  return npTitle.length() ? npTitle : String("- Not Playing -");
}

// Decode one UTF-8 code point at s[i], advancing i past it. Malformed/truncated
// bytes decode to the raw byte so plain ASCII can never break. Thai is 3-byte.
static uint32_t utf8Next(const String &s, unsigned int &i) {
  uint8_t c = (uint8_t)s[i++];
  uint8_t n;
  uint32_t cp;
  if (c < 0x80) return c;
  else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
  else return c;                                 // stray continuation/invalid lead byte
  for (uint8_t k = 0; k < n; k++) {
    if (i >= s.length() || ((uint8_t)s[i] & 0xC0) != 0x80) return c;  // truncated
    cp = (cp << 6) | ((uint8_t)s[i++] & 0x3F);
  }
  return cp;
}

// Which bundled font (if any) carries this code point.
static const GFXfont *musicGlyphFont(const MusicFace &f, uint32_t cp) {
  if (cp >= 0x0E00 && cp <= 0x0E7F) return f.thai;
  if (cp >= 0x20 && cp <= 0x7E) return f.latin;
  return nullptr;                                // unsupported -> skipped
}

// Safe 16-bit read of a PROGMEM (flash/IROM) field. DO NOT replace with
// pgm_read_word: GFX_Library_for_Arduino's Arduino_GFX.h #undefs the ESP8266
// core's safe pgm_read_word and redefines it as a naive *(uint16_t*) deref
// ("workaround of a15 asm compile error"). A narrow 16-bit load from the
// flash-mapped IROM region faults with LoadStoreErrorCause (exception 3) — it
// was crash-rebooting the device on every switch to the MUSIC screen. memcpy_P
// uses the safe aligned-32-bit path. (pgm_read_byte/_dword are NOT poisoned.)
static inline uint16_t musicReadWordP(const void *flashAddr) {
  uint16_t v;
  memcpy_P(&v, flashAddr, sizeof(v));
  return v;
}

// Sum of glyph advances (combining marks advance 0, so they add no width). This
// is the marquee/centering metric, matching how the glyphs are laid out.
static int musicTextWidth(const MusicFace &f, const String &s) {
  int w = 0;
  unsigned int i = 0;
  while (i < s.length()) {
    uint32_t cp = utf8Next(s, i);
    if (f.latinSize && cp >= 0x20 && cp <= 0x7E) {   // built-in ASCII: fixed cell
      w += 6 * f.latinSize;                           // 5px glyph + 1px gap, scaled
      continue;
    }
    const GFXfont *gf = musicGlyphFont(f, cp);
    if (!gf) continue;
    const GFXglyph *g = (const GFXglyph *)pgm_read_dword(&gf->glyph) +
                        (cp - musicReadWordP(&gf->first));
    w += pgm_read_byte(&g->xAdvance);
  }
  return w;
}

// Blit a UTF-8 string at baseline (x, baselineY). Transparent (only set pixels
// drawn) so combining marks overlay the base cleanly; the caller clears the
// target first. One startWrite/endWrite batches physical SPI writes; for canvases
// it just mirrors the same draw contract. writePixel clips at target edges.
static void musicDrawText(Arduino_GFX *target, const MusicFace &f, int x, int baselineY,
                          const String &s, uint16_t fg) {
  // ASCII top for the built-in font seats its 7-row glyph on the shared baseline.
  const int latinTop = baselineY - (MUSIC_LATIN_CAP_ROWS * f.latinSize - 1);
  if (f.latinSize) { target->setFont(NULL); target->setTextSize(f.latinSize); }
  target->startWrite();
  unsigned int i = 0;
  while (i < s.length()) {
    uint32_t cp = utf8Next(s, i);
    if (f.latinSize && cp >= 0x20 && cp <= 0x7E) {   // built-in ASCII glyph
      // drawChar manages its own transaction; close ours around it (no-op on the
      // RAM canvases, balanced on the direct-panel fallback). bg==fg => transparent.
      target->endWrite();
      target->drawChar(x, latinTop, (unsigned char)cp, fg, fg);
      target->startWrite();
      x += 6 * f.latinSize;
      continue;
    }
    const GFXfont *gf = musicGlyphFont(f, cp);
    if (!gf) continue;
    const GFXglyph *g = (const GFXglyph *)pgm_read_dword(&gf->glyph) +
                        (cp - musicReadWordP(&gf->first));
    uint16_t bo = musicReadWordP(&g->bitmapOffset);
    uint8_t  w  = pgm_read_byte(&g->width);
    uint8_t  h  = pgm_read_byte(&g->height);
    uint8_t  xa = pgm_read_byte(&g->xAdvance);
    int8_t   xo = (int8_t)pgm_read_byte(&g->xOffset);
    int8_t   yo = (int8_t)pgm_read_byte(&g->yOffset);
    const uint8_t *bitmap = (const uint8_t *)pgm_read_dword(&gf->bitmap);
    uint8_t bits = 0, bit = 0;
    for (uint8_t yy = 0; yy < h; yy++) {
      for (uint8_t xx = 0; xx < w; xx++) {
        if (!(bit++ & 7)) bits = pgm_read_byte(&bitmap[bo++]);
        if (bits & 0x80) target->writePixel(x + xo + xx, baselineY + yo + yy, fg);
        bits <<= 1;
      }
    }
    x += xa;
  }
  target->endWrite();
  if (f.latinSize) target->setTextSize(1);   // restore default for other text draws
}

static void musicLayoutTitle() {
  musicTitleW = musicTextWidth(MUSIC_FACE_TITLE, musicTitleStr());
#if MUSIC_TITLE_SCROLL
  musicTitleScrolls = (musicTitleW > MUSIC_META_W);
#else
  musicTitleScrolls = false;
#endif
  musicScrollX = 0;
  musicScrollLastMs = millis();
  musicScrollHoldUntilMs = millis() + MUSIC_SCROLL_HOLD_MS;
}

static void musicLayoutArtist() {
  musicArtistW = musicTextWidth(MUSIC_FACE_ARTIST, npArtist);
  musicArtistScrolls = (musicArtistW > MUSIC_META_W);
  musicArtistScrollX = 0;
  musicArtistScrollLastMs = millis();
  musicArtistScrollHoldUntilMs = millis() + MUSIC_SCROLL_HOLD_MS;
}

static int musicCenteredX(int boxW, int textW, int minX = 0) {
  int x = (boxW - textW) / 2;
  return x > minX ? x : minX;
}

// Title/artist sit in the meta column to the right of the disc. `baseX` is the
// column's left in TARGET coords: 0 for the column canvas (whose origin is
// already MUSIC_META_X), or MUSIC_META_X for the gfx fallback. Short lines are
// centered; long lines scroll, with a second copy one gap past the first so the
// marquee wraps seamlessly. writePixel clips glyphs at the column edge.
static void musicBlitTitle(Arduino_GFX *target, int baseX, int baseline) {
  int x = baseX + (musicTitleScrolls ? -musicScrollX : musicCenteredX(MUSIC_META_W, musicTitleW));
  musicDrawText(target, MUSIC_FACE_TITLE, x, baseline, musicTitleStr(), C_MUS_INK);
  if (musicTitleScrolls)
    musicDrawText(target, MUSIC_FACE_TITLE, x + musicTitleW + MUSIC_SCROLL_GAP,
                  baseline, musicTitleStr(), C_MUS_INK);
}

static void musicBlitArtist(Arduino_GFX *target, int baseX, int baseline) {
  int x = baseX + (musicArtistScrolls ? -musicArtistScrollX : musicCenteredX(MUSIC_META_W, musicArtistW));
  musicDrawText(target, MUSIC_FACE_ARTIST, x, baseline, npArtist, C_MUS_INK_SOFT);
  if (musicArtistScrolls)
    musicDrawText(target, MUSIC_FACE_ARTIST, x + musicArtistW + MUSIC_SCROLL_GAP,
                  baseline, npArtist, C_MUS_INK_SOFT);
}

static bool musicEnsureTitleCanvas() {
  if (musicTitleCanvasOk) return true;
  if (!musicTitleCanvas) {
    musicTitleCanvas = new Arduino_Canvas_Indexed(
        MUSIC_META_W, MUSIC_TITLE_BAND_H, gfx, MUSIC_META_X, MUSIC_TITLE_BAND_Y);
    if (!musicTitleCanvas) return false;
  }
  musicTitleCanvasOk = musicTitleCanvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  return musicTitleCanvasOk;
}

static void musicDrawTitle() {
  int baseline = MUSIC_FACE_TITLE.ascent;
  if (!musicEnsureTitleCanvas()) {
    gfx->fillRect(MUSIC_META_X, MUSIC_TITLE_BAND_Y, MUSIC_META_W, MUSIC_TITLE_BAND_H, C_MUS_PAPER);
    musicBlitTitle(gfx, MUSIC_META_X, MUSIC_TITLE_BAND_Y + baseline);
    return;
  }
  musicTitleCanvas->fillScreen(C_MUS_PAPER);
  musicBlitTitle(musicTitleCanvas, 0, baseline);
  musicTitleCanvas->flush();
}

static bool musicEnsureArtistCanvas() {
  if (musicArtistCanvasOk) return true;
  if (!musicArtistCanvas) {
    musicArtistCanvas = new Arduino_Canvas_Indexed(
        MUSIC_META_W, MUSIC_ARTIST_BAND_H, gfx, MUSIC_META_X, MUSIC_ARTIST_BAND_Y);
    if (!musicArtistCanvas) return false;
  }
  musicArtistCanvasOk = musicArtistCanvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  return musicArtistCanvasOk;
}

static void musicDrawArtist() {
  int baseline = MUSIC_FACE_ARTIST.ascent;
  if (!npArtist.length()) {
    if (musicEnsureArtistCanvas()) {
      musicArtistCanvas->fillScreen(C_MUS_PAPER);
      musicArtistCanvas->flush();
    } else {
      gfx->fillRect(MUSIC_META_X, MUSIC_ARTIST_BAND_Y, MUSIC_META_W, MUSIC_ARTIST_BAND_H, C_MUS_PAPER);
    }
    return;
  }
  if (!musicEnsureArtistCanvas()) {
    gfx->fillRect(MUSIC_META_X, MUSIC_ARTIST_BAND_Y, MUSIC_META_W, MUSIC_ARTIST_BAND_H, C_MUS_PAPER);
    musicBlitArtist(gfx, MUSIC_META_X, MUSIC_ARTIST_BAND_Y + baseline);
    return;
  }
  musicArtistCanvas->fillScreen(C_MUS_PAPER);
  musicBlitArtist(musicArtistCanvas, 0, baseline);
  musicArtistCanvas->flush();
}

static bool musicStopped() {
  return !npTitle.length();
}

static bool musicPausedKnown() {
  return !musicStopped() && npPaused == 1;
}

static bool musicShouldAnimate() {
  // Animate whenever a song is present; deliberately ignore the paused flag so the
  // marquee / indicator / visualizer keep moving even when playback is paused.
  return !musicStopped();
}

// Vinyl-disc album art (static — drawn once with the chrome). A warm bark disc
// with concentric grooves and an ember center label, per the Tend design's
// "no photo, concentric warm rings" art. No EQ animation: the scrolling lyrics
// supply the screen's motion, and the disc has none in the source design.
static void musicDrawArt() {
  int cx = MUSIC_ART_X + MUSIC_ART_W / 2;
  int cy = MUSIC_ART_Y + MUSIC_ART_H / 2;
  gfx->fillRoundRect(MUSIC_ART_X, MUSIC_ART_Y, MUSIC_ART_W, MUSIC_ART_H,
                     MUSIC_ART_R, C_MUS_PAPER_DEEP);     // backing (rounded corners)
  gfx->fillCircle(cx, cy, MUSIC_ART_W / 2 - 1, C_MUS_BARK);
  for (int r = MUSIC_ART_W / 2 - 3; r > MUSIC_LABEL_R + 1; r -= 3)
    gfx->drawCircle(cx, cy, r, C_MUS_BARK_DEEP);          // grooves
  gfx->fillCircle(cx, cy, MUSIC_LABEL_R + 2, C_MUS_PAPER_SOFT);  // label ring
  gfx->fillCircle(cx, cy, MUSIC_LABEL_R, C_MUS_EMBER);          // ember center label
  gfx->fillCircle(cx, cy, 2, C_MUS_PAPER_SOFT);                 // spindle hole
}

static String musicClock(int seconds) {
  if (seconds < 0) seconds = 0;
  int m = seconds / 60;
  int s = seconds % 60;
  String out = String(m) + ":";
  if (s < 10) out += "0";
  out += String(s);
  return out;
}

static int musicDisplayPos() {
  if (npPos < 0) return -1;
  long p = npPos;
  if (npPaused == 0) p += (long)(millis() - npPosBaseMs) / 1000L;
  if (npDur > 0 && p > npDur) p = npDur;
  return (int)p;
}

static bool musicProgressReliable() {
  return !musicStopped() && npDur > 0 && musicDisplayPos() >= 0;
}

// Header row: an "eyebrow" label on the left (NOW PLAYING, or PAUSED to reflect
// state) and the wall clock on the right, both in muted ink on paper — the small
// built-in 5x7 font (the labels are ASCII). Redrawn on switch-in, on each
// pause/identity change, and once per minute for the clock; never per second, so
// it can't flicker. Seeds musicClockMin so the tick knows the minute it painted.
static void musicDrawHeader() {
  gfx->fillRect(0, 0, 240, 22, C_MUS_PAPER);
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUS_INK_MUTED, C_MUS_PAPER);
  gfx->setCursor(MUSIC_PAD, MUSIC_HDR_Y - 6);
  gfx->print(musicPausedKnown() ? F("PAUSED") : F("NOW PLAYING"));
  unsigned long e = nowEpoch();
  String t = e ? hhmm(e + TZ_OFFSET) : String("--:--");
  printRight(224, MUSIC_HDR_Y - 6, 1, t, C_MUS_INK_MUTED, C_MUS_PAPER);
  musicClockMin = e ? (int)(((e + TZ_OFFSET) / 60) % 60) : -1;
}

static void musicDrawTime() {
  // Clear only the zones where elapsed/remaining text lands; the wide middle gap
  // is always paper and never needs clearing (halves the visible flash).
  gfx->fillRect(MUSIC_PAD, MUSIC_TIME_Y - 1, 60, 10, C_MUS_PAPER);   // elapsed zone
  gfx->fillRect(164, MUSIC_TIME_Y - 1, 60, 10, C_MUS_PAPER);          // remaining zone
  if (!musicProgressReliable()) return;
  int pos = musicDisplayPos();
  uint16_t tc = C_MUS_INK_MUTED;
  gfx->setTextSize(1);
  gfx->setTextColor(tc, C_MUS_PAPER);
  gfx->setCursor(MUSIC_PAD, MUSIC_TIME_Y);
  gfx->print(musicClock(pos));
  int rem = npDur - pos;
  printRight(224, MUSIC_TIME_Y, 1, rem > 0 ? "-" + musicClock(rem) : "0:00", tc, C_MUS_PAPER);
}

static void musicDrawProgress() {
  const int x = MUSIC_PAD, y = MUSIC_PROGRESS_Y, w = 240 - MUSIC_PAD * 2, h = 4;
  // No outer fillRect: the full-width track fillRoundRect below overwrites any old
  // fill, eliminating the paper-flash intermediate step.
  gfx->fillRoundRect(x, y, w, h, 2, C_MUS_PAPER_DEEP);   // sunken track (clears old fill)
  if (!musicProgressReliable()) return;
  int pos = musicDisplayPos();
  uint16_t fc = musicPausedKnown() ? C_MUS_INK_FAINT : C_MUS_EMBER;
  int fillW = constrain((int)((long)pos * w / npDur), 0, w);
  if (fillW > 0) gfx->fillRoundRect(x, y, fillW, h, 2, fc);
}

// Progress + time region only (between the art and the lyric band). The lyric
// band has its own change-gated repaint and is NOT cleared here.
// No outer fillRect: musicDrawTime clears its own zones; musicDrawProgress
// redraws the full-width track without needing a pre-clear.
static void musicDrawFooter() {
  musicDrawTime();
  musicDrawProgress();
}

static bool musicCompactLyricText(const String &s, String &out) {
  out = "";
  unsigned int i = 0;
  while (i < s.length()) {
    uint32_t cp = utf8Next(s, i);
    if (cp >= 0x0E00 && cp <= 0x0E7F) return false;  // keep Thai on the real font
    if (cp >= 0x20 && cp <= 0x7E) out += (char)cp;
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0x2026) out += "...";
    else out += '?';
  }
  return out.length() > 0;
}

static bool musicShouldCompactLyric(const String &s, String &compact) {
  const int maxChars = (240 - MUSIC_PAD * 2) / 6;
  return musicCompactLyricText(s, compact) &&
         (musicTextWidth(MUSIC_FACE_ARTIST, s) > (240 - MUSIC_PAD * 2) ||
          (int)compact.length() > maxChars);
}

static bool musicLyricUsesCompact(const String &s) {
  String compact;
  return musicShouldCompactLyric(s, compact);
}

static void musicDrawCompactAsciiLyric(Arduino_GFX *target, int y, const String &s, uint16_t fg) {
  const int maxChars = (240 - MUSIC_PAD * 2) / 6;  // default GFX font, textSize 1
  target->setFont(NULL);
  target->setTextSize(1);
  target->setTextColor(fg, C_MUS_PAPER);

  String rest = s;
  for (int row = 0; row < 3 && rest.length(); row++) {
    String line = rest;
    if ((int)rest.length() > maxChars) {
      int remainingRows = 2 - row;
      int minBreak = (int)rest.length() - remainingRows * maxChars;
      if (minBreak < 1) minBreak = 1;
      int breakAt = -1;
      int searchFrom = maxChars;
      if (searchFrom >= (int)rest.length()) searchFrom = (int)rest.length() - 1;
      for (int i = searchFrom; i >= minBreak; i--) {
        if (rest[i] == ' ') { breakAt = i; break; }
      }
      if (breakAt < 1 || breakAt > maxChars) breakAt = maxChars;
      line = rest.substring(0, breakAt);
      int start = breakAt;
      while (start < (int)rest.length() && rest[start] == ' ') start++;
      rest = rest.substring(start);
    } else {
      rest = "";
    }
    if ((int)line.length() > maxChars) line = line.substring(0, maxChars);
    target->setCursor(musicCenteredX(240, line.length() * 6, MUSIC_PAD), y + row * 10);
    target->print(line);
  }
}

// Draw one lyric line into `target`. `yOff` translates absolute screen baselines
// into the target's coordinate space: 0 for the panel, MUSIC_LYRIC_Y for the band
// canvas (whose origin is the band top).
static void musicDrawLyricLine(Arduino_GFX *target, int yOff, const String &s,
                               int baseline, uint16_t fg,
                               int scrollX, int textW, bool scrolls) {
  String compact;
  if (musicShouldCompactLyric(s, compact)) {
    musicDrawCompactAsciiLyric(target, baseline - yOff - 27, compact, fg);
    return;
  }
  int x = scrolls ? MUSIC_PAD - scrollX : musicCenteredX(240, textW, MUSIC_PAD);
  musicDrawText(target, MUSIC_FACE_ARTIST, x, baseline - yOff, s, fg);
  if (scrolls)
    musicDrawText(target, MUSIC_FACE_ARTIST, x + textW + MUSIC_SCROLL_GAP,
                  baseline - yOff, s, fg);
}

static void musicLayoutLyrics(const String &l1, const String &l2) {
  musicLyricW1 = musicLyricUsesCompact(l1) ? 0 : musicTextWidth(MUSIC_FACE_ARTIST, l1);
  musicLyricW2 = musicLyricUsesCompact(l2) ? 0 : musicTextWidth(MUSIC_FACE_ARTIST, l2);
  musicLyricScrolls1 = l1.length() && musicLyricW1 > (240 - MUSIC_PAD * 2);
  musicLyricScrolls2 = l2.length() && musicLyricW2 > (240 - MUSIC_PAD * 2);
  musicLyricScrollX1 = 0;
  musicLyricScrollX2 = 0;
  musicLyricScrollLastMs = millis();
  musicLyricScrollHoldUntilMs = millis() + MUSIC_SCROLL_HOLD_MS;
}

static bool musicEnsureLyricCanvas() {
  if (musicLyricCanvasOk) return true;
  if (!musicLyricCanvas) {
    musicLyricCanvas = new Arduino_Canvas_Indexed(
        240, MUSIC_LYRIC_SLOT_H, gfx, 0, 0);
    if (!musicLyricCanvas) return false;
  }
  musicLyricCanvasOk = musicLyricCanvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  return musicLyricCanvasOk;
}

static void musicPaintLyricSlot(const String &s, int baseline, uint16_t fg,
                                int scrollX, int textW, bool scrolls) {
  int y = baseline - MUSIC_LYRIC_TOP_PAD;
  if (musicEnsureLyricCanvas()) {
    musicLyricCanvas->fillScreen(C_MUS_PAPER);
    musicDrawLyricLine(musicLyricCanvas, y, s, baseline, fg, scrollX, textW, scrolls);
    gfx->drawIndexedBitmap(0, y, musicLyricCanvas->getFramebuffer(),
                           musicLyricCanvas->getColorIndex(),
                           240, MUSIC_LYRIC_SLOT_H);
  } else {
    gfx->fillRect(0, y, 240, MUSIC_LYRIC_SLOT_H, C_MUS_PAPER);
    musicDrawLyricLine(gfx, 0, s, baseline, fg, scrollX, textW, scrolls);
  }
}

static void musicPaintLyrics() {
  musicPaintLyricSlot(musicLyricShown1, MUSIC_LYRIC1_BASE, C_MUS_INK,
                      musicLyricScrollX1, musicLyricW1, musicLyricScrolls1);
  musicPaintLyricSlot(musicLyricShown2, MUSIC_LYRIC2_BASE, C_MUS_INK_MUTED,
                      musicLyricScrollX2, musicLyricW2, musicLyricScrolls2);
}

// Two-line lyric band at the bottom (the user's deviation from the source design,
// which has transport controls here): the current line in ink, the upcoming line
// dim below it. Long English/Latin lines use the compact built-in font and wrap
// inside their lyric slot; Thai lines keep the bundled bitmap font so combining
// marks remain correct. Repaints in place only when a line actually changes
// (push / auto-promote), never on the 1 s tick, so the band never flickers.
// This keeps the same opaque-repaint discipline as the title/artist bands.
static void musicDrawLyrics() {
  String l1 = npLyric, l2 = npLyric2;
  if (musicStopped()) { l1 = ""; l2 = ""; }
  if (l1 == musicLyricShown1 && l2 == musicLyricShown2) return;
  musicLyricShown1 = l1;
  musicLyricShown2 = l2;
  musicLayoutLyrics(l1, l2);
  musicPaintLyrics();
}

static void drawMusicChrome() {
  gfx->fillScreen(C_MUS_PAPER);
  musicDrawHeader();
  musicDrawArt();
  musicLyricShown1 = "\x01";        // force the lyric band to repaint after the clear
  musicLyricShown2 = "\x01";
  musicTimeLastMs = 0;
  musicDrawFooter();
}

void drawMusic() {
  if (!musicChromeReady) { drawMusicChrome(); musicChromeReady = true; }
  else musicDrawHeader();   // refresh eyebrow (play/pause) + clock on pause/identity change
  musicLayoutTitle();
  musicLayoutArtist();
  musicDrawTitle();
  musicDrawArtist();
  musicDrawFooter();
  musicDrawLyrics();
}

// Per-loop tick: refresh the clock/progress, promote synced lyric lines on time,
// and advance the title/artist marquees. No timer ISR — all millis()-polled.
static void musicTick() {
  // Frame stream first: while the Mac's lyrics daemon is pushing rendered
  // frames they own the panel, and every on-device draw below is suppressed
  // (it would scribble over the streamed pixels). When the stream drops (or
  // the daemon clears on track end) repaint the on-device fallback once.
  lyricsStreamTick();
  bool streaming = lyricsStreamActive();
  if (streaming != musicStreamWas) {
    musicStreamWas = streaming;
    if (streaming) {
      musicReleaseCanvases();
    } else {
      musicChromeReady = false;
      drawMusic();
    }
  }
  if (streaming) return;

  unsigned long now = millis();

  // Progress bar / time counter (once per second)
  if (now - musicTimeLastMs >= 1000UL) {
    musicTimeLastMs = now;
    musicDrawTime();
    musicDrawProgress();
    // Header clock: repaint only when the displayed minute rolls over.
    unsigned long e = nowEpoch();
    if (e) {
      int mn = (int)(((e + TZ_OFFSET) / 60) % 60);
      if (mn != musicClockMin) musicDrawHeader();
    }
    // Synced-lyric auto-promote: when the interpolated position reaches the
    // upcoming line's timestamp, slide it up to current (the daemon refills the
    // next line on its following push). This keeps the swap on-beat between the
    // daemon's coarser pushes, using the same interpolated clock as the progress.
    if (npLyricAt >= 0 && npPaused == 0 && musicDisplayPos() >= npLyricAt) {
      npLyric = npLyric2;
      npLyric2 = "";
      npLyricAt = -1;
      musicDrawLyrics();
    }
  }

  // Title marquee — seamless infinite loop: scroll by (titleW + gap) then reset to 0
  // so the wrap-around second copy lands exactly where the first started.
  if (musicTitleScrolls && musicShouldAnimate() && now >= musicScrollHoldUntilMs) {
    int scrollMax = musicTitleW + MUSIC_SCROLL_GAP;
    if (musicScrollX >= scrollMax) {
      musicScrollX = 0;
      musicScrollLastMs = now;
      musicScrollHoldUntilMs = now + MUSIC_SCROLL_HOLD_MS;
      musicDrawTitle();
    } else {
      long span = (long)(now - musicScrollLastMs) * MUSIC_SCROLL_PXPS / 1000L;
      if (span >= MUSIC_SCROLL_STEP_PX) {
        musicScrollLastMs = now;
        musicScrollX += (int)span;
        if (musicScrollX >= scrollMax) {
          musicScrollX = scrollMax;
          musicScrollHoldUntilMs = now + MUSIC_SCROLL_HOLD_MS;
        }
        musicDrawTitle();
      }
    }
  }

  // Artist marquee — same seamless-loop pattern as the title
  if (musicArtistScrolls && musicShouldAnimate() && now >= musicArtistScrollHoldUntilMs) {
    int artistScrollMax = musicArtistW + MUSIC_SCROLL_GAP;
    if (musicArtistScrollX >= artistScrollMax) {
      musicArtistScrollX = 0;
      musicArtistScrollLastMs = now;
      musicArtistScrollHoldUntilMs = now + MUSIC_SCROLL_HOLD_MS;
      musicDrawArtist();
    } else {
      long span = (long)(now - musicArtistScrollLastMs) * MUSIC_SCROLL_PXPS / 1000L;
      if (span >= MUSIC_SCROLL_STEP_PX) {
        musicArtistScrollLastMs = now;
        musicArtistScrollX += (int)span;
        if (musicArtistScrollX >= artistScrollMax) {
          musicArtistScrollX = artistScrollMax;
          musicArtistScrollHoldUntilMs = now + MUSIC_SCROLL_HOLD_MS;
        }
        musicDrawArtist();
      }
    }
  }

  // Lyric marquee for Thai/non-compact overflow. English long lines already wrap
  // in compact text, so only custom-font lyric lines reach this branch.
  if ((musicLyricScrolls1 || musicLyricScrolls2) &&
      musicShouldAnimate() && now >= musicLyricScrollHoldUntilMs) {
    long span = (long)(now - musicLyricScrollLastMs) * MUSIC_SCROLL_PXPS / 1000L;
    if (span >= MUSIC_SCROLL_STEP_PX) {
      bool changed = false;
      musicLyricScrollLastMs = now;
      if (musicLyricScrolls1) {
        int scrollMax = musicLyricW1 + MUSIC_SCROLL_GAP;
        musicLyricScrollX1 += (int)span;
        if (musicLyricScrollX1 >= scrollMax) {
          musicLyricScrollX1 = 0;
          musicLyricScrollHoldUntilMs = now + MUSIC_SCROLL_HOLD_MS;
        }
        changed = true;
      }
      if (musicLyricScrolls2) {
        int scrollMax = musicLyricW2 + MUSIC_SCROLL_GAP;
        musicLyricScrollX2 += (int)span;
        if (musicLyricScrollX2 >= scrollMax) {
          musicLyricScrollX2 = 0;
          musicLyricScrollHoldUntilMs = now + MUSIC_SCROLL_HOLD_MS;
        }
        changed = true;
      }
      if (changed) musicPaintLyrics();
    }
  }
}

// Full repaint of the current screen (chrome + content). Used on mode switch,
// after OTA failure, and by dashboard actions that want the effect visible.
void drawMeter() {
  switch (lcdScreen) {
    case SCREEN_MUSIC:
      musicChromeReady = false;
      musicStreamWas = false;   // fallback just painted; stream re-claims via tick
      drawMusic();
      break;
    case SCREEN_POMODORO: pomodoroScreenBegin(); break;
    case SCREEN_WATER:    waterScreenBegin(); break;
    case SCREEN_STATS:    statsScreenBegin(); break;
    case SCREEN_PET:      petScreenBegin(); break;
    case SCREEN_SAND:     sandScreenBegin(); break;
    case SCREEN_SWARM:    swarmScreenBegin(); break;
    case SCREEN_COMIC:
    case SCREEN_APOD:     dailyScreenBegin(); break;
    default:              clockScreenBegin(); break;
  }
}

void tendShowScreen(uint8_t screen) {
  // Leaving MUSIC drops the lyrics frame stream and frees its buffers; the
  // daemon keeps rendering for its other boards and we resync on return.
  if (lcdScreen == SCREEN_MUSIC && screen != SCREEN_MUSIC) lyricsStreamStop();
  lcdScreen = screen;
  drawMeter();
}

void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  if (!otaUpdateOk || Update.hasError() || otaError.length()) {
    String msg = otaError.length() ? otaError : String("unknown update error");
    server.send(500, "text/plain", "Update failed: " + msg);
    otaInProgress = false;
    otaUpdateOk = false;
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);
    drawMeter();
    return;
  }

  server.send(200, "text/html",
              "<!doctype html><meta charset='utf-8'>"
              "<body style='font-family:system-ui'>Update OK. Rebooting...</body>");
  delay(500);
  ESP.restart();
}

void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    otaUpdateOk = false;
    otaError = "";
    Serial.printf("OTA update: %s\n", upload.filename.c_str());
    lyricsStreamStop();   // free the stream's 14 KB and quiet its socket for OTA
    WiFiUDP::stopAll();
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    backlightStopForFlash();   // load-bearing: PWM ISR + flash write = reset mid-OTA
    drawOtaStatus("uploading firmware...");

    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace, U_FLASH)) {
      setOtaError();
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && !otaError.length()) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      setOtaError();
    }
  } else if (upload.status == UPLOAD_FILE_END && !otaError.length()) {
    if (Update.end(true)) {
      Serial.printf("OTA success: %u bytes\n", upload.totalSize);
      otaUpdateOk = true;
      drawOtaStatus("success, rebooting...");
    } else {
      setOtaError();
      drawOtaStatus("failed");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    otaError = "upload aborted";
    otaInProgress = false;
    otaUpdateOk = false;
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);
    setBacklight(lcdBrightness);   // OTA failed, no reboot: restore the live PWM level
    drawOtaStatus("aborted");
  }

  yield();
}

void setup() {
  Serial.begin(115200);

  // Capture why we just reset BEFORE anything else can clobber it. A repeated
  // "Exception"/"Watchdog" here = a crash loop; "Power on"/"Brown out" = power.
  // Exposed in /usage.json as "rst" so it's readable over the network (no USB).
  bootReason = ESP.getResetReason();
  bootInfo = ESP.getResetInfo();
  Serial.println();
  Serial.print("Reset reason: "); Serial.println(bootReason);
  Serial.print("Reset info: ");   Serial.println(bootInfo);

  // --- Display init (mirrors the GeekMagic open firmware) ---
  pinMode(LCD_BL, OUTPUT);
  EEPROM.begin(EE_SIZE);
  eepromInitLayout();
  applyBrightness(loadBrightness(), false);  // PWM-dimmed; full-on ran the panel hot
  // Arduino_GFX defaults ST7789 on ESP8266 to SPI_MODE2; this panel needs mode 3.
  // Start the bus ourselves and tell gfx->begin() not to reconfigure it.
  bus->begin(40000000, SPI_MODE3);
  gfx->begin(GFX_SKIP_DATABUS_BEGIN);  // hardware reset (RST) + ST7789 init
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setTextSize(2);
  gfx->setCursor(12, 100);
  gfx->print("starting...");

  // Tries the saved network; if it can't connect it opens a "Clawdmeter-setup"
  // hotspot — join it from your phone to pick a new WiFi. No re-flashing needed.
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);          // give up after 3 min and reboot/retry
  wm.setAPCallback([](WiFiManager *m) {    // show setup hint on the screen
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setTextSize(2);
    gfx->setCursor(12, 70);  gfx->print("Setup WiFi:");
    gfx->setCursor(12, 100); gfx->print("join hotspot");
    gfx->setTextColor(0xFD20, 0x0000);
    gfx->setCursor(12, 130); gfx->print("Clawdmeter-setup");
  });
  if (!wm.autoConnect("Clawdmeter-setup")) {
    Serial.println("WiFi setup timed out, restarting...");
    ESP.restart();
  }
  Serial.print("Connected: http://");
  Serial.println(WiFi.localIP());

  // Let the radio idle between AP beacons instead of full active RX. Combined with
  // the delay() in loop() (which yields to the SDK), average WiFi current drops a
  // lot -> the ESP8266 runs cooler. CPU stays on, so the web server stays responsive.
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);

  // NTP in UTC (we apply the UTC+7 offset at display time). This gives the wait
  // screen a clock before the daemon ever pushes; daemon time takes over later.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Stable hostname so the daemon/browser don't chase IPs: http://clawdmeter.local/
  if (MDNS.begin("clawdmeter")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Also at: http://clawdmeter.local/");
  }

  drawMeter();   // shows "waiting for daemon..." + the device IP until data arrives

  server.on("/", handleRoot);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/usage", HTTP_GET, handleUsage);   // GET allowed too, handy for testing
  server.on("/usage.json", handleUsageJson);
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/mode", HTTP_POST, handleMode);
  server.on("/hydrate/log", handleHydrateLog);
  server.on("/hydrate/now", handleHydrateNow);
  server.on("/hydrate/snooze", handleHydrateSnooze);
  server.on("/hydrate/config", handleHydrateConfig);
  server.on("/pomodoro", handlePomodoro);
  server.on("/pet", handlePet);
  server.on("/sand", handleSand);
  server.on("/swarm", handleSwarm);
  server.on("/daily", handleDaily);
  server.on("/refresh", handleRefresh);
  server.on("/nowplaying", HTTP_GET, handleNowPlaying);
  server.on("/nowplaying", HTTP_POST, handleNowPlaying);
  server.on("/restart", HTTP_GET, handleRestart);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/factory-reset", HTTP_GET, handleFactoryReset);
  server.on("/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleFirmwareUpload);
  Serial.println("OTA update: http://clawdmeter.local/update");
  server.begin();
}

void loop() {
  if (!otaInProgress) MDNS.update();
  server.handleClient();
  if (otaInProgress) {
    // Keep feeding the SDK/WiFi stack between upload chunks. Returning here
    // without a yield can make OTA unstable on slower or lossy WiFi links.
    delay(2);
    return;
  }

  // Background countdowns advance regardless of the visible screen (the ESP32
  // ran pomodoro_tick unconditionally too). With no chime on this box, a
  // firing alert pulls its own screen forward via tendShowScreen.
  pomodoroTick();
  waterTick();

  // Every screen is millis()-polled from here (never a timer ISR, so nothing
  // fights the WiFi/TCP stack and no IRAM is spent).
  switch (lcdScreen) {
    case SCREEN_MUSIC:    musicTick(); break;
    case SCREEN_POMODORO: pomodoroScreenTick(); break;
    case SCREEN_WATER:    waterScreenTick(); break;
    case SCREEN_STATS:    statsScreenTick(); break;
    case SCREEN_PET:      petScreenTick(); break;
    case SCREEN_SAND:     sandScreenTick(); break;
    case SCREEN_SWARM:    swarmScreenTick(); break;
    case SCREEN_COMIC:
    case SCREEN_APOD:     dailyScreenTick(); break;
    default:              clockScreenTick(); break;
  }

  // Yield to the SDK so WIFI_MODEM_SLEEP can actually engage between beacons.
  // 2 ms is invisible to a 1 s clock tick and a page polled once per second.
  delay(2);
}
