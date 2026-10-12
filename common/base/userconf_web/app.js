// 吉里吉里Z エンジン設定 (-userconf) の画面
//   GET  /api/userconf        項目一覧
//   POST /api/userconf/save   {"values":{name:value}} を .cfu へ保存
//   POST /api/userconf/close  終了 (サーバが止まるとこの窓も閉じる)
import { app } from './lib/appserve.js';

const TEXT = {
	ja: {
		title: '吉里吉里Z エンジン設定', file: '保存先: ', save: '保存して閉じる', cancel: '保存しないで閉じる',
		resetAll: 'すべて初期値に戻す', reset: '初期値に戻す', def: '初期値: ', changed: (n) => `変更した項目: ${n}`,
		saved: '保存しました。閉じています…', saveFailed: '保存できませんでした: ', loadFailed: '設定を読み込めませんでした: ',
		confirmClose: '変更を保存せずに閉じますか?', empty: '設定できる項目がありません', pick: '左の一覧から項目を選んでください',
		emptyValue: '(空)'
	},
	en: {
		title: 'Kirikiri Z Settings', file: 'Saved to: ', save: 'Save and close', cancel: 'Close without saving',
		resetAll: 'Reset all to defaults', reset: 'Reset to default', def: 'Default: ', changed: (n) => `${n} changed`,
		saved: 'Saved. Closing…', saveFailed: 'Could not save: ', loadFailed: 'Could not load the settings: ',
		confirmClose: 'Close without saving your changes?', empty: 'There are no settings to change', pick: 'Select an item from the list',
		emptyValue: '(empty)'
	},
	chs: {
		title: '吉里吉里Z 引擎设置', file: '保存位置：', save: '保存并关闭', cancel: '不保存直接关闭',
		resetAll: '全部恢复默认', reset: '恢复默认', def: '默认值：', changed: (n) => `已更改 ${n} 项`,
		saved: '已保存，正在关闭…', saveFailed: '无法保存：', loadFailed: '无法读取设置：',
		confirmClose: '要放弃更改并关闭吗？', empty: '没有可设置的项目', pick: '请从左侧列表选择一个项目',
		emptyValue: '(空)'
	},
	cht: {
		title: '吉里吉里Z 引擎設定', file: '儲存位置：', save: '儲存並關閉', cancel: '不儲存直接關閉',
		resetAll: '全部恢復預設', reset: '恢復預設', def: '預設值：', changed: (n) => `已變更 ${n} 項`,
		saved: '已儲存，正在關閉…', saveFailed: '無法儲存：', loadFailed: '無法讀取設定：',
		confirmClose: '要放棄變更並關閉嗎？', empty: '沒有可設定的項目', pick: '請從左側清單選擇一個項目',
		emptyValue: '(空)'
	}
};

const $ = (id) => document.getElementById(id);
let T = TEXT.ja;
let model = null;          // サーバから来た項目一覧
let items = [];            // 項目を平らに並べたもの
let values = {};           // 今の値 (name -> value)
let initial = {};          // 開いた時点の値
let selected = null;       // 選択中の項目 name
let closing = false;

function setStatus(msg, isError) {
	const s = $('status');
	s.textContent = msg || '';
	s.classList.toggle('error', !!isError);
}

function changedCount() {
	return items.filter((it) => values[it.name] !== initial[it.name]).length;
}

function updateStatus() {
	const n = changedCount();
	setStatus(n ? T.changed(n) : '');
}

function valueLabel(it) {
	const v = values[it.name];
	if (it.type === 'select') {
		const c = it.values.find((x) => x.value === v);
		return c ? (c.desc || c.value) : v;
	}
	return v === '' ? T.emptyValue : v;
}

function renderList() {
	const list = $('list');
	list.textContent = '';
	if (!items.length) {
		const d = document.createElement('div');
		d.className = 'uc-cat';
		d.textContent = T.empty;
		list.appendChild(d);
		return;
	}
	for (const cat of model.categories) {
		const h = document.createElement('div');
		h.className = 'uc-cat';
		h.textContent = cat.name;
		list.appendChild(h);
		for (const it of cat.options) {
			const row = document.createElement('div');
			row.className = 'uc-item';
			row.dataset.name = it.name;
			row.tabIndex = 0;
			const cap = document.createElement('span');
			cap.className = 'cap';
			cap.textContent = it.caption || it.name;
			const val = document.createElement('span');
			val.className = 'val';
			row.append(cap, val);
			row.addEventListener('click', () => select(it.name));
			row.addEventListener('keydown', (e) => {
				if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); select(it.name); }
				if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
					e.preventDefault();
					const i = items.findIndex((x) => x.name === it.name) + (e.key === 'ArrowDown' ? 1 : -1);
					if (i >= 0 && i < items.length) select(items[i].name, true);
				}
			});
			list.appendChild(row);
		}
	}
	refreshList();
}

