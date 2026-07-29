{
  description = "Logos Blockchain Module - Qt6 Plugin";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.4";
    logos-blockchain.url = "github:logos-blockchain/logos-blockchain?ref=133570d58a141629a0c751fdf4412a66898acac2";
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
}
