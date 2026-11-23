// A small Markdown reader for a mod's README.md: headings, paragraphs, lists, bold and italic, inline code, code blocks,
// tables, quotes and links. It builds the page with el() and text nodes only, never innerHTML: a README is the mod
// author's text, so HTML in it stays text. Only https:// links become links, and they open through the host
// (open_url, which checks them again); relative links and pictures stay text, and nothing is loaded. What the reader
// does not know stays as it is written.
'use strict';

const MD_ITEM = /^(\s*)([-*+]|\d{1,9}[.)])\s+(.*)$/;
const MD_FENCE = /^\s{0,3}(`{3,}|~{3,})/;
const MD_HEADING = /^\s{0,3}(#{1,6})(?:\s+(.*))?$/;  // the closing #s are taken off by mdHeadingText
const MD_RULE = /^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/;
const MD_QUOTE = /^\s{0,3}>/;
// A table's line of dashes: cells of -, maybe with a : at either end ("|---|:-:|").
function mdTableLine(line) {
  const cells = line.trim().split('|');
  if (cells.length && !cells[0].trim()) cells.shift();
  if (cells.length && !cells[cells.length - 1].trim()) cells.pop();
  return cells.length > 0 && cells.every((c) => /^:?-+:?$/.test(c.trim()));
}

// A heading's text without the #s that may close it ("## Title ##").
function mdHeadingText(text) {
  let t = (text || '').trimEnd();
  let k = t.length;
  while (k > 0 && t[k - 1] === '#') k--;
  if (k < t.length && (k === 0 || /\s/.test(t[k - 1]))) t = t.slice(0, k).trimEnd();
  return t;
}
// sticky: they match at lastIndex only, so a long line is never copied for each character; bounded, so text that never
// closes a link costs little; a URL may hold one level of brackets (".../Foo_(bar)")
const MD_ESCAPE = /\\[\\`*_{}[\]()#+\-.!|>~]/y;
const MD_TICKS = /`+/y;
const MD_URL = '((?:[^()\\s]|\\([^()\\s]{0,2048}\\)){1,2048})(?:\\s+"[^"]{0,500}")?\\)';
const MD_PICTURE = new RegExp('!\\[([^\\]]{0,500})\\]\\(' + MD_URL, 'y');
const MD_LINKED = new RegExp('\\[([^\\]]{1,500})\\]\\(' + MD_URL, 'y');
const MD_BADGE = new RegExp('\\[!\\[([^\\]]{0,500})\\]\\([^()\\s]{0,2048}(?:\\s+"[^"]{0,500}")?\\)\\]\\(' + MD_URL, 'y');  // a picture in a link
const MD_AUTOLINK = /<(https:\/\/[^>\s]{1,2048})>/y;
// A line longer than this is shown as it is (no block or inline reading: crafted lines cannot slow the page down).
const MD_LONG = 2000;
// A table shows at most this many columns and rows; the rest is said in a line under it.
const MD_TABLE_COLUMNS = 64;
const MD_TABLE_ROWS = 500;

function mdAt(re, text, i) {
  re.lastIndex = i;
  return re.exec(text);
}

// A link from the README: https:// opens through the host; anything else is its text only.
function mdLink(label, url) {
  if (!/^https:\/\//.test(url)) return el('span', { class: 'md-rel' }, mdInline(label));
  // external: a link from a mod's own text, so any https site (the app's own buttons open nexusmods.com only)
  return el('button', { class: 'md-link', type: 'button', title: url, onclick: () => send({ cmd: 'open_url', url, external: true }) }, mdInline(label));
}

// One line's text with `code`, **bold**, *italic* / _italic_, [links](https://...), <https://...> and \-escapes.
function mdInline(text) {
  const out = [];
  let buf = '';
  const flush = () => { if (buf) { out.push(buf); buf = ''; } };
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    let m;
    if (c === '\\' && mdAt(MD_ESCAPE, text, i)) { buf += text[i + 1]; i += 2; continue; }
    if (c === '`') {
      const ticks = mdAt(MD_TICKS, text, i)[0];
      const end = text.indexOf(ticks, i + ticks.length);
      if (end > i) {
        flush();
        out.push(el('code', {}, text.slice(i + ticks.length, end)));
        i = end + ticks.length;
      } else { buf += ticks; i += ticks.length; }
      continue;
    }
    if (c === '!' && (m = mdAt(MD_PICTURE, text, i))) {  // a picture: never loaded
      buf += '[picture: ' + (m[1] || m[2]) + ']';
      i += m[0].length;
      continue;
    }
    if (c === '[' && text[i + 1] === '!' && (m = mdAt(MD_BADGE, text, i))) {  // a badge: its picture's text, linked
      flush();
      out.push(mdLink('[picture: ' + (m[1] || 'badge') + ']', m[2]));
      i += m[0].length;
      continue;
    }
    if (c === '[' && (m = mdAt(MD_LINKED, text, i))) {
      flush();
      out.push(mdLink(m[1], m[2]));
      i += m[0].length;
      continue;
    }
    if (c === '<' && (m = mdAt(MD_AUTOLINK, text, i))) {
      flush();
      out.push(mdLink(m[1], m[1]));
      i += m[0].length;
      continue;
    }
    if (c === '*' || c === '_') {
      const mark = text[i + 1] === c ? c + c : c;
      const inWord = c === '_' && i > 0 && /\w/.test(text[i - 1]);  // snake_case is no emphasis
      const end = inWord ? -1 : text.indexOf(mark, i + mark.length);
      const inner = end > 0 ? text.slice(i + mark.length, end) : '';
      if (inner && !/^\s|\s$/.test(inner) && !(c === '_' && /\w/.test(text[end + mark.length] || ''))) {
        flush();
        out.push(el(mark.length === 2 ? 'strong' : 'em', {}, mdInline(inner)));
        i = end + mark.length;
        continue;
      }
      buf += mark;
      i += mark.length;
      continue;
    }
    buf += c;
    i++;
  }
  flush();
  return out;
}

