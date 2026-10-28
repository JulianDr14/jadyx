# Jadyx game compatibility

Updated **28 October 2026**. This page collects console tests and community reports,
not an exhaustive compatibility database. For the latest comments and additional games, visit
[the compatibility forum](https://discord.com/channels/1561574819672100994/1561576709772419083).
[Back to the project README](../../README.md#games).

These results describe particular test sessions, not a guarantee of completion or identical
performance on another console, game update, Jadyx build, or set of mods. Rows marked
*community* are user reports rather than independently reproduced benchmarks. An unspecified
build stays unspecified; a game update number is not the emulator version.

First-time shader compilation can cause stutters. Where a report does not distinguish shader
compilation from other frame drops, no claim is made that the reported FPS excludes it.
The README's internal measurements were taken after shader compilation, in Game mode.

## Recent additions

| Game | Console | Performance | Notes |
| --- | --- | --- | --- |
| Pokémon Legends: Arceus *(community)* | Series S | **30 FPS**, reported stable | Jadyx 0.3.9.0; tester reports rare frame drops and no graphical issues so far. [Report](https://discord.com/channels/1561574819672100994/1563655903243272222) |
| Yoshi’s Crafted World *(community)* | Series S | **60 FPS**, reported stable | Jadyx 0.3.9.0; game update 1.0.1; about 3.45 GB RAM reported. [Report](https://discord.com/channels/1561574819672100994/1564796636448948304) |
| Persona 4 Golden *(community)* | Series S | **60 FPS**, reported stable | Jadyx 0.3.9.0; no bugs observed during the reported testing. [Report](https://discord.com/channels/1561574819672100994/1564898078626943096) |
| Melty Blood: Type Lumina *(community)* | Series S | **60 FPS** | Jadyx 0.3.9.0; tester reports a perfect 60 FPS. [Report](https://discord.com/channels/1561574819672100994/1564712234893181018) |
| Bomb Rush Cyberfunk *(community)* | Series S | **30 FPS**, reported stable | Jadyx 0.3.9.0; no issues mentioned in the report. [Report](https://discord.com/channels/1561574819672100994/1564946917631463545) |
| New Super Mario Bros. U Deluxe *(community)* | Series X | **60 FPS**, reported stable | Jadyx 0.3.9.0; game 1.0.0; gameplay, map and menus tested. [Report](https://discord.com/channels/1561574819672100994/1564688222028832838) |
| New Super Mario Bros. U Deluxe *(community)* | Series S | **60 FPS** most of the time | Jadyx build not stated; no graphical glitches reported; session resume also reported working. [Report](https://discord.com/channels/1561574819672100994/1563824547587821568) |
| Cuphead *(community)* | Series X | **60 FPS** in boss fights, map and menus | Jadyx 0.3.9.0; game update 1.3.7; FPS drops reported on loading screens. [Report](https://discord.com/channels/1561574819672100994/1564671465184563210) |
| OMORI *(community)* | Series X | **60 FPS**, reported stable | Jadyx 0.3.9.0; game update 1.0.4. [Report](https://discord.com/channels/1561574819672100994/1564703059664179330) |
| JoJo’s Bizarre Adventure: All-Star Battle R *(community)* | Series X | **30 FPS**, reported stable | Jadyx 0.3.9.0; base game 1.0. [Report](https://discord.com/channels/1561574819672100994/1564699377367384184) |
| Diablo III: Eternal Collection *(community)* | Series S | **60 FPS** | Jadyx build not stated; no graphical errors reported. [Report](https://discord.com/channels/1561574819672100994/1564295698613272697) |
| Captain Toad: Treasure Tracker *(community)* | Series S | **60 FPS** | Jadyx build not stated; tested from an external HDD. [Report](https://discord.com/channels/1561574819672100994/1564171670581944360) |
| Paper Mario: The Origami King *(community)* | Series S | **30 FPS**, reported locked | Jadyx build not stated; game update 1.0.1; tester reports almost no rendering issues. [Report](https://discord.com/channels/1561574819672100994/1563823217733271602) |
| Super Mario RPG *(community)* | Series S | **47–56 FPS** in gameplay; drops to **37 FPS** | Jadyx 0.3.9.0; heavier enemy scenes and cinematics drop frames; menus at 60 FPS. [Report](https://discord.com/channels/1561574819672100994/1564665606257512682) |

## Earlier results and updated retests

| Game | Console | Performance | Notes |
| --- | --- | --- | --- |
| Mario Kart 8 (32-bit) | Series X | **60 FPS** in races, stable | No graphical errors. Frames of 16.7 ms, slowest 17.1 ms mid-race; rain, reflections and particles included |
| Mario Kart 8 *(community)* | Series X | **50–60 FPS** in game | Jadyx 0.3.0.0; separate community report |
| Mario Strikers | Series X | **60 FPS** in matches, stable | No graphical errors, special-move cutscenes included. GPU at 14 % |
| Pokémon Let's Go | Series X | **60 FPS**, stable | No graphical errors |
| Mario Wonder | Series X | **55–60 FPS**, stable | No crashes or graphical errors |
| Mario Wonder *(community)* | Series S | **60 FPS** | Latest community report |
| Under Night In-Birth II Sys:Celes *(community)* | Series S | **60 FPS** | In-game; community report |
| Naruto x Boruto: Connections *(community)* | Series S | **30 FPS**, reported stable | No FPS drops reported by the tester |
| Mario 3D World | Series X | **60 FPS**, stable | In-game |
| Hades II *(community)* | Series S | **60 FPS**, reported smooth with no drops | Jadyx 0.3.8.0; community report |
| Bayonetta *(community)* | Series S | **60 FPS**, dropping to **42 FPS** during combos | Jadyx 0.3.8.0; community report |
| Bayonetta 2 *(community)* | Series S | **60 FPS**, dropping to **30–40 FPS** during heavy action and combat | Jadyx 0.3.8.0; community report |
| Super Mario Odyssey | Series X | **45–50 FPS** in very open areas; **60 FPS** in more enclosed areas and in certain levels | In-game |
| 13 Sentinels *(community)* | Series X | **60 FPS** in battles and exploration | Playable |
| NEO TWEWY *(community)* | Series X | **20–30 FPS** in game, 50 in menus | In-game; drops to 10–17 FPS while new shaders compile |
| Pokémon Sword *(community)* | Series S | **20–30 FPS** | Reported stable apart from roughly one-second stutters, possibly shader compilation. Open areas still to be tested; build not specified |
| Yo-kai Watch 4++ *(community)* | Series S | **27–30 FPS** reported in open areas after the recent update, previously about 20 | Minor model/environment distortions remain in the reported English-patch setup. Exact build of the latest retest not stated. [Report](https://discord.com/channels/1561574819672100994/1561826789980901377/1564662987162132603) |
| Hyrule Warriors: Age of Calamity *(community)* | Series X | **30 FPS** in cutscenes; around **20 FPS** with drops in gameplay | Jadyx 0.3.8.0; gameplay reached after shader compilation. Older builds failed after the first cutscene. [Report](https://discord.com/channels/1561574819672100994/1564802263686586578) |
| The Legend of Zelda: Breath of the Wild | Series X | **27–29 FPS** observed in the open world with shaders compiled | Previously about 23 FPS in the tested open area. Holds 30 FPS in lighter areas; work in progress |
| Zelda TotK | Series X | Renders **without graphical errors**, then **hits the memory wall** | The heaviest loads need more than the app's 5120 MiB, see [The memory wall](../../README.md#the-memory-wall). On PC it reaches the open world at **30 FPS** |


For Hades II and both Bayonetta reports, Series S is inferred from the same tester’s explicit
console identification and other compatibility reports; those three posts do not repeat the model.
Earlier measurements retain their original scope and should not be read as retests of 0.3.9.0.

## Completion and remaining problems

A tester reports completing **Cuphead: The Delicious Last Course**
([completion report](https://discord.com/channels/1561574819672100994/1564443110308847616/1564841590776930446)).
This establishes completion of that DLC in that tester’s session, not completion of every game
or a guarantee of zero bugs.

Mario Odyssey’s existing Series X performance result remains above. A separate tester reported
repeated crashes during the transition to the Metro Kingdom and occasional crashes entering
paintings ([report](https://discord.com/channels/1561574819672100994/1564816931423522897)).
That report does not change the classification of the main Odyssey thread; it records a
progression problem that FPS figures alone would miss.

## Reading the forum labels

Colors and status tags describe different aspects. Green indicates stable playable results with
no relevant errors reported; blue covers minor graphical issues; yellow covers performance
issues beyond shader compilation; orange covers combined performance/graphics problems or
instability; red covers severe failures. The status tag separately records Playable, Ingame,
Boots, Doesn’t boot, or Unplayable. Tester classifications and their latest comments take
precedence over guesses based on a single FPS number.
