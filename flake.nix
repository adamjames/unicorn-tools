{
  description = "Cosmic Unicorn LED streaming - firmware and tools";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    nixpkgs-unstable.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, nixpkgs-unstable, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        unstable = import nixpkgs-unstable {
          inherit system;
          config.allowUnfree = true;
        };

        # Import module configurations
        picoEnv = import ./nix/pico.nix { inherit pkgs; };
        toolsEnv = import ./nix/tools.nix { inherit pkgs unstable; };
        threedsEnv = import ./nix/3ds.nix { inherit pkgs unstable; };

      in {

        # Development shells
        devShells = {
          # Full environment: Pico + streaming tools
          default = pkgs.mkShell {
            buildInputs = picoEnv.buildInputs ++ toolsEnv.buildInputs;
            shellHook = ''
              ${picoEnv.shellHook}
              ${toolsEnv.shellHook}
              echo ""
              echo "Cosmic Unicorn Development Environment"
              echo "  firmware/cosmic/deploy.sh [full|lite]  - Build and deploy Pico firmware"
              echo "  tools/cosmic_cast.py                   - Stream screen to LED panel"
              echo "  tools/cosmic_mock.py                   - Mock LED panel for testing"
            '';
          };

          # Pico-only (lighter weight)
          pico = pkgs.mkShell {
            buildInputs = picoEnv.buildInputs;
            shellHook = picoEnv.shellHook;
          };

          # 3DS build environment
          "3ds" = pkgs.mkShell {
            buildInputs = threedsEnv.buildInputs ++ toolsEnv.buildInputs;
            shellHook = ''
              ${toolsEnv.shellHook}
              ${threedsEnv.shellHook}
            '';
          };

          # Streaming tools only (no build tools)
          tools = pkgs.mkShell {
            buildInputs = toolsEnv.buildInputs;
            shellHook = toolsEnv.shellHook;
          };
        };

        # Apps for direct execution
        apps = {
          # 3DS build commands
          "3ds-build" = {
            type = "app";
            program = "${threedsEnv.buildSysmodule}/bin/cosmic-3ds-build";
          };
          "3gx-build" = {
            type = "app";
            program = "${threedsEnv.buildPlugin}/bin/cosmic-3gx-build";
          };
          "3ds-deploy" = {
            type = "app";
            program = "${threedsEnv.deployScript}/bin/cosmic-3ds-deploy";
          };
        };

        # NixOS module for udev rules
        nixosModules.default = {
          services.udev.extraRules = picoEnv.udevRules;
          environment.sessionVariables.NIXOS_OZONE_WL = "1";
        };
      }
    );
}
