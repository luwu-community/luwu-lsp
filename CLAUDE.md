# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

**`luwu-lsp` is the language server for [Luwu](../luwu), our own community fork of Luau.** It is itself a fork of
upstream [`JohnnyMorganz/luau-lsp`](https://github.com/JohnnyMorganz/luau-lsp) (git remote `upstream`; our remote is
`origin` = `luwu-community/luwu-lsp`), and it builds against Luwu instead of upstream Luau.

- "Upstream" means `JohnnyMorganz/luau-lsp` (for this repo) or `luau-lang/luau` (for the language). Neither is the
  source of truth for Luwu semantics.
- Most of the code, docs (`editors/`, `tests/README.md`) and settings names (`luau-lsp.*`) are still inherited from
  upstream. Don't assume upstream docs, issue numbers, release process, VSCode Marketplace listing or crash-reporting
  setup apply here.
- `../luwu` refers to `../luwu` or `$LUWU_TEST_PATH`.
- Luwu-specific work is mostly editor support for Luwu language features
  (classes, none, destructuring, integers, ? operators), and improving DX.
- The language spec lives in `$LUWU_TEST_PATH/rfcs` and ask before doing anything that implies a semantics change.

## The `luwu/` submodule

**Never read or edit the `luwu/` submodule in this repo -- it can be stale.** The Luwu being developed lives at
`LUWU_TEST_PATH` (usually `../luwu`). Read Luwu sources there.

Analysis/Ast changes the LSP needs are made in `../luwu` first, then the LSP side is
adjusted here. The submodule is only bumped when we deliberately sync (`seal ./rebuild.luau submodule-update`, then
commit the pointer).

## Building

Use `rebuild.luau` (needs `seal` 0.8.1+) from the repo root. It configures `build/`
with `-DLSP_LUAU_PATH=<luwu dir>`, reconfigures automatically when that path changes, regenerates keyword hover docs
(`scripts/generate_keywords`, writes `keyword_hovers.json`), and builds the requested targets.

```bash
# Dev: build the CLI against LUWU_TEST_PATH (../luwu)
seal ./rebuild.luau

# Build the tests too
seal ./rebuild.luau --targets CLI,Test

# Build against the vendored submodule instead (what --release does by default)
seal ./rebuild.luau --luwu=here
seal ./rebuild.luau --release --luwu=here

# Stale build artifacts? Wipe build/ and reconfigure
seal ./rebuild.luau --clean
```

Build errors are also written to `build_errors.log`.