function refreshList() {
	for (const row of $('list').querySelectorAll('.uc-item')) {
		const it = items.find((x) => x.name === row.dataset.name);
		row.querySelector('.val').textContent = valueLabel(it);
		row.classList.toggle('changed', values[it.name] !== it.default);
		row.classList.toggle('selected', it.name === selected);
	}
	updateStatus();
}

function select(name, focusRow) {
	selected = name;
	refreshList();
	renderDetail();
	if (focusRow) {
		const row = $('list').querySelector(`.uc-item[data-name="${CSS.escape(name)}"]`);
		if (row) { row.focus(); row.scrollIntoView({ block: 'nearest' }); }
	}
}

function renderDetail() {
	const d = $('detail');
	d.textContent = '';
	const it = items.find((x) => x.name === selected);
	if (!it) {
		const p = document.createElement('div');
		p.className = 'uc-empty';
		p.textContent = items.length ? T.pick : T.empty;
		d.appendChild(p);
		return;
	}
	const h = document.createElement('h2');
	h.textContent = it.caption || it.name;
	const nm = document.createElement('div');
	nm.className = 'name';
	nm.textContent = '-' + it.name;

	let input;
	if (it.type === 'select') {
		input = document.createElement('select');
		for (const c of it.values) {
			const o = document.createElement('option');
			o.value = c.value;
			o.textContent = c.desc || c.value;
			input.appendChild(o);
		}
		input.value = values[it.name];
		input.addEventListener('change', () => { values[it.name] = input.value; refreshList(); });
	} else {
		input = document.createElement('input');
		input.type = 'text';
		if (it.length > 0) input.maxLength = it.length;
		input.value = values[it.name];
		input.addEventListener('input', () => { values[it.name] = input.value; refreshList(); });
	}
	const reset = document.createElement('button');
	reset.textContent = T.reset;
	reset.addEventListener('click', () => { values[it.name] = it.default; renderDetail(); refreshList(); });
	const row = document.createElement('div');
	row.className = 'row';
	row.append(input, reset);

	const def = document.createElement('div');
	def.className = 'def';
	const dc = it.type === 'select' ? it.values.find((x) => x.value === it.default) : null;
	def.textContent = T.def + (dc ? (dc.desc || dc.value) : (it.default === '' ? T.emptyValue : it.default));

	const desc = document.createElement('div');
	desc.className = 'desc';
	desc.textContent = it.description || '';

	d.append(h, nm, row, def, desc);
}

async function closeApp() {
	closing = true;
	try { await app.post('/api/userconf/close', {}); } catch (e) { /* サーバ側で閉じる */ }
	window.close();
}

async function save() {
	$('save').disabled = true;
	try {
		await app.post('/api/userconf/save', { values });
		initial = { ...values };
		setStatus(T.saved);
		await closeApp();
	} catch (e) {
		setStatus(T.saveFailed + (e && e.message ? e.message : e), true);
		$('save').disabled = false;
	}
}

async function main() {
	await app.ready();
	try {
		model = await app.get('/api/userconf');
	} catch (e) {
		setStatus(T.loadFailed + (e && e.message ? e.message : e), true);
		return;
	}
	T = TEXT[model.lang] || TEXT.ja;
	document.documentElement.lang = model.lang === 'chs' ? 'zh-Hans' : model.lang === 'cht' ? 'zh-Hant' : model.lang;
	document.title = T.title;
	$('title').textContent = T.title;
	$('file').textContent = model.file ? T.file + model.file : '';
	$('save').textContent = T.save;
	$('cancel').textContent = T.cancel;
	$('reset-all').textContent = T.resetAll;

	items = model.categories.flatMap((c) => c.options);
	for (const it of items) values[it.name] = it.current;
	initial = { ...values };
	renderList();
	select(items.length ? items[0].name : null);

	$('save').addEventListener('click', save);
	$('cancel').addEventListener('click', () => {
		if (changedCount() && !confirm(T.confirmClose)) return;
		closeApp();
	});
	$('reset-all').addEventListener('click', () => {
		for (const it of items) values[it.name] = it.default;
		renderDetail();
		refreshList();
	});
	window.addEventListener('beforeunload', (e) => {
		if (!closing && changedCount()) { e.preventDefault(); e.returnValue = ''; }
	});
}

main();
