#!/usr/bin/env python3
"""Publish a production firmware package for MEB-Preheat."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


APP_CONFIG = Path("main/app_config.h")
APP_VERSION_RE = re.compile(r'(#define\s+MEB_APP_VERSION\s+")([^"]+)(")')
SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
DEFAULT_BUILD_DIR = "build-production"
DEFAULT_RELEASE_DIR = "release"
DEFAULT_REMOTE = "origin"
REQUIRED_ARTIFACTS = (
    Path("meb-preheat.bin"),
    Path("bootloader/bootloader.bin"),
    Path("partition_table/partition-table.bin"),
    Path("ota_data_initial.bin"),
    Path("flash_args"),
    Path("flasher_args.json"),
)
OPTIONAL_ARTIFACTS = (
    Path("flash_project_args"),
    Path("flash_app_args"),
    Path("app-flash_args"),
    Path("bootloader-flash_args"),
    Path("partition-table-flash_args"),
    Path("otadata-flash_args"),
    Path("project_description.json"),
    Path("bootloader/project_description.json"),
)


@dataclass(frozen=True, order=True)
class Version:
    major: int
    minor: int
    patch: int

    @classmethod
    def parse(cls, value: str) -> Version | None:
        match = SEMVER_RE.match(value.strip())
        if not match:
            return None
        return cls(*(int(part) for part in match.groups()))

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


def run(
    command: list[str],
    *,
    cwd: Path,
    capture: bool = False,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    printable = " ".join(command)
    print(f"$ {printable}", flush=True)
    return subprocess.run(
        command,
        cwd=cwd,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def git(root: Path, *args: str, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    return run(["git", *args], cwd=root, capture=capture, check=check)


def output(completed: subprocess.CompletedProcess[str]) -> str:
    return (completed.stdout or "").strip()


def repo_root() -> Path:
    starts = [Path(__file__).resolve().parents[1], Path.cwd()]
    for start in starts:
        completed = run(["git", "rev-parse", "--show-toplevel"], cwd=start, capture=True, check=False)
        if completed.returncode == 0:
            root = output(completed)
            if root:
                return Path(root)
    raise SystemExit("Not inside a git repository")


def ensure_clean_tree(root: Path) -> None:
    status = output(git(root, "status", "--porcelain", "--untracked-files=all", capture=True))
    if status:
        raise SystemExit(
            "Git working tree has pending changes. Commit, stash, or remove them before publishing:\n"
            + status
        )


def read_app_version(root: Path) -> str:
    config = root / APP_CONFIG
    text = config.read_text(encoding="utf-8")
    match = APP_VERSION_RE.search(text)
    if not match:
        raise SystemExit(f"Could not find MEB_APP_VERSION in {APP_CONFIG}")
    return match.group(2)


def write_app_version(root: Path, version: str) -> None:
    config = root / APP_CONFIG
    text = config.read_text(encoding="utf-8")
    new_text, count = APP_VERSION_RE.subn(rf'\g<1>{version}\g<3>', text, count=1)
    if count != 1:
        raise SystemExit(f"Could not update MEB_APP_VERSION in {APP_CONFIG}")
    config.write_text(new_text, encoding="utf-8")


def semver_tags(root: Path) -> list[tuple[Version, str]]:
    tags: list[tuple[Version, str]] = []
    raw_tags = output(git(root, "tag", "--list", capture=True))
    for tag in raw_tags.splitlines():
        version = Version.parse(tag)
        if version is not None:
            tags.append((version, tag))
    return sorted(tags)


def latest_semver_tag(root: Path) -> tuple[Version, str] | None:
    tags = semver_tags(root)
    return tags[-1] if tags else None


def normalize_version_input(value: str) -> str:
    value = value.strip()
    match = SEMVER_RE.match(value)
    if not match:
        return value
    return ".".join(match.groups())


def prompt_version(current_version: str) -> str:
    while True:
        entered = input(f"New version [{current_version}]: ").strip()
        if entered:
            return normalize_version_input(entered)
        print("Enter a new version number, for example 0.7.0.")


def confirm(prompt: str, *, default: bool = False, assume_yes: bool = False) -> bool:
    if assume_yes:
        return True

    suffix = " [Y/n]: " if default else " [y/N]: "
    while True:
        answer = input(prompt + suffix).strip().lower()
        if not answer:
            return default
        if answer in {"y", "yes"}:
            return True
        if answer in {"n", "no"}:
            return False
        print("Please answer yes or no.")


def version_is_logical(new_value: str, current_value: str, latest_tag: tuple[Version, str] | None) -> tuple[bool, str]:
    new_version = Version.parse(new_value)
    current_version = Version.parse(current_value)

    if new_version is None:
        return False, f"{new_value!r} is not a simple semantic version like 0.7.0"

    baselines: list[tuple[Version, str]] = []
    if current_version is not None:
        baselines.append((current_version, f"current firmware version {current_version}"))
    if latest_tag is not None:
        baselines.append((latest_tag[0], f"latest git tag {latest_tag[1]}"))

    if not baselines:
        return True, ""

    baseline_version, baseline_label = max(baselines, key=lambda item: item[0])
    if new_version <= baseline_version:
        return False, f"{new_version} is not greater than {baseline_label}"

    return True, ""


def ensure_tag_available(root: Path, remote: str, tag_name: str, *, check_remote: bool) -> None:
    local = git(root, "rev-parse", "--verify", "--quiet", f"refs/tags/{tag_name}", capture=True, check=False)
    if local.returncode == 0:
        raise SystemExit(f"Tag {tag_name!r} already exists locally")

    if not check_remote:
        return

    remote_ref = f"refs/tags/{tag_name}"
    remote_check = git(root, "ls-remote", "--exit-code", "--tags", remote, remote_ref, capture=True, check=False)
    if remote_check.returncode == 0:
        raise SystemExit(f"Tag {tag_name!r} already exists on remote {remote!r}")
    if remote_check.returncode not in {0, 2}:
        stderr = (remote_check.stderr or "").strip()
        raise SystemExit(f"Could not check remote tag {tag_name!r}: {stderr}")


def current_branch(root: Path) -> str:
    branch = output(git(root, "branch", "--show-current", capture=True))
    if not branch:
        raise SystemExit("Cannot publish from a detached HEAD")
    return branch


def resolve_under_root(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def check_build_environment(root: Path, build_command: str | None) -> None:
    if build_command:
        return

    script = root / "scripts/build_production.ps1"
    powershell = shutil.which("pwsh") or shutil.which("powershell")
    if os.name == "nt" and powershell and script.exists():
        return

    missing: list[str] = []
    if not os.environ.get("IDF_PATH"):
        missing.append("IDF_PATH")
    if shutil.which("cmake") is None:
        missing.append("cmake")
    if shutil.which("ninja") is None:
        missing.append("ninja")

    if missing:
        raise SystemExit(
            "Production build environment is not ready. Missing: "
            + ", ".join(missing)
            + ". Load the ESP-IDF environment or pass --build-command."
        )


def build_firmware(root: Path, build_dir_arg: str, sdkconfig_arg: str, build_command: str | None) -> None:
    if build_command:
        print(f"$ {build_command}")
        subprocess.run(build_command, cwd=root, shell=True, check=True)
        return

    check_build_environment(root, build_command)
    build_dir = resolve_under_root(root, build_dir_arg)
    sdkconfig = resolve_under_root(root, sdkconfig_arg)
    script = root / "scripts/build_production.ps1"
    powershell = shutil.which("pwsh") or shutil.which("powershell")

    if os.name == "nt" and powershell and script.exists():
        run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script),
                "-BuildDir",
                build_dir_arg,
                "-Sdkconfig",
                sdkconfig_arg,
            ],
            cwd=root,
        )
        return

    defaults = f"{root / 'sdkconfig'};{root / 'sdkconfig.defaults.production'}"
    run(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            "-DIDF_TARGET=esp32c5",
            f"-DSDKCONFIG={sdkconfig}",
            f"-DSDKCONFIG_DEFAULTS={defaults}",
        ],
        cwd=root,
    )
    run(["cmake", "--build", str(build_dir)], cwd=root)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_artifacts(build_dir: Path, release_dir: Path) -> list[dict[str, object]]:
    artifacts: list[dict[str, object]] = []

    for relative in REQUIRED_ARTIFACTS:
        source = build_dir / relative
        if not source.is_file():
            raise SystemExit(f"Required build artifact is missing: {source}")

    for relative in (*REQUIRED_ARTIFACTS, *OPTIONAL_ARTIFACTS):
        source = build_dir / relative
        if not source.is_file():
            continue

        destination = release_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        artifacts.append(
            {
                "path": relative.as_posix(),
                "size": destination.stat().st_size,
                "sha256": sha256_file(destination),
            }
        )

    return artifacts


def git_log(root: Path, previous_tag: str | None) -> list[str]:
    command = ["log", "--reverse", "--pretty=format:%h %ad %s", "--date=short"]
    if previous_tag:
        command.append(f"{previous_tag}..HEAD")
    else:
        command.append("HEAD")
    lines = output(git(root, *command, capture=True))
    return [line for line in lines.splitlines() if line.strip()]


def write_checksums(release_dir: Path, artifacts: list[dict[str, object]]) -> None:
    lines = [f"{artifact['sha256']}  {artifact['path']}" for artifact in artifacts]
    (release_dir / "checksums.sha256").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_manifest(
    release_dir: Path,
    *,
    version: str,
    tag_name: str,
    branch: str,
    commit: str,
    previous_tag: str | None,
    artifacts: list[dict[str, object]],
) -> None:
    manifest = {
        "project": "meb-preheat",
        "version": version,
        "tag": tag_name,
        "branch": branch,
        "commit": commit,
        "previous_tag": previous_tag,
        "build_mode": "production",
        "built_at": dt.datetime.now(dt.UTC).replace(microsecond=0).isoformat(),
        "artifacts": artifacts,
    }
    (release_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def write_release_notes(
    root: Path,
    release_dir: Path,
    *,
    version: str,
    tag_name: str,
    branch: str,
    commit: str,
    previous_tag: str | None,
    artifacts: list[dict[str, object]],
) -> None:
    changes = git_log(root, previous_tag)
    compare_label = f"since {previous_tag}" if previous_tag else "from repository history"
    lines = [
        f"# MEB-Preheat Firmware {version}",
        "",
        f"- Tag: `{tag_name}`",
        f"- Commit: `{commit}`",
        f"- Branch: `{branch}`",
        "- Build mode: `production`",
        f"- Built: `{dt.datetime.now(dt.UTC).replace(microsecond=0).isoformat()}`",
        "",
        f"## Changes {compare_label}",
        "",
    ]

    if changes:
        lines.extend(f"- {line}" for line in changes)
    else:
        lines.append("- No commits found for this release range.")

    lines.extend(
        [
            "",
            "## Artifacts",
            "",
        ]
    )
    lines.extend(f"- `{artifact['path']}` ({artifact['size']} bytes)" for artifact in artifacts)
    lines.extend(
        [
            "",
            "## Flashing",
            "",
            "From this release folder:",
            "",
            "```sh",
            "python -m esptool --chip esp32c5 -b 460800 --before default-reset --after hard-reset --port COM4 write_flash \"@flash_args\"",
            "```",
            "",
            "Adjust the serial port for the target machine. For OTA updates, use `meb-preheat.bin` with `scripts/meb_ota_update.py`.",
            "",
        ]
    )
    (release_dir / "RELEASE_NOTES.md").write_text("\n".join(lines), encoding="utf-8")


def create_version_commit(root: Path, version: str, current_version: str) -> None:
    if version == current_version:
        print(f"MEB_APP_VERSION already is {version}; no version commit needed.")
        return

    write_app_version(root, version)
    git(root, "add", str(APP_CONFIG))
    git(root, "commit", "-m", f"Release {version}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Publish a production firmware release package.")
    parser.add_argument("--version", help="Release version to use instead of prompting, for example 0.7.0")
    parser.add_argument("--tag", help="Git tag to create. Defaults to the version string.")
    parser.add_argument("--remote", default=DEFAULT_REMOTE, help=f"Git remote to push, default {DEFAULT_REMOTE}")
    parser.add_argument("--build-dir", default=DEFAULT_BUILD_DIR, help=f"Production build directory, default {DEFAULT_BUILD_DIR}")
    parser.add_argument("--sdkconfig", default="sdkconfig.production", help="Generated production sdkconfig path")
    parser.add_argument("--release-dir", default=DEFAULT_RELEASE_DIR, help=f"Release package root, default {DEFAULT_RELEASE_DIR}")
    parser.add_argument("--build-command", help="Override the production build command")
    parser.add_argument("--skip-fetch", action="store_true", help="Do not fetch remote tags before validation")
    parser.add_argument("--skip-push", action="store_true", help="Create the commit/tag/package locally without pushing")
    parser.add_argument("--yes", action="store_true", help="Accept normal confirmations")
    parser.add_argument(
        "--allow-nonlogical-version",
        action="store_true",
        help="Allow a version that is not greater than the current firmware version/latest tag",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = repo_root()
    build_dir = resolve_under_root(root, args.build_dir)
    release_root = resolve_under_root(root, args.release_dir)

    ensure_clean_tree(root)

    if not args.skip_fetch:
        git(root, "fetch", "--tags", args.remote)

    current = read_app_version(root)
    latest_tag = latest_semver_tag(root)
    latest_text = latest_tag[1] if latest_tag else "none"

    print(f"Current firmware version: {current}")
    print(f"Latest semantic git tag: {latest_text}")

    version = normalize_version_input(args.version) if args.version else prompt_version(current)
    tag_name = args.tag or version
    branch = current_branch(root)
    release_dir = release_root / version

    logical, reason = version_is_logical(version, current, latest_tag)
    if not logical:
        print(f"Version warning: {reason}")
        if args.allow_nonlogical_version:
            print("Continuing because --allow-nonlogical-version was provided.")
        elif not confirm("This version does not look like a normal increment. Continue anyway?"):
            raise SystemExit("Release cancelled.")

    ensure_tag_available(root, args.remote, tag_name, check_remote=not args.skip_push)
    if release_dir.exists():
        raise SystemExit(f"Release directory already exists: {release_dir}")
    check_build_environment(root, args.build_command)

    print()
    print("Release plan:")
    print(f"  Version: {version}")
    print(f"  Tag: {tag_name}")
    print(f"  Branch: {branch}")
    print(f"  Remote: {args.remote}")
    print(f"  Build dir: {build_dir}")
    print(f"  Release dir: {release_dir}")
    print("  Source update: " + ("no change" if version == current else f"{APP_CONFIG} {current} -> {version}"))
    print("  Push: " + ("skipped" if args.skip_push else "branch and tag"))
    print()

    if not confirm("Publish this firmware release?", assume_yes=args.yes):
        raise SystemExit("Release cancelled.")

    previous_tag = latest_tag[1] if latest_tag else None
    create_version_commit(root, version, current)
    commit = output(git(root, "rev-parse", "HEAD", capture=True))

    git(root, "tag", "-a", tag_name, "-m", f"Release {version}")
    if not args.skip_push:
        git(root, "push", args.remote, branch)
        git(root, "push", args.remote, tag_name)

    build_firmware(root, args.build_dir, args.sdkconfig, args.build_command)

    release_dir.mkdir(parents=True)
    artifacts = copy_artifacts(build_dir, release_dir)
    write_checksums(release_dir, artifacts)
    write_manifest(
        release_dir,
        version=version,
        tag_name=tag_name,
        branch=branch,
        commit=commit,
        previous_tag=previous_tag,
        artifacts=artifacts,
    )
    write_release_notes(
        root,
        release_dir,
        version=version,
        tag_name=tag_name,
        branch=branch,
        commit=commit,
        previous_tag=previous_tag,
        artifacts=artifacts,
    )

    print()
    print(f"Release package created: {release_dir}")
    print(f"Firmware image: {release_dir / 'meb-preheat.bin'}")
    print(f"Release notes: {release_dir / 'RELEASE_NOTES.md'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nRelease cancelled.", file=sys.stderr)
        raise SystemExit(130)
