<!--
SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# CI y releases

## Ramas

- Se trabaja en ramas cortas (`feat/…`, `fix/…`, `perf/…`) que entran a `main` por PR.
- `main` es la rama de trabajo. Una release no es una rama: es un tag `vX.Y.Z` sobre `main`.

## Workflows

| Workflow | Cuándo | Qué hace |
|---|---|---|
| `ci.yml` | Cada PR y cada push a `main` (salvo cambios que solo tocan docs) | Compila `jadyx` para UWP y Mesa `spirv_to_dxil.dll`; prueba `tools/xbox/release.py` |
| `ci.yml` a mano | Actions → CI → Run workflow, en cualquier rama | Lo mismo y además un appx firmado de prueba (artifact `jadyx-package-temporary-signing`, 14 días), usando por defecto un certificado efímero |
| `release.yml` | Actions → Release → Run workflow, en `main`, con la versión (`0.3.0`) | Valida la versión, compila, firma, sube la versión del manifest en un commit, crea el tag y publica la GitHub Release |
| `build.yml` | Lo llaman los dos anteriores | La build y el empaquetado |

La release solo crea commit, tag y página si la build y la firma salieron bien. El push del
commit y del tag es atómico: si `main` avanzó mientras compilaba, no se publica nada y hay que
volver a lanzarla.

## Versiones

- Una release pública es `MAJOR.MINOR.PATCH` y se empaqueta como `MAJOR.MINOR.PATCH.0`.
- Las features suben MINOR (`0.4.0`) y los fixes PATCH (`0.3.1`).
- El cuarto número es para los paquetes de fix entre releases (`0.3.0.1`, `0.3.0.2`…), como hasta
  ahora.
- `release.py version` rechaza una versión que no sea mayor que la del manifest y que todas las
  releases publicadas, porque la consola solo actualiza hacia arriba.
- La versión se sube con
  `release.py bump X.Y.Z.0 --manifest dist/uwp/AppxManifest.xml --issue-template .github/ISSUE_TEMPLATE/bug_report.yml`,
  que también la añade la primera al desplegable de versiones del Bug Report (los formularios de
  issues solo admiten opciones fijas). `release.yml` no publica si la versión no está en ese
  desplegable.

## Qué lleva una release

- `Jadyx_X.Y.Z.0_x64.appx`: `package-appx.ps1 -Release`, es decir, la biblioteca (`library=1`,
  `play=1`) y nada más. `-Release` rechaza `-Game`, `-BootNro`, `-Profile`, `-Hunt`,
  `-GraphicsCapture`, `-DebugLayer`, `-BootCfg` y las sondas, y falla si el `boot.cfg` final trae
  otra línea. Los diagnósticos (DRED, perfiladores, menús de captura) están apagados sin su línea
  de `boot.cfg`; el bug tracker queda encendido, que es su valor por defecto.
- `Microsoft.VCLibs.x64.14.00.appx`: el framework del CRT, dependencia obligatoria.
- `Jadyx.cer`: el certificado de firma.
- `Jadyx_X.Y.Z.0_symbols.zip`: `jadyx.pdb`, para resolver los crash logs (`pdb_symbols.py`).
- `build-info.json` (versión, commit, run) y `SHA256SUMS.txt`.

Las notas salen de los commits convencionales de `main` (`--first-parent`) desde la release
anterior: `feat` → New, `fix` → Fixes, `perf` → Performance. `chore`, `docs`, `ci`, `build`,
`test`, `style`, `refactor` y los merges de upstream no aparecen. El texto de *highlights* se
pone encima.

## Firma

CI firma con el certificado del proyecto (`CN=JadyxDev`), guardado en dos secrets del repo:

- `JADYX_PFX_BASE64`: el `.pfx` en base64.
- `JADYX_PFX_PASSWORD`: su contraseña.

Es el mismo certificado del PC de desarrollo, así que un paquete de CI actualiza encima de uno
local y al revés. Para cargarlo o renovarlo:

```powershell
$pw = Read-Host -AsSecureString "PFX password"
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=JadyxDev' | Select-Object -First 1
Export-PfxCertificate -Cert $cert -FilePath jadyx.pfx -Password $pw
[IO.File]::WriteAllText("$PWD\jadyx.b64", [Convert]::ToBase64String([IO.File]::ReadAllBytes("$PWD\jadyx.pfx")))
gh secret set JADYX_PFX_BASE64 --body (Get-Content -Raw jadyx.b64)
gh secret set JADYX_PFX_PASSWORD   # la escribe en el prompt, la misma de arriba
Remove-Item jadyx.pfx, jadyx.b64
```

No se pasa nada a `gh` con `|` desde Windows PowerShell: la tubería cambia el encoding y le
agrega un salto de línea, y el secret ya no coincide con el certificado o la contraseña.

El certificado actual vence el 16/10/2027. Los paquetes firmados antes de esa fecha siguen siendo
válidos, porque la firma lleva timestamp.

La ejecución manual de CI usa por defecto un certificado autofirmado temporal de 30 días y adjunta
su `.cer` al artefacto `jadyx-package-temporary-signing`. Es solo para pruebas: no comparte la clave
pública fijada por el actualizador ni permite actualizar una instalación firmada con el certificado
del proyecto. Para generar un paquete compatible con instalaciones existentes, desactiva
`temporary_signing` y configura los dos secrets del certificado del proyecto. Las releases nunca
usan el certificado temporal.
