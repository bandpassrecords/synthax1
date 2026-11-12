---
name: release
description: Cut a Synthax1 release end to end. Sets and checks the project version, drafts the GitHub release notes for approval, publishes the release (which creates the tag and starts the Build workflow), watches the pipeline, checks that the Windows, macOS and Linux zips arrived, and if a run fails deletes the release and tag, diagnoses, fixes, pushes and tries again. Use when the user says "release", "cut a release", "release 0.2.0", "ship vX.Y.Z", or "publish a new version".
argument-hint: "[X.Y.Z]"
---

# Release

Carries a release from "the code is on `main`" to "every zip is on the GitHub
release". Two checkpoints need the user, both at the start:

1. the version number, and
2. the GitHub release notes.

Once the user has said go, keep going on your own through the pipeline,
failures and retries, reporting as you go.

## How releasing works in this repo (read first)

- **One workflow:** `.github/workflows/build.yml`. On a `v*` tag it builds and
  tests Windows, macOS and Linux, then its `Release` job zips each platform as
  `Synthax1-vX.Y.Z-<Platform>.zip` and attaches the zips to the release for that
  tag (softprops/action-gh-release). A run takes about **15 to 25 minutes**.
- **Create the release first, with its notes.** `gh release create` creates the
  tag on the remote, the tag push starts the workflow, and the `Release` job then
  only attaches the zips. Don't push a bare tag: the release would be created
  without notes.
- **Version gate:** the first `VERSION X.Y.Z` in `CMakeLists.txt` (the
  `project()` call) must equal the tag. Every build job checks it first, and the
  plugin and the About text show that version. `prepare_release.py` (next to this
  file) sets and checks it.
- **What a zip holds:** `VST3/`, `Standalone/` (with its `factory/` presets), `AU/`
  on macOS, plus `LICENSE`, `BINARY-LICENSE.txt` and `licenses/` (AGPLv3 and the
  JUCE license).
- **Licensing:** the code is MIT, but the binaries include JUCE under its AGPLv3
  option, so the binaries are AGPLv3. Every release's notes say so.
- **Not signed yet:** neither Windows nor macOS binaries are code-signed or
  notarized. The notes tell macOS users how to open them.
- **No product names:** the notes, like the README and the website, describe
  `.fxp`/`.fxb` support without naming another product.
- **Use `gh` first.** For tags, releases, runs and logs use `gh`, always with
  `-R bandpassrecords/synthax1`. Use `git` only to stage, commit and push
  (remote `origin`).
- **Commits:** follow the user's global rules: no `Co-Authored-By:` trailer and
  no "Generated with Claude Code" footer, in commits and in the release notes.

## 1. Version and pre-flight

- Version: from `$ARGUMENTS` if given (strip a leading `v`). Otherwise propose
  one from the newest remote tag and the changes since it:
  ```
  gh api repos/bandpassrecords/synthax1/git/matching-refs/tags/v \
    --jq '.[].ref | sub("refs/tags/"; "")' | sort -V | tail -1
  ```
  With no tag yet, the version is the one in `CMakeLists.txt`.
- It must be `X.Y.Z` and higher than the newest remote tag. Neither a release
  nor a tag may exist for it yet; both must fail (404):
  - `gh release view vX.Y.Z -R bandpassrecords/synthax1`
  - `gh api repos/bandpassrecords/synthax1/git/ref/tags/vX.Y.Z`
- Release from `main`, up to date with `origin/main`
  (`git fetch origin && git status -sb` shows no ahead/behind).
- The working tree must be clean apart from the untracked files the user keeps
  out of the repo (for example screen recordings and `screenshots/`).
- The newest `main` run of `Build` should be green:
  `gh run list -R bandpassrecords/synthax1 --workflow build.yml --branch main --limit 1`.
  If it failed, fix that first (step 6 explains how to read the logs).

## 2. Notes (checkpoint: user approval)

Collect what changed since the last tag (or since the start, for the first
release):

```
gh api repos/bandpassrecords/synthax1/compare/<last-tag>...main \
  --jq '.commits[] | select(.parents | length == 1) | .commit.message'
```

Draft the notes in this shape and show them:

- `### Highlights`: bold-lead bullets, one per change a musician would notice,
  e.g. `- **Name of the thing**: what it does for the user.`
