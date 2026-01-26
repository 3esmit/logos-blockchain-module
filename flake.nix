{
  description = "Logos Blockchain Module - Qt6 Plugin (Nix)";

  inputs = {
    nixpkgs.follows = "logos-liblogos/nixpkgs";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";

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
          qt = pkgs.qt6;
          llvmPkgs = pkgs.llvmPackages;
          logosSdk = inputs.logos-cpp-sdk.packages.${system}.default;
          logosBlockchainSrc = inputs.logos-blockchain;
        in
        f { inherit pkgs qt llvmPkgs logosSdk logosBlockchainSrc; });
    in
    {
      packages = forAllSystems ({ pkgs, qt, llvmPkgs, logosSdk, logosBlockchainSrc }:
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "logos-blockchain-module";
            version = "dev";
            src = ./.;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              qt.wrapQtAppsHook
              pkgs.patchelf
            ];

            buildInputs = [
              qt.qtbase
              qt.qttools

              pkgs.rustc
              pkgs.cargo
              pkgs.git

              llvmPkgs.clang
              llvmPkgs.llvm
              llvmPkgs.libclang
            ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [
              pkgs.libiconv
            ];

            LOGOS_CPP_SDK_ROOT = "${logosSdk}";
            LOGOS_BLOCKCHAIN_ROOT = "${logosBlockchainSrc}";

            LIBCLANG_PATH = "${llvmPkgs.libclang.lib}/lib";
            CLANG_PATH = "${llvmPkgs.clang}/bin/clang";

            CARGO_HOME = "${"$"}TMPDIR/cargo-home";

            cmakeFlags = [
              "-DUNTITLED_USE_QT=ON"
              "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
              "-DLOGOS_BLOCKCHAIN_ROOT=${logosBlockchainSrc}"
              "-DCOPY_PLUGIN_TO_SOURCE_DIR=OFF"
            ];

            preConfigure = ''
              mkdir -p $CARGO_HOME
            '';
          };
        }
      );

      devShells = forAllSystems ({ pkgs, qt, llvmPkgs, logosSdk, logosBlockchainSrc }:
        {
          default = pkgs.mkShell {
            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.patchelf
            ];

            buildInputs = [
              qt.qtbase
              qt.qttools

              pkgs.rustc
              pkgs.cargo
              pkgs.git

              llvmPkgs.clang
              llvmPkgs.llvm
              llvmPkgs.libclang
            ];

            LOGOS_CPP_SDK_ROOT = "${logosSdk}";
            LOGOS_BLOCKCHAIN_ROOT = "${logosBlockchainSrc}";

            LIBCLANG_PATH = "${llvmPkgs.libclang.lib}/lib";
            CLANG_PATH = "${llvmPkgs.clang}/bin/clang";

            shellHook = ''
              echo "Logos Blockchain Module dev environment"
              echo "LOGOS_CPP_SDK_ROOT:    $LOGOS_CPP_SDK_ROOT"
              echo "LOGOS_BLOCKCHAIN_ROOT: $LOGOS_BLOCKCHAIN_ROOT"
              echo "LIBCLANG_PATH:         $LIBCLANG_PATH"
              echo "CLANG_PATH:            $CLANG_PATH"
              echo ""
              echo "Build with:"
              echo "  just clean"
              echo "  just build"
            '';
          };
        }
      );
    };
}
