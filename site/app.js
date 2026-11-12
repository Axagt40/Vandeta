/* Shared site behaviour: mobile nav, copy buttons, current page, year. */
(function () {
  "use strict";

  // Mobile navigation
  var toggle = document.querySelector(".nav-toggle");
  var nav = document.getElementById("site-nav");
  if (toggle && nav) {
    toggle.addEventListener("click", function () {
      var open = nav.classList.toggle("open");
      toggle.setAttribute("aria-expanded", String(open));
    });
    nav.addEventListener("click", function (event) {
      if (event.target.closest("a")) {
        nav.classList.remove("open");
        toggle.setAttribute("aria-expanded", "false");
      }
    });
  }

  // Mark the current page in the nav
  var here = location.pathname.split("/").pop() || "index.html";
  document.querySelectorAll(".site-nav a[href]").forEach(function (link) {
    var href = link.getAttribute("href").split("#")[0];
    if (href === here) link.setAttribute("aria-current", "page");
  });

  // Copy buttons: <button class="btn btn-small" data-copy="text">Copy</button>
  document.querySelectorAll("[data-copy]").forEach(function (button) {
    button.addEventListener("click", function () {
      var text = button.getAttribute("data-copy");
      var done = function () {
        var label = button.textContent;
        button.textContent = "Copied";
        window.setTimeout(function () {
          button.textContent = label;
        }, 1400);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, function () {});
      } else {
        var area = document.createElement("textarea");
        area.value = text;
        area.setAttribute("readonly", "");
        area.style.position = "fixed";
        area.style.opacity = "0";
        document.body.appendChild(area);
        area.select();
        try {
          document.execCommand("copy");
          done();
        } catch (error) {
          /* nothing to do */
        }
        document.body.removeChild(area);
      }
    });
  });

  // Homepage release note: follow the newest pre-release, which is the build
  // that ships. Values come from the release assets; nothing is hardcoded.
  var note = document.querySelector("[data-release-note]");
  if (note) {
    fetch("https://api.github.com/repos/Axagt40/Vandeta/releases?per_page=30", {
      headers: { Accept: "application/vnd.github+json" },
    })
      .then(function (response) {
        if (!response.ok) throw new Error("release list " + response.status);
        return response.json();
      })
      .then(function (list) {
        var pre = (list || []).filter(function (r) {
          return r.prerelease && !r.draft;
        })[0];
        if (!pre) throw new Error("no pre-release");
        var asset = (pre.assets || []).filter(function (a) {
          return a.name === "vandeta.bin";
        })[0];
        if (!asset) throw new Error("no image asset");
        var version = String(pre.tag_name || "vandeta-1.4-pre")
          .replace(/^vandeta-/, "")
          .replace(/-/, " ");
        var line = "vandeta " + version + " \u00b7 " + asset.size.toLocaleString("en-US") + " bytes";
        var digest = (asset.digest || "").replace(/^sha256:/, "");
        if (digest.length === 64) line += " \u00b7 sha256 " + digest.slice(0, 8) + "\u2026" + digest.slice(-6);
        note.textContent = line + " \u00b7 AGPL-3.0";
      })
      .catch(function () {
        note.textContent = "release list unavailable";
      });
  }

  // Footer year
  document.querySelectorAll("[data-year]").forEach(function (node) {
    node.textContent = String(new Date().getFullYear());
  });
})();
