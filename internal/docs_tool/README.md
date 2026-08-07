# PDF documentation conversion

Convert every PDF under `docs/` into agent-readable Markdown:

```bash
bazel run //internal/docs_tool:pdf_to_markdown -- --clean
```

The generated output lives in `documentation/pdf/` and is intentionally not
hand edited. Each PDF gets:

- `document.md`: layout-preserving text, separated by source page.
- `tables/<category>.md`: Markdown tables grouped into practical categories
  such as `registers`, `pinout-and-io`, `memory-map`, and
  `bill-of-materials`.
- `tables/index.md`: document-local table catalogue.
- `manifest.json` (at the output root): source hash, page, context, category,
  and file for every table, intended for programmatic agent lookup.

The Bazel-built Rust binary uses Poppler's `pdftotext -layout`. It preserves text-layer PDFs. If the manifest reports a
missing text layer, OCR that source PDF first and rerun the converter. Table
categories are heuristic, so use the recorded source page to verify important
hardware values against the original PDF.

To convert a different directory without changing the default output:

```bash
bazel run //internal/docs_tool:pdf_to_markdown -- --source path/to/pdfs --output internal/documentation/other-pdfs --clean
```
