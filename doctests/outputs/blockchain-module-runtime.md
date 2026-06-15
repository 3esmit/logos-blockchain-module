# Running This Blockchain Module Against logoscore

`logos-blockchain-module` is a Logos `core` module that wraps the
[logos-blockchain](https://github.com/logos-blockchain/logos-blockchain) C
bindings — a full Cryptarchia consensus node with wallet, blend, keystore and
block-explorer APIs — and ships the zk circuit binaries the node needs at
runtime. This doc-test exercises **this** blockchain-module commit end-to-end
through the headless `logoscore` runtime:

1. Build the `logoscore` CLI and the `lgpm` local package manager from their
   published flakes. `logoscore` is the headless frontend for `logos-liblogos`,
   so building it brings in the whole module-runtime stack (`logos_host`,
   `liblogos_core`, the IPC layer).
2. Build **this** blockchain module as an installable `.lgx` package straight
   from its own flake's `#lgx` output, **pinned to the commit under test** — so
   the module you run is built from exactly what is checked out here, not the
   latest published release.
3. Install the `.lgx` into a `./modules` directory with `lgpm`.
4. Start `logoscore` in daemon mode (`-D`), load `liblogos_blockchain_module`, introspect
   it with `module-info`, and call several of its methods — verifying the module
   actually runs and round-trips real values through the logos-blockchain C
   library.

Joining the devnet means dialing real peers, NTP sync and zk proving — none of
which is reproducible in CI — so this doc-test deliberately stays **offline**.
We exercise the methods that do real, deterministic work without a running node
(generating a node user-config and its keystore, deriving the peer id,
re-syncing the config from the keystore) and we confirm the node-backed methods
(wallet, explorer, consensus) are wired up and callable by asserting on the
well-defined `The node is not running` response they return before a node is
started. Starting an actual node is covered by the UI app and the developer
guide.

Because the module is built from the commit under test and then loaded and
called through a real `logoscore` daemon, a green run is real evidence that this
change keeps the blockchain module loadable and callable.

**What you'll build:** This `liblogos_blockchain_module`, packaged as `.lgx`, installed with `lgpm`, and called through a `logoscore` daemon.

**What you'll learn:**

- How to build the `logoscore` runtime and the `lgpm` package manager from their flakes
- How a module's flake exposes a ready-to-install `.lgx` via its `#lgx` output
- How to install an `.lgx` into a modules directory with `lgpm`
- How to start the `logoscore` daemon, load a module, introspect it, and call its methods
- How to generate a node user-config and keystore, and derive the peer id, offline
- How to re-sync a user config from its keystore with `update_user_config`
- How the node-backed methods report a clear error until a node is started
- How to shut the daemon down and confirm it has exited

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **A Linux or macOS machine.**

---

## Step 1: Build logoscore

Build the `logoscore` CLI from its published flake. The result is symlinked to
`./logos/`. `logoscore` is the headless frontend for `logos-liblogos`, so this
one build brings in the whole module-runtime stack the daemon needs.

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli' --out-link ./logos
```

The build produces `logos/bin/logoscore` plus bundled runtime libraries
and a `logos/modules/` directory containing the built-in
`capability_module` (required for the auth handshake when loading
modules).

---

## Step 2: Build the lgpm package manager

`lgpm` installs `.lgx` packages into a modules directory and scans what is
installed. Build it from the `logos-package-manager` flake and link it as
`./lgpm`.

### 2.1 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

The executable is at `./lgpm/bin/lgpm`.

---

## Step 3: Build and install this blockchain module

Build **this** blockchain module's `.lgx` straight from its flake's `#lgx`
output and install it into a local `./modules` directory with `lgpm`. Every
module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes a ready-to-install `#lgx`.

> The `` in the URL is what pins the build to a specific commit: the
> doc-test runner expands it to a concrete ref. Locally that is this
> checkout's `HEAD` (see `run.sh`); in CI it is the commit being tested. With
> no pin it falls back to the latest `master`.

### 3.1 Build the module's .lgx

Build the `#lgx` output and link it as `./blockchain-lgx`. (This compiles
the module, the logos-blockchain Rust node and its zk circuits through
Nix, so the first build is slow.)

```bash
# From inside the clone this is simply: nix build '.#lgx'
nix build 'github:logos-blockchain/logos-blockchain-module#lgx' -o blockchain-lgx
```

The `.lgx` package is now under `./blockchain-lgx/`:

```bash
ls blockchain-lgx/*.lgx
```

### 3.2 Seed the modules directory with the bundled capability module

`liblogos_blockchain_module` is loaded through the host's capability layer, so the
modules directory also needs the `capability_module` that ships with
`logoscore`. Copy it across first.

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 3.3 Install the .lgx with lgpm

Install the freshly-built package into `./modules`. `liblogos_blockchain_module` is
a `core` module, so it goes to `--modules-dir`. The package is unsigned
(a local dev build), so we pass `--allow-unsigned`.

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file blockchain-lgx/*.lgx
```

### 3.4 Confirm the install

Scan the directory and confirm the module landed:

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 4: Run the daemon and call the module

Start `logoscore` in daemon mode pointed at `./modules`, then use the client
subcommands to load `liblogos_blockchain_module`, introspect it, and call several of
its methods. Daemon output is captured in `logs.txt`.

### 4.1 Start the daemon

Start logoscore in daemon mode in the background, capturing output to
`logs.txt`:

```bash
logoscore -D -m ./modules > logs.txt &
```

The `-D` flag starts the daemon. The client subcommands below connect to
this running process via the config written under `~/.logoscore/`.

```bash
sleep 3
```

### 4.2 Inspect the startup log

Review the daemon's startup output:

```bash
cat logs.txt
```

### 4.3 Check daemon status

Verify the daemon is running:

```bash
logoscore status
```

### 4.4 List discovered modules

`liblogos_blockchain_module` should be visible in the scan directory:

```bash
logoscore list-modules
```

### 4.5 Load the module

Load `liblogos_blockchain_module` into the running daemon:

```bash
logoscore load-module liblogos_blockchain_module
```

### 4.6 Confirm the module is loaded

Re-run `status`; the module that was `not_loaded` before now reports
`loaded`:

```bash
logoscore status
```

### 4.7 Introspect the module with module-info

`module-info` lists the `Q_INVOKABLE` methods the module exposes — the
same methods you can `call`:

```bash
logoscore module-info liblogos_blockchain_module
```

### 4.8 Generate a node user-config

`generate_user_config` takes a JSON argument describing the node and
writes a ready-to-run config file to the `output` path, plus a sibling
`keystore.yaml` holding freshly-generated default keys — no node or
network required. We write the config to `./user-config.yaml`; a `0`
result means success. The JSON is passed with logoscore's `@file` syntax
after writing it to disk:

```json
{
  "output": "./user-config.yaml"
}
```

```bash
logoscore call liblogos_blockchain_module generate_user_config @gen-config.json
```

### 4.9 Confirm the keystore was written alongside the config

`generate_user_config` also writes a `keystore.yaml` next to the config,
holding the node's freshly-generated default keys. Both files are written
relative to the daemon's working directory:

```bash
ls -1 user-config.yaml keystore.yaml
```

### 4.10 Derive the node's peer id

`get_peer_id` reads the network key out of the config we just generated
and derives the libp2p peer id from it — a deterministic, offline
round-trip through the logos-blockchain C library. The result is the
node's base58 peer id (the `12D3Koo…` form):

```bash
logoscore call liblogos_blockchain_module get_peer_id ./user-config.yaml
```

### 4.11 Update the user-config from the keystore

`update_user_config` re-syncs a user config with the keys in a keystore
file — the same offline maintenance operation the `update-config` CLI
command performs. It takes the config path and the keystore path and
returns `0` on success. We point it at the pair generated above:

```bash
logoscore call liblogos_blockchain_module update_user_config ./user-config.yaml ./keystore.yaml
```

### 4.12 Query consensus info before the node is running

The node-backed methods (wallet, explorer, consensus) need a started
node. Calling `get_cryptarchia_info` now — before `start` — returns the
module's well-defined `The node is not running` message. This proves the
method is wired through the IPC bridge and callable; actually starting
the node (which dials the devnet) is out of scope for this offline
doc-test:

```bash
logoscore call liblogos_blockchain_module get_cryptarchia_info
```

### 4.13 Query a wallet balance before the node is running

`wallet_get_balance` behaves the same way — it reports the node is not
running rather than crashing, so a frontend can surface a clean error:

```bash
logoscore call liblogos_blockchain_module wallet_get_balance <address-hex>
```

### 4.14 Query the block explorer before the node is running

The explorer method `get_block` is the same: callable through the bridge,
and reporting the node is not running until one is started:

```bash
logoscore call liblogos_blockchain_module get_block <header-id-hex>
```

### 4.15 Stop the daemon

Shut the daemon down cleanly:

```bash
logoscore stop
```

The daemon removes its state file and exits.

```bash
sleep 2
```

### 4.16 Confirm the daemon has stopped

With no daemon running, the client reports `not_running` and exits
non-zero, so we add `|| true` to let the doc-test assert on the output:

```bash
logoscore status
```
