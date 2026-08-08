# PDF documentation conversion

This tool converts local PDFs into agent-readable Markdown for private
engineering use. Vendor PDFs and generated extractions must not be committed
to the public `zub_1cg` repository.

With the access-controlled documentation repository checked out as a sibling,
run:

```bash
bazel run //internal/docs_tool:pdf_to_markdown -- \
  --source ../zub_1cg_documentation_private/internal/reference_docs \
  --output ../zub_1cg_documentation_private/internal/documentation/pdf \
  --clean
```

The output is intentionally not hand edited. Each PDF gets:

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
