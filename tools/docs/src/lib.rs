use regex::Regex;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Table {
    pub page: usize,
    pub title: String,
    pub category: String,
    pub rows: Vec<Vec<String>>,
}

#[derive(Serialize)]
struct ManifestTable {
    category: String,
    page: usize,
    context: String,
    file: String,
    ordinal: usize,
}

#[derive(Serialize)]
struct DocumentManifest {
    source_pdf: String,
    output: String,
    sha256: String,
    pages: usize,
    text_characters: usize,
    tables: Vec<ManifestTable>,
    warning: Option<String>,
}

#[derive(Serialize)]
struct Manifest {
    format: u8,
    source: String,
    documents: Vec<DocumentManifest>,
}

fn multi_space() -> &'static Regex {
    static PATTERN: OnceLock<Regex> = OnceLock::new();
    PATTERN.get_or_init(|| Regex::new(r"\s{2,}").unwrap())
}
fn page_number() -> &'static Regex {
    static PATTERN: OnceLock<Regex> = OnceLock::new();
    PATTERN.get_or_init(|| Regex::new(r"(?i)^\s*(?:page\s+)?\d+(?:\s+of\s+\d+)?\s*$").unwrap())
}
fn heading() -> &'static Regex {
    static PATTERN: OnceLock<Regex> = OnceLock::new();
    PATTERN.get_or_init(|| {
        Regex::new(r"^(?:\d+(?:\.\d+)*\.?\s+)?[A-Z][A-Za-z0-9 /,&()_+.-]{2,}$").unwrap()
    })
}
fn compact_cell() -> &'static Regex {
    static PATTERN: OnceLock<Regex> = OnceLock::new();
    PATTERN.get_or_init(|| Regex::new(r"(?i)\d|[A-Z]{2,}|[_/]|\b(?:yes|no|on|off)\b").unwrap())
}

pub fn split_columns(line: &str) -> Vec<String> {
    multi_space()
        .split(line.trim())
        .filter(|part| !part.trim().is_empty())
        .map(|part| part.trim().to_owned())
        .collect()
}

fn is_table_row(line: &str) -> bool {
    let cells = split_columns(line);
    cells.len() >= 2
        && line.trim().len() >= 5
        && cells.iter().any(|cell| compact_cell().is_match(cell))
}

fn closest_title(lines: &[String], start: usize) -> String {
    for line in lines[start.saturating_sub(12)..start].iter().rev() {
        let candidate = line.trim();
        if candidate.is_empty() || page_number().is_match(candidate) {
            continue;
        }
        if heading().is_match(candidate) || candidate.len() < 100 {
            return candidate.to_owned();
        }
    }
    "Untitled table".to_owned()
}

pub fn classify_table(title: &str, rows: &[Vec<String>]) -> String {
    let mut text = title.to_lowercase();
    for row in rows.iter().take(3) {
        text.push(' ');
        text.push_str(&row.join(" ").to_lowercase());
    }
    for (category, words) in [
        (
            "bill-of-materials",
            &["bill of materials", "designator", "manufacturer", "mpn"][..],
        ),
        (
            "registers",
            &["register", "bit field", "offset", "bit(s)"][..],
        ),
        (
            "pinout-and-io",
            &["pin", "connector", "mio", "bank", "i/o", "io standard"][..],
        ),
        (
            "memory-map",
            &["memory map", "address map", "base address", "address range"][..],
        ),
        (
            "electrical-and-timing",
            &[
                "voltage",
                "current",
                "power",
                "timing",
                "temperature",
                "parameter",
            ][..],
        ),
        (
            "configuration",
            &["boot", "switch", "jumper", "configuration", "mode setting"][..],
        ),
        (
            "revision-history",
            &["revision", "version", "document control", "change history"][..],
        ),
    ] {
        if words.iter().any(|word| text.contains(word)) {
            return category.to_owned();
        }
    }
    "general".to_owned()
}

