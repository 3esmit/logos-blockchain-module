{
  description = "Logos Blockchain Module - Qt6 Plugin";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder?ref=38ddf92c1f240f4e420d300a1fbabb1609d5db01";
    logos-blockchain.url = "github:3esmit/logos-blockchain?rev=3c400456fce46efc04fd5768facb6e13af1803f3";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;

        externalLibInputs = {
          logos_blockchain = inputs.logos-blockchain;
        };

        tests = {
          dir = ./tests;
          mockCLibs = [ "logos_blockchain" ];
        };

        postInstall = ''
          # Remove nix references to make the module portable.
          find "$out" -type f | while read -r binary; do
            if file "$binary" | grep -E -q "Mach-O|shared library|executable|archive"; then
              echo "Scrubbing references inside verified target: $binary"
              chmod +w "$binary" 2>/dev/null || true

              perl -pi -e 's|/nix/store/[a-z0-9]{32}-boost|/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-boost|g' "$binary" 2>/dev/null || true
              perl -pi -e 's|/nix/store/[a-z0-9]{32}-nlohmann_json|/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-nlohmann_json|g' "$binary" 2>/dev/null || true
              perl -pi -e 's|/nix/store/[a-z0-9]{32}-vendor-cargo-deps|/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-vendor-cargo-deps|g' "$binary" 2>/dev/null || true
              perl -pi -e 's|/nix/var/nix/b/[a-z0-9]{26}/|/tmp/eeeeeeeeeeeeeeeeeeeeeeeeee/|g' "$binary" 2>/dev/null || true
            fi
          done
        '';
      };
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      packageProfileChecks = logos-module-builder.lib.common.forAllSystems nixpkgs ({ system, pkgs }:
        let
          variant = if pkgs.stdenv.isDarwin
            then if pkgs.stdenv.isAarch64 then "darwin-arm64" else "darwin-amd64"
            else if pkgs.stdenv.isAarch64 then "linux-arm64" else "linux-amd64";
          libExt = if pkgs.stdenv.isDarwin then "dylib" else "so";
          portable = module.packages.${system}.lgx-portable;
        in {
          testnet-package-profile = pkgs.runCommand
            "blockchain-module-testnet-package-profile" {
              nativeBuildInputs = [ pkgs.gnutar pkgs.binutils pkgs.gnugrep pkgs.findutils ];
            } ''
              set -eu
              archive="$(${pkgs.findutils}/bin/find ${portable} -maxdepth 1 -type f -name '*.lgx' -print -quit)"
              test -n "$archive"
              count="$(${pkgs.findutils}/bin/find ${portable} -maxdepth 1 -type f -name '*.lgx' -print | ${pkgs.coreutils}/bin/wc -l)"
              test "$count" -eq 1
              payload="$TMPDIR/payload"
              ${pkgs.coreutils}/bin/mkdir -p "$payload"
              ${pkgs.gnutar}/bin/tar -xzf "$archive" -C "$payload"
              core="$payload/variants/${variant}/liblogos_blockchain.${libExt}"
              test -f "$core"
              ${pkgs.binutils}/bin/strings "$core" > "$TMPDIR/protocols"
              ${pkgs.gnugrep}/bin/grep -Fq -- 'chain_sync_protocol_name: /logos-blockchain-testnet-0.2.0/chainsync/1.0.0' "$TMPDIR/protocols"
              if ${pkgs.gnugrep}/bin/grep -Fq -- 'chain_sync_protocol_name: /logos-blockchain/chainsync/X.Y.Z' "$TMPDIR/protocols"; then
                exit 1
              fi
              ${pkgs.coreutils}/bin/mkdir -p "$out"
              printf '%s\n' 'verified Testnet chainsync profile' > "$out/result"
            '';
        });
    in module // {
      checks = builtins.mapAttrs
        (system: existing: existing // packageProfileChecks.${system})
        module.checks;
    };
}