- `---`
- One `### <Area>` section per theme (Sound, Modulation, Preset browser, ...),
  with more detailed bullets. Leave out refactors, CI and tests.
- `### Install`: which zip for which platform, where VST3/AU go, and how to open
  unsigned binaries on macOS.
- `### License`: source MIT; binaries AGPLv3 because they include JUCE; link to
  the source.

Wait for the user's OK or edits, then save the notes to a scratch file.

## 3. Prepare and verify the commit

```
python .claude/skills/release/prepare_release.py X.Y.Z --dry-run
python .claude/skills/release/prepare_release.py X.Y.Z
```

- It sets the `project()` version in `CMakeLists.txt` and exits non-zero, with
  the reason, if the version gate would still fail. Running it again is harmless.
- If the version changed, build and run the tests (the default quality gate in
  `AGENTS.md`, or on Windows the Release build plus `ctest -E NativeProcessor`),
  commit only `CMakeLists.txt` as `chore: release vX.Y.Z`, and `git push origin main`.
- Confirm GitHub has the commit before publishing:
  `gh api repos/bandpassrecords/synthax1/commits/main --jq .sha` must equal
  `git rev-parse HEAD`.

## 4. Publish

```
gh release create vX.Y.Z -R bandpassrecords/synthax1 \
  --target <sha of the pushed commit> --title vX.Y.Z --notes-file <notes file>
```

- Use `--target` with the exact SHA, not `main`, so a later push can't slip into
  the tag.
- Find the run it started (it can take a few seconds to appear):
  ```
  gh run list -R bandpassrecords/synthax1 --workflow build.yml \
    --branch vX.Y.Z --limit 1 --json databaseId,createdAt,status
  ```
  Only accept a run created after this attempt's release was published.

## 5. Watch the pipeline

- Watch it in the background:
  `gh run watch <run-id> -R bandpassrecords/synthax1 --exit-status`.
- Tell the user it's running and roughly when it should finish.
- **Success:** the release must hold exactly these assets:
  ```
  gh release view vX.Y.Z -R bandpassrecords/synthax1 --json assets --jq '.assets[].name' | sort
  ```
  `Synthax1-vX.Y.Z-Linux.zip`, `Synthax1-vX.Y.Z-Windows.zip`,
  `Synthax1-vX.Y.Z-macOS.zip`. A missing zip counts as a failure. Report the
  release URL, the assets and how many attempts it took.
- **Failure:** go to step 6.

## 6. When a run fails

1. **Diagnose:** `gh run view <run-id> -R bandpassrecords/synthax1 --log-failed`.
   Name the failing job and step and quote the actual error.
2. **Transient** (runner lost, network timeout, download hiccup, rate limit):
   the release still exists, so `gh run rerun <run-id> -R bandpassrecords/synthax1 --failed`
   and go back to step 5. At most twice for the same job, then treat it as real.
3. **Needs the user** (billing, a missing secret, an account problem): stop and
   report exactly what's needed. Don't delete the release; a `--failed` rerun
   finishes it once they fix it.
4. **A real problem in the repo** (test failure, version gate, build error,
   workflow bug):
   - Delete the release **and** its tag:
     ```
     gh release delete vX.Y.Z -R bandpassrecords/synthax1 --cleanup-tag --yes
     gh api repos/bandpassrecords/synthax1/git/ref/tags/vX.Y.Z   # must now 404
     git tag -d vX.Y.Z 2>/dev/null
     ```
     If the tag survived: `gh api -X DELETE repos/bandpassrecords/synthax1/git/refs/tags/vX.Y.Z`.
   - Fix the root cause on `main` as for any fix (regression test for a code bug,
     the quality gate), commit it on its own with a normal message, push, and
     confirm the push landed.
   - Go back to step 4 with the new SHA.
5. **Stop after 3 attempts that each needed a code fix.** Delete the
   half-finished release, report what each attempt changed and the current
   error, and ask how to proceed.

Keep the user posted at each transition: published, failed (and why), fix
pushed, retry started, done.

## Afterwards

- The website's download section (`gh-pages` branch, `index.html`, section
  `#download`) says "coming soon" until the first release. After a release,
  point its buttons at the new zips (or at the releases page) and push
  `gh-pages`, if the user wants that.
