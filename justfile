default: build

configure:
    cmake -S . -B build -G Ninja \
      ${LOGOS_CORE_ROOT:+-DLOGOS_CORE_ROOT="$LOGOS_CORE_ROOT"} \
      ${LOGOS_BLOCKCHAIN_ROOT:+-DLOGOS_BLOCKCHAIN_ROOT="$LOGOS_BLOCKCHAIN_ROOT"} \
      -DCOPY_PLUGIN_TO_SOURCE_DIR=ON

build:
    cmake --build build --parallel --target liblogos-blockchain-module

update: clean-full
    cmake --build build --parallel --target logos_blockchain_libs
    just build

clean:
    rm -rf build
    rm -rf target
    rm -f liblogos-blockchain-module.log
    rm -rf logos_blockchain_db/

clean-full: clean

rebuild: clean configure build

run:
    RUST_BACKTRACE=full RUST_LOG=trace ../logos-module-viewer/result/bin/logos-module-viewer --module target/liblogos-blockchain-module.so > liblogos-blockchain-module.log 2>&1

nix:
    nix develop .#

# View the log with unicode characters rendered
unicode-logs file:
    perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge' {{ file }} | less -R

logs:
    just unicode-logs liblogos-blockchain-module.log

logs-tail:
    tail -f liblogos-blockchain-module.log | perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge'