pub fn normalize_rows(mut rows: Vec<Vec<String>>) -> Vec<Vec<String>> {
    let mut counts: Vec<(usize, usize)> = Vec::new();
    for row in &rows {
        if row.len() >= 3 {
            if let Some((_, count)) = counts.iter_mut().find(|(length, _)| *length == row.len()) {
                *count += 1;
            } else {
                counts.push((row.len(), 1));
            }
        }
    }
    let mut width = 0;
    let mut highest_count = 0;
    for (length, count) in counts {
        if count > highest_count {
            width = length;
            highest_count = count;
        }
    }
    if width == 0 {
        width = rows.iter().map(Vec::len).max().unwrap_or(0);
    }
    let digits = Regex::new(r"^\d+$").unwrap();
    let designator = Regex::new(r"(?i)^[A-Z]+\d").unwrap();
    let mut normalized: Vec<Vec<String>> = Vec::new();
    for mut row in rows.drain(..) {
        if row.len() == width.saturating_sub(1)
            && row.len() >= 2
            && digits.is_match(&row[0])
            && digits.is_match(&row[1])
        {
            row.insert(2, String::new());
        }
        if row.len() < width && !normalized.is_empty() {
            let target = if row.len() == 1 && designator.is_match(&row[0]) && width >= 3 {
                2
            } else {
                width - 1
            };
            normalized.last_mut().unwrap()[target].push(' ');
            normalized.last_mut().unwrap()[target].push_str(&row.join(" "));
            continue;
        }
        if row.len() > width {
            let tail = row.split_off(width - 1).join(" ");
            row.push(tail);
        }
        row.resize(width, String::new());
        normalized.push(row);
    }
    normalized
}

pub fn extract_tables(lines: &[String], page: usize) -> Vec<Table> {
    let mut tables = Vec::new();
    let mut index = 0;
    while index < lines.len() {
        if !is_table_row(&lines[index]) {
            index += 1;
            continue;
        }
        let start = index;
        let mut rows = Vec::new();
        while index < lines.len() {
            if is_table_row(&lines[index]) {
                rows.push(split_columns(&lines[index]));
                index += 1;
            } else if !rows.is_empty()
                && lines[index + 1..usize::min(index + 5, lines.len())]
                    .iter()
                    .any(|line| is_table_row(line))
            {
                if !lines[index].trim().is_empty() {
                    rows.push(split_columns(&lines[index]));
                }
                index += 1;
            } else {
                break;
            }
        }
        if rows.len() >= 2 {
            let rows = normalize_rows(rows);
            let title = closest_title(lines, start);
            tables.push(Table {
                page,
                category: classify_table(&title, &rows),
                title,
                rows,
            });
        } else {
            index = usize::max(index, start + 1);
        }
    }
    tables
}

pub fn markdown_table(rows: &[Vec<String>]) -> String {
    let cell = |value: &String| {
        value
            .replace('|', "\\|")
            .replace('\n', " ")
            .trim()
            .to_owned()
    };
    let line = |row: &[String]| {
        format!(
            "| {} |",
            row.iter().map(cell).collect::<Vec<_>>().join(" | ")
        )
    };
    let mut result = vec![
        line(&rows[0]),
        format!(
            "| {} |",
            rows[0]
                .iter()
                .map(|_| "---")
                .collect::<Vec<_>>()
                .join(" | ")
        ),
    ];
    result.extend(rows[1..].iter().map(|row| line(row)));
    result.join("\n")
}

fn document_markdown(source: &Path, pages: &[Vec<String>], tables: &[Table]) -> String {
    let mut by_page = HashMap::new();
    for table in tables {
        *by_page.entry(table.page).or_insert(0usize) += 1;
    }
    let mut chunks = vec!["---".to_owned(), format!("source_pdf: {}", source.display()), "conversion: pdftotext -layout".to_owned(), "table_index: tables/index.md".to_owned(), "---".to_owned(), String::new(), format!("# {}", source.file_stem().unwrap().to_string_lossy()), String::new(), "This is extracted text. Use the table index for structured table data; page numbers refer to the source PDF.".to_owned()];
    for (number, lines) in pages.iter().enumerate() {
        let content: Vec<&str> = lines
            .iter()
            .map(String::as_str)
            .filter(|line| !line.trim().is_empty() && !page_number().is_match(line))
            .collect();
        if content.is_empty() {
            continue;
        }
        chunks.extend([
            String::new(),
            format!("## Source page {}", number + 1),
            String::new(),
            content
                .iter()
                .map(|line| line.trim_end())
                .collect::<Vec<_>>()
                .join("\n"),
        ]);
        if let Some(count) = by_page.get(&(number + 1)) {
            chunks.extend([
                String::new(),
                format!(
                    "_Structured tables extracted from this page: {count}. See `tables/index.md`._"
                ),
            ]);
        }
    }
    format!("{}\n", chunks.join("\n").trim_end())
}

