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

  /* --- 案件パネル (WebServer.registerPanel で足されたタブ) --- */
  .panel > iframe { flex:1; min-height:0; width:100%; border:0; background:#1e1e1e; }

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
      <select id="cLang" title="UI language"></select>
      <button id="cEvent" class="subtle" data-i18n-title="ctl.event.title">イベント: —</button>
      <button id="cExit" data-i18n-title="ctl.exit.title" data-i18n="ctl.exit">終了</button>
      <span id="status">connecting...</span>
    </div>
  </div>

  <section class="panel" id="panel-console">
    <div class="toolbar">
      <input id="filter" class="grow" type="search" data-i18n-ph="con.filter">
      <button id="clear" class="subtle">Clear</button>
    </div>
    <div id="log"></div>
    <div id="bottom">
      <span id="prompt">krkrz&gt;</span>
      <input id="cmd" autocomplete="off" spellcheck="false"
        data-i18n-ph="con.input">
    </div>
  </section>

  <section class="panel" id="panel-watch" hidden>
    <div class="toolbar">
      <input id="wexpr" class="grow" type="text" autocomplete="off" spellcheck="false"
        data-i18n-ph="watch.expr">
      <button id="wadd" data-i18n="watch.add">追加</button>
      <span class="sep"></span>
      <label data-i18n="watch.auto">自動更新</label>
      <select id="wauto">
        <option value="-1" data-i18n="watch.off">オフ</option>
        <option value="0" data-i18n="watch.everyframe">毎フレーム</option>
        <option value="100" data-i18n="watch.ms100">0.1 秒</option>
        <option value="200" data-i18n="watch.ms200">0.2 秒</option>
        <option value="500" data-i18n="watch.ms500">0.5 秒</option>
        <option value="1000" data-i18n="watch.s1">1 秒</option>
        <option value="3000" data-i18n="watch.s3">3 秒</option>
        <option value="5000" data-i18n="watch.s5">5 秒</option>
        <option value="9000" data-i18n="watch.s9">9 秒</option>
      </select>
      <button id="weval" class="subtle" data-i18n="watch.update">更新</button>
      <button id="wclear" class="subtle" data-i18n="watch.clear">全消し</button>
      <span id="wstatus"></span>
    </div>
    <div id="wwrap"><table id="wtable">
      <thead><tr>
        <th class="c-id">#</th><th class="c-expr" data-i18n="watch.col.expr">式</th><th class="c-val" data-i18n="watch.col.value">値</th><th class="c-del"></th>
      </tr></thead>
      <tbody id="wbody"></tbody>
    </table>
    <div id="wempty" data-i18n="watch.empty"></div>
    </div>
  </section>

  <section class="panel" id="panel-pad" hidden>
    <div class="toolbar">
      <button id="pRun" data-i18n="pad.run">実行 (Ctrl+Enter)</button>
      <span class="sep"></span>
      <input id="pPath" class="grow" type="text" autocomplete="off" spellcheck="false"
        data-i18n-ph="pad.path">
      <button id="pLoad" class="subtle" data-i18n="pad.load">読込</button>
      <button id="pSave" class="subtle" data-i18n="pad.save">保存</button>
      <span id="pstatus"></span>
    </div>
    <textarea id="pText" spellcheck="false" autocomplete="off"
      data-i18n-ph="pad.placeholder"></textarea>
    <div id="pout"></div>
  </section>
</div>
)HTMLPAGE"
R"HTMLPAGE(<script>
(function(){
  //-------------------------------------------------------------------------
  // 文言 (i18n)
  //
  // **既定の言語は本体の言語設定に従う**。 /state が返す langs が本体の
  // 決定結果 (優先順) で、 -language= も OS 言語もそこに反映されている。
  // ここに表がある言語のうち、 先に見つかったものを使う (例: zh-Hant の環境で
  // cht の表が無ければ en へ落ちる)。 ブラウザ側で明示的に選ぶこともでき、
  // その選択は localStorage に残る。
  //
  // 表はページに同梱する。 -replweb だけで完結させる方針なので資材を外から
  // 読まない。 切替が即時なのもこのおかげ。
  // **言語を足すときはここに 1 ブロック増やすだけ** (キーが無ければ ja へ落ちる)。
  //-------------------------------------------------------------------------
  var I18N = {
    ja: {
      'lang.name':          '日本語',
      'ctl.event':          'イベント:',
      'ctl.event.on':       '有効',
      'ctl.event.off':      '停止中',
      'ctl.event.title':    'System.eventDisabled の切替 (吉里吉里2 コントローラの Event 相当)',
      'ctl.exit':           '終了',
      'ctl.exit.title':     'アプリを終了する',
      'ctl.exit.confirm':   'アプリを終了します。よろしいですか?',
      'con.filter':         'フィルタ (ログを絞り込み)',
      'con.input':          'TJS 式/文を入力 (Enter=実行, .help=コマンド一覧, ↑↓=履歴)',
      'watch.expr':         '監視する式 (Enter=追加)',
      'watch.add':          '追加',
      'watch.auto':         '自動更新',
      'watch.off':          'オフ',
      'watch.everyframe':   '毎フレーム',
      'watch.ms100':        '0.1 秒',
      'watch.ms200':        '0.2 秒',
      'watch.ms500':        '0.5 秒',
      'watch.s1':           '1 秒',
      'watch.s3':           '3 秒',
      'watch.s5':           '5 秒',
      'watch.s9':           '9 秒',
      'watch.update':       '更新',
      'watch.clear':        '全消し',
      'watch.clear.confirm':'監視式をすべて削除します。よろしいですか?',
      'watch.col.expr':     '式',
      'watch.col.value':    '値',
      'watch.del':          '削除',
      'watch.empty':        '監視式はまだありません。上の欄に式を入れて Enter。式が例外を投げても「(error) …」が値として並ぶだけで、アプリは止まりません。',
      'pad.run':            '実行 (Ctrl+Enter)',
      'pad.path':           'ストレージパス (例 test.tjs)',
      'pad.load':           '読込',
      'pad.save':           '保存',
      'pad.placeholder':    'TJS スクリプト (複数行可)。Ctrl+Enter で実行。\n内容はこのブラウザに自動保存されます (保存ボタンはストレージへの書き出し)。',
      'pad.running':        '実行中...',
      'pad.needpath':       'パスを入れてください',
      'pad.loaded':         '読込みました',
      'pad.loadfail':       '読み込めません',
      'pad.saved':          '保存しました',
      'pad.savefail':       '保存できません',
      'err.network':        '通信できません',
      'gone.disconnected':  '本体との接続が切れました',
      'gone.exited':        '吉里吉里Z が終了しました',
      'gone.body':          '吉里吉里Z が終了したため、このページは操作できません。',
      'gone.close':         'このタブは閉じて構いません。'
    },
    en: {
      'lang.name':          'English',
      'ctl.event':          'Events:',
      'ctl.event.on':       'enabled',
      'ctl.event.off':      'stopped',
      'ctl.event.title':    'Toggle System.eventDisabled (the Event button of the kirikiri2 controller)',
      'ctl.exit':           'Exit',
      'ctl.exit.title':     'Terminate the application',
      'ctl.exit.confirm':   'Terminate the application. Are you sure?',
      'con.filter':         'Filter (narrow the log)',
      'con.input':          'TJS expression/statement (Enter=run, .help=commands, Up/Down=history)',
      'watch.expr':         'Expression to watch (Enter=add)',
      'watch.add':          'Add',
      'watch.auto':         'Auto update',
      'watch.off':          'off',
      'watch.everyframe':   'every frame',
      'watch.ms100':        '0.1 s',
      'watch.ms200':        '0.2 s',
      'watch.ms500':        '0.5 s',
      'watch.s1':           '1 s',
      'watch.s3':           '3 s',
      'watch.s5':           '5 s',
      'watch.s9':           '9 s',
      'watch.update':       'Update',
      'watch.clear':        'Clear all',
      'watch.clear.confirm':'Remove all watch expressions. Are you sure?',
      'watch.col.expr':     'Expression',
      'watch.col.value':    'Value',
      'watch.del':          'Remove',
      'watch.empty':        'No watch expressions yet. Type one above and press Enter. An expression that throws just shows "(error) ..." as its value - the application keeps running.',
      'pad.run':            'Run (Ctrl+Enter)',
      'pad.path':           'Storage path (e.g. test.tjs)',
      'pad.load':           'Load',
      'pad.save':           'Save',
      'pad.placeholder':    'TJS script (multiple lines allowed). Ctrl+Enter to run.\nThe text is kept in this browser automatically (Save writes it to storage).',
      'pad.running':        'running...',
      'pad.needpath':       'Enter a path',
      'pad.loaded':         'Loaded',
      'pad.loadfail':       'Cannot load',
      'pad.saved':          'Saved',
      'pad.savefail':       'Cannot save',
      'err.network':        'Cannot reach the server',
      'gone.disconnected':  'Lost connection to the application',
      'gone.exited':        'Kirikiri Z has exited',
      'gone.body':          'Kirikiri Z has exited, so this page can no longer be used.',
      'gone.close':         'You can close this tab.'
    }
  };

)HTMLPAGE"
R"HTMLPAGE(  var kLangKey = 'krkrz.repl.lang';
  var lang = 'ja';          // 実効言語
  var engineLangs = ['ja']; // 本体が返した優先順

  function t(key){
    var tbl = I18N[lang] || I18N.ja;
    var v = tbl[key];
    if(v === undefined) v = I18N.ja[key];    // 未訳は日本語へ落とす
    return v === undefined ? key : v;
  }

  //! data-i18n / -ph / -title を今の言語で塗り直す (切替でも呼ぶ)
  function applyI18n(){
    document.documentElement.lang = (lang === 'ja') ? 'ja' : lang;
    var n = document.querySelectorAll('[data-i18n]');
    for(var i=0;i<n.length;i++) n[i].textContent = t(n[i].getAttribute('data-i18n'));
    n = document.querySelectorAll('[data-i18n-ph]');
    for(i=0;i<n.length;i++) n[i].placeholder = t(n[i].getAttribute('data-i18n-ph'));
    n = document.querySelectorAll('[data-i18n-title]');
    for(i=0;i<n.length;i++) n[i].title = t(n[i].getAttribute('data-i18n-title'));
  }

  //! 本体の優先順のうち、表を持っている最初の言語を採る
  function pickLang(list){
    for(var i=0;i<list.length;i++) if(I18N[list[i]]) return list[i];
    return 'ja';
  }

  function setLang(code, remember){
    if(!I18N[code]) return;
    lang = code;
    if(remember){ try { localStorage.setItem(kLangKey, code); } catch(_) {} }
    var sel = document.getElementById('cLang');
    if(sel) sel.value = code;
    applyI18n();
    if(typeof onLangChanged === 'function') onLangChanged();
  }

  // 言語セレクタ (表にあるものを並べる)
  (function initLangSelect(){
    var sel = document.getElementById('cLang');
    for(var code in I18N){
      var o = document.createElement('option');
      o.value = code;
      o.textContent = I18N[code]['lang.name'] || code;
      sel.appendChild(o);
    }
    sel.addEventListener('change', function(){ setLang(sel.value, true); });
  })();

  // 起動時: ブラウザ側の明示指定があればそれ、無ければ後で /state の langs に従う
  var saved = null;
  try { saved = localStorage.getItem(kLangKey); } catch(_) {}
  setLang(saved && I18N[saved] ? saved : 'ja', false);

  //-------------------------------------------------------------------------
  // タブ
  //-------------------------------------------------------------------------
  var panels = { console: document.getElementById('panel-console'),
                 watch:   document.getElementById('panel-watch'),
                 pad:     document.getElementById('panel-pad') };
  var focusOn = { console: 'cmd', watch: 'wexpr', pad: 'pText' };
  var tabsEl = document.getElementById('tabs');
  var appEl  = document.getElementById('app');
  var current = 'console';

  function allTabs(){ return document.querySelectorAll('.tab'); }
  function selectTab(name){
    if(!panels[name]) return;
    current = name;
    var tabs = allTabs();
    for(var i=0;i<tabs.length;i++)
      tabs[i].classList.toggle('active', tabs[i].getAttribute('data-tab') === name);
    for(var k in panels) panels[k].hidden = (k !== name);
    // 案件パネルは初めて開いたときに読み込む (重いツールを起動時に抱えない)
    var p = panels[name];
    if(p.dataset && p.dataset.src && !p.firstElementChild){
      var f = document.createElement('iframe');
      f.src = p.dataset.src;
      p.appendChild(f);
    }
    var el = document.getElementById(focusOn[name]);
    if(el) el.focus();
  }
  tabsEl.addEventListener('click', function(e){
    var t = e.target.closest ? e.target.closest('.tab') : null;
    if(t) selectTab(t.getAttribute('data-tab'));
  });

  //-------------------------------------------------------------------------
  // 案件パネル (WebServer.registerPanel)
  //
  // 組み込みタブの右へ、案件が登録したパネルをタブとして並べる。中身は
  // iframe で読む — こちらの内部 DOM に依存させないため。登録/解除は
  // /sub/panels の push で開いているページにも反映される。
  //-------------------------------------------------------------------------
  function applyPanels(list){
    if(!list || !list.length){ removeExtTabs({}); return; }
    var seen = {};
    for(var i=0;i<list.length;i++){
      var d = list[i];
      if(!d || !d.id || !d.path) continue;
      var key = 'x-' + d.id;
      seen[key] = true;
      var tab = document.querySelector('.tab[data-tab="' + key + '"]');
      if(!tab){
        tab = document.createElement('button');
        tab.className = 'tab ext';
        tab.setAttribute('data-tab', key);
        tabsEl.appendChild(tab);
        var sec = document.createElement('section');
        sec.className = 'panel';
        sec.id = 'panel-' + key;
        sec.hidden = true;
        appEl.appendChild(sec);
        panels[key] = sec;
      }
      tab.textContent = d.label || d.id;
      var sec2 = panels[key];
      // path が変わったら中身を作り直す
      if(sec2.dataset.src !== d.path){
        sec2.dataset.src = d.path;
        if(sec2.firstElementChild) sec2.removeChild(sec2.firstElementChild);
        if(current === key) selectTab(key);   // 表示中なら即読み直す
      }
    }
    removeExtTabs(seen);
  }
  function removeExtTabs(seen){
    var ext = document.querySelectorAll('.tab.ext');
    for(var i=0;i<ext.length;i++){
      var key = ext[i].getAttribute('data-tab');
      if(seen[key]) continue;
      ext[i].parentNode.removeChild(ext[i]);
      if(panels[key]){
        if(panels[key].parentNode) panels[key].parentNode.removeChild(panels[key]);
        delete panels[key];
      }
      if(current === key) selectTab('console');   // 消えたタブを見ていたら戻す
    }
  }
  fetch('/panels').then(function(r){ return r.json(); })
    .then(applyPanels).catch(function(){});
  var pes = new EventSource('/sub/panels');
  pes.onmessage = function(e){
    try { applyPanels(JSON.parse(e.data)); } catch(_) {}
  };

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
    deadTimer = setTimeout(function(){ closeSelf(t('gone.disconnected')); }, kDeadMs);
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
    var h1 = document.createElement('h1'); h1.textContent = reason;
    var p1 = document.createElement('p'); p1.textContent = t('gone.body');
    var p2 = document.createElement('p'); p2.textContent = t('gone.close');
    var box = document.createElement('div');
    box.appendChild(h1); box.appendChild(p1); box.appendChild(p2);
    d.appendChild(box);
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
      .catch(function(){ wnote(t('err.network')); });
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
    del.type = 'button'; del.textContent = '×'; del.title = t('watch.del');
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
    if(window.confirm(t('watch.clear.confirm'))) wsend({ op:'clear' });
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
    pOut.textContent = t('pad.running');
    pOut.className = '';
    fetch('/pad/exec', { method:'POST', body: src })
      .then(function(r){ return r.json(); })
      .then(function(j){
        if(j.ok){ pOut.textContent = '=> ' + j.result; pOut.className = 'result'; }
        else    { pOut.textContent = j.error;          pOut.className = 'error'; }
      })
      .catch(function(){ pOut.textContent = t('err.network'); pOut.className = 'error'; });
  }
  document.getElementById('pRun').addEventListener('click', padRun);
  pText.addEventListener('keydown', function(e){
    // Ctrl+Enter で実行 (textarea なので Enter は改行のまま)
    if(e.key === 'Enter' && (e.ctrlKey || e.metaKey)){ e.preventDefault(); padRun(); }
  });

  document.getElementById('pLoad').addEventListener('click', function(){
    var path = pPath.value.replace(/^\s+|\s+$/g, '');
    if(!path){ pnote(t('pad.needpath'), true); return; }
    fetch('/pad/file?path=' + encodeURIComponent(path))
      .then(function(r){
        if(!r.ok) throw new Error(String(r.status));
        return r.text();
      })
      .then(function(t){ pText.value = t; padSaveDraft(); pnote(t('pad.loaded')); })
      .catch(function(e){ pnote(t('pad.loadfail') + ' (' + e.message + ')', true); });
  });

  document.getElementById('pSave').addEventListener('click', function(){
    var path = pPath.value.replace(/^\s+|\s+$/g, '');
    if(!path){ pnote(t('pad.needpath'), true); return; }
    fetch('/pad/file?path=' + encodeURIComponent(path),
          { method:'POST', body: pText.value })
      .then(function(r){ return r.json().then(function(j){ return { ok:r.ok, j:j }; }); })
      .then(function(res){
        if(res.ok) pnote(t('pad.saved'));
        else pnote(res.j && res.j.error ? res.j.error : t('pad.savefail'), true);
      })
      .catch(function(){ pnote(t('err.network'), true); });
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

  var lastState = null;
  function applyState(st){
    if(!st) return;
    // 本体が «これから終了する» と言ってきた → 待たずに閉じる
    if(st.exiting){ closeSelf(t('gone.exited')); return; }
    // 既定の言語は本体の設定に従う (ブラウザ側で明示指定していないときだけ)。
    if(st.langs && st.langs.length){
      engineLangs = st.langs;
      var saved = null;
      try { saved = localStorage.getItem(kLangKey); } catch(_) {}
      if(!saved) setLang(pickLang(engineLangs), false);
    }
    if(typeof st.eventDisabled !== 'boolean') return;
    lastState = st;
    renderState();
  }
  function renderState(){
    if(!lastState) return;
    var off = lastState.eventDisabled;     // true = イベント配送が止まっている
    cEvent.textContent = t('ctl.event') + ' ' + (off ? t('ctl.event.off') : t('ctl.event.on'));
    cEvent.classList.toggle('off', off);
    cEvent.classList.toggle('on', !off);
  }
  // 言語を切り替えたら、data-i18n で塗れない動的表示も作り直す
  function onLangChanged(){ renderState(); }
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
    if(!window.confirm(t('ctl.exit.confirm'))) return;
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
