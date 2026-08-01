#!/usr/bin/env python3
"""Convert PDFs in docs/ into agent-friendly Markdown and table indexes.

The converter intentionally relies only on Poppler's ``pdftotext`` command.
It therefore works in the Nix development shell without adding a Python
dependency.  PDFs must contain a text layer; scan-only PDFs are reported in
the manifest so they can be OCRed before running this tool again.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


PAGE_NUMBER = re.compile(r"^\s*(?:page\s+)?\d+(?:\s+of\s+\d+)?\s*$", re.I)
MULTI_SPACE = re.compile(r"\s{2,}")
HEADING = re.compile(r"^(?:\d+(?:\.\d+)*\.?\s+)?[A-Z][A-Za-z0-9 /,&()_+.-]{2,}$")


@dataclass
class Table:
    page: int
    title: str
    category: str
    rows: list[list[str]]


def split_columns(line: str) -> list[str]:
    """Split Poppler's layout-preserving text into table cells."""
    return [part.strip() for part in MULTI_SPACE.split(line.strip()) if part.strip()]


def is_table_row(line: str) -> bool:
    cells = split_columns(line)
    # Two columns alone are very commonly prose with indentation.  Requiring
    # at least one compact/value-like cell keeps false positives manageable.
    return len(cells) >= 2 and len(line.strip()) >= 5 and any(
        re.search(r"\d|[A-Z]{2,}|[_/]|\b(?:yes|no|on|off)\b", cell, re.I)
        for cell in cells
    )


def closest_title(lines: list[str], start: int) -> str:
    for line in reversed(lines[max(0, start - 12) : start]):
        candidate = line.strip()
        if not candidate or PAGE_NUMBER.match(candidate):
            continue
        if HEADING.match(candidate) or len(candidate) < 100:
            return candidate
    return "Untitled table"


def classify_table(title: str, rows: list[list[str]]) -> str:
    text = (title + " " + " ".join(" ".join(row) for row in rows[:3])).lower()
    rules = (
        ("bill-of-materials", ("bill of materials", "designator", "manufacturer", "mpn")),
        ("registers", ("register", "bit field", "offset", "bit(s)")),
        ("pinout-and-io", ("pin", "connector", "mio", "bank", "i/o", "io standard")),
        ("memory-map", ("memory map", "address map", "base address", "address range")),
        ("electrical-and-timing", ("voltage", "current", "power", "timing", "temperature", "parameter")),
        ("configuration", ("boot", "switch", "jumper", "configuration", "mode setting")),
        ("revision-history", ("revision", "version", "document control", "change history")),
    )
    for category, words in rules:
        if any(word in text for word in words):
            return category
    return "general"


def normalize_rows(rows: list[list[str]]) -> list[list[str]]:
    """Make ragged layout text a valid Markdown table without dropping cells."""
    # Wrapped cells are commonly split into two visual columns, so they must
    # not determine the schema width of an otherwise seven-column BOM. Prefer
    # the most frequent 3+ column shape and fall back to the widest row for
    # simple two-column tables.
    substantial = Counter(len(row) for row in rows if len(row) >= 3)
    width = substantial.most_common(1)[0][0] if substantial else max(map(len, rows))
    normalized: list[list[str]] = []
    for row in rows:
        if (
            len(row) == width - 1
            and len(row) >= 2
            and row[0].isdigit()
            and row[1].isdigit()
        ):
            # A layout extractor can omit an empty middle cell (notably a BOM
            # designator); retain the row instead of treating it as wrapping.
            row.insert(2, "")
        if len(row) < width and normalized:
            # A wrapped row in a PDF has no first-column value. Preserve it by
            # attaching the text to the previous row. Component-designator
            # continuations are more useful in the third column; other wraps
            # belong in the final cell.
            target = 2 if len(row) == 1 and re.match(r"^[A-Z]+\d", row[0], re.I) and width >= 3 else -1
            normalized[-1][target] += " " + " ".join(row)
            continue
        if len(row) > width:
            row = row[: width - 1] + [" ".join(row[width - 1 :])]
        normalized.append(row + [""] * (width - len(row)))
    return normalized


def extract_tables(lines: list[str], page: int) -> list[Table]:
    tables: list[Table] = []
    index = 0
    while index < len(lines):
        if not is_table_row(lines[index]):
            index += 1
            continue
        start = index
        rows: list[list[str]] = []
        while index < len(lines):
            line = lines[index]
            if is_table_row(line):
                rows.append(split_columns(line))
                index += 1
            elif rows and any(
                is_table_row(candidate) for candidate in lines[index + 1 : index + 5]
            ):
                # A cell may wrap onto one or more lines, and PDFs frequently
                # insert a blank line after it. Keep those fragments with the
                # table; normalize_rows attaches them to the preceding row.
                if line.strip():
                    rows.append(split_columns(line))
                index += 1
            else:
                break
        if len(rows) >= 2:
            rows = normalize_rows(rows)
            title = closest_title(lines, start)
            tables.append(Table(page, title, classify_table(title, rows), rows))
        else:
            index = max(index, start + 1)
    return tables


