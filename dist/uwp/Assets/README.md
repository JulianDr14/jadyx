# Brand assets

The Jadyx logo, wordmark, tiles and splash screen are by
[@dramon8945](https://www.instagram.com/dramon8945/):

- `JadyxLogo.png`, `JadyxWordmark.png`
- `Square150x150Logo.scale-200.png`, `Square44x44Logo.scale-200.png`, `StoreLogo.scale-200.png`
- `Wide310x150Logo.scale-200.png`, `SplashScreen.scale-200.png`

The `.scale-200` suffix is the file's real size (200% of the base size the manifest names);
`tools/xbox/package-appx.ps1` builds the `resources.pri` that maps one to the other.

`InputPrompts/` is third-party; see its own README.

`Fonts/` holds [Inter](https://rsms.me/inter/) by Rasmus Andersson (Regular, Medium, SemiBold,
Bold), under the SIL Open Font License 1.1; see `Fonts/OFL.txt`.
