# Aion2Pipe project instructions

Publication exception (2026-10-03): the owner explicitly requested publishing the existing private files to the public repository after being informed that they contain credentials. Exact authorized paths are recorded in `.codex/published-private-files.txt`; these paths override the private-file exclusion rules below and in the packaging skill. Do not automatically expand this exception to new private files. Packaging still must not modify or stage files.

## Packaging and source hygiene

Before creating a release ZIP, repackaging a client, or changing packaging scripts/output locations, read and follow [.codex/skills/aion2pipe-package/SKILL.md](.codex/skills/aion2pipe-package/SKILL.md).

Use its `scripts/package.ps1` entry point. Release outputs belong in ignored `release/`, native build outputs in `build/`, and temporary verification files in `artifacts/`. Do not create versioned `dist-*` directories or release files at the project root. Keep actual credentials in ignored `private/`.

Every release directory and ZIP name must contain both the explicit version and the packaging timestamp: `Aion2Pipe-<version>-<yyyyMMdd-HHmmss>-UTC8-win64-portable`. Let the standard packaging script generate the UTC+08:00 timestamp; do not omit it when repackaging.

Preserve existing user changes. Check Git-visible files before and after packaging; do not stage, commit, force-add ignored files, or clean the working tree as part of packaging. The skill defines the complete directory and verification rules.

Before source upload or for packaging preflight only, run `.\.codex\skills\aion2pipe-package\scripts\package.ps1 -CheckOnly`. Resolve reported generated/private files without deleting user data or silently changing the Git index.
