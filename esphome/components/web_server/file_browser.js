// ESPHome web_server file browser module.
// Injected into the EXISTING web_server v3 page (served as /0.js via the js_include hook) —
// it appends a collapsible "Files" card below <esp-app>; it is NOT a separate page.
// Talks exclusively to the /files/* REST API.
// Styling mirrors the v3 entity table (system font, hairline row separators, #03a9f4
// accent, automatic dark mode); all rules are scoped to #esp-file-browser so nothing
// leaks into the host page, whether we land inside the shadow root or in the body.
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

  const style = $("style", { textContent: `
    #esp-file-browser { max-width: 600px; margin: 0 auto; font-family: -apple-system, system-ui, sans-serif; color: #222; }
    #esp-file-browser .efb-title { display: flex; align-items: center; gap: 8px; cursor: pointer; user-select: none;
      font-size: 1.17em; font-weight: bold; padding: 10px 8px; border-top: 1px solid #e0e0e0; }
    #esp-file-browser .efb-chevron { color: #03a9f4; font-size: .8em; transition: transform .15s; }
    #esp-file-browser.efb-open .efb-chevron { transform: rotate(90deg); }
    #esp-file-browser .efb-row { display: flex; gap: 8px; align-items: center; padding: 6px 8px;
      border-top: 1px solid #e0e0e0; min-height: 28px; }
    #esp-file-browser .efb-row:hover { background: rgba(3, 169, 244, .06); }
    #esp-file-browser .efb-name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    #esp-file-browser .efb-dir { color: #03a9f4; margin-right: 6px; }
    #esp-file-browser .efb-size, #esp-file-browser .efb-muted { font-size: .85em; opacity: .6; }
    #esp-file-browser .efb-status { font-size: .85em; opacity: .8; min-height: 1.2em; padding: 4px 8px; }
    #esp-file-browser button, #esp-file-browser a.efb-act { font: inherit; font-size: .8em; color: #03a9f4;
      background: none; border: 1px solid #03a9f4; border-radius: 4px; padding: 2px 8px; cursor: pointer;
      text-decoration: none; line-height: 1.4; }
    #esp-file-browser button:hover, #esp-file-browser a.efb-act:hover { background: #03a9f4; color: #fff; }
    #esp-file-browser button:disabled { opacity: .4; pointer-events: none; }
    #esp-file-browser button.efb-primary { background: #03a9f4; color: #fff; }
    #esp-file-browser button.efb-danger { color: #d32f2f; border-color: #d32f2f; }
    #esp-file-browser button.efb-danger:hover { background: #d32f2f; color: #fff; }
    #esp-file-browser input[type=file] { font-size: .8em; max-width: 45%; }
    @media (prefers-color-scheme: dark) {
      #esp-file-browser { color: #e1e1e1; }
      #esp-file-browser .efb-title, #esp-file-browser .efb-row { border-top-color: #333; }
      #esp-file-browser .efb-row:hover { background: rgba(3, 169, 244, .12); }
    }
  ` });

  const card = $("div", { id: "esp-file-browser" });
  const title = $("div", { className: "efb-title" },
    $("span", { className: "efb-chevron", textContent: "\u25B6" }),
    $("span", { textContent: "Files" }));
  const body = $("div");
  body.style.display = "none";
  title.onclick = () => {
    const open = body.style.display === "none";
    body.style.display = open ? "" : "none";
    card.classList.toggle("efb-open", open);
    if (open) refresh();
  };
  const status = $("div", { className: "efb-status" });
  const listing = $("div");
  card.append(style, title, body);
  body.append(status, listing);

  let cwd = null; // null = storage overview

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

  // label may be a string or a prebuilt .efb-name element; extras (e.g. size) render
  // as muted text between the name and the action buttons, exactly like v3 states.
  const row = (label, ...actions) => {
    const r = $("div", { className: "efb-row" });
    const l = typeof label === "string" ? $("span", { className: "efb-name", textContent: label }) : label;
    r.append(l, ...actions);
    return r;
  };
  const dirName = (name) => $("span", { className: "efb-name" },
    $("span", { className: "efb-dir", textContent: "\u25B8" }), $("span", { textContent: name }));
  const btn = (label, fn, cls) => {
    const b = $("button", { textContent: label });
    if (cls) b.className = cls;
    b.onclick = () => fn().then(refresh).catch((e) => setStatus("Error: " + e.message));
    return b;
  };

  async function refresh() {
    listing.textContent = "";
    try {
      if (cwd === null) {
        const storages = await api("/files/storages");
        for (const s of storages) {
          const open = btn("open", async () => { cwd = s.mount_path; });
          const acts = [$("span", { className: "efb-muted", textContent: s.type + (s.mounted ? "" : " · not mounted") }), open];
          // Per-direction capabilities: e.g. USB auto-mounts on insertion and only offers
          // safe-eject; each button also only makes sense in the opposite mounted state.
          if (s.can_mount && !s.mounted) {
            acts.push(btn("mount", () => api(`/files/mount?path=${encodeURIComponent(s.mount_path)}`, { method: "POST" })));
          }
          if (s.can_unmount && s.mounted) {
            acts.push(btn("unmount", () => api(`/files/unmount?path=${encodeURIComponent(s.mount_path)}`, { method: "POST" })));
          }
          listing.append(row(dirName(s.mount_path), ...acts));
        }
      } else {
        listing.append(row(dirName(cwd), btn("up", async () => {
          const i = cwd.lastIndexOf("/");
          cwd = i > 0 ? cwd.slice(0, i) : null;
        })));
        const up = $("input", { type: "file" });
        const startBtn = $("button", { textContent: "start upload", disabled: true, className: "efb-primary" });
        up.onchange = () => { startBtn.disabled = !up.files.length; };
        startBtn.onclick = () => {
          if (!up.files.length) return;
          const f = up.files[0];
          startBtn.disabled = true;
          const fd = new FormData();
          fd.append("file", f);
          // XHR instead of fetch: fetch has no upload progress events
          const xhr = new XMLHttpRequest();
          xhr.open("POST", `/files/upload?path=${encodeURIComponent(cwd + "/" + f.name)}`);
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
            refresh();
          };
          xhr.onerror = () => { setStatus("Upload error: connection failed"); refresh(); };
          setStatus(`uploading ${f.name}…0%`);
          xhr.send(fd);
        };
        listing.append(row("upload:", up, startBtn, btn("mkdir", async () => {
          const n = prompt("New directory name");
          if (n) await api(`/files/mkdir?path=${encodeURIComponent(cwd + "/" + n)}`, { method: "POST" });
        })));
        const d = await api(`/files/list?path=${encodeURIComponent(cwd)}`);
        for (const e of d.entries) {
          const p = cwd + "/" + e.name;
          if (e.is_dir) {
            listing.append(row(dirName(e.name),
              btn("open", async () => { cwd = p; }),
              btn("copy", async () => {
                const to = prompt("Copy directory to (full path)", p);
                if (!to) return;
                const j = await api(`/files/copy?from=${encodeURIComponent(p)}&to=${encodeURIComponent(to)}`, { method: "POST" });
                await pollJob(j.job, "copy");
              }),
              btn("move", async () => {
                const to = prompt("Move/rename directory to (full path)", p);
                if (!to) return;
                const j = await api(`/files/move?from=${encodeURIComponent(p)}&to=${encodeURIComponent(to)}`, { method: "POST" });
                await pollJob(j.job, "move");
              }),
              btn("del", () => confirm(`Delete ${e.name} recursively?`)
                ? api(`/files/delete?path=${encodeURIComponent(p)}&recursive=1`, { method: "POST" }) : Promise.resolve(), "efb-danger")));
          } else {
            const dl = $("a", { textContent: "download", className: "efb-act", href: `/files/download?path=${encodeURIComponent(p)}` });
            listing.append(row(e.name, $("span", { className: "efb-size", textContent: fmtSize(e.size) }), dl,
              btn("info", async () => {
                const st = await api(`/files/stat?path=${encodeURIComponent(p)}`);
                alert(`${st.name}\nSize: ${st.size} bytes (${fmtSize(st.size)})\nModified: ${st.mtime ? new Date(st.mtime * 1000).toLocaleString() : "unknown"}`);
              }),
              btn("copy", async () => {
                const to = prompt("Copy to (full path)", p);
                if (!to) return;
                const j = await api(`/files/copy?from=${encodeURIComponent(p)}&to=${encodeURIComponent(to)}`, { method: "POST" });
                await pollJob(j.job, "copy");
              }),
              btn("move", async () => {
                const to = prompt("Move/rename to (full path)", p);
                if (!to) return;
                const j = await api(`/files/move?from=${encodeURIComponent(p)}&to=${encodeURIComponent(to)}`, { method: "POST" });
                await pollJob(j.job, "move");
              }),
              btn("del", () => confirm(`Delete ${e.name}?`)
                ? api(`/files/delete?path=${encodeURIComponent(p)}`, { method: "POST" }) : Promise.resolve(), "efb-danger")));
          }
        }
        if (d.truncated) listing.append(row($("span", { className: "efb-muted efb-name", textContent: "… (listing truncated)" })));
      }
    } catch (e) {
      setStatus("Error: " + e.message);
    }
  }

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
