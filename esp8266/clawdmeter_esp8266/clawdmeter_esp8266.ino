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
String npLyric2 = "";            // upcoming lyric line (kept for the dashboard's /usage.json)
int npLyricAt = -1;              // reserved lyric timing field (no longer rendered on-device)
// Interpolated playback position, surfaced in /usage.json for the dashboard's
// Now Playing panel. Defined with the MUSIC screen below; declared here because
// handleUsageJson() (above that block) reads it.
static int musicDisplayPos();
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
    <button class="btn" data-scr="music" data-get="/mode?screen=music">lyrics</button>
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
  <div class="ctx" id="cx-music"><div class="mut" style="margin-top:12px">now showing on the lcd &middot; streamed lyrics from the mac daemon (:8766)</div></div>
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
  <div class="label"><span class="eb">now playing</span><span class="mut">push to /nowplaying</span></div>
  <div class="cfg" style="grid-template-columns:1fr 1fr">
    <input id="npT" type="text" placeholder="title">
    <input id="npA" type="text" placeholder="artist">
    <input id="npP" type="number" min="0" placeholder="pos (s)">
    <input id="npD" type="number" min="0" placeholder="dur (s)">
  </div>
  <div class="acts"><label class="mut" style="display:flex;align-items:center;gap:6px"><input type="checkbox" id="npX"> paused</label><button class="btn" id="npBtn">push</button></div>
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
$('npBtn').onclick=function(e){e.preventDefault();
  var q=[];function add(k,v){q.push(k+'='+encodeURIComponent(v))}
  add('title',$('npT').value);add('artist',$('npA').value);
  if($('npP').value!=='')add('pos',$('npP').value);
  if($('npD').value!=='')add('dur',$('npD').value);
  add('paused',$('npX').checked?1:0);
  fetch('/nowplaying?'+q.join('&'),{cache:'no-store'}).then(tick).catch(function(_){})};
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
// Two jobs, both data-only — nothing is painted on-device here anymore:
//   1. Learn the pushing Mac's IP so the MUSIC screen can dial its lyrics
//      daemon for the frame stream (lyricsStreamNoteHost).
//   2. Stash the song fields for the dashboard's Now Playing panel, which reads
//      them back from /usage.json. The physical MUSIC screen is rendered solely
//      by the daemon's streamed frames (lyrics_stream.cpp); when no frame is
//      flowing it shows a placeholder, never an on-device now-playing card.
void handleNowPlaying() {
  lyricsStreamNoteHost(server.client().remoteIP());
  String oldTitle = npTitle;
  String oldArtist = npArtist;
  int oldDur = npDur;
  if (server.hasArg("title")) npTitle = server.arg("title");
  if (server.hasArg("artist")) npArtist = server.arg("artist");
  if (server.hasArg("pos")) {
    npPos = server.arg("pos").toInt();
    npPosBaseMs = millis();
  }
  if (server.hasArg("dur")) npDur = server.arg("dur").toInt();
  if (server.hasArg("paused")) npPaused = constrain(server.arg("paused").toInt(), -1, 1);
  bool trackChanged = oldTitle != npTitle || oldArtist != npArtist;
  bool identityChanged = trackChanged || oldDur != npDur;
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

// ---- MUSIC screen: daemon-streamed lyrics only -----------------------------
// The MUSIC screen is rendered entirely by lyrics_display_daemon.py: it streams
// 240x240 1-bpp frames (Core Text — real Thai shaping + syllable karaoke, plus
// the full-screen album-art hero frames) over the WebSocket in lyrics_stream.cpp.
// The old on-device now-playing renderer (marquees, lyric bands, bundled Thai
// font) is gone. Whenever no daemon frame owns the panel we paint a simple Tend
// paper placeholder (eyebrow + status line + clock) and wait for the stream.
// Run the daemon with --insecure so it speaks the proto=1 path this chip's client
// uses (see lyrics_stream.cpp for why the secure proto=2 heap won't fit here).

static bool musicStreamWas = false;        // last lyricsStreamActive() seen by musicTick
static bool musicConnWas = false;          // last lyricsStreamConnecting() (drives the eyebrow)
static uint8_t musicConnDots = 0;          // 0-3 dot pulse for the "connecting" status line
static unsigned long musicTickLastMs = 0;  // 1 Hz cadence for the placeholder clock/dots

// Interpolated playback position, surfaced in /usage.json for the dashboard's Now
// Playing panel (never drawn on-device). Forward-declared up by the np* globals so
// handleUsageJson(), which sits above this block, can read it.
static int musicDisplayPos() {
  if (npPos < 0) return -1;
  long p = npPos;
  if (npPaused == 0) p += (long)(millis() - npPosBaseMs) / 1000L;
  if (npDur > 0 && p > npDur) p = npDur;
  return (int)p;
}

// The status line under the header rule: "reaching the lyrics daemon..." once a
// Mac IP is known and the socket is still dialing; otherwise "waiting for
// lyrics" — worded to read true both before any daemon has pushed and when a
// connected daemon is idle between tracks (it sends a "clear", dropping the
// frame while the socket stays open). Clears only its own band so it can't flash
// the rest of the paper.
#define MUSIC_STATUS_Y 108
static void musicDrawPlaceholderStatus() {
  gfx->fillRect(0, MUSIC_STATUS_Y - 2, 240, 16, C_TND_PAPER);
  String msg = lyricsStreamConnecting() ? String("reaching the lyrics daemon")
                                        : String("waiting for lyrics");
  if (lyricsStreamConnecting())
    for (uint8_t i = 0; i < (musicConnDots % 4); i++) msg += '.';
  printCentered(MUSIC_STATUS_Y, 1, msg, C_TND_MUTE, C_TND_PAPER);
}

// Full placeholder paint: Tend chrome (paper + hearth mark + eyebrow + hairline),
// the status line, and the header clock. Shown on switch-in and whenever the
// stream hands the panel back to us.
static void musicDrawPlaceholder() {
  musicConnWas = lyricsStreamConnecting();
  tendHeader(musicConnWas ? "connecting" : "now playing");
  musicDrawPlaceholderStatus();
  tendHeaderClock();
}

// Per-loop tick. Pump the socket; the stream blits its own frames straight to the
// panel while it owns it (musicTick paints nothing then). On the transition back
// to no-stream, repaint the placeholder once. While idle: repaint the whole
// placeholder if the connecting-state (and thus the eyebrow) flipped, otherwise
// keep the clock fresh and pulse the "connecting..." dots at 1 Hz.
static void musicTick() {
  lyricsStreamTick();
  bool streaming = lyricsStreamActive();
  if (streaming != musicStreamWas) {
    musicStreamWas = streaming;
    if (!streaming) musicDrawPlaceholder();   // stream dropped -> back to placeholder
  }
  if (streaming) return;                        // frames own the panel

  unsigned long now = millis();
  bool conn = lyricsStreamConnecting();
  if (conn != musicConnWas) {                   // eyebrow text changed -> full repaint
    musicDrawPlaceholder();
    musicTickLastMs = now;
    return;
  }
  if (now - musicTickLastMs >= 1000UL) {
    musicTickLastMs = now;
    if (conn) musicConnDots++;
    musicDrawPlaceholderStatus();               // pulse dots / keep the line fresh
    tendHeaderClock();                          // cheap: clears only the clock rect
  }
}

// Full repaint of the current screen (chrome + content). Used on mode switch,
// after OTA failure, and by dashboard actions that want the effect visible.
void drawMeter() {
  switch (lcdScreen) {
    case SCREEN_MUSIC:
      musicStreamWas = false;   // stream re-claims the panel via musicTick
      musicTickLastMs = 0;
      musicDrawPlaceholder();
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
