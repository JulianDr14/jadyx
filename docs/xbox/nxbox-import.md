# NXbox → Jadyx transfer contract (version 1)

Task: `jx-25-nxbox-import`, based on local `nxbox/integration3` (`31c2a77097`).
This document is the sender/receiver contract for the parallel NXbox migration work.
The reference NXbox checkout was inspected read-only; this change adds no transfer
client, downloads, credentials, payload data or new dependency. No network is used
by the importer. Existing Jadyx paths, LocalSettings keys and locale catalogs remain
its source of truth.

## Handoff and success

NXbox installs Jadyx and copies the user's existing files with Device Portal while
Jadyx is stopped. Payloads go directly to Jadyx's **normal** LocalState paths:
`jadyx/keys/`, `jadyx/nand/system/Contents/registered/`, the user's normal NAND save
hierarchy and `jadyx/load/<title-id>/` for mods. Preserve save user IDs and the
complete directory layout; this importer does not transform profiles or payloads.
The sender must resolve the target's actual normal storage layout rather than
creating an `nxbox` payload subtree. No payload belongs in this repository.

Then publish these UTF-8 files under **LocalState/nxbox-import/**:

1. `library.json`: title IDs, pins and history.
2. `receipt.json`: successful-copy summary, containing no secrets or file contents.
3. Optional `usb-folders.txt`: paths as a reminder, never transferable permissions.
4. `settings.json` **last**, after all payload copies and metadata writes succeed.
   Launch Jadyx only after publishing this file. Do not run the sender concurrently
   with Jadyx. The directory must represent one complete transfer, not a live queue.

Jadyx checks this directory at every library entry. When absent it creates nothing
and changes no settings. All three JSON files must be readable, version 1 and at
most 1 MiB each, with JSON nesting limited to 32 levels, before any settings are applied. Malformed JSON, a missing file or
an unknown version leaves the transfer unmarked, for repair/retry at the next
library entry. Unknown setting fields and unsupported setting values are skipped.
Malformed individual library records are skipped; an invalid games container is
rejected. Duplicate title IDs use their first valid record.

After applying equivalent settings and persisting title-ID library records into
`LocalState/jadyx/nxbox-import-library.json`, Jadyx atomically writes
`nxbox-import/imported.flag` (`jadyx-nxbox-import 1\n`). This means the metadata has
been accepted durably, including records awaiting USB authorization. It does not
assert that every game is already accessible. The sender must never supply this
flag or `welcome-dismissed.flag` and must not erase an existing completed import.
An existing flag prevents reapplication, even if input files are later changed.

The welcome is kept until Continue/B is pressed; dismissal persists
`welcome-dismissed.flag`. A restart before acknowledgment shows it again without
reimporting. Its receipt describes what NXbox **reported copying**, not a second
firmware or content verification by Jadyx. The sender must count only successful
copies (one save directory/mod installation, not each contained file), report the
copied firmware's version, and use zero/false/empty string for absent payloads.

## JSON schemas

These are JSON Schema draft 2020-12 contracts. Unknown properties are allowed for
forward-compatible additions; unsupported major versions require a new receiver.
`program_id` is a nonzero, 16-digit hexadecimal base application/title ID, never
a path, display name, update ID or DLC ID. Examples below use synthetic IDs.

`settings.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["version"],
  "properties": {
    "version": {"const": 1},
    "language": {"enum": ["en", "es", "pt-BR", "pt_br"]},
    "resolution": {"enum": [0.5, 0.75, 1, 1.5, 2, 3]},
    "controller_layout": {"enum": ["xbox", "switch"]},
    "confirm_button": {"enum": ["a", "b"]},
    "per_game": {
      "type": "object",
      "propertyNames": {"pattern": "^(?!0000000000000000$)[0-9a-fA-F]{16}$"},
      "additionalProperties": {
        "type": "object",
        "properties": {"resolution": {"enum": [0.5, 0.75, 1, 1.5, 2, 3]}}
      }
    }
  }
}
```

```json
{
  "version": 1,
  "language": "pt_br",
  "resolution": 0.75,
  "controller_layout": "switch",
  "confirm_button": "b",
  "per_game": {"0100000000000001": {"resolution": 1}}
}
```

Mapping: `language` → `system_ui_language`; scale → `system_resolution` or
`game_resolution_<lowercase-id>` using Jadyx's validated enum, never NXbox enum
ordinals; `controller_layout=switch` → `controller_swap_face=true` (physical
A/B and X/Y swaps), `xbox` → false. The sender translates its native values to
these semantic values. Jadyx UI confirmation always uses A: `confirm_button` has
no equivalent and is intentionally skipped. Other per-game fields are skipped;
only resolution has a matching per-title persistence path today. Imported
resolution preferences retain the existing **default-off**
`NXBOX_D3D12_RESOLUTION_SCALE` runtime gate; importing does not enable it.

A LocalSettings key that **exists**, including an explicitly saved default, is
never replaced. Jadyx's existing System/Controller save routines may save the
whole category, so those categories are conservatively treated as already owned.
Only imported, absent keys are written. If a later write fails, retry preserves
both earlier imported values and intervening user changes. No global settings
object is saved as part of migration.

`library.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["version", "games"],
  "properties": {
    "version": {"const": 1},
    "games": {
      "type": "array", "maxItems": 4096,
      "items": {
        "type": "object", "required": ["program_id"],
        "properties": {
          "program_id": {"type": "string", "pattern": "^(?!0000000000000000$)[0-9a-fA-F]{16}$"},
          "pinned": {"type": "boolean", "default": false},
          "seconds": {"type": "integer", "minimum": 0, "maximum": 18446744073709551615, "default": 0},
          "last_played": {"type": "integer", "minimum": 0, "maximum": 9223372036854775807, "default": 0},
          "sessions": {"type": "integer", "minimum": 0, "maximum": 4294967295, "default": 0}
        }
      }
    }
  }
}
```

```json
{
  "version": 1,
  "games": [
    {"program_id": "0100000000000001", "pinned": true,
     "seconds": 7200, "last_played": 1792800000, "sessions": 3}
  ]
}
```

Times are whole seconds; `last_played` is Unix time, 0 means unknown. The sender
must aggregate one record per base title. Homebrew without a stable application
ID is not imported as history. After metadata discovers a title, its record is
resolved to the first matching Jadyx launch path. Favorites merge by set union;
seconds, sessions and last-played use **max**, preserving the greater existing
value and avoiding double-counting on retries. They are not summed across apps.
Existing Jadyx history remains intact. Resolved records are removed from the
pending file after favorites/history save successfully; later scans cannot
re-pin a title the user subsequently unpins. Unresolved titles remain pending
across restarts and welcome dismissal. This imports Jadyx library history;
it does not rewrite the core's separate playtime.bin.

`receipt.json` (additional summary needed by the welcome):

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object", "required": ["version"],
  "properties": {
    "version": {"const": 1},
    "keys": {"type": "boolean", "default": false},
    "firmware": {"type": "string", "maxLength": 64, "default": ""},
    "saves": {"type": "integer", "minimum": 0, "maximum": 4294967295, "default": 0},
    "mods": {"type": "integer", "minimum": 0, "maximum": 4294967295, "default": 0}
  }
}
```

```json
{"version": 1, "keys": false, "firmware": "", "saves": 0, "mods": 0}
```

Firmware text has no control characters and is limited to 64 UTF-8 bytes.
`usb-folders.txt` is LF or CRLF, one absolute Windows USB folder path per line:

```text
E:\Games
F:\Homebrew
```

Empty, duplicate, over-1024-byte and control-character lines are skipped; at most
64 paths are retained. Paths are never opened or authorized from this file.
The welcome displays the first three as reminders. Choose **Re-add USB folder**
(A), pick one folder, wait for its scan, then repeat for other folders. Each
selection follows the existing brokered Jadyx folder-access/persistence path.
Cancel leaves the welcome available. Continue/B closes the card; Settings > File
manager remains available for later authorization. AppContainer access tokens
are never imported. There is no settings/metadata import environment switch:
the presence of a completed explicit transfer is its opt-in, and absent folders
are inert. Renderer experiments keep their existing switches.

## Finding and implementation

Previously Jadyx ignored `nxbox-import/`. LocalSettings already provides the
presence information needed to preserve user choices, while library favorites
and history are keyed by launch path. The new portable parser/store accepts
semantic settings and stages title IDs; UWP adapters apply only absent keys and
resolve history as the metadata worker learns IDs. The existing library canvas,
semantic design tokens, picker and en/es/pt_br catalogs provide the welcome.
No upstream submission or change to emulator execution is involved.

A regression test also found `WriteFileAtomically` could return true after a
failed rename because successful cleanup overwrote its error code. Cleanup now
uses a separate error code; failed destination writes cannot mark an import as
complete. This is a safe error-reporting correction shared by frontend stores.

## Host validation

Run from the repository root using an **already installed** nlohmann include
folder (the project already depends on it; do not download another copy):

```sh
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror -Isrc \
  -isystem "$JSON_INCLUDE" tools/xbox/tests/nxbox-import.cpp -o /tmp/jadyx-nxbox-import
/tmp/jadyx-nxbox-import
python3 -B tools/xbox/tests/run-resolution-options.py
python3 -B tools/xbox/tests/usb-play-in-place.py
python3 -B tools/xbox/tests/integration-locales.py
clang++ -std=c++20 -Isrc tools/xbox/tests/play-history.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp -o /tmp/jadyx-play-history
/tmp/jadyx-play-history
clang++ -std=c++20 -Isrc tools/xbox/tests/localization-catalogs.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp -o /tmp/jadyx-locales
/tmp/jadyx-locales
```

The new host test first failed because the importer was absent. The rename
failure assertion then failed against the old atomic writer (exit 134) before
its correction. Final results (macOS, before → after):

| Gate | Before | After |
| --- | --- | --- |
| New importer | Missing-header failure | 57 explicit assertions pass with strict warnings |
| Failed atomic rename | Regression assertion fails, exit 134 | Pass; no false success |
| Resolution runner | 4 pass, 2 existing failures | Same 4 pass and same 2 failures |
| USB runner | 6/6 pass | 6/6 pass |
| Catalog source order | 1/1 pass | 1/1 pass |
| Play-history executable | Pass | Pass |
| Catalog completeness/placeholders executable | Pass | Pass |

The existing resolution-runner failures are `game-menu` and `system-settings`,
already documented in the integration notes. No new failing host gates.
The maximum 4096 records with full-width counters serialize to **557079 bytes**,
below the **1048576-byte** file limit. There are five new translated strings in
each of en/es/pt_br. Formatter was run on new/changed code; unrelated formatting
was preserved. `git diff --check` passes. The strict test uses a local pre-existing
nlohmann header; no library was installed.
No UWP/MSVC build, PC runtime, console, push or network operation was performed.

## Exact console test (pending; owner executes)

1. Build using `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`,
   package through the normal `build-uwp/package/jadyx.appx` workflow, install on
   Series X|S in Dev Mode/Game mode. With no `nxbox-import/`, launch twice and
   verify the existing library/settings are unchanged and no migration files appear.
2. In Jadyx save Spanish as the interface language and select the normal controller
   layout, including explicitly saving its default. Stop the app. With Device
   Portal create LocalState/nxbox-import and upload the four metadata examples
   above, with `settings.json` last. Replace the synthetic title ID with the base
   ID of a legally owned local test title already in the library; keep its actual
   data outside the repository. Use truthful receipt counts for any transferred
   user-owned payloads. This test needs no new keys or firmware.
3. Launch. Verify Spanish and the saved layout are retained, the welcome summary
   matches the receipt, and the title is pinned with at least 7200 seconds (or
   its greater existing history). Verify `imported.flag` and the pending file in
   LocalState; the title should disappear from pending after metadata resolves it.
4. Press A on Re-add USB folder, cancel once, press A again and select the user's
   USB game folder. Verify normal scanning and access, repeat for a second folder,
   then B/Continue. Restart: no welcome, no second import, unchanged time. Unpin the
   imported title and rescan/restart: it must stay unpinned.
5. Repeat in a disposable test installation with no saved LocalSettings. Verify
   pt_br becomes pt-BR, the Switch physical layout is selected, global scale is
   0.75 and the per-title scale is 1. Keep the existing resolution runtime gate
   off; migration must not enable it. Check all three welcome translations by
   selecting en/es/pt-BR before an unacknowledged welcome is reopened.
6. In a disposable transfer remove imported.flag and one required JSON file;
   restart: no marker and no partial settings import. Supply the file, restart,
   verify success. Repeat with invalid JSON/version 2. Simulate unavailable
   destination storage and verify no success marker; restore storage and retry.
7. Include a title unavailable until USB authorization. Dismiss the welcome,
   restart, authorize the folder from File manager and verify its pending pin/time
   resolves once. Stop/relaunch during an unacknowledged welcome: show it again
   without duplicating history. Inspect logs for migration warnings and any
   storage failure. Console visual/input and actual payload compatibility remain
   unverified until these steps are completed.
