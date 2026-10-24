# Compatibility freshness without an application release

## Finding and scope

Base: local `nxbox/integration3`, fast-forwarded from the main-based
`nxbox/19-compat-fresh` branch. The existing library loader ran after the first
paint, but refresh needed `NXBOX_COMPAT_REFRESH=1`, every valid remote response
replaced local data regardless of age, and `jadyx_version`/`source` were required.

This change only updates curated badge metadata. It adds no Discord bot, workflow,
dependency, game downloads, game content, credentials, firmware or payment feature.
No network, PC run, console access, push or package build was used for this work.

## Changes

- The single curated source is now `compat/compat.json`, moved from `dist/compat`.
  Packaging still installs it as `compat/compat.json`. Existing reports and their
  attribution are preserved; the document adds `generated` and sorted title IDs.
- `jadyx_version`, `report_url`, `date` and `source` are optional strings, bounded
  to 1024 bytes each. New reports use `source: "community"`; curators may set
  `"manual"`. Legacy descriptive source strings remain accepted for old files.
- `generated` is optional for legacy documents. New documents use ISO 8601 UTC
  (`YYYY-MM-DDTHH:MM:SS[.fraction]Z`, 1–9 fractional digits). Invalid timestamps,
  including impossible calendar dates, reject the whole document. Undated files
  rank below dated files; equal timestamps prefer downloaded, cached, then bundled.
- Opening the library starts one background refresh after the first draw, using
  `https://raw.githubusercontent.com/JulianDr14/jadyx/main/compat/compat.json`.
  The old opt-in is removed, as explicitly requested. This is bounded metadata
  I/O, with no emulation policy change. Bundled/cached data publishes first.
- The existing HTTP transport limits the complete response to 256 KiB and polls
  cancellation every 100 ms. A 3-second total deadline covers headers and body;
  late responses are discarded. Worker shutdown requests cancellation and joins.
- Only a validated response winning the timestamp comparison is published/cached.
  The cache is `LocalState/jadyx/compat_cache.json`. Atomic replacement reuses
  the updater's UWP storage helper (`ReplaceFileFromAppW` for an existing file).
  A failed cache write preserves the previous file and the new in-memory table.
  No UI error appears; one `Compatibility refresh:` info line records the outcome
  in `jadyx_log.txt` per library session. The legacy cache path is not migrated.
- Focus labels retain the existing localized status/FPS/model text, appending
  ` · Jadyx <version>` only when present. Jadyx is the product name in all locales.

Manual curation from the Discord compatibility channel, from the repository root:

```sh
bun tools/xbox/compat-add.ts --id 0100000000000000 --name "Synthetic fixture" \
  --status playable --console "Series S" --fps "30 FPS" \
  --version 0.3.8.5 --url https://discord.com/channels/1/2/3
```

The command makes no requests. It upserts the Jadyx report for that base program
ID/model, preserves the existing note and other models/products, removes duplicate
Jadyx rows for that model, sorts the title keys, sets report date/source, and bumps
`generated` monotonically in UTC milliseconds. Review the diff before committing
real reports to main. The synthetic command above is an example, not a report to
publish. Status, ID, version, model, URL and output size are validated before an
atomic write. No automation publishes the file.

## Host verification and numbers

Before implementation, the existing compatibility harness, game-library harness,
localization-catalog harness and integration-locales Python test all passed.
The new C++ freshness harness failed at loading missing optional fields; the Bun
CLI test failed because the tool did not exist. After implementation all six
harnesses/tests pass, with no regression in the four baseline checks. The Bun
test has 17 expectations, exercising insert/update, ordering, console separation,
metadata, timestamp bump and rejection without file mutation. Freshness tests
cover newest-wins in all three source positions, fractional ordering, malformed
JSON/date fallback, optional-field types and metadata retention. The prior
compatibility harness still checks bundled data, update IDs, foreign provenance,
unknown statuses and oversized/malformed documents. Bundled file: 3449 bytes.

The requested NXbox command was executed both before and after:
`python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`.
Both report `ImportError: Start directory is not importable: 'tests/port'` because
that directory does not exist in Jadyx. No NXbox failure counts are claimed here.

Reproduce from the repository root, with `JSON_INCLUDE` pointing to an **existing**
nlohmann include directory (the project's existing dependency; no download):

```sh
for test in compatibility compat-fresh; do
  clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror -Isrc \
    -isystem "$JSON_INCLUDE" "tools/xbox/tests/$test.cpp" \
    src/jadyx/library/compatibility.cpp -o "/tmp/jadyx-jx19-$test"
  "/tmp/jadyx-jx19-$test"
done
bun test tools/xbox/tests/compat-add.test.ts
python3 -B tools/xbox/tests/integration-locales.py
clang++ -std=c++20 -Isrc tools/xbox/tests/game-library.cpp -o /tmp/jadyx-jx19-library
/tmp/jadyx-jx19-library
clang++ -std=c++20 -Isrc tools/xbox/tests/localization-catalogs.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp -o /tmp/jadyx-jx19-locales
/tmp/jadyx-jx19-locales
```

C++ host checks use warnings as errors, including shadow/conversion warnings.
Prettier checked the Bun files and JSON; clang-format formatted C++ changes.
MSVC/UWP compilation, WinRT deadline/cancellation, actual atomic replacement and
visual layout remain unverified on this macOS host.

## Exact console test (pending)

1. On the Windows build machine, close instances using the package layout, build
   with `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`,
   increment the delivery version per project policy, then run
   `powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library`.
   Install `build-uwp\package\jadyx.appx` through Device Portal in Game mode.
2. Back up any existing `LocalState/jadyx/compat_cache.json`. Remove that cache
   only, disconnect networking, and open the library. Confirm badges from the
   bundle, responsive navigation, and no error popup. Focus an entry with a
   version and one without; verify the suffix only on the versioned report in
   English, Spanish and Brazilian Portuguese, in both themes.
3. Close the app. Through Device Portal, upload a valid copy of the bundle to
   `LocalState/jadyx/compat_cache.json`, set `generated` to a later UTC timestamp,
   and change one existing Jadyx report's status/version to recognizable test
   values. Reopen offline: verify the cache wins. Reopen online while main's
   document is older: verify the cache remains unchanged and the log contains
   exactly one `Compatibility refresh: ignored older download` line.
4. Close the app and set the test cache's timestamp earlier than the bundle.
   Reopen offline: the bundle must win. Repeat with truncated JSON and with an
   invalid `generated`: both must fall back to the bundle without an error UI.
5. Once a maintainer has **manually reviewed and published** a genuinely newer
   report on main, reopen online without reinstalling the app. Verify that
   report appears, the cache matches the valid downloaded document, and reopening
   offline preserves it. Repeat after another newer curated report to exercise
   replacement of an existing cache, rather than just initial creation.
6. Block access to raw.githubusercontent.com with a silently dropping network
   rule, reopen, and confirm navigation remains responsive and one ignored-refresh
   line appears after approximately 3 seconds (poll/scheduling tolerance). Close
   during the request and confirm shutdown cancels promptly. Restore networking
   and the backed-up cache. Invalid remote bodies are covered by host selection
   tests; do not corrupt the public curated file to manufacture a console failure.

No FPS, gameplay compatibility or console result is certified by host tests.
