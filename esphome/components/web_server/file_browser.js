// ESPHome web_server file browser module.
// Injected into the EXISTING web_server v3 page (served as /0.js via the js_include hook) —
// it appends a collapsible "Files" card below <esp-app>; it is NOT a separate page.
// Talks exclusively to the /files/* REST API.
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

  const card = $("div", { id: "esp-file-browser" });
  card.style.cssText = "max-width:600px;margin:8px auto;padding:12px;border:1px solid #8884;border-radius:12px;font-family:sans-serif";
  const title = $("h2", { textContent: "Files" });
  title.style.cssText = "cursor:pointer;margin:0;font-size:1.1em";
  const body = $("div");
  body.style.display = "none";
  title.onclick = () => { body.style.display = body.style.display === "none" ? "" : "none"; if (body.style.display === "") refresh(); };
  const status = $("div");
  status.style.cssText = "font-size:.85em;opacity:.8;min-height:1.2em";
  const listing = $("div");
  card.append(title, body);
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

  const row = (label, ...actions) => {
    const r = $("div");
    r.style.cssText = "display:flex;gap:8px;align-items:center;padding:2px 0";
    const l = $("span", { textContent: label });
    l.style.flex = "1";
    r.append(l, ...actions);
    return r;
  };
  const btn = (label, fn) => {
    const b = $("button", { textContent: label });
    b.style.cssText = "font-size:.8em";
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
          const acts = [open];
          // Per-direction capabilities: e.g. USB auto-mounts on insertion and only offers
          // safe-eject; each button also only makes sense in the opposite mounted state.
          if (s.can_mount && !s.mounted) {
            acts.push(btn("mount", () => api(`/files/mount?path=${encodeURIComponent(s.mount_path)}`, { method: "POST" })));
          }
          if (s.can_unmount && s.mounted) {
            acts.push(btn("unmount", () => api(`/files/unmount?path=${encodeURIComponent(s.mount_path)}`, { method: "POST" })));
          }
          listing.append(row(`${s.mount_path} (${s.type}${s.mounted ? "" : ", not mounted"})`, ...acts));
        }
      } else {
        listing.append(row(cwd, btn("up", async () => {
          const i = cwd.lastIndexOf("/");
          cwd = i > 0 ? cwd.slice(0, i) : null;
        })));
        const up = $("input", { type: "file" });
        const startBtn = $("button", { textContent: "start upload", disabled: true });
        startBtn.style.cssText = "font-size:.8em";
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
            listing.append(row("📁 " + e.name,
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
                ? api(`/files/delete?path=${encodeURIComponent(p)}&recursive=1`, { method: "POST" }) : Promise.resolve())));
          } else {
            const dl = $("a", { textContent: "download", href: `/files/download?path=${encodeURIComponent(p)}` });
            dl.style.fontSize = ".8em";
            listing.append(row(`${e.name} (${fmtSize(e.size)})`, dl,
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
                ? api(`/files/delete?path=${encodeURIComponent(p)}`, { method: "POST" }) : Promise.resolve())));
          }
        }
        if (d.truncated) listing.append(row("… (listing truncated)"));
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
