# Repository guidance for agents

## Host-side tooling

- Do not add Python tooling, Python runtime dependencies, or Python-based test
  runners to this repository. Host-side tools and their tests are Rust.
- Build, test, and run Rust tools through Bazel. Do not document or rely on
  standalone `cargo build`, `cargo test`, or checked-in `target/` outputs.
- Rust binaries belong behind Bazel `rust_binary` targets so their outputs are
  cached in Bazel's output tree. Use `bazel build`, `bazel test`, and
  `bazel run` with `--config=host` for host tools.

## Rust dependencies

- Keep each Rust tool's `Cargo.toml` and `Cargo.lock` checked in as dependency
  inputs, not as an alternative build path.
- Register every tool's crates with the `crate_universe` module extension in
  `MODULE.bazel` via `crate.from_cargo`, then consume the generated repository
  from its Bazel targets. This keeps dependency fetching and compilation under
  Bazel's repository rules and cache.
- When a manifest or lockfile changes, allow Bazel to update `MODULE.bazel.lock`
  and commit that lockfile update with the dependency change.

## PDF documentation

- The committed Markdown under `documentation/pdf/` is generated; do not edit
  it manually.
- Regenerate it with:

  ```sh
  bazel run --config=host //tools/docs:pdf_to_markdown -- --clean
  ```

- The converter relies on Poppler's `pdftotext -layout`, which is supplied by
  the Nix development environment. Preserve deterministic output compatibility
  when modifying its extraction or manifest logic.
