# Builds the logos-blockchain-module library
{ pkgs, common, src, logosBlockchainC }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-lib";
  version = common.version;
  
  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta;

  preConfigure = ''
    runHook prePreConfigure
    # Stage logos-blockchain C lib into build tree
    export BLOCKCHAIN_LIB_STAGE="''${NIX_BUILD_TOP}/blockchain-lib"
    mkdir -p "$BLOCKCHAIN_LIB_STAGE/lib" "$BLOCKCHAIN_LIB_STAGE/include"
    cp -r "${logosBlockchainC}/lib"/* "$BLOCKCHAIN_LIB_STAGE/lib/"
    cp -r "${logosBlockchainC}/include"/* "$BLOCKCHAIN_LIB_STAGE/include/"
    runHook postPreConfigure
  '';

  configurePhase = ''
    runHook preConfigure
    mkdir -p build
    cd build
    cmake .. ''${cmakeFlags} \
      -DCMAKE_INSTALL_PREFIX=$out \
      -DLOGOS_BLOCKCHAIN_LIB="''${NIX_BUILD_TOP}/blockchain-lib/lib" \
      -DLOGOS_BLOCKCHAIN_INCLUDE="''${NIX_BUILD_TOP}/blockchain-lib/include"
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    # We are already in build/ from configurePhase
    ninja
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    # We are in build/ from previous phases; plugin is in current dir
    ninja install
    runHook postInstall
  '';
}
