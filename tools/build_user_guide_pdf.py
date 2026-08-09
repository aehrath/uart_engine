#!/usr/bin/env python3
"""Render the repository README into the printable BLE UART user guide PDF."""

from __future__ import annotations

import argparse
from html import escape
import pathlib
import re
from typing import Iterable

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen.canvas import Canvas
from reportlab.platypus import (
    BaseDocTemplate,
    Frame,
    KeepTogether,
    Image as ReportLabImage,
    ListFlowable,
    ListItem,
    PageTemplate,
    Paragraph,
    Preformatted,
    Spacer,
    Table,
    TableStyle,
)

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "README.md"
DEFAULT_OUTPUT = ROOT / "output" / "pdf" / "UART_DMA_BLE_HCI_User_Guide.pdf"
REGULAR_FONT = ROOT / "cmake-build-debug" / "_deps" / "space_grotesk-src" / \
    "ttf" / "static" / "SpaceGrotesk-Regular.ttf"
BOLD_FONT = ROOT / "cmake-build-debug" / "_deps" / "space_grotesk-src" / \
    "ttf" / "static" / "SpaceGrotesk-Bold.ttf"

INK = colors.HexColor("#182235")
MUTED = colors.HexColor("#52627A")
BLUE = colors.HexColor("#1769AA")
CYAN = colors.HexColor("#008D86")
PALE_BLUE = colors.HexColor("#EAF3FA")
PALE_GRAY = colors.HexColor("#F3F5F8")
BORDER = colors.HexColor("#C8D2E0")
CODE_BG = colors.HexColor("#101827")
CODE_FG = colors.HexColor("#EDF4FF")


