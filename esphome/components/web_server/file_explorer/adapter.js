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
  var RAW = '/raw';
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

  // ---------------------------------------------------------------------------
  // Raw device nodes (block media): /raw/devices + read/write/verify/erase, all
  // long ops as worker jobs polled on /raw/job. Ported from the simple browser.
  // ---------------------------------------------------------------------------

  function fmtSize(n) {
    n = Number(n) || 0;
    var u = ['B', 'KB', 'MB', 'GB'];
    var i = 0;
    while (n >= 1024 && i < u.length - 1) {
      n /= 1024;
      i++;
    }
    return (i ? n.toFixed(1) : n) + ' ' + u[i];
  }

  function fmtHex(n) {
    return '0x' + (Number(n) || 0).toString(16).toUpperCase();
  }

  var rawStatusNode = null;
  function rawStatus(text, sticky) {
    if (!rawStatusNode) {
      rawStatusNode = document.createElement('div');
      rawStatusNode.setAttribute('style',
        'position:absolute;left:12px;bottom:12px;max-width:70%;z-index:8;padding:8px 12px;' +
        'background:Canvas;color:CanvasText;border:1px solid rgba(127,127,127,0.4);border-radius:8px;' +
        'box-shadow:0 4px 16px rgba(0,0,0,0.3);font:13px system-ui,-apple-system,sans-serif;');
      (card || document.body).appendChild(rawStatusNode);
    }
    rawStatusNode.textContent = text;
    if (!sticky) {
      setTimeout(function () {
        if (rawStatusNode && rawStatusNode.textContent === text) {
          document.body.removeChild(rawStatusNode);
          rawStatusNode = null;
        }
      }, 5000);
    }
  }

  function pollRawJob(job, label, done) {
    var iv = setInterval(function () {
      request('GET', RAW + '/job' + q({ id: job }), function (ok, s) {
        // A job that 404s between polls finished and had its slot recycled -- treat as done.
        if (!ok || !s || s.state === undefined) {
          clearInterval(iv);
          rawStatus(label + ' -- done');
          if (done) done();
          return;
        }
        if (s.state === 'done') {
          clearInterval(iv);
          rawStatus(s.result && s.result !== 'OK' ? label + ' failed: ' + s.result : label + ' -- done');
          if (done) done();
          return;
        }
        var phase = s.phase === 'erase' ? 'erasing '
          : s.phase === 'verify'
            ? 'verifying' + (s.verify_passes > 1 ? ' (pass ' + s.verify_pass + '/' + s.verify_passes + ')' : '') + ' '
            : '';
        if (s.bytes_total > 0) rawStatus(label + '... ' + phase + fmtSize(s.bytes_done) + ' / ' + fmtSize(s.bytes_total), true);
        else if (s.bytes_done > 0) rawStatus(label + '... ' + phase + fmtSize(s.bytes_done), true);
        else if (phase) rawStatus(label + '... ' + phase, true);
      });
    }, 500);
  }

  // A small labelled-field dialog on top of overlay(). fields: {key,label,value,hint,type}.
  // Lightweight file/dir picker: navigate available storages -> dirs -> files and hand a path
  // back. mode 'file' picks a file, mode 'dir' picks the folder currently shown. Only mounted
  // storages (and non-mountable ones that are loaded) are offered.
  function browsePath(mode, onpick) {
    var cur = '';
    var roots = [];
    var chosenFile = null;
    var box = document.createElement('div');
    box.className = 'esph-fe-browser';
    var pathbar = document.createElement('div');
    pathbar.className = 'esph-fe-browser-path';
    var list = document.createElement('div');
    list.className = 'esph-fe-browser-list';
    box.appendChild(pathbar);
    box.appendChild(list);
    var pick = document.createElement('button');
    pick.textContent = mode === 'dir' ? 'Select this folder' : 'Select file';
    var cancel = document.createElement('button');
    cancel.textContent = 'Cancel';
    var dlg = overlay(mode === 'dir' ? 'Choose a folder' : 'Choose a file', box, [cancel, pick]);
    cancel.onclick = dlg.close;
    pick.onclick = function () {
      if (mode === 'dir') { dlg.close(); onpick(cur); }
      else if (chosenFile !== null) { dlg.close(); onpick(chosenFile); }
    };
    function parentOf(p) {
      if (roots.indexOf(p) !== -1) return '';
      var i = p.lastIndexOf('/');
      return i > 0 ? p.slice(0, i) : '';
    }
    function addRow(label, cls, onclick) {
      var r = document.createElement('div');
      r.className = 'esph-fe-browser-row' + (cls ? ' ' + cls : '');
      r.textContent = label;
      r.onclick = onclick;
      list.appendChild(r);
      return r;
    }
    function render(items, isRoot) {
      list.textContent = '';
      pathbar.textContent = cur || '/';
      chosenFile = null;
      pick.disabled = mode === 'file';
      if (!isRoot) addRow('.. (up)', 'esph-fe-browser-up', function () { navigate(parentOf(cur)); });
      items.forEach(function (it) {
        if (it.isDir) {
          addRow(it.name + '/', 'esph-fe-browser-dir', function () {
            navigate(cur === '' ? it.path : (cur.replace(/\/$/, '') + '/' + it.name));
          });
        } else if (mode === 'file') {
          var full = cur.replace(/\/$/, '') + '/' + it.name;
          var row = addRow(it.name, 'esph-fe-browser-file', function () {
            chosenFile = full;
            pick.disabled = false;
            var sel = list.querySelector('.esph-fe-browser-sel');
            if (sel) sel.classList.remove('esph-fe-browser-sel');
            row.classList.add('esph-fe-browser-sel');
          });
        }
      });
    }
    function navigate(path) {
      cur = path;
      if (path === '') {
        request('GET', API + '/storages', function (ok, body) {
          var items = [];
          roots = [];
          if (ok && body && body.storages) body.storages.forEach(function (s) {
            var avail = !s.can_mount;
            if (avail) { roots.push(s.mount_path); items.push({ name: s.mount_path, isDir: true, path: s.mount_path }); }
          });
          render(items, true);
        });
      } else {
        request('GET', API + '/list' + q({ path: path }), function (ok, body) {
          var items = [];
          if (ok && body && body.entries) body.entries.forEach(function (it) {
            items.push({ name: it.name, isDir: !!it.is_dir });
          });
          render(items, false);
        });
      }
    }
    navigate('');
  }

  function formDialog(title, fields, onRun) {
    var content = document.createElement('div');
    content.style.padding = '12px';
    content.style.minWidth = '320px';
    var inputs = {};
    fields.forEach(function (f) {
      var wrap = document.createElement('div');
      wrap.style.margin = '6px 0';
      var input = document.createElement('input');
      if (f.type === 'check') {
        input.type = 'checkbox';
        input.checked = !!f.value;
        var l = document.createElement('label');
        l.style.fontSize = '13px';
        l.appendChild(input);
        l.appendChild(document.createTextNode(' ' + f.label));
        wrap.appendChild(l);
      } else if (f.type === 'dirpick' || f.type === 'filepick') {
        input.type = 'text';
        input.value = f.value ? String(f.value) : '';
        input.readOnly = true;
        if (f.hint) input.placeholder = f.hint;
        input.style.flex = '1 1 auto';
        input.style.boxSizing = 'border-box';
        var labp = document.createElement('div');
        labp.style.fontSize = '12px';
        labp.style.marginBottom = '2px';
        labp.textContent = f.label;
        var browse = document.createElement('button');
        browse.type = 'button';
        browse.textContent = 'Browse\u2026';
        browse.onclick = function () {
          browsePath(f.type === 'dirpick' ? 'dir' : 'file', function (p) { input.value = p; });
        };
        var prow = document.createElement('div');
        prow.style.display = 'flex';
        prow.style.gap = '6px';
        prow.appendChild(input);
        prow.appendChild(browse);
        wrap.appendChild(labp);
        wrap.appendChild(prow);
      } else {
        input.type = 'text';
        input.value = f.value !== undefined && f.value !== null ? String(f.value) : '';
        if (f.hint) input.placeholder = f.hint;
        input.style.width = '100%';
        input.style.boxSizing = 'border-box';
        var lab = document.createElement('div');
        lab.style.fontSize = '12px';
        lab.style.marginBottom = '2px';
        lab.textContent = f.label;
        wrap.appendChild(lab);
        wrap.appendChild(input);
      }
      inputs[f.key] = input;
      content.appendChild(wrap);
    });
    var run = document.createElement('button');
    run.textContent = 'Run';
    var cancel = document.createElement('button');
    cancel.textContent = 'Cancel';
    var dlg = overlay(title, content, [cancel, run]);
    cancel.onclick = dlg.close;
    run.onclick = function () {
      var v = {};
      fields.forEach(function (f) {
        v[f.key] = f.type === 'check' ? inputs[f.key].checked : inputs[f.key].value;
      });
      onRun(v, dlg);
    };
    return dlg;
  }

  function rawRead(dev, fe) {
    formDialog('Read ' + (dev.node_name || dev.id), [
      { key: 'whole', label: 'Read the whole device', type: 'check', value: false },
      { key: 'address', label: 'Address', value: '0x0' },
      { key: 'size', label: 'Size (bytes)', value: 256 },
      { key: 'to_dir', type: 'dirpick', label: 'Target folder (empty = download to browser)', hint: '/sdcard' },
      { key: 'to_name', label: 'Target filename', hint: 'dump.bin' },
    ], function (v, dlg) {
      var base = 'device=' + encodeURIComponent(dev.id) +
        (v.whole ? '&all=1' : '&address=' + encodeURIComponent(v.address) + '&size=' + encodeURIComponent(v.size));
      dlg.close();
      var to_path = v.to_dir ? (v.to_dir.replace(/\/+$/, '') + '/' + (v.to_name || 'dump.bin')) : '';
      if (!to_path) {
        window.location = RAW + '/read?' + base;
        return;
      }
      request('GET', RAW + '/read?' + base + '&to_path=' + encodeURIComponent(to_path), function (ok, body, status) {
        if (!ok || !body || !body.job) return rawStatus('read failed: ' + errorText(body, status));
        pollRawJob(body.job, 'reading ' + (dev.node_name || dev.id) + ' -> ' + to_path, function () { fe.RefreshFolders(true); });
      });
    });
  }

  function rawWrite(dev, fe) {
    var fields = [
      { key: 'address', label: 'Address', value: '0x0' },
      { key: 'from_path', type: 'filepick', label: 'Source file', hint: '/sdcard/fw.bin' },
    ];
    if (dev.write_needs_erase) fields.push({ key: 'erase', label: 'Erase first (' + fmtSize(dev.erase_sector) + ' sectors)', type: 'check', value: true });
    fields.push({ key: 'verify', label: 'Verify after write', type: 'check', value: true });
    fields.push({ key: 'verify_passes', label: 'Verify passes', value: 1 });
    formDialog('Write to ' + (dev.node_name || dev.id), fields, function (v, dlg) {
      if (!v.from_path) return window.alert('no file given');
      var passes = v.verify ? Math.max(1, parseInt(v.verify_passes, 10) || 1) : 0;
      var url = RAW + '/write?device=' + encodeURIComponent(dev.id) + '&address=' + encodeURIComponent(v.address) +
        '&from_path=' + encodeURIComponent(v.from_path) + '&verify=' + passes + (v.erase ? '&erase=1' : '');
      dlg.close();
      request('POST', url, function (ok, body, status) {
        if (!ok || !body || !body.job) return rawStatus('write failed: ' + errorText(body, status));
        pollRawJob(body.job, 'writing ' + v.from_path + ' -> ' + (dev.node_name || dev.id), function () { fe.RefreshFolders(true); });
      });
    });
  }

  function rawVerify(dev, fe) {
    formDialog('Verify ' + (dev.node_name || dev.id) + ' against a file', [
      { key: 'address', label: 'Address', value: '0x0' },
      { key: 'from_path', type: 'filepick', label: 'Source file', hint: '/sdcard/fw.bin' },
      { key: 'passes', label: 'Verify passes', value: 1 },
    ], function (v, dlg) {
      if (!v.from_path) return window.alert('no file given');
      var passes = Math.max(1, parseInt(v.passes, 10) || 1);
      var url = RAW + '/verify?device=' + encodeURIComponent(dev.id) + '&address=' + encodeURIComponent(v.address) +
        '&from_path=' + encodeURIComponent(v.from_path) + '&passes=' + passes;
      dlg.close();
      request('POST', url, function (ok, body, status) {
        if (!ok || !body || !body.job) return rawStatus('verify failed: ' + errorText(body, status));
        pollRawJob(body.job, 'verifying ' + (dev.node_name || dev.id) + ' against ' + v.from_path, null);
      });
    });
  }

  function rawErase(dev, fe) {
    var sect = dev.pseudo_erase ? '' : ' (multiple of ' + fmtHex(dev.erase_sector) + ')';
    var fields = [
      { key: 'address', label: 'Address' + sect, value: '0x0' },
      { key: 'size', label: 'Size (bytes)' + sect, value: dev.pseudo_erase ? 256 : dev.erase_sector },
      { key: 'all', label: 'Erase the whole device', type: 'check', value: false },
    ];
    if (dev.can_erase_chip) fields.push({ key: 'sliced', label: 'Erase sector-by-sector (skip fast chip erase)', type: 'check', value: false });
    formDialog('Erase ' + (dev.node_name || dev.id), fields, function (v, dlg) {
      if (v.all && !window.confirm('Erase all of ' + (dev.node_name || dev.id) + '? Everything on it is gone.')) return;
      var base = v.all
        ? 'device=' + encodeURIComponent(dev.id) + '&all=1'
        : 'device=' + encodeURIComponent(dev.id) + '&address=' + encodeURIComponent(v.address) + '&size=' + encodeURIComponent(v.size);
      if (v.sliced) base += '&sliced=1';
      dlg.close();
      request('POST', RAW + '/erase?' + base, function (ok, body, status) {
        if (!ok || !body || !body.job) return rawStatus('erase failed: ' + errorText(body, status));
        pollRawJob(body.job, 'erasing ' + (dev.node_name || dev.id), function () { fe.RefreshFolders(true); });
      });
    });
  }

  // Upload and download go straight to /files/* the way the simple browser does, instead of
  // through the widget's own machinery (its chunked ?dir= multipart upload and its iframe POST
  // download form), which dropped the storage mount path and tripped the node's multipart path.
  function pollFlush(job, done) {
    var iv = setInterval(function () {
      request('GET', API + '/job' + q({ id: job }), function (ok, body) {
        if (ok && body && body.state === 'done') {
          clearInterval(iv);
          done();
        }
      });
    }, 500);
  }

  // Sequential upload queue with a small progress panel. The node accepts one upload at a time,
  // so queued files run one after another, each showing its own progress. The panel is plain
  // inline-styled DOM in document.body so it does not depend on the shadow-scoped stylesheets.
  var uploadQueue = [];
  var uploadBusy = false;
  var uploadPanel = null;
  var uploadListNode = null;

  function closeUploadPanel() {
    if (uploadPanel && uploadPanel.parentNode) uploadPanel.parentNode.removeChild(uploadPanel);
    uploadPanel = null;
    uploadListNode = null;
  }

  function ensureUploadPanel() {
    if (uploadPanel) return;
    uploadPanel = document.createElement('div');
    uploadPanel.setAttribute('style',
      'position:absolute;right:12px;bottom:12px;width:300px;max-width:70%;max-height:60%;display:flex;' +
      'flex-direction:column;overflow:hidden;z-index:8;background:Canvas;color:CanvasText;' +
      'border:1px solid rgba(127,127,127,0.4);border-radius:8px;box-shadow:0 4px 16px rgba(0,0,0,0.3);' +
      'font:13px system-ui,-apple-system,sans-serif;');
    var head = document.createElement('div');
    head.setAttribute('style',
      'padding:8px 12px;font-weight:600;border-bottom:1px solid rgba(127,127,127,0.3);' +
      'display:flex;align-items:center;justify-content:space-between;');
    var headText = document.createElement('span');
    headText.textContent = 'Transfers';
    var headClose = document.createElement('button');
    headClose.textContent = '\u00d7';
    headClose.setAttribute('style', 'border:none;background:none;font-size:18px;line-height:1;cursor:pointer;color:inherit;');
    headClose.onclick = closeUploadPanel;
    head.appendChild(headText);
    head.appendChild(headClose);
    uploadListNode = document.createElement('div');
    uploadListNode.setAttribute('style', 'padding:4px 0;overflow:auto;flex:1 1 auto;');
    uploadPanel.appendChild(head);
    uploadPanel.appendChild(uploadListNode);
    (frameEl || card || document.body).appendChild(uploadPanel);
  }

  function addJobRow(label) {
    ensureUploadPanel();
    var item = {};
    var row = document.createElement('div');
    row.setAttribute('style', 'padding:6px 12px;');
    var name = document.createElement('div');
    name.setAttribute('style', 'white-space:nowrap;overflow:hidden;text-overflow:ellipsis;');
    name.textContent = label;
    var track = document.createElement('div');
    track.setAttribute('style',
      'height:6px;margin-top:4px;border-radius:3px;background:rgba(127,127,127,0.25);overflow:hidden;');
    var fill = document.createElement('div');
    fill.setAttribute('style', 'height:100%;width:0%;background:#03a9f4;transition:width 0.15s;');
    track.appendChild(fill);
    var stat = document.createElement('div');
    stat.setAttribute('style', 'margin-top:2px;font-size:11px;opacity:0.7;');
    stat.textContent = 'queued';
    row.appendChild(name);
    row.appendChild(track);
    row.appendChild(stat);
    uploadListNode.appendChild(row);
    item.fill = fill;
    item.stat = stat;
    item.row = row;
    return item;
  }

  function setUploadRow(item, pct, text, color) {
    if (item.fill) {
      item.fill.style.width = pct + '%';
      if (color) item.fill.style.background = color;
    }
    if (item.stat) item.stat.textContent = text;
  }

  // Append the file currently in flight to a job's scrolling sub-list (created on first use),
  // right under its always-visible summary row. Returns true when it was a new file.
  function fmtBytes(n) {
    if (typeof n !== 'number' || !isFinite(n)) return '';
    var u = ['B', 'KB', 'MB', 'GB'], i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i === 0 ? n : n.toFixed(1)) + ' ' + u[i];
  }

  // Append (on a new file) or live-update the current file's sub-line under a job's summary
  // row, with its byte progress. Returns true when a new file started.
  function pushSubFile(item, name, doneB, totalB) {
    if (!name) return false;
    if (!item.sub) {
      item.sub = document.createElement('div');
      item.sub.setAttribute('style',
        'max-height:88px;overflow:auto;margin:2px 0 2px 22px;font-size:11px;opacity:0.75;');
      item.row.appendChild(item.sub);
    }
    var isNew = name !== item.lastSub;
    if (isNew) {
      item.lastSub = name;
      item.curSub = document.createElement('div');
      item.curSub.setAttribute('style', 'white-space:nowrap;overflow:hidden;text-overflow:ellipsis;');
      item.sub.appendChild(item.curSub);
      item.sub.scrollTop = item.sub.scrollHeight;
    }
    var tail = '';
    if (typeof totalB === 'number' && totalB > 0) {
      tail = '  ' + fmtBytes(doneB) + ' / ' + fmtBytes(totalB) + ' (' + Math.round((100 * doneB) / totalB) + '%)';
    }
    item.curSub.textContent = name + tail;
    return isNew;
  }

  function enqueueUpload(fe, folder, file) {
    var item = { fe: fe, folder: folder, file: file };
    var row = addJobRow(file.name);
    item.fill = row.fill;
    item.stat = row.stat;
    uploadQueue.push(item);
    if (!uploadBusy) processUploadQueue();
  }

  function processUploadQueue() {
    if (!uploadQueue.length) {
      uploadBusy = false;
      // Clear the panel a few seconds after the batch is done, unless a new upload arrived.
      setTimeout(function () {
        if (!uploadBusy && !uploadQueue.length) closeUploadPanel();
      }, 4000);
      return;
    }
    uploadBusy = true;
    doUpload(uploadQueue.shift(), false, processUploadQueue);
  }

  function doUpload(item, overwrite, done) {
    var url = API + '/upload' + q({ path: childPath(item.folder, item.file.name), overwrite: overwrite ? '1' : undefined });
    var fd = new FormData();
    fd.append('file', item.file);
    var xhr = new XMLHttpRequest();
    xhr.open('POST', url, true);
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable) {
        var pct = Math.round((100 * e.loaded) / e.total);
        setUploadRow(item, pct, pct + '%');
      }
    };
    xhr.onload = function () {
      var body = null;
      try {
        body = JSON.parse(xhr.responseText);
      } catch (e) {
        body = null;
      }
      if (xhr.status === 409) {
        if (window.confirm('"' + item.file.name + '" already exists. Overwrite?')) return doUpload(item, true, done);
        setUploadRow(item, 100, 'skipped', '#9e9e9e');
        return done();
      }
      if (xhr.status < 200 || xhr.status >= 300) {
        setUploadRow(item, 100, 'failed: ' + errorText(body, xhr.status), '#e53935');
        return done();
      }
      var finish = function () {
        setUploadRow(item, 100, 'done', '#43a047');
        if (item.fe) item.fe.RefreshFolders(true);
        done();
      };
      // Staged upload: the node flushes the PSRAM buffer to storage as a background job.
      if (body && body.job) {
        setUploadRow(item, 100, 'flushing...');
        pollFlush(body.job, finish);
      } else {
        finish();
      }
    };
    xhr.onerror = function () {
      setUploadRow(item, 100, 'failed: no response from the node', '#e53935');
      done();
    };
    xhr.send(fd);
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

  function awaitJob(id, done, onprogress) {
    var tries = 0;
    (function tick() {
      request('GET', API + '/job' + q({ id: id }), function (ok, body) {
        if (!ok || !body) return done(false, 'lost track of the transfer');
        if (body.state === 'done') {
          var good = !body.result || body.result === 'OK';
          return done(good, good ? null : body.result);
        }
        if (onprogress) onprogress(body);
        if (++tries > POLL_MAX) return done(false, 'transfer timed out');
        setTimeout(tick, POLL_MS);
      });
    })();
  }

  // ---------------------------------------------------------------------------
  // File kinds
  // ---------------------------------------------------------------------------

  var IMAGE_EXT = ['.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.svg', '.ico'];
  var TEXT_EXT = (window.ESPHFE && window.ESPHFE.textFormats) || ['.txt', '.log'];
  var CHANGE_POLL_MS = (window.ESPHFE && window.ESPHFE.changePollMs) || 5000;

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
  function isLog(name) {
    return hasExt(name, ['.log']);
  }

  function entryFor(name, isDir, size, mtime, id) {
    var e = {
      id: id !== undefined ? id : name,
      name: name,
      type: isDir ? 'folder' : 'file',
      hash: String(mtime || 0),
      // Folders carry attrs.canmodify so the widget enables its modify tools (delete, rename,
      // new folder/file) inside a storage. The synthetic root stays canmodify:false via
      // initpath, so the storages themselves are not deletable. Without attrs the widget reads
      // canmodify off an undefined path segment and its tool handlers throw.
      attrs: { canmodify: true },
    };
    // Only files carry a size. Folders leave the key absent so the widget's selection-size
    // sum skips them instead of adding undefined and showing NaN.
    if (!isDir) e.size = size;
    return e;
  }

  // ---------------------------------------------------------------------------
  // Listing
  // ---------------------------------------------------------------------------

  // Medium -> icon category for the root entries. Same coarse buckets the simple browser
  // uses (sd/usb/net/mem/disk), matched by substring so bus-prefixed kinds fall in place
  // (spi_flash -> flash -> mem, onewire_eeprom -> eeprom -> mem). ftp is a network share
  // and mram is a memory IC, so they join net/mem rather than the generic disk fallback.
  function typeIcon(kind) {
    var k = String(kind || '').toLowerCase();
    if (k.indexOf('sd') !== -1) return 'sd';
    if (k.indexOf('usb') !== -1) return 'usb';
    if (k.indexOf('nfs') !== -1 || k.indexOf('ftp') !== -1 || k.indexOf('net') !== -1) return 'net';
    if (k.indexOf('flash') !== -1 || k.indexOf('eeprom') !== -1 || k.indexOf('fram') !== -1 ||
        k.indexOf('mram') !== -1 || k.indexOf('littlefs') !== -1 || k.indexOf('part') !== -1)
      return 'mem';
    return 'disk';
  }

  // entry id -> icon category, rebuilt on every root refresh. setRootTypeStyles() turns this
  // into per-entry style rules (keyed by data-feid/data-itemid) before the widget renders, so
  // the type glyph is correct from the first paint rather than swapped in afterwards.
  var rootKindById = {};
  // Whether each root entry's icon should read as filled: mountable storages when mounted,
  // non-mountable storages (and device nodes) whenever they are present/loaded.
  var rootFilledById = {};

  // Reflect the current storage's type on the card so the breadcrumb folder icon can match it
  // (the storages carry it; device nodes are never navigated into). Cleared at the root.
  function setCurStorage(path) {
    if (!card) return;
    ['sd', 'usb', 'net', 'mem', 'disk'].forEach(function (x) {
      card.classList.remove('esph-cur-' + x);
    });
    if (!path) return;
    for (var k in rootKindById) {
      if (k.indexOf('dev:') === 0) continue;
      if (path === k || path.indexOf(k + '/') === 0) {
        card.classList.add('esph-cur-' + rootKindById[k]);
        return;
      }
    }
  }

  // Emit one style rule per root entry, keyed by its data-feid/data-itemid, so the widget
  // renders the correct type glyph from the first paint. No post-render DOM change, no
  // folder-then-type flash. Rules reference the per-type mask/colour custom properties in
  // adapter.css. Must be called before the entries are handed to the widget.
  function setRootTypeStyles() {
    if (!card) return;
    var css = '';
    for (var id in rootKindById) {
      var cat = rootKindById[id];
      var q = '"' + String(id).replace(/[\\"]/g, '\\$&') + '"';
      css += '#esp-file-explorer [data-feid=' + q + '] .fe_fileexplorer_item_icon::before,\n' +
             '#esp-file-explorer [data-itemid=' + q + '] .fe_fileexplorer_item_icon::before {\n' +
             '  mask-image: var(--fe-t-' + cat + ');\n' +
             '  -webkit-mask-image: var(--fe-t-' + cat + ');\n' +
             '  background-color: var(--fe-c-' + cat + ');\n' +
             '  opacity: ' + (rootFilledById[id] === false ? '0.4' : '1') + ';\n}\n';
    }
    var st = card.querySelector('#esph-fe-typestyles');
    if (!st) {
      st = document.createElement('style');
      st.id = 'esph-fe-typestyles';
      card.appendChild(st);
    }
    st.textContent = css;
  }

  function refreshRoot(cb) {
    request('GET', API + '/storages', function (ok, body, status) {
      if (!ok || !body || !body.storages) return cb(errorText(body, status));
      var entries = [];
      rootKindById = {};
      rootFilledById = {};
      for (var i = 0; i < body.storages.length; i++) {
        var s = body.storages[i];
        var e = entryFor(s.mounted ? s.mount_path : s.mount_path + '  (not mounted)', true, 0, 0, s.mount_path);
        e.esphMounted = s.mounted;
        e.esphCanMount = !!s.can_mount;
        e.esphCanUnmount = !!s.can_unmount;
        e.esphCanFormat = !!s.can_format;
        var cat = typeIcon(s.kind || s.type);
        e.esphType = cat;
        rootKindById[s.mount_path] = cat;
        // Filled = not currently unmounted-and-mountable. can_mount is true exactly when a
        // storage can still be mounted (i.e. it is unmounted); mounted ones and non-mountable
        // loaded ones report can_mount false. More reliable than the drivers' is_mounted flag.
        rootFilledById[s.mount_path] = !s.can_mount;
        entries.push(e);
      }
      // Raw device nodes, if this build has the raw API. A 404 just means it is not
      // configured -- carry on with the storages alone.
      request('GET', RAW + '/devices', function (dok, devs) {
        if (dok && devs && devs.length) {
          for (var j = 0; j < devs.length; j++) {
            var d = devs[j];
            if (!d.node) continue;
            var de = entryFor(d.node_name || d.id, false, d.capacity, 0, 'dev:' + d.id);
            de.esphDevice = d;
            de.attrs = { canmodify: false };
            rootKindById['dev:' + d.id] = 'devnode';
            rootFilledById['dev:' + d.id] = true;
            entries.push(de);
          }
        }
        setRootTypeStyles();
        cb(null, entries);
      });
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

  function overlay(title, contentElem, footerElems, fill) {
    var back = document.createElement('div');
    back.className = 'esph-fe-overlay';
    var box = document.createElement('div');
    box.className = fill ? 'esph-fe-dialog esph-fe-dialog--fill' : 'esph-fe-dialog';

    var close = document.createElement('button');
    close.className = 'esph-fe-close';
    close.textContent = '\u00d7';

    var foot = document.createElement('div');
    foot.className = 'esph-fe-dialog-foot';
    (footerElems || []).forEach(function (el) {
      foot.appendChild(el);
    });

    var head = document.createElement('div');
    head.className = 'esph-fe-dialog-head';
    var label = document.createElement('span');
    label.textContent = title;
    head.appendChild(label);
    head.appendChild(close);
    box.appendChild(head);
    box.appendChild(contentElem);
    box.appendChild(foot);
    back.appendChild(box);
    // Into the card, not document.body: the card sits in the v3 app's shadow root, and a
    // dialog parked outside it would get none of this sheet's rules.
    (frameEl || card || document.body).appendChild(back);

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
    overlay(name, img, [], true);
  }

  // Deliberately a plain textarea. This edits config and log files on a microcontroller; a
  // highlighting editor would cost more flash than everything else here put together.
  function showEditor(path, name, writable) {
    var originalContent = null;
    var area = document.createElement('textarea');
    area.className = 'esph-fe-editor';
    area.readOnly = true;
    area.value = 'loading\u2026';

    var status = document.createElement('span');
    status.className = 'esph-fe-status';
    var save = document.createElement('button');
    save.textContent = 'Save';
    save.disabled = true;

    overlay('Editing ' + name, area, writable ? [status, save] : [status], true);

    var xhr = new XMLHttpRequest();
    xhr.open('GET', API + '/download' + q({ path: path, inline: '1' }), true);
    xhr.onload = function () {
      if (xhr.status >= 200 && xhr.status < 300) {
        area.value = xhr.responseText;
        originalContent = xhr.responseText;
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

    function writeFile() {
      status.textContent = 'saving\u2026';
      var slash = path.lastIndexOf('/');
      var base = slash >= 0 ? path.slice(slash + 1) : path;
      var form = new FormData();
      form.append('file', new Blob([area.value], { type: 'text/plain' }), base);
      var up = new XMLHttpRequest();
      up.open('POST', API + '/upload' + q({ path: path, overwrite: '1' }), true);
      up.onload = function () {
        if (up.status < 200 || up.status >= 300) {
          status.textContent = 'save failed (HTTP ' + up.status + ')';
          save.disabled = false;
          return;
        }
        var body = null;
        try { body = JSON.parse(up.responseText); } catch (e) { body = null; }
        if (body && body.job !== undefined) {
          awaitJob(body.job, function (good, msg) {
            status.textContent = good ? 'saved' : 'save failed: ' + (msg || 'error');
            if (good) originalContent = area.value;
            save.disabled = false;
          });
        } else {
          originalContent = area.value;
          status.textContent = 'saved';
          save.disabled = false;
        }
      };
      up.onerror = function () {
        status.textContent = 'save failed';
        save.disabled = false;
      };
      up.send(form);
    }

    save.onclick = function () {
      save.disabled = true;
      status.textContent = 'checking\u2026';
      // Optimistic concurrency: re-read the file and refuse to clobber it silently if it changed
      // on disk since it was opened (e.g. a log still being written).
      var chk = new XMLHttpRequest();
      chk.open('GET', API + '/download' + q({ path: path, inline: '1' }), true);
      chk.onload = function () {
        if (chk.status >= 200 && chk.status < 300 && originalContent !== null &&
            chk.responseText !== originalContent &&
            !window.confirm('"' + name + '" changed on disk since you opened it. Overwrite it with your version?')) {
          status.textContent = 'not saved -- the file changed on disk';
          save.disabled = false;
          return;
        }
        writeFile();
      };
      chk.onerror = writeFile;
      chk.send();
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

    var dlg = overlay('Monitoring ' + name, pre, [status, toggle], true);

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

  // Auto-refresh: poll /files/changes and re-list the current folder when it (or the roots
  // level, while at the root) is reported changed -- including changes from other clients or
  // plain API calls, matching the simple browser.
  function startChangePoll(fe) {
    var cursor = 0;
    setInterval(function () {
      if (document.hidden) return;
      request('GET', API + '/changes' + q({ since: cursor }), function (ok, body) {
        if (!ok || !body) return;
        cursor = body.seq || cursor;
        var cur = pathOf(fe.GetCurrentFolder());
        var dirs = body.dirs || [];
        if (body.reset || (cur === '' && dirs.indexOf('') !== -1) || dirs.indexOf(cur) !== -1) {
          fe.RefreshFolders(true);
        }
      });
    }, CHANGE_POLL_MS);
  }

  function build(parent, access) {
    var canWrite = !!access.write;
    var canRead = !!access.read;
    var canMount = !!access.mount;
    var canUnmount = !!access.unmount;
    var canFormat = !!access.format;

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
        var row = addJobRow((kind === 'copy' ? 'Copying ' : 'Moving ') + id);
        setUploadRow(row, 30, 'working...');
        request('POST', API + '/' + kind + q({ from: from, to: to }), function (ok, body, status) {
          function finish(good, msg, entry) {
            setUploadRow(row, 100, good ? 'done' : ('failed: ' + (msg || 'error')), good ? '#4caf50' : '#e53935');
            if (!good && failure === null) failure = msg;
            if (--pending === 0) {
              done(failure === null ? true : failure, moved);
              setTimeout(function () { if (!uploadBusy && !uploadQueue.length) closeUploadPanel(); }, 4000);
            }
          }
          if (!ok || !body) return finish(false, errorText(body, status));
          if (body.job === undefined) return finish(true, null);
          var nfiles = 0;
          awaitJob(body.job, function (jok, msg) {
            finish(jok, msg);
          }, function (b) {
            if (pushSubFile(row, b.file, b.file_done, b.file_total)) nfiles++;
            var pct = b.file_total ? Math.round((100 * b.file_done) / b.file_total) : 30;
            setUploadRow(row, pct, nfiles + (nfiles === 1 ? ' file' : ' files') + (b.file ? ' - ' + b.file : ''));
          });
        });
      });
    }

    var opts = {
      group: 'esphome',
      initpath: [['', '/', { canmodify: false }]],
      // Capture browser back/forward (incl. mouse side buttons) and navigate the explorer
      // instead of the page, rather than letting them be swallowed with no effect.
      capturebrowser: true,

      onrefresh: function (folder) {
        folder.SetBusyRef(1);
        refreshFolder(folder, function (err, entries) {
          folder.SetBusyRef(-1);
          if (err) return folder.SetEntries([], { error: err });
          folder.SetEntries(entries);
          // The widget renders the item icons itself; paint our type icons once those
          // nodes exist. Only the root carries storages/devices -- everything below is
          // ordinary files and folders that keep the widget's own icons.
          var curPath = pathOf(folder);
          setCurStorage(curPath);
          updateStorageTools();
        });
      },

      onopenfile: function (folder, entry) {
        if (entry.type === 'folder') return;
        if (entry.esphDevice) return rawRead(entry.esphDevice, fe);
        var path = childPath(folder, entry.name);
        if (isImage(entry.name)) return showImage(path, entry.name);
        if (isLog(entry.name)) return showTail(path, entry.name);
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
        // create=1 tells the node this is a new, possibly empty file, so an empty upload body
        // still creates it (see web_server_idf multipart handling).
        xhr.open('POST', API + '/upload' + q({ path: childPath(folder, name), dir: pathOf(folder), create: '1' }), true);
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
        var what = entries.length === 1 ? '"' + entries[0].name + '"' : entries.length + ' items';
        if (!window.confirm('Delete ' + what + '? This cannot be undone.')) return deleted(false);
        entries.forEach(function (entry) {
          // recursive only for directories; on a file remove_recursive() would list_dir() a
          // non-directory and fail NOT_FOUND (the simple browser sends it the same way).
          var recursive = entry.type === 'folder' ? '1' : undefined;
          var url = API + '/delete' + q({ path: childPath(folder, entry.name), recursive: recursive });
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
      opts.istextfile = isText;
      opts.onedit = function (entry) {
        showEditor(childPath(fe.GetCurrentFolder(), entry.name), entry.name, canWrite);
      };
      opts.onmonitor = function (entry) {
        showTail(childPath(fe.GetCurrentFolder(), entry.name), entry.name);
      };
      opts.onproperties = function (entry) {
        showProperties(entry);
      };

      opts.oninitupload = function (startupload, fileinfo) {
        // The widget calls this once per selected file. Queue it (the node uploads one at a
        // time) with its own progress, and cancel the widget's own chunked upload.
        enqueueUpload(fe, fileinfo.folder, fileinfo.file);
        startupload(false);
      };
    }

    if (canRead) {
      opts.oninitdownload = function (startdownload, folder, ids, entries) {
        if (entries.length !== 1 || entries[0].type === 'folder') {
          // No archive endpoint on the node, so a multi-selection cannot become one file.
          return startdownload('select a single file to download');
        }
        // Download the simple browser's way: a plain GET link to /files/download. Do not call
        // startdownload() -- that builds the widget's iframe POST form, which dropped the query
        // (the storage mount path) and aborted the transfer.
        var a = document.createElement('a');
        a.href = API + '/download' + q({ path: childPath(folder, entries[0].name) });
        a.download = entries[0].name;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
      };
    }

    var fe = new FileExplorer(parent, opts);
    feInstance = fe;

    // Per-storage mount/unmount: a toolbar button added only when the node permits the
    // operation at all (server still enforces it with 403). updateStorageTools() then shows
    // it only while a single root storage that supports the operation is selected, and it
    // acts on that storage's mount path.
    function makeStorageTool(cls, title, endpoint, confirmMsg) {
      var btn = fe.AddToolbarButton(cls, title);
      btn.classList.add('fe_fileexplorer_hidden');
      btn.addEventListener('click', function () {
        if (btn.classList.contains('fe_fileexplorer_disabled')) return;
        var sel = fe.GetSelectedFolderEntries();
        if (sel.length !== 1) return;
        var path = sel[0].id;
        if (confirmMsg && !window.confirm(confirmMsg.replace('%s', path))) return;
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
      // The widget fires selection_changed from inside its constructor (SetPath), before
      // `fe` is assigned here; skip that call -- the initial state is nothing selected.
      if (!fe) return;
      var atRoot = pathOf(fe.GetCurrentFolder()) === '';
      var sel = fe.GetNumSelectedItems() === 1 ? fe.GetSelectedFolderEntries() : [];
      var e = sel.length === 1 ? sel[0] : null;
      toggleTool(mountTool, !!(atRoot && e && e.esphCanMount && !e.esphMounted));
      toggleTool(unmountTool, !!(atRoot && e && e.esphCanUnmount && e.esphMounted));
      toggleTool(formatTool, !!(atRoot && e && e.esphCanFormat && !e.esphMounted));
      var dev = e && e.esphDevice ? e.esphDevice : null;
      if (card) {
        var fileOps = card.querySelectorAll('[class*="fe_fileexplorer_folder_tool_"]');
        for (var i = 0; i < fileOps.length; i++)
          // At the root there are only storages and device nodes -- file operations never
          // apply, so drop them and leave only the contextual mount/format or raw tools.
          fileOps[i].classList.toggle('fe_fileexplorer_hidden', atRoot);
      }
      toggleTool(readTool, !!dev);
      toggleTool(writeTool, !!(dev && dev.writable));
      toggleTool(verifyTool, !!(dev && dev.writable));
      toggleTool(eraseTool, !!(dev && dev.erasable));
      // Monitor (tail) is a per-file action: shown while a single text file (per
      // text_file_formats) is selected, in any folder.
      toggleTool(monitorTool, !!(e && e.type !== 'folder' && isText(e.name)));
      toggleTool(editTool, !!(e && e.type !== 'folder' && isText(e.name)));
      toggleTool(infoTool, !!e);
      fe.ToolStateUpdated();
    }

    var mountTool = canMount ? makeStorageTool('esph-fe-tool-mount', 'Mount storage', '/mount') : null;
    var unmountTool = canUnmount ? makeStorageTool('esph-fe-tool-unmount', 'Unmount storage', '/unmount') : null;
    var formatTool = canFormat
      ? makeStorageTool('esph-fe-tool-format', 'Format storage', '/format', 'Format %s? Everything on it will be lost.')
      : null;

    // Raw device actions open a dialog rather than firing a single POST.
    function makeRawTool(cls, title, fn) {
      var btn = fe.AddToolbarButton(cls, title);
      btn.classList.add('fe_fileexplorer_hidden');
      btn.addEventListener('click', function () {
        if (btn.classList.contains('fe_fileexplorer_disabled')) return;
        var sel = fe.GetSelectedFolderEntries();
        if (sel.length === 1 && sel[0].esphDevice) fn(sel[0].esphDevice, fe);
      });
      return btn;
    }
    var readTool = makeRawTool('esph-fe-tool-read', 'Read device', rawRead);
    var writeTool = makeRawTool('esph-fe-tool-write', 'Write device', rawWrite);
    var verifyTool = makeRawTool('esph-fe-tool-verify', 'Verify device', rawVerify);
    var eraseTool = makeRawTool('esph-fe-tool-erase', 'Erase device', rawErase);

    function showProperties(entry) {
      var isDir = entry.type === 'folder';
      var path = childPath(fe.GetCurrentFolder(), entry.name);
      var box = document.createElement('div');
      box.className = 'esph-fe-props';
      function row(k) {
        var r = document.createElement('div');
        r.className = 'esph-fe-prop';
        var kk = document.createElement('span');
        kk.className = 'esph-fe-prop-k';
        kk.textContent = k;
        var vv = document.createElement('span');
        vv.className = 'esph-fe-prop-v';
        r.appendChild(kk);
        r.appendChild(vv);
        box.appendChild(r);
        return vv;
      }
      row('Name').textContent = entry.name;
      row('Type').textContent = isDir ? 'Folder' : 'File';
      row('Path').textContent = path;
      if (!isDir) row('Size').textContent = fmtBytes(entry.size) + ' (' + (entry.size || 0) + ' bytes)';
      var dlg = overlay('Properties: ' + entry.name, box, [], false);
      if (isDir) {
        var v = row('Total size');
        v.textContent = 'computing\u2026';
        var queue = [path], total = 0, files = 0;
        function update(done) {
          v.textContent = fmtBytes(total) + ' (' + total + ' bytes, ' + files + ' files)' + (done ? '' : ' \u2026');
        }
        // Serial walk (one listing at a time) so we do not flood the node; stops if closed.
        function step() {
          if (dlg.isClosed()) return;
          if (!queue.length) return update(true);
          var p = queue.shift();
          request('GET', API + '/list' + q({ path: p }), function (ok, body) {
            if (dlg.isClosed()) return;
            if (ok && body && body.entries) {
              body.entries.forEach(function (it) {
                if (it.is_dir) queue.push(p === '' ? it.name : p + '/' + it.name);
                else { total += it.size || 0; files++; }
              });
              update(false);
            }
            step();
          });
        }
        step();
      }
    }

    var infoTool = fe.AddToolbarButton('esph-fe-tool-info', 'Properties');
    infoTool.classList.add('fe_fileexplorer_hidden');
    infoTool.addEventListener('click', function () {
      var sel = fe.GetSelectedFolderEntries();
      if (sel.length !== 1) return;
      showProperties(sel[0]);
    });
    var monitorTool = fe.AddToolbarButton('esph-fe-tool-monitor', 'Monitor');
    monitorTool.classList.add('fe_fileexplorer_hidden');
    monitorTool.addEventListener('click', function () {
      var sel = fe.GetSelectedFolderEntries();
      if (sel.length !== 1 || sel[0].type === 'folder') return;
      showTail(childPath(fe.GetCurrentFolder(), sel[0].name), sel[0].name);
    });
    var editTool = fe.AddToolbarButton('esph-fe-tool-edit', 'Edit');
    editTool.classList.add('fe_fileexplorer_hidden');
    editTool.addEventListener('click', function () {
      var sel = fe.GetSelectedFolderEntries();
      if (sel.length !== 1 || sel[0].type === 'folder') return;
      showEditor(childPath(fe.GetCurrentFolder(), sel[0].name), sel[0].name, canWrite);
    });
    if (CHANGE_POLL_MS > 0) startChangePoll(fe);
    return fe;
  }

  // ---------------------------------------------------------------------------
  // Card and mounting
  // ---------------------------------------------------------------------------

  // Built in start(), read by overlay() — the dialogs go inside it.
  var card = null;
  var frameEl = null;
  var feInstance = null;

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

    // A [ Log | Files ] tab bar. Only one panel is visible at a time, so the growing log can
    // be hidden outright and whichever panel is active gets the full column height.
    var tabbar = document.createElement('div');
    tabbar.className = 'esph-fe-tabbar';
    var logTab = document.createElement('div');
    logTab.className = 'esph-fe-tab';
    logTab.textContent = 'Debug Log';
    var filesTab = document.createElement('div');
    filesTab.className = 'esph-fe-tab';
    filesTab.textContent = 'File-Explorer';
    // Double-click either tab widens the log column, exactly like the log's own tab header:
    // the v3 app listens for this event and toggles the column to full width.
    function widen() {
      tabbar.dispatchEvent(new CustomEvent('log-tab-header-double-clicked', { bubbles: true, composed: true }));
    }
    logTab.addEventListener('dblclick', widen);
    filesTab.addEventListener('dblclick', widen);
    tabbar.appendChild(logTab);
    tabbar.appendChild(filesTab);

    var frame = document.createElement('div');
    frame.className = 'esph-fe-frame';
    frame.style.display = 'none';  // Log is the default panel

    // The widget's own container. Kept as a separate element so file-explorer.js owns its
    // subtree outright and the frame's padding is not something it has to reason about.
    var host = document.createElement('div');
    host.id = 'file-explorer';
    frame.appendChild(host);

    el.appendChild(tabbar);
    el.appendChild(frame);
    return { card: el, host: host, logTab: logTab, filesTab: filesTab, frame: frame };
  }

  // Places the card directly below the entity table — above the log, which grows. The v3 app
  // renders into an open shadow root; if it has not rendered yet, retry once shortly after,
  // and fall back to appending next to <esp-app> so the browser is reachable either way.
  function attach(el, tabs) {
    var app = document.querySelector('esp-app');

    function mount() {
      // Sit in the log column, above the log, and switch between the two via the tab bar so
      // the growing log never pushes the browser down -- only one panel is visible at a time.
      // Fall back to the entity table, then to appending after the app, so it stays reachable.
      var root = app && app.shadowRoot;
      var log = root && root.querySelector('esp-log');
      var anchor = log || (root && root.querySelector('esp-entity-table'));
      if (!anchor || !anchor.parentNode) return false;
      anchor.parentNode.insertBefore(el, anchor);
      wireTabs(tabs, log);
      return true;
    }

    if (!mount()) {
      setTimeout(function () {
        if (!mount()) {
          (app ? app.parentNode : document.body).appendChild(el);
          wireTabs(tabs, null);
        }
      }, 300);
    }
  }

  // Hide exactly the log's own "Debug Log" header -- the .tab-header inside esp-log's own shadow
  // root -- with an inline style. Inline beats the component's own .tab-header rule, and scoping
  // to the log's shadow root leaves the entity table's same-named header (a separate shadow root)
  // untouched. The log renders it asynchronously, so retry a few frames.
  function hideLogHeader(log, tries) {
    var hdr = log.shadowRoot && log.shadowRoot.querySelector('.tab-header');
    if (hdr) {
      hdr.style.display = 'none';
      return;
    }
    if ((tries || 0) < 20) {
      requestAnimationFrame(function () {
        hideLogHeader(log, (tries || 0) + 1);
      });
    }
  }

  // Switch between the log and the browser. Files hides esp-log entirely (so its growth cannot
  // push anything), Log hides the browser frame. With no log present the browser is always on.
  function wireTabs(tabs, log) {
    if (!log) {
      tabs.frame.style.display = '';
      tabs.logTab.style.display = 'none';
      tabs.filesTab.classList.add('esph-fe-tab--active');
      return;
    }
    hideLogHeader(log, 0);
    function select(files) {
      tabs.frame.style.display = files ? '' : 'none';
      log.style.display = files ? 'none' : '';
      tabs.filesTab.classList.toggle('esph-fe-tab--active', files);
      tabs.logTab.classList.toggle('esph-fe-tab--active', !files);
      // The widget may have first rendered while hidden; refresh on show so the listing is
      // current and laid out against the now-visible frame.
      if (files && feInstance) feInstance.RefreshFolders(true);
    }
    tabs.logTab.addEventListener('click', function () {
      select(false);
    });
    tabs.filesTab.addEventListener('click', function () {
      select(true);
    });
    select(false);  // default: Log
  }

  // ---------------------------------------------------------------------------
  // Entry point
  // ---------------------------------------------------------------------------

  function start() {
    var built = buildCard();
    card = built.card;
    frameEl = built.frame;
    attach(card, built);
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
