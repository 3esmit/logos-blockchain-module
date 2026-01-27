{
  description = "Logos Blockchain Module - Qt6 Plugin (Nix)";

  inputs = {
    nixpkgs.follows = "logos-liblogos/nixpkgs";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-core.url = "github:logos-co/logos-cpp-sdk";
    logos-blockchain = {
      url = "github:logos-blockchain/logos-blockchain";
      flake = false;
    };
    logos-module-viewer.url = "github:logos-co/logos-module-viewer";
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvmPkgs = pkgs.llvmPackages;
          logosCore = inputs.logos-core.packages.${system}.default;
          logosBlockchain = inputs.logos-blockchain;

          # 1. Environment variables shared by build and shell
          env = {
            LOGOS_CORE_ROOT = "${logosCore}";
            LOGOS_BLOCKCHAIN_ROOT = "${logosBlockchain}";
            LIBCLANG_PATH = "${llvmPkgs.libclang.lib}/lib";
            CLANG_PATH = "${llvmPkgs.clang}/bin/clang";
          };

          # 2. Tools and libraries shared by build and shell
          baseArgs = env // {
            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.patchelf
            ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qttools
              pkgs.rustc
              pkgs.cargo
              pkgs.git
              llvmPkgs.clang
              llvmPkgs.llvm
              llvmPkgs.libclang
            ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [
              pkgs.libiconv
            ];
          };

          viewer = inputs.logos-module-viewer.packages.${system}.default;
        in
        f { inherit pkgs system baseArgs viewer; });
    in
    {
      packages = forAllSystems ({ pkgs, baseArgs, ... }:
        {
          default = pkgs.stdenv.mkDerivation (baseArgs // {
            pname = "logos-blockchain-module";
            version = "dev";
            src = ./.;

            nativeBuildInputs = baseArgs.nativeBuildInputs ++ [ pkgs.qt6.wrapQtAppsHook ];

            CARGO_HOME = "${"$"}TMPDIR/cargo-home";
            preConfigure = "mkdir -p $CARGO_HOME";
          });
        }
      );

      apps = forAllSystems ({ pkgs, system, viewer, ... }:
        let
          pkg = self.packages.${system}.default;
        in
        {
          default = {
            type = "app";
            program = "${pkgs.writeShellScriptBin "inspect-module" ''
              ${viewer}/bin/logos-module-viewer --module ${pkg}/lib/liblogos-blockchain-module.so
            ''}/bin/inspect-module";
          };
        }
      );

      devShells = forAllSystems ({ pkgs, system, ... }:
        let
          pkg = self.packages.${system}.default;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ pkg ];

            # Inherit environment variables from the package
            inherit (pkg) LOGOS_CORE_ROOT LOGOS_BLOCKCHAIN_ROOT LIBCLANG_PATH CLANG_PATH;

            shellHook = ''
              BLUE='\e[1;34m'
              GREEN='\e[1;32m'
              YELLOW='\e[1;33m'
              RESET='\e[0m'

              echo -e "\n''${BLUE}=== Logos Blockchain Module Development Environment ===''${RESET}"
              echo -e "''${GREEN}Core SDK:''${RESET}       $LOGOS_CORE_ROOT"
              echo -e "''${GREEN}Blockchain:''${RESET}     $LOGOS_BLOCKCHAIN_ROOT"
              echo -e "''${BLUE}---------------------------------------------------------''${RESET}"
            '';
          };
        }
      );
    };
}
