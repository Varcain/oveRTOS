/*! coi-serviceworker v0.1.7 - Guido Zuidhof and contributors, licensed under MIT */
/*
 * Adds Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy
 * headers via a service worker, enabling SharedArrayBuffer on hosts
 * that don't support custom response headers (e.g. GitHub Pages).
 *
 * Include as the FIRST <script> in your HTML.
 * On first load, the SW registers and the page reloads automatically.
 * On second load, the SW intercepts all fetches and adds headers.
 */
if (typeof window === "undefined") {
  /* ── Service Worker context ─────────────────────────────────── */
  self.addEventListener("install", function () {
    self.skipWaiting();
  });

  self.addEventListener("activate", function (event) {
    event.waitUntil(self.clients.claim());
  });

  self.addEventListener("fetch", function (event) {
    if (
      event.request.cache === "only-if-cached" &&
      event.request.mode !== "same-origin"
    ) {
      return;
    }

    event.respondWith(
      fetch(event.request)
        .then(function (response) {
          if (response.status === 0) {
            return response;
          }

          var newHeaders = new Headers(response.headers);
          newHeaders.set("Cross-Origin-Embedder-Policy", "require-corp");
          newHeaders.set("Cross-Origin-Opener-Policy", "same-origin");

          return new Response(response.body, {
            status: response.status,
            statusText: response.statusText,
            headers: newHeaders,
          });
        })
        .catch(function (e) {
          console.error(e);
        })
    );
  });
} else {
  /* ── Window context ─────────────────────────────────────────── */
  (function () {
    /* Already cross-origin isolated — nothing to do. */
    if (window.crossOriginIsolated) {
      return;
    }

    var n = navigator;

    if (!n.serviceWorker) {
      console.error(
        "COOP/COEP: Service workers not supported. SharedArrayBuffer will not work."
      );
      return;
    }

    n.serviceWorker
      .register(window.document.currentScript.src)
      .then(function (registration) {
        /* SW is active but not controlling this page yet — reload. */
        if (registration.active && !n.serviceWorker.controller) {
          console.log("COOP/COEP: Service worker active, reloading for control...");
          window.location.reload();
          return;
        }

        /* SW is installing — wait for it to activate, then reload. */
        if (!registration.active) {
          registration.addEventListener("updatefound", function () {
            var worker = registration.installing;
            if (!worker) return;
            worker.addEventListener("statechange", function () {
              if (worker.state === "activated") {
                console.log("COOP/COEP: Service worker activated, reloading...");
                window.location.reload();
              }
            });
          });
        }
      })
      .catch(function (e) {
        console.error("COOP/COEP: Registration failed:", e);
      });
  })();
}
