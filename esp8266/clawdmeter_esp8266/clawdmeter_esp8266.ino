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
extern "C" {
#include <user_interface.h>
#include <wpa2_enterprise.h>      // 802.1X (WPA2-Enterprise) station auth, in the core's SDK
}

ESP8266WebServer server(80);

// Firmware version. Single source of truth: it is published as "fw" in
// /usage.json, and both the dashboard and the OTA page read it from there
// rather than carrying their own copy -- a second literal in the HTML would
// be gzipped into index_html_gz.h and drift silently. Bumping this line is
// the whole change.
#define FW_VERSION "1.1.0"

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

// EEPROM layout v2 (see tend.h): brightness; bytes 2..15 stay reserved.
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

// Commit wrapped in the flash guard. See backlightStopForFlash above for why.
void tendEepromCommit() {
  backlightStopForFlash();
  EEPROM.commit();
  setBacklight(lcdBrightness);
}

// Stamp the v2 marker, migrating a v1 (brightness-only) chip in place; on a
// fresh chip default the brightness too. Bytes 2..15 held water config and pet
// counters before those screens were removed and are now left untouched.
// Runs once in setup before any screen reads config.
static void eepromInitLayout() {
  const uint8_t marker = EEPROM.read(EE_MARKER_ADDR);
  if (marker == EEPROM_MARKER) return;
  if (marker != EEPROM_MARKER_V1) {
    EEPROM.write(EE_BRIGHTNESS_ADDR, LCD_BRIGHTNESS);
  }
  EEPROM.write(EE_MARKER_ADDR, EEPROM_MARKER);
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

// WiFi provisioning. With no saved network — first boot, or after /wifi-reset —
// the box opens this hotspot and runs a captive setup portal on it; see setup().
#define WIFI_SETUP_AP "Clawdmeter-setup"
#define WIFI_PORTAL_TIMEOUT_S 180   // give up, reboot, and retry the saved network

// WPA2-Enterprise (802.1X: PEAP / TTLS with MSCHAPv2). Office networks that ask
// for a username *and* a password at join time. WiFiManager's portal only has a
// password box, so the portal grows one optional "username" field: left blank it
// is the plain WPA2-PSK flow as before; filled in, the board joins with 802.1X.
// The SDK keeps none of the enterprise settings across a reset, so they live in
// EEPROM (see tend.h) and are re-applied every boot before the first connect.
// No CA certificate is pinned -- the server certificate is not validated -- and
// EAP-TLS (client certificate) networks are not supported.
#define EEPROM_EAP_MARKER 0xE1
#define WIFI_EAP_CONNECT_MS (30UL * 1000UL)   // EAP handshake is slower than PSK

struct EapConfig {
  char ssid[33];
  char user[65];
  char pass[65];
};
static EapConfig eapCfg;
static bool eapOn = false;   // 802.1X credentials loaded and applied to the SDK

static void eepromReadStr(int addr, char *dst, size_t len) {
  for (size_t i = 0; i < len; i++) dst[i] = (char)EEPROM.read(addr + i);
  dst[len - 1] = '\0';
}

static void eepromWriteStr(int addr, const String &src, size_t len) {
  for (size_t i = 0; i < len; i++) {
    EEPROM.write(addr + i, i < src.length() && i < len - 1 ? (uint8_t)src[i] : 0);
  }
}

static bool eapLoad() {
  if (EEPROM.read(EE_EAP_FLAG_ADDR) != EEPROM_EAP_MARKER) return false;
  eepromReadStr(EE_EAP_SSID_ADDR, eapCfg.ssid, sizeof(eapCfg.ssid));
  eepromReadStr(EE_EAP_USER_ADDR, eapCfg.user, sizeof(eapCfg.user));
  eepromReadStr(EE_EAP_PASS_ADDR, eapCfg.pass, sizeof(eapCfg.pass));
  return eapCfg.ssid[0] && eapCfg.user[0];
}

// Arm the SDK for 802.1X. Takes effect on the next station connect, whoever
// issues it: our WiFi.begin(), WiFiManager's, or the watchdog's reconnect().
static void eapApply() {
  wifi_station_set_wpa2_enterprise_auth(1);
  wifi_station_clear_enterprise_identity();
  wifi_station_clear_enterprise_username();
  wifi_station_clear_enterprise_password();
  wifi_station_clear_enterprise_cert_key();
  wifi_station_clear_enterprise_ca_cert();
  // Same string for the outer (identity) and inner (username) EAP names: what
  // the user typed is what their laptop would send for both.
  wifi_station_set_enterprise_identity((u8 *)eapCfg.user, strlen(eapCfg.user));
  wifi_station_set_enterprise_username((u8 *)eapCfg.user, strlen(eapCfg.user));
  wifi_station_set_enterprise_password((u8 *)eapCfg.pass, strlen(eapCfg.pass));
  // No CA cert to check against anyway, and before NTP the clock reads 1970.
  wifi_station_set_enterprise_disable_time_check(true);
  eapOn = true;
}

static void eapDisable() {
  wifi_station_clear_enterprise_identity();
  wifi_station_clear_enterprise_username();
  wifi_station_clear_enterprise_password();
  wifi_station_set_wpa2_enterprise_auth(0);
  eapOn = false;
}

// Join the stored 802.1X network ourselves. WiFi.begin(ssid) with no password,
// not WiFiManager's begin(ssid, pass): a password makes the core set a WPA-PSK
// auth threshold, which an enterprise AP need not satisfy. persistent(false)
// keeps this out of the SDK's flash sector -- nothing to write, and no flash
// write while the backlight PWM is running (see backlightStopForFlash).
static bool eapConnect() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(eapCfg.ssid);
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_EAP_CONNECT_MS) {
    delay(100);
  }
  WiFi.persistent(true);
  return WiFi.status() == WL_CONNECTED;
}

