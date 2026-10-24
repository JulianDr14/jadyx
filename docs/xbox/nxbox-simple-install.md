<!--
SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Simple Xbox installation, with the VCLibs dependency preserved

> **Status:** the certificate step is kept in the README and deploy guide for now. Removing it is
> still being validated on more consoles.

## Finding and scope

Jadyx's README had five installation steps and generated release notes had six. Both asked
for three downloads up front: the installer, VCLibs and a public certificate. The change makes
both guides four steps: install, select Game mode, import the owner's keys/firmware, add the
owner's games. The main download remains `Jadyx_<version>_x64.appx`, now labelled
**Jadyx Xbox installer** in release assets. The filename and updater URL do not change.

VCLibs is **still required**, not optional at runtime. The guide requests its separate framework
APPX only if Device Portal asks for a dependency. An already installed compatible version can
satisfy that dependency. The minimum version remains exactly as declared in
`dist/uwp/AppxManifest.xml` (`Microsoft.VCLibs.140.00`, 14.0.33519.0). No CRT DLLs are bundled,
no manifest dependency is removed, and no launch checks in `tools/xbox/package-appx.ps1` change.
This deliberately does not revive the rejected `nxbox/10-simple-install` implementation.
No application, renderer, translation, memory policy or runtime behaviour changes.

## Offline evidence and certificate limits

Inspected the read-only NXbox reference locally (no code copied):

- `README.md`, install section: one emulator APPX through Device Portal, Game mode, own keys,
  games under `LocalState\games` or USB. There is no certificate-upload step.
- `dist/nxbox/AppxManifest.xml`: still declares `Microsoft.VCLibs.140.00` (14.0.30704.0).
  That lower minimum is not copied into Jadyx.
- `.github/workflows/release-nxbox.yml`, publish command: publishes `out/*.appx`, ZIPs and build
  information, without a `.cer` asset. This is source evidence, not a query of live releases.

Jadyx's own signing chain is unchanged:

1. `release.yml` invokes the reusable `build.yml` with `package: true`.
2. `build.yml` supplies the existing distribution PFX to `package-appx.ps1 -Release` and copies
   the signed APPX, exported public `.cer` and VCLibs into its build artifact.
3. `package-appx.ps1` checks the PFX subject against the publisher, signs with SignTool
   `/fd SHA256` and a timestamp, checks its exit status, then exports the public certificate
   separately. Deleting that separate export does not modify the signed APPX bytes.
4. `release.yml` now removes only `out/Jadyx.cer` from the public release staging directory,
   before generating the final checksums. The build artifact and local package retain it for
   Windows sideloading. Symbols, VCLibs, build information and signed update metadata remain.
   Existing downloaded-asset comparisons and updater signature verification remain intact.

**What is not established:** signing a package does not prove that a target trusts its signer.
The old packaging comment and deployment guide asserted that Xbox needs a separate certificate
upload, but supplied no Microsoft citation. NXbox's source shows a different documented flow;
that is also not proof for a clean Jadyx console. The public Xbox guide now follows the requested
APPX-first flow with no certificate step; this is a documentation/distribution change awaiting
hardware acceptance, not a claim that certificate trust is universally bypassed.

