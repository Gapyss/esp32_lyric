(() => {
  "use strict";

  const WS_URL = "ws://127.0.0.1:8765/extension";
  const TICK_MS = 500;
  const TRACK_SCAN_MS = 1000;
  const RECONNECT_MS = 1500;

  let ws = null;
  let reconnectTimer = 0;
  let tickTimer = 0;
  let trackTimer = 0;
  let domObserver = null;
  let lastTrackKey = "";
  let lastPaused = null;
  let lastPosition = null;

  function getVideo() {
    return document.querySelector("video");
  }

  function finiteNumber(value) {
    const number = Number.parseFloat(value);
    return Number.isFinite(number) ? number : NaN;
  }

  function mediaMetadata() {
    const metadata = navigator.mediaSession && navigator.mediaSession.metadata;
    if (!metadata) {
      return {};
    }
    return {
      title: metadata.title || "",
      artist: metadata.artist || "",
      album: metadata.album || "",
      artUrl: bestArtwork(metadata.artwork)
    };
  }

  function bestArtwork(artwork) {
    if (!Array.isArray(artwork) || artwork.length === 0) {
      return "";
    }
    // Pick the largest declared size; the daemon downscales to the panel anyway.
    let best = "";
    let bestArea = -1;
    for (const entry of artwork) {
      const src = entry && entry.src;
      if (!src) {
        continue;
      }
      const dims = String((entry && entry.sizes) || "").split("x");
      const area = (Number.parseInt(dims[0], 10) || 0) * (Number.parseInt(dims[1], 10) || 0);
      if (area >= bestArea) {
        bestArea = area;
        best = src;
      }
    }
    return best;
  }

  function textFromSelectors(selectors) {
    for (const selector of selectors) {
      const el = document.querySelector(selector);
      const text = el && (el.textContent || el.getAttribute("title") || "").trim();
      if (text) {
        return text.replace(/\s+/g, " ");
      }
    }
    return "";
  }

  function cleanTitle(title) {
    return (title || "")
      .replace(/\s+[-|]\s+YouTube Music\s*$/, "")
      .replace(/\s+/g, " ")
      .trim();
  }

  function videoIdFromLocation() {
    try {
      const url = new URL(window.location.href);
      const fromUrl = url.searchParams.get("v") || "";
      if (fromUrl) {
        return fromUrl;
      }
    } catch (_err) {
      // Keep scanning DOM fallbacks.
    }
    const link = document.querySelector("ytmusic-player-bar a[href*='watch?v='], a[href*='music.youtube.com/watch?v=']");
    if (!link) {
      return "";
    }
    try {
      return new URL(link.href, window.location.origin).searchParams.get("v") || "";
    } catch (_err) {
      return "";
    }
  }

  function playerState() {
    const video = getVideo();
    const progressBar = document.querySelector("#progress-bar");
    const progressPosition = progressBar ? finiteNumber(progressBar.getAttribute("aria-valuenow")) : NaN;
    const progressDuration = progressBar ? finiteNumber(progressBar.getAttribute("aria-valuemax")) : NaN;
    const videoPosition = video && Number.isFinite(video.currentTime) ? video.currentTime : 0;
    const videoDuration = video && Number.isFinite(video.duration) ? video.duration : 0;
    return {
      positionSec: Number.isFinite(progressPosition) ? progressPosition : videoPosition,
      durationSec: Number.isFinite(progressDuration) && progressDuration > 0 ? progressDuration : videoDuration,
      paused: video ? Boolean(video.paused) : true,
      playbackRate: video && Number.isFinite(video.playbackRate) ? video.playbackRate : 1
    };
  }

  function currentTrack() {
    const md = mediaMetadata();
    const state = playerState();
    const title =
      md.title ||
      textFromSelectors([
        "ytmusic-player-bar .title",
        "ytmusic-player-bar yt-formatted-string.title",
        "ytmusic-player-bar [class*='title']",
        "ytmusic-player-page .title",
        "yt-formatted-string.title"
      ]);
    const artist =
      md.artist ||
      textFromSelectors([
        "ytmusic-player-bar .byline a",
        "ytmusic-player-bar .subtitle a",
        "ytmusic-player-bar yt-formatted-string.byline",
        "ytmusic-player-bar [class*='byline']",
        "ytmusic-player-bar [class*='subtitle']"
      ]);
    return {
      videoId: videoIdFromLocation(),
      title: cleanTitle(title || document.title),
      artist,
      album: md.album || textFromSelectors(["ytmusic-player-bar .album", "ytmusic-player-bar [class*='album']"]),
      artUrl: md.artUrl || "",
      durationSec: state.durationSec
    };
  }

  function send(type, payload) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    ws.send(JSON.stringify({ type, payload, sentAtMs: Date.now() }));
  }

  function sendTrackIfChanged(force = false) {
    const track = currentTrack();
    const key = [track.videoId, track.title, track.artist, Math.round(track.durationSec || 0)].join("\u0000");
    if (!force && key === lastTrackKey) {
      return;
    }
    lastTrackKey = key;
    send("now-playing", track);
  }

  function sendTick(force = false) {
    const state = playerState();
    const roundedPosition = Math.round(state.positionSec * 10) / 10;
    if (!force && lastPaused === state.paused && lastPosition === roundedPosition) {
      return;
    }
    lastPaused = state.paused;
    lastPosition = roundedPosition;
    send("tick", state);
  }

  function sendEvent(type) {
    send("event", { type, ...playerState() });
  }

  function sendTheme(theme) {
    send("set-theme", { theme });
  }

  function bindVideoEvents() {
    const video = getVideo();
    if (!video || video.dataset.g4pysLyricsBound === "1") {
      return;
    }
    video.dataset.g4pysLyricsBound = "1";
    video.addEventListener("play", () => sendEvent("play"), true);
    video.addEventListener("pause", () => sendEvent("pause"), true);
    video.addEventListener("seeked", () => sendEvent("seek"), true);
    video.addEventListener("ended", () => sendEvent("ended"), true);
    video.addEventListener("ratechange", () => sendTick(true), true);
  }

  // After the extension is reloaded/updated, the copy of this script already
  // injected into open tabs is orphaned: its chrome.* context is gone, so any
  // chrome API access throws "Extension context invalidated". Detect that and
  // tear ourselves down instead of throwing on every timer tick.
  function extensionAlive() {
    try {
      return Boolean(chrome.runtime && chrome.runtime.id);
    } catch (_err) {
      return false;
    }
  }

  function teardown() {
    window.clearInterval(tickTimer);
    window.clearInterval(trackTimer);
    window.clearTimeout(reconnectTimer);
    if (domObserver) {
      domObserver.disconnect();
      domObserver = null;
    }
    try {
      if (ws) {
        ws.close();
      }
    } catch (_err) {
      // ws already gone; nothing to do.
    }
    ws = null;
  }

  function startLoops() {
    window.clearInterval(tickTimer);
    window.clearInterval(trackTimer);
    trackTimer = window.setInterval(() => {
      if (!extensionAlive()) {
        teardown();
        return;
      }
      bindVideoEvents();
      sendTrackIfChanged(false);
    }, TRACK_SCAN_MS);
    tickTimer = window.setInterval(() => {
      if (!extensionAlive()) {
        teardown();
        return;
      }
      sendTick(false);
    }, TICK_MS);
    bindVideoEvents();
    sendTrackIfChanged(true);
    sendTick(true);
    if (!domObserver) {
      domObserver = new MutationObserver(() => sendTrackIfChanged(false));
      domObserver.observe(document.documentElement, { childList: true, subtree: true, characterData: true });
    }
  }

  function connect() {
    window.clearTimeout(reconnectTimer);
    try {
      ws = new WebSocket(WS_URL);
    } catch (_err) {
      reconnectTimer = window.setTimeout(connect, RECONNECT_MS);
      return;
    }

    ws.addEventListener("open", () => {
      if (!extensionAlive()) {
        teardown();
        return;
      }
      startLoops();
      try {
        chrome.storage.local.get(["theme"], (result) => {
          if (result && result.theme) {
            sendTheme(result.theme);
          }
        });
      } catch (_err) {
        // Context invalidated between the alive-check and the call; ignore.
      }
    });
    ws.addEventListener("close", () => {
      window.clearInterval(tickTimer);
      window.clearInterval(trackTimer);
      if (domObserver) {
        domObserver.disconnect();
        domObserver = null;
      }
      if (!extensionAlive()) {
        teardown();
        return;
      }
      reconnectTimer = window.setTimeout(connect, RECONNECT_MS);
    });
    ws.addEventListener("error", () => {
      try {
        ws.close();
      } catch (_err) {
        reconnectTimer = window.setTimeout(connect, RECONNECT_MS);
      }
    });
  }

  chrome.runtime.onMessage.addListener((message) => {
    if (message && message.type === "g4pys-set-theme") {
      sendTheme(message.theme);
    }
  });

  connect();
})();
