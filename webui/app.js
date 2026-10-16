/* ==========================================================================
   LocalCraft · 前端逻辑
   无框架、无构建、无依赖。所有重活都在 C 后端完成。
   ========================================================================== */
'use strict';

const TOKEN = new URLSearchParams(location.search).get('token') || '';
const $  = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));

/* ---------------------------------------------------------------- 图标 -- */
const I = {
  ok:    '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9.4"/><path d="M8.1 12.3l2.7 2.7L16 9.6"/></svg>',
  error: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9.4"/><path d="M15 9l-6 6M9 9l6 6"/></svg>',
  warn:  '<svg viewBox="0 0 24 24"><path d="M12 3.7L21.1 19H2.9z"/><path d="M12 10.1v3.6M12 16.7h.01"/></svg>',
  info:  '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9.4"/><path d="M12 11.2v5.4M12 7.7h.01"/></svg>',
  ask:   '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9.4"/><path d="M9.5 9.3a2.6 2.6 0 0 1 5.1.8c0 1.7-2.6 2.3-2.6 2.3M12 16.6h.01"/></svg>',
  check: '<svg viewBox="0 0 24 24"><path d="M5 12.6l4.4 4.4L19 7.4"/></svg>',
  empty: '<svg viewBox="0 0 24 24"><path d="M3.5 6.8A1.8 1.8 0 0 1 5.3 5h3.6l1.5 1.9h7.3a1.8 1.8 0 0 1 1.8 1.8v7.5a1.8 1.8 0 0 1-1.8 1.8H5.3a1.8 1.8 0 0 1-1.8-1.8z"/><path d="M9.2 13h5.6"/></svg>',
};

/* ---------------------------------------------------------------- 状态 -- */
const S = {
  view: 'rename',
  planId: null,
  plan: null,
  policy: 'rename',
  onlyConflicts: false,
  jobId: null,
  timer: null,
  source: 'rename',       // 当前预览来自哪个页签
};

