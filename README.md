# Logos Blockchain Module

### Setup

#### IDE

If you're using an IDE with CMake integration make sure it points to the same cmake directory as the `justfile`, which defaults to `build`.

This will reduce friction when working on the project.

#### Nix

* Use `nix flake update` to bring all nix context and packages
* Use `nix build` to build the package
* Use `nix run` to launch the module-viewer and check your module loads properly
* Use `nix develop` to setup your IDE

### Troubleshooting

#### Nix + IDE Integration
If your IDE reports that a file doesn't belong to the project or that files cannot be found, the CMake cache
is likely missing the Nix-provided paths. This happens when the IDE runs CMake on its own, outside the Nix
environment, leaving the required paths empty.

To fix it:

1. **Regenerate the cache from within the Nix shell**

   This provides the required Nix paths and writes them into `build/CMakeCache.txt`:
   ```bash
   nix develop -c just configure
   ```

2. **Reload the CMake project without resetting the cache**

   If on RustRover: Open the CMake tool window (**View → Tool Windows → CMake**) and click the **Reload** button (↺) in the toolbar.

> Resetting the cache would wipe the paths you just wrote, so make sure to reload only.
