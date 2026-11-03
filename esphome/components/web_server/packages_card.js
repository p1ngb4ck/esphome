(function () {
  'use strict';

  var API = '/api/packages/';
  var tab = 'installed';
  var installed = null;
  var available = null;
  var busyTimer = null;
  var view, errorLine, nameInput, fileInput;

  function call(method, op, params, cb) {
    var parts = [];
    for (var k in params || {}) {
      if (params[k] !== undefined && params[k] !== null) parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(params[k]));
    }
    var xhr = new XMLHttpRequest();
    xhr.open(method, API + op + (parts.length ? '?' + parts.join('&') : ''), true);
    xhr.onload = function () {
      var body = null;
      try {
        body = JSON.parse(xhr.responseText);
      } catch (e) {
        body = null;
      }
      cb(xhr.status >= 200 && xhr.status < 300 && body !== null, body, xhr.status);
    };
    xhr.onerror = function () {
      cb(false, null, 0);
    };
    xhr.send();
  }

  function el(tag, css, text) {
    var e = document.createElement(tag);
    if (css) e.style.cssText = css;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function kb(n) {
    n = Number(n) || 0;
    return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.round(n / 1024) + ' KB';
  }

  function showError(text) {
    errorLine.textContent = text || '';
  }

  var BTN = 'padding:3px 8px;border-radius:5px;border:0;background:rgba(127,127,127,.25);color:inherit;cursor:pointer;margin-left:4px';
  var BTN_MAIN = 'padding:3px 8px;border-radius:5px;border:0;background:#44739e;color:#fff;cursor:pointer;margin-left:4px';

  function button(text, main, action) {
    var b = el('button', main ? BTN_MAIN : BTN, text);
    b.addEventListener('click', action);
    return b;
  }

  function act(op, params) {
    call('POST', op, params, function (ok, body, status) {
      if (!ok || !body.success) {
        showError(body && body.error ? body.error : 'no answer from the device (HTTP ' + status + ')');
      } else {
        showError('');
      }
      load();
    });
  }

  function table(head) {
    var t = el('table', 'width:100%;border-collapse:collapse;font-size:13px');
    var tr = el('tr', 'opacity:.7;text-align:left');
    head.forEach(function (h, i) {
      tr.appendChild(el('th', i === 0 ? 'padding:6px' : '', h));
    });
    t.appendChild(tr);
    return t;
  }

  function row(t, cells, actions) {
    var tr = el('tr', 'border-top:1px solid rgba(127,127,127,.3)');
    cells.forEach(function (c, i) {
      var td = el('td', i === 0 ? 'padding:6px' : '');
      if (c instanceof Node) td.appendChild(c);
      else td.textContent = c;
      tr.appendChild(td);
    });
    var td = el('td', 'text-align:right;white-space:nowrap');
    actions.forEach(function (a) {
      td.appendChild(a);
    });
    tr.appendChild(td);
    t.appendChild(tr);
  }

  function renderInstalled() {
    var t = table(['Package', 'Version', 'Flash', 'PSRAM', 'State']);
    installed.packages.forEach(function (p) {
      var state = installed.installing === p.name ? 'installing' : p.running ? 'running' : p.enabled ? 'enabled' : 'disabled';
      var color = state === 'running' ? '#8bc34a' : state === 'disabled' ? '#9e9e9e' : '#03a9f4';
      var actions = [];
      actions.push(button(p.hold ? 'Unhold' : 'Hold', false, function () {
        act(p.hold ? 'unhold' : 'hold', { name: p.name });
      }));
      actions.push(button(p.enabled ? 'Disable' : 'Enable', false, function () {
        act(p.enabled ? 'disable' : 'enable', { name: p.name });
      }));
      actions.push(button('Remove', false, function () {
        if (confirm('Remove ' + p.name + '?')) act('remove', { name: p.name });
      }));
      row(t, [p.name, p.version, kb(p.flash), kb(p.psram), el('span', 'color:' + color, state)], actions);
    });
    view.appendChild(t);
    var drop = el('div', 'margin-top:10px;border:1px dashed rgba(127,127,127,.6);border-radius:6px;padding:14px;text-align:center;opacity:.8', 'Drop an .ipk here: uploaded to the packages folder, then installed');
    drop.addEventListener('dragover', function (e) {
      e.preventDefault();
    });
    drop.addEventListener('drop', function (e) {
      e.preventDefault();
      if (e.dataTransfer.files.length) upload(e.dataTransfer.files[0]);
    });
    view.appendChild(drop);
  }

  function renderAvailable() {
    var bar = el('div', 'margin-bottom:6px');
    bar.appendChild(button('Refresh', true, function () {
      act('refresh', {});
    }));
    view.appendChild(bar);
    if (!available) {
      view.appendChild(el('div', 'opacity:.7;padding:6px', 'No repository index loaded.'));
      return;
    }
    var t = table(['Package', 'Version', 'Repository', 'Size', 'Description']);
    available.packages.forEach(function (p) {
      var actions = [];
      if (!p.installed) actions.push(button('Install', true, function () {
        act('install', { name: p.name });
      }));
      else actions.push(el('span', 'opacity:.7', 'Installed'));
      row(t, [p.name, p.version, p.repository, kb(p.size), p.description], actions);
    });
    view.appendChild(t);
  }

  function renderRepositories() {
    var t = table(['Repository', 'URL', 'Packages', 'State']);
    (available ? available.repositories : []).forEach(function (r) {
      var actions = [button(r.enabled ? 'Disable' : 'Enable', false, function () {
        act('repo_enable', { name: r.name, enabled: r.enabled ? '0' : '1' });
      })];
      if (!r.builtin) actions.push(button('Delete', false, function () {
        if (confirm('Delete ' + r.name + '?')) act('repo_delete', { name: r.name });
      }));
      row(t, [r.name + (r.builtin ? ' (config)' : ''), r.url, String(r.count), r.state], actions);
    });
    view.appendChild(t);
    var add = el('div', 'margin-top:8px;display:flex;gap:6px');
    var n = el('input', 'flex:0 0 140px');
    n.placeholder = 'Name';
    var u = el('input', 'flex:1');
    u.placeholder = 'https://...';
    var k = el('input', 'flex:1');
    k.placeholder = 'Public key (usign)';
    add.appendChild(n);
    add.appendChild(u);
    add.appendChild(k);
    add.appendChild(button('Add', true, function () {
      act('repo_add', { name: n.value, url: u.value, public_key: k.value });
    }));
    view.appendChild(add);
  }

  function render() {
    view.textContent = '';
    if (!installed) return;
    if (tab === 'installed') renderInstalled();
    else if (tab === 'available') renderAvailable();
    else renderRepositories();
  }

  function load() {
    call('GET', 'list', null, function (ok, body) {
      if (!ok) return;
      installed = body;
      call('GET', 'available', null, function (ok2, body2) {
        available = ok2 ? body2 : null;
        render();
        var busy = installed.installing || (available && available.refreshing);
        if (busy && !busyTimer) {
          busyTimer = setTimeout(function () {
            busyTimer = null;
            load();
          }, 1000);
        }
      });
    });
  }

  function packageName(file) {
    var base = file.replace(/\.ipk$/i, '');
    var m = /^(.+?)_[^_]+_[^_]+$/.exec(base);
    return (m ? m[1] : base).replace(/-/g, '_');
  }

  function upload(file) {
    var name = nameInput.value || packageName(file.name);
    var dir = installed && installed.directory;
    if (!dir) {
      showError('No packages folder configured on the device');
      return;
    }
    var path = dir + '/' + name + '.ipk';
    var form = new FormData();
    form.append('file', file, name + '.ipk');
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/files/upload?path=' + encodeURIComponent(path) + '&overwrite=1', true);
    xhr.onload = function () {
      if (xhr.status < 200 || xhr.status >= 300) {
        showError('Upload failed (HTTP ' + xhr.status + ')');
        return;
      }
      act('install', { name: name, source: path });
    };
    xhr.onerror = function () {
      showError('Upload failed');
    };
    showError('Uploading ' + file.name + ' ...');
    xhr.send(form);
  }

  function build() {
    var card = el('div', 'margin:1em 0;background:rgba(127,127,127,.08);border-radius:12px;padding:10px 12px');
    card.id = 'esp-packages';
    var head = el('div', 'display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap');
    head.appendChild(el('b', 'font-size:16px', 'Packages'));
    var tabs = {};
    [['installed', 'Installed'], ['available', 'Available'], ['repositories', 'Repositories']].forEach(function (t, i) {
      var s = el('span', 'padding:4px 10px;border-radius:6px;cursor:pointer;' + (i === 0 ? 'margin-left:12px' : ''), t[1]);
      s.addEventListener('click', function () {
        tab = t[0];
        paint();
        render();
      });
      tabs[t[0]] = s;
      head.appendChild(s);
    });
    function paint() {
      for (var k in tabs) tabs[k].style.background = k === tab ? '#44739e' : 'rgba(127,127,127,.25)';
      for (var j in tabs) tabs[j].style.color = j === tab ? '#fff' : 'inherit';
    }
    paint();
    nameInput = el('input', 'margin-left:auto;width:120px');
    nameInput.placeholder = 'package name';
    fileInput = el('input');
    fileInput.type = 'file';
    fileInput.accept = '.ipk';
    fileInput.style.display = 'none';
    fileInput.addEventListener('change', function () {
      if (fileInput.files.length) {
        if (!nameInput.value) nameInput.value = packageName(fileInput.files[0].name);
        upload(fileInput.files[0]);
        fileInput.value = '';
      }
    });
    head.appendChild(nameInput);
    head.appendChild(fileInput);
    head.appendChild(button('Upload .ipk', false, function () {
      fileInput.click();
    }));
    card.appendChild(head);
    view = el('div');
    card.appendChild(view);
    errorLine = el('div', 'margin-top:8px;color:#e57373;font-size:12px');
    card.appendChild(errorLine);
    return card;
  }

  function attach(card) {
    var app = document.querySelector('esp-app');
    function mount() {
      var root = app && app.shadowRoot;
      var anchor = root && root.querySelector('esp-entity-table');
      if (!anchor || !anchor.parentNode) return false;
      anchor.parentNode.insertBefore(card, anchor);
      return true;
    }
    if (!mount()) {
      setTimeout(function () {
        if (!mount()) (app ? app.parentNode : document.body).appendChild(card);
      }, 300);
    }
  }

  function start() {
    attach(build());
    load();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