def markdown_table(rows: list[list[str]]) -> str:
    def cell(value: str) -> str:
        return value.replace("|", "\\|").replace("\n", " ").strip()

    header = rows[0]
    return "\n".join(
        ["| " + " | ".join(map(cell, header)) + " |", "| " + " | ".join("---" for _ in header) + " |"]
        + ["| " + " | ".join(map(cell, row)) + " |" for row in rows[1:]]
    )


def document_markdown(source: Path, pages: list[list[str]], tables: list[Table]) -> str:
    by_page = Counter(table.page for table in tables)
    chunks = [
        "---",
        f"source_pdf: {source.as_posix()}",
        "conversion: pdftotext -layout",
        "table_index: tables/index.md",
        "---",
        "",
        f"# {source.stem}",
        "",
        "This is extracted text. Use the table index for structured table data; page numbers refer to the source PDF.",
    ]
    for page_number, lines in enumerate(pages, 1):
        content = [line.rstrip() for line in lines if line.strip() and not PAGE_NUMBER.match(line)]
        if not content:
            continue
        chunks += ["", f"## Source page {page_number}", "", "\n".join(content)]
        if by_page[page_number]:
            chunks += ["", f"_Structured tables extracted from this page: {by_page[page_number]}. See `tables/index.md`._"]
    return "\n".join(chunks).rstrip() + "\n"


def run_pdftotext(pdf: Path) -> str:
    try:
        result = subprocess.run(
            ["pdftotext", "-layout", "-enc", "UTF-8", str(pdf), "-"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError as error:
        raise RuntimeError("pdftotext is required; install Poppler.") from error
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"pdftotext failed: {error.stderr.strip()}") from error
    return result.stdout


def write_document(pdf: Path, source_root: Path, output_root: Path) -> dict:
    relative = pdf.relative_to(source_root)
    doc_root = output_root / relative.with_suffix("")
    text = run_pdftotext(pdf)
    pages = [page.splitlines() for page in text.split("\f")]
    if pages and not any(line.strip() for line in pages[-1]):
        pages.pop()
    tables = [table for number, lines in enumerate(pages, 1) for table in extract_tables(lines, number)]
    doc_root.mkdir(parents=True, exist_ok=True)
    (doc_root / "document.md").write_text(document_markdown(relative, pages, tables), encoding="utf-8")

    categories: dict[str, list[Table]] = defaultdict(list)
    for table in tables:
        categories[table.category].append(table)
    index = [f"# Tables: {pdf.name}", "", "Tables are grouped by inferred subject. Confirm critical values against the source PDF.", ""]
    manifest_tables = []
    for category in sorted(categories):
        target = doc_root / "tables" / f"{category}.md"
        target.parent.mkdir(parents=True, exist_ok=True)
        entries = [f"# {category.replace('-', ' ').title()}", ""]
        for number, table in enumerate(categories[category], 1):
            entries += [f"## Table {number} — source page {table.page}", "", f"Context: {table.title}", "", markdown_table(table.rows), ""]
            manifest_tables.append({"category": category, "page": table.page, "context": table.title, "file": target.relative_to(output_root).as_posix(), "ordinal": number})
        target.write_text("\n".join(entries).rstrip() + "\n", encoding="utf-8")
        index.append(f"- [{category.replace('-', ' ')}](tables/{category}.md): {len(categories[category])} table(s)")
    (doc_root / "tables" / "index.md").write_text("\n".join(index) + "\n", encoding="utf-8")
    return {
        "source_pdf": relative.as_posix(),
        "output": doc_root.relative_to(output_root).as_posix(),
        "sha256": hashlib.sha256(pdf.read_bytes()).hexdigest(),
        "pages": len(pages),
        "text_characters": len(text.strip()),
        "tables": manifest_tables,
        "warning": None if text.strip() else "No text layer found; OCR the PDF and rerun.",
    }


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("docs"), help="directory searched recursively for PDFs (default: docs)")
    parser.add_argument("--output", type=Path, default=Path("documentation/pdf"), help="generated documentation directory (default: documentation/pdf)")
    parser.add_argument("--clean", action="store_true", help="remove the output directory before conversion")
    args = parser.parse_args(argv)
    if not args.source.is_dir():
        parser.error(f"source directory does not exist: {args.source}")
    if shutil.which("pdftotext") is None:
        parser.error("pdftotext is required; install Poppler first")
    if args.output.exists() and args.clean:
        shutil.rmtree(args.output)
    pdfs = sorted(args.source.rglob("*.pdf")) + sorted(args.source.rglob("*.PDF"))
    if not pdfs:
        parser.error(f"no PDFs found under {args.source}")
    documents = []
    for pdf in pdfs:
        print(f"Converting {pdf}", file=sys.stderr)
        documents.append(write_document(pdf, args.source, args.output))
    manifest = {"format": 1, "source": args.source.as_posix(), "documents": documents}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (args.output / "README.md").write_text(
        "# Generated PDF documentation\n\n"
        "Each directory contains extracted source text (`document.md`) and categorized structured tables (`tables/`). "
        "`manifest.json` lets agents locate a table by source document, page, category, and table file. "
        "Rebuild with `python3 tools/docs/pdf_to_markdown.py --clean`.\n",
        encoding="utf-8",
    )
    print(f"Converted {len(documents)} PDF(s) into {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
