#!/usr/bin/env python3
"""Publish one Windows CI build of Silent Hill Online to its realtime releases.

Online ships only as realtime builds, one per push to SlickAmogus/silenthill-online:
players can only play together on the same build, so the launcher follows the
newest one by default and there is no scripted release stream. This is the
online copy of tools/realtime_publish.py (the single-player port's publisher)
and differs from it in four ways:

  - The launcher is part of the build and its manifest (with launcher_version),
    so the launcher updates itself from here. The single-player stream keeps
    its launcher out, because its scripted releases update it instead.
  - Runtime files CI cannot build (ANGLE, the ffmpeg runtime, UnRAR.dll) are
    carried from the newest single-player release, and steam_api64.dll from this
    repo's `runtime` release, uploaded there by hand.
  - The changelog is the recent history, since there is no official release.
  - Zips are SHOnline_rt-<version>.zip.

Two pre-releases, because a GitHub asset list cannot hold folders:

  `realtime` - the newest build, plus realtime.json (the index of the newest KEEP
               builds, which the launcher reads from HERE) and CHANGELOG_REALTIME.md
  `realtime-history` - every build the index lists; index zip_urls point here, so
               a rollback target keeps its URL after it leaves `realtime`

Order matters for anyone mid-update: the new zip is uploaded first, then the
index that references it, and only then are zips no retained build references
deleted.
"""

import argparse
import datetime as dt
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

INDEX_NAME = "realtime.json"
CHANGELOG_NAME = "CHANGELOG_REALTIME.md"
LAUNCHER_NAME = "SilentHillPC_Launcher.exe"
# Newest-only builds other workflows replace in place (build-linux/-macos on
# pc-port, build-ios on ios-port, build-android on android-port). Never pruned.
# Newest-only builds the mobile workflows replace in place (online-ios.yml on
# `ios`, online-android.yml on `android`). Never pruned here.
FIXED_ASSETS = {INDEX_NAME, CHANGELOG_NAME, "SilentHillOnline-ios-unsigned.ipa",
                "SilentHillOnline-android.apk", "altstore-source.json"}
HISTORY_TAG = "realtime-history"
WIN_ZIP_PREFIX = "SHOnline_rt-"
RUNTIME_TAG = "runtime"
RUNTIME_FILES = ["steam_api64.dll"]
# Written by the build job next to the staged files; read, then left out of the build.
LAUNCHER_VERSION_FILE = "launcher_version.txt"
# Folders the game expects that hold no shipped files.
EMPTY_DIRS = ["gamedata/save/", "gamedata/coopsaves/"]

# Never carried: the single-player changelog and config.cfg (it comes from source).
CARRY_EXCLUDE = {"CHANGELOG.md", "config.cfg", LAUNCHER_NAME}

RELEASE_NOTES = """Silent Hill Online, built automatically from every push. Older builds are under [Realtime history]({history_url}).

**Newest build: {version}** (`{commit}`, {date} UTC)
- Windows: [{zip_name}]({zip_url}): unzip into a folder and run `SilentHillPC_Launcher.exe`. Put your `Silent Hill (USA).bin` in `gamedata/`.
- Android: `SilentHillOnline-android.apk` (newest `android` build). The app offers each new build itself.
- iOS: `SilentHillOnline-ios-unsigned.ipa` (newest `ios` build). Sideload it with Sideloadly, or add `altstore-source.json` from this release to AltStore or SideStore to be offered every new build.
- Browser: https://sh1pc.com/playonline/

The launcher follows these builds on its own and offers the newest one before you play, because everyone in a session needs the same build. It installs only the files that changed. **Build Settings** can roll back to any of the last {keep} builds, but an older build can only play with others on that same build. `CHANGELOG_REALTIME.md` lists what changed."""

HISTORY_NOTES = """Older Silent Hill Online builds. The newest is on [Realtime builds]({realtime_url}).

Windows: the last {keep} builds, as `SHOnline_rt-<version>.zip`. Android and iOS: the last 15 of each, as `<platform>_<date>-<commit>`. The launcher's **Build Settings** can roll back to any of them. Back up `gamedata/save` and `gamedata/coopsaves` before going back to an older one."""


def log(msg):
    print(msg, flush=True)


