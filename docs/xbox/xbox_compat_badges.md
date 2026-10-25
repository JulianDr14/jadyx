# Library compatibility badges

Each cover in the library carries a small drawn badge: green check (**Playable**), yellow
exclamation (**Issues**), red cross (**Broken**) or grey question mark (**Untested**). When a game
is focused, the Home hero and the Library shelf heading show the translated status, the reported
performance and the console model of the report. Icons are Direct2D shapes; colours live in
`Palette` (`ui_painter.h`).

Code: `src/jadyx/library/compatibility.{h,cpp}` (parsing, selection) and
`compatibility_loader.{h,cpp}` (background loading). Test: `tools/xbox/tests/compatibility.cpp`.

## Data file

[`dist/compat/compat.json`](../../dist/compat/compat.json) is packaged as `compat\compat.json`.

```json
{ "schema_version": 1, "titles": { "<16-hex base title ID>": [ { ...report... } ] } }
```

Each title holds an array, so separate reports, consoles and sources stay distinct. Every report
needs all of these string fields:

| Field | Value |
| --- | --- |
| `title_id` | Same base ID as the enclosing key |
| `name` | Public game name, not empty |
| `status` | `playable`, `issues`, `broken` or `untested` (anything else reads as Untested) |
| `console` | `X` or `S` |
| `fps_note` | Observed performance; leave empty rather than estimate |
| `jadyx_version` | Version the report was made on; empty if unknown |
| `note` | Short limitations or test scope |
| `source` | Where the report comes from, not empty; mark community reports |
| `product` | `Jadyx`; reports for any other product are kept but never shown as a badge |

Rules:
- Verify the title ID from your own dump's metadata or log; never guess regional editions.
- No game files, keys, firmware or download links.
- Replace a superseded report instead of adding next to it: with several Jadyx reports for the same
  model, the most cautious one wins (Broken > Issues > Untested > Playable).
- A report on Series X is not a claim about Series S. The library does not detect the model, so it
  shows the most cautious report and names its model in the focus text.
- Only the update bit (`0x800`) is folded into the base ID; DLC and other programs stay distinct.

Validation is all or nothing: malformed JSON, a wrong schema version, an invalid ID, a missing or
non-string field, or a field over 1024 bytes rejects the whole document, which then cannot replace
a good fallback. The document limit is 256 KiB. An empty `titles` object is a valid reset.

## Loading

The loader starts after the library's first draw and does all file, parsing, HTTP and cache work on
its own thread, so the first frame can show Untested until the table arrives. It uses the first
valid source of:

1. a validated remote copy (only with refresh enabled, see below),
2. `LocalState\compat\compat.json` (the cache of the last valid remote copy),
3. the bundled `compat\compat.json`,
4. an empty table (everything Untested).

Failures stay silent: offline, HTTP errors, corrupt data or unavailable storage just keep the next
source.

## Optional refresh

Off by default; without it no compatibility request is made. Enable it with the environment
variable `JADYX_COMPAT_REFRESH=1`, or on the console with the same line in `boot.cfg`:

```powershell
.\tools\xbox\package-appx.ps1 -Library -BootCfg 'JADYX_COMPAT_REFRESH=1'
```

A non-empty environment value wins over `boot.cfg`; any value other than `1` disables it. The
library then makes one request per session to
`https://raw.githubusercontent.com/JulianDr14/jadyx/main/dist/compat/compat.json`, with a 3 s total
deadline and a 256 KiB cap, and caches the response atomically only if it validates. Leaving the
library cancels and joins the request.

## Console test (pending)

- [ ] Package layout has `compat\compat.json` identical to the source.
- [ ] Offline, without a cache: listed games show their badges, unlisted ones grey; navigation does
      not wait for the table. A pinned tile's pin does not overlap the badge.
- [ ] Focus text names the model and performance, in en, es and pt-BR, on Home and Library, with
      controller and pointer, at 720p and 1080p, with long names.
- [ ] A cached copy edited to each status shows all four shapes; an update uses its base game's
      badge, a DLC does not.
- [ ] A corrupt or incomplete cache falls back to the bundle silently.
- [ ] With refresh on: a valid response updates badges without moving focus and writes the cache;
      a 404, bad body or timeout keeps the local badges; closing the library mid-request is clean.
