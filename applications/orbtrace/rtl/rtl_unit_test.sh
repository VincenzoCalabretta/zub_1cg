#!/usr/bin/env bash
set -euo pipefail

: "${XILINX_VIVADO:?set XILINX_VIVADO to the Vivado installation directory}"
rtl_dir="${TEST_SRCDIR}/${TEST_WORKSPACE}/orbtrace/rtl"
work_dir="${TEST_TMPDIR}/orbtrace-xsim"
mkdir -p "${work_dir}"
cd "${work_dir}"

run_xsim() {
  local snapshot="$1"
  local success_marker="$2"
  local log="${snapshot}.log"

  "${XILINX_VIVADO}/bin/xsim" "${snapshot}" -runall 2>&1 | tee "${log}"
  if grep -Eq '(^|[[:space:]])(Fatal|Error):' "${log}"; then
    echo "XSim reported a fatal or error while running ${snapshot}" >&2
    return 1
  fi
  if ! grep -Fq "${success_marker}" "${log}"; then
    echo "XSim did not report the success marker for ${snapshot}" >&2
    return 1
  fi
}

"${XILINX_VIVADO}/bin/xvlog" -sv -i "${rtl_dir}" \
  "${rtl_dir}/orbtrace_ddr_capture.sv" \
  "${rtl_dir}/orbtrace_swo_nrz.sv" \
  "${rtl_dir}/orbtrace_swo_manchester.sv" \
  "${rtl_dir}/orbtrace_channel_packetizer.sv" \
  "${rtl_dir}/orbtrace_orbflow_encoder.sv" \
  "${rtl_dir}/orbtrace_dap_engine.sv" \
  "${rtl_dir}/tb/orbtrace_capture_tb.sv" \
  "${rtl_dir}/tb/orbtrace_dap_tb.sv" \
  "${rtl_dir}/tb/orbtrace_pipeline_tb.sv"
"${XILINX_VIVADO}/bin/xelab" orbtrace_capture_tb -s orbtrace_capture_tb_snapshot
run_xsim orbtrace_capture_tb_snapshot "orbtrace capture RTL tests passed"
"${XILINX_VIVADO}/bin/xelab" orbtrace_dap_tb -s orbtrace_dap_tb_snapshot
run_xsim orbtrace_dap_tb_snapshot "orbtrace CMSIS-DAP RTL tests passed"
"${XILINX_VIVADO}/bin/xelab" orbtrace_pipeline_tb -s orbtrace_pipeline_tb_snapshot
run_xsim orbtrace_pipeline_tb_snapshot "orbtrace randomized pipeline RTL tests passed"
