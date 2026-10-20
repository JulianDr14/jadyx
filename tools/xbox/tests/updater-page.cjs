// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const header = fs.readFileSync('src/jadyx/storage/network_portal_page.h', 'utf8');
const script = '//' + header.split('// Guided app update.')[1].split('</script>')[0];
(async () => {
  for (const language of ['en', 'es', 'pt']) {
    for (const mode of ['valid', 'foreign', 'filename', 'busy']) {
      const elements = new Map();
      let clicked = 0, callback;
      const element = id => {
        if (!elements.has(id)) elements.set(id, {
          addEventListener: (event, fn) => { callback = fn; },
          click: () => { clicked++; }
        });
        return elements.get(id);
      };
      const ticket = 'a'.repeat(64);
      vm.runInNewContext(script, {
        $: element, es: language === 'es', pt: language === 'pt',
        location: {hostname: '192.168.1.10'},
        api: async (url, options) => {
          assert.equal(url, '/api/v1/update/download');
          assert.equal(options.method, 'POST');
          if (mode === 'busy') throw Error('busy');
          return {json: async () => ({
            download: mode === 'foreign' ? 'https://other.invalid/package' : '/api/v1/update/package?ticket=' + ticket,
            filename: mode === 'filename' ? 'wrong.exe' : 'Jadyx_0.3.6.0_x64.appx'
          })};
        }
      });
      assert.equal(element('app-update-portal').href, 'https://192.168.1.10:11443');
      await callback();
      assert.equal(clicked, mode === 'valid' ? 1 : 0);
      assert.equal(element('app-update-download').disabled, false);
      if (mode !== 'valid') assert.ok(element('app-update-status').textContent.length > 20);
    }
  }
  console.log('PASS: update browser assistance, three languages, busy recovery, foreign URL/filename rejection');
})().catch(error => {console.error(error); process.exitCode = 1;});
