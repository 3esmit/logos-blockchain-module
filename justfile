default: build

configure:
    cmake -S . -B build -G Ninja \
      ${LOGOS_CORE_ROOT:+-DLOGOS_CORE_ROOT="$LOGOS_CORE_ROOT"} \
      ${LOGOS_BLOCKCHAIN_LIB:+-DLOGOS_BLOCKCHAIN_LIB="$LOGOS_BLOCKCHAIN_LIB"} \
      ${LOGOS_BLOCKCHAIN_INCLUDE:+-DLOGOS_BLOCKCHAIN_INCLUDE="$LOGOS_BLOCKCHAIN_INCLUDE"}

build: configure
    cmake --build build --parallel --target logos-blockchain-module

clean:
    rm -rf build result

rebuild: clean build

nix:
    nix develop

prettify:
    nix shell nixpkgs#clang-tools -c clang-format -i src/**.cpp src/**.h

unicode-logs file:
    perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge' {{file}} | less -R
