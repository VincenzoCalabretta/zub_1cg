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
        bashInteractive binutils coreutils findutils gawk gnugrep gnused gzip which
        util-linux procps file git gnumake perl python3 glibc.dev libgcc stdenv.cc.cc zlib
        openssl libxcrypt libxcrypt-legacy ncurses ncurses5 readline libxml2 expat
        fontconfig freetype glib gtk3 pango cairo gdk-pixbuf at-spi2-atk dbus
        nss nspr cups alsa-lib libdrm mesa libGL libsecret
        libx11 libxau libxcomposite libxcursor libxdamage libxdmcp libxext
        libxfixes libxi libxinerama libxrandr libxrender libxscrnsaver libxtst
        libxcb libxshmfence libxkbcommon
      ];
      mkXilinxTool = pkgs: {
        tool,
        version ? "2023.2",
        xsctRoot ? "Vitis",
      }: pkgs.buildFHSEnv {
        # Keep the command name stable inside a development shell.  Nix still
        # keeps different tool versions separate through their derivations.
        name = tool;
        targetPkgs = _: xilinxRuntimePkgs pkgs;
        runScript = pkgs.writeShellScript "run-${tool}" ''
          set -euo pipefail
          : "''${XILINX_ROOT:?set XILINX_ROOT to the root containing Vivado/${version} and ${xsctRoot}/${version}}"
          unset GTK_PATH GDK_PIXBUF_MODULE_FILE GI_TYPELIB_PATH XDG_CONFIG_DIRS XDG_DATA_DIRS
          export GDK_BACKEND=x11
          export XILINX_VIVADO="$XILINX_ROOT/Vivado/${version}"
          ${if xsctRoot == "Vitis" then ''export XILINX_VITIS="$XILINX_ROOT/Vitis/${version}"'' else ''export XILINX_SDK="$XILINX_ROOT/SDK/${version}"''}
          export LD_LIBRARY_PATH="${pkgs.ncurses5}/lib:${pkgs.libxcrypt-legacy}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
          # XSim invokes GCC directly and expects the FHS C runtime startup
          # objects in its conventional library search path.
          export LIBRARY_PATH="/usr/lib64''${LIBRARY_PATH:+:$LIBRARY_PATH}"
          executable="$XILINX_ROOT/${if tool == "xsct" then xsctRoot else "Vivado"}/${version}/bin/${tool}"
          if [[ ! -x "$executable" ]]; then
            echo "missing licensed AMD tool: $executable" >&2
            exit 2
          fi
          exec "$executable" ${if tool == "xsct" then "-nodisp" else ""} "$@"
        '';
      };
      # Use this for the vendor installer itself on NixOS, where its hardcoded
      # /bin/bash and /bin/rm paths do not exist outside an FHS environment.
      mkXilinxInstaller = pkgs: pkgs.buildFHSEnv {
        name = "xilinx-installer";
        targetPkgs = _: xilinxRuntimePkgs pkgs;
        runScript = pkgs.writeShellScript "run-xilinx-installer" ''
          exec /bin/bash "$@"
        '';
      };

      mkDevShell = {
        system,
        pkgs ? import nixpkgs { inherit system; },
        extraPackages ? [ ],
        extraShellHook ? "",
        xilinxVersion ? "2023.2",
        xilinxXsctRoot ? "Vitis",
      }:
        let
          bootgen = mkBootgen pkgs;
          armGcc = pkgs.gcc-arm-embedded;
          aarch64Gcc = pkgs.pkgsCross.aarch64-embedded.buildPackages.gcc;
          mkTool = tool: mkXilinxTool pkgs {
            inherit tool;
            version = xilinxVersion;
            xsctRoot = xilinxXsctRoot;
          };
          vivado = mkTool "vivado";
          xvlog = mkTool "xvlog";
          xelab = mkTool "xelab";
          xsim = mkTool "xsim";
          xsct = mkTool "xsct";
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
            xvlog
            xelab
            xsim
            xsct
          ] ++ extraPackages;
          shellHook = ''
            export ARM_GCC_BIN="${armGcc}/bin"
            export AARCH64_GCC_BIN="${aarch64Gcc}/bin"
            export BOOTGEN="${bootgen}/bin/bootgen"
            export OPENOCD="${pkgs.openocd}/bin/openocd"
            export VIVADO="${vivado}/bin/vivado"
            export XVLOG="${xvlog}/bin/xvlog"
            export XELAB="${xelab}/bin/xelab"
            export XSIM="${xsim}/bin/xsim"
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
        mkTool = tool: mkXilinxTool pkgs { inherit tool; };
        vivado = mkTool "vivado";
        xvlog = mkTool "xvlog";
        xelab = mkTool "xelab";
        xsim = mkTool "xsim";
        xsct = mkTool "xsct";
        xilinxInstaller = mkXilinxInstaller pkgs;
        mk2019Tool = tool: mkXilinxTool pkgs {
          inherit tool;
          version = "2019.1";
          xsctRoot = "SDK";
        };
        vivado2019 = mk2019Tool "vivado";
        xvlog2019 = mk2019Tool "xvlog";
        xelab2019 = mk2019Tool "xelab";
        xsim2019 = mk2019Tool "xsim";
        xsct2019 = mk2019Tool "xsct";
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
          inherit bootgen vivado xelab xsim xsct xvlog xilinxInstaller;
          vivado_2019 = vivado2019;
          xvlog_2019 = xvlog2019;
          xelab_2019 = xelab2019;
          xsim_2019 = xsim2019;
          xsct_2019 = xsct2019;
          xilinx_installer = xilinxInstaller;
          tooling = toolBundle;
          default = toolBundle;
        };
        devShells.default = mkDevShell { inherit system pkgs; };
        devShells.vivado_2019 = mkDevShell {
          inherit system pkgs;
          xilinxVersion = "2019.1";
          xilinxXsctRoot = "SDK";
        };
      });
}
