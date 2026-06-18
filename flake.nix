{
  description = "Logos Blockchain Module - Qt6 Plugin";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder?ref=bc868bf47f9d797637ac78af814b0db718dde4b8";
    # v0.1.3-rc.10-compatible + rust-rapidsnark nix fixes + cli commands + leader_claim C binding + config generation fixes
    logos-blockchain.url = "github:logos-blockchain/logos-blockchain?ref=3dc81b4ec49ba977220f3ff432885258265aa986";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
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

      preConfigure = { externalLibs }:
        if externalLibs ? logos_blockchain then ''
          if [ -d "${externalLibs.logos_blockchain}/circuits" ]; then
            echo "Staging zk circuits from logos-blockchain..."
            cp -r "${externalLibs.logos_blockchain}/circuits" ./circuits
            chmod -R u+w ./circuits
          else
            echo "WARNING: no circuits/ found in logos-blockchain derivation"
          fi
        '' else ''
          echo "Skipping zk circuits staging (logos_blockchain mocked for tests)"
        '';

      # Logos Core Edge-case
      # The current version of Logos Core expects circuits' binaries under `lib/circuits/`.
      # Until we address this in Logos Core, we use this hook to include to ensure the circuits' binaries
      # are included in the binary bundle and avoid the circuits being mangled by Nix (which did that when
      # copying them in a previous phase).
      postInstall = ''
        if [ -d "$LOGOS_MODULE_SOURCE_DIR/circuits" ]; then
          cp -r "$LOGOS_MODULE_SOURCE_DIR/circuits" "$out/lib/circuits"
          chmod -R u+w "$out/lib/circuits"

          if [ -f "$out/lib/circuits/lib/libgmp.a" ]; then
            echo "Removing loose static library libgmp.a from staged circuits..."
            rm "$out/lib/circuits/lib/libgmp.a"
          fi
        fi

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
}
