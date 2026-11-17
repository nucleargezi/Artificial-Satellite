// ==UserScript==
// @name         QOJ to CPH-NG (Contest & Attachment Importer)
// @namespace    https://github.com/yorisou/yrs-cli
// @version      1.0.0
// @description  在 QOJ 比赛页一键整场导入 CPH-NG, 单题页免切语言直接从附件 ZIP 极速提取全套大小样例
// @match        https://qoj.ac/contest/*
// @match        https://qoj.ac/problem/*
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// @connect      localhost
// @run-at       document-end
// ==/UserScript==

(function () {
  'use strict';

  const CPH_PORT = 27121;
  const CONCURRENCY = 6;
  const MAX_ZIP_BYTES = 32 * 1024 * 1024;
  const MAX_SINGLE_SAMPLE_BYTES = 8 * 1024 * 1024;
  const FALLBACK_TO_STATEMENT = true;

  const utf8Decoder = new TextDecoder('utf-8');
  const naturalCollator = new Intl.Collator(undefined, {
    numeric: true,
    sensitivity: 'base',
  });

  function generateUuid() {
    if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
      return crypto.randomUUID();
    }
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
      const r = (Math.random() * 16) | 0;
      const v = c === 'x' ? r : (r & 0x3) | 0x8;
      return v.toString(16);
    });
  }

  function normalizeText(s) {
    return String(s || '').replace(/\r\n/g, '\n');
  }

  function normalizeProblemUrl(urlStr) {
    try {
      const u = new URL(urlStr);
      const m = u.pathname.match(/^(\/(?:contest\/\d+\/)?problem\/\d+)/);
      if (m) {
        return `${u.origin}${m[1]}`;
      }
      return `${u.origin}${u.pathname}`;
    } catch {
      return String(urlStr || '');
    }
  }

  // --- Pure Native ZIP Reader (Central Directory + DecompressionStream) ---

  function parseZipEntries(u8) {
    if (!u8 || u8.length < 22) return null;
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);

    const minEocd = Math.max(0, u8.length - 65557);
    let eocdOffset = -1;
    for (let i = u8.length - 22; i >= minEocd; --i) {
      if (dv.getUint32(i, true) === 0x06054b50) {
        const commentLen = dv.getUint16(i + 20, true);
        if (i + 22 + commentLen === u8.length) {
          eocdOffset = i;
          break;
        }
      }
    }
    if (eocdOffset < 0) return null;

    const diskNum = dv.getUint16(eocdOffset + 4, true);
    const cdDisk = dv.getUint16(eocdOffset + 6, true);
    const totalEntries = dv.getUint16(eocdOffset + 10, true);
    const cdSize = dv.getUint32(eocdOffset + 12, true);
    const cdOffset = dv.getUint32(eocdOffset + 16, true);

    if (
      diskNum !== 0 ||
      cdDisk !== 0 ||
      totalEntries === 0xffff ||
      cdSize === 0xffffffff ||
      cdOffset === 0xffffffff ||
      cdOffset + cdSize > eocdOffset
    ) {
      return null;
    }

    const entries = [];
    let ptr = cdOffset;
    for (let i = 0; i < totalEntries; ++i) {
      if (ptr + 46 > cdOffset + cdSize) return null;
      if (dv.getUint32(ptr, true) !== 0x02014b50) return null;

      const flags = dv.getUint16(ptr + 8, true);
      const method = dv.getUint16(ptr + 10, true);
      const compSize = dv.getUint32(ptr + 20, true);
      const uncompSize = dv.getUint32(ptr + 24, true);
      const nameLen = dv.getUint16(ptr + 28, true);
      const extraLen = dv.getUint16(ptr + 30, true);
      const commentLen = dv.getUint16(ptr + 32, true);
      const lfhOffset = dv.getUint32(ptr + 42, true);

      const headerEnd = ptr + 46 + nameLen + extraLen + commentLen;
      if (headerEnd > cdOffset + cdSize) return null;

      const name = utf8Decoder
        .decode(u8.subarray(ptr + 46, ptr + 46 + nameLen))
        .replace(/\\/g, '/');
      ptr = headerEnd;

      const base = name.split('/').pop() || '';
      if (
        !name ||
        name.endsWith('/') ||
        name.startsWith('__MACOSX/') ||
        name.includes('/__MACOSX/') ||
        base.startsWith('.')
      ) {
        continue;
      }

      if ((flags & 0x01) !== 0) continue;
      if (method !== 0 && method !== 8) continue;
      if (compSize === 0xffffffff || uncompSize === 0xffffffff) continue;

      if (lfhOffset + 30 > cdOffset) continue;
      if (dv.getUint32(lfhOffset, true) !== 0x04034b50) continue;

      const lfhNameLen = dv.getUint16(lfhOffset + 26, true);
      const lfhExtraLen = dv.getUint16(lfhOffset + 28, true);
      const dataStart = lfhOffset + 30 + lfhNameLen + lfhExtraLen;
      const dataEnd = dataStart + compSize;
      if (dataEnd > cdOffset) continue;

      entries.push({
        name,
        method,
        compSize,
        uncompSize,
        dataStart,
        dataEnd,
      });
    }

    return entries;
  }

  function replacePathDirSegment(lowerPath, fromWords, toWord) {
    const parts = lowerPath.split('/');
    const results = [];
    for (let i = 0; i < parts.length - 1; ++i) {
      if (fromWords.includes(parts[i])) {
        const copy = parts.slice();
        copy[i] = toWord;
        results.push(copy.join('/'));
      }
    }
    return results;
  }

  function pairSampleEntries(entries) {
    if (!entries || entries.length === 0) return [];

    const byLower = new Map();
    for (const e of entries) {
      if (e.uncompSize <= MAX_SINGLE_SAMPLE_BYTES) {
        byLower.set(e.name.toLowerCase(), e);
      }
    }

    const pairs = [];
    const usedAnswers = new Set();

    // Strategy 1: *.in matched with *.ans / *.out
    for (const entry of entries) {
      if (entry.uncompSize > MAX_SINGLE_SAMPLE_BYTES) continue;
      const lower = entry.name.toLowerCase();
      if (!lower.endsWith('.in')) continue;

      const lowerStem = lower.slice(0, -3);
      const candidates = [`${lowerStem}.ans`, `${lowerStem}.out`];

      for (const outDir of ['out', 'ans', 'output', 'outputs']) {
        const altStems = replacePathDirSegment(
          lowerStem,
          ['in', 'input', 'inputs'],
          outDir,
        );
        for (const alt of altStems) {
          candidates.push(`${alt}.ans`, `${alt}.out`, `${alt}.txt`);
        }
      }

      for (const cand of candidates) {
        const ansEntry = byLower.get(cand);
        if (ansEntry && ansEntry !== entry && !usedAnswers.has(ansEntry.name)) {
          usedAnswers.add(ansEntry.name);
          pairs.push({ inEntry: entry, ansEntry });
          break;
        }
      }
    }

    // Strategy 2: fallback for *.txt sample naming
    if (pairs.length === 0) {
      for (const entry of entries) {
        if (entry.uncompSize > MAX_SINGLE_SAMPLE_BYTES) continue;
        const lower = entry.name.toLowerCase();
        if (!lower.endsWith('.txt')) continue;

        const candidates = [];
        for (const outDir of ['out', 'ans', 'output', 'outputs', 'answer', 'answers']) {
          candidates.push(
            ...replacePathDirSegment(
              lower,
              ['in', 'input', 'inputs'],
              outDir,
            ),
          );
        }

        const slash = lower.lastIndexOf('/');
        const dir = slash >= 0 ? lower.slice(0, slash + 1) : '';
        const base = slash >= 0 ? lower.slice(slash + 1) : lower;
        if (/(^|[_\-.])input([_\-.]|$)/.test(base)) {
          for (const rep of ['output', 'answer', 'out', 'ans']) {
            candidates.push(
              dir + base.replace(/(^|[_\-.])input(?=[_\-.]|$)/g, `$1${rep}`),
            );
          }
        } else if (/(^|[_\-.])in([_\-.]|$)/.test(base)) {
          for (const rep of ['out', 'ans', 'output', 'answer']) {
            candidates.push(
              dir + base.replace(/(^|[_\-.])in(?=[_\-.]|$)/g, `$1${rep}`),
            );
          }
        }

        for (const cand of candidates) {
          const ansEntry = byLower.get(cand);
          if (ansEntry && ansEntry !== entry && !usedAnswers.has(ansEntry.name)) {
            usedAnswers.add(ansEntry.name);
            pairs.push({ inEntry: entry, ansEntry });
            break;
          }
        }
      }
    }

    pairs.sort((a, b) =>
      naturalCollator.compare(a.inEntry.name, b.inEntry.name),
    );
    return pairs;
  }

  async function inflateEntry(u8, entry) {
    const slice = u8.subarray(entry.dataStart, entry.dataEnd);
    if (entry.method === 0) {
      return slice;
    }
    if (entry.method === 8) {
      try {
        const stream = new Blob([slice])
          .stream()
          .pipeThrough(new DecompressionStream('deflate-raw'));
        const buf = await new Response(stream).arrayBuffer();
        return new Uint8Array(buf);
      } catch {
        return null;
      }
    }
    return null;
  }

  async function readZipBytesWithLimit(res) {
    const clHeader = res.headers && res.headers.get ? res.headers.get('content-length') : null;
    if (clHeader) {
      const cl = Number(clHeader);
      if (Number.isFinite(cl) && cl > MAX_ZIP_BYTES) {
        if (res.body && typeof res.body.cancel === 'function') {
          await res.body.cancel().catch(() => {});
        }
        return null;
      }
    }

    if (!res.body || typeof res.body.getReader !== 'function') {
      const buf = await res.arrayBuffer();
      if (buf.byteLength > MAX_ZIP_BYTES || buf.byteLength < 22) return null;
      const u8 = new Uint8Array(buf);
      if (u8[0] !== 0x50 || u8[1] !== 0x4b) return null;
      return u8;
    }

    const reader = res.body.getReader();
    const chunks = [];
    let total = 0;
    let checkedMagic = false;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!value || value.byteLength === 0) continue;

      total += value.byteLength;
      if (total > MAX_ZIP_BYTES) {
        await reader.cancel().catch(() => {});
        return null;
      }
      chunks.push(value);

      if (!checkedMagic && total >= 2) {
        let b0, b1;
        if (chunks[0].byteLength >= 2) {
          b0 = chunks[0][0];
          b1 = chunks[0][1];
        } else {
          b0 = chunks[0][0];
          b1 = chunks[1][0];
        }
        checkedMagic = true;
        if (b0 !== 0x50 || b1 !== 0x4b) {
          await reader.cancel().catch(() => {});
          return null;
        }
      }
    }

    if (total < 22) return null;
    const merged = new Uint8Array(total);
    let offset = 0;
    for (const c of chunks) {
      merged.set(c, offset);
      offset += c.byteLength;
    }
    return merged;
  }

  async function fetchSampleText(url) {
    try {
      const res = await fetch(url, { credentials: 'include' });
      if (!res.ok) return null;
      const clHeader = res.headers && res.headers.get ? res.headers.get('content-length') : null;
      if (clHeader && Number(clHeader) > MAX_SINGLE_SAMPLE_BYTES) {
        if (res.body && typeof res.body.cancel === 'function') {
          await res.body.cancel().catch(() => {});
        }
        return null;
      }
      const text = await res.text();
      if (text.length > MAX_SINGLE_SAMPLE_BYTES) return null;
      return normalizeText(text);
    } catch {
      return null;
    }
  }

  async function extractTestsFromAttachmentPage(attDoc, pageUrl) {
    const anchors = Array.from(attDoc.querySelectorAll('a[href]'));
    const zipUrls = [];
    const looseEntries = [];
    const seen = new Set();

    for (const a of anchors) {
      const rawHref = (a.getAttribute('href') || '').trim();
      if (!rawHref || rawHref.startsWith('#') || rawHref.startsWith('javascript:')) {
        continue;
      }
      let absUrl;
      try {
        absUrl = new URL(rawHref, pageUrl).href;
      } catch {
        continue;
      }
      if (absUrl === pageUrl || seen.has(absUrl)) continue;
      seen.add(absUrl);

      const u = new URL(absUrl);
      const lowerPath = u.pathname.toLowerCase();
      const lowerHref = absUrl.toLowerCase();

      if (
        lowerPath.endsWith('.zip') ||
        /\.zip$/i.test((a.textContent || '').trim()) ||
        (lowerPath.endsWith('/download.php') &&
          !/type=(?:statement|solution|testlib)\b/i.test(u.search))
      ) {
        zipUrls.push(absUrl);
        continue;
      }

      if (/\.(?:in|ans|out|txt)$/i.test(lowerPath)) {
        const cleanName = decodeURIComponent(u.pathname.replace(/^\/+/, ''));
        looseEntries.push({
          name: cleanName || (a.textContent || '').trim(),
          url: absUrl,
          uncompSize: 0,
        });
      } else if (/\.(?:in|ans|out|txt)$/i.test((a.textContent || '').trim())) {
        looseEntries.push({
          name: (a.textContent || '').trim(),
          url: absUrl,
          uncompSize: 0,
        });
      }
      void lowerHref;
    }

    for (const zUrl of zipUrls) {
      const tests = await fetchAttachmentFromUrl(zUrl, true);
      if (tests && tests.length > 0) return tests;
    }

    if (looseEntries.length > 0) {
      const pairs = pairSampleEntries(looseEntries);
      if (pairs.length > 0) {
        const tests = [];
        for (const { inEntry, ansEntry } of pairs) {
          const [inp, ans] = await Promise.all([
            fetchSampleText(inEntry.url),
            fetchSampleText(ansEntry.url),
          ]);
          if (inp !== null && ans !== null) {
            tests.push({ input: inp, output: ans });
          }
        }
        if (tests.length > 0) return tests;
      }
    }

    return null;
  }

  async function fetchAttachmentFromUrl(url, zipOnly = false) {
    try {
      const res = await fetch(url, { credentials: 'include' });
      if (!res.ok) return null;

      const ctype = ((res.headers && res.headers.get && res.headers.get('content-type')) || '').toLowerCase();
      const isAttachPage = !zipOnly && !/\/download\.php/i.test(url) && ctype.includes('text/html');
      if (isAttachPage) {
        const html = await res.text();
        const attDoc = new DOMParser().parseFromString(html, 'text/html');
        return await extractTestsFromAttachmentPage(attDoc, url);
      }

      const u8 = await readZipBytesWithLimit(res);
      if (!u8) return null;
      return await extractTestsFromZipBytes(u8);
    } catch {
      return null;
    }
  }

  function findAttachmentUrlsInDoc(doc, pid, baseUrl) {
    const candidates = [];
    const origin = (() => {
      try {
        return new URL(baseUrl).origin;
      } catch {
        return location.origin;
      }
    })();

    if (doc && doc.querySelectorAll) {
      const links = Array.from(doc.querySelectorAll('a[href]'));
      for (const a of links) {
        const rawHref = (a.getAttribute('href') || '').trim();
        if (!rawHref || rawHref.startsWith('#') || rawHref.startsWith('javascript:')) {
          continue;
        }
        let abs;
        try {
          const u = new URL(rawHref, baseUrl);
          if (u.origin !== origin) continue;
          abs = u.href;
        } catch {
          continue;
        }

        const lower = abs.toLowerCase();
        const text = (a.textContent || '').trim();
        if (
          /type=(?:statement|solution|testlib)/i.test(lower) ||
          /\/(?:manage|statistics|submissions|hacks|statement)(?:\/|$)/i.test(lower)
        ) {
          continue;
        }

        let score = 0;
        if (/download\.php/i.test(lower) && /type=attachments?\b/i.test(lower)) {
          score = 10;
        } else if (/\/problem\/\d+\/attachments(?:\/|$|\?)/i.test(lower)) {
          score = 9;
        } else if (/download\.php/i.test(lower) && /type=problem\b/i.test(lower)) {
          score = 8;
        } else if (/\.zip(?:$|\?)/i.test(lower)) {
          score = 7;
        } else if (/^(?:attachments?|下发文件|附加文件|附件下载|附件)$/i.test(text)) {
          score = 6;
        }

        if (score > 0) {
          candidates.push({ url: abs, score });
        }
      }
    }

    candidates.sort((a, b) => b.score - a.score);
    const ordered = [];
    const seen = new Set();
    for (const c of candidates) {
      if (!seen.has(c.url)) {
        seen.add(c.url);
        ordered.push(c.url);
      }
    }

    for (const fallback of [
      `${origin}/download.php?type=attachments&id=${encodeURIComponent(pid)}`,
      `${origin}/download.php?type=problem&id=${encodeURIComponent(pid)}`,
    ]) {
      if (!seen.has(fallback)) {
        seen.add(fallback);
        ordered.push(fallback);
      }
    }
    return ordered;
  }

  async function resolveAndFetchAttachmentTests(doc, pid, baseUrl) {
    const urls = findAttachmentUrlsInDoc(doc, pid, baseUrl);
    for (const url of urls) {
      const tests = await fetchAttachmentFromUrl(url, false);
      if (tests && tests.length > 0) return tests;
    }
    return null;
  }

  async function fetchAttachmentTests(pid, baseOrigin = location.origin) {
    return await resolveAndFetchAttachmentTests(null, pid, `${baseOrigin}/problem/${pid}`);
  }

  async function extractTestsFromZipBytes(u8) {
    const entries = parseZipEntries(u8);
    if (!entries) return null;

    const pairs = pairSampleEntries(entries);
    if (pairs.length === 0) return [];

    const tests = [];
    for (const { inEntry, ansEntry } of pairs) {
      const [inBytes, ansBytes] = await Promise.all([
        inflateEntry(u8, inEntry),
        inflateEntry(u8, ansEntry),
      ]);
      if (!inBytes || !ansBytes) continue;
      tests.push({
        input: normalizeText(utf8Decoder.decode(inBytes)),
        output: normalizeText(utf8Decoder.decode(ansBytes)),
      });
    }
    return tests;
  }

  // --- QOJ DOM / Statement Parsers ---

  function parseLimitsFromDoc(doc) {
    let timeLimit = 1000;
    let memoryLimit = 512;

    const content = doc.querySelector('.uoj-content') || doc.body || doc;
    const candidates = [];
    const nodes = content.querySelectorAll
      ? content.querySelectorAll('.badge, p, script[type*="math/tex"], div.row')
      : [];
    for (const el of nodes) {
      if (el && el.textContent) candidates.push(el.textContent);
    }
    if (content.textContent) {
      candidates.push(content.textContent.slice(0, 2000));
    }

    const timeRe =
      /(?:时间限制|Time\s*Limit)\s*[:：]?\s*([0-9]+(?:\.[0-9]+)?)\s*(ms|毫秒|sec|seconds?|s|秒)/i;
    const memRe =
      /(?:空间限制|内存限制|Memory\s*Limit)\s*[:：]?\s*([0-9]+(?:\.[0-9]+)?)\s*(GiB|GB|G|MiB|MB|M|KiB|KB|K)/i;

    for (const text of candidates) {
      const tm = text.match(timeRe);
      if (tm) {
        const val = parseFloat(tm[1]);
        const unit = tm[2].toLowerCase();
        if (Number.isFinite(val) && val > 0) {
          timeLimit =
            unit === 'ms' || unit === '毫秒'
              ? Math.round(val)
              : Math.round(val * 1000);
          break;
        }
      }
    }

    for (const text of candidates) {
      const mm = text.match(memRe);
      if (mm) {
        const val = parseFloat(mm[1]);
        const unit = mm[2].toLowerCase();
        if (Number.isFinite(val) && val > 0) {
          if (unit.startsWith('g')) {
            memoryLimit = Math.round(val * 1024);
          } else if (unit.startsWith('k')) {
            memoryLimit = Math.max(1, Math.round(val / 1024));
          } else {
            memoryLimit = Math.round(val);
          }
          break;
        }
      }
    }

    return { timeLimit, memoryLimit };
  }

  function extractTitleFromDoc(doc, fallbackName) {
    if (fallbackName && fallbackName.trim()) {
      return fallbackName.trim().replace(/\s+/g, ' ');
    }
    const headerRow = doc.querySelector('.uoj-content .page-header.row, .page-header.row');
    if (headerRow) {
      const centerH1 = headerRow.querySelector('h1.text-center, h1:nth-of-type(2)');
      if (centerH1 && centerH1.textContent.trim()) {
        return centerH1.textContent.trim().replace(/\s+/g, ' ');
      }
    }
    const pageHeader = doc.querySelector('.uoj-content h1.page-header, h1.page-header');
    if (pageHeader && pageHeader.textContent.trim()) {
      return pageHeader.textContent.trim().replace(/\s+/g, ' ');
    }
    const h1 = doc.querySelector('.uoj-content h1');
    if (h1 && h1.textContent.trim()) {
      return h1.textContent.trim().replace(/\s+/g, ' ');
    }
    return String(doc.title || 'QOJ Problem')
      .replace(/\s*-\s*Problem\s*-\s*.*$/i, '')
      .trim();
  }

  function getElementClass(el) {
    if (!el) return '';
    if (typeof el.className === 'string') return el.className;
    if (el.getAttribute) return el.getAttribute('class') || '';
    return '';
  }

  function isCodeSnippetPre(pre, prevTitleKind) {
    if (
      pre.closest &&
      (pre.closest('#tab-submit-answer') || pre.closest('#tab-custom-test'))
    ) {
      return true;
    }
    const preCls = getElementClass(pre);
    if (/(?:^|\s)(?:sh_sourceCode|hljs|prettyprint|source-code)(?:\s|$)/i.test(preCls)) {
      return true;
    }
    const codeEl = pre.querySelector ? pre.querySelector('code') : null;
    if (codeEl) {
      const codeCls = getElementClass(codeEl);
      const langMatch = codeCls.match(/(?:^|\s)(?:language|lang)-([a-z0-9_+-]+)/i);
      if (langMatch) {
        const lang = langMatch[1].toLowerCase();
        if (!['plain', 'text', 'plaintext', 'txt', 'none'].includes(lang)) {
          return true;
        }
      }
      if (/(?:^|\s)(?:hljs|sh_\w+|cpp|c\+\+|python|java|rust)(?:\s|$)/i.test(codeCls)) {
        return true;
      }
    }
    if (prevTitleKind !== 'input') {
      const trimmed = String(pre.textContent || '').trimStart();
      if (/^#\s*include\s*[<"]/.test(trimmed)) {
        return true;
      }
    }
    return false;
  }

  function pairPresSequentially(list) {
    const tests = [];
    for (let i = 0; i + 1 < list.length; i += 2) {
      tests.push({ input: list[i], output: list[i + 1] });
    }
    return tests;
  }

  function extractPreTestsFromDoc(doc) {
    const scope =
      doc.querySelector('#tab-statement') ||
      doc.querySelector('.uoj-content article') ||
      doc.querySelector('.uoj-content');
    if (!scope) return [];

    const nodes = Array.from(
      scope.querySelectorAll('h1, h2, h3, h4, h5, h6, strong, b, dt, .panel-heading, pre'),
    );

    const INPUT_TITLE_RE =
      /^(?:(?:sample|example|样例)\s*#?\d*\s*)?(?:input|stdin|输入)\s*(?:#?\d+)?\s*[:：]?$/i;
    const OUTPUT_TITLE_RE =
      /^(?:(?:sample|example|样例)\s*#?\d*\s*)?(?:output|stdout|answer|输出)\s*(?:#?\d+)?\s*[:：]?$/i;
    const STOP_SECTION_RE =
      /^(?:explanation|note|notes|hint|hints|prompt|提示|说明|解释|样例解释|样例说明)(?:\s|$|[:：])/i;
    const SAMPLE_SECTION_RE =
      /(?:^|\b)(?:samples?|examples?)(?:\b|\d|$)|样例/i;

    let seenSampleSection = false;
    let inActiveSample = false;
    let pendingKind = null;
    let pendingInput = null;
    let lastHeadingText = null;
    const anchoredTests = [];
    const sectionPres = [];
    const allValidPres = [];

    for (const el of nodes) {
      const tag = (el.tagName || '').toUpperCase();
      if (tag === 'PRE') {
        lastHeadingText = null;
        if (isCodeSnippetPre(el, pendingKind)) continue;
        const text = normalizeText(el.textContent);
        allValidPres.push(text);
        if (inActiveSample) {
          sectionPres.push(text);
        }
        if (pendingKind === 'input') {
          pendingInput = text;
          pendingKind = null;
        } else if (pendingKind === 'output' && pendingInput !== null) {
          anchoredTests.push({ input: pendingInput, output: text });
          pendingInput = null;
          pendingKind = null;
        }
        continue;
      }

      if (el.closest && el.closest('pre')) continue;

      const headingText = String(el.textContent || '')
        .trim()
        .replace(/\s+/g, ' ');
      if (!headingText || headingText === lastHeadingText) continue;
      lastHeadingText = headingText;

      if (INPUT_TITLE_RE.test(headingText)) {
        pendingKind = 'input';
        seenSampleSection = true;
        inActiveSample = true;
      } else if (OUTPUT_TITLE_RE.test(headingText)) {
        pendingKind = 'output';
        seenSampleSection = true;
        inActiveSample = true;
      } else if (STOP_SECTION_RE.test(headingText)) {
        inActiveSample = false;
        pendingKind = null;
      } else if (
        SAMPLE_SECTION_RE.test(headingText) &&
        !/解释|说明|explanation/i.test(headingText)
      ) {
        seenSampleSection = true;
        inActiveSample = true;
        pendingKind = null;
      } else if (/^H[1-4]$/.test(tag)) {
        inActiveSample = false;
        pendingKind = null;
      }
    }

    if (anchoredTests.length > 0) return anchoredTests;
    if (seenSampleSection && sectionPres.length >= 2) {
      return pairPresSequentially(sectionPres);
    }
    if (!seenSampleSection && allValidPres.length >= 2) {
      return pairPresSequentially(allValidPres);
    }
    return [];
  }

  function findNonPdfStatementUrl(doc, baseUrl) {
    const links = Array.from(doc.querySelectorAll('a[href*="/statement/"]'));
    const scored = [];
    for (const a of links) {
      const href = a.getAttribute('href') || '';
      const text = (a.textContent || '').trim();
      if (/pdf/i.test(href) || /pdf/i.test(text)) continue;

      let abs;
      try {
        abs = new URL(href, baseUrl).href;
      } catch {
        continue;
      }

      let score = 0;
      if (/zh/i.test(href) || /中文/.test(text)) score = 3;
      else if (/en/i.test(href) || /english/i.test(text)) score = 2;
      else score = 1;
      scored.push({ abs, score });
    }
    scored.sort((a, b) => b.score - a.score);
    return scored.length > 0 ? scored[0].abs : null;
  }

  async function fetchHtmlDoc(url) {
    const res = await fetch(url, { credentials: 'include' });
    if (!res.ok) return null;
    const html = await res.text();
    return new DOMParser().parseFromString(html, 'text/html');
  }

  function extractContestTitle(doc) {
    const h1 =
      doc.querySelector('.uoj-content .text-center > h1') ||
      doc.querySelector('.uoj-content h1');
    if (h1 && h1.textContent.trim()) {
      return h1.textContent.trim().replace(/\s+/g, ' ');
    }
    return String(doc.title || 'QOJ Contest')
      .replace(/\s*-\s*(?:Dashboard|Contest).*$/i, '')
      .trim();
  }

  function extractContestProblemsFromDashboard(doc, cid, baseUrl = location.href) {
    const contestTitle = extractContestTitle(doc);
    const tables = Array.from(doc.querySelectorAll('.uoj-content table, table'));
    let problemRows = null;

    for (const table of tables) {
      const links = table.querySelectorAll(
        `a[href*="/contest/${cid}/problem/"], a[href*="/problem/"]`,
      );
      if (links.length > 0) {
        problemRows = Array.from(table.querySelectorAll('tbody tr'));
        if (problemRows.length === 0) {
          problemRows = Array.from(table.querySelectorAll('tr')).slice(1);
        }
        break;
      }
    }

    if (!problemRows || problemRows.length === 0) {
      return { contestTitle, problems: [] };
    }

    const items = [];
    for (let i = 0; i < problemRows.length; ++i) {
      const tr = problemRows[i];
      const link =
        tr.querySelector(`a[href*="/contest/${cid}/problem/"]`) ||
        tr.querySelector('a[href*="/problem/"]');
      if (!link) continue;

      const href = link.getAttribute('href') || '';
      const pm = href.match(/\/problem\/(\d+)/);
      if (!pm) continue;

      const pid = pm[1];
      let absUrl;
      try {
        absUrl = new URL(href, baseUrl).href;
      } catch {
        continue;
      }

      const tds = tr.querySelectorAll('td');
      let letter = tds.length > 0 ? tds[0].textContent.trim().replace(/\s+/g, '') : '';
      if (!letter || !/^[A-Za-z0-9]+$/.test(letter)) {
        letter = i < 26 ? String.fromCharCode(65 + i) : `P${i + 1}`;
      }

      const rawTitle = link.textContent.trim().replace(/\s+/g, ' ');
      const startsWithLetter = new RegExp(
        `^${letter.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?:[.:\\-–—\\s]|$)`,
        'i',
      ).test(rawTitle);
      const name = startsWithLetter || !rawTitle ? rawTitle || letter : `${letter}. ${rawTitle}`;

      items.push({
        pid,
        letter,
        name,
        problemUrl: normalizeProblemUrl(absUrl),
      });
    }

    return { contestTitle, problems: items };
  }

  async function buildProblemPayload({
    pid,
    problemUrl,
    fallbackName,
    group,
    docOrNull,
    batchId,
    batchSize,
  }) {
    const cleanUrl = normalizeProblemUrl(problemUrl);
    const doc = docOrNull || (await fetchHtmlDoc(cleanUrl).catch(() => null));
    const attachTests = await resolveAndFetchAttachmentTests(doc, pid, cleanUrl);

    const name = doc
      ? extractTitleFromDoc(doc, fallbackName)
      : fallbackName || `Problem ${pid}`;
    const { timeLimit, memoryLimit } = doc
      ? parseLimitsFromDoc(doc)
      : { timeLimit: 1000, memoryLimit: 512 };

    let tests = attachTests && attachTests.length > 0 ? attachTests : null;

    if (!tests && FALLBACK_TO_STATEMENT && doc) {
      const directPre = extractPreTestsFromDoc(doc);
      if (directPre.length > 0) {
        tests = directPre;
      } else {
        const altUrl = findNonPdfStatementUrl(doc, cleanUrl);
        if (altUrl) {
          const altDoc = await fetchHtmlDoc(altUrl).catch(() => null);
          if (altDoc) {
            const altPre = extractPreTestsFromDoc(altDoc);
            if (altPre.length > 0) tests = altPre;
          }
        }
      }
    }

    return {
      name,
      group: group || 'QOJ',
      url: cleanUrl,
      interactive: false,
      memoryLimit,
      timeLimit,
      tests: tests || [],
      testType: 'single',
      input: { type: 'stdin' },
      output: { type: 'stdout' },
      languages: {
        java: {
          mainClass: 'Main',
          taskClass: 'Task',
        },
      },
      batch: {
        id: batchId,
        size: batchSize,
      },
    };
  }

  function postToCph(payload) {
    return new Promise((resolve, reject) => {
      if (typeof GM_xmlhttpRequest !== 'function') {
        reject(new Error('GM_xmlhttpRequest unavailable'));
        return;
      }
      GM_xmlhttpRequest({
        method: 'POST',
        url: `http://127.0.0.1:${CPH_PORT}/`,
        headers: { 'Content-Type': 'application/json' },
        data: JSON.stringify(payload),
        timeout: 5000,
        onload(resp) {
          if (resp.status >= 200 && resp.status < 300) {
            resolve(resp);
          } else {
            reject(new Error(`CPH-NG HTTP ${resp.status}`));
          }
        },
        onerror() {
          reject(new Error(`连接 CPH-NG (:${CPH_PORT}) 失败, 请确认 VSCode 已开启`));
        },
        ontimeout() {
          reject(new Error(`连接 CPH-NG (:${CPH_PORT}) 超时`));
        },
      });
    });
  }

  async function runWithConcurrency(items, limit, worker) {
    const results = new Array(items.length);
    let next = 0;
    const runners = Array.from(
      { length: Math.min(limit, items.length) },
      async () => {
        while (true) {
          const idx = next++;
          if (idx >= items.length) break;
          results[idx] = await worker(items[idx], idx);
        }
      },
    );
    await Promise.all(runners);
    return results;
  }

  // --- UI Injection ---

  function makeButton(label, extraClass) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = `btn btn-sm ${extraClass || 'btn-success'}`;
    btn.style.marginLeft = '6px';
    btn.textContent = label;
    return btn;
  }

  function mountSingleProblemUi(pid) {
    const defaultText = '⚡ 导入 CPH';
    const btn = makeButton(defaultText, 'btn-success');

    const navTabs = document.querySelector('.uoj-content ul.nav-tabs, ul.nav-tabs');
    const statBtn = document.querySelector('a.btn[href*="/statistics"]');
    if (statBtn && statBtn.parentNode) {
      btn.classList.add('float-right');
      btn.style.marginRight = '6px';
      statBtn.parentNode.insertBefore(btn, statBtn);
    } else if (navTabs && navTabs.parentNode) {
      const li = document.createElement('li');
      li.className = 'nav-item ml-auto';
      li.style.display = 'flex';
      li.style.alignItems = 'center';
      li.appendChild(btn);
      navTabs.appendChild(li);
    } else {
      const root = document.querySelector('.uoj-content') || document.body;
      root.prepend(btn);
    }

    const trigger = async () => {
      if (btn.disabled) return;
      btn.disabled = true;
      btn.textContent = '⏳ 提取附件...';
      try {
        const contestLink = document.querySelector('.page-header.row h1 small');
        const group = contestLink
          ? `QOJ - ${contestLink.textContent.trim()}`
          : 'QOJ';
        const payload = await buildProblemPayload({
          pid,
          problemUrl: location.href,
          fallbackName: null,
          group,
          docOrNull: document,
          batchId: generateUuid(),
          batchSize: 1,
        });
        await postToCph(payload);
        btn.textContent =
          payload.tests.length > 0
            ? `✅ 已导入 (${payload.tests.length} 组样例)`
            : '✅ 已导入 (无样例)';
      } catch (err) {
        btn.textContent = `❌ ${err.message}`;
      } finally {
        setTimeout(() => {
          btn.disabled = false;
          btn.textContent = defaultText;
        }, 3000);
      }
    };

    btn.addEventListener('click', trigger);

    document.addEventListener('keydown', (e) => {
      if (!e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
      if (e.key.toLowerCase() !== 'q') return;
      const tag = (e.target && e.target.tagName) || '';
      if (tag === 'INPUT' || tag === 'TEXTAREA' || (e.target && e.target.isContentEditable)) {
        return;
      }
      e.preventDefault();
      trigger();
    });
  }

  function mountContestDashboardUi(cid) {
    const info = extractContestProblemsFromDashboard(document, cid);
    if (!info.problems || info.problems.length === 0) return;

    const defaultText = `📦 整场导入 CPH (${info.problems.length} 题)`;
    const btn = makeButton(defaultText, 'btn-success');

    const titleBox = document.querySelector('.uoj-content .text-center');
    const firstTable = document.querySelector('.uoj-content .table-responsive, .uoj-content table');
    if (titleBox) {
      const wrap = document.createElement('div');
      wrap.style.marginTop = '8px';
      wrap.appendChild(btn);
      titleBox.appendChild(wrap);
    } else if (firstTable && firstTable.parentNode) {
      firstTable.parentNode.insertBefore(btn, firstTable);
    } else {
      const root = document.querySelector('.uoj-content') || document.body;
      root.prepend(btn);
    }

    btn.addEventListener('click', async () => {
      if (btn.disabled) return;
      btn.disabled = true;
      const batchId = generateUuid();
      const batchSize = info.problems.length;
      const group = `QOJ - ${info.contestTitle}`;
      let done = 0;

      btn.textContent = `⏳ 拉取中 (0/${batchSize})...`;
      try {
        const payloads = await runWithConcurrency(
          info.problems,
          CONCURRENCY,
          async (prob) => {
            const p = await buildProblemPayload({
              pid: prob.pid,
              problemUrl: prob.problemUrl,
              fallbackName: prob.name,
              group,
              docOrNull: null,
              batchId,
              batchSize,
            });
            done += 1;
            btn.textContent = `⏳ 拉取中 (${done}/${batchSize})...`;
            return p;
          },
        );

        btn.textContent = `⏳ 推送 CPH (${batchSize} 题)...`;
        let totalSamples = 0;
        for (const payload of payloads) {
          totalSamples += payload.tests.length;
          await postToCph(payload);
        }
        btn.textContent = `✅ 已导入 ${batchSize} 题 (共 ${totalSamples} 组样例)`;
      } catch (err) {
        btn.textContent = `❌ ${err.message}`;
      } finally {
        setTimeout(() => {
          btn.disabled = false;
          btn.textContent = defaultText;
        }, 4000);
      }
    });
  }

  function init() {
    const path = location.pathname;
    const problemMatch = path.match(/^\/(?:contest\/\d+\/)?problem\/(\d+)/);
    if (
      problemMatch &&
      !/\/(?:manage|statistics|submissions|hacks)(?:\/|$)/.test(path)
    ) {
      mountSingleProblemUi(problemMatch[1]);
      return;
    }

    const contestMatch = path.match(/^\/contest\/(\d+)\/?$/);
    if (contestMatch) {
      mountContestDashboardUi(contestMatch[1]);
    }
  }

  if (typeof globalThis !== 'undefined' && globalThis.__QOJ_CPH_TEST_HOOK__) {
    globalThis.__QOJ_CPH_TEST_HOOK__({
      parseZipEntries,
      pairSampleEntries,
      inflateEntry,
      extractTestsFromZipBytes,
      fetchAttachmentTests,
      fetchAttachmentFromUrl,
      findAttachmentUrlsInDoc,
      resolveAndFetchAttachmentTests,
      parseLimitsFromDoc,
      extractTitleFromDoc,
      isCodeSnippetPre,
      extractPreTestsFromDoc,
      findNonPdfStatementUrl,
      extractContestProblemsFromDashboard,
      buildProblemPayload,
      normalizeProblemUrl,
      postToCph,
      runWithConcurrency,
    });
  }

  if (typeof document !== 'undefined' && typeof window !== 'undefined') {
    init();
  }
})();
