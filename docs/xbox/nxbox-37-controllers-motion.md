# Controller preference and synthetic motion port

## Restricted controller preference

Jadyx already supports per-game manual controller styles and sideways single
Joy-Con mapping. Its automatic policy keeps an accepted Pro controller, or picks
handheld before a single Joy-Con. NXbox's inventory A rows 120 and 124–125 identify
the missing title policy and motion path. Reference: NXbox commits 3946ecf16 and
a2aafaeb6 (read locally, GPL-3.0-or-later); no game data was copied.

`joycon_preference=1` in the installed package's `boot.cfg` enables the policy.
It defaults off and changes no saved options. In Automatic mode only, IDs
`010003F003A34000` and `0100187003A36000` prefer dual Joy-Cons, then a right Joy-Con
when docked. Other IDs and explicit styles keep Jadyx's behavior. The existing
single Joy-Con mapping makes the left stick drive the right Joy-Con's only stick.

Host baseline: four existing portable gates (controller-style, controller-selection,
player-slots, stick-navigation) pass on macOS. The requested `tests/port` unittest
discovery cannot run: that directory belongs to NXbox, not this repository.
The new controller-preference regression failed to compile before the change
(missing policy), then passed under Clang C++20 with `-Wall -Wextra -Wshadow
-Wconversion -Werror`. It checks both IDs, switch off, undocked fallback, unrelated
IDs, empty styles and the single-stick mapping. No Windows build or console run.

Console test (pending): install a normal Jadyx package with the switch absent,
then with `joycon_preference=1`. Use a synthetic HID homebrew/policy fixture with
each listed program ID: request Pro+dual, then handheld+left+right while docked.
With Automatic expect dual then right, and check left-stick directions and face/
SL/SR buttons in the homebrew readout. With switch absent expect the old policy.
Repeat undocked (handheld fallback), with an unrelated ID, and with explicit Pro,
left and right styles (manual choice wins). Connect a second pad and reconnect
both: player ownership must remain stable. Collect `jadyx_log.txt` and
`jadyx_diag.txt`; style reconnection logs must match the fixture. This verifies
policy, not commercial-title compatibility.

Host command:

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror -Isrc \
  tools/xbox/tests/controller-preference.cpp -o /tmp/jadyx-controller-preference
/tmp/jadyx-controller-preference
```
