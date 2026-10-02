# Aion2Pipe project instructions

## Packaging and source hygiene

Before creating a release ZIP, repackaging a client, or changing packaging scripts/output locations, read and follow [.codex/skills/aion2pipe-package/SKILL.md](.codex/skills/aion2pipe-package/SKILL.md).

Use its `scripts/package.ps1` entry point. Release outputs belong in ignored `release/`, native build outputs in `build/`, and temporary verification files in `artifacts/`. Do not create versioned `dist-*` directories or release files at the project root. Keep actual credentials in ignored `private/`.

Preserve existing user changes. Check Git-visible files before and after packaging; do not stage, commit, force-add ignored files, or clean the working tree as part of packaging. The skill defines the complete directory and verification rules.

Before source upload or for packaging preflight only, run `.\.codex\skills\aion2pipe-package\scripts\package.ps1 -CheckOnly`. Resolve reported generated/private files without deleting user data or silently changing the Git index.
