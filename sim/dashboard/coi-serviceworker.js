/*! coi-serviceworker v0.1.7 - Guido Zuidhof, licensed under MIT */
/*
 * Adds Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy
 * headers via a service worker, enabling SharedArrayBuffer on hosts
 * that don't support custom response headers (e.g. GitHub Pages).
 */
if (typeof window === "undefined") {
  self.addEventListener("install", function () {
    self.skipWaiting();
  });
  self.addEventListener("activate", function (e) {
    e.waitUntil(self.clients.claim());
  });
  self.addEventListener("fetch", function (e) {
    if (
      e.request.cache === "only-if-cached" &&
      e.request.mode !== "same-origin"
    ) {
      return;
    }
    e.respondWith(
      fetch(e.request)
        .then(function (r) {
          if (r.status === 0) return r;
          var h = new Headers(r.headers);
          h.set("Cross-Origin-Embedder-Policy", "require-corp");
          h.set("Cross-Origin-Opener-Policy", "same-origin");
          return new Response(r.body, {
            status: r.status,
            statusText: r.statusText,
            headers: h,
          });
        })
        .catch(function (e) {
          console.error(e);
        })
    );
  });
} else {
  (function () {
    var coi = {
      shouldRegister: function () {
        return !window.crossOriginIsolated;
      },
      quiet: false,
    };

    if (!coi.shouldRegister()) return;

    var n = navigator;
    if (!n.serviceWorker) {
      if (!coi.quiet)
        console.error(
          "COOP/COEP: Service workers are not supported. SharedArrayBuffer will not work."
        );
      return;
    }

    n.serviceWorker
      .register(window.document.currentScript.src)
      .then(function (r) {
        if (r.active && !n.serviceWorker.controller) {
          window.location.reload();
        } else if (!r.active) {
          r.addEventListener("updatefound", function () {
            var w = r.installing;
            w.addEventListener("statechange", function () {
              if (w.state === "activated") {
                window.location.reload();
              }
            });
          });
        }
      });
  })();
}
