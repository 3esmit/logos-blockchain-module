# Logos Blockchain Module

A Logos core module that wraps the [logos-blockchain](https://github.com/logos-blockchain/logos-blockchain) C bindings.

### Build for local inspection

```bash
nix build '.#lgx'
```

This development bundle targets `linux-amd64-dev`.

### Build for deployment

```bash
nix build '.#lgx-portable'
```

Use the portable bundle when installing through the Logos package manager. It
targets `linux-amd64`, which is the normal Linux deployment variant. Apply any
environment-specific package-signing policy before installation.
