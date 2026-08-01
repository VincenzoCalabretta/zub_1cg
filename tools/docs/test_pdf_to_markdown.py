import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from pdf_to_markdown import classify_table, extract_tables, markdown_table, normalize_rows


class PdfToMarkdownTests(unittest.TestCase):
    def test_bom_is_extracted_and_categorized(self):
        lines = [
            "Bill of Materials",
            "#     Quantity     Designator     Manufacturer",
            "1     2            C1, C2         Murata",
            "2     1            U1             AMD",
        ]
        tables = extract_tables(lines, page=7)
        self.assertEqual(len(tables), 1)
        self.assertEqual(tables[0].category, "bill-of-materials")
        self.assertIn("| # | Quantity | Designator | Manufacturer |", markdown_table(tables[0].rows))

    def test_register_category(self):
        self.assertEqual(classify_table("UART Register Summary", [["Register", "Offset"]]), "registers")

    def test_missing_middle_cell_remains_a_row(self):
        rows = normalize_rows([["#", "Quantity", "Designator", "Description", "Maker"], ["4", "25", "capacitor", "TDK"]])
        self.assertEqual(rows[1], ["4", "25", "", "capacitor", "TDK"])


if __name__ == "__main__":
    unittest.main()
