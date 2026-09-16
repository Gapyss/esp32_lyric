(() => {
  "use strict";

  const darkButton = document.getElementById("dark");
  const lightButton = document.getElementById("light");
  const firmwareIpInput = document.getElementById("firmwareIp");
  const firmwareMacInput = document.getElementById("firmwareMac");
  const saveFirmwareButton = document.getElementById("saveFirmware");
  const testFirmwareButton = document.getElementById("testFirmware");
  const status = document.getElementById("status");
  const health = document.getElementById("health");
  const refreshButton = document.getElementById("refresh");

  function render(theme) {
    darkButton.classList.toggle("active", theme === "dark");
    lightButton.classList.toggle("active", theme === "light");
  }

  function setStatus(message) {
    status.textContent = message;
  }

  function normalizeMac(value) {
    return (value || "").trim().replace(/-/g, ":").toUpperCase();
  }

  function macIsValid(value) {
    return value === "" || /^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(value);
  }

  function firmwareBaseUrl(rawValue) {
    const raw = (rawValue || "").trim();
    if (!raw) {
      throw new Error("Enter the firmware IP or host.");
    }
    const withProtocol = /^[a-z][a-z0-9+.-]*:\/\//i.test(raw) ? raw : `http://${raw}`;
    const url = new URL(withProtocol);
    if (url.protocol !== "http:") {
      throw new Error("Firmware URL must use http.");
    }
    url.pathname = "/";
    url.search = "";
    url.hash = "";
    return url;
  }

  function firmwareOrigin(url) {
    return `${url.protocol}//${url.host}/*`;
  }

  async function ensureFirmwarePermission(url) {
    const origins = [firmwareOrigin(url)];
    return chrome.permissions.request({ origins });
  }

  async function saveFirmwareSettings() {
    const firmwareIp = firmwareIpInput.value.trim();
    const firmwareMac = normalizeMac(firmwareMacInput.value);
    firmwareMacInput.value = firmwareMac;
    if (firmwareIp) {
      firmwareBaseUrl(firmwareIp);
    }
    if (!macIsValid(firmwareMac)) {
      throw new Error("MAC must look like AA:BB:CC:DD:EE:FF.");
    }
    await chrome.storage.local.set({ firmwareIp, firmwareMac });
  }

  async function testFirmware() {
    testFirmwareButton.disabled = true;
    setStatus("Checking firmware...");
    try {
      const baseUrl = firmwareBaseUrl(firmwareIpInput.value);
      const firmwareMac = normalizeMac(firmwareMacInput.value);
      firmwareMacInput.value = firmwareMac;
      if (!macIsValid(firmwareMac)) {
        throw new Error("MAC must look like AA:BB:CC:DD:EE:FF.");
      }
      const allowed = await ensureFirmwarePermission(baseUrl);
      if (!allowed) {
        setStatus("Permission denied for firmware address.");
        return;
      }
      await chrome.storage.local.set({
        firmwareIp: firmwareIpInput.value.trim(),
        firmwareMac
      });
      const controller = new AbortController();
      const timer = window.setTimeout(() => controller.abort(), 3500);
      const usageUrl = new URL("/usage.json", baseUrl);
      let response;
      try {
        response = await fetch(usageUrl.href, {
          cache: "no-store",
          signal: controller.signal
        });
      } finally {
        window.clearTimeout(timer);
      }
      if (!response.ok) {
        throw new Error(`Firmware returned HTTP ${response.status}.`);
      }
      const body = await response.json();
      setStatus(`Firmware connected: ${body.mode || "ok"}.`);
    } catch (err) {
      const message = err && err.name === "AbortError" ? "Firmware check timed out." : err.message;
      setStatus(message || "Firmware check failed.");
    } finally {
      testFirmwareButton.disabled = false;
    }
  }

  // ---- Status panel --------------------------------------------------------
  // The board's own waiting screen cannot tell you which link in the chain is
  // broken: "waiting for lyrics" is shown both when nothing has ever pushed and
  // when a connected daemon is simply between tracks. This answers it from the
  // one place that can see every link at once.

  function healthRow(state, label, text) {
    const row = document.createElement("div");
    row.className = "hrow";
    const dot = document.createElement("span");
    dot.className = `dot ${state}`;
    const body = document.createElement("span");
    const strong = document.createElement("b");
    strong.textContent = `${label} `;
    body.appendChild(strong);
    body.appendChild(document.createTextNode(text));
    row.appendChild(dot);
    row.appendChild(body);
    health.appendChild(row);
  }

  function healthDetail(text) {
    const div = document.createElement("div");
    div.className = "hdetail";
    div.textContent = text;
    health.appendChild(div);
  }

  function agoText(ms) {
    const seconds = Math.round(ms / 1000);
    if (seconds < 90) return `${seconds}s ago`;
    return `${Math.round(seconds / 60)}m ago`;
  }

  // knockLanded says a recent knock reached the board, which means it certainly
  // knows this Mac's address. That is what disambiguates lyr=0: on its own it
  // conflates "nobody has ever told me where the Mac is" (broken) with
  // "connected, but the daemon has nothing to show between tracks" (fine). The
  // board reports both as 0 because lyricsStreamActive() is
  // (LYR_OPEN && lyrFrameValid) -- an open socket with no current frame looks
  // exactly like a board that has never heard from anyone.
  function describeBoard(d, knockLanded) {
    const up = Number(d.up) || 0;
    let state;
    if (d.lyr === 0) {
      state = knockLanded ? "connected, idle (nothing playing)"
                          : "not connected - no Mac address known";
    } else {
      state = { 1: "connecting to the daemon", 2: "streaming lyrics" }[d.lyr] || "unknown state";
    }
    const parts = [
      state,
      `wifi ${d.ssid || "?"} (${d.rssi} dBm)`,
      `heap ${Math.round((Number(d.heap) || 0) / 1024)}k`,
      `up ${Math.floor(up / 3600)}h${String(Math.floor((up % 3600) / 60)).padStart(2, "0")}m`,
      `boot ${d.rst || "?"}`
    ];
    // Watchdog fields; absent on firmware older than the WiFi watchdog.
    const down = Number(d.wifidown) || 0;
    const drops = Number(d.wifidrops) || 0;
    if (down) parts.push(`WIFI DOWN ${down}s`);
    if (drops) parts.push(`${drops} wifi drop${drops === 1 ? "" : "s"}`);
    if (d.mdnsok === 0) parts.push("mDNS refresh failed");
    return parts;
  }

  async function renderHealth() {
    health.textContent = "";
    const [store, beats] = await Promise.all([
      chrome.storage.local.get(["firmwareIp", "lastKnock"]),
      chrome.storage.session.get(["daemonHeartbeat"])
    ]);

    // 1. The daemon. Inferred from the content script's heartbeat rather than by
    // opening a socket of our own: a second connection on :8765 would show up as
    // another extension client to the daemon, and this panel should observe the
    // chain, not join it.
    const beat = Number(beats.daemonHeartbeat) || 0;
    const beatAge = Date.now() - beat;
    if (!beat) {
      healthRow("warn", "Daemon", "unknown - no YouTube Music tab has connected yet");
      healthDetail("Open music.youtube.com; this panel reads the tab's socket to the daemon.");
    } else if (beatAge < 90000) {
      healthRow("ok", "Daemon", `connected (seen ${agoText(beatAge)})`);
    } else {
      healthRow("bad", "Daemon", `not connected - last seen ${agoText(beatAge)}`);
      healthDetail("Start it with esp8266/tools/lyrics.sh");
    }

    // 2. This extension's backup knock.
    const knock = store.lastKnock;
    // A knock that landed within the board's own 10-minute host-expiry window
    // is proof it knows where we are.
    const knockLanded = Boolean(knock && knock.state === "ok"
                                && Date.now() - knock.at < 10 * 60 * 1000);
    if (!knock) {
      healthRow("warn", "Knock", "not run yet");
    } else {
      const when = agoText(Date.now() - knock.at);
      const state = knock.state === "ok" ? "ok" : (knock.state === "failed" ? "bad" : "warn");
      healthRow(state, "Knock", knock.state === "ok" ? `delivered ${when}` : `${knock.state} ${when}`);
      if (knock.detail) healthDetail(knock.detail);
    }

    // 3. The board.
    if (!store.firmwareIp) {
      healthRow("warn", "Board", "no address saved - fill in the field below");
      return;
    }
    let baseUrl;
    try {
      baseUrl = firmwareBaseUrl(store.firmwareIp);
    } catch (err) {
      healthRow("bad", "Board", err.message);
      return;
    }
    const origins = [firmwareOrigin(baseUrl)];
    if (!(await chrome.permissions.contains({ origins }))) {
      healthRow("warn", "Board", "no permission for this address");
      healthDetail("Click Connect below to grant it. Until then the backup knock cannot run.");
      return;
    }
    const controller = new AbortController();
    const timer = window.setTimeout(() => controller.abort(), 3500);
    try {
      const response = await fetch(new URL("/usage.json", baseUrl).href,
                                   { cache: "no-store", signal: controller.signal });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const body = await response.json();
      const parts = describeBoard(body, knockLanded);
      const healthy = body.lyr === 2 || (body.lyr === 0 && knockLanded);
      healthRow(healthy ? "ok" : (body.lyr === 1 ? "warn" : "bad"), "Board", parts[0]);
      healthDetail(parts.slice(1).join(" - "));
    } catch (err) {
      healthRow("bad", "Board",
        err && err.name === "AbortError" ? "did not answer in 3.5s" : "unreachable");
      healthDetail("Powered off, on another network, or wedged.");
    } finally {
      window.clearTimeout(timer);
    }
  }

  async function setTheme(theme) {
    await chrome.storage.local.set({ theme });
    render(theme);
    const tabs = await chrome.tabs.query({ url: "https://music.youtube.com/*" });
    if (tabs.length === 0) {
      setStatus("Open music.youtube.com to apply.");
      return;
    }
    setStatus("");
    for (const tab of tabs) {
      if (tab.id !== undefined) {
        chrome.tabs.sendMessage(tab.id, { type: "g4pys-set-theme", theme }).catch(() => {});
      }
    }
  }

  darkButton.addEventListener("click", () => setTheme("dark"));
  lightButton.addEventListener("click", () => setTheme("light"));
  saveFirmwareButton.addEventListener("click", async () => {
    try {
      await saveFirmwareSettings();
      setStatus("Firmware settings saved.");
    } catch (err) {
      setStatus(err.message || "Firmware settings are invalid.");
    }
  });
  testFirmwareButton.addEventListener("click", () => {
    testFirmware();
  });

  refreshButton.addEventListener("click", async () => {
    refreshButton.disabled = true;
    setStatus("Knocking...");
    try {
      // Force a knock rather than waiting out the alarm, so Refresh is also the
      // "make it work now" button.
      await chrome.runtime.sendMessage({ type: "g4pys-knock-now" });
    } catch (_err) {
      // Worker asleep or reloading; the render below still shows the last state.
    }
    await renderHealth();
    setStatus("");
    refreshButton.disabled = false;
  });

  chrome.storage.local.get(["theme", "firmwareIp", "firmwareMac"], (result) => {
    render(result.theme || "light");
    firmwareIpInput.value = result.firmwareIp || "";
    firmwareMacInput.value = result.firmwareMac || "";
  });

  renderHealth().catch((err) => {
    // A rejection here would otherwise leave the panel simply blank, which is
    // the one thing this panel exists to stop happening.
    health.textContent = "";
    healthRow("bad", "Status", "could not be read");
    healthDetail(String((err && err.message) || err));
  });
})();