fn run_pdftotext(pdf: &Path) -> Result<String, String> {
    let output = Command::new("pdftotext")
        .args(["-layout", "-enc", "UTF-8"])
        .arg(pdf)
        .arg("-")
        .output()
        .map_err(|error| format!("pdftotext is required: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "pdftotext failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    String::from_utf8(output.stdout).map_err(|error| error.to_string())
}

fn relative_path(path: &Path, root: &Path) -> Result<PathBuf, String> {
    path.strip_prefix(root)
        .map(Path::to_owned)
        .map_err(|_| format!("{} is outside {}", path.display(), root.display()))
}
fn write_file(path: &Path, contents: &str) -> Result<(), String> {
    fs::create_dir_all(path.parent().unwrap()).map_err(io_error)?;
    fs::write(path, contents).map_err(io_error)
}
fn io_error(error: io::Error) -> String {
    error.to_string()
}

fn ensure_ascii_json(json: String) -> String {
    let mut escaped = String::with_capacity(json.len());
    for character in json.chars() {
        match character as u32 {
            0..=0x7f => escaped.push(character),
            codepoint @ 0x80..=0xffff => escaped.push_str(&format!("\\u{codepoint:04x}")),
            codepoint => {
                let scalar = codepoint - 0x1_0000;
                let high = 0xd800 + (scalar >> 10);
                let low = 0xdc00 + (scalar & 0x3ff);
                escaped.push_str(&format!("\\u{high:04x}\\u{low:04x}"));
            }
        }
    }
    escaped
}

fn write_document(
    pdf: &Path,
    source_root: &Path,
    output_root: &Path,
) -> Result<DocumentManifest, String> {
    let relative = relative_path(pdf, source_root)?;
    let doc_root = output_root.join(relative.with_extension(""));
    let text = run_pdftotext(pdf)?;
    let mut pages: Vec<Vec<String>> = text
        .split('\u{c}')
        .map(|page| page.lines().map(str::to_owned).collect())
        .collect();
    if pages
        .last()
        .is_some_and(|page| page.iter().all(|line| line.trim().is_empty()))
    {
        pages.pop();
    }
    let tables: Vec<Table> = pages
        .iter()
        .enumerate()
        .flat_map(|(number, lines)| extract_tables(lines, number + 1))
        .collect();
    write_file(
        &doc_root.join("document.md"),
        &document_markdown(&relative, &pages, &tables),
    )?;
    let mut categories: BTreeMap<String, Vec<&Table>> = BTreeMap::new();
    for table in &tables {
        categories
            .entry(table.category.clone())
            .or_default()
            .push(table);
    }
    let mut index = vec![
        format!("# Tables: {}", pdf.file_name().unwrap().to_string_lossy()),
        String::new(),
        "Tables are grouped by inferred subject. Confirm critical values against the source PDF."
            .to_owned(),
        String::new(),
    ];
    let mut manifest_tables = Vec::new();
    for (category, entries) in categories {
        let target = doc_root.join("tables").join(format!("{category}.md"));
        let mut output = vec![
            format!(
                "# {}",
                category
                    .replace('-', " ")
                    .split_whitespace()
                    .map(|word| {
                        let mut chars = word.chars();
                        chars
                            .next()
                            .map(|first| first.to_uppercase().collect::<String>() + chars.as_str())
                            .unwrap_or_default()
                    })
                    .collect::<Vec<_>>()
                    .join(" ")
            ),
            String::new(),
        ];
        for (ordinal, table) in entries.iter().enumerate() {
            output.extend([
                format!("## Table {} — source page {}", ordinal + 1, table.page),
                String::new(),
                format!("Context: {}", table.title),
                String::new(),
                markdown_table(&table.rows),
                String::new(),
            ]);
            manifest_tables.push(ManifestTable {
                category: category.clone(),
                page: table.page,
                context: table.title.clone(),
                file: target
                    .strip_prefix(output_root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/"),
                ordinal: ordinal + 1,
            });
        }
        write_file(&target, &format!("{}\n", output.join("\n").trim_end()))?;
        index.push(format!(
            "- [{}](tables/{}.md): {} table(s)",
            category.replace('-', " "),
            category,
            entries.len()
        ));
    }
    write_file(
        &doc_root.join("tables/index.md"),
        &format!("{}\n", index.join("\n")),
    )?;
    let bytes = fs::read(pdf).map_err(io_error)?;
    Ok(DocumentManifest {
        source_pdf: relative.to_string_lossy().replace('\\', "/"),
        output: doc_root
            .strip_prefix(output_root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/"),
        sha256: format!("{:x}", Sha256::digest(bytes)),
        pages: pages.len(),
        text_characters: text.trim().chars().count(),
        tables: manifest_tables,
        warning: if text.trim().is_empty() {
            Some("No text layer found; OCR the PDF and rerun.".to_owned())
        } else {
            None
        },
    })
}

pub fn convert(source: &Path, output: &Path, clean: bool) -> Result<usize, String> {
    if !source.is_dir() {
        return Err(format!(
            "source directory does not exist: {}",
            source.display()
        ));
    }
    if clean && output.exists() {
        fs::remove_dir_all(output).map_err(io_error)?;
    }
    let mut lower_case_pdfs = Vec::new();
    let mut upper_case_pdfs = Vec::new();
    for entry in walk(source)? {
        let path = entry?;
        match path.extension().and_then(|ext| ext.to_str()) {
            Some("pdf") => lower_case_pdfs.push(path),
            Some("PDF") => upper_case_pdfs.push(path),
            _ => {}
        }
    }
    lower_case_pdfs.sort();
    upper_case_pdfs.sort();
    lower_case_pdfs.append(&mut upper_case_pdfs);
    let pdfs = lower_case_pdfs;
    if pdfs.is_empty() {
        return Err(format!("no PDFs found under {}", source.display()));
    }
    let mut documents = Vec::new();
    for pdf in &pdfs {
        eprintln!("Converting {}", pdf.display());
        documents.push(write_document(pdf, source, output)?);
    }
    fs::create_dir_all(output).map_err(io_error)?;
    let manifest = serde_json::to_string_pretty(&Manifest {
        format: 1,
        source: source.to_string_lossy().replace('\\', "/"),
        documents,
    })
    .map_err(|error| error.to_string())?;
    write_file(
        &output.join("manifest.json"),
        &format!("{}\n", ensure_ascii_json(manifest)),
    )?;
    write_file(&output.join("README.md"), "# Generated PDF documentation\n\nEach directory contains extracted source text (`document.md`) and categorized structured tables (`tables/`). `manifest.json` lets agents locate a table by source document, page, category, and table file. Rebuild with `bazel run //tools/docs:pdf_to_markdown -- --clean`.\n")?;
    Ok(pdfs.len())
}

fn walk(root: &Path) -> Result<Vec<Result<PathBuf, String>>, String> {
    let mut paths = Vec::new();
    for entry in fs::read_dir(root).map_err(io_error)? {
        let path = entry.map_err(io_error)?.path();
        if path.is_dir() {
            paths.extend(walk(&path)?);
        } else {
            paths.push(Ok(path));
        }
    }
    Ok(paths)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bom_is_extracted_and_categorized() {
        let lines = [
            "Bill of Materials",
            "#     Quantity     Designator     Manufacturer",
            "1     2            C1, C2         Murata",
            "2     1            U1             AMD",
        ]
        .map(str::to_owned);
        let tables = extract_tables(&lines, 7);
        assert_eq!(tables.len(), 1);
        assert_eq!(tables[0].category, "bill-of-materials");
        assert!(markdown_table(&tables[0].rows)
            .contains("| # | Quantity | Designator | Manufacturer |"));
    }
    #[test]
    fn register_category() {
        assert_eq!(
            classify_table(
                "UART Register Summary",
                &[vec!["Register".into(), "Offset".into()]]
            ),
            "registers"
        );
    }
    #[test]
    fn missing_middle_cell_remains_a_row() {
        let rows = normalize_rows(vec![
            vec![
                "#".into(),
                "Quantity".into(),
                "Designator".into(),
                "Description".into(),
                "Maker".into(),
            ],
            vec!["4".into(), "25".into(), "capacitor".into(), "TDK".into()],
        ]);
        assert_eq!(rows[1], vec!["4", "25", "", "capacitor", "TDK"]);
    }

    #[test]
    fn json_uses_ascii_escapes_for_reproducible_manifests() {
        assert_eq!(
            ensure_ascii_json("{\"label\":\"µ—\"}".into()),
            "{\"label\":\"\\u00b5\\u2014\"}"
        );
    }
}
