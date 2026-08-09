#!/usr/bin/env bash
set -euo pipefail

: "${VIVADO:?set VIVADO to the licensed Vivado 2023.2 executable or wrapper}"
: "${XSCT:?set XSCT to the licensed Vitis 2023.2 xsct executable or wrapper}"
[[ -x "$VIVADO" ]] || { echo "VIVADO is not executable: $VIVADO" >&2; exit 2; }
[[ -x "$XSCT" ]] || { echo "XSCT is not executable: $XSCT" >&2; exit 2; }
"$VIVADO" -version | head -1 | grep -q 'v2023.2' || {
  echo "the ZUBoard PS workflow is qualified only with Vivado 2023.2" >&2
  exit 2
}

workspace="${BUILD_WORKSPACE_DIRECTORY:?run with bazel run}"
runfiles_root="${RUNFILES_DIR:-$0.runfiles}"
board_xml="$(find -L "$runfiles_root" -path '*/zub1cg/1.2/board.xml' -type f | head -1)"
preset_helper="$(find -L "$runfiles_root" -path '*/sdk/boards/zub_1cg/board_preset.tcl' -type f | head -1)"
create_script="$(find -L "$runfiles_root" -path '*/sdk/boards/zub_1cg/create_ps_handoff.tcl' -type f | head -1)"
export_script="$(find -L "$runfiles_root" -path '*/sdk/boards/zub_1cg/export_psu_init.tcl' -type f | head -1)"
for required in "$board_xml" "$preset_helper" "$create_script" "$export_script"; do
  [[ -f "$required" ]] || { echo "missing generator runfile: $required" >&2; exit 2; }
done
bdf_root="$(dirname "$(dirname "$(dirname "$board_xml")")")"
output_dir="$workspace/sdk/boards/zub_1cg/generated"
scratch="$output_dir/.tmp"
mkdir -p "$output_dir"
rm -rf "$scratch"
mkdir -p "$scratch"
trap 'rm -rf "$scratch"' EXIT

LC_ALL=C TZ=UTC "$VIVADO" -mode batch -source "$create_script" \
  -tclargs "$workspace" "$scratch" "$bdf_root" "$preset_helper"
LC_ALL=C TZ=UTC "$XSCT" "$export_script" "$scratch/zub1cg_ps.xsa" "$scratch"

generated="$scratch/psu_init.tcl"
grep -q '^proc psu_init' "$generated" || { echo "generated file has no psu_init procedure" >&2; exit 1; }
grep -q '^proc psu_post_config' "$generated" || { echo "generated file has no psu_post_config procedure" >&2; exit 1; }
actual_sha="$(sha256sum "$generated" | cut -d' ' -f1)"
expected_sha="${ZUB1CG_EXPECTED_PSINIT_SHA256:-ee38a3b846798523c7278b0219dd20befdb868e4da40f5bba241f5772f56d2dc}"
if [[ "$actual_sha" != "$expected_sha" ]]; then
  echo "psu_init.tcl SHA-256 mismatch: got $actual_sha, expected $expected_sha" >&2
  exit 1
fi

install -m 0644 "$generated" "$output_dir/psu_init.tcl"
install -m 0644 "$scratch/zub1cg_ps.xsa" "$output_dir/zub1cg_ps.xsa"
install -m 0644 "$scratch/ps_configuration.json" "$output_dir/ps_configuration.json"
printf '%s  psu_init.tcl\n' "$actual_sha" > "$output_dir/psu_init.sha256"
echo "generated licensed local artifact: $output_dir/psu_init.tcl"
echo "SHA-256: $actual_sha"