Plain CMake works too (keep `-DCMAKE_BUILD_TYPE=RelWithDebInfo`; Debug-style asserts in Luwu Analysis can crash the
whole server on known TODOs):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLSP_LUAU_PATH=$LUWU_TEST_PATH
cmake --build build --target Luwu.LanguageServer.Test -j8
```

**Use `-j16` at most** when invoking cmake directly -- full parallelism has crashed the dev machine.

### CMake targets

- `Luwu.LanguageServer`: static library with the LSP implementation
- `Luwu.LanguageServer.CLI`: executable, output name `luwu-lsp`
- `Luwu.LanguageServer.Test`: doctest test executable

### CMake options

- `LSP_LUAU_PATH`: Luau/Luwu source tree to build against (defaults to `./luwu`)
- `LUAU_ENABLE_TIME_TRACE`: Luau time tracing
- `LSP_BUILD_ASAN`: AddressSanitizer
- `LSP_STATIC_CRT`: static CRT on Windows
- `LSP_BUILD_WITH_SENTRY`: crash reporting (off by default; the DSN in `src/main.cpp` is upstream's)
- `LSP_WERROR`: warnings as errors (default ON)

## Running tests

Run from the repo root (tests read `tests/testdata/` by relative path):

```bash
./build/Luwu.LanguageServer.Test
./build/Luwu.LanguageServer.Test --test-case="TestName"
./build/Luwu.LanguageServer.Test --new-solver
./build/Luwu.LanguageServer.Test --fflags=true
./build/Luwu.LanguageServer.Test --list-test-cases
```

After LSP changes, the user tests in their editor with the freshly built `build/luwu-lsp`; tell them to reload the
editor.

## Luwu feature flags

Class features are behind fast flags defined in Luwu (see `../luwu/CLAUDE.md` "Feature flags"):

- `FFlag::DebugLuauUserDefinedClasses` -- parser/compiler support (from upstream)
- `FFlag::DebugLuauUserDefinedClassesRuntime` -- VM runtime (from upstream)
- `FFlag::LuwuBetterUserDefinedClasses` -- Luwu-specific class features

`applyLuwuFlags()` (`src/Flags.cpp`) turns on every `Luwu`-prefixed flag plus the two class `DebugLuau` flags at
startup, and `--luau-compat` / `luau-lsp.luauCompatibilityMode` turns them off. It also forces `LuauSolverV2` on in
every configuration (classes only typecheck under the new solver). It runs before user-supplied flags, so
`--flag:Name=false` and `luau-lsp.fflags.override` still win.

Flags get renamed in Luwu; when they do, update every `FFlag::` reference and `LUAU_FASTFLAG` declaration here too.
Check the current name in `../luwu` rather than trusting existing tests.

## Architecture

- **LanguageServer** (`src/LanguageServer.cpp`, `src/include/LSP/LanguageServer.hpp`): JSON-RPC dispatcher.
- **WorkspaceFolder** (`src/Workspace.cpp`, `src/include/LSP/Workspace.hpp`): owns the Luau `Frontend`; LSP
  operations are methods on it.
- **WorkspaceFileResolver** (`src/WorkspaceFileResolver.cpp`): Luau `FileResolver` -- file reading, module
  resolution, config loading.
- **LSPPlatform** / **RobloxPlatform** (`src/include/Platform/`): platform hooks; the Roblox one (sourcemaps,
  DataModel types) is inherited from upstream and kept for backwards compat.
- **Operations** (`src/operations/`): one file per LSP feature (Completion, Hover, CodeAction, Rename, InlayHints, ...).
- **LuauExt** (`src/LuauExt.cpp`, `src/include/LSP/LuauExt.hpp`): AST/type helpers, including the class helpers
  (`findClassStatContainingPosition`, `findClassNameReferences`, `findClassMemberReferences`).
- **Transport** (`src/transport/`): stdio and named-pipe JSON-RPC.
- **Protocol** (`src/include/Protocol/`): LSP structs with nlohmann/json serialization.
- **Dependencies**: `luwu/` (don't touch, see above), `extern/` (json, glob, argparse, toml, doctest).
- **Editor clients** (`editors/`): still upstream's VSCode/nvim/zed/IntelliJ clients.

## Code style

- C++17, Allman braces (`.clang-format`), 4-space indent, 150 column limit
- Luwu/Luau code: prefer new features like `const` over `local`, snake_case.
  Luwu/Luau scripts not inherited from upstream are written in the seal runtime.

## Changelog and commits

- At the end of a session, add a `CHANGELOG.md` entry under `[Unreleased]` for user-facing changes. Ask user before updating
  `CHANGELOG.md`
- Never commit unless the user asks for it in that turn.
- Upstream GitHub issue numbers don't apply to this fork; only reference issues from `luwu-community/luwu-lsp`.

## Testing patterns

Tests use `Fixture` from `tests/Fixture.h` with doctest's `TEST_CASE_FIXTURE`:

```cpp
TEST_CASE_FIXTURE(Fixture, "FeatureName")
{
    auto uri = newDocument("test.luau", "local x = 1");
    // Test operations using workspace
}
```

- `newDocument()`: create and register a test document
- `check()`: type check source
- `loadDefinition()`: load definition files
- `loadSourcemap()`: load a Rojo sourcemap
- `sourceWithMarker()`: parse source with a `|` cursor marker

Class tests need the class flags and the new solver:

```cpp
TEST_CASE_FIXTURE(Fixture, "class_feature")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();
    // ...
}
```

Use `ENABLE_NEW_SOLVER()`, never `ScopedFastFlag{FFlag::LuauSolverV2, true}` -- the Frontend caches the solver mode at
construction, and the macro updates both.
