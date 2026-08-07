{
  description = "Reusable AES-ZUB-1CG board SDK (Bazel 8 + Nix)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      mkBootgen = pkgs: pkgs.stdenv.mkDerivation {
        pname = "bootgen";
        version = "2026.1";
        src = pkgs.fetchFromGitHub {
          owner = "Xilinx";
          repo = "bootgen";
          rev = "e576e5e1e227a74c31e54607623c1bb7f38eec12";
          sha256 = "sha256-gz15FkMQ5aQahGVLe0k6KDA/1WhTVIZUA4qCq2iWTFM=";
        };
        nativeBuildInputs = [ pkgs.gnumake pkgs.gcc pkgs.openssl.dev ];
        buildInputs = [ pkgs.openssl ];
        env.NIX_CFLAGS_COMPILE =
          "-Wno-deprecated-declarations -Wno-incompatible-pointer-types";
        makeFlags = [
          "CXXFLAGS=-std=c++14 -O -Wall -Wno-reorder -Wno-aligned-new -Wno-misleading-indentation -Wno-class-memaccess"
        ];
        installPhase = ''
          mkdir -p $out/bin
          cp build/bin/bootgen $out/bin/
        '';
      };

      mkDevShell = {
        system,
        pkgs ? import nixpkgs { inherit system; },
        extraPackages ? [ ],
        extraShellHook ? "",
      }:
        let
          bootgen = mkBootgen pkgs;
          armGcc = pkgs.gcc-arm-embedded;
          aarch64Gcc = pkgs.pkgsCross.aarch64-embedded.buildPackages.gcc;
        in pkgs.mkShell {
          name = "zub_1cg-sdk";
          packages = [
            armGcc
            aarch64Gcc
            pkgs.bazel_8
            pkgs.buildifier
            pkgs.rustc
            pkgs.cargo
            pkgs.rustfmt
            pkgs.clippy
            pkgs.pkg-config
            pkgs.systemdMinimal
            pkgs.poppler-utils
            bootgen
            pkgs.openocd
            pkgs.picocom
            pkgs.openssl
            pkgs.udisks2
            pkgs.usbutils
            pkgs.util-linux
            pkgs.git
            pkgs.file
            pkgs.which
            pkgs.jq
          ] ++ extraPackages;
          shellHook = ''
            export ARM_GCC_BIN="${armGcc}/bin"
            export AARCH64_GCC_BIN="${aarch64Gcc}/bin"
            export BOOTGEN="${bootgen}/bin/bootgen"
            export OPENOCD="${pkgs.openocd}/bin/openocd"
            export PKG_CONFIG_PATH="${pkgs.systemdMinimal.dev}/lib/pkgconfig''${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
            ${extraShellHook}
          '';
        };
    in {
      lib.mkDevShell = mkDevShell;
    } // flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs { inherit system; };
        bootgen = mkBootgen pkgs;
        toolBundle = pkgs.symlinkJoin {
          name = "zub_1cg-tools";
          paths = [
            bootgen
            pkgs.openocd
            pkgs.gcc-arm-embedded
            pkgs.pkgsCross.aarch64-embedded.buildPackages.gcc
          ];
        };
      in {
        packages = {
          inherit bootgen;
          tooling = toolBundle;
          default = toolBundle;
        };
        devShells.default = mkDevShell { inherit system pkgs; };
      });
}
