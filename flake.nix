{
  description = "Logos Blockchain Module - Qt6 Plugin";

  inputs = {
    nixpkgs.follows = "logos-liblogos/nixpkgs";

    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-core.url = "github:logos-co/logos-cpp-sdk";

    #logos-blockchain.url = "github:logos-blockchain/logos-blockchain?rev=d7ff5984fa3371a89e944b163b358465a5e307ad"; # pre-0.1.3 + genesis fixes
    logos-blockchain.url = "github:logos-blockchain/logos-blockchain?ref=feat/nix/disable-testing"; # pre-0.1.3 + genesis fixes

    logos-module-viewer.url = "github:logos-co/logos-module-viewer";
  };

  outputs =
    {
      self,
      nixpkgs,
      logos-core,
      logos-blockchain,
      logos-module-viewer,
      ...
    }:
    let
      lib = nixpkgs.lib;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];

      forAll = lib.genAttrs systems;

      mkPkgs = system: import nixpkgs { inherit system; };
    in
    {
      packages = forAll (
        system:
        let
          pkgs = mkPkgs system;
          llvmPkgs = pkgs.llvmPackages;

          logosCore = logos-core.packages.${system}.default;
          logosBlockchainC = logos-blockchain.packages.${system}.logos-blockchain-c;

          logosBlockchainModule = pkgs.stdenv.mkDerivation {
            pname = "logos-blockchain-module";
            version = "dev";
            src = ./.;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.qt6.wrapQtAppsHook
            ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              pkgs.qt6.qttools
              llvmPkgs.clang
              llvmPkgs.libclang
              logosBlockchainC
            ]
            ++ lib.optionals pkgs.stdenv.isDarwin [
              pkgs.libiconv
              pkgs.cacert
            ];

            LIBCLANG_PATH = "${llvmPkgs.libclang.lib}/lib";
            CLANG_PATH = "${llvmPkgs.clang}/bin/clang";
            SSL_CERT_FILE = lib.optionalString pkgs.stdenv.isDarwin "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

            cmakeFlags = [
              "-DLOGOS_CORE_ROOT=${logosCore}"
              "-DLOGOS_BLOCKCHAIN_LIB=${logosBlockchainC}/lib"
              "-DLOGOS_BLOCKCHAIN_INCLUDE=${logosBlockchainC}/include"
            ];

            postInstall = ''
              mkdir $out/share
              cp -r ${logosBlockchainC}/circuits $out/share
            '';

            # Logos Core Edge-case
            # The current version of Logos Core expects circuits' binaries under `lib/circuits/`.
            # Until we address this in Logos Core, we use this hook to include to ensure the circuits' binaries
            # are included in the binary bundle and avoid the circuits being mangled by Nix (which did that when
            # copying them in a previous phase).
            postFixup = ''
              cp -r ${logosBlockchainC}/circuits $out/lib/circuits
            '';
        };
        in
        {
          lib = logosBlockchainModule;
          default = logosBlockchainModule;
        }
      );

      apps = forAll (
        system:
        let
          pkgs = mkPkgs system;
          logosBlockchainModuleLib = self.packages.${system}.lib;
          logosModuleViewer = logos-module-viewer.packages.${system}.default;
          extension = if pkgs.stdenv.isDarwin then "dylib"
            else if pkgs.stdenv.hostPlatform.isWindows then "dll"
            else "so";
          inspectModule = {
            type = "app";
            program =
              "${pkgs.writeShellScriptBin "inspect-module" ''
                exec ${logosModuleViewer}/bin/logos-module-viewer \
                  --module ${logosBlockchainModuleLib}/lib/liblogos_blockchain_module.${extension}
              ''}/bin/inspect-module";
          };
        in
        {
          inspect-module = inspectModule;
          default = inspectModule;
        }
      );

      devShells = forAll (
        system:
        let
          pkgs = mkPkgs system;
          pkg = self.packages.${system}.default;
          logosCore = logos-core.packages.${system}.default;
          logosBlockchainC = logos-blockchain.packages.${system}.logos-blockchain-c;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ pkg ];

            inherit (pkg)
              LIBCLANG_PATH
              CLANG_PATH;

            LOGOS_CORE_ROOT = "${logosCore}";
            LOGOS_BLOCKCHAIN_LIB = "${logosBlockchainC}/lib";
            LOGOS_BLOCKCHAIN_INCLUDE = "${logosBlockchainC}/include";

            shellHook = ''
              BLUE='\e[1;34m'
              GREEN='\e[1;32m'
              RESET='\e[0m'

              echo -e "\n''${BLUE}=== Logos Blockchain Module Development Environment ===''${RESET}"
              echo -e "''${GREEN}LOGOS_CORE_ROOT:''${RESET}       $LOGOS_CORE_ROOT"
              echo -e "''${GREEN}LOGOS_BLOCKCHAIN_LIB:''${RESET}  $LOGOS_BLOCKCHAIN_LIB"
              echo -e "''${GREEN}LOGOS_BLOCKCHAIN_INCLUDE:''${RESET} $LOGOS_BLOCKCHAIN_INCLUDE"
              echo -e "''${BLUE}---------------------------------------------------------''${RESET}"
            '';
          };
        }
      );
    };
}
