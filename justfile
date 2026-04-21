default: build

# Inside `nix develop` / `ws develop logos-blockchain-module` the module-builder
# provides LOGOS_CPP_SDK_ROOT and LOGOS_MODULE_BUILDER_ROOT — CMake picks them
# up automatically via the logos_module() macro, no explicit -D flags needed.
configure:
    cmake -S . -B build -G Ninja

build: configure
    cmake --build build --parallel --target liblogos_blockchain_module_module_plugin

clean:
    rm -rf build result

rebuild: clean build

nix:
    nix develop

prettify:
    nix shell nixpkgs#clang-tools -c clang-format -i src/**.cpp src/**.h

unicode-logs file:
    perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge' {{file}} | less -R
