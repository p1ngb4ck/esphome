// ESPHome web_server file browser module.
// Injected into the EXISTING web_server v3 page (served as /0.js via the js_include hook) —
// it appends a "Files" tab card below <esp-app>; it is NOT a separate page.
// Talks exclusively to the /files/* REST API.
// This file exists in two variants which differ ONLY in how action buttons render — codegen
// (web_server/__init__.py) embeds one of them per the file_browser: actions_as_icons option:
//   file_browser.js       — text labels in the flat borderless uppercase v3 button style
//   file_browser_icons.js — mdi icons
// Keep every other change in sync between the two.
//
// Look and feel is lifted verbatim from the v3 bundle: .tab-header / .tab-container framing
// and the flat uppercase action buttons (border:none, #03a9f4, 12.25px/1.09px tracking).
// The listing is a lazy tree: storage roots are always visible, expanding a directory issues
// exactly one /files/list call — all tree state lives in the browser, the device holds none.
(() => {
  const $ = (t, a = {}, ...c) => {
    const e = document.createElement(t);
    Object.assign(e, a);
    e.append(...c);
    return e;
  };
  const api = async (url, opts) => {
    const r = await fetch(url, opts);
    if (!r.ok) {
      let msg = r.status;
      try { msg = (await r.json()).error || msg; } catch (e) {}
      throw new Error(msg);
    }
    return r.json();
  };
  // Per-operation access advertised by /files/storages. Defaults are permissive so a button is
  // only ever hidden once the server has said the operation is disabled. Server-side each
  // disabled operation is still enforced with 403 — this only keeps the UI honest.
  const ACCESS = { list: true, read: true, write: true, delete: true, mount: true, unmount: true };
  // /files/storages now returns { access, storages }; capture access and return the array so the
  // two call sites can keep iterating storages as before.
  const loadStorages = async () => {
    const r = await api("/files/storages");
    if (r && r.access)
      Object.assign(ACCESS, r.access);
    return (r && r.storages) || [];
  };
  // Rewritten by codegen from the file_browser: change_poll_interval option; 0 disables the
  // auto-refresh poll entirely.
  const CHANGE_POLL_MS = 5000;

  const fmtSize = (n) => n < 1024 ? n + " B" : n < 1048576 ? (n / 1024).toFixed(1) + " kB" : (n / 1048576).toFixed(1) + " MB";
  const enc = encodeURIComponent;

  // Material Design icon paths (24x24), rendered inline so they follow currentColor.
  const TYPE_ICONS = {
    sd: "M18,8H16V4H18M15,8H13V4H15M12,8H10V4H12M18,2H10L4,8V20A2,2 0 0,0 6,22H18A2,2 0 0,0 20,20V4A2,2 0 0,0 18,2Z",
    usb: "M15,7V11H16V13H13V5H15L12,1L9,5H11V13H8V10.93C8.7,10.56 9.2,9.85 9.2,9C9.2,7.78 8.21,6.8 7,6.8C5.78,6.8 4.8,7.78 4.8,9C4.8,9.85 5.3,10.56 6,10.93V13A2,2 0 0,0 8,15H11V18.05C10.29,18.41 9.8,19.15 9.8,20C9.8,21.22 10.78,22.2 12,22.2C13.22,22.2 14.2,21.22 14.2,20C14.2,19.15 13.71,18.41 13,18.05V15H16A2,2 0 0,0 18,13V11H19V7H15Z",
    net: "M10,2C8.89,2 8,2.89 8,4V7C8,8.11 8.89,9 10,9H11V11H2V13H6V15H5C3.89,15 3,15.89 3,17V20C3,21.11 3.89,22 5,22H9C10.11,22 11,21.11 11,20V17C11,15.89 10.11,15 9,15H8V13H16V15H15C13.89,15 13,15.89 13,17V20C13,21.11 13.89,22 15,22H19C20.11,22 21,21.11 21,20V17C21,15.89 20.11,15 19,15H18V13H22V11H13V9H14C15.11,9 16,8.11 16,7V4C16,2.89 15.11,2 14,2H10Z",
    mem: "M6,4H18V5H21V7H18V9H21V11H18V13H21V15H18V17H21V19H18V20H6V19H3V17H6V15H3V13H6V11H3V9H6V7H3V5H6V4M8,6V18H16V6H8M10,8H14V16H10V8Z",
    disk: "M12,3C7.58,3 4,4.79 4,7C4,9.21 7.58,11 12,11C16.42,11 20,9.21 20,7C20,4.79 16.42,3 12,3M4,9V12C4,14.21 7.58,16 12,16C16.42,16 20,14.21 20,12V9C20,11.21 16.42,13 12,13C7.58,13 4,11.21 4,9M4,14V17C4,19.21 7.58,21 12,21C16.42,21 20,19.21 20,17V14C20,16.21 16.42,18 12,18C7.58,18 4,16.21 4,14Z",
  };
  const ICONS = TYPE_ICONS;
  const icon = (name) => {
    const NS = "http://www.w3.org/2000/svg";
    const s = document.createElementNS(NS, "svg");
    s.setAttribute("viewBox", "0 0 24 24");
    s.setAttribute("width", "16");
    s.setAttribute("height", "16");
    const path = document.createElementNS(NS, "path");
    path.setAttribute("fill", "currentColor");
    path.setAttribute("d", ICONS[name]);
    s.append(path);
    return s;
  };
  // Type/medium icons do not go through act() — they are identification, not actions,
  // and render as icons in both variants.
  const ACT_TEXT = { movedir: "move", all: "read all" };
  // Labels are the shared action names, except where those would mislead as words.
  const act = (name) => ACT_TEXT[name] || name;
  // Icon per medium: 'kind' from /files/storages when the driver reports one (sd, usb,
  // nfs, flash, littlefs, eeprom, ...), otherwise the coarse type (filesystem/network).
  const typeIcon = (kind, type) => {
    const k = (kind || type || "").toLowerCase();
    if (k.includes("sd")) return "sd";
    if (k.includes("usb")) return "usb";
    if (k.includes("nfs") || k.includes("net")) return "net";
    if (k.includes("flash") || k.includes("eeprom") || k.includes("fram") ||
        k.includes("littlefs") || k.includes("part"))
      return "mem";
    return "disk";
  };

  const style = $("style", { textContent: `
    #esp-file-browser {
      font-family: -apple-system, system-ui, Helvetica, Roboto, Oxygen, Ubuntu, sans-serif; color: inherit; }
    /* tab header + frame: verbatim v3 tab-header / tab-container */
    #esp-file-browser .efb-tab { display: inline-flex; max-width: 90%; font-weight: 400;
      padding-inline: 1.5em; padding-top: .5em; padding-bottom: .5em; align-items: center;
      border-radius: 12px 12px 0 0; background-color: rgba(127,127,127,.3); margin-top: 1em; user-select: none; }
    #esp-file-browser .efb-frame { border: 2px solid rgba(127,127,127,.3); border-radius: 0 12px 12px 12px;
      padding: 4px 0; }
    #esp-file-browser .efb-row { display: flex; gap: 4px; align-items: center; padding: 2px 8px; min-height: 32px; }
    /* zebra stripes, same neutral grey family as the v3 tables; declared before :hover so
       hovering still reads as the stronger highlight (equal specificity, later rule wins) */
    #esp-file-browser .efb-row.efb-odd { background: rgba(127,127,127,.07); }
    #esp-file-browser .efb-row:hover { background: rgba(127,127,127,.12); }
    #esp-file-browser .efb-name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    #esp-file-browser .efb-twist { width: 1.1em; text-align: center; color: #03a9f4; cursor: pointer;
      user-select: none; transition: transform .15s; flex: none; }
    #esp-file-browser .efb-twist.efb-open { transform: rotate(90deg); }
    #esp-file-browser .efb-twist.efb-none { visibility: hidden; cursor: default; }
    #esp-file-browser .efb-dirname { cursor: pointer; }
    #esp-file-browser .efb-size, #esp-file-browser .efb-muted { font-size: .85em; opacity: .6; flex: none; }
    #esp-file-browser .efb-status { font-size: .85em; opacity: .8; min-height: 1.2em; padding: 2px 12px; }
    /* actions: verbatim v3 button style — flat, borderless, uppercase */
    #esp-file-browser button, #esp-file-browser a.efb-act {
      font-family: ui-monospace, system-ui, Helvetica, Roboto, Oxygen, Ubuntu, sans-serif;
      cursor: pointer; border-radius: 4px; color: #03a9f4; border: none; background-color: unset;
      padding: 8px; font-weight: 500; font-size: 12.25px; letter-spacing: 1.09375px;
      text-transform: uppercase; text-decoration: none; transition: all 350ms; flex: none; }
    #esp-file-browser button:hover, #esp-file-browser a.efb-act:hover { background: rgba(3,169,244,.12); }
    #esp-file-browser button:disabled { opacity: .4; pointer-events: none; }
    #esp-file-browser button svg, #esp-file-browser a.efb-act svg { display: block; }
    #esp-file-browser button, #esp-file-browser a.efb-act { padding: 6px; }
    #esp-file-browser .efb-type { flex: none; display: flex; opacity: .75; }
    #esp-file-browser .efb-modal-back { position: fixed; inset: 0; background: rgba(0,0,0,.45);
      display: flex; align-items: center; justify-content: center; z-index: 10; }
    /* Canvas/CanvasText are the CSS system colors: they resolve against the element's
       EFFECTIVE color-scheme — which the v3 app toggles by setting 'color-scheme' on <html>
       (its theme button), independent of the OS preference. Hardcoded colors keyed to a
       prefers-color-scheme media query desynced from that toggle (white modal chrome on the
       dark page and vice versa); the system colors follow whatever the page currently is. */
    #esp-file-browser .efb-modal { background: Canvas; color: CanvasText;
      border: 2px solid rgba(127,127,127,.3); border-radius: 12px; padding: 1em 1.25em;
      min-width: 32em; max-width: 92%; width: 40em; box-shadow: 0 4px 24px rgba(0,0,0,.3); }
    #esp-file-browser .efb-modal-title { font-weight: 500; margin-bottom: .75em; }
    #esp-file-browser .efb-field { display: flex; align-items: center; gap: .75em;
      justify-content: space-between; margin: .4em 0; }
    #esp-file-browser .efb-field-label { font-size: .9em; opacity: .8; }
    #esp-file-browser .efb-field-note { font-size: .85em; opacity: .7; margin: .4em 0; font-style: italic; }
    #esp-file-browser .efb-picker { min-height: 16em; max-height: 55vh; overflow-y: auto;
      margin: .3em 0 .6em; border: 1px solid rgba(127,127,127,.3); border-radius: 4px; padding: 4px 0; }
    #esp-file-browser .efb-picker-row { font-size: .9em; display: flex; align-items: center; }
    #esp-file-browser .efb-picker-pick { margin-left: auto; padding: 0 .6em; cursor: pointer;
      opacity: .7; font-size: .8em; text-decoration: underline; }
    #esp-file-browser .efb-picker-pick:hover { opacity: 1; }
    #esp-file-browser .efb-field input[type=text] { flex: 1; min-width: 0; max-width: 60%;
      background: transparent; color: inherit; border: none;
      border-bottom: 1px solid rgba(127,127,127,.4); padding: 2px 0; font-family: ui-monospace, monospace; }
    #esp-file-browser .efb-modal-bar { display: flex; justify-content: flex-end; margin-top: 1em; }
    #esp-file-browser button.efb-danger { color: #d32f2f; }
    #esp-file-browser button.efb-danger:hover { background: rgba(211,47,47,.12); }
    #esp-file-browser input[type=file] { font-size: .8em; flex: 1; min-width: 0; }
    #esp-file-browser .efb-ow { font-size: .8em; opacity: .8; display: flex; align-items: center; gap: .25em; white-space: nowrap; }
    /* busy spinner shown while a directory listing is in flight */
    #esp-file-browser .efb-spin { flex: none; width: 12px; height: 12px; border: 2px solid rgba(3,169,244,.25);
      border-top-color: #03a9f4; border-radius: 50%; animation: efb-rot .7s linear infinite; }
    @keyframes efb-rot { to { transform: rotate(360deg); } }
  ` });

  const card = $("div", { id: "esp-file-browser" });
  const tab = $("div", { className: "efb-tab", textContent: "Files" });
  const frame = $("div", { className: "efb-frame" });
  const status = $("div", { className: "efb-status" });
  const tree = $("div");
  card.append(style, tab, frame);
  frame.append(status, tree);

  // The status line remembers whether it currently shows an error, so background state
  // changes (change poll) can retire errors that no longer describe reality — e.g. a mount
  // failure whose card has since been swapped and auto-mounted.
  let statusIsError = false;
  const setStatus = (t) => { status.textContent = t || ""; statusIsError = /^Error/i.test(t || ""); };

  // Zebra striping. The listing is a lazy tree of nested containers, so CSS nth-child cannot
  // alternate across expansion levels — instead a tiny pass walks the *visible* rows in DOM
  // order and marks every second one. Driven by a MutationObserver (rows appear/disappear,
  // subtrees collapse via display:none) and debounced to one run per frame.
  let stripePending = false;
  const restripe = () => {
    if (stripePending) return;
    stripePending = true;
    requestAnimationFrame(() => {
      stripePending = false;
      let i = 0;
      for (const r of tree.querySelectorAll(".efb-row")) {
        if (r.offsetParent === null) continue;  // inside a collapsed (display:none) subtree
        r.classList.toggle("efb-odd", (i++ & 1) === 1);
      }
    });
  };
  // attributeFilter: collapsing toggles children.style.display; class flips above do not
  // match "style", so the observer cannot feed itself.
  new MutationObserver(restripe).observe(tree, {
    childList: true, subtree: true, attributes: true, attributeFilter: ["style"],
  });

  const pollJob = async (job, label) => {
    for (;;) {
      const s = await api(`/files/job?id=${job}`);
      if (s.state === "done") {
        setStatus(`${label}: ${s.result === "OK" ? "done" : s.result}`);
        return;
      }
      if (s.file && !s.bytes_total) {
        // A tree job: the whole tree's total is unknown, the file in flight has one.
        const pct = s.file_total > 0 ? ` ${Math.round(100 * s.file_done / s.file_total)}%` : "";
        setStatus(`${label}: ${s.file}${pct} — Total: ${fmtSize(s.bytes_done)}`);
      } else {
        setStatus(s.bytes_total > 0
          ? `${label}: ${Math.round(100 * s.bytes_done / s.bytes_total)}% (${fmtSize(s.bytes_done)}/${fmtSize(s.bytes_total)})`
          : `${label}: ${fmtSize(s.bytes_done)}…`);
      }
      await new Promise((r) => setTimeout(r, 500));
    }
  };

  const btn = (name, title, fn, after, cls) => {
    const b = $("button", { title }, act(name));
    b.setAttribute("aria-label", title);
    if (cls) b.className = cls;
    b.onclick = () => fn().then(after).catch((e) => setStatus("Error: " + e.message));
    return b;
  };

  // Directories currently expanded, path -> their reload; the change poll below relists
  // exactly these when the server reports them dirty.
  const openDirs = new Map();

  // The upload/mkdir form exists exactly once, attached to the most recently expanded
  // (= marked) directory or root; expanding elsewhere moves it there.
  let currentUpload = null;

  // --- tree ---------------------------------------------------------------
  // One node per directory: header row + (when expanded) a child container that is
  // filled by exactly one /files/list call. Collapse drops the children; every expand
  // re-fetches, so the view is always fresh and the device keeps zero listing state.

  const uploadRow = (path, reload) => {
    const up = $("input", { type: "file" });
    const ow = $("input", { type: "checkbox", title: "Overwrite if the file already exists" });
    const owLabel = $("label", { className: "efb-ow" }, ow, document.createTextNode(" overwrite"));
    const startBtn = $("button", { title: "Upload", disabled: true }, act("upload"));
    up.onchange = () => { startBtn.disabled = !up.files.length; };
    startBtn.onclick = () => {
      if (!up.files.length) return;
      const f = up.files[0];
      startBtn.disabled = true;
      const fd = new FormData();
      fd.append("file", f);
      // XHR instead of fetch: fetch has no upload progress events
      const xhr = new XMLHttpRequest();
      xhr.open("POST", `/files/upload?path=${enc(path + "/" + f.name)}${ow.checked ? "&overwrite=1" : ""}`);
      xhr.upload.onprogress = (e) => setStatus(e.lengthComputable
        ? `uploading ${f.name}: ${Math.round(100 * e.loaded / e.total)}% (${fmtSize(e.loaded)}/${fmtSize(e.total)})`
        : `uploading ${f.name}: ${fmtSize(e.loaded)}…`);
      xhr.onload = () => {
        if (xhr.status === 200) {
          let j = {};
          try { j = JSON.parse(xhr.responseText); } catch (e) {}
          if (j.job) {
            // staged upload: the device flushes the PSRAM buffer to storage in background
            setStatus(`upload received: ${f.name} (${fmtSize(j.bytes || 0)}) — flushing…`);
            pollJob(j.job, "flush").catch((e) => setStatus("Flush error: " + e.message)).then(reload);
            return;
          }
          setStatus(`upload done: ${f.name} (${fmtSize(j.bytes || 0)})`);
        } else {
          let msg = xhr.status === 409 ? `${f.name} already exists (tick overwrite to replace)` : xhr.status;
          try { msg = JSON.parse(xhr.responseText).error || msg; } catch (e) {}
          setStatus("Upload error: " + msg);
        }
        reload();
      };
      xhr.onerror = () => { setStatus("Upload error: connection failed"); reload(); };
      setStatus(`uploading ${f.name}…0%`);
      xhr.send(fd);
    };
    const r = $("div", { className: "efb-row" });
    r.append(up, owLabel, startBtn, btn("mkdir", "New directory", async () => {
      const n = prompt("New directory name");
      if (n) await api(`/files/mkdir?path=${enc(path + "/" + n)}`, { method: "POST" });
    }, reload));
    return r;
  };

  // Copy and move share one flow. overwrite is chosen up front in the modal; when it is off and
  // the destination exists the API still answers 409, which we surface as an error.
  const transfer = async (op, from, to, overwrite) => {
    const q = `from=${enc(from)}&to=${enc(to)}${overwrite ? "&overwrite=1" : ""}`;
    const r = await fetch(`/files/${op}?${q}`, { method: "POST" });
    if (!r.ok) {
      let msg = r.status === 409 ? `${to} already exists (enable overwrite to replace)` : r.status;
      try { msg = (await r.json()).error || msg; } catch (e) {}
      throw new Error(msg);
    }
    await pollJob((await r.json()).job, op);
  };

  // Target picker shared by copy and move: pick a destination directory (or type it), give the
  // new name, choose overwrite. dir + name compose the full destination path.
  const transferModal = (op, path, name, isDir) => {
    const parent = path.slice(0, path.lastIndexOf("/")) || "/";
    modal(`${op === "copy" ? "Copy" : "Move / rename"} ${isDir ? "directory " : ""}${name}`, [
      { key: "to_dir", label: "Destination directory", value: parent },
      { type: "picker", mode: "dir", target: "to_dir", label: "…or pick a destination directory:" },
      { key: "new_name", label: "Name", value: name },
      { key: "overwrite", label: "Overwrite if it exists", type: "check", value: false },
      { type: "note", compute: (v) =>
        "Destination: " + ((v.to_dir || parent).replace(/\/$/, "")) + "/" + (v.new_name || name) },
    ], async (v) => {
      const dir = (v.to_dir || parent).replace(/\/$/, "");
      const nm = (v.new_name || name).trim();
      if (!nm) throw new Error("no name given");
      await transfer(op, path, dir + "/" + nm, v.overwrite);
    });
  };

  // copy is a read (source read, new file written); move deletes the source, so it is grouped
  // with delete. Each button appears only when its operation is allowed.
  const copyMoveDel = (path, name, isDir, reload) => {
    const out = [];
    if (ACCESS.read)
      out.push(btn("copy", "Copy", async () => transferModal("copy", path, name, isDir), reload));
    if (ACCESS.delete) {
      out.push(btn(isDir ? "movedir" : "move", "Move / rename", async () => transferModal("move", path, name, isDir), reload));
      out.push(btn("del", "Delete", () => confirm(isDir ? `Delete ${name} recursively?` : `Delete ${name}?`)
        ? api(`/files/delete?path=${enc(path)}${isDir ? "&recursive=1" : ""}`, { method: "POST" }) : Promise.resolve(),
        reload, "efb-danger"));
    }
    return out;
  };

  const fileRow = (path, e, depth, reload) => {
    const r = $("div", { className: "efb-row" });
    r.style.paddingLeft = (8 + depth * 18) + "px";
    r.append(
      $("span", { className: "efb-twist efb-none", textContent: "\u25B8" }),
      $("span", { className: "efb-name", textContent: e.name }),
      $("span", { className: "efb-size", textContent: fmtSize(e.size) }));
    if (ACCESS.read)
      r.append($("a", { title: "Download", className: "efb-act", href: `/files/download?path=${enc(path)}` }, act("download")));
    r.append(
      btn("info", "Details", async () => {
        const st = await api(`/files/stat?path=${enc(path)}`);
        alert(`${st.name}\nSize: ${st.size} bytes (${fmtSize(st.size)})\nModified: ${st.mtime ? new Date(st.mtime * 1000).toLocaleString() : "unknown"}`);
      }, () => {}),
      ...copyMoveDel(path, e.name, false, reload));
    return r;
  };

  // dirNode: header row + child container; extras = trailing row content (e.g. root state
  // and mount/unmount); actions = per-directory action buttons (omitted for roots).
  const dirNode = (path, name, depth, opts) => {
    const wrap = $("div");
    const row = $("div", { className: "efb-row" });
    row.style.paddingLeft = (8 + depth * 18) + "px";
    const twist = $("span", { className: "efb-twist", textContent: "\u25B8" });
    const label = $("span", { className: "efb-name efb-dirname", textContent: name });
    const children = $("div");
    children.style.display = "none";
    let expanded = false;

    const reloadChildren = async () => {
      const spin = $("span", { className: "efb-spin" });
      row.insertBefore(spin, label.nextSibling);
      try {
        const d = await api(`/files/list?path=${enc(path)}`);
        children.textContent = "";
        if (currentUpload != null && currentUpload.parentNode != null)
          currentUpload.remove();
        // Upload + New-directory both write, so the whole row appears only when write is allowed.
        currentUpload = ACCESS.write ? uploadRow(path, reloadChildren) : null;
        if (currentUpload != null)
          children.append(currentUpload);
        const entries = d.entries.slice().sort((a, b) =>
          (b.is_dir - a.is_dir) || a.name.localeCompare(b.name));
        for (const e of entries) {
          const p = path + "/" + e.name;
          if (e.is_dir) {
            children.append(dirNode(p, e.name, depth + 1, { actions: copyMoveDel(p, e.name, true, reloadChildren) }));
          } else {
            children.append(fileRow(p, e, depth + 1, reloadChildren));
          }
        }
        if (d.truncated) {
          const t = $("div", { className: "efb-row" });
          t.style.paddingLeft = (8 + (depth + 1) * 18) + "px";
          t.append($("span", { className: "efb-muted efb-name", textContent: "… (listing truncated)" }));
          children.append(t);
        }
      } finally {
        spin.remove();
      }
    };

    const toggle = () => {
      if (opts.canExpand === false) return;
      expanded = !expanded;
      twist.classList.toggle("efb-open", expanded);
      children.style.display = expanded ? "" : "none";
      if (expanded) {
        openDirs.set(path, reloadChildren);
        reloadChildren().catch((e) => setStatus("Error: " + e.message));
      } else {
        openDirs.delete(path);
        children.textContent = "";
      }
    };
    twist.onclick = toggle;
    label.onclick = toggle;
    if (opts.canExpand === false) twist.classList.add("efb-none");

    row.append(twist, label, ...(opts.extras || []), ...(opts.actions || []));
    wrap.append(row, children);
    return wrap;
  };

  // --- embedded file/dir picker ------------------------------------------
  // Reuses the same /files endpoints as the tree, but renders a minimal navigate-and-select
  // view (no per-entry actions). mode "file" makes files selectable, mode "dir" makes
  // directories selectable. onPick(path) is called with the chosen path.
  const pickerView = (mode, onPick) => {
    const box = $("div", { className: "efb-picker" });
    const renderList = async (path, container, depth) => {
      container.textContent = "";
      let d;
      try {
        d = await api(`/files/list?path=${enc(path)}`);
      } catch (e) {
        container.append($("div", { className: "efb-muted efb-name", textContent: "Error: " + e.message }));
        return;
      }
      const entries = d.entries.slice().sort((a, b) => (b.is_dir - a.is_dir) || a.name.localeCompare(b.name));
      for (const e of entries) {
        const p = path + "/" + e.name;
        const row = $("div", { className: "efb-row efb-picker-row" });
        row.style.paddingLeft = (8 + depth * 18) + "px";
        if (e.is_dir) {
          const twist = $("span", { className: "efb-twist", textContent: "\u25B8" });
          const label = $("span", { className: "efb-name efb-dirname", textContent: e.name });
          const kids = $("div");
          kids.style.display = "none";
          let open = false;
          const toggle = () => {
            open = !open;
            twist.classList.toggle("efb-open", open);
            kids.style.display = open ? "" : "none";
            if (open) renderList(p, kids, depth + 1);
            else kids.textContent = "";
          };
          twist.onclick = toggle;
          label.onclick = mode === "dir" ? () => onPick(p) : toggle;
          row.append(twist, label);
          if (mode === "dir")
            row.append($("span", { className: "efb-picker-pick", textContent: "select" , onclick: () => onPick(p) }));
          const wrap = $("div");
          wrap.append(row, kids);
          container.append(wrap);
        } else {
          const label = $("span", { className: "efb-name", textContent: e.name });
          const size = $("span", { className: "efb-size", textContent: fmtSize(e.size) });
          row.append($("span", { className: "efb-twist efb-none", textContent: "\u25B8" }), label, size);
          if (mode === "file") {
            row.style.cursor = "pointer";
            row.onclick = () => onPick(p);
          }
          container.append(row);
        }
      }
    };
    (async () => {
      try {
        for (const s of await loadStorages()) {
          if (!s.mounted) continue;
          const rootRow = $("div", { className: "efb-row efb-picker-row" });
          const twist = $("span", { className: "efb-twist", textContent: "\u25B8" });
          const label = $("span", { className: "efb-name efb-dirname", textContent: s.mount_path });
          const kids = $("div");
          kids.style.display = "none";
          let open = false;
          const toggle = () => {
            open = !open;
            twist.classList.toggle("efb-open", open);
            kids.style.display = open ? "" : "none";
            if (open) renderList(s.mount_path, kids, 1);
            else kids.textContent = "";
          };
          twist.onclick = toggle;
          label.onclick = mode === "dir" ? () => onPick(s.mount_path) : toggle;
          rootRow.append(twist, label);
          if (mode === "dir")
            rootRow.append($("span", { className: "efb-picker-pick", textContent: "select", onclick: () => onPick(s.mount_path) }));
          const wrap = $("div");
          wrap.append(rootRow, kids);
          box.append(wrap);
        }
      } catch (e) {
        box.append($("div", { className: "efb-muted efb-name", textContent: "Error: " + e.message }));
      }
    })();
    return box;
  };

  // --- modal --------------------------------------------------------------
  // Raw operations need parameters (address, size, a path) that a single icon cannot carry —
  // and each of them is destructive or long-running enough to deserve a deliberate confirm.
  //
  // Field types: "text" (default), "check", "note" (read-only line, recomputed on every change
  // via compute(values)), and "picker" (embedded file/dir chooser that fills a target text
  // field). A picker field has no value of its own; it writes into fields[key=f.target].
  const modal = (title, fields, onSubmit) => {
    const back = $("div", { className: "efb-modal-back" });
    const box = $("div", { className: "efb-modal" });
    box.append($("div", { className: "efb-modal-title", textContent: title }));
    const inputs = {};
    const notes = [];  // { el, compute }
    const snapshot = () => {
      const v = {};
      for (const f of fields)
        if (f.type !== "note" && f.type !== "picker")
          v[f.key] = f.type === "check" ? inputs[f.key].checked : inputs[f.key].value.trim();
      return v;
    };
    const refresh = () => {
      const v = snapshot();
      for (const n of notes) n.el.textContent = n.compute(v);
    };
    for (const f of fields) {
      if (f.type === "note") {
        const line = $("div", { className: "efb-field-note" });
        notes.push({ el: line, compute: f.compute });
        box.append(line);
        continue;
      }
      if (f.type === "picker") {
        // Embedded chooser: clicking an entry writes its path into the target text field.
        const line = $("div", { className: "efb-field-note", textContent: f.label });
        box.append(line);
        box.append(pickerView(f.mode, (path) => {
          if (inputs[f.target]) {
            // dir mode fills the directory; a filename field (if any) is appended by the caller's
            // note/compute. file mode fills the whole path.
            inputs[f.target].value = f.mode === "dir" && f.filenameKey && inputs[f.filenameKey]
              ? path + "/" + (inputs[f.filenameKey].value.trim() || "")
              : path;
            refresh();
          }
        }));
        continue;
      }
      const row = $("label", { className: "efb-field" });
      row.append($("span", { className: "efb-field-label", textContent: f.label }));
      const el = f.type === "check"
        ? $("input", { type: "checkbox", checked: !!f.value })
        : $("input", { type: "text", value: f.value == null ? "" : String(f.value) });
      if (f.hint) el.placeholder = f.hint;
      inputs[f.key] = el;
      el.addEventListener(f.type === "check" ? "change" : "input", refresh);
      row.append(el);
      box.append(row);
    }
    refresh();
    const close = () => back.remove();
    const bar = $("div", { className: "efb-modal-bar" });
    const cancel = $("button", { textContent: "cancel" });
    cancel.onclick = close;
    const ok = $("button", { textContent: "ok" });
    ok.onclick = () => {
      const values = snapshot();
      close();
      Promise.resolve(onSubmit(values)).catch((e) => setStatus("Error: " + e.message));
    };
    bar.append(cancel, ok);
    box.append(bar);
    back.append(box);
    back.onclick = (e) => { if (e.target === back) close(); };
    card.append(back);
    const first = box.querySelector("input");
    if (first) first.focus();
  };

  // --- device nodes (raw media) -------------------------------------------
  // A raw device has no directories, so its node is a leaf: no twisty, just its operations.
  // What those are comes from /raw/devices — the driver's own geometry plus what this build
  // allows — so the UI never offers what the medium or the config rules out.
  const fmtHex = (n) => "0x" + Number(n).toString(16).toUpperCase();

  const deviceNode = (dev) => {
    const wrap = $("div");
    const row = $("div", { className: "efb-row" });
    row.style.paddingLeft = "8px";
    row.append(
      $("span", { className: "efb-twist efb-none", textContent: "\u25B8" }),
      $("span", { className: "efb-type", title: dev.kind }, icon(typeIcon(dev.kind, ""))),
      $("span", { className: "efb-name", textContent: dev.node_name || dev.id }),
      $("span", { className: "efb-size", textContent: fmtSize(dev.capacity) }));

    // Raw actions are worker jobs now (the API answers {job:N}): poll like copy/move does.

    // A job that vanishes between polls finished and had its slot recycled — treat as done.

    // Raw jobs poll /raw/job (their own endpoint + result cache): /files/job never learned a
    // raw job's final result — the worker recycles the slot right after completion, the poll
    // then 404'd and every raw error was silently swallowed as "done".
    // Human phase prefix for the status line: "erasing… ", "verifying (pass 2/3)… ", etc.
    // Empty for the plain write phase and for phase-less jobs.
    const phaseLabel = (s) => {
      if (s.phase === "erase") return "erasing ";
      if (s.phase === "verify") {
        const p = s.verify_passes > 1 ? ` (pass ${s.verify_pass}/${s.verify_passes})` : "";
        return `verifying${p} `;
      }
      return "";
    };
    const waitRawJob = async (job, label) => {
      let verified = 0;
      for (;;) {
        await new Promise((res) => setTimeout(res, 500));
        let s;
        try { s = await api(`/raw/job?id=${job}`); } catch (e) { break; }
        if (!s || s.state === undefined) break;
        if (s.phase === "verify" && s.verify_passes) verified = s.verify_passes;
        if (s.state === "done") {
          // error_to_string() spells success "OK" (uppercase) — comparing against "ok" turned
          // every successful job into a reported error.
          if (s.result && s.result !== "OK") throw new Error(`${label}: ${s.result}`);
          break;
        }
        if (s.bytes_total > 0) setStatus(`${label}\u2026 ${phaseLabel(s)}${fmtSize(s.bytes_done)} / ${fmtSize(s.bytes_total)}`);
        else if (s.bytes_done > 0) setStatus(`${label}\u2026 ${phaseLabel(s)}${fmtSize(s.bytes_done)}`);
        else if (s.phase) setStatus(`${label}\u2026 ${phaseLabel(s)}`.trim());
      }
      const vsuffix = verified ? ` \u2014 verified (${verified} pass${verified > 1 ? "es" : ""})` : "";
      setStatus(`${label} \u2014 done${vsuffix}`);
      // Relist whatever is expanded — same as the change poll's reset path. (A raw job may
      // have written a file into any open directory.)
      for (const r of openDirs.values()) r().catch(() => {});
    };


    const readModal = (whole) => modal(whole ? `Read all of ${dev.node_name}` : `Read from ${dev.node_name}`, [
      { key: "address", label: "Address", value: whole ? "0x0" : "0x0" },
      { key: "size", label: "Size (bytes)", value: whole ? dev.capacity : 256 },
      { key: "to_path", label: "To file on device", hint: "empty = download" },
      { key: "filename", label: "Filename (for picker)", hint: "dump.bin" },
      { type: "picker", mode: "dir", target: "to_path", filenameKey: "filename",
        label: "…or pick a target directory (uses the filename above):" },
    ], async (v) => {
      const q = `device=${enc(dev.id)}&address=${enc(v.address)}&size=${enc(v.size)}`;
      if (!v.to_path) {
        window.location.href = `/raw/read?${q}`;
        return;
      }
      setStatus(`reading ${dev.node_name} \u2192 ${v.to_path}\u2026`);
      const r = await api(`/raw/read?${q}&to_path=${enc(v.to_path)}`);
      await waitRawJob(r.job, `reading ${dev.node_name} \u2192 ${v.to_path}`);
    });

    {
      const acts = [
        btn("download", "Read a range", async () => readModal(false), () => {}),
        btn("all", "Read the whole device", async () => readModal(true), () => {}),
      ];
      if (dev.writable) {
        acts.push(btn("upload", "Write a file to this device", async () => modal(`Write to ${dev.node_name}`, [
          { key: "address", label: "Address", value: "0x0" },
          { key: "from_path", label: "File on device", hint: "/sdcard/fw.bin" },
          { type: "picker", mode: "file", target: "from_path", label: "…or pick a file:" },
          ...(dev.write_needs_erase
            ? [{ key: "erase", label: `Erase first (${fmtSize(dev.erase_sector)} sectors)`, type: "check", value: true }]
            : []),
          { key: "verify", label: "Verify after write (read back and compare)", type: "check", value: true },
          { key: "verify_passes", label: "Verify passes", value: 1 },
        ], async (v) => {
          if (!v.from_path) throw new Error("no file given");
          setStatus(`writing ${v.from_path} \u2192 ${dev.node_name}\u2026`);
          const passes = v.verify ? Math.max(1, parseInt(v.verify_passes) || 1) : 0;
          const q = `device=${enc(dev.id)}&address=${enc(v.address)}&from_path=${enc(v.from_path)}&verify=${passes}`;
          const r = await api(`/raw/write?${q}${v.erase ? "&erase=1" : ""}`, { method: "POST" });
          await waitRawJob(r.job, `writing ${v.from_path} \u2192 ${dev.node_name}`);
        }), () => {}));
      }
      if (dev.writable) {
        acts.push(btn("verify", "Verify against a file", async () => modal(`Verify ${dev.node_name} against a file`, [
          { key: "address", label: "Address", value: "0x0" },
          { key: "from_path", label: "File on device", hint: "/sdcard/fw.bin" },
          { type: "picker", mode: "file", target: "from_path", label: "…or pick a file:" },
          { key: "passes", label: "Verify passes", value: 1 },
        ], async (v) => {
          if (!v.from_path) throw new Error("no file given");
          setStatus(`verifying ${dev.node_name} against ${v.from_path}\u2026`);
          const passes = Math.max(1, parseInt(v.passes) || 1);
          const q = `device=${enc(dev.id)}&address=${enc(v.address)}&from_path=${enc(v.from_path)}&passes=${passes}`;
          const r = await api(`/raw/verify?${q}`, { method: "POST" });
          await waitRawJob(r.job, `verifying ${dev.node_name} against ${v.from_path}`);
        }), () => {}));
      }
      if (dev.erasable) {
        acts.push(btn("erase", "Erase a range", async () => modal(`Erase ${dev.node_name}`, [
          // Pseudo-erase media (EEPROM/FRAM: no erase opcode, the device fills 0xFF via
          // writes) are byte-addressable — no sector multiple exists to demand.
          { key: "address", label: dev.pseudo_erase ? "Address" : `Address (multiple of ${fmtHex(dev.erase_sector)})`, value: "0x0" },
          { key: "size", label: dev.pseudo_erase ? "Size (bytes)" : `Size (multiple of ${fmtHex(dev.erase_sector)})`, value: dev.pseudo_erase ? 256 : dev.erase_sector },
          { key: "all", label: "Erase the whole device", type: "check", value: false },
          // Only meaningful when a whole-chip erase would otherwise be used (can_erase_chip is
          // already true only for a task-safe chip-erase-capable device). Checking it forces the
          // slower block-by-block path instead — handy for comparing the two.
          ...(dev.can_erase_chip
            ? [{ key: "sliced", label: "Erase sector-by-sector (skip fast chip erase)", type: "check", value: false }]
            : []),
        ], async (v) => {
          if (v.all && !confirm(`Erase all of ${dev.node_name}? Everything on it is gone.`)) return;
          const q = (v.all
            ? `device=${enc(dev.id)}&all=1`
            : `device=${enc(dev.id)}&address=${enc(v.address)}&size=${enc(v.size)}`)
            + (v.sliced ? "&sliced=1" : "");
          setStatus(`erasing ${dev.node_name}\u2026`);
          const r = await api(`/raw/erase?${q}`, { method: "POST" });
          await waitRawJob(r.job, `erasing ${dev.node_name}`);
        }), () => {}, "efb-danger"));
      }
      acts.push(btn("info", "Details", async () => {
        alert(`${dev.node_name} (${dev.kind})\n` +
          `Capacity: ${fmtSize(dev.capacity)}\n` +
          `Write page: ${dev.write_page} B\n` +
          (dev.erase_sector ? `Erase sector: ${fmtSize(dev.erase_sector)}\n`
            : dev.pseudo_erase && dev.erasable ? "Erase: pseudo (fills 0xFF)\n" : "Erase: not supported\n") +
          (dev.erase_block ? `Erase block: ${fmtSize(dev.erase_block)}\n` : "") +
          (dev.write_needs_erase ? "Writes need an erase first\n" : "Writes overwrite in place\n"));
      }, () => {}));
      row.append(...acts);
    }

    wrap.append(row);
    return wrap;
  };

  // --- roots: always visible ----------------------------------------------
  async function renderRoots() {
    tree.textContent = "";
    openDirs.clear();  // every node below is rebuilt collapsed
    try {
      const storages = await loadStorages();
      for (const s of storages) {
                const extras = s.mounted ? [] : [$("span", { className: "efb-muted", textContent: "not mounted" })];
        if (ACCESS.mount && s.can_mount && !s.mounted) {
          extras.push(btn("mount", "Mount", () => api(`/files/mount?path=${enc(s.mount_path)}`, { method: "POST" }), renderRoots));
        }
        if (ACCESS.unmount && s.can_unmount && s.mounted) {
          extras.push(btn("unmount", "Unmount", () => api(`/files/unmount?path=${enc(s.mount_path)}`, { method: "POST" }), renderRoots));
        }
        const node = dirNode(s.mount_path, s.mount_path, 0, { extras, canExpand: !!s.mounted });
        const row = node.firstChild;
        const ti = $("span", { className: "efb-type", title: s.kind || s.type }, icon(typeIcon(s.kind, s.type)));
        row.insertBefore(ti, row.children[1]);
        tree.append(node);
      }
    } catch (e) {
      setStatus("Error: " + e.message);
    }
    // Raw devices, if this build has the raw API and any device asked for a node. A 404 simply
    // means no raw API — not an error worth showing.
    try {
      for (const dev of await api("/raw/devices")) {
        if (dev.node)
          tree.append(deviceNode(dev));
      }
    } catch (e) {
      /* no raw api configured */
    }
  }
  renderRoots();


  // Auto-refresh. /files/changes hands back the directories whose listings changed since this
  // client's cursor — including changes made by API calls no browser initiated. Only what is
  // actually expanded gets relisted; "" marks the roots level (a mount came or went). A
  // cursor that fell behind the server's small ring comes back as reset — then everything
  // open is considered dirty. Errors stay quiet: the next tick simply tries again.
  let changeCursor = 0;
  const pollChanges = async () => {
    const c = await api(`/files/changes?since=${changeCursor}`);
    changeCursor = c.seq;
    // The server reports actual state changes: whatever error the line still shows is
    // about a world that no longer exists. Only then — an unchanged world keeps its error.
    if (statusIsError && (c.reset || (c.dirs || []).length))
      setStatus("");
    if (c.reset) {
      for (const r of openDirs.values()) r().catch(() => {});
      return;
    }
    for (const d of c.dirs || []) {
      if (d === "") { renderRoots(); return; }  // rebuilds every node — nothing further to do
      const r = openDirs.get(d);
      if (r) r().catch(() => {});
    }
  };
  if (CHANGE_POLL_MS > 0)
    setInterval(() => { if (!document.hidden) pollChanges().catch(() => {}); }, CHANGE_POLL_MS);

  const attach = () => {
    const app = document.querySelector("esp-app");
    // Place the card right below the entity table, ABOVE the (growing) log. The v3 app
    // renders inside an open shadow root; fall back to appending after <esp-app> (and retry
    // once shortly after, in case the app hasn't rendered its shadow content yet).
    const mount = () => {
      const table = app && app.shadowRoot && app.shadowRoot.querySelector("esp-entity-table");
      if (table && table.parentNode) {
        table.parentNode.insertBefore(card, table.nextSibling);
        return true;
      }
      return false;
    };
    if (!mount()) {
      setTimeout(() => {
        if (!mount()) (app ? app.parentNode : document.body).appendChild(card);
      }, 300);
    }
  };
  document.readyState === "loading" ? document.addEventListener("DOMContentLoaded", attach) : attach();
})();