// Wipe the stored 802.1X credentials (the /wifi-reset and /factory-reset path).
// Caller commits.
static void eapForget() {
  EEPROM.write(EE_EAP_FLAG_ADDR, 0x00);
  eepromWriteStr(EE_EAP_PASS_ADDR, String(), sizeof(eapCfg.pass));
}

// WiFi watchdog. autoConnect() runs exactly once, in setup(); before this,
// nothing in loop() ever re-read WiFi.status(), so an association lost *after*
// boot -- router reboot, channel change, a weak-signal eviction -- parked the
// panel on "wifi lost" until somebody power-cycled the box. Every other recovery
// path in this firmware is automatic; this one was a hole.
//
// Two stages, deliberately far apart:
//
//   reconnect()   cheap, non-destructive (wifi_station_disconnect + connect; it
//                 does NOT clear credentials), so it fires early and repeats.
//   ESP.restart() held back a long way on purpose. A reboot lands in
//                 wm.autoConnect(), which BLOCKS in the captive portal for
//                 WIFI_PORTAL_TIMEOUT_S and then reboots again. Restart eagerly
//                 and a fifteen-minute router outage becomes a hotspot loop that
//                 also puts /update out of reach -- strictly worse than a screen
//                 that says "wifi lost" until the router comes back.
#define WIFI_WATCHDOG_RECONNECT_MS  (30UL * 1000UL)         // down this long -> reconnect()
#define WIFI_WATCHDOG_RETRY_MS      (30UL * 1000UL)         // ... and again this often
#define WIFI_WATCHDOG_RESTART_MS    (10UL * 60UL * 1000UL)  // still down -> ESP.restart()

