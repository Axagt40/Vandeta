/* Full feature list for the flasher page.
 *
 * The list is not copied into this page: it is read from the repository README
 * when the page opens, so a release can never ship next to a stale summary.
 * The README is public, so this needs no token and no build step.
 *
 * If GitHub cannot be reached the panel keeps a link to the README instead of
 * showing an empty box.
 */
(function () {
  "use strict";

  var REPO = "Axagt40/Vandeta";
  var RAW = "https://raw.githubusercontent.com/" + REPO;
  var TARGET = document.getElementById("feature-list");
  if (!TARGET) return;

  var escapeHtml = function (text) {
    return text
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  };

  // Inline markdown the README actually uses in this section: **bold**, `code`
  // and [text](url). Everything is escaped first, so the replacements cannot
  // inject markup.
  var inline = function (text) {
    var out = escapeHtml(text);
    out = out.replace(/`([^`]+)`/g, "<code>$1</code>");
    out = out.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    out = out.replace(
      /\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g,
      '<a href="$2" rel="noopener">$1</a>',
    );
    return out;
  };

  // The README groups the list in <details><summary> blocks with markdown
  // bullets inside. Convert that to a group per summary, keeping the same
  // order and wording.
  var render = function (markdown) {
    var lines = markdown.split(/\r?\n/);
    var html = "";
    var listOpen = false;
    var groupOpen = false;

    var closeList = function () {
      if (listOpen) {
        html += "</ul>";
        listOpen = false;
      }
    };
    var closeGroup = function () {
      closeList();
      if (groupOpen) {
        html += "</details>";
        groupOpen = false;
      }
    };

    lines.forEach(function (line) {
      var text = line.trim();
      var summary = text.match(/^<summary>(.*)<\/summary>$/i);
      if (summary) {
        closeGroup();
        html +=
          '<details class="feat-group" open><summary>' +
          inline(summary[1].replace(/<[^>]+>/g, "")) +
          "</summary>";
        groupOpen = true;
        return;
      }
      // Markdown furniture that is not content: the section's own heading, a
      // horizontal rule, and the <details> wrappers themselves.
      if (/^<\/?details[^>]*>$/i.test(text)) return;
      if (/^-{3,}$/.test(text) || /^#{1,6}\s/.test(text)) return;
      if (text.length === 0) {
        closeList();
        return;
      }
      var bullet = text.match(/^[*-]\s+(.*)$/);
      if (bullet) {
        if (!listOpen) {
          html += "<ul>";
          listOpen = true;
        }
        html += "<li>" + inline(bullet[1]) + "</li>";
        return;
      }
      // A wrapped bullet line belongs to the previous <li>; anything else is a
      // short paragraph between groups.
      if (listOpen && /^\s{2,}/.test(line)) {
        html = html.replace(/<\/li>$/, " " + inline(text) + "</li>");
        return;
      }
      closeList();
      html += "<p>" + inline(text) + "</p>";
    });
    closeGroup();
    return html;
  };

  var load = function (ref) {
    return fetch(RAW + "/" + ref + "/README.md", { cache: "no-store" }).then(function (response) {
      if (!response.ok) throw new Error("README " + response.status);
      return response.text();
    });
  };

  TARGET.innerHTML = '<p class="file-meta">Loading the feature list from GitHub&hellip;</p>';

  load("main")
    .catch(function () {
      return load("master");
    })
    .then(function (readme) {
      // The list starts at its own heading and ends at the next top-level one.
      var start = readme.indexOf("Vandeta feature list");
      if (start < 0) throw new Error("feature section not in README");
      start = readme.lastIndexOf("\n## ", start);
      if (start < 0) throw new Error("feature heading not found");
      var body = readme.slice(start + 1);
      var next = body.indexOf("\n## ", 4);
      if (next > 0) body = body.slice(0, next);

      var html = render(body);
      if (!html) throw new Error("empty feature section");
      TARGET.innerHTML = html;
    })
    .catch(function (error) {
      TARGET.innerHTML =
        '<p class="file-meta">The live list could not be loaded (' +
        escapeHtml(String(error.message || error)) +
        '). Read it at <a href="https://github.com/' +
        REPO +
        '#readme" rel="noopener">github.com/' +
        REPO +
        "</a>.</p>";
    });
})();
