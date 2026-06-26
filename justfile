default: build

# Inside `nix develop` / `ws develop logos-blockchain-module` the module-builder
# provides LOGOS_CPP_SDK_ROOT and LOGOS_MODULE_BUILDER_ROOT — CMake picks them
# up automatically via the logos_module() macro, no explicit -D flags needed.
configure:
    cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
    cmake --build build --parallel --target blockchain_module_module_plugin

clean:
    rm -rf build result

rebuild: clean build

nix:
    nix develop

# Launch CLion inside the Nix dev shell so it inherits required variables and fully integrates with Nix.
clion:
    nix develop -c "$HOME/.local/share/JetBrains/Toolbox/apps/clion/bin/clion.sh" . >/dev/null 2>&1 &

prettify:
    nix shell nixpkgs#clang-tools -c clang-format -i src/**.cpp src/**.h

lint: configure
    nix shell nixpkgs#clang-tools --command clang-tidy \
        -p build \
        --header-filter='src/.*' \
        --extra-arg-before=--driver-mode=g++ \
        --extra-arg=-I"$LOGOS_EXT_ROOT_LOGOS_BLOCKCHAIN/include" \
        src/logos_blockchain_module.cpp

unicode-logs file:
    perl -pe 's/\\u([0-9A-Fa-f]{4})/chr(hex($1))/ge' {{file}} | less -R
