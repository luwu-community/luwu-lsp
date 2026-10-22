# Luwu Language Server

A language server for [Luwu](https://github.com/luwu-community/luwu), a community fork of the
[Luau](https://github.com/luau-lang/luau) programming language.

`luwu-lsp` is a fork of JohnnyMorgan's [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) that builds against Luwu instead of Luau.

In addition to supporting everything Luau LSP supports, Luwu LSP *significantly* improves DX, bringing type hovers that contain exactly which type aliases
the type you're hovering over references, providing both in-hover go-to links to them as well as surfacing the relevant types in the hover itself, better
TypeError messages, somewhat improved reliability with more complex FFlag configuration, better table and string formatting, more refactoring options, and
importantly, support for all Luwu features that don't exist in Luau.

Note that Luwu is compatible with all Luau code 'til 0.731 and switching to Luwu LSP is just a drop-in replacement to your existing
Luau LSP server path binary.

Luwu language features, such as classes, the `none` primitive, standard library additions, improved extern type definition behavior, and more, are on by default; pass `--luau-compat` (or set
`luwu.luauCompatibilityMode`) for Luau compatibility mode, which disables them. The new type solver is enabled in
every configuration.

## Getting Started

Prebuilt binaries and VS Code extensions are on the
[`luwu-latest` prerelease](https://github.com/luwu-community/luwu-lsp/releases/tag/luwu-latest). It's rebuilt from the
`luwu` branch on every push, so download it again to update.

| Platform | VS Code extension | Server only |
|---|---|---|
| Windows x64 | `luwu-lsp-win32-x64.vsix` | `luwu-lsp-windows-x64.zip` |
| macOS (Apple Silicon) | `luwu-lsp-darwin-arm64.vsix` | `luwu-lsp-macos-universal.zip` |
| macOS (Intel) | `luwu-lsp-darwin-x64.vsix` | `luwu-lsp-macos-universal.zip` |
| Linux x64 | `luwu-lsp-linux-x64.vsix` | `luwu-lsp-linux-x64.zip` |
| Linux arm64 | `luwu-lsp-linux-arm64.vsix` | `luwu-lsp-linux-arm64.zip` |

The Linux builds run on any distribution with glibc 2.28 or newer (RHEL 8, Debian 10, Ubuntu 18.10 and later).

### VS Code

The `.vsix` has the server bundled. These commands download the one for your platform and install it in one go. Run
them again to update.

Linux and macOS:

```sh
case "$(uname -sm)" in
    "Linux x86_64") target=linux-x64 ;;
    "Linux aarch64") target=linux-arm64 ;;
    "Darwin arm64") target=darwin-arm64 ;;
    "Darwin x86_64") target=darwin-x64 ;;
esac
curl -fL -o "${TMPDIR:-/tmp}/luwu-lsp.vsix" "https://github.com/luwu-community/luwu-lsp/releases/download/luwu-latest/luwu-lsp-$target.vsix"
code --install-extension "${TMPDIR:-/tmp}/luwu-lsp.vsix"
```

Windows (PowerShell):

```powershell
$vsix = "$env:TEMP\luwu-lsp.vsix"
Invoke-WebRequest "https://github.com/luwu-community/luwu-lsp/releases/download/luwu-latest/luwu-lsp-win32-x64.vsix" -OutFile $vsix
code --install-extension $vsix
```

If you downloaded the `.vsix` from the release page instead, pass its full path, e.g.
`code --install-extension ~/Downloads/luwu-lsp-linux-x64.vsix` (`$HOME\Downloads\luwu-lsp-win32-x64.vsix` on Windows).
You can also install it from VS Code: open the Extensions view, click `...` at the top and pick **Install from VSIX...**.

Then:

1. If you have JohnnyMorganz's **Luau Language Server** extension installed, disable it (at least in workspaces where you
   use Luwu). Otherwise both extensions start a server on the same files.
2. Run **Developer: Reload Window**.

#### Settings

The extension's settings live under `luwu.*` (`luwu.server.path`, `luwu.types.definitionFiles`, ...). It also reads
any `luau-lsp.*` settings you already have, so an existing Luau LSP configuration carries over. A `luwu.*` setting
always wins over its `luau-lsp.*` counterpart. Set `"luwu.inheritLuauLspSettings": false` to ignore `luau-lsp.*`
entirely, for example when you keep upstream's extension installed and configure the two separately.

For now, edit settings in `settings.json` (**Preferences: Open User Settings (JSON)** or `.vscode/settings.json`)
rather than the Settings UI. The extension declares every setting under both names, so the UI lists each one twice
and it's easy to change the `luau-lsp.*` copy that a `luwu.*` value then overrides. Write new settings as `luwu.*`:

```json
{
    "luwu.platform.type": "standard",
    "luwu.types.definitionFiles": ["./globals.d.luau"]
}
```

### Other editors

Download the server zip for your platform and unzip `luwu-lsp` (`luwu-lsp.exe` on Windows) somewhere on your `PATH`. Then
point your editor's Luau language server configuration at it. It's a drop-in replacement for the `luau-lsp` binary and
takes the same arguments (`luwu-lsp lsp`). On macOS, Gatekeeper blocks the downloaded binary until you clear its
quarantine flag with `xattr -d com.apple.quarantine luwu-lsp`.

The server also typechecks from the command line:

```sh
luwu-lsp analyze --defs=globals.d.luau src/
```

The bundled clients in [`editors/`](editors/README.md) are inherited from luau-lsp.

### Build From Source

To build via `rebuild.luau`, you need [CMake](https://cmake.org/), git and [seal 0.8.x](https://github.com/seal-runtime/seal/releases/tag/v0.8.1).
You can install `seal` manually or via a toolchain manager such as Rokit.

```sh
git clone https://github.com/luwu-community/luwu-lsp.git --recurse-submodules

```

To use Luwu LSP with your existing Luau LSP (vs)code extension, set `"luau-lsp.server.path": "/path/to/luwu-lsp/build/luwu-lsp"` and then
Reload Language Server.

```sh
seal ./rebuild.luau --release          # build luwu-lsp against the luwu submodule
seal ./rebuild.luau --targets CLI,Test # dev build against $LUWU_TEST_PATH, including tests
seal ./rebuild.luau --luwu=here        # dev build against the submodule
seal ./rebuild.luau --clean            # wipe build/ and reconfigure
seal ./rebuild.luau --help
```

For development alongside Luwu itself, set the environment variable `LUWU_TEST_PATH` to your local checkout of the `luwu` repository; dev builds
compile against it instead of the submodule. Run `git config core.hooksPath .githooks` once to enable the pre-commit
hook that refuses a commit whose `luwu` submodule doesn't match the commit checked out in `LUWU_TEST_PATH`.

## Configuration

General configuration (strictness, lints, require aliases) comes from `.luaurc` files, as described in Luau's
[RFC documentation](https://rfcs.luau.org/config-luaurc.html). Language-server-specific settings are under `luwu` in
your editor's settings (see [Settings](#settings) for how `luau-lsp.*` ones are read).

To provide global type definitions for a custom environment, use `luwu.types.definitionFiles`, with documentation
in `luwu.types.documentationFiles`. Require-by-string (`require("./module")`) works out of the box.

LSP platform defaults to `standard` mode.

Roblox platform support is inherited from luau-lsp and enabled with `luwu.platform.type: "roblox"`; see the
[luau-lsp README](https://github.com/JohnnyMorganz/luau-lsp#readme) for how it is configured.

## Supported Features

- [x] Diagnostics (incl. type errors)
- [x] Autocompletion
- [x] Hover
- [x] Signature Help
- [x] Go To Definition
- [x] Go To Type Definition
- [x] Find References
- [x] Document Link
- [x] Document Symbol
- [x] Color Provider
- [x] Rename
- [x] Semantic Tokens
- [x] Inlay Hints
- [x] Documentation Comments ([Moonwave Style](https://github.com/evaera/moonwave))
- [x] Code Actions
- [x] Workspace Symbols
- [x] Folding Range
- [x] Call Hierarchy

### Compile with CMake

Use this if you don't have the seal runtime installed or just need to use CMake instead.

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo   # add -DLSP_LUAU_PATH=/path/to/luwu to use another checkout
cmake --build build --target Luwu.LanguageServer.CLI
```

The binary is `build/luwu-lsp`.

### Tests

Build `Luwu.LanguageServer.Test`, then run it from the repository root (tests read `tests/testdata/` by relative path):

```sh
./build/Luwu.LanguageServer.Test
```
