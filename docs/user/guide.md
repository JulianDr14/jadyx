<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Jadyx: from installation to your first game

[Português (Brasil)](guide.pt-BR.md) · [Español](guide.es.md)

Follow these three parts in order. You need an Xbox Series X or S already activated in **Developer Mode**, a controller, and a computer on the same local network. Device Portal is the Xbox's management website; Jadyx Portal is a separate website started inside Jadyx for importing files.

Prepare only dumps from **your own Switch and games you own**: `prod.keys`, `title.keys` when needed, firmware, and games. Jadyx includes none of them. Do not request or share these files in support channels.

## Part 1 · Install Jadyx

1. Get `Jadyx_<version>_x64.appx` and `Microsoft.VCLibs.x64.14.00.appx` from the [Jadyx release page](https://github.com/JulianDr14/jadyx/releases/latest). Keep both files on your computer.
2. On Xbox, open **Dev Home**. In **Remote Access**, enable **Xbox Device Portal** and configure its username/password if requested. Note the address shown, typically `https://192.168.x.x:11443`.
3. Open that exact address in your computer's browser. A certificate warning is normal for this local Xbox page: proceed only after checking it is your console's address. Sign in with the Remote Access credentials. You should now see Device Portal.
4. On its **Home** page, choose **Add**, select the Jadyx `.appx`, then **Next**. When asked for dependency packages, add `Microsoft.VCLibs.x64.14.00.appx`, then **Start**. Wait for completion: Jadyx should appear in Dev Home. The `.cer` is **not required on Xbox Developer Mode**; the maintainer confirmed installation with APPX + VCLibs on Series X on 23–24 October 2026.
5. In Xbox Dev Home, highlight Jadyx, open **View details**, and change **App type** to **Game**. Verify it says Game before launching. App mode allows about **1 GB** of memory; Game mode gives Jadyx about **5 GB (5120 MiB)**. This is RAM, not storage for files.

For later updates, install the new APPX over the existing app. **Do not uninstall first**: uninstalling deletes the app's internal keys, firmware, saves and games.

> **Coming soon:** a simpler installation flow. For now, use the APPX + VCLibs steps above.

## Part 2 · Add your own keys, firmware and games

Internal Developer Mode storage may have only a few GB free, even if the Xbox has a large SSD. Keep big games on a USB drive; portal uploads use internal storage.

### Keys and firmware: Jadyx Portal

1. Launch Jadyx. Use the controller bumpers (LB/RB) to reach **Settings**, then open **File manager**. Select **Jadyx Portal**, whose action is **Start portal**. It opens a screen with a local address and **Connection code**.
2. On your computer, open the address shown by Jadyx, not the Device Portal address. Enter the code. Both devices must be on the same local network. If it expires, select **Generate connection code** in Jadyx and try the new code.
3. In the browser, open **System > Keys**. Select **Choose files** and your `prod.keys`, plus `title.keys` if your dump needs it, or a ZIP containing them. Choose **Upload and review**. Review versions and warnings, then **Confirm installation**. Do not accept a warning without checking your source files.
4. In **System > Firmware**, choose your own firmware ZIP or extracted `.nca` files (the browser also offers **Or choose an extracted folder**). Choose **Upload and review**, review the result, then **Confirm installation**. Import keys first: they must be able to read this firmware. Wait for **Installation complete** before leaving.
5. Back in Jadyx's File manager, check **Keys ready** and **Firmware: … files**. The count confirms files are present; it does not guarantee every game will work.

Keys are imported into `LocalState\jadyx\keys` and loaded by the app. If using Device Portal to copy your own keys manually, close Jadyx, open **File explorer > LocalAppData > Jadyx's package > LocalState > jadyx > keys**, upload `prod.keys` and, when needed, `title.keys`, then reopen Jadyx. A random `keys` folder at the USB root is not this location.

### Games: USB for large files, or internal storage

- **USB (recommended for large games):** copy your own complete `.nsp` or `.xci` dumps to a folder on the drive; homebrew `.nro` is also supported. Use a filesystem that permits files over 4 GiB, such as NTFS; FAT32 does not. Connect it to Xbox as media storage. **Do not choose “games & apps” or accept formatting**, which erases the drive. In **Settings > File manager > Add a games folder**, select that folder in the picker. The games stay on USB; keep it connected while playing. Subfolders are scanned too.
- **Browser upload:** in Jadyx Portal, open **Content > Games > Choose NSP files**, select a base-game `.nsp`, then **Upload / resume**. Wait for validation and addition to the library. This route copies to internal storage and accepts base NSP games, not XCI or updates/DLC.
- **Device Portal alternative:** with Jadyx closed, upload your dumps through **File explorer > LocalAppData > Jadyx's package > LocalState > games**. Reopen Jadyx afterward. This is `LocalState\games`, separate from `LocalState\jadyx`.

For your own updates/DLC, first add the base game. In the library, press **Menu** on it, choose **Updates** or **DLC**, then **Add content…**. Choose **USB / original folder · no copy** or **Internal storage**, select a complete NSP/XCI package, review it, and confirm. Keep externally referenced content connected. Only one update is active; DLC can be activated individually. Do not upload updates as base games.

## Part 3 · Play

1. Return to **Library**. If a newly copied game is missing, press **X** to scan again. Highlight the game and press **A** to play. Leave the USB connected if the game or selected content lives there.
2. Wait while shaders compile. First visits to new areas can stutter; compatibility and performance vary by game. Jadyx is a beta.
3. Hold **View + Menu** to open the paused in-game menu. Choose **Resume** to continue or **Back to home** to return to the library; wait for the return to finish before closing the app.

**Series S:** use the same installer and Game setting. There are Series S community reports in the [README](../../README.md#progress), but Series X results do not guarantee the same performance or compatibility on S. Include your model in reports.

### If something goes wrong

- **Dependency prompt / Microsoft.VCLibs missing:** return to Part 1 and supply the x64 VCLibs APPX as a dependency, not as the Jadyx package.
- **“The existing keys cannot read this content”:** recheck/import your own compatible `prod.keys`. If an update dump is rights-protected and contains no ticket, its corresponding title key must be in your own `title.keys`; `prod.keys` alone cannot supply it. Reimport both through Keys, then retry. Also check that your dump is complete. Never post the keys.
- **Firmware unreadable/incomplete:** import compatible keys first, then a complete firmware dump from your own console. A folder containing game NCA files is not firmware.
- **Upload fails / not enough space:** check free internal Developer Mode storage in Device Portal. Uploads can fail when that disk is full. Free space from files you no longer need, preserve saves, and use USB for big games. The main Xbox SSD's advertised capacity is not your available app storage.
- **`bad allocation` / app closes:** verify **App type: Game** first. Even in Game mode, a demanding game can exceed the approximately 5 GB limit.
- **USB game missing/unreadable:** check the connection and reselect the folder with **Add a games folder**. Ensure the copy finished; a file path alone does not grant the app access.
- **Black screen or crash after these checks:** collect both logs below; a black screen alone does not identify the cause.

### Get the logs

Open Xbox **Device Portal > File explorer > LocalAppData**, select Jadyx's package, then **LocalState > jadyx > log**. Download **both**:

- `LocalState\jadyx\log\jadyx_diag.txt`
- `LocalState\jadyx\log\jadyx_log.txt`

Send them to [Jadyx support](https://discord.gg/3BhyyXGxpd), with the app version, Series X/S model, whether it is set to Game, what you did, and what happened. Never include keys, firmware or game files.
