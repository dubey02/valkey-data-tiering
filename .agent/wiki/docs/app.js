/* app.js -- the book (reading) view of the data tiering wiki.
 *
 * Nothing here is generated. The whole view is three static files (index.html,
 * app.js, style.css) that read the wiki's own markdown at runtime:
 *
 *   * the part/chapter hierarchy comes from the curated tables in ../index.md,
 *     which is already the registry humans maintain -- no duplicate manifest;
 *   * chapter titles come from each page's front-matter `title:`;
 *   * section ToCs come from the H2/H3 headings in each page;
 *   * prose is rendered with the marked.js vendored under keg/viewer/vendor/.
 *
 * Routing is hash-based because GitHub Pages cannot rewrite paths and we refuse
 * to commit one generated .html per page:
 *
 *   #/                          top-level table of contents
 *   #/part/components           a part's ToC
 *   #/state-machine             a chapter
 *   #/state-machine/blocking-matrix   a section within a chapter
 */
(function () {
  "use strict";

  var MD_BASE = "../"; // docs/ -> .agent/wiki/

  /* ------------------------------------------------------------------ config
   * The only hand-maintained structure: part titles/blurbs, which index.md
   * heading feeds each part, and the pages that index.md does not list.
   */
  var PARTS = [
    {
      slug: "orientation",
      title: "Orientation",
      blurb: "What data tiering is, and how the pieces fit together.",
      from: "Top level",
    },
    {
      slug: "components",
      title: "Components",
      blurb: "The engine-side subsystems: state machine, spill/fetch plumbing, " +
             "throttling, eviction, accounting, backends.",
      from: "Components (L2)",
    },
    {
      slug: "interfaces",
      title: "Interfaces",
      blurb: "Header-level reference for every boundary a backend or operator sees.",
      from: "Interfaces (L3)",
    },
    {
      slug: "flows",
      title: "Flows",
      blurb: "End-to-end sequences through the system, one page per path.",
      from: "Flows (L3/L4)",
    },
    {
      slug: "decisions",
      title: "Decisions & Limitations",
      blurb: "Design decisions with citations, and the known gaps.",
      from: "Decisions (L4)",
    },
    {
      slug: "reference",
      title: "Reference",
      blurb: "Wiki conventions, graph schema, retrieval cheatsheet, and the change log.",
      pages: [
        { rel: "WIKI.md", summary: "Scope, page format, front matter, edge kinds, tiers, and lint rules" },
        { rel: "AGENTS.md", summary: "Operating contract for the agent that maintains this wiki" },
        { rel: "keg/README.md", summary: "KEG tooling: lint, graph extraction, query, citation verification" },
        { rel: "keg/RETRIEVAL.md", summary: "Cheatsheet for graph-aware retrieval queries" },
        { rel: "log.md", summary: "Chronological change log for every wiki edit" },
      ],
    },
  ];

  var SLUGS = {
    "00-overview.md": "overview",
    "01-architecture.md": "architecture",
    "WIKI.md": "wiki-conventions",
    "AGENTS.md": "agent-contract",
    "keg/README.md": "keg-tooling",
    "keg/RETRIEVAL.md": "keg-retrieval",
    "log.md": "change-log",
  };

  var ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"];

  /* ------------------------------------------------------------------ utils */

  function slugify(text) {
    return (
      text
        .replace(/`([^`]*)`/g, "$1")
        .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1")
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-+|-+$/g, "") || "section"
    );
  }

  function stripInline(text) {
    return text
      .replace(/`([^`]*)`/g, "$1")
      .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1")
      .replace(/\*\*([^*]+)\*\*/g, "$1")
      .replace(/\*([^*]+)\*/g, "$1")
      .replace(/(^|\W)__([^_]+)__(?=\W|$)/g, "$1$2")
      .replace(/(^|\W)_([^_]+)_(?=\W|$)/g, "$1$2")
      .trim();
  }

  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
      .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  function splitFrontMatter(text) {
    if (text.indexOf("---") !== 0) return { fm: {}, body: text };
    var end = text.indexOf("\n---", 3);
    if (end === -1) return { fm: {}, body: text };
    var raw = text.slice(4, end);
    var nl = text.indexOf("\n", end + 1);
    var fm = {};
    raw.split("\n").forEach(function (line) {
      var m = /^([A-Za-z_][A-Za-z0-9_]*):\s*(.+)$/.exec(line);
      if (m && m[2].trim()) fm[m[1]] = m[2].trim().replace(/^['"]|['"]$/g, "");
    });
    return { fm: fm, body: nl === -1 ? "" : text.slice(nl + 1) };
  }

  // H2/H3 outline of a markdown body, skipping fenced code and the H1.
  function outline(body) {
    var flat = [], fence = false;
    body.split("\n").forEach(function (line) {
      if (/^\s*(```|~~~)/.test(line)) { fence = !fence; return; }
      if (fence) return;
      var m = /^(#{2,3})\s+(.*?)\s*#*\s*$/.exec(line);
      if (!m) return;
      var title = stripInline(m[2]);
      flat.push({ level: m[1].length, title: title, anchor: slugify(title), children: [] });
    });
    var nested = [];
    flat.forEach(function (s) {
      if (s.level === 2 || !nested.length) { s.level = 2; nested.push(s); }
      else nested[nested.length - 1].children.push(s);
    });
    return nested;
  }

  function resolvePath(baseRel, href) {
    if (/^([a-z]+:|#|\/\/)/i.test(href)) return null;
    var parts = baseRel.split("/").slice(0, -1).concat(href.split("/"));
    var out = [];
    parts.forEach(function (p) {
      if (!p || p === ".") return;
      if (p === "..") out.pop(); else out.push(p);
    });
    return out.join("/");
  }

  /* ------------------------------------------------------------------ model */

  var mdCache = {};
  function fetchMd(rel) {
    if (mdCache[rel]) return mdCache[rel];
    mdCache[rel] = fetch(MD_BASE + rel).then(function (r) {
      if (!r.ok) throw new Error(rel + ": HTTP " + r.status);
      return r.text();
    });
    return mdCache[rel];
  }

  var model = null; // { parts: [...], chapters: [...], bySlug: {}, byRel: {} }

  // Parse the curated tables in index.md into part buckets.
  function parseIndex(text) {
    var buckets = {}, current = null;
    text.split("\n").forEach(function (line) {
      var h = /^##\s+(.*?)\s*$/.exec(line);
      if (h) { current = stripInline(h[1]); buckets[current] = buckets[current] || []; return; }
      if (!current) return;
      var row = /^\|\s*\[([^\]]*)\]\(([^)]+\.md)\)\s*\|([^|]*)\|([^|]*)\|/.exec(line);
      if (!row) return;
      buckets[current].push({
        rel: resolvePath("index.md", row[2]),
        label: stripInline(row[1]),
        summary: stripInline(row[3]),
        status: stripInline(row[4]),
      });
    });
    return buckets;
  }

  function buildModel(indexText) {
    var buckets = parseIndex(indexText);
    var chapters = [], n = 0;
    var parts = PARTS.map(function (spec, pi) {
      var part = {
        slug: spec.slug, title: spec.title, blurb: spec.blurb,
        roman: ROMAN[pi] || String(pi + 1), chapters: [],
      };
      var rows = spec.pages || buckets[spec.from] || [];
      rows.forEach(function (row) {
        var rel = row.rel;
        var ch = {
          rel: rel,
          slug: SLUGS[rel] || rel.replace(/^.*\//, "").replace(/\.md$/, ""),
          title: row.label || rel,
          summary: row.summary || "",
          status: row.status || "",
          number: ++n,
          part: part,
          sections: null, // filled once the markdown is read
        };
        part.chapters.push(ch);
        chapters.push(ch);
      });
      return part;
    });
    var bySlug = {}, byRel = {};
    chapters.forEach(function (c) { bySlug[c.slug] = c; byRel[c.rel] = c; });
    return { parts: parts, chapters: chapters, bySlug: bySlug, byRel: byRel };
  }

  // Read a chapter's markdown once: title from front matter, outline from headings.
  function hydrate(ch) {
    if (ch._hydrated) return ch._hydrated;
    ch._hydrated = fetchMd(ch.rel).then(function (text) {
      var parsed = splitFrontMatter(text);
      var h1 = /^\s*#\s+(.+)$/m.exec(parsed.body);
      ch.title = parsed.fm.title || (h1 ? stripInline(h1[1]) : ch.title);
      ch.status = parsed.fm.status || ch.status;
      ch.tier = parsed.fm.tier || "";
      ch.body = parsed.body.replace(/^\s*#\s+[^\n]*\n+/, "");
      ch.sections = outline(ch.body);
      ch.sections.forEach(function (s, i) {
        s.number = ch.number + "." + (i + 1);
        s.children.forEach(function (c, j) { c.number = s.number + "." + (j + 1); });
      });
      return ch;
    });
    return ch._hydrated;
  }

  /* ------------------------------------------------------------------- views */

  var main = null, crumbs = null, pagerTop = null;

  function href(ch) { return "#/" + ch.slug; }
  function partHref(p) { return "#/part/" + p.slug; }

  function sectionList(ch, base) {
    if (!ch.sections || !ch.sections.length) return "";
    var out = ['<ol class="toc sections">'];
    ch.sections.forEach(function (s) {
      out.push('<li><a href="' + base + "/" + s.anchor + '"><span class="num">' +
               s.number + '.</span> ' + esc(s.title) + "</a>");
      if (s.children.length) {
        out.push('<ol class="toc sections">');
        s.children.forEach(function (c) {
          out.push('<li><a href="' + base + "/" + c.anchor + '"><span class="num">' +
                   c.number + '.</span> ' + esc(c.title) + "</a></li>");
        });
        out.push("</ol>");
      }
      out.push("</li>");
    });
    out.push("</ol>");
    return out.join("");
  }

  function chapterEntry(ch) {
    return '<div class="ch-line"><span class="num">' + ch.number + '.</span>' +
      '<a href="' + href(ch) + '">' + esc(ch.title) + "</a>" +
      (ch.status ? '<span class="badge ' + esc(ch.status) + '">' + esc(ch.status) + "</span>" : "") +
      "</div>" +
      (ch.summary ? '<p class="summary">' + esc(ch.summary) + "</p>" : "") +
      '<div class="sec-slot" data-slug="' + ch.slug + '">' + sectionList(ch, href(ch)) + "</div>";
  }

  function renderIndex() {
    setChrome([], pager(null, null, model.chapters[0]));
    document.title = "Data Tiering Wiki";
    var html = ['<article class="page index-page">',
      "<h1>Data Tiering Wiki</h1>",
      '<p class="lede">Reading view of the LLM-maintained wiki for Valkey <strong>data ' +
      "tiering</strong>: in v1, keys always stay in the dict and only values spill to flash " +
      "&mdash; key spilling may be enabled by config later. Every chapter below is the " +
      "wiki's own markdown, read at request time; the same content is browsable as a " +
      'typed-edge graph in the <a href="../keg/viewer/index.html">graph view</a>.</p>',
      '<p class="startline">New here? Start at <a href="#/overview">1. Overview</a> &#8594; ' +
      '<a href="#/architecture">2. Architecture</a>, then drill into the parts below.</p>',
      "<h2>Table of Contents</h2>", '<ol class="toc parts">'];
    model.parts.forEach(function (p) {
      html.push("<li>", '<div class="part-line"><span class="num">' + p.roman + '.</span>' +
        '<a class="part" href="' + partHref(p) + '">' + esc(p.title) + "</a></div>",
        '<p class="blurb">' + esc(p.blurb) + "</p>", '<ol class="toc chapters">');
      p.chapters.forEach(function (c) { html.push("<li>" + chapterEntry(c) + "</li>"); });
      html.push("</ol></li>");
    });
    html.push("</ol></article>");
    main.innerHTML = html.join("");
    fillSections(model.chapters);
  }

  // Section ToCs need each page's headings; fill them in as the fetches land.
  function fillSections(chapters) {
    chapters.forEach(function (ch) {
      hydrate(ch).then(function () {
        var slot = main.querySelector('.sec-slot[data-slug="' + ch.slug + '"]');
        if (slot) slot.innerHTML = sectionList(ch, href(ch));
        var line = slot && slot.parentNode.querySelector(".ch-line a");
        if (line) line.textContent = ch.title;
      }).catch(function () { /* a missing page must not break the index */ });
    });
  }

  function renderPart(part) {
    var i = model.parts.indexOf(part);
    var prev = i > 0 ? model.parts[i - 1] : null;
    setChrome([{ label: part.title }], pager(prev, "index", part.chapters[0]));
    document.title = "Part " + part.roman + ". " + part.title;
    var html = ['<article class="page part-page">',
      '<p class="kicker">Part ' + part.roman + "</p>", "<h1>" + esc(part.title) + "</h1>",
      '<p class="lede">' + esc(part.blurb) + "</p>",
      "<h2>Table of Contents</h2>", '<ol class="toc chapters">'];
    part.chapters.forEach(function (c) { html.push("<li>" + chapterEntry(c) + "</li>"); });
    html.push("</ol></article>");
    main.innerHTML = html.join("");
    fillSections(part.chapters);
  }

  function renderChapter(ch, anchor) {
    var i = model.chapters.indexOf(ch);
    hydrate(ch).then(function () {
      setChrome([{ label: ch.part.title, href: partHref(ch.part) }, { label: ch.title }],
                pager(model.chapters[i - 1] || ch.part, ch.part, model.chapters[i + 1]));
      document.title = ch.number + ". " + ch.title;

      var meta = [];
      if (ch.status) meta.push('<span class="badge ' + esc(ch.status) + '">' + esc(ch.status) + "</span>");
      if (ch.tier) meta.push('<span class="badge tier">' + esc(ch.tier) + "</span>");
      meta.push('<a class="src" href="' + MD_BASE + esc(ch.rel) + '">source: ' + esc(ch.rel) + "</a>");

      var toc = "";
      if (ch.sections.length) {
        var list = sectionList(ch, href(ch));
        toc = ch.sections.length > 20
          ? '<nav class="page-toc long"><details><summary>Contents <span class="count">(' +
            ch.sections.length + " sections)</span></summary>" + list + "</details></nav>"
          : '<nav class="page-toc"><h2>Contents</h2>' + list + "</nav>";
      }

      main.innerHTML = '<article class="page chapter">' +
        '<p class="kicker">Part ' + ch.part.roman + ". " + esc(ch.part.title) +
        " &middot; Chapter " + ch.number + "</p>" +
        "<h1>" + ch.number + ". " + esc(ch.title) + "</h1>" +
        '<p class="meta">' + meta.join("") + "</p>" + toc +
        '<div id="md-body" class="md"></div></article>' +
        '<footer class="page-footer"><nav class="pager">' +
        pager(model.chapters[i - 1] || ch.part, ch.part, model.chapters[i + 1]) +
        "</nav></footer>";

      var box = document.getElementById("md-body");
      box.innerHTML = window.marked.parse(ch.body);
      rewriteLinks(box, ch.rel);
      numberHeadings(box, ch.number);
      scrollSpy();
      if (anchor) {
        var el = document.getElementById(anchor);
        if (el) el.scrollIntoView();
      } else window.scrollTo(0, 0);
    }).catch(function (err) { fail(String(err)); });
  }

  /* ------------------------------------------------------------------ chrome */

  function pager(prev, up, next) {
    function link(t, label, cls) {
      if (!t) return '<span class="' + cls + ' disabled">' + label + "</span>";
      var h, tip;
      if (t === "index") { h = "#/"; tip = "Home"; }
      else if (t.chapters) { h = partHref(t); tip = t.title; }
      else { h = href(t); tip = t.title; }
      return '<a class="' + cls + '" href="' + h + '" title="' + esc(tip) + '">' + label + "</a>";
    }
    return link(prev, "&#8592; Prev", "prev") + link(up, "&#8593; Up", "up") +
           link(next, "Next &#8594;", "next");
  }

  function setChrome(trail, pagerHtml) {
    var bits = ['<a href="#/">Home</a>'];
    trail.forEach(function (t, i) {
      bits.push(t.href && i < trail.length - 1
        ? '<a href="' + t.href + '">' + esc(t.label) + "</a>"
        : '<span class="here">' + esc(t.label) + "</span>");
    });
    crumbs.innerHTML = bits.join('<span class="sep">/</span>');
    pagerTop.innerHTML = pagerHtml;
  }

  function rewriteLinks(root, mdRel) {
    root.querySelectorAll("a[href]").forEach(function (a) {
      var raw = a.getAttribute("href");
      if (/^#/.test(raw)) return;
      var frag = "", at = raw.indexOf("#");
      if (at > -1) { frag = raw.slice(at + 1); raw = raw.slice(0, at); }
      var rel = resolvePath(mdRel, raw);
      if (rel === null) { a.target = "_blank"; a.rel = "noopener"; return; }
      var ch = model.byRel[rel];
      if (ch) {
        a.setAttribute("href", href(ch) + (frag ? "/" + frag : ""));
        a.classList.add("xref");
      } else if (rel === "index.md") {
        a.setAttribute("href", "#/");
      } else {
        a.setAttribute("href", MD_BASE + rel + (frag ? "#" + frag : ""));
        a.classList.add("asset");
      }
    });
    root.querySelectorAll("img[src]").forEach(function (img) {
      var rel = resolvePath(mdRel, img.getAttribute("src"));
      if (rel !== null) img.setAttribute("src", MD_BASE + rel);
    });
  }

  function numberHeadings(root, chapter) {
    var h2 = 0, h3 = 0;
    root.querySelectorAll("h2, h3").forEach(function (h) {
      var id = slugify(h.textContent.trim());
      // marked assigns its own heading ids, which for simple titles equal ours.
      // Only treat it as a collision when a *different* element holds the id --
      // otherwise every such heading gets pointlessly suffixed and the ToC
      // anchors stop resolving.
      var held = document.getElementById(id);
      if (held && held !== h) {
        var n = 2;
        while (document.getElementById(id + "-" + n)) n++;
        id += "-" + n;
      }
      h.id = id;
      var num;
      if (h.tagName === "H2") { h2 += 1; h3 = 0; num = chapter + "." + h2; }
      else { h3 += 1; num = chapter + "." + h2 + "." + h3; }
      var label = document.createElement("span");
      label.className = "hnum";
      label.textContent = num + ". ";
      h.insertBefore(label, h.firstChild);
      var handle = document.createElement("a");
      handle.className = "hanchor";
      handle.href = window.location.hash.replace(/\/[^/]*$/, "") === window.location.hash
        ? window.location.hash + "/" + id
        : window.location.hash.replace(/\/[^/]*$/, "/" + id);
      handle.textContent = "#";
      handle.setAttribute("aria-label", "link to this section");
      h.appendChild(handle);
    });
  }

  function scrollSpy() {
    var links = [].slice.call(document.querySelectorAll(".page-toc a"));
    if (!links.length) return;
    var targets = links.map(function (a) {
      var parts = a.getAttribute("href").split("/");
      return { link: a, el: document.getElementById(parts[parts.length - 1]) };
    }).filter(function (t) { return t.el; });
    if (!targets.length) return;
    var queued = false;
    function update() {
      queued = false;
      var y = window.scrollY + 90, active = targets[0];
      targets.forEach(function (t) { if (t.el.offsetTop <= y) active = t; });
      links.forEach(function (a) { a.classList.toggle("active", a === active.link); });
    }
    window.addEventListener("scroll", function () {
      if (!queued) { queued = true; window.requestAnimationFrame(update); }
    });
    update();
  }

  function fail(msg) {
    main.innerHTML = '<article class="page"><div class="md"><p class="error"><strong>' +
      esc(msg) + "</strong><br>This view reads the wiki's markdown over HTTP, so it must be " +
      "served (<code>python3 -m http.server</code>) rather than opened as a " +
      "<code>file://</code> path.</p></div></article>";
  }

  /* ------------------------------------------------------------------ router */

  function route() {
    var hash = window.location.hash.replace(/^#\/?/, "");
    var seg = hash.split("/").filter(Boolean);
    if (!seg.length) return renderIndex();
    if (seg[0] === "part") {
      var part = model.parts.filter(function (p) { return p.slug === seg[1]; })[0];
      return part ? renderPart(part) : renderIndex();
    }
    var ch = model.bySlug[seg[0]];
    if (!ch) return renderIndex();
    return renderChapter(ch, seg[1]);
  }

  function keyboardNav() {
    document.addEventListener("keydown", function (ev) {
      if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
      var tag = (ev.target.tagName || "").toLowerCase();
      if (tag === "input" || tag === "textarea") return;
      var sel = ev.key === "ArrowLeft" ? "header .pager a.prev"
        : ev.key === "ArrowRight" ? "header .pager a.next"
        : ev.key === "u" ? "header .pager a.up" : null;
      if (!sel) return;
      var a = document.querySelector(sel);
      if (a) window.location.hash = a.getAttribute("href").replace(/^#/, "#");
    });
  }

  function boot() {
    main = document.getElementById("main");
    crumbs = document.getElementById("crumbs");
    pagerTop = document.getElementById("pager");
    keyboardNav();
    fetchMd("index.md").then(function (text) {
      model = buildModel(text);
      window.addEventListener("hashchange", route);
      route();
    }).catch(function (err) { fail(String(err)); });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else boot();
})();
