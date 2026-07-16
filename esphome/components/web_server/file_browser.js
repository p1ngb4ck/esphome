// ESPHome web_server file browser module.
// Injected into the EXISTING web_server v3 page (served as /0.js via the js_include hook) —
// it appends a "Files" tab card below <esp-app>; it is NOT a separate page.
// Talks exclusively to the /files/* REST API.
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
  const fmtSize = (n) => n < 1024 ? n + " B" : n < 1048576 ? (n / 1024).toFixed(1) + " kB" : (n / 1048576).toFixed(1) + " MB";
  const enc = encodeURIComponent;

  // Material Design icon paths (24x24), rendered inline so they follow currentColor.
  const ICONS = {
    download: "M5,20H19V18H5M19,9H15V3H9V9H5L12,16L19,9Z",
    upload: "M9,16V10H5L12,3L19,10H15V16H9M5,20V18H19V20H5Z",
    info: "M13,9H11V7H13M13,17H11V11H13M12,2A10,10 0 0,0 2,12A10,10 0 0,0 12,22A10,10 0 0,0 22,12A10,10 0 0,0 12,2Z",
    copy: "M19,21H8V7H19M19,5H8A2,2 0 0,0 6,7V21A2,2 0 0,0 8,23H19A2,2 0 0,0 21,21V7A2,2 0 0,0 19,5M16,1H4A2,2 0 0,0 2,3V17H4V3H16V1Z",
    move: "M4,11V13H16L10.5,18.5L11.92,19.92L19.84,12L11.92,4.08L10.5,5.5L16,11H4Z",
    del: "M19,4H15.5L14.5,3H9.5L8.5,4H5V6H19M6,19A2,2 0 0,0 8,21H16A2,2 0 0,0 18,19V7H6V19Z",
    mkdir: "M10,4L12,6H20A2,2 0 0,1 22,8V18A2,2 0 0,1 20,20H4C2.89,20 2,19.1 2,18V6C2,4.89 2.89,4 4,4H10M15,9V12H12V14H15V17H17V14H20V12H17V9H15Z",
    mount: "M16,7V3H14V7H10V3H8V7C7.45,7 7,7.45 7,8V13.5L10.5,17V21H13.5V17L17,13.5V8C17,7.45 16.55,7 16,7Z",
    unmount: "M12,5L5.33,15H18.67L12,5M5,17H19V19H5V17Z",
    sd: "M18,8H16V4H18M15,8H13V4H15M12,8H10V4H12M18,2H10L4,8V20A2,2 0 0,0 6,22H18A2,2 0 0,0 20,20V4A2,2 0 0,0 18,2Z",
    usb: "M15,7V11H16V13H13V5H15L12,1L9,5H11V13H8V10.93C8.7,10.56 9.2,9.85 9.2,9C9.2,7.78 8.21,6.8 7,6.8C5.78,6.8 4.8,7.78 4.8,9C4.8,9.85 5.3,10.56 6,10.93V13A2,2 0 0,0 8,15H11V18.05C10.29,18.41 9.8,19.15 9.8,20C9.8,21.22 10.78,22.2 12,22.2C13.22,22.2 14.2,21.22 14.2,20C14.2,19.15 13.71,18.41 13,18.05V15H16A2,2 0 0,0 18,13V11H19V7H15Z",
    net: "M10,2C8.89,2 8,2.89 8,4V7C8,8.11 8.89,9 10,9H11V11H2V13H6V15H5C3.89,15 3,15.89 3,17V20C3,21.11 3.89,22 5,22H9C10.11,22 11,21.11 11,20V17C11,15.89 10.11,15 9,15H8V13H16V15H15C13.89,15 13,15.89 13,17V20C13,21.11 13.89,22 15,22H19C20.11,22 21,21.11 21,20V17C21,15.89 20.11,15 19,15H18V13H22V11H13V9H14C15.11,9 16,8.11 16,7V4C16,2.89 15.11,2 14,2H10Z",
    mem: "M6,4H18V5H21V7H18V9H21V11H18V13H21V15H18V17H21V19H18V20H6V19H3V17H6V15H3V13H6V11H3V9H6V7H3V5H6V4M8,6V18H16V6H8M10,8H14V16H10V8Z",
    disk: "M12,3C7.58,3 4,4.79 4,7C4,9.21 7.58,11 12,11C16.42,11 20,9.21 20,7C20,4.79 16.42,3 12,3M4,9V12C4,14.21 7.58,16 12,16C16.42,16 20,14.21 20,12V9C20,11.21 16.42,13 12,13C7.58,13 4,11.21 4,9M4,14V17C4,19.21 7.58,21 12,21C16.42,21 20,19.21 20,17V14C20,16.21 16.42,18 12,18C7.58,18 4,16.21 4,14Z",
  };
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
  const typeIcon = (t) => {
    const k = (t || "").toLowerCase();
    return k.includes("sd") ? "sd" : k.includes("usb") ? "usb"
      : (k.includes("nfs") || k.includes("net")) ? "net"
      : (k.includes("flash") || k.includes("part") || k.includes("data")) ? "mem" : "disk";
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
    #esp-file-browser button.efb-danger { color: #d32f2f; }
    #esp-file-browser button.efb-danger:hover { background: rgba(211,47,47,.12); }
    #esp-file-browser input[type=file] { font-size: .8em; flex: 1; min-width: 0; }
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

  const setStatus = (t) => { status.textContent = t || ""; };

  const pollJob = async (job, label) => {
    for (;;) {
      const s = await api(`/files/job?id=${job}`);
      if (s.state === "done") {
        setStatus(`${label}: ${s.result === "OK" ? "done" : s.result}`);
        return;
      }
      setStatus(s.bytes_total > 0
        ? `${label}: ${Math.round(100 * s.bytes_done / s.bytes_total)}% (${fmtSize(s.bytes_done)}/${fmtSize(s.bytes_total)})`
        : `${label}: ${fmtSize(s.bytes_done)}…`);
      await new Promise((r) => setTimeout(r, 500));
    }
  };

  const btn = (name, title, fn, after, cls) => {
    const b = $("button", { title }, icon(name));
    b.setAttribute("aria-label", title);
    if (cls) b.className = cls;
    b.onclick = () => fn().then(after).catch((e) => setStatus("Error: " + e.message));
    return b;
  };

  // The upload/mkdir form exists exactly once, attached to the most recently expanded
  // (= marked) directory or root; expanding elsewhere moves it there.
  let currentUpload = null;

  // --- tree ---------------------------------------------------------------
  // One node per directory: header row + (when expanded) a child container that is
  // filled by exactly one /files/list call. Collapse drops the children; every expand
  // re-fetches, so the view is always fresh and the device keeps zero listing state.

  const uploadRow = (path, reload) => {
    const up = $("input", { type: "file" });
    const startBtn = $("button", { title: "Upload", disabled: true }, icon("upload"));
    up.onchange = () => { startBtn.disabled = !up.files.length; };
    startBtn.onclick = () => {
      if (!up.files.length) return;
      const f = up.files[0];
      startBtn.disabled = true;
      const fd = new FormData();
      fd.append("file", f);
      // XHR instead of fetch: fetch has no upload progress events
      const xhr = new XMLHttpRequest();
      xhr.open("POST", `/files/upload?path=${enc(path + "/" + f.name)}`);
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
          let msg = xhr.status;
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
    r.append(up, startBtn, btn("mkdir", "New directory", async () => {
      const n = prompt("New directory name");
      if (n) await api(`/files/mkdir?path=${enc(path + "/" + n)}`, { method: "POST" });
    }, reload));
    return r;
  };

  const copyMoveDel = (path, name, isDir, reload) => [
    btn("copy", "Copy", async () => {
      const to = prompt(`Copy ${isDir ? "directory " : ""}to (full path)`, path);
      if (!to) return;
      const j = await api(`/files/copy?from=${enc(path)}&to=${enc(to)}`, { method: "POST" });
      await pollJob(j.job, "copy");
    }, reload),
    btn("move", "Move / rename", async () => {
      const to = prompt(`Move/rename ${isDir ? "directory " : ""}to (full path)`, path);
      if (!to) return;
      const j = await api(`/files/move?from=${enc(path)}&to=${enc(to)}`, { method: "POST" });
      await pollJob(j.job, "move");
    }, reload),
    btn("del", "Delete", () => confirm(isDir ? `Delete ${name} recursively?` : `Delete ${name}?`)
      ? api(`/files/delete?path=${enc(path)}${isDir ? "&recursive=1" : ""}`, { method: "POST" }) : Promise.resolve(),
      reload, "efb-danger"),
  ];

  const fileRow = (path, e, depth, reload) => {
    const r = $("div", { className: "efb-row" });
    r.style.paddingLeft = (8 + depth * 18) + "px";
    const dl = $("a", { title: "Download", className: "efb-act", href: `/files/download?path=${enc(path)}` }, icon("download"));
    r.append(
      $("span", { className: "efb-twist efb-none", textContent: "\u25B8" }),
      $("span", { className: "efb-name", textContent: e.name }),
      $("span", { className: "efb-size", textContent: fmtSize(e.size) }),
      dl,
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
        currentUpload = uploadRow(path, reloadChildren);
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
      if (expanded) reloadChildren().catch((e) => setStatus("Error: " + e.message));
      else children.textContent = "";
    };
    twist.onclick = toggle;
    label.onclick = toggle;
    if (opts.canExpand === false) twist.classList.add("efb-none");

    row.append(twist, label, ...(opts.extras || []), ...(opts.actions || []));
    wrap.append(row, children);
    return wrap;
  };

  // --- roots: always visible ----------------------------------------------
  async function renderRoots() {
    tree.textContent = "";
    try {
      const storages = await api("/files/storages");
      for (const s of storages) {
                const extras = s.mounted ? [] : [$("span", { className: "efb-muted", textContent: "not mounted" })];
        if (s.can_mount && !s.mounted) {
          extras.push(btn("mount", "Mount", () => api(`/files/mount?path=${enc(s.mount_path)}`, { method: "POST" }), renderRoots));
        }
        if (s.can_unmount && s.mounted) {
          extras.push(btn("unmount", "Unmount", () => api(`/files/unmount?path=${enc(s.mount_path)}`, { method: "POST" }), renderRoots));
        }
        const node = dirNode(s.mount_path, s.mount_path, 0, { extras, canExpand: !!s.mounted });
        const row = node.firstChild;
        const ti = $("span", { className: "efb-type", title: s.type }, icon(typeIcon(s.type)));
        row.insertBefore(ti, row.children[1]);
        tree.append(node);
      }
    } catch (e) {
      setStatus("Error: " + e.message);
    }
  }
  renderRoots();

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
