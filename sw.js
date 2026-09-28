/* Salume Studio service worker — offline app shell.
 *
 * Strategy:
 *   - Precache the app shell (HTML, icon, manifest) on install.
 *   - Never touch the data sync endpoint (/data/), the recorder API (/api/), or
 *     the MQTT proxy (/mqtt): those must always hit the network so curing data,
 *     recorded history, and live readings stay current. Only same-origin GETs
 *     are handled at all.
 *   - Navigations & the shell use network-first (so a rebuilt app is picked up
 *     immediately when online) with a cache fallback for offline.
 *   - Other same-origin GETs use cache-first, falling back to the network.
 *   - Google Fonts (the app's typefaces) are cached stale-while-revalidate so
 *     the app keeps its look offline; without them it falls back to system fonts.
 *
 * Bump CACHE when the shell changes to evict the old copy.
 */
const CACHE = "salume-shell-v2";
const FONT_HOSTS = ["fonts.googleapis.com", "fonts.gstatic.com"];
const SHELL = ["./", "charcuterie.html", "icon.png", "manifest.webmanifest"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (e) => {
  const req = e.request;
  if (req.method !== "GET") return;

  const url = new URL(req.url);
  if (FONT_HOSTS.includes(url.hostname)) {                         // fonts: serve cached, refresh in background
    e.respondWith(
      caches.open(CACHE).then((c) => c.match(req).then((m) => {
        const net = fetch(req).then((res) => { if (res.ok || res.type === "opaque") c.put(req, res.clone()); return res; })
          .catch(() => m);
        return m || net;
      }))
    );
    return;
  }
  if (url.origin !== self.location.origin) return;                 // let other cross-origin requests pass through
  if (url.pathname.startsWith("/data/") || url.pathname.startsWith("/api/") ||
      url.pathname.startsWith("/mqtt")) return; // never cache: live data & API

  const isShell = req.mode === "navigate" ||
    url.pathname === "/" || url.pathname.endsWith("charcuterie.html");

  if (isShell) {
    // Network-first: prefer a fresh shell, fall back to cache when offline.
    e.respondWith(
      fetch(req)
        .then((res) => {
          const copy = res.clone();
          caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {});
          return res;
        })
        .catch(() => caches.match(req).then((m) => m || caches.match("charcuterie.html")))
    );
    return;
  }

  // Everything else same-origin: cache-first with a network fallback that fills the cache.
  e.respondWith(
    caches.match(req).then((m) => m || fetch(req).then((res) => {
      const copy = res.clone();
      caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {});
      return res;
    }).catch(() => m))
  );
});
