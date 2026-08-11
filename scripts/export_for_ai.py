#!/usr/bin/env python3
"""
export_for_ai.py — Bundle this repo's source into one text file for an LLM.

Goal: produce a single, readable file an AI can be fed as context, while
leaving out anything that isn't source code (build output, CI/lint tooling,
media, lockfiles, IDE cruft) and anything that could be a secret (.env,
private keys, credentials, tokens found inside otherwise-normal files).

Usage:
    python3 scripts/export_for_ai.py
    python3 scripts/export_for_ai.py -o context.md --max-file-kb 500
    python3 scripts/export_for_ai.py --include-tooling   # keep CI/lint configs too
    python3 scripts/export_for_ai.py --no-git            # don't rely on .gitignore

By default it lists candidate files with `git ls-files -co --exclude-standard`
(tracked + untracked-but-not-ignored, i.e. it already honours .gitignore),
then layers its own filters on top — because things like .clang-format,
.github/workflows/*, or a committed sample .env would otherwise sail through.

No third-party dependencies; stdlib only.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# --------------------------------------------------------------------------
# Filters
# --------------------------------------------------------------------------

# Directories that are never source, regardless of what git tracks.
ALWAYS_EXCLUDE_DIRS = {
    ".git", ".tools", ".firecrawl", "build", "cmake-build-debug",
    "cmake-build-release", "node_modules", ".venv", "venv", "__pycache__",
    ".idea", ".vscode", "dist", "out", "Testing", "CMakeFiles", ".dSYM",
}

# Dev/CI/lint tooling — excluded by default because it's config, not the
# program logic. Re-include with --include-tooling.
TOOLING_PATH_GLOBS = [
    ".github/*", ".github/**/*",
    ".clang-format", ".clang-tidy",
    "Brewfile",
    ".gitignore", ".gitattributes",
    "CMakeUserPresets.json", "compile_commands.json",
    "Makefile", "DartConfiguration.tcl",
]

# File-name patterns that are always treated as secrets and always skipped,
# even with --include-tooling. No flag re-includes these.
SECRET_NAME_GLOBS = [
    ".env", ".env.*", "*.env",
    "*.pem", "*.key", "*.p12", "*.pfx", "*.jks", "*.keystore",
    "id_rsa*", "id_dsa*", "id_ecdsa*", "id_ed25519*", "*_rsa",
    ".netrc", ".npmrc", ".pypirc",
    "credentials*.json", "secrets*.*", "*.asc",
    "*apikey*", "*api_key*",
]

# Binary / media / build-artifact extensions — never useful as text context.
BINARY_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".icns", ".webp", ".tiff",
    ".mp4", ".mov", ".mxf", ".avi", ".mkv", ".m4v",
    ".wav", ".mp3", ".aac", ".m4a", ".flac",
    ".zip", ".tar", ".gz", ".tgz", ".7z", ".rar",
    ".ttf", ".otf", ".woff", ".woff2",
    ".o", ".a", ".dylib", ".so", ".dll", ".exe", ".bin",
    ".pdf", ".psd", ".sketch", ".fig",
    ".db", ".sqlite", ".sqlite3",
    ".ds_store",
}

# Regexes that flag likely secrets embedded in otherwise-normal source files.
# Matches get redacted in place; the file itself is still included.
SECRET_CONTENT_PATTERNS = [
    ("AWS Access Key ID", re.compile(r"AKIA[0-9A-Z]{16}")),
    ("AWS Secret Key", re.compile(r"(?i)aws_secret_access_key\s*[:=]\s*['\"]?[A-Za-z0-9/+=]{40}['\"]?")),
    ("Private key block", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----")),
    ("Slack token", re.compile(r"xox[baprs]-[0-9A-Za-z-]{10,}")),
    ("GitHub token", re.compile(r"gh[pousr]_[A-Za-z0-9]{36,}")),
    ("Google API key", re.compile(r"AIza[0-9A-Za-z_\-]{35}")),
    ("Generic bearer/JWT", re.compile(r"eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}")),
    ("Assigned secret-looking value", re.compile(
        r"(?i)\b(api[_-]?key|secret|token|password|passwd)\b\s*[:=]\s*['\"][^'\"\s]{12,}['\"]"
    )),
]

MAX_FILE_KB_DEFAULT = 300
MAX_TOTAL_MB_DEFAULT = 20

LANG_BY_EXT = {
    ".cc": "cpp", ".cpp": "cpp", ".h": "cpp", ".hpp": "cpp",
    ".mm": "objectivec", ".m": "objectivec",
    ".metal": "metal",
    ".py": "python", ".json": "json", ".md": "markdown",
    ".yml": "yaml", ".yaml": "yaml", ".sh": "bash", ".cmake": "cmake",
}


@dataclass
class Skipped:
    path: str
    reason: str


@dataclass
class Included:
    path: str
    size: int
    redactions: int = 0


@dataclass
class Report:
    included: list[Included] = field(default_factory=list)
    skipped: list[Skipped] = field(default_factory=list)


def list_candidate_files(root: Path, use_git: bool) -> tuple[list[Path], bool]:
    """Return (paths relative to root, whether git was actually used)."""
    if use_git:
        try:
            out = subprocess.run(
                ["git", "-C", str(root), "ls-files", "-co", "--exclude-standard"],
                capture_output=True, text=True, check=True,
            ).stdout
            files = [Path(line) for line in out.splitlines() if line.strip()]
            if files:
                return files, True
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
    # Fallback: manual walk, best-effort (no .gitignore awareness).
    files = []
    for p in root.rglob("*"):
        if p.is_dir():
            continue
        rel = p.relative_to(root)
        if any(part in ALWAYS_EXCLUDE_DIRS for part in rel.parts):
            continue
        files.append(rel)
    return files, False


def matches_any_glob(rel_posix: str, name: str, globs: list[str]) -> bool:
    return any(fnmatch.fnmatch(rel_posix, g) or fnmatch.fnmatch(name, g) for g in globs)


def is_binary_bytes(data: bytes) -> bool:
    if b"\x00" in data:
        return True
    # Heuristic: too many non-printable bytes -> treat as binary.
    text_chars = bytes(range(32, 127)) + b"\n\r\t\f\b"
    nontext = sum(b not in text_chars for b in data)
    return len(data) > 0 and nontext / len(data) > 0.30


def redact_secrets(text: str) -> tuple[str, int]:
    count = 0
    for label, pattern in SECRET_CONTENT_PATTERNS:
        def _sub(m: re.Match, label=label) -> str:
            nonlocal count
            count += 1
            return f"«REDACTED:{label}»"
        text = pattern.sub(_sub, text)
    return text, count


def build_report(root: Path, args: argparse.Namespace) -> tuple[Report, list[str]]:
    files, used_git = list_candidate_files(root, use_git=not args.no_git)
    output_path_resolved = (root / args.output).resolve()
    max_file_bytes = args.max_file_kb * 1024
    max_total_bytes = args.max_total_mb * 1024 * 1024

    report = Report()
    total_bytes = 0
    chunks: list[str] = []

    for rel in sorted(files, key=lambda p: p.as_posix()):
        rel_posix = rel.as_posix()
        abs_path = root / rel
        name = rel.name

        if abs_path.resolve() == output_path_resolved:
            continue
        if any(part in ALWAYS_EXCLUDE_DIRS for part in rel.parts):
            continue
        if not abs_path.is_file():
            continue

        if matches_any_glob(rel_posix, name, SECRET_NAME_GLOBS):
            report.skipped.append(Skipped(rel_posix, "secret filename pattern"))
            continue

        if not args.include_tooling and matches_any_glob(rel_posix, name, TOOLING_PATH_GLOBS):
            report.skipped.append(Skipped(rel_posix, "tooling/CI config"))
            continue

        if rel.suffix.lower() in BINARY_EXTENSIONS or name == ".DS_Store":
            report.skipped.append(Skipped(rel_posix, "binary/media extension"))
            continue

        try:
            raw = abs_path.read_bytes()
        except OSError as e:
            report.skipped.append(Skipped(rel_posix, f"unreadable ({e})"))
            continue

        if is_binary_bytes(raw[:8192]):
            report.skipped.append(Skipped(rel_posix, "binary content"))
            continue

        if len(raw) > max_file_bytes:
            report.skipped.append(
                Skipped(rel_posix, f"too large ({len(raw)//1024} KB > {args.max_file_kb} KB)")
            )
            continue

        if total_bytes + len(raw) > max_total_bytes:
            report.skipped.append(Skipped(rel_posix, "total size budget exceeded"))
            continue

        text = raw.decode("utf-8", errors="replace")
        if not args.no_secret_scan:
            text, n_redacted = redact_secrets(text)
        else:
            n_redacted = 0

        total_bytes += len(raw)
        report.included.append(Included(rel_posix, len(raw), n_redacted))

        lang = LANG_BY_EXT.get(rel.suffix.lower(), "")
        chunks.append(f"### {rel_posix}\n\n```{lang}\n{text.rstrip()}\n```\n")

    return report, chunks


def render_output(root: Path, report: Report, chunks: list[str], used_git: bool) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    total_kb = sum(f.size for f in report.included) // 1024
    redacted_files = [f for f in report.included if f.redactions]

    lines = [
        f"# Repo export for AI review",
        "",
        f"- Root: `{root}`",
        f"- Generated: {now}",
        f"- File list source: {'git ls-files (honours .gitignore)' if used_git else 'manual walk (no .gitignore support)'}",
        f"- Included: {len(report.included)} files, ~{total_kb} KB",
        f"- Skipped: {len(report.skipped)} files (see manifest at the end)",
    ]
    if redacted_files:
        lines.append(f"- ⚠️ Redacted probable secrets in {len(redacted_files)} file(s) — double-check these by hand.")
    lines += ["", "## File tree", "", "```"]
    lines += [f.path for f in report.included]
    lines += ["```", "", "## Files", ""]
    lines += chunks
    lines += ["## Skipped files (manifest)", "", "| Path | Reason |", "|---|---|"]
    lines += [f"| {s.path} | {s.reason} |" for s in report.skipped]
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", nargs="?", default=".", help="Repo root (default: current directory)")
    parser.add_argument("-o", "--output", default="repo_export.md", help="Output file (default: repo_export.md)")
    parser.add_argument("--max-file-kb", type=int, default=MAX_FILE_KB_DEFAULT, help="Skip individual files larger than this")
    parser.add_argument("--max-total-mb", type=int, default=MAX_TOTAL_MB_DEFAULT, help="Stop once total included size exceeds this")
    parser.add_argument("--include-tooling", action="store_true", help="Also include CI/lint/build config files")
    parser.add_argument("--no-secret-scan", action="store_true", help="Skip in-content secret redaction (not recommended)")
    parser.add_argument("--no-git", action="store_true", help="Don't use git ls-files; walk the filesystem instead")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 1

    files, used_git = list_candidate_files(root, use_git=not args.no_git)
    report, chunks = build_report(root, args)
    output_text = render_output(root, report, chunks, used_git)

    out_path = root / args.output
    out_path.write_text(output_text, encoding="utf-8")

    print(f"Wrote {out_path} — {len(report.included)} files included, {len(report.skipped)} skipped.")
    redacted = [f for f in report.included if f.redactions]
    if redacted:
        print(f"Redacted likely secrets in {len(redacted)} file(s):")
        for f in redacted:
            print(f"  - {f.path} ({f.redactions} match(es))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