No offline copy of the relevant Microsoft documentation was found in the inspected Jadyx/NXbox
documentation or local temporary/cache file inventory. Network access was prohibited. Therefore
no Microsoft statement about Xbox's signer trust requirements is asserted or quoted here.
These primary-source pages are follow-up references, **not fetched or verified in this task**:
[Device Portal for Xbox](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-xbox)
and [Sign an app package](https://learn.microsoft.com/en-us/windows/msix/package/sign-app-package-using-signtool).
The browser's Device Portal HTTPS certificate warning is separate from package signing trust.
Windows certificate installation is outside this Xbox guide.

## Host verification

Work started on `nxbox/11-simple-install-v2` at `main` commit `0fc5c0183694d4cc91b062f00a5641315de30003`.
No Windows build, PC deployment, console, network, signing, push or release was run.
NXbox's `tests/port` suite is not present in Jadyx; the Jadyx tests below were used instead.

Before and after: **14/14 existing Python fixture scripts passed**, zero new failures. The release
fixture intentionally prints two validation errors while testing rejection; its final result is
`release: ok` with exit status 0. Reproduce from the repository root:

```sh
for name in release address-wait-trace draw-preparation-report dxil-branches dxil-probe \
  gc-trace material-distance material-intermediates material-probe-parser material-probe \
  material-reflections material-trace noperspective-interpolation pipeline-build-trace; do
  python3 -B "tools/xbox/tests/$name.py" || exit 1
done
```

A four-case host contract check failed **3/4 before** the change and passed **4/4 after**:
README has four steps/no `.cer`/conditional VCLibs/internal games path; generated release
instructions have four steps/no `.cer`/conditional VCLibs; public assets exclude `.cer` before
checksums and label the installer; manifest, packager and reusable build remain byte-identical
to `main`. This checks documentation and workflow structure, not deployment success.
The contract check is reproduced below (uses the host's existing PyYAML; no project dependency
added). Extract this block to a temporary `.py` file and run `python3 -B <file>` at the repo root.

```python
from pathlib import Path
import re
import subprocess
import unittest
import yaml

class InstallContract(unittest.TestCase):
    def test_readme(self):
        section = Path('README.md').read_text().split('## Running it on a console\n', 1)[1].split('\n## ', 1)[0]
        self.assertEqual(len(re.findall(r'^\d+\. ', section, re.M)), 4)
        self.assertNotIn('.cer', section)
        self.assertIn('If Device Portal asks for a dependency', section)
        self.assertIn('LocalState\\games', section)

    def test_release_instructions(self):
        text = Path('.github/workflows/release.yml').read_text()
        section = text.split('          ## Install\n', 1)[1].split('          ## Reporting bugs', 1)[0]
        self.assertEqual(len(re.findall(r'^          \d+\. ', section, re.M)), 4)
        self.assertNotIn('.cer', section)
        self.assertIn('If Device Portal asks for a dependency', section)

    def test_assets(self):
        workflow = yaml.safe_load(Path('.github/workflows/release.yml').read_text())
        steps = workflow['jobs']['publish']['steps']
        prepare = next((step.get('run', '') for step in steps if step.get('name') == 'Prepare Xbox release assets'), '')
        self.assertIn('rm out/Jadyx.cer', prepare)
        self.assertLess(next(i for i,s in enumerate(steps) if s.get('name') == 'Prepare Xbox release assets'), next(i for i,s in enumerate(steps) if s.get('name') == 'Include update metadata in checksums'))
        publish = next(step['run'] for step in steps if step.get('name') == 'Publish')
        self.assertIn('#Jadyx Xbox installer', publish)

    def test_packaging_unchanged(self):
        for name in ['tools/xbox/package-appx.ps1', 'dist/uwp/AppxManifest.xml', '.github/workflows/build.yml']:
            self.assertEqual(Path(name).read_bytes(), subprocess.check_output(['git', 'show', 'main:' + name]))
        self.assertIn('PackageDependency Name="Microsoft.VCLibs.140.00"', Path('dist/uwp/AppxManifest.xml').read_text())

unittest.main()
```

The actual workflow release-notes shell block was executed offline with a synthetic version;
it produced four steps and the expected APPX filename, with no `.cer` instruction. The asset
preparation block was exercised on synthetic files: it preserved the APPX and VCLibs bytes and
removed the `.cer` before checksum enumeration. Bash blocks passed `bash -n`, the workflow
parsed with the host's existing PyYAML, and `git diff --check` passed. GitHub Actions and actual
signed-release publication remain untested. Prettier passed for the changed workflow and new
document; its warnings in the three edited existing Markdown files also reproduce on `main`,
so no unrelated whole-file formatting changes were included.

## Pending console test

All the following are still required; **none was run here**:

1. On a Series X or S in Dev Mode, record OS version, installed Jadyx version and the installed
   `Microsoft.VCLibs.140.00` version. Use a test console where Jadyx's signing certificate has not
   been manually imported to test clean signer trust. Do not remove a shared framework or
   alter the owner's trust stores to manufacture this condition.
2. Use a normal signed release APPX with the same distribution signer and a version higher than
   any installed Jadyx. In Device Portal (`https://<console-ip>:11443`) choose **Add**, select only
   `Jadyx_<version>_x64.appx`, and proceed through installation without uploading a `.cer`.
   With compatible VCLibs already installed, require success with that single upload. Save the
   deployment result/HRESULT. A signer trust rejection fails this gate; record it before
   changing the instructions or making any claim about Microsoft-supported behaviour.
3. On a separate test environment without compatible VCLibs, try the APPX alone, record the
   dependency prompt/error, then add the release's `Microsoft.VCLibs.x64.14.00.appx` as a
   dependency and retry. Require successful installation and activation. Do not certify this
   fallback solely from a console that already has the framework.
4. In Dev Home select Jadyx → **View details** → **App type: Game**. Launch and require a
   responsive library. Import only the tester's own keys/firmware through File manager or the
   Jadyx Portal. Confirm library discovery of the tester's own content in `LocalState\games`
   and in a selected USB folder. Exercise the project's own homebrew for launch/render checks.
   Record `LocalState\jadyx\log\jadyx_diag.txt` and `jadyx_log.txt`; reject missing-DLL,
   activation or renderer errors. Nothing private is added to the package or repository.
5. Install a newer same-signer release over an existing installation without uninstalling or
   uploading `.cer`; verify settings, saves and internal games survive, then restart the console
   and launch again. Repeat clean-install/dependency/upgrade coverage on Series X and Series S
   when available, recording each model and result. One model's result does not certify both.

Pending release-machine verification: run the unchanged signing/package pipeline, inspect the
APPX manifest and payload to confirm the framework dependency and absence of bundled CRT DLLs,
then confirm the public assets, hashes, installer label and updater URL in a draft release.
The offline host checks cannot certify those generated binaries or a real Device Portal flow.
