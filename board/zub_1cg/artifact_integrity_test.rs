use elf_check_lib::{parse_artifact_hashes, sha256_hex};
use std::path::PathBuf;

fn board_artifacts_dir() -> PathBuf {
    let base = std::env::var("RUNFILES_DIR")
        .or_else(|_| std::env::var("TEST_SRCDIR"))
        .expect("neither RUNFILES_DIR nor TEST_SRCDIR is set");
    PathBuf::from(base).join("_main").join("board/zub_1cg")
}

#[test]
fn board_artifacts_match_manifest() {
    let dir = board_artifacts_dir();
    let manifest_path = dir.join("artifacts.json");
    let manifest = std::fs::read_to_string(&manifest_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", manifest_path.display(), e));

    let entries = parse_artifact_hashes(&manifest);
    assert!(
        !entries.is_empty(),
        "artifacts.json yielded no (path, sha256) entries"
    );

    for (rel_path, expected) in entries {
        let full_path = dir.join(&rel_path);
        let data = std::fs::read(&full_path)
            .unwrap_or_else(|e| panic!("cannot read {}: {}", full_path.display(), e));
        let actual = sha256_hex(&data);
        assert_eq!(
            actual, expected,
            "SHA-256 mismatch for {rel_path}:\n  computed  {actual}\n  manifest  {expected}"
        );
    }
}