/* ============================================================== 后端 --- */
async function call(method, params = {}) {
  let res;
  try {
    res = await fetch(`/api/call?token=${encodeURIComponent(TOKEN)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ method, params }),
    });
  } catch (e) {
    throw new Error('无法连接到 LocalCraft 服务，请重新启动程序。');
  }
  if (!res.ok) throw new Error(`服务返回 ${res.status}`);
  const data = await res.json();
  if (!data.ok) throw new Error(data.error || '未知错误');
  return data.result;
}

/* ============================================================ 通知提示 -- */
function toast(kind, title, message, ms = 4200) {
  const el = document.createElement('div');
  el.className = `toast toast-${kind}`;
  el.innerHTML = `${I[kind] || I.info}<div class="toast-body"><strong></strong><span></span></div>`;
  el.querySelector('strong').textContent = title;
  el.querySelector('span').textContent = message || '';
  $('#toasts').appendChild(el);
  const close = () => { el.classList.add('leaving'); setTimeout(() => el.remove(), 250); };
  el.addEventListener('click', close);
  if (ms) setTimeout(close, ms);
}

/* ============================================================ 确认弹窗 -- */
function modal({ title, bodyHtml, confirmText = '确定', cancelText = '取消',
                 danger = false, icon = 'warn' }) {
  return new Promise(resolve => {
    const wrap = document.createElement('div');
    wrap.className = 'modal-backdrop';
    wrap.innerHTML = `
      <div class="modal" role="dialog" aria-modal="true">
        <h3>${I[icon] || ''}<span></span></h3>
        <div class="modal-body"></div>
        <div class="modal-actions">
          <button class="btn btn-quiet" data-act="cancel"></button>
          <button class="btn ${danger ? 'btn-danger' : 'btn-primary'}" data-act="ok"></button>
        </div>
      </div>`;
    wrap.querySelector('h3 span').textContent = title;
    wrap.querySelector('.modal-body').innerHTML = bodyHtml;
    wrap.querySelector('[data-act="cancel"]').textContent = cancelText;
    wrap.querySelector('[data-act="ok"]').textContent = confirmText;
    $('#modal-root').appendChild(wrap);

    const done = v => { wrap.remove(); document.removeEventListener('keydown', onKey); resolve(v); };
    const onKey = e => {
      if (e.key === 'Escape') done(false);
      else if (e.key === 'Enter') done(true);
    };
    document.addEventListener('keydown', onKey);
    wrap.querySelector('[data-act="ok"]').onclick = () => done(true);
    wrap.querySelector('[data-act="cancel"]').onclick = () => done(false);
    wrap.onclick = e => { if (e.target === wrap) done(false); };
    wrap.querySelector('[data-act="ok"]').focus();
  });
}

/* ==================================================== 模块 2：格式转换 == */

const TARGETS = [
  { ext: 'md',   name: 'Markdown',   memo: '把任意文档抽成 Markdown', builtin: true },
  { ext: 'txt',  name: '纯文本',      memo: '剥离所有排版标记',        builtin: true },
  { ext: 'html', name: 'HTML',       memo: '带基础样式的单页',        builtin: true },
  { ext: 'csv',  name: 'CSV 表格',    memo: '适合表格类文档',          builtin: true },
  { ext: 'png',  name: 'PNG 图片',    memo: '无损，带透明通道',        builtin: true },
  { ext: 'jpg',  name: 'JPEG 图片',   memo: '有损，体积小',            builtin: true },
  { ext: 'bmp',  name: 'BMP 图片',    memo: '无压缩位图',              builtin: true },
  { ext: 'tiff', name: 'TIFF 图片',   memo: '印刷常用',                builtin: true },
  { ext: 'docx', name: 'Word 文档',   memo: '需要 Pandoc 或 LibreOffice', builtin: false },
  { ext: 'pdf',  name: 'PDF',        memo: '需要 LibreOffice',        builtin: false },
];

let convertTarget = 'md';

function renderFormats() {
  $('#convert-formats').innerHTML = TARGETS.map(t => `
    <button class="fmt ${t.ext === convertTarget ? 'is-active' : ''}" data-fmt="${t.ext}">
      <b>${t.name}</b>
      <span>.${t.ext}</span>
      <em class="${t.builtin ? 'builtin' : 'external'}">${t.builtin ? '内置' : '需外部引擎'}</em>
    </button>`).join('');
}

async function loadEngines() {
  try {
    const r = await call('convert.engines', {});
    const NAMES = { pandoc: 'Pandoc', soffice: 'LibreOffice', ffmpeg: 'FFmpeg', libraw: 'LibRaw / dcraw' };
    const USE = {
      pandoc: 'Markdown / Word / HTML / EPUB 高质量互转',
      soffice: 'Word / PPT / Excel / PDF 互转，版式保真度最高',
      ffmpeg: '音视频格式转换与提取',
      libraw: '相机 RAW（CR2/CR3/NEF/ARW/RAF/RW2…）解码',
    };
    $('#convert-engines').innerHTML = (r.engines || []).map(e => `
      <div class="engine ${e.found ? 'found' : ''}">
        <span class="pill ${e.found ? 'pill-ok' : 'pill-info'}">${e.found ? '已找到' : '未找到'}</span>
        <div>
          <div class="engine-name">${NAMES[e.key] || e.key}</div>
          <div class="engine-use">${USE[e.key] || ''}</div>
          <div class="engine-path">${esc(e.found ? e.path : '把可执行文件放进 ' + r.tools_dir + ' 即可启用')}</div>
        </div>
      </div>`).join('');
  } catch (e) { /* 引擎探测失败不影响其它功能 */ }
}

async function doConvertPreview() {
  const cfg = {
    dir: val('#convert-dir'), target: convertTarget,
    recursive: on('#convert-recursive'), policy: S.policy,
  };
  if (!cfg.dir) { toast('warn', '请先选择文件夹'); return; }
  try {
    const payload = await call('convert.preview', cfg);
    setStep('convert', 2);
    renderPreview(payload, 'convert');
    const st = payload.stats || {};
    toast('ok', '预览已生成', `将生成 ${st.executable} 个新文件`, 5200);
  } catch (e) { toast('error', '生成预览失败', e.message, 9000); }
}

/* ==================================================== 模块 3：内容去重 == */

async function doDedupScan() {
  const dir = val('#dedup-dir');
  if (!dir) { toast('warn', '请先选择文件夹'); return; }
  const btn = $('#btn-dedup-scan');
  btn.disabled = true;
  btn.textContent = '正在抽取文本并计算指纹…';
  try {
    const r = await call('dedup.scan', {
      dir, threshold: num('#dedup-threshold', 92) / 100,
      recursive: on('#dedup-recursive'), min_chars: num('#dedup-minchars', 40),
    });
    renderDedupResult(r);
    toast('ok', '扫描完成（只读）',
          `${r.stats.scanned} 个文档，发现 ${r.stats.groups} 组内容重复`, 6000);
  } catch (e) {
    toast('error', '扫描失败', e.message, 9000);
  } finally {
    btn.disabled = false;
    btn.textContent = '只读扫描（不修改任何文件）';
  }
}

function renderDedupResult(r) {
  $('#dedup-result-card').hidden = false;
  const st = r.stats;
  $('#dedup-result-sub').textContent =
    `相似度阈值 ${(st.threshold * 100).toFixed(0)}%（汉明距离 ≤ ${st.max_distance}）` +
    (st.skipped ? ` · ${st.skipped} 个文件因无法抽取文本或内容过短被跳过` : '');
  $('#dedup-stats').innerHTML = [
    ['文档', st.scanned, ''], ['重复组', st.groups, st.groups ? 'is-warn' : ''],
    ['可移走', st.duplicates, st.duplicates ? 'is-danger' : ''], ['跳过', st.skipped, ''],
  ].map(([l, v, c]) => `<div class="stat ${c}"><b>${v}</b><span>${l}</span></div>`).join('');

  if (!r.groups || !r.groups.length) {
    $('#dedup-groups').innerHTML = `<div class="empty">${I.empty}
      <h3>没有发现内容重复的文档</h3>
      <p>可以把相似度阈值调低一点再试，或者换一个有更多文档的文件夹。</p></div>`;
    return;
  }
  $('#dedup-groups').innerHTML = r.groups.map(g => `
    <div class="dedup-group">
      <div class="dedup-group-head">
        <span class="pill pill-warn">第 ${g.group} 组 · ${g.items.length} 份</span>
        <h3>内容雷同，默认保留第一份</h3>
      </div>
      ${g.items.map(it => `
        <div class="dedup-item ${it.keeper ? 'is-keeper' : ''}">
          <span class="sim-badge ${it.keeper ? 'is-keeper' : ''}">
            ${it.keeper ? '保留' : (it.similarity * 100).toFixed(0) + '%'}
          </span>
          <span class="dedup-path" title="${esc(it.path)}">${esc(it.path)}</span>
          <span class="dedup-size">${esc(it.size_text)} · ${it.tokens} 词</span>
        </div>`).join('')}
    </div>`).join('');
}

async function doDedupPreview() {
  const dir = val('#dedup-dir');
  if (!dir) { toast('warn', '请先选择文件夹'); return; }
  try {
    const payload = await call('dedup.preview', {
      dir, threshold: num('#dedup-threshold', 92) / 100,
      recursive: on('#dedup-recursive'), min_chars: num('#dedup-minchars', 40),
      action: num('#dedup-action', 0), policy: S.policy,
    });
    renderPreview(payload, 'dedup');
    const st = payload.stats || {};
    if (!st.executable) toast('info', '没有需要处理的项', '这一批文档没有发现内容重复。', 6000);
    else toast('ok', '预览已生成', `将移走 ${st.executable} 份重复文档`, 5200);
  } catch (e) { toast('error', '生成预览失败', e.message, 9000); }
}

/* ==================================================== 模块 4：文档摘要 == */

let lastSummary = null;

function summaryAlgo() {
  const el = $('#summary-algo .seg.is-active');
  return el ? parseInt(el.dataset.value, 10) : 1;
}

async function doSummarize() {
  const path = val('#summary-path');
  if (!path) { toast('warn', '请先选择一个文档', '也可以选文件夹做批量摘要。'); return; }
  const btn = $('#btn-summary-file');
  const old = btn.textContent;
  btn.disabled = true; btn.textContent = '分析中…';
  try {
    const r = await call('summarize.file', {
      path, sentences: num('#summary-sentences', 5), algorithm: summaryAlgo(),
    });
    lastSummary = r;
    renderSummary(r);
    toast('ok', '摘要已生成',
          `抽出 ${r.sentences.length} 个关键句、${r.keywords.length} 个关键词`, 5200);
  } catch (e) {
    toast('error', '无法生成摘要', e.message, 9000);
  } finally {
    btn.disabled = false; btn.textContent = old;
  }
}

function renderSummary(r) {
  $('#summary-result-card').hidden = false;
  $('#summary-batch-card').hidden = true;
  $('#summary-title').textContent = r.name;
  $('#summary-sub').textContent =
    `${summaryAlgo() ? 'TextRank' : 'TF-IDF'} · 原文 ${r.text_length} 字 · 抽取式摘要（不依赖任何 AI 模型）`;
  $('#summary-stats').innerHTML = [
    ['原文', r.text_length + ' 字', ''],
    ['关键句', r.sentences.length, 'is-ok'],
    ['关键词', r.keywords.length, ''],
  ].map(([l, v, c]) => `<div class="stat ${c}"><b>${v}</b><span>${l}</span></div>`).join('');

  $('#summary-sentences').innerHTML = r.sentences.map((s, i) => `
    <li class="sentence-item">
      <span class="sentence-no">${i + 1}</span>
      <div class="sentence-text">
        <div>${esc(s.text)}</div>
        <div class="sentence-meta">原文位置 ${s.start}–${s.end} · 权重 ${s.score.toFixed(3)}</div>
      </div>
    </li>`).join('');

  $('#summary-keywords').innerHTML = r.keywords.map(k =>
    `<span class="chip" title="权重 ${k.score.toFixed(2)}">${esc(k.word)}</span>`).join('');
}

async function doBatchSummary() {
  const dir = val('#summary-dir');
  if (!dir) { toast('warn', '请先选择文件夹'); return; }
  try {
    const r = await call('summarize.batch', {
      dir, sentences: num('#summary-sentences', 5), algorithm: summaryAlgo(),
      recursive: true, limit: 100,
    });
    toast('info', '开始批量摘要', `共 ${r.total} 个文档`, 4500);
    pollBatch(r.job_id);
  } catch (e) { toast('error', '无法开始批量摘要', e.message, 9000); }
}

function pollBatch(jobId) {
  $('#taskbar').hidden = false;
  $('#task-label').textContent = '正在分析文档…';
  $('#task-bar').style.width = '0%';
  const step = async () => {
    let job;
    try { job = await call('job.status', { job_id: jobId }); }
    catch (e) { $('#taskbar').hidden = true; toast('error', '任务丢失', e.message); return; }
    const pct = job.total ? (job.done / job.total) * 100 : 0;
    $('#task-bar').style.width = `${pct}%`;
    $('#task-count').textContent = `${job.done} / ${job.total}`;
    if (job.label) $('#task-label').textContent = job.label;
    if (!job.finished) { setTimeout(step, 250); return; }
    $('#taskbar').hidden = true;
    if (job.error) { toast('error', '批量摘要失败', job.error, 9000); return; }
    if (job.result) renderBatch(job.result);
  };
  step();
}

function renderBatch(r) {
  $('#summary-result-card').hidden = true;
  $('#summary-batch-card').hidden = false;
  $('#batch-sub').textContent = `成功 ${r.ok} 篇，失败 ${r.failed} 篇，共 ${r.total} 篇（每篇取前 3 句）`;
  $('#batch-list').innerHTML = (r.results || []).map(it => {
    if (!it.ok) {
      return `<div class="batch-item">
        <div class="batch-item-head"><span class="pill pill-warn">跳过</span>
          <span class="batch-item-name">${esc(it.name)}</span></div>
        <div class="batch-item-error">${esc(it.error || '')}</div></div>`;
    }
    const txt = (it.sentences || []).slice(0, 3).map(s => s.text).join(' ');
    const kw = (it.keywords || []).slice(0, 8).map(k => k.word).join(' · ');
    return `<div class="batch-item">
      <div class="batch-item-head"><span class="pill pill-ok">OK</span>
        <span class="batch-item-name">${esc(it.name)}</span></div>
      <div class="batch-item-text">${esc(txt)}</div>
      <div class="sentence-meta" style="margin-top:5px">关键词：${esc(kw)}</div></div>`;
  }).join('');
}

/* ---- 模块 5：导出（同样走可回退的计划）---- */

async function doExport(fmt) {
  if (!lastSummary) {
    toast('warn', '请先生成摘要', '导出会把「原文 + 摘要」合成一份文档。');
    return;
  }
  const fmtName = { md: 'Markdown', docx: 'Word', pdf: 'PDF' }[fmt] || fmt;
  const ok = await modal({
    title: `导出为 ${fmtName}`,
    icon: 'ask',
    confirmText: '生成预览',
    bodyHtml: `<p>将根据刚才的摘要生成一份 ${fmtName} 文档，放在源文档旁边。</p>
      <p>生成会写入回退日志：<span class="strong">之后可以一键撤销</span>，
         撤销时产物进回收站，源文档不受影响。</p>
      <p class="hint">${fmt === 'pdf'
        ? 'PDF 使用标准中文宋体，文字可选中；主流阅读器都能正常显示中文。'
        : fmt === 'docx'
        ? 'Word 文档使用 Heading 1–6 样式并带 outlineLvl，导航窗格与目录可直接用。'
        : 'Markdown 保留标题层级与上下标标记。'}</p>`,
  });
  if (!ok) return;
  try {
    const r = await call('export.run', {
      path: lastSummary.path, format: fmt,
      out_name: lastSummary.name.replace(/\.[^.]+$/, '') + '_摘要',
    });
    renderPreview(r, 'summary');
    toast('info', '导出预览已生成', '在预览表里确认后点「执行」即可。', 6000);
  } catch (e) {
    toast('error', '无法准备导出', e.message, 9000);
  }
}

/* ================================================================ 视图 -- */
const PAGES = {
  rename:   ['批量重命名', '按规则批量改名；执行前先看对照表，随时可回退。'],
  organize: ['整理分类', '按类型、时间、名称自动归类，扫描阶段完全只读。'],
  convert:  ['格式转换', '文档与图片批量互转；内置转换断网可用，产物可回退。'],
  dedup:    ['内容去重', '按内容指纹找出「文件名不同但正文雷同」的文档。'],
  summary:  ['文档摘要', 'TextRank / TF-IDF 抽取关键句与关键词，不依赖任何 AI 模型。'],
  preview:  ['预览对照表', '确认「旧状态 → 新状态」无误后，才会真正执行。'],
  history:  ['回退历史', '日志驱动的一键回退，不受系统撤销与时间限制。'],
  env:      ['离线自检', '本机能力与引擎检测，全程不联网。'],
};

function setStep(name, n) {
  const el = $(`.stepper[data-stepper="${name}"]`);
  if (!el) return;
  $$('li', el).forEach((li, i) => {
    li.classList.toggle('is-done', i < n);
    li.classList.toggle('is-active', i === n);
  });
}

function showView(name) {
  S.view = name;
  $$('.view').forEach(v => v.classList.toggle('is-active', v.id === `view-${name}`));
  $$('.nav-item').forEach(b => b.classList.toggle('is-active', b.dataset.view === name));
  const [t, s] = PAGES[name] || ['', ''];
  $('#page-title').textContent = t;
  $('#page-sub').textContent = s;
  $('#content').scrollTop = 0;
  if (name === 'history') refreshHistory();
  if (name === 'env') loadEnv().catch(() => {});
  if (name === 'convert') loadEngines();
}

/* ================================================================ 表单 -- */
const val = sel => ($(sel) ? $(sel).value.trim() : '');
const on  = sel => !!($(sel) && $(sel).checked);
const num = (sel, d) => { const v = parseInt(val(sel), 10); return Number.isFinite(v) ? v : d; };
const list = sel => val(sel).split(/[\s,;]+/).filter(Boolean);
const toBytes = text => {
  const m = String(text || '').trim().toUpperCase().match(/^([\d.]+)\s*(KB|MB|GB|B)?$/);
  if (!m) return 0;
  const mul = { B: 1, KB: 1024, MB: 1024 ** 2, GB: 1024 ** 3 }[m[2] || 'B'] || 1;
  return Math.round(parseFloat(m[1]) * mul);
};

function renameConfig() {
  return {
    dir: val('#rename-dir'), template: val('#rename-template'),
    find: val('#rename-find'), replace: val('#rename-replace'),
    regex: on('#rename-regex'), ignore_case: on('#rename-icase'),
    target: val('#rename-target'),
    prefix: val('#rename-prefix'), suffix: val('#rename-suffix'),
    number: on('#rename-number'), number_pos: val('#rename-number-pos'),
    number_start: num('#rename-number-start', 1), number_width: num('#rename-number-width', 3),
    number_step: num('#rename-number-step', 1),
    case: val('#rename-case'), ext_case: val('#rename-ext-case'),
    include_ext: list('#rename-include'), exclude_ext: list('#rename-exclude'),
    recursive: on('#rename-recursive'), reverse: on('#rename-reverse'),
    sort: val('#rename-sort'), policy: S.policy,
  };
}

function organizeConfig() {
  const mode = ($('#organize-mode .seg.is-active') || { dataset: {} }).dataset.value || 'move';
  return {
    dir: val('#organize-dir'), template: val('#organize-template') || '{category}',
    mode, recursive: on('#organize-recursive'),
    min_size: toBytes(val('#organize-min')), max_size: toBytes(val('#organize-max')),
    older_than: num('#organize-older', 0), policy: S.policy,
  };
}

/* ============================================================ 预览渲染 -- */
const LEVEL = { ok: ['pill-ok', '正常'], info: ['pill-info', '提示'],
                warn: ['pill-warn', '警告'], error: ['pill-error', '冲突'] };

const esc = s => String(s == null ? '' : s)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

function splitPath(full) {
  const i = Math.max(full.lastIndexOf('\\'), full.lastIndexOf('/'));
  return i < 0 ? ['', full] : [full.slice(0, i + 1), full.slice(i + 1)];
}

function renderPreview(payload, source) {
  S.plan = payload;
  S.planId = payload.plan_id;
  if (source) S.source = source;

  $('#preview-title').textContent = payload.title || '预览对照表';
  $('#preview-note').textContent = payload.note
    ? `规则：${payload.note}`
    : (payload.root ? `目标目录：${payload.root}` : '');

  const st = payload.stats || {};
  $('#preview-stats').innerHTML = [
    ['总数', st.total, ''], ['可执行', st.executable, 'is-ok'],
    ['覆盖', st.overwrite, st.overwrite ? 'is-warn' : ''],
    ['冲突', st.blocked, st.blocked ? 'is-danger' : ''],
  ].map(([label, v, cls]) =>
    `<div class="stat ${cls}"><b>${v ?? 0}</b><span>${label}</span></div>`).join('');

  drawRows();
  $('#confirm-info').textContent = st.executable
    ? `将处理 ${st.executable} 个文件。执行过程会逐步写入回退日志，之后可一键还原。`
    : '没有可执行的行：请检查勾选状态或冲突项。';
  $('#btn-execute').disabled = !st.executable;

  const isRollback = payload.category === 'rollback';
  const pol = $('#preview-policy');
  pol.style.opacity = isRollback ? '.4' : '1';
  pol.style.pointerEvents = isRollback ? 'none' : 'auto';

  showView('preview');
}

function visibleRows() {
  const rows = (S.plan && S.plan.rows) || [];
  return S.onlyConflicts
    ? rows.filter(r => r.level === 'warn' || r.level === 'error' || r.selected !== '✓')
    : rows;
}

function drawRows() {
  const body = $('#preview-body');
  const rows = visibleRows();
  if (!rows.length) {
    body.innerHTML = `<tr><td colspan="7"><div class="empty">${I.empty}
      <h3>${S.onlyConflicts ? '没有冲突项' : '没有需要处理的行'}</h3>
      <p>${S.onlyConflicts
          ? '当前所有文件都不存在命名冲突或跳过情况。'
          : '换个规则、换个目录，或确认扩展名过滤条件是否正确。'}</p>
      </div></td></tr>`;
    $('#preview-foot').textContent = '';
    return;
  }

  const MAX = 3000;
  body.innerHTML = rows.slice(0, MAX).map(r => {
    const [cls, text] = LEVEL[r.level] || ['pill-info', r.level];
    const [od, on_] = splitPath(r.old || '');
    const [nd, nn] = splitPath(r.new || '');
    const note = r.conflicts || r.note || '';
    const noteCls = r.level === 'error' ? 'note-danger' : r.level === 'warn' ? 'note-warn' : '';
    const trCls = [r.level === 'error' ? 'is-blocked' : '',
                   r.selected === '✓' ? '' : 'is-off'].join(' ');
    const blocked = r.level === 'error';
    return `<tr class="${trCls}">
      <td><div class="check ${r.selected === '✓' ? 'on' : ''} ${blocked ? 'disabled' : ''}"
               data-toggle="${r.index - 1}">${I.check}</div></td>
      <td class="mono muted">${r.index}</td>
      <td><span class="pill ${cls}">${text}</span></td>
      <td><div class="path path-old"><span class="path-dir">${esc(od)}</span>${esc(on_)}</div></td>
      <td class="arrow-cell">→</td>
      <td><div class="path path-new"><span class="path-dir">${esc(nd)}</span>${esc(nn)}</div></td>
      <td><div class="note-cell ${noteCls}">${esc(note)}</div></td>
    </tr>`;
  }).join('');

  const total = (S.plan.rows || []).length;
  $('#preview-foot').textContent = total > MAX
    ? `表格仅渲染前 ${MAX} 行（共 ${total} 行）；执行时会处理全部勾选项。`
    : `共 ${total} 行 —— 点击任意一行即可勾选或取消。`;
}

/* ============================================================ 任务进度 -- */
function startTask(jobId, kind) {
  S.jobId = jobId;
  $('#taskbar').hidden = false;
  $('#task-label').textContent = kind === 'rollback' ? '正在回退…' : '正在执行…';
  $('#task-bar').style.width = '0%';
  $('#task-count').textContent = '0 / 0';
  $('#btn-cancel-task').disabled = false;

  const tick = async () => {
    if (!S.jobId) return;
    let job;
    try { job = await call('job.status', { job_id: jobId }); }
    catch (e) { stopTask(); toast('error', '任务状态丢失', e.message); return; }

    const pct = job.total ? (job.done / job.total) * 100 : 0;
    $('#task-bar').style.width = `${pct}%`;
    $('#task-count').textContent = `${job.done} / ${job.total}`;
    if (job.label) {
      const [d, n] = splitPath(job.label);
      $('#task-label').textContent = n || job.label;
    }
    if (job.finished) { stopTask(); await afterTask(job, kind); return; }
    S.timer = setTimeout(tick, 200);
  };
  tick();
}

function stopTask() {
  S.jobId = null;
  if (S.timer) clearTimeout(S.timer);
  S.timer = null;
  $('#taskbar').hidden = true;
}

async function afterTask(job, kind) {
  if (job.error) { toast('error', '执行出错', job.error, 9000); return; }
  const r = job.result;
  if (!r) return;

  if (kind === 'rollback') {
    await refreshHistory();
    toast(r.failed ? 'warn' : 'ok', r.failed ? '回退完成（有跳过）' : '回退完成',
          r.message, 8500);
    return;
  }
  if (r.cancelled)      toast('warn', '已取消', `${r.message}。已完成的部分可通过回退还原。`, 8500);
  else if (r.failed)    toast('warn', '完成（部分失败）', r.message, 9000);
  else                  toast('ok', '执行完成', r.message, 8000);

  if (r.errors && r.errors.length) {
    toast('error', '失败明细', r.errors.slice(0, 3).join('   ·   '), 13000);
  }
  await loadAppInfo();
  await refreshHistory();
  if (S.planId) {
    try { renderPreview(await call('plan.get', { plan_id: S.planId })); } catch (_) {}
  }
}

/* ================================================================ 动作 -- */
async function doPreview(kind) {
  const cfg = kind === 'rename' ? renameConfig() : organizeConfig();
  if (!cfg.dir) { toast('warn', '请先选择文件夹', '点击「选择文件夹」，或直接粘贴完整路径。'); return; }

  const btn = kind === 'rename' ? $('#btn-rename-preview') : $('#btn-organize-preview');
  btn.disabled = true;
  try {
    const payload = await call(`${kind}.preview`, cfg);
    setStep(kind, 2);
    renderPreview(payload, kind);
    const st = payload.stats || {};
    if (!st.total) toast('info', '没有匹配到文件', '请检查目录、扩展名过滤条件或规则是否为空。');
    else toast('ok', '预览已生成',
               `共 ${st.total} 项，可执行 ${st.executable} 项` +
               (st.blocked ? `，${st.blocked} 项存在冲突` : ''), 5200);
  } catch (e) {
    toast('error', '生成预览失败', e.message, 8000);
  } finally {
    btn.disabled = false;
  }
}

async function doExecute() {
  if (!S.planId || !S.plan) return;
  const st = S.plan.stats || {};
  if (!st.executable) { toast('warn', '没有可执行项', '请先勾选要处理的行。'); return; }

  const isRollback = S.plan.category === 'rollback';
  let extra = '';
  if (st.overwrite) extra += `<li>其中 <span class="strong">${st.overwrite}</span> 项会覆盖同名文件，旧文件会先移入回收站（仍可回退）。</li>`;
  if (st.blocked)   extra += `<li><span class="danger-text">${st.blocked}</span> 项存在冲突，将被自动跳过。</li>`;

  const ok = await modal({
    title: isRollback ? '确认执行回退' : '确认执行批量操作',
    confirmText: isRollback ? `回退（${st.executable} 步）` : `执行（${st.executable} 项）`,
    bodyHtml: `
      <p>即将对 <span class="strong">${st.executable}</span> 个条目执行操作。</p>
      <p>目标目录：<span class="strong">${esc(S.plan.root || '—')}</span></p>
      ${extra ? `<ul>${extra}</ul>` : ''}
      <p>执行过程会逐步写入回退日志，之后可在「回退历史」中一键还原。</p>`,
  });
  if (!ok) return;

  try {
    const { job_id } = await call('exec.run', { plan_id: S.planId });
    startTask(job_id, 'execute');
  } catch (e) { toast('error', '无法开始执行', e.message, 8000); }
}

async function doDryRun() {
  if (!S.planId) return;
  try {
    const r = await call('exec.dry', { plan_id: S.planId });
    if (r.problems && r.problems.length) {
      await modal({
        title: '只读预演发现问题', icon: 'warn', confirmText: '知道了', cancelText: '关闭',
        bodyHtml: `<p>预演不修改任何文件。当前状态下有
          <span class="danger-text">${r.problems.length}</span> 项无法执行：</p>
          <ul>${r.problems.slice(0, 12).map(p => `<li>${esc(p)}</li>`).join('')}</ul>`,
      });
    } else {
      toast('ok', '只读预演通过', `全部 ${r.ok} 项当前状态正常，可以安全执行。`, 6000);
    }
  } catch (e) { toast('error', '预演失败', e.message); }
}

/* ================================================================ 历史 -- */
async function refreshHistory() {
  try {
    const data = await call('history.list', { limit: 300 });
    const rows = data.rows || [];
    const badge = $('#badge-journals');
    badge.textContent = rows.length;
    badge.hidden = !rows.length;

    const body = $('#history-body');
    if (!rows.length) {
      body.innerHTML = `<tr><td colspan="6"><div class="empty">${I.empty}
        <h3>暂无操作记录</h3>
        <p>在「批量重命名」或「整理分类」里执行一次操作后，这里就会出现可回退的记录。</p>
      </div></td></tr>`;
      return;
    }
    body.innerHTML = rows.map(r => {
      const tag = r.kind === 'rollback' ? '<span class="pill pill-info">回退</span>'
                : r.undone ? '<span class="pill pill-warn">已回退</span>' : '';
      return `<tr>
        <td class="mono muted">${esc(r.when)}</td>
        <td><span class="strong">${esc(r.category)}</span> ${tag}</td>
        <td class="mono">${r.steps} 步</td>
        <td><span class="pill ${r.status === 'completed' ? 'pill-ok' : 'pill-info'}">${esc(r.status)}</span></td>
        <td><div class="path muted" title="${esc(r.root)}">${esc(r.root)}</div></td>
        <td style="text-align:right">
          <button class="btn btn-soft btn-sm" data-rollback="${esc(r.path)}">回退</button>
        </td></tr>`;
    }).join('');
  } catch (e) { toast('error', '读取历史失败', e.message); }
}

async function previewRollback(path) {
  try {
    const payload = await call('history.rollback_preview', { path });
    renderPreview(payload, 'history');
    $('#confirm-info').textContent =
      `${(payload.stats || {}).executable || 0} 个步骤可回退；` +
      `回退过程同样写日志，之后还能撤销回来。`;
    const detail = await call('history.detail', { path });
    renderHistoryDetail(detail);
    if (!(payload.stats || {}).executable) {
      toast('info', '没有可回退的步骤',
            '文件可能已被移动或删除，或原位置已被其它文件占用（不会覆盖）。', 8500);
    }
  } catch (e) { toast('error', '无法预览回退', e.message, 8000); }
}

function renderHistoryDetail(detail) {
  const card = $('#history-detail-card');
  card.hidden = false;
  const KIND = { move: '移动', mkdir: '建目录', rmdir: '删目录', copy: '复制', create: '生成' };
  const steps = (detail.steps || []).slice(0, 100);
  $('#history-detail').innerHTML = steps.length
    ? steps.map(s => `<div class="step-item">
        <span class="step-kind">${KIND[s.kind] || s.kind}</span>
        <span class="step-path">${esc(s.src ? s.src + '  →  ' + s.dst : s.dst)}</span>
      </div>`).join('')
    : `<div class="empty"><p>该记录没有可回退的步骤。</p></div>`;
}

async function runRollback(path) {
  const ok = await modal({
    title: '确认回退', confirmText: '执行回退',
    bodyHtml: `<p>将把该操作完整回退到执行前的状态。</p>
      <p>文件会被移回原位；执行时新建的目录会被清理。</p>
      <p>若原位置已被其它文件占用，LocalCraft 会<span class="strong">跳过</span>而不会覆盖。</p>
      <p>回退过程也会写日志，之后还能再撤销回来。</p>`,
  });
  if (!ok) return;
  try {
    const payload = await call('history.rollback_preview', { path });
    const { job_id } = await call('history.rollback_run', { plan_id: payload.plan_id });
    startTask(job_id, 'rollback');
  } catch (e) { toast('error', '无法回退', e.message, 8000); }
}

/* ================================================================ 自检 -- */
const CAPS = [
  '批量重命名：模板 / 查找替换 / 序号 / 大小写',
  '批量移动与分类：按类型、年份、扩展名、首字母',
  '预览对照表：执行前列出「旧状态 → 新状态」',
  '冲突检测：源缺失、目标占用、同批撞车、链式移动',
  '一键回退：日志逆序重放，不受时间限制',
  '只读预演：执行前校验，绝不碰文件',
  '中文与长路径：\\\\?\\ 前缀、保留名、非法字符',
  '覆盖保护：旧文件先进回收站，永不直接删除',
];

async function loadEnv() {
  const [info, env] = await Promise.all([call('app.info'), call('app.engines')]);
  paintAppInfo(info);

  $('#env-info').innerHTML = [
    ['程序', `LocalCraft ${info.version}`],
    ['运行模式', info.portable ? '便携模式（数据写在程序目录）' : '标准模式'],
    ['数据目录', info.data_root],
    ['日志目录', info.journal_dir],
    ['回收站占用', info.trash],
    ['历史记录', `${info.journal_count} 份`],
  ].map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join('');

  $('#env-caps').innerHTML = CAPS.map(c =>
    `<li>${I.ok}<span>${esc(c)}</span></li>`).join('');

  $('#env-engines').innerHTML = (env.engines || []).map(e => `
    <div class="engine">
      <span class="pill pill-info">未接入</span>
      <div>
        <div class="engine-name">${esc(e.name)}</div>
        <div class="engine-use">${esc(e.provides)}</div>
        <div class="engine-path">${esc(e.hint)}</div>
      </div>
    </div>`).join('');
}

function paintAppInfo(info) { $('#foot-storage').textContent = info.storage || ''; }
async function loadAppInfo() {
  try { paintAppInfo(await call('app.info')); } catch (_) {}
}

/* ============================================================ 扫描统计 -- */
async function doScan() {
  const dir = val('#organize-dir');
  if (!dir) { toast('warn', '请先选择文件夹'); return; }
  const btn = $('#btn-organize-scan');
  btn.disabled = true;
  try {
    const data = await call('organize.scan', { dir, recursive: on('#organize-recursive') });
    const wrap = $('#organize-stats');
    wrap.hidden = false;
    const rows = data.rows || [];
    wrap.innerHTML = rows.length ? rows.slice(0, 12).map(r => `
      <div class="statbar">
        <span class="statbar-name">${esc(r.category)}</span>
        <div class="statbar-track"><div class="statbar-fill" style="width:${(r.ratio * 100).toFixed(1)}%"></div></div>
        <span class="statbar-size">${esc(r.size_text)}</span>
        <span class="statbar-count">${r.count} 个</span>
      </div>`).join('') + `
      <div class="statbar" style="margin-top:6px;padding-top:9px;border-top:1px solid var(--line)">
        <span class="statbar-name">合计</span><div></div>
        <span class="statbar-size">${esc(data.total_size_text)}</span>
        <span class="statbar-count">${data.total_count} 个</span>
      </div>`
      : `<p class="hint">该目录下没有找到文件。</p>`;
    toast('ok', '扫描完成（只读）',
          `${data.total_count} 个文件，共 ${data.total_size_text}`, 5200);
  } catch (e) { toast('error', '扫描失败', e.message); }
  finally { btn.disabled = false; }
}

async function doRenameScan() {
  const dir = val('#rename-dir');
  const bar = $('#rename-scan');
  if (!dir) { bar.hidden = true; return; }
  try {
    const r = await call('rename.preview', { ...renameConfig(), dir, limit: 0 });
    bar.hidden = false;
    $('#rename-scan-text').textContent =
      `只读扫描：匹配到 ${r.stats.total} 个文件（尚未做任何修改）`;
  } catch (_) {
    bar.hidden = true;
  }
}

/* ================================================================ 绑定 -- */
function bind() {
  $('#nav').addEventListener('click', e => {
    const b = e.target.closest('.nav-item');
    if (b) showView(b.dataset.view);
  });
  $('#btn-rollback-shortcut').onclick = () => showView('history');

  $('#btn-theme').onclick = () => {
    const html = document.documentElement;
    const cur = html.dataset.theme || 'auto';
    html.dataset.theme = cur === 'auto' ? 'light' : cur === 'light' ? 'dark' : 'auto';
    try { localStorage.setItem('lc-theme', html.dataset.theme); } catch (_) {}
  };
  try {
    const saved = localStorage.getItem('lc-theme');
    if (saved) document.documentElement.dataset.theme = saved;
  } catch (_) {}

  document.addEventListener('click', async e => {
    const pick = e.target.closest('[data-pick]');
    if (pick) {
      const input = $('#' + pick.dataset.pick);
      try {
        const { path } = await call('fs.pick_dir', { initial: input.value });
        if (path) {
          input.value = path;
          if (pick.dataset.pick === 'rename-dir') doRenameScan();
        }
      } catch (err) { toast('error', '选择文件夹失败', err.message); }
      return;
    }

    const chip = e.target.closest('.chip');
    if (chip) {
      const input = $('#' + (chip.dataset.insert || chip.dataset.set));
      if (!input) return;
      if (chip.dataset.set) { input.value = chip.dataset.token; return; }
      const s = input.selectionStart ?? input.value.length;
      const t = chip.dataset.token;
      input.value = input.value.slice(0, s) + t + input.value.slice(input.selectionEnd ?? s);
      input.focus();
      input.setSelectionRange(s + t.length, s + t.length);
      return;
    }

    const seg = e.target.closest('.seg');
    if (seg) {
      $$('.seg', seg.parentElement).forEach(x => x.classList.remove('is-active'));
      seg.classList.add('is-active');
      if (seg.parentElement.id === 'preview-policy') await setPolicy(seg.dataset.value);
      return;
    }

    const fmt = e.target.closest('[data-fmt]');
    if (fmt) {
      convertTarget = fmt.dataset.fmt;
      renderFormats();
      return;
    }

    const exp = e.target.closest('[data-export]');
    if (exp) { await doExport(exp.dataset.export); return; }

    const toggle = e.target.closest('[data-toggle]');
    if (toggle && !toggle.classList.contains('disabled')) {
      await toggleRow(parseInt(toggle.dataset.toggle, 10));
      return;
    }

    const rb = e.target.closest('[data-rollback]');
    if (rb) await runRollback(rb.dataset.rollback);
  });

  $('#btn-rename-preview').onclick = () => doPreview('rename');
  $('#btn-organize-preview').onclick = () => doPreview('organize');
  $('#btn-organize-scan').onclick = doScan;
  $('#btn-convert-preview').onclick = doConvertPreview;
  $('#btn-dedup-scan').onclick = doDedupScan;
  $('#btn-dedup-preview').onclick = doDedupPreview;
  $('#btn-summary-file').onclick = async () => {
    try {
      const { path } = await call('fs.pick_file', { initial: val('#summary-path') });
      if (path) { $('#summary-path').value = path; doSummarize(); }
    } catch (e) { toast('error', '选择文件失败', e.message); }
  };
  $('#btn-summary-batch').onclick = doBatchSummary;

  const thr = $('#dedup-threshold');
  if (thr) thr.oninput = () => { $('#dedup-threshold-val').textContent = thr.value + '%'; };
  $('#btn-execute').onclick = doExecute;
  $('#btn-preview-dry').onclick = doDryRun;
  $('#btn-back-edit').onclick = () => {
    const back = S.plan && S.plan.category === 'organize' ? 'organize' : 'rename';
    setStep(back, 1);
    showView(back);
  };

  $('#btn-preview-all').onclick = () => setAll(true);
  $('#btn-preview-none').onclick = () => setAll(false);
  $('#btn-preview-invert').onclick = invertAll;
  $('#preview-only-conflicts').onchange = e => { S.onlyConflicts = e.target.checked; drawRows(); };
  $('#btn-preview-csv').onclick = exportCsv;

  $('#btn-history-refresh').onclick = refreshHistory;
  $('#btn-history-dir').onclick = async () => {
    try {
      const info = await call('app.info');
      await call('fs.reveal', { path: info.journal_dir });
    } catch (e) { toast('error', '无法打开', e.message); }
  };
  $('#btn-purge-journals').onclick = async () => {
    const ok = await modal({
      title: '清理旧日志', confirmText: '清理',
      bodyHtml: '<p>删除 90 天前的操作日志。</p><p><span class="danger-text">这些操作将无法再回退。</span></p>',
    });
    if (!ok) return;
    try {
      const r = await call('history.purge_journals', { days: 90 });
      toast('ok', '清理完成', `已删除 ${r.removed} 份旧日志。`);
      refreshHistory();
    } catch (e) { toast('error', '清理失败', e.message); }
  };
  $('#btn-purge-trash').onclick = async () => {
    const ok = await modal({
      title: '清空回收站', confirmText: '清空', danger: true,
      bodyHtml: `<p>回收站里保存的是被覆盖的旧文件与撤销产物，用于把操作完整还原。</p>
        <p><span class="danger-text">清空后这些内容永久丢失</span>，相关操作将无法完整回退。</p>`,
    });
    if (!ok) return;
    try {
      const r = await call('history.purge_trash');
      toast('ok', '已清空', `删除 ${r.removed} 个条目，释放 ${r.freed}。`);
      refreshHistory();
    } catch (e) { toast('error', '清空失败', e.message); }
  };

  $('#btn-cancel-task').onclick = async () => {
    if (!S.jobId) return;
    try {
      await call('job.cancel', { job_id: S.jobId });
      $('#btn-cancel-task').disabled = true;
      $('#task-label').textContent = '正在取消…';
    } catch (e) { toast('error', '取消失败', e.message); }
  };

  $('#btn-quit').onclick = async () => {
    const ok = await modal({
      title: '退出 LocalCraft', icon: 'ask', confirmText: '退出', cancelText: '留下',
      bodyHtml: '<p>所有数据都保存在本机，退出不会丢失任何东西。</p>' +
                '<p>下次启动仍可回退今天做过的操作。</p>',
    });
    if (!ok) return;
    try { await call('app.quit'); } catch (_) {}
    document.body.innerHTML =
      `<div class="empty" style="height:100vh;justify-content:center">
         <h3>LocalCraft 已退出</h3><p>可以直接关闭这个窗口。</p></div>`;
    setTimeout(() => window.close(), 400);
  };

  document.addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      e.preventDefault();
      if (S.view === 'rename') doPreview('rename');
      else if (S.view === 'organize') doPreview('organize');
      else if (S.view === 'preview') doExecute();
    }
    if (e.key === 'Escape' && S.view === 'preview') {
      const back = S.plan && S.plan.category === 'organize' ? 'organize' : 'rename';
      showView(back);
    }
  });

  ['#rename-dir'].forEach(sel => {
    const el = $(sel);
    if (el) el.addEventListener('change', doRenameScan);
  });

  setInterval(() => call('app.beat', {}).catch(() => {}), 8000);
}

/* ============================================================ 列表操作 -- */
async function toggleRow(index) {
  try { renderPreview(await call('plan.toggle', { plan_id: S.planId, index }), S.source); }
  catch (e) { toast('warn', '无法切换', e.message); }
}
async function setAll(selected) {
  try { renderPreview(await call('plan.set_all', { plan_id: S.planId, selected }), S.source); }
  catch (e) { toast('error', '操作失败', e.message); }
}
async function invertAll() {
  if (!S.plan) return;
  const rows = (S.plan.rows || []).slice();
  let last = await call('plan.set_all', { plan_id: S.planId, selected: false });
  for (const r of rows) {
    if (r.level === 'error') continue;
    if (r.selected !== '✓') last = await call('plan.toggle', { plan_id: S.planId, index: r.index - 1 });
  }
  renderPreview(last, S.source);
}

async function setPolicy(policy) {
  if (!S.planId) return;
  S.policy = policy;
  try {
    renderPreview(await call('plan.set_policy', { plan_id: S.planId, policy }), S.source);
    toast('info', '已切换冲突策略', {
      rename: '同名时自动加 (1)(2) 后缀，绝不覆盖。',
      overwrite: '旧文件先移入回收站，再写入新文件（可回退）。',
      skip: '遇到同名文件直接跳过该行。',
      ask: '同名项标红，由你逐行决定。',
    }[policy] || '', 4000);
  } catch (e) { toast('error', '切换失败', e.message); }
}

async function exportCsv() {
  if (!S.planId) return;
  try {
    const { path } = await call('fs.pick_save', { filename: `localcraft-${S.planId}.csv` });
    if (!path) return;
    const r = await call('plan.export_csv', { plan_id: S.planId, path });
    if (r.cancelled) return;
    toast('ok', '已导出 CSV', r.path, 7000);
  } catch (e) { toast('error', '导出失败', e.message); }
}

/* ================================================================ 启动 -- */
(async function boot() {
  /* 通知 index.html 的兜底遮罩：界面起来了，别显示错误页 */
  window.__lcBooted = true;
  try {
    bind();
    renderFormats();
    showView('rename');
    setStep('rename', 1);
    setStep('organize', 1);
    setStep('convert', 1);
    try { await loadAppInfo(); await refreshHistory(); }
    catch (e) { toast('error', '无法连接后端', e.message, 10000); }
  } catch (e) {
    /* 早期就抛错的话，把详细堆栈也写到遮罩上，方便定位 */
    window.__lcBooted = false;
    const el = document.getElementById('boot-error');
    if (el) {
      const u = document.getElementById('boot-url');
      if (u) u.textContent = location.href + '\n\n' + (e && e.stack ? e.stack : e);
      el.style.display = 'block';
    }
    throw e;
  }
})();
