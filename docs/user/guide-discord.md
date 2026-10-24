<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Discord #install-help pins

Post each block below as a separate message. The headings and fences are editorial and are not part of the messages.

## Message 1 of 3

```text
JADYX · 1/3 · INSTALL
Full guide (English): https://github.com/JulianDr14/jadyx/blob/main/docs/user/guide.md
Português: https://github.com/JulianDr14/jadyx/blob/main/docs/user/guide.pt-BR.md
Español: https://github.com/JulianDr14/jadyx/blob/main/docs/user/guide.es.md

You need an Xbox Series X/S already in Developer Mode and a computer on the same local network. Use only dumps from your own Switch and games you own. Never share keys, firmware or games here.

1. Get Jadyx_<version>_x64.appx and Microsoft.VCLibs.x64.14.00.appx:
https://github.com/JulianDr14/jadyx/releases/latest
2. Xbox Dev Home > Remote Access: enable Xbox Device Portal; note its address and login. Open that address (usually https://<xbox-ip>:11443) on your computer. Proceed past the local certificate warning only after checking the Xbox address; sign in.
3. Device Portal Home > Add: select Jadyx APPX > Next; add VCLibs as a dependency > Start. Wait until Jadyx appears in Dev Home. No .cer needed on Xbox dev mode.
4. Xbox Dev Home > Jadyx > View details > App type: Game. Check it! Game gives about 5 GB (5120 MiB) RAM, App only about 1 GB. RAM is separate from file storage.

To update later, install over the existing app. Do NOT uninstall first: that deletes internal data, including saves.
```

## Message 2 of 3

```text
JADYX · 2/3 · YOUR FILES
Device Portal installs the app. Jadyx Portal imports files; it is a different website.

1. Open Jadyx > Settings (bumpers) > File manager > Jadyx Portal (Start portal). Open the address it shows on your computer and enter the Connection code. Generate a new code if expired.
2. Browser: System > Keys > Choose files. Pick your prod.keys and, if needed, title.keys (or their ZIP). Upload and review > check versions/warnings > Confirm installation.
3. System > Firmware: select your firmware ZIP or extracted NCA files > Upload and review > check > Confirm installation. Import compatible keys FIRST. Wait for Installation complete. In Jadyx, check Keys ready and Firmware: … files.

GAMES: Internal dev-mode storage may have only a few GB free. Keep large games on USB. Copy your complete NSP/XCI dumps into a folder on it (NRO homebrew also works). Use storage supporting files over 4 GiB, such as NTFS. Connect as media; NEVER choose “games & apps” or format it. In Jadyx: Settings > File manager > Add a games folder; pick that folder. Games stay on USB; leave it connected.

For a smaller base NSP: browser Content > Games > Choose NSP files > Upload / resume. Wait for validation. This copies internally; no XCI or updates here.
For updates/DLC you own: library > Menu on the base game > Updates or DLC > Add content…; choose USB / original folder · no copy or Internal storage, select a complete NSP/XCI, review and confirm. Only one update is active.

Own keys copied manually go in LocalState\jadyx\keys; the app seeds/imports keys there. Device Portal game copies go in LocalState\games. Close Jadyx before manual copies and reopen afterward. See full guide for the file explorer route.
```

## Message 3 of 3

```text
JADYX · 3/3 · PLAY & HELP
Library > X rescans folders. Highlight a game > A to play. First visits can stutter while shaders compile. Hold View + Menu for the paused menu; Resume continues, Back to home returns to the library. Wait for the return to finish before closing.

Series S uses the same installer and Game setting. Performance/compatibility can differ from Series X; tell us your model.

COMMON PROBLEMS
• Microsoft.VCLibs prompt: supply the x64 VCLibs APPX as a dependency in Device Portal.
• “The existing keys cannot read this content”: check your own compatible prod.keys and complete dump. A rights-protected update without a ticket needs its corresponding key in your own title.keys. Reimport both, retry; never post them.
• Firmware unreadable/incomplete: import compatible keys, then complete firmware from your own console.
• Upload fails: check free INTERNAL dev-mode disk space in Device Portal. A full disk prevents uploads; use USB for big games and preserve saves when freeing space.
• bad allocation / closes: check App type: Game. Even then, some games exceed about 5 GB RAM.
• USB game missing/unreadable: reconnect, re-add its folder, check the copy finished.

Black screen or crash? Get BOTH logs:
Device Portal > File explorer > LocalAppData > Jadyx's package > LocalState > jadyx > log
LocalState\jadyx\log\jadyx_diag.txt
LocalState\jadyx\log\jadyx_log.txt
Include Jadyx version, Series X/S, Game setting, steps and result. Never include keys, firmware or games. Full English/PT/ES guides are linked in message 1.
```
