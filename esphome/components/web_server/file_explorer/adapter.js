// Binds the js-fileexplorer widget to ESPHome's /files/* endpoints.
//
// Nothing in file-explorer.js is touched, so it can be swapped for a newer upstream copy
// without changing anything here. Every operation the widget performs is a callback that this
// file answers.
//
// Two things the widget has no concept of:
//   * storages. The root listing is the set of registered storages rather than a directory,
//     and mount/unmount are toolbar tools rather than file operations.
//   * jobs. copy and move answer {job:N} and finish later, so those callbacks poll
//     /files/job until it reports done and only then tell the widget.

(function () {
  'use strict';

  var API = '/files';
  var POLL_MS = 250;
  var POLL_MAX = 2400; // ten minutes — a large copy onto a slow card

  // ---------------------------------------------------------------------------
  // Transport
  // ---------------------------------------------------------------------------

  function request(method, url, cb) {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.onload = function () {
      var body = null;
      if (xhr.responseText) {
        try {
          body = JSON.parse(xhr.responseText);
        } catch (e) {
          body = null;
        }
      }
      cb(xhr.status >= 200 && xhr.status < 300, body, xhr.status);
    };
    xhr.onerror = function () {
      cb(false, null, 0);
    };
    xhr.send();
  }

  function q(params) {
    var out = [];
    for (var k in params) {
      if (params[k] !== undefined && params[k] !== null) {
        out.push(encodeURIComponent(k) + '=' + encodeURIComponent(params[k]));
      }
    }
    return out.length ? '?' + out.join('&') : '';
  }

  function errorText(body, status) {
    if (body && body.error) return body.error;
    if (status === 403) return "not permitted by this node's configuration";
    if (status === 404) return 'not found';
    if (status === 507) return 'not enough space';
    if (status === 0) return 'no response from the node';
    return 'failed (HTTP ' + status + ')';
  }

  // The widget models a location as an array of {id, name} from the root down. Element 0 is
  // the synthetic root; element 1 is the storage, whose id is its mount path.
  function pathOf(folder) {
    var parts = folder.GetPath();
    if (parts.length <= 1) return '';
    var p = parts[1][0];
    for (var i = 2; i < parts.length; i++) p += '/' + parts[i][1];
    return p;
  }

  function childPath(folder, name) {
    var base = pathOf(folder);
    return base === '' ? name : base + '/' + name;
  }

  function awaitJob(id, done) {
    var tries = 0;
    (function tick() {
      request('GET', API + '/job' + q({ id: id }), function (ok, body) {
        if (!ok || !body) return done(false, 'lost track of the transfer');
        if (body.state === 'done') {
          var good = !body.result || body.result === 'OK';
          return done(good, good ? null : body.result);
        }
        if (++tries > POLL_MAX) return done(false, 'transfer timed out');
        setTimeout(tick, POLL_MS);
      });
    })();
  }

  // ---------------------------------------------------------------------------
  // File kinds
  // ---------------------------------------------------------------------------

  var IMAGE_EXT = ['.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.svg', '.ico'];
  var TEXT_EXT = ['.txt', '.log', '.yaml', '.yml', '.conf', '.cfg', '.ini', '.csv', '.md', '.json'];

  function hasExt(name, list) {
    var lower = name.toLowerCase();
    for (var i = 0; i < list.length; i++) {
      if (lower.slice(-list[i].length) === list[i]) return true;
    }
    return false;
  }
  function isImage(name) {
    return hasExt(name, IMAGE_EXT);
  }
  function isText(name) {
    return hasExt(name, TEXT_EXT);
  }

  function entryFor(name, isDir, size, mtime, id) {
    return {
      id: id !== undefined ? id : name,
      name: name,
      type: isDir ? 'folder' : 'file',
      size: isDir ? undefined : size,
      hash: String(mtime || 0),
    };
  }

  // ---------------------------------------------------------------------------
  // Listing
  // ---------------------------------------------------------------------------

  function refreshRoot(cb) {
    request('GET', API + '/storages', function (ok, body, status) {
      if (!ok || !body || !body.storages) return cb(errorText(body, status));
      var entries = [];
      for (var i = 0; i < body.storages.length; i++) {
        var s = body.storages[i];
        var e = entryFor(s.mounted ? s.mount_path : s.mount_path + '  (not mounted)', true, 0, 0, s.mount_path);
        e.esphMounted = s.mounted;
        e.esphCanMount = !!s.can_mount;
        e.esphCanUnmount = !!s.can_unmount;
        entries.push(e);
      }
      cb(null, entries);
    });
  }

  function refreshFolder(folder, cb) {
    var path = pathOf(folder);
    if (path === '') return refreshRoot(cb);
    request('GET', API + '/list' + q({ path: path }), function (ok, body, status) {
      if (!ok || !body || !body.entries) return cb(errorText(body, status));
      var entries = [];
      for (var i = 0; i < body.entries.length; i++) {
        var it = body.entries[i];
        entries.push(entryFor(it.name, it.is_dir, it.size, it.mtime));
      }
      if (body.truncated) {
        // The node stopped early rather than spend the memory. Say so instead of presenting a
        // short listing as a complete one.
        entries.push(entryFor('… listing truncated by the node …', false, 0, 0));
      }
      cb(null, entries);
    });
  }

  // ---------------------------------------------------------------------------
  // Overlays: image preview, text editor, log follower
  // ---------------------------------------------------------------------------

  function overlay(title, contentElem, footerElems) {
    var back = document.createElement('div');
    back.className = 'esph-fe-overlay';
    var box = document.createElement('div');
    box.className = 'esph-fe-dialog';

    var head = document.createElement('div');
    head.className = 'esph-fe-dialog-head';
    var label = document.createElement('span');
    label.textContent = title;
    var close = document.createElement('button');
    close.className = 'esph-fe-close';
    close.textContent = '\u00d7';
    head.appendChild(label);
    head.appendChild(close);

    var foot = document.createElement('div');
    foot.className = 'esph-fe-dialog-foot';
    (footerElems || []).forEach(function (el) {
      foot.appendChild(el);
    });

    box.appendChild(head);
    box.appendChild(contentElem);
    box.appendChild(foot);
    back.appendChild(box);
    // Into the card, not document.body: the card sits in the v3 app's shadow root, and a
    // dialog parked outside it would get none of this sheet's rules.
    (card || document.body).appendChild(back);

    var closed = false;
    function shut() {
      if (closed) return;
      closed = true;
      if (back.parentNode) back.parentNode.removeChild(back);
    }
    close.onclick = shut;
    back.onclick = function (e) {
      if (e.target === back) shut();
    };

    return {
      close: shut,
      isClosed: function () {
        return closed;
      },
    };
  }

  function showImage(path, name) {
    var img = document.createElement('img');
    img.className = 'esph-fe-image';
    img.src = API + '/download' + q({ path: path, inline: '1' });
    overlay(name, img, []);
  }

  // Deliberately a plain textarea. This edits config and log files on a microcontroller; a
  // highlighting editor would cost more flash than everything else here put together.
  function showEditor(path, name, writable) {
    var area = document.createElement('textarea');
    area.className = 'esph-fe-editor';
    area.readOnly = true;
    area.value = 'loading\u2026';

    var status = document.createElement('span');
    status.className = 'esph-fe-status';
    var save = document.createElement('button');
    save.textContent = 'Save';
    save.disabled = true;

    overlay(name, area, writable ? [status, save] : [status]);

    var xhr = new XMLHttpRequest();
    xhr.open('GET', API + '/download' + q({ path: path, inline: '1' }), true);
    xhr.onload = function () {
      if (xhr.status >= 200 && xhr.status < 300) {
        area.value = xhr.responseText;
        area.readOnly = !writable;
        save.disabled = !writable;
        status.textContent = area.value.length + ' bytes';
      } else {
        area.value = '';
        status.textContent = 'could not read the file (HTTP ' + xhr.status + ')';
      }
    };
    xhr.onerror = function () {
      area.value = '';
      status.textContent = 'no response from the node';
    };
    xhr.send();

    save.onclick = function () {
      save.disabled = true;
      status.textContent = 'saving\u2026';
      var slash = path.lastIndexOf('/');
      var dir = slash > 0 ? path.slice(0, slash) : path;
      var base = slash >= 0 ? path.slice(slash + 1) : path;
      var form = new FormData();
      form.append('file', new Blob([area.value], { type: 'text/plain' }), base);
      var up = new XMLHttpRequest();
      up.open('POST', API + '/upload' + q({ path: path, dir: dir }), true);
      up.onload = function () {
        status.textContent = up.status >= 200 && up.status < 300 ? 'saved' : 'save failed (HTTP ' + up.status + ')';
        save.disabled = false;
      };
      up.onerror = function () {
        status.textContent = 'save failed';
        save.disabled = false;
      };
      up.send(form);
    };
  }

  // Follows a growing file the way tail -f does: ask only for the bytes past what is already
  // on screen, which is what Range support on /files/download is for. Polling rather than
  // streaming, because the node has no push channel for arbitrary files.
  function showTail(path, name) {
    var pre = document.createElement('pre');
    pre.className = 'esph-fe-tail';

    var status = document.createElement('span');
    status.className = 'esph-fe-status';
    var toggle = document.createElement('button');
    toggle.textContent = 'Pause';

    var dlg = overlay(name + ' \u2014 following', pre, [status, toggle]);

    var have = 0;
    var running = true;

    function append(text) {
      var atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 40;
      pre.textContent += text;
      if (atEnd) pre.scrollTop = pre.scrollHeight;
    }

    function poll() {
      if (dlg.isClosed()) return;
      if (!running) return setTimeout(poll, 1000);

      var xhr = new XMLHttpRequest();
      xhr.open('GET', API + '/download' + q({ path: path, inline: '1' }), true);
      if (have > 0) xhr.setRequestHeader('Range', 'bytes=' + have + '-');
      xhr.onload = function () {
        if (dlg.isClosed()) return;
        if (xhr.status === 206) {
          append(xhr.responseText);
          have += xhr.responseText.length;
          status.textContent = have + ' bytes';
        } else if (xhr.status === 200) {
          // No range honoured, or first fetch: take the whole thing.
          pre.textContent = '';
          append(xhr.responseText);
          have = xhr.responseText.length;
          status.textContent = have + ' bytes';
        } else if (xhr.status === 416) {
          status.textContent = 'waiting for more\u2026';
        } else {
          status.textContent = 'read failed (HTTP ' + xhr.status + ')';
        }
        setTimeout(poll, 1000);
      };
      xhr.onerror = function () {
        if (dlg.isClosed()) return;
        status.textContent = 'no response';
        setTimeout(poll, 2000);
      };
      xhr.send();
    }

    toggle.onclick = function () {
      running = !running;
      toggle.textContent = running ? 'Pause' : 'Resume';
    };

    poll();
  }

  // ---------------------------------------------------------------------------
  // Widget
  // ---------------------------------------------------------------------------

  function build(parent, access) {
    var canWrite = !!access.write;
    var canRead = !!access.read;
    var canMount = !!access.mount;
    var canUnmount = !!access.unmount;

    // Rebuilds a node-side path from the widget's source-path model.
    function pathFromParts(parts) {
      var p = '';
      for (var i = 1; i < parts.length; i++) {
        p = i === 1 ? parts[i][0] : p + '/' + parts[i][1];
      }
      return p;
    }

    function transfer(kind, done, srcpath, srcids, destfolder) {
      var src = pathFromParts(srcpath);
      var dest = pathOf(destfolder);
      var pending = srcids.length;
      var moved = [];
      var failure = null;
      if (!pending) return done(true, moved);

      srcids.forEach(function (id) {
        var from = src === '' ? id : src + '/' + id;
        var to = dest === '' ? id : dest + '/' + id;
        request('POST', API + '/' + kind + q({ from: from, to: to }), function (ok, body, status) {
          function finish(good, msg, entry) {
            if (good && entry) moved.push(entry);
            if (!good && failure === null) failure = msg;
            if (--pending === 0) done(failure === null ? true : failure, moved);
          }
          if (!ok || !body) return finish(false, errorText(body, status));
          if (body.job === undefined) return finish(true, null, entryFor(id, false, 0, 0));
          awaitJob(body.job, function (jok, msg) {
            finish(jok, msg, jok ? entryFor(id, false, 0, 0) : null);
          });
        });
      });
    }

    var opts = {
      group: 'esphome',
      initpath: [['', '/', { canmodify: false }]],

      onrefresh: function (folder) {
        folder.SetBusyRef(1);
        refreshFolder(folder, function (err, entries) {
          folder.SetBusyRef(-1);
          if (err) return folder.SetEntries([], { error: err });
          folder.SetEntries(entries);
        });
      },

      onopenfile: function (folder, entry) {
        if (entry.type === 'folder') return;
        var path = childPath(folder, entry.name);
        if (isImage(entry.name)) return showImage(path, entry.name);
        if (isText(entry.name)) return showEditor(path, entry.name, canWrite);
        window.location = API + '/download' + q({ path: path });
      },

      // Mount/unmount are per-storage, so they are toolbar tools that only light up when a
      // single root storage advertising the capability is selected -- see build() below.
      onselchanged: function () {
        updateStorageTools();
      },
    };

    if (canWrite) {
      opts.onnewfolder = function (created, folder) {
        var name = window.prompt('New folder name');
        if (!name) return created(false);
        request('POST', API + '/mkdir' + q({ path: childPath(folder, name) }), function (ok, body, status) {
          created(ok ? entryFor(name, true, 0, 0) : errorText(body, status));
        });
      };

      opts.onnewfile = function (created, folder) {
        var name = window.prompt('New file name');
        if (!name) return created(false);
        var form = new FormData();
        form.append('file', new Blob([''], { type: 'text/plain' }), name);
        var xhr = new XMLHttpRequest();
        xhr.open('POST', API + '/upload' + q({ path: childPath(folder, name), dir: pathOf(folder) }), true);
        xhr.onload = function () {
          var ok = xhr.status >= 200 && xhr.status < 300;
          created(ok ? entryFor(name, false, 0, 0) : 'could not create the file (HTTP ' + xhr.status + ')');
        };
        xhr.onerror = function () {
          created('no response from the node');
        };
        xhr.send(form);
      };

      // The API has no rename; a move inside the same directory is exactly that.
      opts.onrename = function (renamed, folder, entry, newname) {
        var from = childPath(folder, entry.name);
        var to = childPath(folder, newname);
        request('POST', API + '/move' + q({ from: from, to: to }), function (ok, body, status) {
          if (!ok || !body) return renamed(errorText(body, status));
          function done() {
            entry.name = newname;
            renamed(entry);
          }
          if (body.job === undefined) return done();
          awaitJob(body.job, function (jok, msg) {
            if (!jok) return renamed(msg || 'rename failed');
            done();
          });
        });
      };

      opts.ondelete = function (deleted, folder, ids, entries) {
        var pending = entries.length;
        var failure = null;
        if (!pending) return deleted(true);
        entries.forEach(function (entry) {
          var url = API + '/delete' + q({ path: childPath(folder, entry.name), recursive: '1' });
          request('POST', url, function (ok, body, status) {
            if (!ok && failure === null) failure = errorText(body, status);
            if (--pending === 0) deleted(failure === null ? true : failure);
          });
        });
      };

      opts.oncopy = function (copied, srcpath, srcids, destfolder) {
        transfer('copy', copied, srcpath, srcids, destfolder);
      };
      opts.onmove = function (moved, srcpath, srcids, destfolder) {
        transfer('move', moved, srcpath, srcids, destfolder);
      };

      opts.oninitupload = function (startupload, fileinfo) {
        fileinfo.uploadurl = API + '/upload' + q({ dir: pathOf(fileinfo.folder) });
        startupload(fileinfo, true);
      };
    }

    if (canRead) {
      opts.oninitdownload = function (startdownload, folder, ids, entries) {
        if (entries.length !== 1 || entries[0].type === 'folder') {
          // No archive endpoint on the node, so a multi-selection cannot become one file.
          return startdownload('select a single file to download');
        }
        startdownload({ url: API + '/download' + q({ path: childPath(folder, entries[0].name) }) });
      };
    }

    var fe = new FileExplorer(parent, opts);

    // Per-storage mount/unmount: a toolbar button added only when the node permits the
    // operation at all (server still enforces it with 403). updateStorageTools() then shows
    // it only while a single root storage that supports the operation is selected, and it
    // acts on that storage's mount path.
    function makeStorageTool(cls, title, endpoint) {
      var btn = fe.AddToolbarButton(cls, title);
      btn.classList.add('fe_fileexplorer_hidden');
      btn.addEventListener('click', function () {
        if (btn.classList.contains('fe_fileexplorer_disabled')) return;
        var sel = fe.GetSelectedFolderEntries();
        if (sel.length !== 1) return;
        var path = sel[0].id;
        request('POST', API + endpoint + q({ path: path }), function (ok, body, status) {
          if (!ok) window.alert(title.toLowerCase() + ' failed: ' + errorText(body, status));
          fe.RefreshFolders(true);
        });
      });
      return btn;
    }

    function toggleTool(btn, show) {
      if (!btn) return;
      btn.classList.toggle('fe_fileexplorer_hidden', !show);
      btn.classList.toggle('fe_fileexplorer_disabled', !show);
    }

    function updateStorageTools() {
      var atRoot = pathOf(fe.GetCurrentFolder()) === '';
      var sel = atRoot && fe.GetNumSelectedItems() === 1 ? fe.GetSelectedFolderEntries() : [];
      var e = sel.length === 1 ? sel[0] : null;
      toggleTool(mountTool, !!(e && e.esphCanMount && !e.esphMounted));
      toggleTool(unmountTool, !!(e && e.esphCanUnmount && e.esphMounted));
      fe.ToolStateUpdated();
    }

    var mountTool = canMount ? makeStorageTool('esph-fe-tool-mount', 'Mount storage', '/mount') : null;
    var unmountTool = canUnmount ? makeStorageTool('esph-fe-tool-unmount', 'Unmount storage', '/unmount') : null;

    addTools(fe, parent);
    return fe;
  }

  // The log follower lives in a strip above the widget; mount/unmount moved into the widget
  // toolbar as per-storage tools (see build()).
  function addTools(fe, parent) {
    var bar = document.createElement('div');
    bar.className = 'esph-fe-bar';

    function tool(label, fn) {
      var b = document.createElement('button');
      b.textContent = label;
      b.onclick = fn;
      bar.appendChild(b);
    }

    tool('Follow file', function () {
      var path = window.prompt('File to follow (e.g. /sdcard/logs/test.log)');
      if (path) showTail(path, path);
    });

    parent.insertBefore(bar, parent.firstChild);
  }

  // ---------------------------------------------------------------------------
  // Card and mounting
  // ---------------------------------------------------------------------------

  // Built in start(), read by overlay() — the dialogs go inside it.
  var card = null;

  // The v3 page frames each section as a tab header over a rounded container. The browser is
  // another such section, so it builds the same two elements rather than sitting in the page
  // as a bare block. Same shape the simple browser (file_browser.js) produces.
  function buildCard() {
    var el = document.createElement('div');
    el.id = 'esp-file-explorer';

    // Both stylesheets are linked from in here rather than from the document head: the card
    // is mounted into an open shadow root, and document-level sheets do not cross that
    // boundary. Relative URLs inside file-explorer.css (the sprite sheet, the icon font)
    // resolve against the sheet's own URL, so they still land on /file-explorer/*.
    ['/file-explorer/file-explorer.css', '/file-explorer/adapter.css'].forEach(function (href) {
      var link = document.createElement('link');
      link.rel = 'stylesheet';
      link.href = href;
      el.appendChild(link);
    });

    var tab = document.createElement('div');
    tab.className = 'esph-fe-tab';
    tab.textContent = 'Files';

    var frame = document.createElement('div');
    frame.className = 'esph-fe-frame';

    // The widget's own container. Kept as a separate element so file-explorer.js owns its
    // subtree outright and the frame's padding is not something it has to reason about.
    var host = document.createElement('div');
    host.id = 'file-explorer';
    frame.appendChild(host);

    el.appendChild(tab);
    el.appendChild(frame);
    return { card: el, host: host };
  }

  // Places the card directly below the entity table — above the log, which grows. The v3 app
  // renders into an open shadow root; if it has not rendered yet, retry once shortly after,
  // and fall back to appending next to <esp-app> so the browser is reachable either way.
  function attach(el) {
    var app = document.querySelector('esp-app');

    function mount() {
      var table = app && app.shadowRoot && app.shadowRoot.querySelector('esp-entity-table');
      if (table && table.parentNode) {
        table.parentNode.insertBefore(el, table.nextSibling);
        return true;
      }
      return false;
    }

    if (!mount()) {
      setTimeout(function () {
        if (!mount()) (app ? app.parentNode : document.body).appendChild(el);
      }, 300);
    }
  }

  // ---------------------------------------------------------------------------
  // Entry point
  // ---------------------------------------------------------------------------

  function start() {
    var built = buildCard();
    card = built.card;
    attach(card);
    // The access rights decide which callbacks are defined at all — the widget only shows a
    // rename affordance when onrename exists, so a read-only node gets a read-only browser
    // rather than buttons that fail.
    request('GET', API + '/storages', function (ok, body) {
      var access = ok && body && body.access ? body.access : { list: true, read: true, write: false };
      build(built.host, access);
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
