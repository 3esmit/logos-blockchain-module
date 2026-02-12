# Common build configuration shared across all packages
{ pkgs, logosSdk }:

{
  pname = "logos-blockchain-module";
  version = "1.0.0";
  
  # Common native build inputs
  nativeBuildInputs = [ 
    pkgs.cmake 
    pkgs.ninja 
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
  ];
  
  # Common runtime dependencies
  buildInputs = [ 
    pkgs.qt6.qtbase 
    pkgs.qt6.qtremoteobjects 
  ];
  
  # Common CMake flags
  cmakeFlags = [ 
    "-GNinja"
    "-DLOGOS_CORE_ROOT=${logosSdk}"
  ];

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos Blockchain Module - Qt6 Plugin";
    platforms = platforms.unix ++ platforms.windows;
  };
}
