//---------------------------------------------------------------------------
// -replweb の埋め込みビューワー (単一 HTML)
//
// ReplWebServer.cpp の匿名 namespace から #include される。 タブ構成:
//   Console : /events の SSE + POST /cmd (従来のページ)
//   Watch   : GET|POST /watch + GET /sub/watch (監視式)
//   Pad     : POST /pad/exec + GET|POST /pad/file (スクリプトエディタ)
// 上のバーには吉里吉里2 の「コントローラ」相当 (イベント停止 / 終了) を置く
//   (GET|POST /state + GET /sub/state)。
// 設計は doc/DebugToolsRevival.md。
//
// **本体埋め込みを基本とする** (-replweb だけで完結し、 プラグインもスクリプトも
// 要らない)。 案件が自前 UI に差し替えたい場合は WebServer.serveStatic を使う。
//---------------------------------------------------------------------------
const char* kHtmlPage = R"HTMLPAGE(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kirikiri Z REPL</title>
<style>
  :root { color-scheme: dark; }
  html,body { margin:0; height:100%; background:#1e1e1e; color:#d4d4d4;
    font-family: Consolas, "Cascadia Mono", "MS Gothic", monospace; font-size:14px; }
  #app { display:flex; flex-direction:column; height:100vh; }
  #top { padding:4px 8px; background:#252526; border-bottom:1px solid #333;
    display:flex; gap:8px; align-items:center; }
  #top b { color:#4ec9b0; white-space:nowrap; }
  #tabs { display:flex; gap:2px; }
  .tab { background:#2d2d2d; color:#a0a0a0; border:0; border-bottom:2px solid transparent;
    padding:4px 14px; cursor:pointer; font:inherit; }
  .tab:hover { background:#37373d; color:#d4d4d4; }
  .tab.active { background:#1e1e1e; color:#d4d4d4; border-bottom-color:#0e639c; }
  #ctl { display:flex; gap:6px; align-items:center; margin-left:auto; }
  #cEvent { min-width:9.5em; }
  #cEvent.on  { background:#3a3d41; color:#a0a0a0; }   /* イベント有効 (通常) */
  #cEvent.off { background:#8a6d1f; color:#fff; }      /* イベント停止中 (目立たせる) */
  #cExit { background:#6a2f2f; }
  #cExit:hover { background:#8a3a3a; }
  #status { font-size:12px; color:#808080; white-space:nowrap; }

  .panel { flex:1; min-height:0; display:flex; flex-direction:column; }
  .panel[hidden] { display:none; }
  .toolbar { display:flex; gap:6px; align-items:center; padding:5px 8px;
    background:#252526; border-bottom:1px solid #333; }
  .toolbar input[type=search], .toolbar input[type=text] {
    background:#1e1e1e; color:#d4d4d4; border:1px solid #3c3c3c;
    padding:3px 6px; border-radius:3px; font:inherit; }
  .toolbar label { color:#a0a0a0; font-size:12px; }
  .grow { flex:1; }
  .sep { width:1px; align-self:stretch; background:#3c3c3c; margin:0 4px; }
  select { background:#1e1e1e; color:#d4d4d4; border:1px solid #3c3c3c;
    padding:3px 4px; border-radius:3px; font:inherit; }
  button { background:#0e639c; color:#fff; border:0; padding:4px 10px;
    border-radius:3px; cursor:pointer; font:inherit; }
  button:hover { background:#1177bb; }
  button.subtle { background:#3a3d41; }
  button.subtle:hover { background:#4a4d51; }

  /* --- Console --- */
  #log { flex:1; min-height:0; overflow-y:auto; padding:6px 8px; white-space:pre-wrap;
    word-break:break-word; }
  .echo    { color:#9cdcfe; }
  .result  { color:#6a9955; }
  .error   { color:#f48771; }
  .help    { color:#4ec9b0; }
  .info    { color:#d4d4d4; }
  .warn    { color:#dcdcaa; }
  .debug   { color:#4fc1ff; }
  .verbose { color:#808080; }
  .critical{ color:#f14c4c; font-weight:bold; }
  .hidden { display:none; }
  #bottom { display:flex; align-items:center; gap:6px; padding:6px 8px;
    background:#252526; border-top:1px solid #333; }
  #prompt { color:#6a9955; font-weight:bold; }
  #cmd { flex:1; background:#1e1e1e; color:#d4d4d4; border:1px solid #3c3c3c;
    padding:5px 8px; border-radius:3px; font:inherit; }

  /* --- Watch --- */
  #wwrap { flex:1; min-height:0; overflow-y:auto; }
  #wtable { width:100%; border-collapse:collapse; }
  #wtable th { position:sticky; top:0; background:#252526; color:#808080;
    font-weight:normal; font-size:12px; text-align:left;
    padding:4px 8px; border-bottom:1px solid #333; }
  #wtable td { padding:3px 8px; border-bottom:1px solid #2a2a2a;
    vertical-align:top; word-break:break-word; }
  #wtable tr:hover td { background:#252526; }
  .c-id   { width:3em; color:#808080; text-align:right; }
  .c-expr { width:40%; color:#9cdcfe; cursor:text; }
  .c-expr[contenteditable=true] { background:#1e1e1e; outline:1px solid #0e639c; color:#d4d4d4; }
  .c-val  { color:#6a9955; }
  .c-val.error { color:#f48771; }
  .c-del  { width:2em; text-align:center; }
  .c-del button { background:transparent; color:#808080; padding:0 4px; }
  .c-del button:hover { background:transparent; color:#f48771; }
  #wempty { padding:12px; color:#808080; }
  #wstatus { font-size:12px; color:#f48771; }

  /* --- Pad --- */
  #pText { flex:1; min-height:0; resize:none; border:0; outline:none;
    background:#1e1e1e; color:#d4d4d4; font:inherit; padding:8px;
    white-space:pre; overflow:auto; tab-size:4; }
  #pout { max-height:35%; overflow-y:auto; padding:6px 8px; white-space:pre-wrap;
    word-break:break-word; background:#252526; border-top:1px solid #333; }
  #pout:empty { display:none; }
  #pstatus { font-size:12px; color:#808080; white-space:nowrap; }
  #pstatus.err { color:#f48771; }

  /* --- 本体が終了したときの受け皿 (window.close() が効かない場合) --- */
  #gone { position:fixed; inset:0; background:rgba(30,30,30,.96); z-index:9999;
    display:flex; align-items:center; justify-content:center; text-align:center; }
  #gone h1 { color:#f48771; font-size:20px; margin:0 0 12px; }
  #gone p  { color:#a0a0a0; margin:4px 0; }
</style></head>
)HTMLPAGE"
// MSVC の文字列リテラル上限 (C2026) を避けるため分割 — 隣接連結で 1 本になる。
R"HTMLPAGE(<body><div id="app">
  <div id="top">
    <b>Kirikiri&nbsp;Z REPL</b>
    <div id="tabs">
      <button class="tab active" data-tab="console">Console</button>
      <button class="tab" data-tab="watch">Watch</button>
      <button class="tab" data-tab="pad">Pad</button>
    </div>
    <div id="ctl">
      <button id="cEvent" class="subtle" title="System.eventDisabled の切替 (吉里吉里2 コントローラの Event 相当)">イベント: —</button>
      <button id="cExit" title="アプリを終了する">終了</button>
      <span id="status">connecting...</span>
    </div>
  </div>

  <section class="panel" id="panel-console">
    <div class="toolbar">
      <input id="filter" class="grow" type="search" placeholder="フィルタ (ログを絞り込み)">
      <button id="clear" class="subtle">Clear</button>
    </div>
    <div id="log"></div>
    <div id="bottom">
      <span id="prompt">krkrz&gt;</span>
      <input id="cmd" autocomplete="off" spellcheck="false"
        placeholder="TJS 式/文を入力 (Enter=実行, .help=コマンド一覧, ↑↓=履歴)">
    </div>
  </section>

  <section class="panel" id="panel-watch" hidden>
    <div class="toolbar">
      <input id="wexpr" class="grow" type="text" autocomplete="off" spellcheck="false"
        placeholder="監視する式 (Enter=追加)">
      <button id="wadd">追加</button>
      <span class="sep"></span>
      <label>自動更新</label>
      <select id="wauto">
        <option value="-1">オフ</option>
        <option value="0">毎フレーム</option>
        <option value="100">0.1 秒</option>
        <option value="200">0.2 秒</option>
        <option value="500">0.5 秒</option>
        <option value="1000">1 秒</option>
        <option value="3000">3 秒</option>
        <option value="5000">5 秒</option>
        <option value="9000">9 秒</option>
      </select>
      <button id="weval" class="subtle">更新</button>
      <button id="wclear" class="subtle">全消し</button>
      <span id="wstatus"></span>
    </div>
    <div id="wwrap"><table id="wtable">
      <thead><tr>
        <th class="c-id">#</th><th class="c-expr">式</th><th class="c-val">値</th><th class="c-del"></th>
      </tr></thead>
      <tbody id="wbody"></tbody>
    </table>
    <div id="wempty">監視式はまだありません。上の欄に式を入れて Enter。
      式が例外を投げても「(error) …」が値として並ぶだけで、アプリは止まりません。</div>
    </div>
  </section>

  <section class="panel" id="panel-pad" hidden>
    <div class="toolbar">
      <button id="pRun">実行 (Ctrl+Enter)</button>
      <span class="sep"></span>
      <input id="pPath" class="grow" type="text" autocomplete="off" spellcheck="false"
        placeholder="ストレージパス (例 test.tjs)">
      <button id="pLoad" class="subtle">読込</button>
      <button id="pSave" class="subtle">保存</button>
      <span id="pstatus"></span>
    </div>
    <textarea id="pText" spellcheck="false" autocomplete="off"
      placeholder="TJS スクリプト (複数行可)。Ctrl+Enter で実行。&#10;内容はこのブラウザに自動保存されます (保存ボタンはストレージへの書き出し)。"></textarea>
    <div id="pout"></div>
  </section>
</div>
)HTMLPAGE"
R"HTMLPAGE(<script>
(function(){
  //-------------------------------------------------------------------------
  // タブ
  //-------------------------------------------------------------------------
  var panels = { console: document.getElementById('panel-console'),
                 watch:   document.getElementById('panel-watch'),
                 pad:     document.getElementById('panel-pad') };
  var focusOn = { console: 'cmd', watch: 'wexpr', pad: 'pText' };
  var tabs = document.querySelectorAll('.tab');
  function selectTab(name){
    for(var i=0;i<tabs.length;i++)
      tabs[i].classList.toggle('active', tabs[i].getAttribute('data-tab') === name);
    for(var k in panels) panels[k].hidden = (k !== name);
    var f = document.getElementById(focusOn[name]);
    if(f) f.focus();
  }
  for(var i=0;i<tabs.length;i++) (function(t){
    t.addEventListener('click', function(){ selectTab(t.getAttribute('data-tab')); });
  })(tabs[i]);

  //-------------------------------------------------------------------------
  // 本体が居なくなったらこのページも閉じる
  //
  // 本体だけ終わってブラウザのウィンドウが «disconnected» のまま残るのを防ぐ。
  // 判定は 2 系統:
  //   1. 本体が終了を通知した (/sub/state の exiting) → 即閉じる
  //   2. SSE が切れて kDeadMs 復帰しない (クラッシュ / 強制終了) → 閉じる
  // window.close() はアプリモード (--app) や script が開いた窓でしか効かない
  // ので、 閉じられなかったときは代わりに «終了しました» を全面に出す。
  //-------------------------------------------------------------------------
  var kDeadMs = 5000;
  var deadTimer = null, closing = false;

  function markAlive(){
    if(deadTimer){ clearTimeout(deadTimer); deadTimer = null; }
  }
  function markDead(){
    if(closing || deadTimer) return;
    deadTimer = setTimeout(function(){ closeSelf('本体との接続が切れました'); }, kDeadMs);
  }
  function closeSelf(reason){
    if(closing) return;
    closing = true;
    byeSent = true;                 // もう本体は居ないので合図は不要
    try { window.close(); } catch(_) {}
    // close が効かない (通常タブ) 場合の受け皿
    setTimeout(function(){ showGone(reason); }, 400);
  }
  function showGone(reason){
    if(document.getElementById('gone')) return;
    var d = document.createElement('div');
    d.id = 'gone';
    d.innerHTML = '<div><h1>' + reason + '</h1>' +
      '<p>吉里吉里Z が終了したため、このページは操作できません。</p>' +
      '<p>このタブは閉じて構いません。</p></div>';
    document.body.appendChild(d);
  }

  //-------------------------------------------------------------------------
  // Console (従来どおり: /events の SSE + POST /cmd)
  //-------------------------------------------------------------------------
  var log = document.getElementById('log');
  var cmd = document.getElementById('cmd');
  var statusEl = document.getElementById('status');
  var promptEl = document.getElementById('prompt');
  var filterEl = document.getElementById('filter');
  var hist = [], hpos = -1, filterText = '';

  function atBottom(){ return log.scrollHeight - log.scrollTop - log.clientHeight < 40; }
  function applyFilter(el){
    if(!filterText) { el.classList.remove('hidden'); return; }
    el.classList.toggle('hidden', el.textContent.toLowerCase().indexOf(filterText) < 0);
  }
  function append(cls, text){
    var stick = atBottom();
    var d = document.createElement('div');
    d.className = 'line ' + cls;
    d.textContent = text;
    applyFilter(d);
    log.appendChild(d);
    if(stick) log.scrollTop = log.scrollHeight;
  }

  var es = new EventSource('/events');
  es.onopen = function(){ statusEl.textContent = 'connected'; markAlive(); };
  es.onerror = function(){ statusEl.textContent = 'disconnected (retrying)'; markDead(); };
  es.onmessage = function(e){
    try { var m = JSON.parse(e.data); append(m.cls || 'info', m.text || ''); }
    catch(_) {}
  };

  filterEl.addEventListener('input', function(){
    filterText = filterEl.value.toLowerCase();
    var lines = log.children;
    for(var i=0;i<lines.length;i++) applyFilter(lines[i]);
  });
  document.getElementById('clear').addEventListener('click', function(){
    log.innerHTML=''; cmd.focus();
  });

  function submit(){
    var line = cmd.value;
    cmd.value=''; hpos=-1;
    fetch('/cmd', {method:'POST', body: line})
      .then(function(r){ return r.text(); })
      .then(function(cont){ promptEl.innerHTML = (cont==='1') ? '&nbsp;&nbsp;...&gt;' : 'krkrz&gt;'; })
      .catch(function(){});
    if(line.trim() !== '' && (hist.length===0 || hist[hist.length-1]!==line)) hist.push(line);
  }
  cmd.addEventListener('keydown', function(e){
    if(e.key==='Enter'){ e.preventDefault(); submit(); }
    else if(e.key==='ArrowUp'){ e.preventDefault();
      if(hist.length){ if(hpos<0) hpos=hist.length; if(hpos>0) hpos--; cmd.value=hist[hpos]; } }
    else if(e.key==='ArrowDown'){ e.preventDefault();
      if(hpos>=0){ hpos++; if(hpos>=hist.length){ hpos=-1; cmd.value=''; } else cmd.value=hist[hpos]; } }
  });

  //-------------------------------------------------------------------------
  // Watch (GET|POST /watch + GET /sub/watch)
  //
  // 行は id をキーに **その場で作り替えず更新する** (innerHTML の組み直しに
  // すると、自動更新のたびにインライン編集中のキャレットとテキスト選択が
  // 飛ぶため)。
  //-------------------------------------------------------------------------
  var wbody   = document.getElementById('wbody');
  var wempty  = document.getElementById('wempty');
  var wexpr   = document.getElementById('wexpr');
  var wauto   = document.getElementById('wauto');
  var wstatus = document.getElementById('wstatus');
  var wrows   = {};   // id -> { tr, exprCell, valCell, editing }

  function wnote(msg){
    wstatus.textContent = msg || '';
    if(msg) setTimeout(function(){ if(wstatus.textContent === msg) wstatus.textContent = ''; }, 4000);
  }

  function wsend(params){
    return fetch('/watch', { method:'POST', body: new URLSearchParams(params) })
      .then(function(r){ return r.json().then(function(j){ return { ok:r.ok, j:j }; }); })
      .then(function(res){
        if(res.ok) { wnote(''); applyWatch(res.j); }
        else wnote(res.j && res.j.error ? res.j.error : 'error');
      })
      .catch(function(){ wnote('通信できません'); });
  }
  function wload(){
    fetch('/watch').then(function(r){ return r.json(); })
      .then(applyWatch).catch(function(){});
  }

  function beginEdit(row, id){
    if(row.editing) return;
    row.editing = true;
    row.exprCell.contentEditable = 'true';
    row.exprCell.focus();
    var rng = document.createRange();
    rng.selectNodeContents(row.exprCell);
    var sel = window.getSelection();
    sel.removeAllRanges(); sel.addRange(rng);
  }
  function endEdit(row, id, commit){
    if(!row.editing) return;
    row.editing = false;
    row.exprCell.contentEditable = 'false';
    var v = row.exprCell.textContent.replace(/\s+$/, '').replace(/^\s+/, '');
    if(commit && v) wsend({ op:'edit', id:id, expr:v });
    else wload();   // 取り消し: サーバの値へ戻す
  }

  function makeRow(e){
    var tr = document.createElement('tr');
    var idc = document.createElement('td'); idc.className = 'c-id';   idc.textContent = e.id;
    var exc = document.createElement('td'); exc.className = 'c-expr'; exc.spellcheck = false;
    var vac = document.createElement('td'); vac.className = 'c-val';
    var dec = document.createElement('td'); dec.className = 'c-del';
    var del = document.createElement('button');
    del.type = 'button'; del.textContent = '×'; del.title = '削除';
    dec.appendChild(del);
    tr.appendChild(idc); tr.appendChild(exc); tr.appendChild(vac); tr.appendChild(dec);
    var row = { tr:tr, exprCell:exc, valCell:vac, editing:false };
    del.addEventListener('click', function(){ wsend({ op:'rm', id:e.id }); });
    exc.addEventListener('dblclick', function(){ beginEdit(row, e.id); });
    exc.addEventListener('keydown', function(ev){
      if(ev.key === 'Enter'){ ev.preventDefault(); endEdit(row, e.id, true); }
      else if(ev.key === 'Escape'){ ev.preventDefault(); endEdit(row, e.id, false); }
    });
    exc.addEventListener('blur', function(){ endEdit(row, e.id, true); });
    return row;
  }

  function applyWatch(st){
    if(!st || !st.entries) return;
    if(document.activeElement !== wauto) wauto.value = String(st.interval);
    var list = st.entries, seen = {};
    for(var i=0;i<list.length;i++){
      var e = list[i];
      seen[e.id] = true;
      var row = wrows[e.id];
      if(!row){ row = wrows[e.id] = makeRow(e); }
      if(wbody.children[i] !== row.tr)
        wbody.insertBefore(row.tr, wbody.children[i] || null);
      if(!row.editing) row.exprCell.textContent = e.expr;
      row.valCell.textContent = e.value;
      row.valCell.className = 'c-val' + (e.error ? ' error' : '');
    }
    for(var id in wrows){
      if(seen[id]) continue;
      if(wrows[id].tr.parentNode) wbody.removeChild(wrows[id].tr);
      delete wrows[id];
    }
    wempty.style.display = list.length ? 'none' : '';
  }

  function wadd(){
    var v = wexpr.value.replace(/^\s+/, '').replace(/\s+$/, '');
    if(!v) return;
    wexpr.value = '';
    wsend({ op:'add', expr:v });
  }
  document.getElementById('wadd').addEventListener('click', wadd);
  wexpr.addEventListener('keydown', function(e){
    if(e.key === 'Enter'){ e.preventDefault(); wadd(); }
  });
  wauto.addEventListener('change', function(){ wsend({ op:'interval', ms:wauto.value }); });
  document.getElementById('weval').addEventListener('click', function(){ wsend({ op:'eval' }); });
  document.getElementById('wclear').addEventListener('click', function(){
    if(window.confirm('監視式をすべて削除します。よろしいですか?')) wsend({ op:'clear' });
  });

  // 自動更新の push。値が変わったときと、評価を伴わない変更 (削除 / 全消し /
  // 間隔変更) のときにサーバから流れてくる。
  var wes = new EventSource('/sub/watch');
  wes.onmessage = function(e){
    try { applyWatch(JSON.parse(e.data)); } catch(_) {}
  };

)HTMLPAGE"
R"HTMLPAGE(  //-------------------------------------------------------------------------
  // Pad (吉里吉里2 の「スクリプトエディタ」窓の相当物)
  //
  // 原典の機能は Execute と Save。 編集操作 (Undo/Cut/Copy/Paste) は textarea が
  // 持っているので、 ここで足すのはその 2 つと、 読込・下書きの自動保存だけ。
  //
  // 本文は **localStorage に自動保存**する。 Pad は「書いて実行」を繰り返す場所
  // なので、 リロードやアプリ再起動で消えると使い物にならない。 ストレージへの
  // 書き出しは [保存] ボタン (本体側で -replwebpad が要る) と役割を分ける。
  //-------------------------------------------------------------------------
  var pText = document.getElementById('pText');
  var pPath = document.getElementById('pPath');
  var pOut  = document.getElementById('pout');
  var pStat = document.getElementById('pstatus');
  var kPadKey = 'krkrz.repl.pad', kPadPathKey = 'krkrz.repl.padpath';

  function pnote(msg, isErr){
    pStat.textContent = msg || '';
    pStat.classList.toggle('err', !!isErr);
    if(msg) setTimeout(function(){
      if(pStat.textContent === msg){ pStat.textContent=''; pStat.classList.remove('err'); }
    }, 4000);
  }
  function padSaveDraft(){
    try {
      localStorage.setItem(kPadKey, pText.value);
      localStorage.setItem(kPadPathKey, pPath.value);
    } catch(_) {}
  }
  try {
    var d = localStorage.getItem(kPadKey);
    if(d !== null) pText.value = d;
    var dp = localStorage.getItem(kPadPathKey);
    if(dp !== null) pPath.value = dp;
  } catch(_) {}
  pText.addEventListener('input', padSaveDraft);
  pPath.addEventListener('input', padSaveDraft);

  function padRun(){
    var src = pText.value;
    if(!src.replace(/^\s+|\s+$/g, '')) return;
    pOut.textContent = '実行中...';
    pOut.className = '';
    fetch('/pad/exec', { method:'POST', body: src })
      .then(function(r){ return r.json(); })
      .then(function(j){
        if(j.ok){ pOut.textContent = '=> ' + j.result; pOut.className = 'result'; }
        else    { pOut.textContent = j.error;          pOut.className = 'error'; }
      })
      .catch(function(){ pOut.textContent = '通信できません'; pOut.className = 'error'; });
  }
  document.getElementById('pRun').addEventListener('click', padRun);
  pText.addEventListener('keydown', function(e){
    // Ctrl+Enter で実行 (textarea なので Enter は改行のまま)
    if(e.key === 'Enter' && (e.ctrlKey || e.metaKey)){ e.preventDefault(); padRun(); }
  });

  document.getElementById('pLoad').addEventListener('click', function(){
    var path = pPath.value.replace(/^\s+|\s+$/g, '');
    if(!path){ pnote('パスを入れてください', true); return; }
    fetch('/pad/file?path=' + encodeURIComponent(path))
      .then(function(r){
        if(!r.ok) throw new Error(String(r.status));
        return r.text();
      })
      .then(function(t){ pText.value = t; padSaveDraft(); pnote('読込みました'); })
      .catch(function(e){ pnote('読み込めません (' + e.message + ')', true); });
  });

  document.getElementById('pSave').addEventListener('click', function(){
    var path = pPath.value.replace(/^\s+|\s+$/g, '');
    if(!path){ pnote('パスを入れてください', true); return; }
    fetch('/pad/file?path=' + encodeURIComponent(path),
          { method:'POST', body: pText.value })
      .then(function(r){ return r.json().then(function(j){ return { ok:r.ok, j:j }; }); })
      .then(function(res){
        if(res.ok) pnote('保存しました');
        else pnote(res.j && res.j.error ? res.j.error : '保存できません', true);
      })
      .catch(function(){ pnote('通信できません', true); });
  });

  //-------------------------------------------------------------------------
  // 閉じる合図 (-replwebidle 用)
  //
  // サーバは «SSE 購読が 0 本» をブラウザ不在の判定に使うが、 切断はハート
  // ビートの送信が失敗して初めて分かるので、 黙って閉じると気付くまで待たされる。
  // 閉じ際に beacon を 1 発投げて猶予を前倒しさせる。 届かなくても従来どおり
  // 指定秒数で畳まれるので、 **速くなるだけ**の経路。
  //
  // pagehide と beforeunload の両方に付ける (bfcache や モバイルでは
  // beforeunload が走らないことがある)。 sendBeacon は unload 中でも中断され
  // ない数少ない送信手段。
  //-------------------------------------------------------------------------
  var byeSent = false;
  function sendBye(){
    if(byeSent) return;
    byeSent = true;
    try {
      if(navigator.sendBeacon) navigator.sendBeacon('/bye', '');
      else fetch('/bye', { method:'POST', keepalive:true }).catch(function(){});
    } catch(_) {}
  }
  window.addEventListener('pagehide', sendBye);
  window.addEventListener('beforeunload', sendBye);
  // bfcache から復帰したら合図を取り消す (再購読でもサーバ側は取り下げるが、
  // こちら側のフラグも戻して次の close で改めて送れるようにする)。
  window.addEventListener('pageshow', function(){ byeSent = false; });

  //-------------------------------------------------------------------------
  // コントローラ (吉里吉里2 の「コントローラ」窓の相当物)
  //
  // 原典のツールバーは ScriptEditor / Console / Watch / Event / Exit。
  // 前 3 つはここではタブなので、 上のバーに残るのは Event と Exit。
  // 状態は /sub/state の push で追従するので、 **ゲーム側が
  // System.eventDisabled を変えても表示が合う**。
  //-------------------------------------------------------------------------
  var cEvent = document.getElementById('cEvent');
  var cExit  = document.getElementById('cExit');

  function applyState(st){
    if(!st) return;
    // 本体が «これから終了する» と言ってきた → 待たずに閉じる
    if(st.exiting){ closeSelf('吉里吉里Z が終了しました'); return; }
    if(typeof st.eventDisabled !== 'boolean') return;
    var off = st.eventDisabled;            // true = イベント配送が止まっている
    cEvent.textContent = 'イベント: ' + (off ? '停止中' : '有効');
    cEvent.classList.toggle('off', off);
    cEvent.classList.toggle('on', !off);
  }
  function statePost(params){
    return fetch('/state', { method:'POST', body: new URLSearchParams(params) })
      .then(function(r){ return r.json(); })
      .then(applyState)
      .catch(function(){});
  }
  cEvent.addEventListener('click', function(){
    statePost({ op:'event', value:'toggle' });
  });
  cExit.addEventListener('click', function(){
    if(!window.confirm('アプリを終了します。よろしいですか?')) return;
    // 終了させるので閉じる合図は要らない (二重に落としにいかない)
    byeSent = true;
    statePost({ op:'exit' }).then(function(){
      statusEl.textContent = 'terminated';
    });
  });
  var ses = new EventSource('/sub/state');
  ses.onmessage = function(e){
    try { applyState(JSON.parse(e.data)); } catch(_) {}
  };
  fetch('/state').then(function(r){ return r.json(); }).then(applyState).catch(function(){});

  wload();
  cmd.focus();
})();
</script>
</body></html>
)HTMLPAGE";
