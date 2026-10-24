// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
import { test, expect } from "bun:test";
import {
  mkdtempSync,
  mkdirSync,
  writeFileSync,
  readFileSync,
  rmSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const script = resolve(import.meta.dir, "../compat-add.ts");
test("CLI upserts by title/model, sorts titles, preserves other reports and bumps generated", () => {
  const cwd = mkdtempSync(join(tmpdir(), "jadyx-compat-"));
  try {
    mkdirSync(join(cwd, "compat"));
    const file = join(cwd, "compat/compat.json");
    writeFileSync(file, JSON.stringify({ schema_version: 1, titles: {} }));
    const run = (id: string, console: string, status = "issues") =>
      Bun.spawnSync(
        [
          process.execPath,
          script,
          "--id",
          id,
          "--name",
          "Fixture",
          "--status",
          status,
          "--console",
          console,
          "--fps",
          "30",
          "--version",
          "0.3.8.5",
          "--url",
          "https://discord.com/channels/1/2/3",
        ],
        { cwd },
      );
    expect(run("0100000000002000", "Series S").exitCode).toBe(0);
    expect(run("0100000000000000", "Series X").exitCode).toBe(0);
    const first = JSON.parse(readFileSync(file, "utf8"));
    const foreign = {
      ...first.titles["0100000000002000"][0],
      product: "NXbox",
      note: "Foreign fixture",
    };
    first.titles["0100000000002000"].push(foreign);
    writeFileSync(file, JSON.stringify(first));
    expect(run("0100000000002000", "Series S", "playable").exitCode).toBe(0);
    expect(run("0100000000002000", "Series X").exitCode).toBe(0);
    const data = JSON.parse(readFileSync(file, "utf8"));
    expect(Object.keys(data.titles)).toEqual([
      "0100000000000000",
      "0100000000002000",
    ]);
    expect(data.titles["0100000000002000"].length).toBe(3);
    expect(data.titles["0100000000002000"][1]).toEqual(foreign);
    const report = data.titles["0100000000002000"][0];
    expect(report.status).toBe("playable");
    expect(report.console).toBe("S");
    expect(report.source).toBe("community");
    expect(report.jadyx_version).toBe("0.3.8.5");
    expect(report.date).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    expect(Date.parse(data.generated)).toBeGreaterThan(
      Date.parse(first.generated),
    );
    const before = readFileSync(file, "utf8");
    expect(run("not-an-id", "Series S").exitCode).not.toBe(0);
    expect(run("0100000000000000", "Series S", "bogus").exitCode).not.toBe(0);
    expect(run("0100000000000000", "toString").exitCode).not.toBe(0);
    expect(readFileSync(file, "utf8")).toBe(before);
  } finally {
    rmSync(cwd, { recursive: true, force: true });
  }
});