function mdBlank(line) { return !line.trim(); }

// A table starts with a head line and, under it, a line of dashes; both need a '|' ("a | b" over a "---" rule is a
// paragraph and a rule).
function mdTableStarts(lines, i) {
  return lines[i].includes('|') && i + 1 < lines.length && lines[i + 1].length <= MD_LONG && lines[i + 1].includes('|') &&
    lines[i + 1].includes('-') && mdTableLine(lines[i + 1]);
}

// A list item's parts, or null (a long line is never one).
function mdItem(line) { return line.length <= MD_LONG ? MD_ITEM.exec(line) : null; }

function mdStartsBlock(lines, i) {
  const l = lines[i];
  if (l.length > MD_LONG) return true;  // shown as it is, on its own
  return MD_FENCE.test(l) || MD_HEADING.test(l) || MD_RULE.test(l) || MD_QUOTE.test(l) || MD_ITEM.test(l) || mdTableStarts(lines, i);
}

function mdCells(line) {
  let t = line.trim();
  if (t.startsWith('|')) t = t.slice(1);
  if (t.endsWith('|') && !t.endsWith('\\|')) t = t.slice(0, -1);
  return t.split(/(?<!\\)\|/).map((c) => c.trim());
}

// How deep quotes and lists go inside each other; deeper ones are drawn at this depth (a README cannot exhaust the stack).
const MD_MAX_DEPTH = 8;