class NumberedCanvas(Canvas):
    """Canvas that delays page output so every footer can show total pages."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        """Initialize the canvas and retained page-state list."""
        super().__init__(*args, **kwargs)
        self._saved_page_states: list[dict[str, object]] = []

    def showPage(self) -> None:  # noqa: N802 - ReportLab API spelling
        """Retain this page state instead of immediately finalizing it."""
        self._saved_page_states.append(dict(self.__dict__))
        self._startPage()

    def save(self) -> None:
        """Replay retained states with consistent headers and numbered footers."""
        page_count = len(self._saved_page_states)
        for page_number, state in enumerate(self._saved_page_states, start=1):
            self.__dict__.update(state)
            self._draw_chrome(page_number, page_count)
            super().showPage()
        super().save()

    def _draw_chrome(self, page_number: int, page_count: int) -> None:
        """Draw the running title, divider rules, and page number."""
        width, height = letter
        self.saveState()
        self.setStrokeColor(BORDER)
        self.setLineWidth(0.5)
        self.line(0.65 * inch, height - 0.52 * inch,
                  width - 0.65 * inch, height - 0.52 * inch)
        self.line(0.65 * inch, 0.48 * inch,
                  width - 0.65 * inch, 0.48 * inch)
        self.setFillColor(MUTED)
        self.setFont("GuideRegular", 8)
        self.drawString(0.65 * inch, height - 0.39 * inch,
                        "UART DMA / BLE HCI Demonstration Suite")
        self.drawRightString(width - 0.65 * inch, 0.31 * inch,
                             f"Page {page_number} of {page_count}")
        self.restoreState()


def register_fonts() -> None:
    """Register the repository UI fonts, falling back to built-in Helvetica."""
    if REGULAR_FONT.exists() and BOLD_FONT.exists():
        pdfmetrics.registerFont(TTFont("GuideRegular", str(REGULAR_FONT)))
        pdfmetrics.registerFont(TTFont("GuideBold", str(BOLD_FONT)))
        pdfmetrics.registerFontFamily(
            "GuideRegular", normal="GuideRegular", bold="GuideBold",
            italic="GuideRegular", boldItalic="GuideBold")
    else:
        # Alias built-in fonts under stable names used by all styles below.
        pdfmetrics.registerFontFamily(
            "GuideRegular", normal="Helvetica", bold="Helvetica-Bold")
        pdfmetrics.registerFontFamily(
            "GuideBold", normal="Helvetica-Bold", bold="Helvetica-Bold")


def inline_markup(text: str) -> str:
    """Convert the README's inline Markdown subset into ReportLab markup."""
    protected: dict[str, str] = {}

    def protect(value: str) -> str:
        token = f"@@INLINE{len(protected)}@@"
        protected[token] = value
        return token

    def code(match: re.Match[str]) -> str:
        return protect(
            f'<font name="Courier" color="#293A56">{escape(match.group(1))}</font>')

    def link(match: re.Match[str]) -> str:
        label = escape(match.group(1))
        target = match.group(2)
        if target.startswith(("https://", "http://")):
            return protect(f'<link href="{escape(target)}" color="#1769AA">{label}</link>')
        return protect(f'<font color="#1769AA">{label}</font>')

    text = re.sub(r"`([^`]+)`", code, text)
    text = re.sub(r"\[([^]]+)]\(([^)]+)\)", link, text)
    text = escape(text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", text)
    # Replace outer constructs before nested inline code used as a link label.
    for token, value in reversed(list(protected.items())):
        text = text.replace(token, value)
    return text


def build_styles() -> dict[str, ParagraphStyle]:
    """Create the complete typography system used by the guide."""
    sample = getSampleStyleSheet()
    return {
        "title": ParagraphStyle(
            "GuideTitle", parent=sample["Title"], fontName="GuideBold",
            fontSize=25, leading=29, textColor=INK, alignment=TA_LEFT,
            spaceAfter=14, keepWithNext=True),
        "h2": ParagraphStyle(
            "GuideH2", parent=sample["Heading2"], fontName="GuideBold",
            fontSize=17, leading=20, textColor=BLUE, spaceBefore=14,
            spaceAfter=8, keepWithNext=True),
        "h3": ParagraphStyle(
            "GuideH3", parent=sample["Heading3"], fontName="GuideBold",
            fontSize=12.5, leading=16, textColor=CYAN, spaceBefore=12,
            spaceAfter=5, keepWithNext=True),
        "body": ParagraphStyle(
            "GuideBody", parent=sample["BodyText"], fontName="GuideRegular",
            fontSize=9.0, leading=12.6, textColor=INK, spaceAfter=5),
        "bullet": ParagraphStyle(
            "GuideBullet", parent=sample["BodyText"], fontName="GuideRegular",
            fontSize=9.0, leading=12.4, textColor=INK, leftIndent=16,
            firstLineIndent=0, spaceAfter=2),
        "number": ParagraphStyle(
            "GuideNumber", parent=sample["BodyText"], fontName="GuideRegular",
            fontSize=9.0, leading=12.4, textColor=INK, leftIndent=20,
            firstLineIndent=0, spaceAfter=2),
        "code": ParagraphStyle(
            "GuideCode", parent=sample["Code"], fontName="Courier",
            fontSize=7.6, leading=10.0, textColor=CODE_FG,
            leftIndent=0, rightIndent=0, spaceBefore=0, spaceAfter=0),
        "table": ParagraphStyle(
            "GuideTable", parent=sample["BodyText"], fontName="GuideRegular",
            fontSize=7.7, leading=10.2, textColor=INK),
        "table_header": ParagraphStyle(
            "GuideTableHeader", parent=sample["BodyText"], fontName="GuideBold",
            fontSize=7.8, leading=10.2, textColor=colors.white,
            alignment=TA_LEFT),
        "small": ParagraphStyle(
            "GuideSmall", parent=sample["BodyText"], fontName="GuideRegular",
            fontSize=8, leading=11, textColor=MUTED, alignment=TA_CENTER),
    }


def paragraph_from(lines: list[str], styles: dict[str, ParagraphStyle]) -> Paragraph:
    """Join wrapped Markdown source lines into one body paragraph."""
    return Paragraph(inline_markup(" ".join(line.strip() for line in lines)),
                     styles["body"])


def table_widths(column_count: int, available: float) -> list[float]:
    """Choose readable widths for the guide's two- and three-column tables."""
    if column_count == 2:
        return [available * 0.29, available * 0.71]
    if column_count == 3:
        return [available * 0.22, available * 0.57, available * 0.21]
    return [available / column_count] * column_count


def markdown_table(lines: list[str], styles: dict[str, ParagraphStyle],
                   available: float) -> Table:
    """Convert a complete GitHub Markdown table into a repeating-header table."""
    rows: list[list[str]] = []
    for index, line in enumerate(lines):
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if index == 1 and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
            continue
        style = styles["table_header"] if not rows else styles["table"]
        rows.append([Paragraph(inline_markup(cell), style) for cell in cells])
    table = Table(rows, colWidths=table_widths(len(rows[0]), available),
                  repeatRows=1, hAlign="LEFT")
    table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), BLUE),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("BACKGROUND", (0, 1), (-1, -1), colors.white),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, PALE_GRAY]),
        ("GRID", (0, 0), (-1, -1), 0.45, BORDER),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 6),
        ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
    ]))
    return table


