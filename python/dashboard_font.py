"""Dashboard version and font selection mirroring dashboard_font.c."""

from pathlib import Path

_FONT_FAMILY = "TkDefaultFont"


def application_version() -> str:
    """Read the repository-wide semantic application version."""
    try:
        return (Path(__file__).resolve().parents[1] / "VERSION").read_text(
            encoding="utf-8").strip()
    except OSError:
        return "unknown"


def set_font_family(family: str) -> None:
    """Set the family used by subsequently rendered dashboard controls."""
    global _FONT_FAMILY
    _FONT_FAMILY = family


def font_family() -> str:
    """Return the configured dashboard font family."""
    return _FONT_FAMILY
