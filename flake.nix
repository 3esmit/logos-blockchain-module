{
  description = "Logos Blockchain Module - Qt6 Plugin";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # v0.1.3-rc.10-compatible + rust-rapidsnark nix fixes + cli commands
    logos-blockchain.url = "github:logos-blockchain/logos-blockchain?ref=86e24cfd985b6341e96bba76ecad36ff379bd243";
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
        fi
      '';
    };
}
