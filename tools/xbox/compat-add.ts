// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Run from the repository root. Reports are curated by hand; this never contacts Discord.
import { readFileSync, writeFileSync, renameSync, rmSync } from "node:fs";

const file = "compat/compat.json";
try {
  const args = process.argv.slice(2);
  const flags = new Map<string, string>();
  const required = ["id", "name", "status", "console", "fps", "version", "url"];
  for (let index = 0; index < args.length; index += 2) {
    const key = args[index]?.replace(/^--/, "");
    const value = args[index + 1];
    if (
      !args[index]?.startsWith("--") ||
      !required.includes(key) ||
      value === undefined ||
      flags.has(key)
    )
      throw new Error(
        "Expected unique --id --name --status --console --fps --version --url pairs",
      );
    flags.set(key, value);
  }
  for (const key of required) {
    if (!flags.has(key) || flags.get(key)!.length > 1024)
      throw new Error(`Missing or oversized --${key}`);
  }
  const id = flags.get("id")!.toUpperCase();
  if (!/^01[0-9A-F]{14}$/.test(id) || (BigInt(`0x${id}`) & 0x1fffn) !== 0n)
    throw new Error("--id must be a 16-digit base program ID");
  const status = flags.get("status")!;
  if (!["playable", "issues", "broken"].includes(status))
    throw new Error("Invalid --status");
  const consoleModel = new Map([
    ["Series S", "S"],
    ["Series X", "X"],
    ["S", "S"],
    ["X", "X"],
  ]).get(flags.get("console")!);
  if (!consoleModel) throw new Error("--console must be Series S or Series X");
  const version = flags.get("version")!;
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(version))
    throw new Error("Invalid --version");
  const url = new URL(flags.get("url")!);
  if (url.protocol !== "https:" || url.username || url.password)
    throw new Error("--url must use HTTPS without credentials");
  if (!flags.get("name")!.trim()) throw new Error("Empty --name");
  const data = JSON.parse(readFileSync(file, "utf8"));
  if (
    data.schema_version !== 1 ||
    !data.titles ||
    Array.isArray(data.titles) ||
    typeof data.titles !== "object"
  )
    throw new Error("Unsupported compatibility schema");
  const rows = data.titles[id] ?? [];
  if (!Array.isArray(rows)) throw new Error("Invalid title reports");
  const report = {
    title_id: id,
    name: flags.get("name"),
    status,
    console: consoleModel,
    fps_note: flags.get("fps"),
    jadyx_version: version,
    report_url: url.href,
    date: new Date().toISOString().slice(0, 10),
    source: "community",
    product: "Jadyx",
    note: "",
  };
  // One curated Jadyx report per model; preserve foreign provenance and other console models.
  const previous = rows.find(
    (row) => row.product === "Jadyx" && row.console === consoleModel,
  );
  if (previous) report.note = previous.note ?? "";
  const kept = rows.filter(
    (row) => row.product !== "Jadyx" || row.console !== consoleModel,
  );
  const insertion = rows.findIndex(
    (row) => row.product === "Jadyx" && row.console === consoleModel,
  );
  kept.splice(insertion < 0 ? kept.length : insertion, 0, report);
  data.titles[id] = kept;
  data.titles = Object.fromEntries(
    Object.entries(data.titles).sort(([left], [right]) =>
      left.localeCompare(right),
    ),
  );
  const previousTime =
    data.generated === undefined ? 0 : Date.parse(data.generated);
  if (!Number.isFinite(previousTime))
    throw new Error("Invalid generated timestamp");
  data.generated = new Date(
    Math.max(Date.now(), previousTime + 1),
  ).toISOString();
  const bytes = JSON.stringify(data, null, 2) + "\n";
  if (Buffer.byteLength(bytes) > 256 * 1024)
    throw new Error("Compatibility file exceeds 256 KiB");
  const temporary = `${file}.tmp`;
  try {
    writeFileSync(temporary, bytes);
    renameSync(temporary, file);
  } finally {
    rmSync(temporary, { force: true });
  }
  console.log(`Updated ${id} (${consoleModel}); generated ${data.generated}`);
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
