{
  description = "Logos Blockchain Module - Qt6 Plugin";

  inputs = {
    nixpkgs.follows = "logos-liblogos/nixpkgs";

    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-core.url = "github:logos-co/logos-cpp-sdk";

    logos-blockchain.url = "github:logos-blockchain/logos-blockchain";

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
        "x86_64-windows"
      ];

      forAll = lib.genAttrs systems;

      mkPkgs = system: import nixpkgs { inherit system; };
    in
    {
      packages = forAll (
        system:
        let
          pkgs = mkPkgs system;
          src = ./.;

          logosCore = logos-core.packages.${system}.default;
          logosBlockchainC = logos-blockchain.packages.${system}.logos-blockchain-c;

          common = import ./nix/default.nix {
            inherit pkgs;
            logosSdk = logosCore;
          };

          libDerivation = import ./nix/lib.nix {
            inherit pkgs common src logosBlockchainC;
          };

          includeDerivation = import ./nix/include.nix {
            inherit pkgs common src;
            lib = libDerivation;
            logosSdk = logosCore;
          };

          combined = pkgs.symlinkJoin {
            name = "logos-blockchain-module";
            paths = [ libDerivation includeDerivation ];
          };
        in
        {
          lib = libDerivation;
          include = includeDerivation;
          default = combined;
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
              echo -e "''${GREEN}LOGOS_CORE_ROOT:''${RESET} $LOGOS_CORE_ROOT"
              echo -e "''${GREEN}LOGOS_BLOCKCHAIN_LIB:''${RESET}  $LOGOS_BLOCKCHAIN_LIB"
              echo -e "''${GREEN}LOGOS_BLOCKCHAIN_INCLUDE:''${RESET} $LOGOS_BLOCKCHAIN_INCLUDE"
              echo -e "''${BLUE}---------------------------------------------------------''${RESET}"
            '';
          };
        }
      );
    };
}
