"use strict";

// Backup path for the one thing the board cannot work without: knowing this
// Mac's address.
//
// The board is never configured with the daemon's address. It learns it from
// the *source IP* of an inbound HTTP request to /usage and then dials that IP
// back on 8766. Normally the daemon's own BoardAnnouncer does that knock. This
// is the second, independent path, and it is worth having because the two fail
// for different reasons: the daemon knocks `clawdmeter.local`, so a quiet mDNS
// responder takes it out, while this one knocks the numeric IP saved in the
// popup and does not care about mDNS at all.
//
// Why this lives in the service worker and NOT in content_script.js: a content
// script on https://music.youtube.com cannot fetch http://192.168.1.35. It is
// blocked twice over -- as mixed content, and by Private Network Access
// restrictions on secure-public -> private requests -- and it fails silently,
// which is the worst possible outcome for something whose whole job is
// reliability. Extension-origin requests carrying host permissions are not
// subject to either.

const KNOCK_ALARM = "g4pys-board-knock";
const KNOCK_PERIOD_MINUTES = 1;   // the board forgets a Mac after 10 minutes
const KNOCK_TIMEOUT_MS = 3000;
// The content script rewrites its heartbeat every 30s while its daemon socket
// is open. Two missed writes and we treat the daemon as gone.
const HEARTBEAT_FRESH_MS = 90 * 1000;

// Mirrors popup.js's parser. Deliberately duplicated rather than shared: making
// this a module to import one function would change the worker's registration
// for no real gain.
function firmwareBaseUrl(rawValue) {
  const raw = (rawValue || "").trim();
  if (!raw) {
    throw new Error("no firmware address saved");
  }
  const withProtocol = /^[a-z][a-z0-9+.-]*:\/\//i.test(raw) ? raw : `http://${raw}`;
  const url = new URL(withProtocol);
  if (url.protocol !== "http:") {
    throw new Error("firmware URL must use http");
  }
  url.pathname = "/";
  url.search = "";
  url.hash = "";
  return url;
}

async function recordKnock(state, detail) {
  // The popup reads this; it is the only way to see whether this path is doing
  // anything, since a service worker leaves no visible trace of its own.
  await chrome.storage.local.set({
    lastKnock: { state, detail: detail || "", at: Date.now() }
  });
}

async function knockOnce() {
  const [store, beats] = await Promise.all([
    chrome.storage.local.get(["firmwareIp"]),
    // Liveness, not configuration: session storage is memory-backed and cleared
    // on browser restart, which is exactly the lifetime this timestamp wants.
    // Keeping it in local would mean a disk write every 30s per open tab.
    chrome.storage.session.get(["daemonHeartbeat"])
  ]);

  let baseUrl;
  try {
    baseUrl = firmwareBaseUrl(store.firmwareIp);
  } catch (err) {
    await recordKnock("skipped", err.message);
    return;
  }

  // Guard 1: only knock from the machine that is actually running the daemon.
  // The board would happily record whatever address knocked on it, so an
  // unguarded knock from a second computer -- a laptop with this extension
  // installed but no daemon -- would point the board at a machine with nothing
  // listening on 8766 and take the panel down until the real daemon knocked
  // again. A fresh heartbeat means the content script has an OPEN socket to a
  // daemon on this host's 127.0.0.1, which is exactly the condition we need.
  const beat = Number(beats.daemonHeartbeat) || 0;
  const beatAge = Date.now() - beat;
  if (!beat || beatAge > HEARTBEAT_FRESH_MS) {
    await recordKnock("skipped",
      beat ? `daemon socket last seen ${Math.round(beatAge / 1000)}s ago`
           : "no YouTube Music tab with a daemon connection");
    return;
  }

  // Guard 2: http://*/* is an OPTIONAL permission -- it is granted only when
  // someone clicks Connect in the popup. On a fresh profile it is absent, and
  // fetching without it throws in a way that is easy to mistake for the board
  // being unreachable.
  const origins = [`${baseUrl.protocol}//${baseUrl.host}/*`];
  if (!(await chrome.permissions.contains({ origins }))) {
    await recordKnock("blocked", "no permission for the board's address -- click Connect");
    return;
  }

  // /usage, never /usage.json: only /usage calls lyricsStreamNoteHost(). That
  // asymmetry is deliberate in the firmware -- /usage.json is polled by the
  // board's own dashboard from any browser, so teaching it the caller's address
  // would let a phone opening that page redirect the stream at the phone.
  const url = new URL("/usage", baseUrl);
  url.searchParams.set("t", String(Math.floor(Date.now() / 1000)));

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), KNOCK_TIMEOUT_MS);
  try {
    const response = await fetch(url.href, { cache: "no-store", signal: controller.signal });
    // The board reads our source IP off the connection, so arriving is the
    // whole job; the status only tells us whether it was healthy enough to
    // answer properly.
    if (response.ok) {
      await recordKnock("ok", "");
    } else if (response.status === 404) {
      // This file is shared verbatim with the ESP32 e-ink tree, whose firmware
      // serves /usage.json but no /usage -- it finds its daemon by browsing
      // _lyrics._tcp over mDNS and never needed a knock. A 404 therefore means
      // "wrong kind of board", not "broken", and should not read as a failure.
      await recordKnock("skipped", "this board has no /usage - it finds the daemon over mDNS");
    } else {
      await recordKnock("failed", `board answered HTTP ${response.status}`);
    }
  } catch (err) {
    await recordKnock("failed",
      err && err.name === "AbortError" ? "board did not answer in 3s" : String(err && err.message || err));
  } finally {
    clearTimeout(timer);
  }
}

