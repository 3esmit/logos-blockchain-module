default: build

configure:
    test -n "${LOGOS_CPP_SDK_ROOT}" || (echo "LOGOS_CPP_SDK_ROOT not set" && exit 1)
    test -n "${LOGOS_BLOCKCHAIN_ROOT}" || (echo "LOGOS_BLOCKCHAIN_ROOT not set" && exit 1)
    cmake -S . -B build -G Ninja \
      -DLOGOS_CPP_SDK_ROOT="${LOGOS_CPP_SDK_ROOT}" \
      -DLOGOS_BLOCKCHAIN_ROOT="${LOGOS_BLOCKCHAIN_ROOT}" \
      -DCOPY_PLUGIN_TO_SOURCE_DIR=ON

build:
    cmake --build build --parallel --target liblogos-blockchain-module

update: clean-full
    cmake --build build --parallel --target logos_blockchain_stage
    cmake --build build --parallel --target logos_cargo_build
    just build

clean:
    rm -f build/liblogos-blockchain-module.so
    rm -f liblogos-blockchain-module.so
    rm -f liblogos-blockchain-module.log
    rm -rf logos_blockchain_db/

clean-full: clean
    rm -rf build

rebuild: clean configure build

run:
    RUST_BACKTRACE=full RUST_LOG=trace ../logos-module-viewer/result/bin/logos-module-viewer --module liblogos-blockchain-module.so > liblogos-blockchain-module.log 2>&1

nix:
    nix develop .#

# View the log with unicode characters rendered
unicode-logs file:
    perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge' {{ file }} | less -R

logs:
    just unicode-logs liblogos-blockchain-module.log

logs-tail:
    tail -f liblogos-blockchain-module.log | perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge'
