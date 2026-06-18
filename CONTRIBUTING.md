# Contributing

## Nix + IDE Integration

If your IDE reports that a file doesn't belong to the project or that files cannot be found, the CMake cache is likely
missing the Nix-provided paths.

This may happen when the IDE runs CMake on its own, outside the Nix environment, leaving the required paths empty.

#### Preferred fix: launch the IDE from inside the Nix dev shell

This makes the IDE inherit required environment variables directly, so every CMake reload it triggers has the right
environment, not just a one-off cache snapshot.

Example:
```bash
nix develop -c ...
```

Or, if you're using CLion, there's already a shortcut for that:
```bash
just clion
```

#### Alternative: regenerate the cache, then reload without resetting it

If you don't want to launch the IDE from inside the Nix dev shell, you can try to reuse an existing `CMakeCache.txt` on
reload instead of always re-running `cmake` with its own environment.

To do that, you first need to regenerate the cache. This can be done by running:
```bash
nix develop -c just configure
```

Then reload the CMake project without resetting the cache (resetting it would wipe the paths you just wrote):

> JetBrains: **View → Tool Windows → CMake** → **Reload** ↺

This won't help on an IDE that always re-invokes `cmake` with its own (non-Nix) environment on reload.
