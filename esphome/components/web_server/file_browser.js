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

  const style = $("style", { textContent: `
    #esp-file-browser { max-width: 600px; margin: 0 auto;
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

  const btn = (label, fn, after, cls) => {
    const b = $("button", { textContent: label });
    if (cls) b.className = cls;
    b.onclick = () => fn().then(after).catch((e) => setStatus("Error: " + e.message));
    return b;
  };

  // --- tree ---------------------------------------------------------------
  // One node per directory: header row + (when expanded) a child container that is
  // filled by exactly one /files/list call. Collapse drops the children; every expand
  // re-fetches, so the view is always fresh and the device keeps zero listing state.

  const uploadRow = (path, reload) => {
    const up = $("input", { type: "file" });
    const startBtn = $("button", { textContent: "upload", disabled: true });
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
          let bytes = 0;
          try { bytes = JSON.parse(xhr.responseText).bytes; } catch (e) {}
          setStatus(`upload done: ${f.name} (${fmtSize(bytes)})`);
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
    r.append(up, startBtn, btn("mkdir", async () => {
      const n = prompt("New directory name");
      if (n) await api(`/files/mkdir?path=${enc(path + "/" + n)}`, { method: "POST" });
    }, reload));
    return r;
  };

  const copyMoveDel = (path, name, isDir, reload) => [
    btn("copy", async () => {
      const to = prompt(`Copy ${isDir ? "directory " : ""}to (full path)`, path);
      if (!to) return;
      const j = await api(`/files/copy?from=${enc(path)}&to=${enc(to)}`, { method: "POST" });
      await pollJob(j.job, "copy");
    }, reload),
    btn("move", async () => {
      const to = prompt(`Move/rename ${isDir ? "directory " : ""}to (full path)`, path);
      if (!to) return;
      const j = await api(`/files/move?from=${enc(path)}&to=${enc(to)}`, { method: "POST" });
      await pollJob(j.job, "move");
    }, reload),
    btn("del", () => confirm(isDir ? `Delete ${name} recursively?` : `Delete ${name}?`)
      ? api(`/files/delete?path=${enc(path)}${isDir ? "&recursive=1" : ""}`, { method: "POST" }) : Promise.resolve(),
      reload, "efb-danger"),
  ];

  const fileRow = (path, e, depth, reload) => {
    const r = $("div", { className: "efb-row" });
    r.style.paddingLeft = (8 + depth * 18) + "px";
    const dl = $("a", { textContent: "download", className: "efb-act", href: `/files/download?path=${enc(path)}` });
    r.append(
      $("span", { className: "efb-twist efb-none", textContent: "\u25B8" }),
      $("span", { className: "efb-name", textContent: e.name }),
      $("span", { className: "efb-size", textContent: fmtSize(e.size) }),
      dl,
      btn("info", async () => {
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
        // The upload/mkdir form belongs to every expanded directory (and mounted root).
        children.append(uploadRow(path, reloadChildren));
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
        const extras = [$("span", { className: "efb-muted", textContent: s.type + (s.mounted ? "" : " · not mounted") })];
        if (s.can_mount && !s.mounted) {
          extras.push(btn("mount", () => api(`/files/mount?path=${enc(s.mount_path)}`, { method: "POST" }), renderRoots));
        }
        if (s.can_unmount && s.mounted) {
          extras.push(btn("unmount", () => api(`/files/unmount?path=${enc(s.mount_path)}`, { method: "POST" }), renderRoots));
        }
        tree.append(dirNode(s.mount_path, s.mount_path, 0, { extras, canExpand: !!s.mounted }));
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
