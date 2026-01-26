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

          env = {
            LOGOS_CORE_ROOT = "${logosCore}";
            LOGOS_BLOCKCHAIN_ROOT = "${logosBlockchain}";
            LIBCLANG_PATH = "${llvmPkgs.libclang.lib}/lib";
            CLANG_PATH = "${llvmPkgs.clang}/bin/clang";
          };

          args = {
            inherit (env) LOGOS_CORE_ROOT LOGOS_BLOCKCHAIN_ROOT LIBCLANG_PATH CLANG_PATH;

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
        in
        f { inherit pkgs args; });
    in
    {
      packages = forAllSystems ({ pkgs, args }:
        {
          default = pkgs.stdenv.mkDerivation (args // {
            pname = "logos-blockchain-module";
            version = "dev";
            src = ./.;

            nativeBuildInputs = args.nativeBuildInputs ++ [ pkgs.qt6.wrapQtAppsHook ];

            CARGO_HOME = "${"$"}TMPDIR/cargo-home";

            preConfigure = ''
              mkdir -p $CARGO_HOME
            '';
          });
        }
      );

      devShells = forAllSystems ({ pkgs, args }:
        {
          default = pkgs.mkShell (args // {
            shellHook = ''
              echo "Logos Blockchain Module dev environment"
              echo "LOGOS_CORE_ROOT:       $LOGOS_CORE_ROOT"
              echo "LOGOS_BLOCKCHAIN_ROOT: $LOGOS_BLOCKCHAIN_ROOT"
              echo ""
              echo "Build with:"
              echo "  just build"
            '';
          });
        }
      );
    };
}
