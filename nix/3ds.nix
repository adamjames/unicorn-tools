# 3DS homebrew build environment
# Uses Docker for devkitARM toolchain
{ pkgs, unstable }:

let
  # 3gxtool for local use (inspecting 3gx files, etc.)
  threegxtool = pkgs.stdenv.mkDerivation rec {
    pname = "3gxtool";
    version = "1.3";

    src = pkgs.fetchFromGitLab {
      owner = "thepixellizeross";
      repo = "3gxtool";
      rev = "v${version}";
      hash = "sha256-bT16YtCNqiAtkQ2+1eSDojN+WSHYG3nWlGlsGDYA5Kc=";
      fetchSubmodules = true;
    };

    nativeBuildInputs = [ pkgs.cmake pkgs.gnumake ];

    # Don't use cmake for main build - only for yaml-cpp
    dontUseCmakeConfigure = true;

    buildPhase = ''
      runHook preBuild

      # Build yaml-cpp
      cd extern/yaml-cpp
      mkdir build && cd build
      cmake -DYAML_CPP_BUILD_TESTS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
      make -j$NIX_BUILD_CORES
      cd ../../..

      # Set up yaml-cpp for 3gxtool
      mkdir -p lib/yaml-cpp/lib lib/yaml-cpp/include
      cp extern/yaml-cpp/build/libyaml-cpp.a lib/yaml-cpp/lib/
      cp -r extern/yaml-cpp/include/* lib/yaml-cpp/include/
      echo '#include "yaml-cpp/yaml.h"' > lib/yaml-cpp/include/yaml.h

      # Patch Makefile - remove static linking, fix .exe suffix, add dynalo include
      sed -i 's/-static -static-libgcc -static-libstdc++//' Makefile
      sed -i 's/INCLUDES.*:=.*includes/INCLUDES := includes extern\/dynalo\/include\/dynalo/' Makefile
      # Change output from .exe to no extension
      sed -i 's/\$(OUTPUT)\.exe/$(OUTPUT)/g' Makefile
      sed -i 's/\.exe:/:/g' Makefile
      # Fix the .d include that breaks parallel builds - remove -include line
      sed -i '/-include \$(DEPENDS)/d' Makefile

      # Force single-threaded due to broken Makefile dependencies
      make -j1

      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p $out/bin
      # Makefile names output after directory (source), we rename to 3gxtool
      cp source $out/bin/3gxtool
      runHook postInstall
    '';

    meta = {
      description = "Tool for creating and manipulating 3GX plugin files";
      homepage = "https://gitlab.com/thepixellizeross/3gxtool";
    };
  };


  # Build script for 3DS sysmodule
  buildSysmodule = pkgs.writeShellScriptBin "cosmic-3ds-build" ''
    set -euo pipefail

    PROJECT_DIR="''${1:-$(pwd)/firmware/3ds}"

    echo "Building 3DS sysmodule..."

    # Title ID for sysmodule
    TITLE_ID="0004013000002C02"

    # Clean previous build
    rm -rf "$PROJECT_DIR/sysmodule/build" "$PROJECT_DIR/sysmodule"/*.3dsx "$PROJECT_DIR/sysmodule"/*.elf "$PROJECT_DIR"/*.cxi 2>/dev/null || true

    ${pkgs.docker}/bin/docker run --rm \
      -v "$PROJECT_DIR:/work" \
      -w /work/sysmodule \
      -e DEVKITPRO=/opt/devkitpro \
      -e DEVKITARM=/opt/devkitpro/devkitARM \
      devkitpro/devkitarm:latest \
      bash -c '
        # Download makerom
        curl -sL https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/makerom-v0.18.4-ubuntu_x86_64.zip -o /tmp/makerom.zip
        unzip -q /tmp/makerom.zip -d /tmp
        chmod +x /tmp/makerom
        cp /tmp/makerom /opt/devkitpro/tools/bin/

        make clean 2>/dev/null || true
        make && make cxi

        # Build launcher
        if [ -d ../launcher ]; then
          cd ../launcher
          make clean 2>/dev/null || true
          make
        fi
      '

    if [[ -f "$PROJECT_DIR/sysmodule/$TITLE_ID.cxi" ]]; then
      mv "$PROJECT_DIR/sysmodule/$TITLE_ID.cxi" "$PROJECT_DIR/"
      echo "Built: $PROJECT_DIR/$TITLE_ID.cxi"
    fi
  '';

  # Build script for 3GX plugin
  buildPlugin = pkgs.writeShellScriptBin "cosmic-3gx-build" ''
    set -euo pipefail

    PROJECT_DIR="''${1:-$(pwd)/firmware/3ds}"
    PLUGIN_DIR="$PROJECT_DIR/plugin"
    TOOLS_DIR="$PROJECT_DIR/.tools"

    echo "Building 3GX plugin..."

    mkdir -p "$TOOLS_DIR"

    rm -rf "$PLUGIN_DIR"/*.3gx "$PLUGIN_DIR"/*.elf "$PLUGIN_DIR"/build 2>/dev/null || true

    # Build everything inside docker (3gxtool + libctrpf + plugin)
    # 3gxtool must be built inside docker due to glibc incompatibility with NixOS
    ${pkgs.docker}/bin/docker run --rm \
      -v "$PROJECT_DIR:/work" \
      -w /work/plugin \
      -e DEVKITPRO=/opt/devkitpro \
      -e DEVKITARM=/opt/devkitpro/devkitARM \
      devkitpro/devkitarm:latest \
      bash -c '
        set -e

        # Build/cache 3gxtool inside docker
        if [[ ! -x /work/.tools/3gxtool-docker ]]; then
          echo "Building 3gxtool v1.3 inside docker..."
          cd /tmp
          git clone --branch v1.3 --recurse-submodules https://gitlab.com/thepixellizeross/3gxtool.git
          cd 3gxtool

          cd extern/yaml-cpp
          mkdir build && cd build
          cmake -DYAML_CPP_BUILD_TESTS=OFF .. && make -j$(nproc)
          cd ../../..

          mkdir -p lib/yaml-cpp/lib lib/yaml-cpp/include
          cp extern/yaml-cpp/build/libyaml-cpp.a lib/yaml-cpp/lib/
          cp -r extern/yaml-cpp/include/* lib/yaml-cpp/include/
          echo "#include \"yaml-cpp/yaml.h\"" > lib/yaml-cpp/include/yaml.h

          sed -i "s/-static -static-libgcc -static-libstdc++//" Makefile
          sed -i "s/\.exe//g" Makefile
          sed -i "s|INCLUDES.*:=.*includes|INCLUDES := includes extern/dynalo/include/dynalo|" Makefile

          make -j$(nproc)
          mkdir -p /work/.tools
          cp 3gxtool /work/.tools/3gxtool-docker
          chmod +x /work/.tools/3gxtool-docker
        fi

        # Add cached 3gxtool to PATH
        export PATH="/work/.tools:$PATH"
        ln -sf /work/.tools/3gxtool-docker /work/.tools/3gxtool 2>/dev/null || true

        # Install libctrpf (cached or build fresh)
        if [[ -f /work/.tools/libctrpf/lib/libctrpf.a ]]; then
          mkdir -p /opt/devkitpro/libctrpf
          cp -r /work/.tools/libctrpf/* /opt/devkitpro/libctrpf/
        else
          cd /tmp
          git clone --branch 0.8.0 --depth 1 https://gitlab.com/thepixellizeross/ctrpluginframework.git
          cd ctrpluginframework/Library
          make
          mkdir -p /opt/devkitpro/libctrpf/lib /opt/devkitpro/libctrpf/include
          cp lib/libctrpf.a /opt/devkitpro/libctrpf/lib/
          cp -r include/* /opt/devkitpro/libctrpf/include/
          mkdir -p /work/.tools/libctrpf
          cp -r /opt/devkitpro/libctrpf/* /work/.tools/libctrpf/
        fi

        cd /work/plugin
        make clean 2>/dev/null || true
        make
      '

    echo "Built: $PLUGIN_DIR/*.3gx"
  '';

  # Deploy plugin to 3DS via FTP
  deployScript = pkgs.writeShellScriptBin "cosmic-3ds-deploy" ''
    set -euo pipefail

    PROJECT_DIR="''${1:-$(pwd)/firmware/3ds}"
    FTP_HOST="''${2:-10.0.0.222:5000}"

    FTP_URL="ftp://$FTP_HOST"

    PLUGIN=$(find "$PROJECT_DIR/plugin" -name "*.3gx" -type f 2>/dev/null | head -1)
    if [[ -z "$PLUGIN" ]]; then
      echo "ERROR: No plugin found. Run cosmic-3gx-build first."
      exit 1
    fi

    echo "Deploying plugin to 3DS at $FTP_HOST..."

    ${pkgs.curl}/bin/curl -s --ftp-create-dirs -T "$PLUGIN" "$FTP_URL/luma/plugins/default.3gx"

    echo "Deployed: luma/plugins/default.3gx"
  '';

  # Build script for NTR-HR
  buildNtrHr = pkgs.writeShellScriptBin "cosmic-ntr-build" ''
    set -euo pipefail

    PROJECT_DIR="''${1:-$(pwd)/firmware/3ds}"
    NTR_DIR="$PROJECT_DIR/ntr-hr"

    if [[ ! -d "$NTR_DIR" ]]; then
      echo "ERROR: NTR-HR not found. Clone it first:"
      echo "  git clone https://github.com/xzn/ntr-hr firmware/3ds/ntr-hr"
      exit 1
    fi

    echo "Building NTR-HR..."

    # NTR-HR needs its own Dockerfile with Rust
    ${pkgs.docker}/bin/docker build -t ntr-hr-builder "$NTR_DIR/dockerfiles"

    ${pkgs.docker}/bin/docker run --rm \
      -v "$NTR_DIR:/work" \
      -w /work \
      ntr-hr-builder \
      bash -c '
        set -e
        # Initialize libctru submodule if needed
        if [[ ! -f libctru/libctru/Makefile ]]; then
          git submodule update --init --recursive
        fi
        ./make.sh
      '

    echo "Built: $NTR_DIR/release/*.bin"
  '';

  # Deploy NTR-HR binaries to 3DS
  # NTR-HR needs BootNTR selector to load - deploy bins to SD card
  deployNtrHr = pkgs.writeShellScriptBin "cosmic-ntr-deploy" ''
    set -euo pipefail

    PROJECT_DIR="''${1:-$(pwd)/firmware/3ds}"
    NTR_DIR="$PROJECT_DIR/ntr-hr"
    FTP_HOST="''${2:-10.0.0.222:5000}"
    FTP_URL="ftp://$FTP_HOST"

    if [[ ! -d "$NTR_DIR/release" ]]; then
      echo "ERROR: NTR-HR not built. Run cosmic-ntr-build first."
      exit 1
    fi

    echo "Deploying NTR-HR to 3DS at $FTP_HOST..."

    # Deploy all .bin files to /ntr-hr/ on SD card
    for bin in "$NTR_DIR/release"/*.bin; do
      if [[ -f "$bin" ]]; then
        ${pkgs.curl}/bin/curl -s --ftp-create-dirs -T "$bin" "$FTP_URL/ntr-hr/$(basename "$bin")"
        echo "Deployed: ntr-hr/$(basename "$bin")"
      fi
    done

    echo "Done. Use BootNTR Selector to load NTR-HR."
  '';

  # Mock server for testing
  pythonEnv = unstable.python313.withPackages (ps: with ps; [ pygame ]);
  mockServer = pkgs.writeShellScriptBin "cosmic-mock" ''
    exec ${pythonEnv}/bin/python3 "$(dirname "$0")/../../tools/cosmic_mock.py" "$@"
  '';

in
{
  buildInputs = [
    buildSysmodule
    buildPlugin
    deployScript
    buildNtrHr
    deployNtrHr
    mockServer
    pkgs.docker
    pkgs.curl
    threegxtool
  ];

  shellHook = ''
    echo "3DS Build Commands:"
    echo "  cosmic-3ds-build            - Build sysmodule"
    echo "  cosmic-3gx-build            - Build 3GX plugin"
    echo "  cosmic-3ds-deploy [host]    - Deploy via FTP (default: 10.0.0.222:5000)"
    echo "  cosmic-ntr-build            - Build NTR-HR"
    echo "  cosmic-ntr-deploy [host]    - Deploy NTR-HR via FTP"
    echo "  cosmic-mock                 - Run mock LED panel"
  '';

  # Export scripts for flake apps
  inherit buildSysmodule buildPlugin deployScript buildNtrHr deployNtrHr;
}