def run(args, check=True):
    r = subprocess.run(args, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed ({r.returncode}): {r.stderr or ''}")
    return r


def gh(*args, check=True):
    return run(["gh", *args], check=check)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def http_get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "realtime-publish"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def version_key(tag):
    t = tag or ""
    for p in ("beta-", "v"):
        if t.lower().startswith(p):
            t = t[len(p):]
            break
    parts = t.split(".")
    if len(parts) != 4 or not all(x.isdigit() for x in parts):
        return None
    return tuple(int(x) for x in parts)


# -- release state ------------------------------------------------------------

def release_assets(repo, tag):
    r = gh("release", "view", tag, "--repo", repo, "--json", "assets", check=False)
    if r.returncode != 0:
        return None
    return {a["name"] for a in json.loads(r.stdout)["assets"]}


def download_asset(repo, tag, name, dest_dir):
    r = gh("release", "download", tag, "--repo", repo, "--pattern", name,
           "--dir", dest_dir, "--clobber", check=False)
    p = os.path.join(dest_dir, name)
    return p if r.returncode == 0 and os.path.exists(p) else None


# -- carry-forward from the newest official release --------------------------

def newest_official(nightly):
    page, best = 1, None
    while page <= 10:
        data = json.loads(http_get(f"https://api.github.com/repos/{nightly}/releases?per_page=100&page={page}"))
        if not data:
            break
        for rel in data:
            if rel.get("draft"):
                continue
            tag = rel.get("tag_name") or ""
            if tag.lower().startswith("custom-") or (rel.get("target_commitish") or "").lower() == "custom":
                continue
            k = version_key(tag)
            if k and (best is None or k > best[0]):
                best = (k, rel)
        if len(data) < 100:
            break
        page += 1
    return best[1] if best else None


def carry_files(nightly, staged_paths, out_dir):
    """Copies the official release's files that CI does not stage into out_dir.
    Returns (official_tag, official_git_commit, {path: local_file})."""
    rel = newest_official(nightly)
    if rel is None:
        raise RuntimeError(f"no official release found in {nightly} to carry runtime files from")
    tag = rel["tag_name"]
    vurl = next((a["browser_download_url"] for a in rel["assets"] if a["name"] == "version.json"), None)
    if vurl is None:
        raise RuntimeError(f"{tag} has no version.json")
    manifest = json.loads(http_get(vurl))
    wanted = [f for f in manifest.get("files", [])
              if f["path"] not in staged_paths and f["path"] not in CARRY_EXCLUDE]
    log(f"Carry: {len(wanted)} file(s) from {tag} ({manifest.get('mode') or 'loose'} release)")

    local = {}
    if (manifest.get("mode") or "loose") == "zip":
        with zipfile.ZipFile(io.BytesIO(http_get(manifest["zip_url"]))) as z:
            names = {n.replace("\\", "/"): n for n in z.namelist()}
            for f in wanted:
                src = names.get(f["path"])
                if src is None:
                    raise RuntimeError(f"{f['path']} missing from {manifest['zip_name']}")
                dst = os.path.join(out_dir, f["path"])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with z.open(src) as s, open(dst, "wb") as d:
                    shutil.copyfileobj(s, d)
                local[f["path"]] = dst
    else:
        for f in wanted:
            dst = os.path.join(out_dir, f["path"])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as d:
                d.write(http_get(f["url"]))
            local[f["path"]] = dst

    for f in wanted:
        if sha256_of(local[f["path"]]) != f["sha256"].lower():
            raise RuntimeError(f"carried {f['path']} does not match {tag}'s manifest hash")
    return tag, manifest.get("git_commit"), local


# -- changelog -----------------------------------------------------------------

def commit_subjects(prev_commit, commit):
    if prev_commit:
        r = run(["git", "merge-base", "--is-ancestor", prev_commit, commit], check=False)
        if r.returncode == 0:
            out = run(["git", "log", "--no-merges", "--format=%h%x09%s", f"{prev_commit}..{commit}"]).stdout
            lines = [l.split("\t", 1) for l in out.splitlines() if "\t" in l]
            if lines:
                return lines
    out = run(["git", "log", "-1", "--format=%h%x09%s", commit]).stdout.strip()
    return [out.split("\t", 1)] if "\t" in out else [[commit[:9], ""]]


def build_changelog(official_tag, official_commit, commit, repo):
    """Every commit since the official release, grouped by the day it was made,
    newest first. Rebuilt from git on each publish, so it always covers exactly
    what a realtime build has on top of the release, and starts over when the
    next release ships."""
    lines = ["# Silent Hill Online - Realtime builds", ""]
    rng = commit
    if official_commit and run(["git", "merge-base", "--is-ancestor", official_commit, commit],
                               check=False).returncode == 0:
        rng = f"{official_commit}..{commit}"
        lines.append(f"Everything that has landed since the last official release, **{official_tag}**, "
                     "newest first. Each line is one change.")
    else:
        lines.append("Recent changes, newest first. Each line is one change.")
    out = run(["git", "log", "--no-merges", "--date=format:%Y-%m-%d",
               "--format=%cd%x09%h%x09%s", rng]).stdout
    # Grouped explicitly: git log's order can interleave days (a commit written
    # earlier and landed later), which would otherwise repeat a heading.
    days = {}
    for row in out.splitlines()[:None if official_commit else 200]:
        parts = row.split("\t", 2)
        if len(parts) == 3:
            d, short, subj = parts
            days.setdefault(d, []).append(f"- {subj} ([{short}](https://github.com/{repo}/commit/{short}))")
    for d in sorted(days, reverse=True):
        lines += ["", f"## {d}", ""] + days[d]
    return "\n".join(lines) + "\n"


# -- zip ---------------------------------------------------------------------------

def write_zip(zip_path, files):
    """files: {relpath: local_file}. Flat, '/'-separated entries in the runtime
    layout, exactly like a scripted zip release: the launcher matches entries by
    their full path."""
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for rel in sorted(files):
            z.write(files[rel], rel)
        for d in EMPTY_DIRS:
            z.writestr(d, b"")


# -- main --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", required=True, help="staged Windows build (runtime layout)")
    ap.add_argument("--repo", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--run", default="")
    ap.add_argument("--tag", default="realtime")
    ap.add_argument("--nightly", default="SlickAmogus/silent-hill-pc-nightly")
    ap.add_argument("--keep", type=int, default=30)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="realtime-")
    assets = release_assets(a.repo, a.tag)

    index = {"schema": 2, "stream": "realtime", "builds": []}
    if assets is not None:
        p = download_asset(a.repo, a.tag, INDEX_NAME, work) if INDEX_NAME in assets else None
        if p:
            with open(p, encoding="utf-8") as f:
                index = json.load(f)
    # Schema-1 builds (one asset per file) are dropped rather than carried along,
    # but still count toward the day's build numbers so none is ever reused.
    used_versions = [b["version"] for b in index.get("builds", [])]
    index["builds"] = [b for b in index.get("builds", []) if b.get("mode") == "zip"]
    index["schema"] = 2
    prev_build = index["builds"][0] if index["builds"] else None

    if prev_build and prev_build["commit"] == a.commit:
        log(f"{a.commit[:9]} is already the newest realtime build. Nothing to do.")
        return 0

    files = {}
    for root, _, names in os.walk(a.stage):
        for name in names:
            full = os.path.join(root, name)
            files[os.path.relpath(full, a.stage).replace(os.sep, "/")] = full
    if "SilentHillPC.exe" not in files:
        raise RuntimeError(f"SilentHillPC.exe missing from {a.stage}")
    files.pop(CHANGELOG_NAME, None)

    lv_path = files.pop(LAUNCHER_VERSION_FILE, None)
    if LAUNCHER_NAME not in files or lv_path is None:
        raise RuntimeError(f"{LAUNCHER_NAME} or {LAUNCHER_VERSION_FILE} missing from {a.stage}")
    with open(lv_path, encoding="utf-8") as f:
        launcher_version = f.read().strip()

    rt_dir = os.path.join(work, "runtime")
    os.makedirs(rt_dir, exist_ok=True)
    for name in RUNTIME_FILES:
        p = download_asset(a.repo, RUNTIME_TAG, name, rt_dir)
        if p is None:
            raise RuntimeError(f"{name} missing from the {RUNTIME_TAG} release of {a.repo}")
        files[name] = p

    carry_source, _, carried = carry_files(a.nightly, set(files), os.path.join(work, "carry"))
    files.update(carried)
    if not any(p.startswith("avcodec-") for p in files):
        log("WARNING: no avcodec-*.dll in the file set; the ffmpeg FMV fallback will be unavailable.")

    now = dt.datetime.now(dt.timezone.utc)
    day = now.strftime("%Y.%m.%d")
    n = 1 + max([int(v.rsplit(".", 1)[1]) for v in used_versions if v.startswith(day + ".")] or [0])
    version = f"{day}.{n}"
    date = now.strftime("%Y-%m-%d %H:%M:%S")
    build_id = f"rt-{version}"

    subjects = commit_subjects(prev_build["commit"] if prev_build else None, a.commit)
    cl_path = os.path.join(work, CHANGELOG_NAME)
    with open(cl_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(build_changelog(None, None, a.commit, a.repo))
    files[CHANGELOG_NAME] = cl_path

    zip_name = f"SHOnline_{build_id}.zip"
    zip_path = os.path.join(work, zip_name)
    write_zip(zip_path, files)
    base_url = f"https://github.com/{a.repo}/releases/download/{HISTORY_TAG}/"

    build = {
        "id": build_id,
        "version": version,
        "commit": a.commit,
        "date": date,
        "run": a.run,
        "summary": [s for _, s in subjects],
        "mode": "zip",
        "zip_name": zip_name,
        "zip_url": base_url + zip_name,
        "launcher_version": launcher_version,
        "files": [{"path": p, "sha256": sha256_of(files[p])} for p in sorted(files)],
    }
    index["builds"] = ([build] + index["builds"])[:a.keep]
    index["carry_source"] = carry_source
    index["updated"] = date
    idx_path = os.path.join(work, INDEX_NAME)
    with open(idx_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, indent=1)

    history = release_assets(a.repo, HISTORY_TAG)
    referenced = {b["zip_name"] for b in index["builds"]}
    # Builds published before the history release existed live only in `realtime`
    # and their index URLs say so; copy them across so rollback survives the prune.
    migrate = [b for b in index["builds"][1:]
               if b["zip_name"] not in (history or set()) and b["zip_name"] in (assets or set())]
    stale_main = sorted(x for x in (assets or set()) if x not in FIXED_ASSETS and x != zip_name)
    stale_hist = sorted(x for x in (history or set()) if x.startswith(WIN_ZIP_PREFIX) and x not in referenced)
    realtime_url = f"https://github.com/{a.repo}/releases/tag/{a.tag}"
    history_url = f"https://github.com/{a.repo}/releases/tag/{HISTORY_TAG}"
    notes = RELEASE_NOTES.format(keep=a.keep, version=version, commit=a.commit[:9], date=date,
                                 zip_name=zip_name, zip_url=base_url + zip_name, history_url=history_url)
    hist_notes = HISTORY_NOTES.format(keep=a.keep, realtime_url=realtime_url)

    log(f"Build {version} from {a.commit[:9]}: {zip_name} ({os.path.getsize(zip_path) // (1 << 20)} MB, "
        f"{len(files)} files); {len(migrate)} older zip(s) to copy into history; prune "
        f"{len(stale_main)} from realtime, {len(stale_hist)} from history; {len(subjects)} commit(s)")
    for _, s in subjects:
        log(f"  - {s}")
    if a.dry_run:
        log(f"Dry run: zip at {zip_path}, index at {idx_path}. Nothing published.")
        return 0

    if history is None:
        gh("release", "create", HISTORY_TAG, "--repo", a.repo, "--prerelease", "--latest=false",
           "--target", a.commit, "--title", "Realtime history (older builds)", "--notes", hist_notes)
    for b in migrate:
        p = download_asset(a.repo, a.tag, b["zip_name"], work)
        if p:
            gh("release", "upload", HISTORY_TAG, "--repo", a.repo, "--clobber", p)
            os.remove(p)
            b["zip_url"] = base_url + b["zip_name"]
    if migrate:
        with open(idx_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f, indent=1)
    gh("release", "upload", HISTORY_TAG, "--repo", a.repo, "--clobber", zip_path)

    if assets is None:
        gh("release", "create", a.tag, "--repo", a.repo, "--prerelease", "--latest=false",
           "--target", a.commit, "--title", "Silent Hill Online: realtime builds", "--notes", notes)
    gh("release", "upload", a.tag, "--repo", a.repo, "--clobber", zip_path)
    gh("release", "upload", a.tag, "--repo", a.repo, "--clobber", idx_path, cl_path)
    gh("release", "edit", a.tag, "--repo", a.repo, "--prerelease", "--latest=false", "--notes", notes)
    gh("release", "edit", HISTORY_TAG, "--repo", a.repo, "--prerelease", "--latest=false", "--notes", hist_notes)
    for name in stale_main:
        gh("release", "delete-asset", a.tag, name, "--repo", a.repo, "--yes", check=False)
    for name in stale_hist:
        gh("release", "delete-asset", HISTORY_TAG, name, "--repo", a.repo, "--yes", check=False)
    log("Published.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
