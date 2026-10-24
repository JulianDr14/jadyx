// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
const fs = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");
const header = fs.readFileSync("src/jadyx/storage/network_portal_page.h", "utf8");
const script = header.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
function declaration(name, end) {
  const start = script.indexOf("const " + name + "=");
  return script.slice(start, script.indexOf(end, start));
}
function functionText(name) {
  return script.match(new RegExp("(?:async )?function " + name + "\\([^]*?\\n\\}"))[0];
}
for (const locale of ["en", "es", "pt-BR"]) {
  const nodes = new Map();
  const element = (id) => {
    if (!nodes.has(id)) nodes.set(id, { textContent: "", hidden: false, disabled: false });
    return nodes.get(id);
  };
  const context = vm.createContext({
    pt: locale === "pt-BR",
    es: locale === "es",
    $: element,
    state: {
      transfer: { free_bytes: 1500000000, required_bytes: 3250000000 },
      system_supported: true,
      system: {
        available: true,
        phase: "validating",
        current_file: "example.nca",
        no_progress_seconds: 59,
        processed: 12,
        file: { received: 2048, length: 4096 },
      },
    },
    session: "mock",
    running: false,
    systemRunning: false,
    systemReviewId: "",
    size: (n) => String(n) + " B",
    percent: () => "50 %",
    choose() {},
    tabs: [],
    headers: (value) => value,
    fetch: async () => ({
      status: 507,
      ok: false,
      json: async () => ({ error: "space", free_bytes: 1500000000, required_bytes: 3250000000 }),
    }),
  });
  vm.runInContext(
    declaration("words", "if(es||pt)") +
      declaration("systemWords", "for(const [id,text]") +
      functionText("friendlyReason") +
      functionText("systemReason") +
      functionText("renderSystem") +
      functionText("api"),
    context,
  );
  const evaluate = (code) => vm.runInContext(code, context);
  assert.match(evaluate("friendlyReason(Error('space'))"), /1\.50.*3\.25/);
  assert.equal(evaluate("friendlyReason(Error('disk'))"), ""); // Permission errors aren't full disks.
  assert.match(evaluate("words.update_route"), /NSP/);
  assert.match(evaluate("words.dlc_route"), /DLC/);
  assert.match(evaluate("systemWords.title_keys"), /prod\.keys/);
  evaluate("renderSystem()");
  assert.match(element("system-job").textContent, /example\.nca/);
  assert.match(element("system-job").textContent, /2048 B \/ 4096 B/);
  assert.doesNotMatch(element("system-job").textContent, /60 s/);
  evaluate("state.system.no_progress_seconds=60; renderSystem()");
  assert.match(element("system-job").textContent, /60 s/);
  evaluate("state.system.no_progress_seconds=0; renderSystem()");
  assert.doesNotMatch(element("system-job").textContent, /60 s/);
  evaluate("api('/mock')").then(
    () => assert.fail("507 must reject"),
    (error) => {
      assert.equal(error.message, "space");
      assert.equal(error.free_bytes, 1500000000);
      assert.equal(error.required_bytes, 3250000000);
    },
  );
}
console.log(
  "PASS: three locales, disk byte counts/507, routing guidance, firmware filename and 59/60-second stall boundary",
);
