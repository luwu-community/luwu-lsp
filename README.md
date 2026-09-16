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
`luau-lsp.luauCompatibilityMode`) for Luau compatibility mode, which disables them. The new type solver is enabled in
every configuration.

## Getting Started

We're getting ready to ship editor extensions and prebuild binaries, but for now you kinda need to compile luwu-lsp from source.

The bundled clients in [`editors/`](editors/README.md) are inherited from luau-lsp. For now, settings still use the `luau-lsp.*`
prefix.

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
compile against it instead of the submodule.

## Configuration

General configuration (strictness, lints, require aliases) comes from `.luaurc` files, as described in Luau's
[RFC documentation](https://rfcs.luau.org/config-luaurc.html). Language-server-specific settings are under `luau-lsp` in
your editor's settings.

To provide global type definitions for a custom environment, use `luau-lsp.types.definitionFiles`, with documentation
in `luau-lsp.types.documentationFiles`. Require-by-string (`require("./module")`) works out of the box.

LSP platform defaults to `standard` mode.

Roblox platform support is inherited from luau-lsp and enabled with `luau-lsp.platform.type: "roblox"`; see the
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
