# M5Knife release process (0.8.0 onward)

This procedure is for **ReconditeBox/M5Knife** on the **M5Stack Cardputer Adv**.
No release is implied by a passing build; publish only after hardware sign-off.

## 1. Prepare a candidate branch

1. Start from up-to-date `main`; make changes in a `feature/...` branch.
2. Update the version in `src/main.cpp` (header and About) and the
   `README.md` current-version line **together**.
3. Run `python scripts/check_release.py`, then compile locally with:
   `pio run -e m5stack-cardputer-adv` (or the full PlatformIO executable path
   shown in the README on Windows).
4. Open a PR. GitHub Actions must complete the same pinned build successfully.
   Do not merge an uncompiled or failing candidate.
5. Use a FAT32 **test** card and finish
   [REGRESSION_TESTS.md](REGRESSION_TESTS.md) on physical hardware.
6. Fix any failures on that same branch and repeat affected tests; record
   the commit tested so there is no ambiguity about which build passed.

## 2. What the automated build proves

The workflow `.github/workflows/firmware-build.yml` runs on PRs, pushes to
`main`, and manual dispatch. It verifies source version labels, embedded
image assets, the pinned PlatformIO build, and that `firmware.bin` exists.
It uploads the build's `.bin` outputs to the GitHub Actions run as a
**short-lived CI artifact** (currently 14 days).

This catches packaging and compiler problems. It cannot certify that:
USB MSC eject is safe under all hosts; SD data survives interruption; radios
work on real networks; the keypad, display, IR or battery work; or that file
server security is suitable for untrusted networks.

A CI artifact is **not** an official GitHub Release, an OTA updater, or an
automatically flashable full-device image.

## 3. Approve the release candidate

Before marking 0.9.0 or 1.0.0 ready:

- [ ] PR build passes, from the exact candidate commit.
- [ ] Cardputer Adv regression log is complete; no untriaged failures.
- [ ] All SD/write tests used backed-up test media.
- [ ] Confirm migration and recovery from existing 0.7.x NVS settings.
- [ ] Recheck HTTP credentials, hotspot, QR and plaintext HTTP warning.
- [ ] Check reported program/heap use and sustained operation.
- [ ] Confirm About version, README version and filenames agree.
- [ ] Document known limitations, supported hardware and rollback method.
- [ ] **Review the project's copyright/licensing files**. README/About
      describe MIT; ensure the repository actually includes the intended
      license text before the public 1.0 release.
- [ ] Get explicit hardware approval from the maintainer before merge/tag.

## 4. Publish an official GitHub release (after approval)

1. Merge the tested candidate into `main` and confirm its CI workflow passes.
2. Create an annotated version tag pointing at the tested merge commit,
   e.g. `git tag -a v1.0.0 -m "M5Knife 1.0.0"`, then
   `git push origin v1.0.0` **only when ready**.
3. On GitHub, open **Releases → Draft a new release**, choose that tag,
   and write concise tested features, fixes, compatibility notes and
   known limitations.
4. Attach the **verified build artifacts** from the corresponding commit,
   preserving their original filenames. Include SHA-256 checksums.
5. Provide instructions for installing from source with PlatformIO. A
   `firmware.bin` file **alone may not set up a blank device**; it can require
   matching bootloader and partition images at board-specific offsets.
   Do not publish guessed `esptool` flash offsets.
6. Publish only after manual review, then verify downloaded artifacts,
   release URL and version on a test device.

## 5. Recovery / rollback

- Export `/M5KNIFE.CFG` from Settings and keep it somewhere private.
  It includes **plaintext WiFi and HTTP credentials**; do not attach it to
  issues, releases or public logs.
- Back up the SD card. Do not use a card containing your only copy of data.
- Use the instructions in the README to check out a tested stable tag or
  stable branch and build/upload through PlatformIO.
- NVS may retain settings across firmware updates, but a full flash erase
  can wipe them. Import a backed-up valid configuration if necessary.
- After rollback, verify USB, SD and network behaviour on test media before
  resuming real data operations.
