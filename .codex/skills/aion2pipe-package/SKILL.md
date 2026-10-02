---
name: aion2pipe-package
description: Build, verify, and package Aion2Pipe portable Windows releases without polluting the source directory or Git uploads. Use for release ZIP creation, distribution, repackaging, and changes to packaging scripts or output layout.
---

# Package Aion2Pipe

Create a self-contained portable release with `scripts/package.ps1`.

## Directory and Git rules

Use the repository root containing this skill; never assume a drive letter or copy a whole working directory into a package.

| Content | Required location |
| --- | --- |
| C++ build and intermediate files | `build/` |
| Downloaded dependencies | `third_party/` |
| Python environment and chat build output | `chat_bridge/.python/`, `chat_bridge/.venv-local/`, `chat_bridge/build/`, `chat_bridge/dist/` |
| Complete release directory and ZIP | `release/Aion2Pipe-<version>-win64-portable/` and the matching `.zip` |
| Temporary scripts, test reports, screenshots, captures and packaging logs | `artifacts/packaging/<version>/` |
| Actual credentials and private build inputs | `private/` |

- These output locations must be ignored by Git before any build starts. An ignored file already tracked by Git is still uploaded: reject tracked generated/private files too.
- Do not create `dist-*`, `Aion2Pipe-*-win64`, version directories, ZIPs, logs, manifests or checksum files at the repository root. Existing `.gitignore` patterns for old directories are defensive compatibility, not permission to create more.
- Keep reusable packaging helpers in this skill's `scripts/` directory. Temporary helpers belong in `artifacts/packaging/<version>/`; do not add a new one-off `scripts/package-<fix-or-version>.*` for each release. `scripts/package-multi-pc-fix.py` is historical and must not be used as the release workflow.
- Before building, record the Git-visible file list and contents. Existing user changes are allowed; never reset, clean, stage, commit, or overwrite them to obtain a clean status.
- After packaging, verify the Git-visible list and contents are unchanged. The standard script performs this check. Any new visible file or modified source stops delivery until explained and corrected; do not hide unexpected source changes by adding broad ignore patterns.
- Leave an unsuccessful staging directory under `release/` for diagnosis and choose a fresh version on retry. Never automatically delete previous releases or caches, move the source checkout, or stop a running client. Any deliberate cleanup must first check resolved paths and running processes.
- Source uploads contain source, tests, reusable scripts, docs, static data and the skill. Never force-add ignored files or upload release binaries through the source commit: binaries can contain injected credentials.

For an explicit request to modify the build process, finish the intended source edits before taking the packaging baseline. For Git-upload cleanup without a build, read [the repository upload guide](../../../docs/GIT_UPLOAD.md) and verify `git add --dry-run .` without actually staging files.

Preserve these product invariants:

- The public build is virtual-adapter-only: do not add accelerator process detection, local relay ports, Clash, port 7897, or whole-process `local:AION2.exe` redirection.
- Login, world, and chat upstream connections must use the active Windows route so every accelerator is handled through its virtual adapter/TUN mode.
- Never copy machine logs, captures, or generated user data into the release. Never commit real credentials; a deployment-only MQTT configuration may be injected from ignored `private/` during packaging.
- Build the chat bridge only from `chat_bridge/source` in this repository. Never read `E:\project\aion2` or an old `dist-integrated-*` directory.
- Keep real MQTT configuration only in ignored `private/chat_bridge/mqtt.private.local.json`. The tracked source contains an example with placeholders.
- Runtime paths must be relative to the executable directory. Reject development paths found in release configuration or launcher files.
- Run the full CTest suite before packaging. A failed build or test means no ZIP is published.
- Include the bundled chat bridge, WinDivert files, licenses, docs, a Chinese quick-start, a manifest, and SHA-256 checksums.
- Do not overwrite an existing release directory or ZIP; choose a new version instead.

Run from the repository root:

Before uploading source, or to check the rules without building or creating output, run:

```powershell
& .\.codex\skills\aion2pipe-package\scripts\package.ps1 -CheckOnly
```

This read-only check rejects Git-visible generated files, including previously tracked files that now match ignore rules. It does not stage or untrack anything, and does not scan credential contents. A passing check is not a guarantee that source code contains no secrets.

To build a release:

```powershell
& .\.codex\skills\aion2pipe-package\scripts\package.ps1 -Version '<version>'
```

The script writes unpacked and ZIP artifacts under `release/`. Report both paths, the main executable hash, test result, Git cleanliness check, and the requirement to close older instances before switching versions. A passing Git check means packaging added no Git-visible changes relative to its baseline, not that the user's working tree was initially clean.