def list_flowable(items: Iterable[str], ordered: bool,
                  styles: dict[str, ParagraphStyle]) -> ListFlowable:
    """Create a compact bullet or numbered list from Markdown item bodies."""
    style = styles["number"] if ordered else styles["bullet"]
    entries = [ListItem(Paragraph(inline_markup(item), style),
                        leftIndent=12) for item in items]
    options: dict[str, object] = {
        "bulletType": "1" if ordered else "bullet",
        "leftIndent": 18,
        "bulletFontName": "GuideRegular",
        "bulletFontSize": 8.5,
        "bulletColor": BLUE,
        "spaceBefore": 1,
        "spaceAfter": 5,
    }
    if ordered:
        options["start"] = 1
    return ListFlowable(entries, **options)


def markdown_story(markdown: str, styles: dict[str, ParagraphStyle],
                   available: float,
                   source_directory: pathlib.Path) -> list[object]:
    """Parse the README's block Markdown into a ReportLab Platypus story."""
    lines = markdown.splitlines()
    story: list[object] = []
    index = 0
    paragraph_lines: list[str] = []

    def flush_paragraph() -> None:
        if paragraph_lines:
            story.append(paragraph_from(paragraph_lines, styles))
            paragraph_lines.clear()

    while index < len(lines):
        line = lines[index]
        if not line.strip():
            flush_paragraph()
            index += 1
            continue
        if line.startswith("```"):
            flush_paragraph()
            language = line[3:].strip()
            code_lines: list[str] = []
            index += 1
            while index < len(lines) and not lines[index].startswith("```"):
                code_lines.append(lines[index])
                index += 1
            label = f"{language.upper()}\n" if language else ""
            block = Preformatted(label + "\n".join(code_lines), styles["code"],
                                 maxLineLength=110)
            code_table = Table([[block]], colWidths=[available - 8], hAlign="LEFT")
            code_table.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, -1), CODE_BG),
                ("BOX", (0, 0), (-1, -1), 0.6, colors.HexColor("#253550")),
                ("LEFTPADDING", (0, 0), (-1, -1), 9),
                ("RIGHTPADDING", (0, 0), (-1, -1), 9),
                ("TOPPADDING", (0, 0), (-1, -1), 7),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
            ]))
            story.append(code_table)
            story.append(Spacer(1, 7))
            index += 1
            continue
        if line.startswith("# "):
            flush_paragraph()
            story.append(Spacer(1, 0.06 * inch))
            story.append(Paragraph(inline_markup(line[2:]), styles["title"]))
            story.append(Table([[""]], colWidths=[available], rowHeights=[5],
                               style=TableStyle([("BACKGROUND", (0, 0), (-1, -1), CYAN)])))
            story.append(Spacer(1, 0.12 * inch))
            index += 1
            continue
        if line.startswith("## "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(line[3:]), styles["h2"]))
            index += 1
            continue
        if line.startswith("### "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(line[4:]), styles["h3"]))
            index += 1
            continue
        image_match = re.match(r"^!\[([^]]*)\]\(([^)]+)\)$", line.strip())
        if image_match:
            flush_paragraph()
            image_path = (source_directory / image_match.group(2)).resolve()
            if not image_path.is_file():
                raise FileNotFoundError(
                    f"README image does not exist: {image_path}")
            illustration = ReportLabImage(str(image_path))
            scale = min(available / illustration.imageWidth,
                        (6.4 * inch) / illustration.imageHeight)
            illustration.drawWidth = illustration.imageWidth * scale
            illustration.drawHeight = illustration.imageHeight * scale
            illustration.hAlign = "CENTER"
            story.append(illustration)
            story.append(Spacer(1, 5))
            index += 1
            continue
        if line.startswith("| ") and index + 1 < len(lines) and \
                re.match(r"^\|\s*:?-{3,}", lines[index + 1]):
            flush_paragraph()
            table_lines = [line, lines[index + 1]]
            index += 2
            while index < len(lines) and lines[index].startswith("|"):
                table_lines.append(lines[index])
                index += 1
            story.append(markdown_table(table_lines, styles, available))
            story.append(Spacer(1, 7))
            continue
        bullet_match = re.match(r"^-\s+(.+)$", line)
        number_match = re.match(r"^\d+\.\s+(.+)$", line)
        if bullet_match or number_match:
            flush_paragraph()
            ordered = number_match is not None
            items: list[str] = []
            matcher = r"^\d+\.\s+(.+)$" if ordered else r"^-\s+(.+)$"
            while index < len(lines):
                match = re.match(matcher, lines[index])
                if match is None:
                    break
                item_parts = [match.group(1)]
                index += 1
                # Markdown commonly wraps a list item onto indented source
                # lines. Keep those lines inside the same ListItem so ordered
                # lists retain continuous numbering in the rendered guide.
                while index < len(lines) and lines[index].strip() and \
                        re.match(r"^\s{2,}\S", lines[index]):
                    item_parts.append(lines[index].strip())
                    index += 1
                items.append(" ".join(item_parts))
            story.append(list_flowable(items, ordered, styles))
            continue
        paragraph_lines.append(line)
        index += 1
    flush_paragraph()
    return story


