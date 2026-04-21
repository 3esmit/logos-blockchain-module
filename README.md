# Logos Blockchain Module

A Logos core module that wraps the [logos-blockchain](https://github.com/logos-blockchain/logos-blockchain) C bindings and ships the zk circuit binaries needed at runtime.

Built with [logos-module-builder](https://github.com/logos-co/logos-module-builder): `flake.nix`, `CMakeLists.txt`, and `metadata.json` are the only build-system files. The module-builder's `mkLogosModule` pulls the prebuilt `logos-blockchain-c` derivation (lib + header) in via `externalLibInputs`; the circuits directory is staged separately in `preConfigure` / `postInstall` because it's runtime data rather than a library.

### Build and inspect

```bash
# In the workspace (preferred):
ws build logos-blockchain-module
ws build logos-blockchain-module --auto-local   # pick up local dep overrides

# Inspect the built plugin
lm ./result/lib/liblogos_blockchain_module_plugin.so

# Standalone
nix build                              # -> result/lib/<plugin>.so + result/lib/circuits/
nix develop                            # cmake/ninja iteration
```

### Files

- `flake.nix` — `mkLogosModule` call + circuits staging
- `CMakeLists.txt` — single `logos_module()` macro + C++20 bump
- `metadata.json` — module identity, deps, and `nix.external_libraries`
- `src/` — plugin sources (Q_OBJECT + Q_INVOKABLE API)

### Troubleshooting

#### Nix + IDE Integration
If your IDE reports that a file doesn't belong to the project or that files cannot be found, the CMake cache is likely missing the Nix-provided paths. This happens when the IDE runs CMake on its own, outside the Nix environment, leaving `LOGOS_CPP_SDK_ROOT` / `LOGOS_MODULE_BUILDER_ROOT` unset.

To fix it:

1. **Regenerate the cache from within the Nix shell** — this provides the required Nix paths and writes them into `build/CMakeCache.txt`:
   ```bash
   nix develop -c cmake -B build -GNinja
   ```

2. **Reload the CMake project without resetting the cache.** On RustRover: open the CMake tool window (**View → Tool Windows → CMake**) and click **Reload** (↺). Resetting the cache would wipe the paths you just wrote.
