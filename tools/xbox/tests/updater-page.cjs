// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const header = fs.readFileSync('src/jadyx/storage/network_portal_page.h', 'utf8');
const marker = '// Guided app update';
assert.ok(header.includes(marker), 'guided update script not found in the portal page');
const script = marker + header.split(marker)[1].split('</script>')[0];
const ticket = 'a'.repeat(64);
const settle = () => new Promise(resolve => setImmediate(resolve));

function load(language, mode) {
  const elements = new Map(), opened = [], requests = [];
  let clicked = 0, now = 1000000;
  const element = id => {
    if (!elements.has(id)) {
      const listeners = {}, classes = new Set();
      elements.set(id, {
        id, hidden: false, disabled: false, textContent: '', className: '',
        classList: {toggle: (name, on) => { on ? classes.add(name) : classes.delete(name); }, has: name => classes.has(name)},
        addEventListener: (event, fn) => { listeners[event] = fn; },
        fire: event => listeners[event](),
        click: () => { if (id === 'update-package') clicked++; else listeners.click?.(); }
      });
    }
    return elements.get(id);
  };
  const body = element('body');
  const context = {
    $: element, es: language === 'es', pt: language === 'pt',
    document: {body}, location: {hostname: '192.168.1.10', reload: () => {}},
    window: {open: (...args) => opened.push(args)}, scrollTo: () => {},
    size: bytes => bytes + ' B', session: 'session', state: {update: {offered: true}},
    Date: {now: () => now},
    api: async (url, options) => {
      requests.push(url);
      assert.equal(url, '/api/v1/update/download');
      assert.equal(options.method, 'POST');
      if (mode === 'busy' || mode === 'lost') throw Error(mode);
      return {json: async () => ({
        download: mode === 'foreign' ? 'https://other.invalid/package' : '/api/v1/update/package?ticket=' + ticket,
        filename: mode === 'filename' ? 'wrong.exe' : 'Jadyx_0.3.6.0_x64.appx',
        version: '0.3.6.0', size: 1234
      })};
    }
  };
  vm.runInNewContext(script, context);
  return {context, element, body, opened, requests, clicked: () => clicked, advance: ms => { now += ms; }};
}

(async () => {
  for (const language of ['en', 'es', 'pt']) {
    for (const mode of ['valid', 'foreign', 'filename', 'busy', 'lost']) {
      const page = load(language, mode), $ = page.element;
      assert.equal($('update-portal').href, 'https://192.168.1.10:11443');
      assert.ok($('update-go').textContent.length > 5 && $('update-keep').textContent.length > 20);

      // An offered update opens the steps and fetches the ticket ahead of the click.
      page.context.renderUpdate();
      assert.ok(page.body.classList.has('updating'));
      assert.equal($('update-banner').hidden, true);
      await settle();
      assert.equal(page.requests.length, 1);

      if (mode !== 'valid') {
        // Rejected or failed tickets never enable the download; real errors are shown and retried.
        assert.equal($('update-go').disabled, true);
        assert.equal($('update-badge').hidden, true);
        if (mode === 'lost') {
          assert.ok(!/0\.3\.6\.0/.test($('update-meta').textContent));
        } else {
          assert.ok($('update-meta').textContent.length > 20);
          page.context.renderUpdate();
          await settle();
          assert.equal(page.requests.length, 1, 'retried before the backoff');
          page.advance(5001);
          page.context.renderUpdate();
          await settle();
          assert.equal(page.requests.length, 2, 'no retry after the backoff');
        }
        $('update-go').fire('click');
        assert.equal(page.clicked(), 0);
        assert.equal(page.opened.length, 0);
        continue;
      }

      assert.equal($('update-go').disabled, false);
      assert.equal($('update-badge').hidden, false);
      assert.equal($('update-title').textContent, 'Jadyx 0.3.6.0');
      assert.equal($('update-meta').textContent, 'Jadyx_0.3.6.0_x64.appx · 1234 B');
      assert.equal($('update-step-2').className, 'now');

      // The click starts the download and opens Device Portal; the ticket serves only once.
      $('update-go').fire('click');
      assert.equal(page.clicked(), 1);
      assert.equal($('update-package').href, '/api/v1/update/package?ticket=' + ticket);
      assert.equal($('update-package').download, 'Jadyx_0.3.6.0_x64.appx');
      assert.deepEqual(page.opened, [['https://192.168.1.10:11443', '_blank', 'noopener']]);
      assert.equal($('update-step-2').className, 'done');
      assert.equal($('update-step-3').className, 'now');
      assert.equal($('update-go').classList.has('primary'), false);
      await settle();
      assert.equal(page.requests.length, 2, 'a used ticket is replaced');
      assert.equal($('update-go').disabled, false);

      // Skipping keeps a banner to come back to the steps.
      $('update-skip').fire('click');
      assert.equal(page.body.classList.has('updating'), false);
      assert.equal($('update-banner').hidden, false);
      $('update-banner-open').fire('click');
      assert.equal(page.body.classList.has('updating'), true);
      assert.equal($('update-banner').hidden, true);

      // Without an offer the page stays on its content.
      page.context.state = {update: {offered: false}};
      page.context.renderUpdate();
      assert.equal(page.body.classList.has('updating'), false);
    }
  }
  console.log('PASS: guided update page, three languages, ticket prefetch/reuse, busy retry, foreign URL/filename rejection, skip banner');
})().catch(error => {console.error(error); process.exitCode = 1;});
