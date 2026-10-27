# CLAUDE.md

Rules for this repo (MyCam: Android/iPhone → Windows 11 webcam). For background read TASKS.md,
IMPROVEMENTS.md and docs/ARCHITECTURE.md — this file is rules only.

- **Repo is public.** Verify with `gh repo view --json visibility`, don't trust this line. Never commit
  `inspo/`, `screenshots/`, `release/`, `keystore.properties`, `*.jks`, or any real email/secret.
- **Commit author = whoever is working**, from local `git config` — never override it on the command line.
  Always a GitHub no-reply email, never a real one: it can't be undone once pushed.
- **Fetch before any destructive git rewrite** (force-push, history rewrite) on a shared branch, and
  compare against `origin` first. A stale force-push has already dropped a collaborator's commits once.
- **Git workflow** (full version: `docs/git_rules.md`, gitignored but still tracked):
  - `main` must always build; no direct commits — every change via a branch + PR; a broken `main` is the
    top priority. Branch per change off current `main`: `type/short-desc` (feature/fix/docs/refactor/
    chore/hotfix), small, short-lived, rebased onto `main` as you go, deleted after merge.
  - Commits: Conventional Commits, `type(scope): summary` (feat/fix/docs/refactor/perf/test/chore/ci),
    imperative mood, summary <72 chars, no secrets/keys/build output.
  - PR only when the branch is current, builds, is tested, docs are updated, no debug leftovers; title
    like a commit, body covers what/why/how-tested, `Fixes #N`. Merge only when complete, CI green, one
    approval (self-review OK solo), comments resolved, no conflicts — squash-merge, delete the branch.
  - Releases: SemVer, Android + PC share one version. PATCH = fixes, MINOR = features, MAJOR = breaking/
    protocol change. Update `CHANGELOG.md`, bump versions, `chore(release): vX.Y.Z`, tag + push, GitHub
    Release with binaries attached, pre-release flag if beta. Hotfix: branch from the release tag, PR to
    `main`, re-tag.
  - Issues: all work starts as one; claim by commenting and waiting to be assigned, reassign after ~2
    weeks silent. External contributors: fork, add `upstream`, claim an issue, branch, rebase onto
    `upstream/main`, PR.
  - Tagging `vX.Y.Z` and publishing release binaries needs an explicit ask; a normal commit doesn't.
    Never change the repo's visibility without asking either.
- **Protocol changes** touch `protocol/PROTOCOL.md`, `protocol/golden.txt`, `Protocol.kt` and
  `pc/companion/protocol.h` together, with a version bump — never one side alone.
- **Keep `SessionLoop` (`pc/companion/phone_link*.cpp`) single-threaded.** A multi-thread version deadlocks.
- **UsbDk only switches the phone into accessory mode**; WinUSB (via the SYSTEM scheduled task) streams.
  Don't widen what that task runs.
- **Test the release APK (R8 on) on a real phone before publishing.** R8 has broken launch before.
- **Never tune behavior to the test phone** — every option comes from what the device reports
  (`CameraStreamer.modeFor`).
- **`applicationId` `io.github.whoissaaif.mycam` never changes** — that creates a new app on phones.
- **Keep:** opening the phone's own camera app stops the MyCam stream.
- **Design is Windows XP, Luna Blue, only — no dark mode.** Art comes from `design/tools/*.ps1`, never
  hand-edited.
- **Never log tokens, pairing codes, or QR addresses.**
- **Track work in TASKS.md and IMPROVEMENTS.md**; update both when something lands.
- **PowerShell 5 mangles UTF-8** (`·`, `×`, `→`). Use the Edit tool or `sed`, not `Set-Content`.
