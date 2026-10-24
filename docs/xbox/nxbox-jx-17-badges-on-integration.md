# jx-17: merge compatibility badges into integration

Merged `nxbox/15-compat-badges` onto the requested `nxbox/integration` baseline,
preserving FTP, USB play in place, parallel shaders, resolution options, the guide
and package launch checks. Resolved the four expected conflicts by retaining both
features and ordering all 677 entries in each locale exactly like `localization.h`.
The catalog-order regression fails before resolution and passes afterward.

The optional `nxbox/16-themes` tip is already an ancestor and has no theme changes.
Existing host gates have no new failures; the two known macOS failures remain.
No network, PC runtime, Xbox, push or full build was used.

See [integration notes](nxbox-integration-notes.md) for the per-file resolutions,
before/after numbers, exact host commands and pending console integration test.
