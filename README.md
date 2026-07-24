# Logos Blockchain Module

A Logos core module that wraps the [logos-blockchain](https://github.com/logos-blockchain/logos-blockchain) C bindings.

### Build and inspect

```bash
nix build '.#lgx'
```

## Package releases

This repository owns Blockchain Module package releases. Run **Publish
Blockchain Module** from `master`; it reads `blockchain_module` and its version
from `metadata.json`, requires both `linux-amd64` and `darwin-arm64` portable
variants, and publishes a prerelease tagged `blockchain_module-v<version>`.

The shared release workflow merges both native bundles into one
`blockchain_module-<version>.lgx` asset and publishes its `sidecar.json`.
Completed releases are skipped by default; use the `force_build` input to
rebuild and replace assets for the same version. Releases are currently
unsigned.

External catalogs may index these source-owned assets, but this workflow does
not dispatch or publish a catalog index.
