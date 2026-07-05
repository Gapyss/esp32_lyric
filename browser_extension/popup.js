(() => {
  "use strict";

  const darkButton = document.getElementById("dark");
  const lightButton = document.getElementById("light");
  const firmwareIpInput = document.getElementById("firmwareIp");
  const firmwareMacInput = document.getElementById("firmwareMac");
  const saveFirmwareButton = document.getElementById("saveFirmware");
  const testFirmwareButton = document.getElementById("testFirmware");
  const status = document.getElementById("status");

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

  chrome.storage.local.get(["theme", "firmwareIp", "firmwareMac"], (result) => {
    render(result.theme || "dark");
    firmwareIpInput.value = result.firmwareIp || "";
    firmwareMacInput.value = result.firmwareMac || "";
  });
})();
