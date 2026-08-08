load("@rules_rust//rust:toolchain.bzl", "rust_stdlib_filegroup")

rust_stdlib_filegroup(
    name = "rust_std-aarch64-unknown-none",
    srcs = glob(
        [
            "lib/rustlib/aarch64-unknown-none/lib/*.rlib",
            "lib/rustlib/aarch64-unknown-none/lib/self-contained/**",
        ],
        allow_empty = True,
    ),
    visibility = ["//visibility:public"],
)
