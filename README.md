# zub_1cg board SDK

`zub_1cg` is a versioned Bazel/Nix SDK for the Avnet AES-ZUB-1CG. It contains
the exact board support, cross toolchains, firmware rules, artifacts, and host
workflow used by this repository. It is intentionally not a generic ZynqMP
framework.

## Consume the SDK

Pin one Git revision in both dependency systems. Until registry publication,
the Bazel module uses a `git_override`:

```starlark
bazel_dep(name = "zub_1cg", version = "0.1.0")
git_override(
    module_name = "zub_1cg",
    remote = "https://github.com/your-org/zub_1cg.git",
    commit = "<REV>",
)
```

Use the same revision from Nix and compose the exported shell helper:

```nix
inputs.zub_1cg.url = "github:your-org/zub_1cg/<REV>";

devShells.x86_64-linux.default = zub_1cg.lib.mkDevShell {
  system = "x86_64-linux";
  extraPackages = [ ];
  extraShellHook = "";
};
```

The shell provides Bazel 8, both cross compilers, Rust, bootgen, and OpenOCD.
It exports compiler discovery variables but never creates or edits consumer
workspace files.

Firmware targets own their platform transition, so no consumer `.bazelrc`
configuration is needed:

```starlark
load("@zub_1cg//sdk/rules:defs.bzl", "a53_firmware", "r5_firmware", "zub_boot_image")

a53_firmware(name = "a53", srcs = ["a53.c"])
r5_firmware(
    name = "r5",
    srcs = ["r5.c"],
    deps = ["@zub_1cg//sdk/rtos:threadx_r5"],
)
zub_boot_image(name = "boot", r5_firmware = ":r5")
```

Supported Bazel APIs are under `//sdk` and supported host/board workflow entry
points are under `//tooling`. Fetched repositories and `//third_party` are
implementation details.

## Build the bundled applications

From `nix develop`:

```sh
bazel build //applications/apu/...
bazel build //applications/rpu/...
bazel test --config=host //applications/orbtrace:tests
bazel test --config=host //tooling/... //sdk/boards/zub_1cg:artifact_integrity_test
```

Hardware, XSCT, and Vivado targets are manual and require the corresponding
board or proprietary installation. The standalone consumer fixture in
`tests/consumer` has its own module, flake, and `.bazelrc` and uses a
`local_path_override` for repository testing.
