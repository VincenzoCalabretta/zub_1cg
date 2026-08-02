# Pinned Orbtrace compatibility source

The protocol behavior is derived from Orbtrace commit
`e416cd5b074fc725a04cf6b877d458e2074467ae` and retains its BSD-3-Clause
attribution. The relevant immutable inputs are:

| Upstream path | SHA-256 |
|---|---|
| `orbtrace/trace/cobs.py` | `bcf7864f3fce0557fccf1ca76de1b5a289dd31c50ebbf2ed8d3b1a13cac4b596` |
| `orbtrace/trace/orbflow.py` | `13aad689a7f6539cd730544602a239d32eb36ce6106560dccfa07532e80b8c72` |
| `orbtrace/trace/tpiu.py` | `617be3d79840c638c3e19637c8b4413cf79a9e05b6ecb99ff7f8e427360f41a1` |
| `tests/test_cobs.py` | `caa06dba00d4942e64c2ee1360c9f880ebe8df360274c8cd0f1d78709a657efd` |
| `tests/test_tpiu.py` | `d2f02c377c6c06dbbfb822ed9d20e41e82fa9ca05e5f9545e495e26cc02d85b4` |
| `tests/test_swo.py` | `aa207b98ebfd3e128c1beb99186a6a03844076bc1259d99a3b68306865ca2ba6` |
| `verilog/testbeds/stimfiles/fastitm.dat` | `960eedda095a4703637cf2df37ac52ab9602b1696a5bd55666dcb8c87d219293` |
| `verilog/testbeds/stimfiles/slowitm.dat` | `fbfec6e21fe36cb909ba6292755341069db5afa09524714365da8c213d7a911a` |
| `verilog/testbeds/stimfiles/slowitm.csv` | `0071fc8dffa704f7c2291e2fb4a01bc1cee9bb4223a40a6ca200f0c12a6b50ba` |

The Rust fixed vectors cover the COBS empty/zero/254-byte boundaries, checksum
wrapping, TPIU sync/unmangling, malformed SWO, and CMSIS-DAP WAIT/abort paths.
No upstream Python or Amaranth package is used at build or test time.

Orbtrace is Copyright its contributors and distributed under the BSD 3-Clause
License. See upstream `COPYING`; the repository `NOTICE` records this derived
component.