static unsigned long wifiOkMs = 0;         // millis() of the last confirmed association
static unsigned long wifiNextRetryMs = 0;  // next reconnect() attempt, 0 = none pending
static bool wifiWasDown = false;           // edge flag: drives the log and the mDNS refresh
// Surfaced in /usage.json, because none of the watchdog's Serial output is
// reachable: this board is updated over the air with no USB attached, and a
// watchdog reboot is indistinguishable from any other -- ESP.getResetReason()
// says "Software/System restarted" for /restart and /wifi-reset too. Without
// these two counters "it rebooted at some point" is all anyone could ever learn.
static unsigned long wifiDrops = 0;        // associations lost since boot
static bool wifiMdnsRefreshOk = true;      // did the last notifyAPChange() take?

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
// Now Playing panel. Defined with the lyrics screen below; declared here because
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
<title>clawdmeter &middot; lyrics</title>
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
  .v{font-size:18px;font-weight:600;margin-top:5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .acts{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
  .btn{font:inherit;font-size:13px;color:var(--ink);background:transparent;border:1px solid var(--line);border-radius:10px;padding:8px 12px;cursor:pointer;text-decoration:none}
  .btn:hover{border-color:var(--ink)}
  .btn.warn{color:var(--ember)}
  .btn.warn:hover{border-color:var(--ember)}
  .big{font-size:24px;font-weight:700;line-height:1.2;overflow-wrap:anywhere}
  .cfg{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
  input[type=text],input[type=number]{font:inherit;font-size:13px;width:100%;color:var(--ink);background:var(--paper);border:1px solid var(--line);border-radius:10px;padding:8px 10px}
  input[type=range]{width:100%;accent-color:var(--ember)}
  .ctl{display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:12px;margin-top:12px}
  .note{color:var(--mut);font-size:12px;line-height:1.5;margin-top:10px}
  .stale main{opacity:.6}
  @media(max-width:720px){main{padding:14px}.grid{grid-template-columns:1fr}.stats{grid-template-columns:1fr 1fr}.cfg{grid-template-columns:1fr}}
</style></head><body>
<main>
<header>
  <div><div class="eb">tend &middot; lyrics display</div><h1>clawdmeter <span class="mut mono" id="fw" style="font-size:13px;font-weight:400"></span></h1></div>
  <div class="status"><span class="dot" id="dot"></span><span class="mut" id="status">connecting</span></div>
</header>
<section class="card">
  <div class="label"><span class="eb">on the lcd</span><span class="mut" id="lyrS">--</span></div>
  <div class="big" id="npTi">&mdash;</div>
  <div class="mut" id="npAr"></div>
  <div class="track" style="margin-top:12px"><div class="fill" id="npF"></div></div>
  <div class="row"><span class="mono" id="npEl">0:00</span><span class="mono" id="npDu">0:00</span></div>
  <div class="note" id="npL"></div>
</section>
<section class="card">
  <div class="label"><span class="eb">network</span><span class="mut mono">clawdmeter.local</span></div>
  <div class="stats" style="grid-template-columns:1fr 1fr 1fr">
    <div class="stat"><div class="eb">wifi</div><div class="v" id="wS">--</div></div>
    <div class="stat"><div class="eb">ip</div><div class="v mono" id="wI">--</div></div>
    <div class="stat"><div class="eb">signal</div><div class="v mono" id="wR">--</div></div>
  </div>
  <div class="acts"><a class="btn warn" href="/wifi-reset" id="wBtn">change wifi</a><a class="btn warn" href="/factory-reset" id="fBtn">reset settings</a></div>
  <div class="note"><b>change wifi</b> forgets the saved network and reboots into the <b>Clawdmeter-setup</b> hotspot &mdash; join it from a phone and pick the new network. <b>reset settings</b> does that and also restores the default backlight. Either way the lcd tells you the new address once it is back.</div>
</section>
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
  <div class="label"><span class="eb">test push</span><span class="mut">fake a track on /nowplaying</span></div>
  <div class="cfg">
    <input id="npT" type="text" placeholder="title">
    <input id="npA" type="text" placeholder="artist">
    <input id="npP" type="number" min="0" placeholder="pos (s)">
    <input id="npD" type="number" min="0" placeholder="dur (s)">
  </div>
  <div class="acts"><label class="mut" style="display:flex;align-items:center;gap:6px"><input type="checkbox" id="npX"> paused</label><button class="btn" id="npBtn">push</button></div>
</section>
<section class="card">
  <div class="label"><span class="eb">device</span><span class="mut"><span class="mono" id="heap">--</span> heap &middot; up <span class="mono" id="up">--</span> &middot; boot <span id="rst">--</span></span></div>
  <div class="acts"><a class="btn" href="/usage.json">usage json</a><a class="btn" href="/update">ota update</a><a class="btn" href="/restart" id="rBtn">restart</a></div>
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
function commas(n){return n>0?String(n).replace(/\B(?=(\d{3})+(?!\d))/g,','):'--'}
function ageText(a){return a<0?'--':a<60?a+'s':Math.floor(a/60)+'m '+pad2(a%60)+'s'}
function upText(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h>0?h+'h'+pad2(m)+'m':m+'m'}
function meter(pId,fId,v){$(pId).textContent=v>=0?v+'%':'--';$(fId).style.width=(v>=0?Math.min(v,100):0)+'%'}
var LYR=['waiting for the lyrics daemon','reaching the lyrics daemon','streaming'];
$('fBtn').onclick=function(e){if(!confirm('forget wifi AND reset settings, then reboot into Clawdmeter-setup?'))e.preventDefault()};
$('wBtn').onclick=function(e){if(!confirm('forget the saved wifi and reboot into Clawdmeter-setup?'))e.preventDefault()};
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
var ticking=false;
async function tick(){
  if(ticking)return;
  ticking=true;
  var st=$('status'),dot=$('dot');
  try{
    var d=await (await fetch('/usage.json',{cache:'no-store'})).json();
    var now=d.now||0,live=d.age>=0&&d.age<=120;
    var hot=(d.stat&&d.stat!='allowed')||d.s>=90;
    document.body.className=live?'':'stale';
    dot.className='dot'+(d.lyr==2?' live':hot?' hot':'');
    st.textContent=LYR[d.lyr]||'--';
    $('lyrS').textContent=d.age<0?'no daemon push yet':'daemon seen '+ageText(d.age)+' ago';
    $('npTi').textContent=d.title||'—';
    $('npAr').textContent=d.artist||'';
    $('npEl').textContent=mmss(d.pos);
    $('npDu').textContent=d.dur>0?mmss(d.dur):'--:--';
    $('npF').style.width=(d.dur>0&&d.pos>=0?Math.min(100,d.pos*100/d.dur):0)+'%';
    $('npL').textContent=d.lyric||'';
    $('wS').textContent=d.wifi?(d.ssid||'connected'):'offline';
    $('wI').textContent=d.ip||'--';
    $('wR').textContent=d.wifi?d.rssi+' dBm':'--';
    meter('sp','sf',d.s);meter('wp','wf',d.w);
    $('sB').hidden=d.bind!=1;$('wB').hidden=d.bind!=2;
    $('st').textContent=d.sr?'reset '+hm(d.sr):'reset --';
    $('sc').textContent=d.sr&&now?cd(d.sr-now):'--';
    $('wt').textContent=d.wr?'reset '+DOW[lt(d.wr).getUTCDay()]+' '+hm(d.wr):'reset --';
    $('clock').textContent=now?clk(now):'--:--:--';
    $('age').textContent=ageText(d.age);
    $('stok').textContent=commas(d.st);
    $('wtok').textContent=commas(d.wt);
    $('heap').textContent=Math.round(d.heap/1024)+'k';
    $('up').textContent=upText(d.up);
    $('rst').textContent=d.rst||'--';
    $('fw').textContent=d.fw||'';
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
<h1>firmware update <span id="fw" style="color:var(--mut);font-size:13px;font-weight:400"></span></h1>
<p>upload only <code>clawdmeter_esp8266.ino.bin</code> · close dashboard tabs and stop the daemon while updating</p>
<form method="POST" action="/update" enctype="multipart/form-data">
  <input type="file" name="firmware" accept=".bin,.bin.gz" required>
  <button type="submit">update firmware</button>
</form>
<script>fetch("/usage.json",{cache:"no-store"}).then(function(r){return r.json()}).then(function(d){document.getElementById("fw").textContent=d.fw||""}).catch(function(_){})</script>
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
//
// This board draws no usage screen, but the endpoint is load-bearing anyway and
// must not be deleted as dead code: it is how the box learns (1) the Mac's IP,
// the only way it can find the lyrics daemon to dial, and (2) server time, which
// drives the clock on the waiting screen. The numbers themselves feed the
// browser dashboard's meters via /usage.json.
void handleUsage() {
  // The pusher is the Mac that also runs the lyrics daemon — remember its IP
  // so the frame stream can dial back (no config needed).
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
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
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

// One blocking send, no gzip and no size guard — unlike handleRoot(), which
// needs both. This body's worst case is ~1.3 KB (long title + two lyric lines +
// a crash-dump rinfo + a 32-char SSID), comfortably inside one ip=hb2f
// send-buffer fill (~5840 B), so it never hits the truncation failure mode in
// docs/postmortems/dashboard-truncated-send.md. Keep it that way: this is polled
// every 3 s, so a field that can grow unbounded belongs on its own endpoint.
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
             ",\"title\":\"" + jsonEscape(npTitle) + "\"" +
             ",\"artist\":\"" + jsonEscape(npArtist) + "\"" +
             ",\"lyric\":\"" + jsonEscape(npLyric) + "\"" +
             ",\"lyric2\":\"" + jsonEscape(npLyric2) + "\"" +
             ",\"pos\":" + String(musicDisplayPos()) +
             ",\"dur\":" + String(npDur) +
             ",\"paused\":" + String(npPaused) +
             ",\"lyr\":" + String(lyricsStreamActive() ? 2 : (lyricsStreamConnecting() ? 1 : 0)) +
             ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"" +
             ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
             ",\"rssi\":" + String(WiFi.isConnected() ? WiFi.RSSI() : 0) +
             ",\"wifi\":" + String(WiFi.isConnected() ? 1 : 0) +
             ",\"eap\":" + String(eapOn ? 1 : 0) +   // 1 = joined via 802.1X
             // Watchdog state. wifidown counts seconds since the association
             // dropped (0 while up), so "rst":"Software/System restarted" with a
             // low "up" plus a non-zero wifidrops reads as a watchdog reboot
             // rather than someone hitting /restart.
             ",\"wifidrops\":" + String(wifiDrops) +
             ",\"wifidown\":" + String(WiFi.isConnected() ? 0UL : (millis() - wifiOkMs) / 1000UL) +
             ",\"mdnsok\":" + String(wifiMdnsRefreshOk ? 1 : 0) +
             ",\"bl\":" + String(lcdBrightness) +
             ",\"heap\":" + String(ESP.getFreeHeap()) +
             ",\"now\":" + String(nowEpoch()) +
             ",\"rst\":\"" + bootReason + "\"" +
             ",\"rinfo\":\"" + jsonEscape(bootInfo) + "\"" +
             ",\"fw\":\"" FW_VERSION "\"" +
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

// Forget the saved WiFi and reboot straight into the setup hotspot — the way you
// move the box to a new router or a new password without a USB cable. Brightness
// and the rest of the EEPROM survive; /factory-reset below is the wider wipe.
//
// Deliberately reset-and-reboot rather than wm.startConfigPortal(): the portal
// blocks until someone finishes it, and calling it from inside a request handler
// would stall loop() and take the web server down with it for up to 3 minutes.
// Rebooting hands the portal to setup(), which is built to block.
void handleWifiReset() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain",
              "WiFi forgotten. Rebooting to the Clawdmeter-setup hotspot...");
  server.client().flush();   // drain the reply before we tear the link down
  delay(500);

  // resetSettings() erases the SDK's config sector. A backlight PWM timer ISR
  // firing during a flash erase resets the chip mid-write (see
  // backlightStopForFlash), which could leave the credentials half-wiped — park
  // the waveform first and never restore it, since we reboot from here anyway.
  backlightStopForFlash();
  WiFiManager wm;
  wm.resetSettings();
  eapForget();       // the 802.1X username/password are WiFi credentials too
  EEPROM.commit();   // bare, like /factory-reset: the PWM must stay parked
  ESP.restart();   // no delay: resetSettings() has already dropped the STA link
}

// The wider wipe: WiFi credentials *and* stored settings (brightness) back to
// defaults. Use /wifi-reset when all you want is a different network.
void handleFactoryReset() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain",
              "Factory reset OK. Rebooting to Clawdmeter-setup...");
  server.client().flush();
  delay(500);

  // Same flash guard as /wifi-reset, and it covers both writes below. Note the
  // bare EEPROM.commit(): tendEepromCommit() would re-arm the PWM waveform right
  // after, which is the one thing we must not do on the way to ESP.restart().
  backlightStopForFlash();
  WiFiManager wm;
  wm.resetSettings();
  eapForget();
  EEPROM.write(EE_MARKER_ADDR, 0x00);   // invalid marker -> defaults on reboot
  EEPROM.commit();
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

// The epoch is UTC; the caller adds TZ_OFFSET before formatting.
String hhmmss(unsigned long e) {
  unsigned long s = e % 86400UL;
  return pad2(s / 3600) + ":" + pad2((s % 3600) / 60) + ":" + pad2(s % 60);
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

// ---- The lyrics screen (the only screen) -----------------------------------
// Song content is rendered entirely by lyrics_display_daemon.py on the Mac: it
// streams 240x240 1-bpp frames (Core Text — real Thai shaping and syllable
// karaoke, plus full-screen album-art hero frames) over the WebSocket in
// lyrics_stream.cpp. There is no on-device now-playing renderer.
//
// Whenever no daemon frame owns the panel we paint the *waiting screen*: Tend
// chrome, a status line saying what the box is doing, and — the reason it earns
// its space — the device's own address, so you can reach the dashboard from a
// phone without a serial cable or a router-admin hunt.
// Run the daemon with --insecure so it speaks the proto=1 path this chip's client
// uses (see lyrics_stream.cpp for why the secure proto=2 heap won't fit here).

static bool musicStreamWas = false;        // last lyricsStreamActive() seen by musicTick
static bool musicConnWas = false;          // last lyricsStreamConnecting() (drives the eyebrow)
static uint8_t musicConnDots = 0;          // 0-3 dot pulse for the "connecting" status line
static unsigned long musicTickLastMs = 0;  // 1 Hz cadence for the waiting clock/dots
static String musicNetSig = "";            // last address block painted (repaint only on change)

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
static void musicDrawWaitingStatus() {
  gfx->fillRect(0, MUSIC_STATUS_Y - 2, 240, 16, C_TND_PAPER);
  String msg = lyricsStreamConnecting() ? String("reaching the lyrics daemon")
                                        : String("waiting for lyrics");
  if (lyricsStreamConnecting())
    for (uint8_t i = 0; i < (musicConnDots % 4); i++) msg += '.';
  printCentered(MUSIC_STATUS_Y, 1, msg, C_TND_MUTE, C_TND_PAPER);
}

// The address block: where to find this box. The IP goes at size 2 so it reads
// from across the desk while you type it into a phone; under it the mDNS name,
// which is the address worth memorising; at the foot, the network it joined.
//
// Repainted only when the text actually changes (musicNetSig), not on every 1 Hz
// tick — clearing and redrawing a 50 px band once a second is a visible blink.
// RSSI is deliberately absent for the same reason: it jitters every second and
// would defeat the signature. The dashboard shows it instead.
#define MUSIC_ADDR_Y 148
#define MUSIC_HOST_Y 176
#define MUSIC_FOOT_Y 224
static void musicDrawWaitingNet(bool force) {
  const bool up = WiFi.isConnected();
  const String addr = up ? WiFi.localIP().toString() : String("wifi lost");
  const String host = up ? String("clawdmeter.local") : String("reconnecting");
  const String foot = up ? "wifi: " + WiFi.SSID() : String("no network");

  const String sig = addr + "|" + host + "|" + foot;
  if (!force && sig == musicNetSig) return;
  musicNetSig = sig;

  gfx->fillRect(0, MUSIC_ADDR_Y - 4, 240, (MUSIC_HOST_Y + 12) - (MUSIC_ADDR_Y - 4), C_TND_PAPER);
  gfx->fillRect(0, MUSIC_FOOT_Y - 3, 240, 14, C_TND_PAPER);
  printCentered(MUSIC_ADDR_Y, 2, addr, up ? C_TND_INK : C_TND_EMBER, C_TND_PAPER);
  printCentered(MUSIC_HOST_Y, 1, host, C_TND_MUTE, C_TND_PAPER);
  printCentered(MUSIC_FOOT_Y, 1, foot, C_TND_FAINT, C_TND_PAPER);
}

// Full waiting-screen paint: Tend chrome (paper + hearth mark + eyebrow +
// hairline), the status line, the address block, and the header clock. Shown at
// boot and whenever the stream hands the panel back.
static void musicDrawWaiting() {
  musicConnWas = lyricsStreamConnecting();
  // "now playing" would be a lie here — this screen only shows when nothing is.
  tendHeader(musicConnWas ? "connecting" : "lyrics");
  musicDrawWaitingStatus();
  musicDrawWaitingNet(true);
  tendHeaderClock();
}

// Repaint the screen from scratch. Called at boot and after a failed OTA; the
// stream re-claims the panel on its own from musicTick().
static void musicScreenBegin() {
  musicStreamWas = false;
  musicTickLastMs = 0;
  musicDrawWaiting();
}

// Per-loop tick. Pump the socket; the stream blits its own frames straight to the
// panel while it owns it (musicTick paints nothing then). On the transition back
// to no-stream, repaint the waiting screen once. While waiting: repaint in full
// if the connecting-state (and thus the eyebrow) flipped, otherwise keep the
// clock fresh, pulse the "connecting..." dots at 1 Hz, and refresh the address
// block if the IP or network changed under us (DHCP renewal, reconnect).
static void musicTick() {
  lyricsStreamTick();
  bool streaming = lyricsStreamActive();
  if (streaming != musicStreamWas) {
    musicStreamWas = streaming;
    if (!streaming) musicDrawWaiting();   // stream dropped -> back to the waiting screen
  }
  if (streaming) return;                        // frames own the panel

  unsigned long now = millis();
  bool conn = lyricsStreamConnecting();
  if (conn != musicConnWas) {                   // eyebrow text changed -> full repaint
    musicDrawWaiting();
    musicTickLastMs = now;
    return;
  }
  if (now - musicTickLastMs >= 1000UL) {
    musicTickLastMs = now;
    if (conn) musicConnDots++;
    musicDrawWaitingStatus();                   // pulse dots / keep the line fresh
    musicDrawWaitingNet(false);                 // no-op unless the address changed
    tendHeaderClock();                          // cheap: clears only the clock rect
  }
}

void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  if (!otaUpdateOk || Update.hasError() || otaError.length()) {
    String msg = otaError.length() ? otaError : String("unknown update error");
    server.send(500, "text/plain", "Update failed: " + msg);
    otaInProgress = false;
    otaUpdateOk = false;
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);
    // The watchdog was parked above the OTA return for the whole upload, so its
    // last good timestamp is now as old as the transfer. Re-arm it here or a
    // long upload that ends with the link down would satisfy the restart stage
    // on the very first tick after OTA, rebooting instead of trying reconnect().
    wifiOkMs = millis();
    wifiNextRetryMs = 0;
    musicScreenBegin();
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
    wifiOkMs = millis();   // same re-arm as handleUpdateDone(); see the note there
    wifiNextRetryMs = 0;
    setBacklight(lcdBrightness);   // OTA failed, no reboot: restore the live PWM level
    drawOtaStatus("aborted");
  }

  yield();
}

// Watch the association and claw it back. Called from loop() and ONLY outside
// OTA -- see the call site for why that matters. Nothing here paints: the music
// screen's own tick reads WiFi.isConnected() and repaints the address block when
// the state changes, so the watchdog stays a pure network concern.
static void wifiWatchdogTick() {
  const unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    // Any connected tick resets the clock, which means a link that associates
    // for a moment every 25s keeps resetting it and never reaches the restart
    // stage -- reconnect() just fires forever. That is the intended trade: a
    // flapping link is still a link, and rebooting into a 3-minute blocking
    // portal would make it worse. wifidrops in /usage.json is what makes the
    // situation visible; a number climbing steadily is a flapping link.
    wifiOkMs = now;
    wifiNextRetryMs = 0;
    if (wifiWasDown) {
      wifiWasDown = false;
      // The responder was bound to an address that just went away and came
      // back. Without this, clawdmeter.local stays dark for the rest of the
      // session even though the box answers fine on its IP -- and that name is
      // exactly what the daemon's announce knock resolves, so a silent SDK
      // reconnect would otherwise still cost us the lyrics stream.
      wifiMdnsRefreshOk = MDNS.notifyAPChange();
      Serial.print("wifi back: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (!wifiWasDown) {
    wifiWasDown = true;
    wifiDrops++;
    Serial.println("wifi lost; watchdog armed");
    // wifiOkMs is deliberately left alone: it holds the last good tick, which is
    // the instant both stages below are measured from.
  }

  if (now - wifiOkMs >= WIFI_WATCHDOG_RESTART_MS) {
    // Out of cheap options. Reboot into setup(), which retries the saved network
    // first and only opens the portal if that fails -- so a network that came
    // back during the outage is simply rejoined.
    Serial.println("wifi down too long; restarting");
    ESP.restart();
    return;
  }

  if (now - wifiOkMs >= WIFI_WATCHDOG_RECONNECT_MS &&
      (wifiNextRetryMs == 0 || (long)(now - wifiNextRetryMs) >= 0)) {
    wifiNextRetryMs = now + WIFI_WATCHDOG_RETRY_MS;
    Serial.println("wifi watchdog: reconnect()");
    WiFi.reconnect();
  }
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
  tendHeader("starting");
  printCentered(112, 1, "joining wifi", C_TND_MUTE, C_TND_PAPER);

  // Tries the saved network; if it can't connect — first boot, new router, new
  // password — it opens the "Clawdmeter-setup" hotspot instead: join it from a
  // phone, pick a network, done. No re-flashing, and /wifi-reset gets you back
  // here on demand. autoConnect() blocks for the whole portal session, which is
  // exactly why the reset endpoints reboot into it rather than opening it from a
  // request handler (that would stall loop() and the web server with it).
  // An 802.1X network from an earlier portal session goes first, joined by us:
  // the SDK forgot the enterprise settings at reset, and WiFiManager's own
  // attempt would set a WPA-PSK threshold the AP need not meet (see eapConnect).
  bool joined = false;
  if (eapLoad()) {
    eapApply();
    joined = eapConnect();
    if (!joined) Serial.println("802.1X join failed; falling back to the portal");
  }

  if (!joined) {
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    wm.setAPCallback([](WiFiManager *) {    // the portal is up — say so on the panel
      tendHeader("wifi setup");
      printCentered(52, 1, "join this hotspot from a phone", C_TND_MUTE, C_TND_PAPER);
      printCentered(74, 2, WIFI_SETUP_AP, C_TND_EMBER, C_TND_PAPER);
      printCentered(116, 1, "a setup page opens by itself", C_TND_MUTE, C_TND_PAPER);
      printCentered(132, 1, "if not, browse to", C_TND_MUTE, C_TND_PAPER);
      printCentered(152, 2, WiFi.softAPIP().toString(), C_TND_INK, C_TND_PAPER);
      printCentered(184, 1, "work wifi asks for a username?", C_TND_MUTE, C_TND_PAPER);
      printCentered(198, 1, "fill in the username box too", C_TND_MUTE, C_TND_PAPER);
      printCentered(224, 1, "waiting 3 min, then retrying", C_TND_FAINT, C_TND_PAPER);
    });

    // The one extra portal field. Shown on the WiFi page under the password box
    // and read back in the save callback below, which WiFiManager runs before it
    // tries to connect -- so a filled-in username arms 802.1X in time.
    WiFiManagerParameter eapUser("eap_user",
        "Username &mdash; only for work wifi (802.1X / WPA2-Enterprise). "
        "Leave blank for home wifi.", "", 64);
    wm.addParameter(&eapUser);
    bool eapFromPortal = false;
    wm.setSaveParamsCallback([&]() {
      const String ssid = wm.server->arg("s");
      if (ssid.length() == 0) return;            // params-only save, no network picked
      String user = eapUser.getValue();
      user.trim();
      if (user.length() == 0) {                  // a normal home (WPA2-PSK) network
        if (eapOn) eapDisable();
        if (EEPROM.read(EE_EAP_FLAG_ADDR) == EEPROM_EAP_MARKER) {
          eapForget();
          tendEepromCommit();
        }
        return;
      }
      const String pass = wm.server->arg("p");
      strlcpy(eapCfg.ssid, ssid.c_str(), sizeof(eapCfg.ssid));
      strlcpy(eapCfg.user, user.c_str(), sizeof(eapCfg.user));
      strlcpy(eapCfg.pass, pass.c_str(), sizeof(eapCfg.pass));
      eepromWriteStr(EE_EAP_SSID_ADDR, ssid, sizeof(eapCfg.ssid));
      eepromWriteStr(EE_EAP_USER_ADDR, user, sizeof(eapCfg.user));
      eepromWriteStr(EE_EAP_PASS_ADDR, pass, sizeof(eapCfg.pass));
      EEPROM.write(EE_EAP_FLAG_ADDR, EEPROM_EAP_MARKER);
      tendEepromCommit();
      eapApply();
      // WiFiManager's begin(ssid, pass) may still get through with 802.1X armed;
      // if it doesn't, leave the portal instead of looping in it, and let
      // eapConnect() below have its go.
      wm.setBreakAfterConfig(true);
      eapFromPortal = true;
    });

    // Tries the saved network; if it can't connect — first boot, new router, new
    // password — it opens the "Clawdmeter-setup" hotspot instead: join it from a
    // phone, pick a network, done. No re-flashing, and /wifi-reset gets you back
    // here on demand. autoConnect() blocks for the whole portal session, which is
    // exactly why the reset endpoints reboot into it rather than opening it from a
    // request handler (that would stall loop() and the web server with it).
    joined = wm.autoConnect(WIFI_SETUP_AP);
    if (!joined && eapFromPortal) {
      tendHeader("starting");
      printCentered(112, 1, "joining work wifi (802.1X)", C_TND_MUTE, C_TND_PAPER);
      joined = eapConnect();
    }
  }
  if (!joined) {
    // Nobody finished the portal in time, or the 802.1X login was refused.
    // Reboot rather than sit here: the saved network may simply have come back
    // (router reboot), and if it hasn't, the next pass reopens the portal.
    Serial.println("WiFi setup timed out, restarting...");
    ESP.restart();
  }
  Serial.print("Connected: http://");
  Serial.println(WiFi.localIP());

  // Let the SDK try to re-associate on its own before the watchdog's coarser
  // 30s reconnect() ever gets a turn. Never called anywhere before this, so the
  // setting was whatever WiFiManager happened to leave behind.
  WiFi.setAutoReconnect(true);
  // Arm the watchdog from here, not from the zero-initialised global: leaving it
  // at 0 would make the first tick measure the drop from boot and, on a box that
  // took a while in the portal, trip the restart stage immediately.
  wifiOkMs = millis();
  wifiNextRetryMs = 0;
  wifiWasDown = false;

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

  musicScreenBegin();   // waiting screen: status + this box's address, until frames flow

  server.on("/", handleRoot);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/usage", HTTP_GET, handleUsage);   // GET allowed too, handy for testing
  server.on("/usage.json", handleUsageJson);
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/nowplaying", HTTP_GET, handleNowPlaying);
  server.on("/nowplaying", HTTP_POST, handleNowPlaying);
  server.on("/restart", HTTP_GET, handleRestart);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/wifi-reset", HTTP_GET, handleWifiReset);
  server.on("/wifi-reset", HTTP_POST, handleWifiReset);
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

  // Strictly below the OTA return. A WiFi hiccup mid-upload must not be allowed
  // to fire reconnect() -- let alone ESP.restart() -- while Update.write() has
  // the flash open: that is the same reset-mid-flash hazard backlightStopForFlash()
  // exists to prevent, on the one operation that cannot be retried.
  wifiWatchdogTick();

  // The one screen, millis()-polled from here (never a timer ISR, so nothing
  // fights the WiFi/TCP stack and no IRAM is spent).
  musicTick();

  // Yield to the SDK so WIFI_MODEM_SLEEP can actually engage between beacons.
  // 2 ms is invisible to a 1 s clock tick and a page polled once per second.
  delay(2);
}
