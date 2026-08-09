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

      # Public FHS wrappers around a separately installed proprietary
      # Vivado/Vitis tree.  The wrapper contains no AMD software and resolves
      # the installation through XILINX_ROOT only when it is executed.
      xilinxRuntimePkgs = pkgs: with pkgs; [
        bashInteractive coreutils findutils gawk gnugrep gnused gzip which
        util-linux procps file git gnumake perl python3 libgcc stdenv.cc.cc zlib
        openssl libxcrypt libxcrypt-legacy ncurses ncurses5 readline libxml2 expat
        fontconfig freetype glib gtk3 pango cairo gdk-pixbuf at-spi2-atk dbus
        nss nspr cups alsa-lib libdrm mesa libGL libsecret
        libx11 libxau libxcomposite libxcursor libxdamage libxdmcp libxext
        libxfixes libxi libxinerama libxrandr libxrender libxscrnsaver libxtst
        libxcb libxshmfence libxkbcommon
      ];
      mkXilinxTool = pkgs: tool: pkgs.buildFHSEnv {
        name = tool;
        targetPkgs = _: xilinxRuntimePkgs pkgs;
        runScript = pkgs.writeShellScript "run-${tool}" ''
          set -euo pipefail
          : "''${XILINX_ROOT:?set XILINX_ROOT to the root containing Vivado/2023.2 and Vitis/2023.2}"
          unset GTK_PATH GDK_PIXBUF_MODULE_FILE GI_TYPELIB_PATH XDG_CONFIG_DIRS XDG_DATA_DIRS
          export GDK_BACKEND=x11
          export XILINX_VITIS="$XILINX_ROOT/Vitis/2023.2"
          export XILINX_VIVADO="$XILINX_ROOT/Vivado/2023.2"
          export LD_LIBRARY_PATH="${pkgs.ncurses5}/lib:${pkgs.libxcrypt-legacy}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
          executable="$XILINX_ROOT/${if tool == "vivado" then "Vivado" else "Vitis"}/2023.2/bin/${tool}"
          if [[ ! -x "$executable" ]]; then
            echo "missing licensed AMD tool: $executable" >&2
            exit 2
          fi
          exec "$executable" ${if tool == "xsct" then "-nodisp" else ""} "$@"
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
          vivado = mkXilinxTool pkgs "vivado";
          xsct = mkXilinxTool pkgs "xsct";
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
            vivado
            xsct
          ] ++ extraPackages;
          shellHook = ''
            export ARM_GCC_BIN="${armGcc}/bin"
            export AARCH64_GCC_BIN="${aarch64Gcc}/bin"
            export BOOTGEN="${bootgen}/bin/bootgen"
            export OPENOCD="${pkgs.openocd}/bin/openocd"
            export VIVADO="${vivado}/bin/vivado"
            export XSCT="${xsct}/bin/xsct"
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
        vivado = mkXilinxTool pkgs "vivado";
        xsct = mkXilinxTool pkgs "xsct";
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
          inherit bootgen vivado xsct;
          tooling = toolBundle;
          default = toolBundle;
        };
        devShells.default = mkDevShell { inherit system pkgs; };
      });
}