function createAlarm() {
  chrome.alarms.create(KNOCK_ALARM, { periodInMinutes: KNOCK_PERIOD_MINUTES });
}

// Only create it if it is genuinely absent. create() on an existing name
// REPLACES it and restarts its period from zero, and this runs on every worker
// wake -- including the wake that onAlarm itself caused, and the one the popup
// causes by sending a message. Calling it unconditionally would mean opening
// the popup every ~50s stopped the alarm from ever firing on its own, and would
// keep resetting the cadence rather than letting it run steady.
function ensureAlarm() {
  chrome.alarms.get(KNOCK_ALARM, (existing) => {
    if (!existing) {
      createAlarm();
    }
  });
}

// The content script writes the heartbeat from a page context, which counts as
// untrusted; session storage is TRUSTED_CONTEXTS-only by default and would
// reject it. Worst case at browser start is one skipped knock before the worker
// first wakes and applies this, after which it self-heals.
try {
  chrome.storage.session.setAccessLevel({ accessLevel: "TRUSTED_AND_UNTRUSTED_CONTEXTS" });
} catch (_err) {
  // Older Chrome without setAccessLevel; the heartbeat read below just stays empty.
}

// onInstalled/onStartup are the right unconditional creation points.
chrome.runtime.onInstalled.addListener(createAlarm);
chrome.runtime.onStartup.addListener(createAlarm);
// Any other revival re-arms only if the alarm is actually gone (extension
// reload drops alarms; worker teardown does not).
ensureAlarm();

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === KNOCK_ALARM) {
    knockOnce();
  }
});

// Lets the popup force a knock without waiting out the alarm period.
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message && message.type === "g4pys-knock-now") {
    knockOnce().then(() => sendResponse({ done: true }));
    return true;   // keep the channel open for the async reply
  }
  return false;
});

// Daemon socket bridge. content_script.js used to open ws://127.0.0.1:8765
// itself, but a content-script socket carries the page origin, so Local
// Network Access rules (and enterprise policies enforcing them) can block it
// outright -- the console shows only "WebSocket connection ... failed". The
// worker opens it instead, as the extension origin with host permissions, and
// relays: one port per YouTube Music tab, one socket per port. Either side
// closing tears down the other, and the content script reconnects as before.
const BRIDGE_PORT = "g4pys-daemon-bridge";
const DAEMON_WS_URL = "ws://127.0.0.1:8765/extension";

chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== BRIDGE_PORT) {
    return;
  }
  let ws = null;
  let done = false;
  const finish = () => {
    if (done) {
      return;
    }
    done = true;
    try {
      if (ws) {
        ws.close();
      }
    } catch (_err) {
      // Socket already gone.
    }
    try {
      port.postMessage({ type: "close" });
      port.disconnect();
    } catch (_err) {
      // Tab already gone.
    }
  };

  port.onDisconnect.addListener(() => {
    done = true;
    try {
      if (ws) {
        ws.close();
      }
    } catch (_err) {
      // Socket already gone.
    }
  });

  try {
    ws = new WebSocket(DAEMON_WS_URL);
  } catch (_err) {
    finish();
    return;
  }
  ws.addEventListener("open", () => {
    try {
      port.postMessage({ type: "open" });
    } catch (_err) {
      finish();
    }
  });
  ws.addEventListener("close", finish);
  ws.addEventListener("error", finish);

  port.onMessage.addListener((message) => {
    // "keepalive" needs no handling: receiving it is what keeps the worker up.
    if (message && message.type === "send" && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(message.data);
    }
  });
});
