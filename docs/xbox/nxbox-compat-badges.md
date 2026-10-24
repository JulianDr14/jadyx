# Library compatibility badges

> Freshness, cache paths and curation were updated by [jx-19](nxbox-jx-19-compat-fresh.md).
> The opt-in behavior and `compat` paths below describe the original implementation.

Jadyx's library now draws a 24-unit badge over each cover: green check (Playable),
yellow exclamation (Issues), red cross (Broken), or grey question (Untested).
The Home hero and Library shelf heading show the focused game's translated status,
reported performance and console model. English, Spanish and Brazilian Portuguese
use the existing I18n catalogs. Icons are Direct2D shapes, with semantic colors in
`Palette`; there are no emoji, downloaded icons or new dependencies.

## Sources and limits

`compat/compat.json` is metadata, not game content. Its initial 3,406 bytes
contain four title IDs and eight reports: four Jadyx reports (including the Mario
Kart community report) and four historical NXbox reports. Jadyx's README describes
Mario Kart 8, Mario 3D World and Breath of the Wild; the README does not specify a
build for three of those reports, so `jadyx_version` stays empty. The community
Mario Kart report explicitly names 0.3.0.0.

ID evidence is the local read-only NXbox `tools/nxbox-pack/src/compat.json`, at
commit `a7968a92f1bda9bbfdcef3d54a650a045b93e41d`, and Jadyx's
`src/frontend_common/firmware_manager.h` (`MK8DX`). The Switch-only README's
“Mario Kart 8” rows are mapped to Mario Kart 8 Deluxe, not to a Wii U title.
The NXbox JSON's `ingame` maps conservatively to Issues; its Broken record is
preserved. These older NXbox reports are **reference only**, never claims about
Jadyx: `product: "NXbox"` is excluded from badge selection. Thus Persona 5 Royal
remains Untested in Jadyx despite NXbox's historical Broken report. NXbox's README
has newer, different results; it was inspected and is not treated as Jadyx evidence.
No source files were modified in NXbox.

Sixteen Jadyx README rows were omitted because no unambiguous title ID mapping
was found in the inspected local code/docs: Mario Strikers, Pokémon Let's Go,
Mario Wonder (both reports), Under Night In-Birth II, Naruto x Boruto, Hades II,
Bayonetta, Bayonetta 2, Mario Odyssey, 13 Sentinels, NEO TWEWY, Pokémon Sword,
Yo-kai Watch 4++, Hyrule Warriors AoC and Zelda TotK. Regional editions and the
Let's Go editions must not be guessed. Maintainers can add these once an ID is
verified. No network lookup was performed.

Playable means the cited report describes good operation; Issues means reported
problems, and Broken means it does not work or has severe problems. Badges are
reports, not a promise of performance on every version or console. When several
Jadyx reports match, selection is cautious: Broken, Issues, Untested, then Playable.
Reports from different models are retained, and the chosen report's Series X/S
model is always shown on focus; the library does not infer the current console
model. A report on X does not certify S. Only the update bit (`0x800`) is removed
when matching; DLC IDs, regional IDs and unrelated programs stay distinct.

## Maintaining the file

The root is `{ "schema_version": 1, "titles": { "16_HEX_DIGIT_BASE_ID": [...] } }`.
Each title has an array so community reports, consoles and sources remain distinct.
Each report requires these fields:

| Field | Value |
| --- | --- |
| `title_id` | Same base ID as the enclosing key, 16 hexadecimal digits |
| `name` | Verified public game name |
| `status` | `playable`, `issues`, `broken`, or `untested` |
| `console` | `S` or `X` |
| `fps_note` | Short observed performance, no estimates disguised as measurements |
| `jadyx_version` | Reported version; empty if unspecified |
| `note` | Short limitations or test scope |
| `source` | Verifiable report/document reference; mark community reports explicitly |
| `product` | `Jadyx`, or `NXbox` for reference-only evidence |

Use owned dumps' metadata/logs to verify IDs. Do not add game files, derived assets,
keys, firmware, download links or payment features. Preserve tester attribution
when available. Do not carry a report across editions or infer a build number.
Update existing superseded reports deliberately; leaving an old Broken report
alongside a new Playable report keeps the cautious Broken badge.

Unknown statuses become Untested. Malformed JSON, missing/wrong-type fields, invalid
IDs, schema versions or overlong fields reject the entire candidate document;
they cannot replace a valid fallback. Empty `titles` is a valid deliberate reset.
The limit is 256 KiB per document and 1,024 bytes per text field.

## Loading and default refresh

Packaging copies `compat` into the package's `compat` folder. On each library
session, the worker starts **after the first `canvas->Draw` returns**. All file
reads, parsing, HTTP and cache writes run on that worker. The first frame can show
Untested until metadata arrives. Publishing a table marks the library dirty;
there is no per-tile disk or network access and no network wait on first draw.
This ordering is established by code inspection, not a measured console latency.