def build_pdf(input_path: pathlib.Path, output_path: pathlib.Path) -> None:
    """Build a polished PDF whose complete instruction source is README.md."""
    register_fonts()
    styles = build_styles()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    width, height = letter
    left = right = 0.65 * inch
    top = 0.68 * inch
    bottom = 0.62 * inch
    frame = Frame(left, bottom, width - left - right, height - top - bottom,
                  id="guide", leftPadding=0, rightPadding=0,
                  topPadding=0, bottomPadding=0)
    document = BaseDocTemplate(
        str(output_path), pagesize=letter, leftMargin=left, rightMargin=right,
        topMargin=top, bottomMargin=bottom,
        title="UART DMA / BLE HCI Demonstration Suite User Guide",
        author="UART DMA demonstration project",
        subject="Build, operation, simulation, testing, and porting instructions")
    document.addPageTemplates(PageTemplate(id="guide", frames=[frame]))
    story = markdown_story(input_path.read_text(encoding="utf-8"), styles,
                           width - left - right, input_path.parent)
    document.build(story, canvasmaker=NumberedCanvas)


def main() -> int:
    """Parse command-line paths and generate the requested guide."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    arguments = parser.parse_args()
    build_pdf(arguments.input.resolve(), arguments.output.resolve())
    print(arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