// A list from line i, with deeper-indented lists inside its items. Returns the list and the line after it.
function mdList(lines, i, depth) {
  const first = mdItem(lines[i]);
  const indent = first[1].length;
  const ordered = /\d/.test(first[2]);
  const start = ordered ? parseInt(first[2], 10) : 1;
  const items = [];
  while (i < lines.length) {
    const m = mdItem(lines[i]);
    if (!m || m[1].length < indent) break;
    if (m[1].length > indent && depth < MD_MAX_DEPTH) {  // a list inside the last item
      const inner = mdList(lines, i, depth + 1);
      if (items.length) items[items.length - 1].push(inner.node); else items.push([inner.node]);
      i = inner.next;
      continue;
    }
    if (/\d/.test(m[2]) !== ordered) break;
    const parts = [m[3]];
    i++;
    while (i < lines.length && !mdBlank(lines[i]) && lines[i].length <= MD_LONG && /^\s+/.test(lines[i]) && !mdItem(lines[i])) parts.push(lines[i++].trim());
    items.push(mdInline(parts.join(' ')));
    let j = i;  // a blank line between two items keeps the list going
    while (j < lines.length && mdBlank(lines[j])) j++;
    if (j > i && j < lines.length) {
      const next = mdItem(lines[j]);
      if (next && next[1].length >= indent) i = j;
    }
  }
  const attrs = { class: 'md-list' };
  if (ordered && start !== 1) attrs.start = start;  // "2." after a code block stays 2
  return { node: el(ordered ? 'ol' : 'ul', attrs, items.map((parts) => el('li', {}, parts))), next: i };
}

// The README's text as page nodes (depth: inside how many quotes).
function markdownNodes(text, depth = 0) {
  const lines = text.replace(/\r\n?|[\u2028\u2029]/g, '\n').split('\n');
  const out = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    let m;
    if (mdBlank(line)) { i++; continue; }
    if (line.length > MD_LONG) { out.push(el('p', {}, line)); i++; continue; }
    if ((m = MD_FENCE.exec(line))) {  // a code block, to its closing fence or the end
      const body = [];
      i++;
      while (i < lines.length && !lines[i].trim().startsWith(m[1])) body.push(lines[i++]);
      i++;
      out.push(el('pre', { class: 'md-code' }, el('code', {}, body.join('\n'))));
      continue;
    }
    if ((m = MD_HEADING.exec(line))) {  // # is h4: the details already use h3 for "Readme"
      out.push(el('h' + Math.min(m[1].length + 3, 6), { class: 'md-h' }, mdInline(mdHeadingText(m[2]))));
      i++;
      continue;
    }
    if (MD_RULE.test(line)) { out.push(el('hr', { class: 'md-rule' })); i++; continue; }
    if (mdTableStarts(lines, i)) {
      const all = mdCells(line);
      const head = all.slice(0, MD_TABLE_COLUMNS);
      const rows = [];
      let more = 0;
      i += 2;
      while (i < lines.length && !mdBlank(lines[i]) && lines[i].length <= MD_LONG && lines[i].includes('|')) {
        if (rows.length < MD_TABLE_ROWS) rows.push(mdCells(lines[i])); else more++;
        i++;
      }
      out.push(el('div', { class: 'md-table' }, el('table', {},
        el('thead', {}, el('tr', {}, head.map((c) => el('th', {}, mdInline(c))))),
        el('tbody', {}, rows.map((r) => el('tr', {}, head.map((_, k) => el('td', {}, mdInline(r[k] || '')))))))));
      if (more || all.length > head.length)
        out.push(el('p', { class: 'md-more' }, '(The table goes on: ' + [more ? more + ' more rows' : '', all.length > head.length ? (all.length - head.length) + ' more columns' : ''].filter(Boolean).join(', ') + ' in the README file.)'));
      continue;
    }
    if (MD_QUOTE.test(line)) {
      const body = [];
      while (i < lines.length && MD_QUOTE.test(lines[i])) body.push(lines[i++].replace(/^\s{0,3}>\s?/, ''));
      out.push(el('blockquote', { class: 'md-quote' }, depth < MD_MAX_DEPTH
        ? markdownNodes(body.join('\n'), depth + 1)
        : el('p', {}, mdInline(body.join(' ')))));
      continue;
    }
    if (mdItem(line)) {
      const list = mdList(lines, i, 0);
      out.push(list.node);
      i = list.next;
      continue;
    }
    const para = [line.trim()];  // a paragraph: up to a blank line or another block
    i++;
    while (i < lines.length && !mdBlank(lines[i]) && !mdStartsBlock(lines, i)) para.push(lines[i++].trim());
    out.push(el('p', {}, mdInline(para.join(' '))));
  }
  return out;
}
