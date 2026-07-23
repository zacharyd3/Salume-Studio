/* Salume Studio service worker — offline app shell.
 *
 * Strategy:
 *   - Precache the app shell (HTML, icon, manifest) on install.
 *   - Never touch the data sync endpoint (/data/) or the MQTT proxy (/mqtt):
 *     those must always hit the network so curing data and live readings stay
 *     current. Only same-origin GETs are handled at all.
 *   - Navigations & the shell use network-first (so a rebuilt app is picked up
 *     immediately when online) with a cache fallback for offline.
 *   - Other same-origin GETs use cache-first, falling back to the network.
 *
 * Bump CACHE when the shell changes to evict the old copy.
 */
const CACHE = "salume-shell-v1";
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
  if (url.origin !== self.location.origin) return;                 // let cross-origin pass through
  if (url.pathname.startsWith("/data/") || url.pathname.startsWith("/mqtt")) return; // never cache

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
