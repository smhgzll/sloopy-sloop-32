# SPDX-License-Identifier: GPL-3.0-only
"""Project layout and hygiene: the documents a fresh session needs, the secrets that must stay out
of git, the upstream lock."""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tracked_files():
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True, check=False)
    return [line for line in out.stdout.splitlines() if line]


def test_required_docs_exist():
    required = [
        "CLAUDE.md", "PROJECT.md", "README.md", "PROGRESS.md", "NEXT_STEPS.md", "KNOWN_ISSUES.md",
        "docs/ARCHITECTURE.md", "docs/PORT_PLAN.md", "docs/EMULATION.md", "docs/UI_SPEC.md",
        "docs/PROTOCOL.md", "docs/UPSTREAM_MAP.md", "docs/HARDWARE_TESTS.md", "docs/CONFIGURATION.md",
        "config/local.env.example", "config/upstream.lock", "config/partitions.csv",
    ]
    for rel in required:
        assert (ROOT / rel).exists(), rel


def test_next_steps_starts_with_next_action():
    text = (ROOT / "NEXT_STEPS.md").read_text()
    first = next(line for line in text.splitlines() if line.startswith("## "))
    assert first.strip() == "## NEXT ACTION"


def test_secret_files_are_ignored():
    ignore = (ROOT / ".gitignore").read_text()
    assert "/config/local.env" in ignore
    assert "/config/sdkconfig.local.defaults" in ignore
    tracked = tracked_files()
    assert "config/local.env" not in tracked
    assert "config/sdkconfig.local.defaults" not in tracked


def test_no_wifi_password_tracked():
    """only the documented placeholder may appear as a Wi-Fi password in tracked files"""
    pat = re.compile(r"(SLOOPY_WIFI_PASSWORD|CONFIG_SLOOPY_WIFI_PASSWORD)\s*:?=\s*\"?([^\"\n]*)")
    for rel in tracked_files():
        p = ROOT / rel
        if not p.is_file() or p.stat().st_size > 1_000_000:
            continue
        try:
            text = p.read_text()
        except UnicodeDecodeError:
            continue
        for m in pat.finditer(text):
            value = m.group(2).strip().strip('"').rstrip("}")   # (also ${VAR:=default})
            if value.startswith("$"):
                continue                                       # a shell expansion, not a literal
            assert value in ("", "change-this-password"), f"{rel}: a Wi-Fi password value is tracked: {value!r}"


def test_upstream_lock_is_a_commit():
    lock = (ROOT / "config/upstream.lock").read_text()
    m = re.search(r"^SLOOP_COMMIT=([0-9a-f]{40})$", lock, re.M)
    assert m, "config/upstream.lock: SLOOP_COMMIT must be a full commit hash"


def test_reference_asset_exists():
    assert (ROOT / "assets/references/mwave-fm1-front-reference.svg").exists()


def test_upstream_not_vendored():
    assert not any(f.startswith("upstream/sloop-fm1/") for f in tracked_files()), "upstream must not be committed"