The worker first selects valid `LocalState/jadyx/compat_cache.json`, then the bundled
copy, then an empty table (all Untested). A valid remote result supersedes these.
Offline, bad HTTP responses, corrupt remote data and storage failures stay silent.
Only validated complete responses are written atomically to LocalState. A failed
refresh keeps the local table; an unavailable LocalState still uses the bundle.

Refresh is enabled by default after first draw. `NXBOX_COMPAT_REFRESH` is obsolete;
neither an environment value nor a boot.cfg line is needed. Bundled/cached data
appears first; the newest valid `generated` timestamp wins, with remote/cache/bundle
precedence for ties. A failed request leaves the local table in use. The fixed URL is:

`https://raw.githubusercontent.com/JulianDr14/jadyx/main/compat/compat.json`

The existing bounded `Http::Fetch` runs with a total three-second cancellation
deadline and a 256 KiB stream cap. Cancellation is polled every 100 ms. The worker
is stopped/joined when leaving the library. There is one attempt per session, no
retry loop. Before this file lands on main, a 404 simply leaves local data active.

## Host validation

Before implementation, Jadyx's existing game-library and play-history tests passed;
the new compatibility test failed to compile because the implementation did not
exist. After implementation, those two tests, localization-catalogs and the new
compatibility test pass on macOS. The new test covers malformed/oversized files,
every missing/wrong-type required field, unknown statuses, base/update/DLC matching,
model filtering, foreign-emulator isolation, cautious report selection, the actual
bundled JSON, remote/cache/bundle precedence, valid empty reset, and the original refresh policy. The current default-refresh and freshness gates are
documented in [jx-19](nxbox-jx-19-compat-fresh.md).

The requested `python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`
was run before and after: both give `ImportError: Start directory is not importable:
 'tests/port'`. That NXbox suite is absent in Jadyx; it is not a Jadyx regression.

Run from the repository root, reusing an existing nlohmann include directory (the
project already depends on nlohmann; no download or dependency was added):

```sh
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror -Isrc \
  -isystem "$JSON_INCLUDE" tools/xbox/tests/compatibility.cpp \
  src/jadyx/library/compatibility.cpp -o /tmp/jadyx-compatibility
/tmp/jadyx-compatibility
clang++ -std=c++20 -Isrc tools/xbox/tests/game-library.cpp \
  -o /tmp/jadyx-game-library
/tmp/jadyx-game-library
for test in play-history localization-catalogs; do
  clang++ -std=c++20 -Isrc "tools/xbox/tests/$test.cpp" \
    src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp \
    -o "/tmp/jadyx-$test"
  "/tmp/jadyx-$test"
done
```

## Exact console test (pending, not executed)

On the Windows build machine, with no library instance using the package layout:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library
```

1. Verify package layout `build-uwp/package/compat/compat.json` matches the source.
   Install `build-uwp/package/jadyx.appx` through Device Portal; set Game mode.
2. Disconnect the console's network. Back up any existing
   `LocalState/jadyx/compat_cache.json`, then remove just that compatibility file.
   Launch the library. Its first frame/navigation must not wait for compatibility
   I/O. Owned Mario Kart 8 Deluxe and Mario 3D World copies should gain green
   checks, Breath of the Wild yellow, and an unlisted title grey. NXbox-only
   Persona evidence must remain grey. Pin a tile: its pin must not overlap the
   top-left badge. Inspect both Home rows and Library shelf with controller and
   pointer focus; the focus line must name the reported console and performance.
3. Back up the valid bundled JSON outside the package. Through Device Portal,
   place a copy at `LocalState/jadyx/compat_cache.json`; change a Jadyx report's status
   to `broken`, then reopen: red cross. Try `issues`, `playable`, an unknown status
   and an unlisted owned title to exercise all four shapes without emoji. An
   effective update must select its base game's badge; unrelated DLC must not.
4. Corrupt the cached JSON or remove a required field and reopen offline: bundled
   data must recover silently. Repeat with a missing cache. Restore the backup.
5. Reconnect the console with a normal `-Library` package, without a compatibility
   switch. After the source is available on main, the background request must
   select only newer valid metadata, update badges without moving focus and cache
   it at `LocalState/jadyx/compat_cache.json`. Disconnect and reopen: cached badges
   persist and refresh ends after about three seconds. A 404, invalid body or
   timeout must keep local badges. An older remote `generated` timestamp must
   never replace a newer cached file.

6. Test device languages en, es and pt-BR. Status and focus sentence must translate;
   report text remains the original evidence. Check clipping at 720p/1080p and rapid
   focus changes, long game names, settings transitions, launch/return and close
   during an in-flight refresh. Record open-to-first-frame timing against the base
   build, screenshots, cache result, and any critical/render errors.

No UWP/MSVC build, package, PC run, Xbox run, network request, push or upstream
operation was performed for this change. Visual layout, Xbox HTTP/cancellation,
MSVC warnings and console timing remain platform validation gates.
