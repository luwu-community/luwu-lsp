# Luwu for Zed

A Zed extension that adds the Luwu and Luau languages and runs [`luwu-lsp`](../../README.md) for both. It replaces
[zed-luau](https://github.com/4teapo/zed-luau) for Luwu users: that extension only serves `.luau` files, and its
settings have a trap (Zed's own `lsp.luau-lsp.binary` silently disables the extension's definitions and Roblox
options). Uninstall zed-luau when you install this one, or Zed starts both servers for `.luau` files.

## Installing

The extension isn't published yet. The [installer](../../README.md#getting-started) downloads its source
(`luwu-zed-extension.zip` from the `luwu-latest` release), points `binary.path` at the server it installs and names the
folder it unpacked the extension into. Install that folder as a dev extension: run `zed: install dev extension` from
the command palette and pick it, or pick this directory (`editors/zed`) in a clone. Zed compiles the Rust part and
the grammar itself; it needs `cargo` on your PATH, adds the `wasm32-wasip2` target to your toolchain and downloads its
own wasi-sdk for the grammar.
After changing anything in here, `zed: rebuild dev extension` picks it up.

The extension finds the server in this order:

1. `binary.path` from the settings below;
2. `luwu-lsp` on your PATH, unless `binary.ignore_system_version` is true;
3. the current build of the `luwu` branch, from the
   [`luwu-latest`](https://github.com/luwu-community/luwu-lsp/releases/tag/luwu-latest) release. It's downloaded into
   Zed's extension work directory and downloaded again when the release's asset changes.

## Settings

Everything lives under `lsp.luwu-lsp.settings`. The `luau-lsp` object is the server's own configuration, the same keys
the VS Code extension exposes as `luwu.*`, sent to the server as workspace configuration. The other keys are the
extension's:

```json
{
  "lsp": {
    "luwu-lsp": {
      "settings": {
        "binary": { "path": "/path/to/luwu-lsp", "args": [] },
        "definitions": { "@seal": "./.seal/typedefs/globals.d.luau" },
        "documentation": [],
        "luau_compat": false,
        "fflags": { "enable_by_default": true, "override": {} },
        "roblox": { "enabled": false, "security_level": "plugin" },
        "luau-lsp": {
          "diagnostics": { "workspace": true },
          "inlayHints": { "parameterNames": "all" }
        }
      }
    }
  }
}
```

- `definitions` maps a package name to a definitions file, like the VS Code `luwu.types.definitionFiles` setting.
  Relative paths resolve against the worktree root. A plain list of paths works too, so settings copied from
  zed-luau keep working. `documentation` lists documentation files for those definitions.
- `luau_compat` starts the server with `--luau-compat`, turning Luwu's language extensions off for a plain Luau
  project.
- `fflags.enable_by_default: false` passes `--no-flags-enabled`; `fflags.override` sets individual flags.
- `roblox.enabled` downloads Roblox's type definitions and API docs for the chosen `security_level`
  (`none`, `local_user`, `plugin` or `roblox_script`), as zed-luau does.
- `lsp.luwu-lsp.initialization_options` is passed to the server unchanged.

Don't set Zed's own `lsp.luwu-lsp.binary`: Zed then launches that command with exactly the arguments listed there,
skipping the extension and every setting above. Use `binary.path` instead.

## What you get

- Diagnostics, hover (with the server's keyword docs and class signatures rendered as Luwu), completion,
  go to definition (doc-comment references like `[Document.heading_links]` included), rename, inlay hints and
  code actions, all from luwu-lsp.
- Highlighting for Luwu's syntax (see the next section), with doc comments rendered as Markdown: `[Type.member]`
  references look like links, and a ```luwu fence inside a doc comment gets Luwu's colors.
- Snippets (`snippets/luwu.json`, `snippets/luau.json`): `class`, `classp`, `impl`, `trait`, `method`, `init`,
  `cfn`, `fn`, `ifc`, `do`, `comp`, `forw`, `orr`, `req`, `eclass`, `efn`, `type`, `union`, `doc`. Luau gets the
  usual `lfn`, `for`, `fori`, `while`, `repeat`, `pcall` and friends.
- File icons: a cat-mask icon for `.luwu`, two-tone braces for `.luwon` (it's Luwu's JSON), the Luau icon for
  `.luau`, and the mask or the Luau icon with a cog for the configuration files (`.config.luwon` and
  `luwu.luwon`; `.luaurc` and `.config.luau`). Zed only takes icons as
  a whole icon theme, so these come as the `Luwu` and `Luwu Light` icon themes (`icon_themes/luwu.json`), which
  fall back to Zed's defaults for every other file. Select one:

  ```json
  { "icon_theme": "Luwu" }
  ```

- Richer highlighting from the server, if you turn on semantic tokens (off by default in Zed): class and trait
  names, parameters, properties and `self` get their meaning's color on top of the grammar's. The extension
  ships the rules that map luwu-lsp's token types to theme styles (`languages/*/semantic_token_rules.json`);
  enable them per language:

  ```json
  { "languages": { "Luwu": { "semantic_tokens": "combined" }, "Luau": { "semantic_tokens": "combined" } } }
  ```

- The outline (`outline: toggle`) lists classes and traits along with functions, through a text-matched pattern,
  since the grammar doesn't parse class headers.
- Indentation for the constructs the parser can't see, through the line patterns in `languages/luwu/config.toml`:
  a class or trait header (`class Player(` or a closing `) implements Health`), `then`, `do`, `else`, `repeat` and
  an opening bracket indent the next line; `end`, `until`, `else`, `elseif` and a closing bracket outdent. A lone
  `)` closing a multi-line constructor doesn't indent the body, since the same line also closes ordinary calls.

## Highlighting is a stopgap, on a forked grammar

There is no tree-sitter grammar for Luwu yet, so both languages parse with tree-sitter-luau. Luwu's keywords
(`class`, `trait`, `when`, `give`, `none`, ...) come out of that grammar as identifiers, and
`languages/luwu/highlights.scm` colors them by their text. Two consequences:

- A variable named `match` or `class` gets the keyword color too.
- Where the parser can't recover from Luwu syntax (class bodies, destructuring, `do` expressions), an error node
  can swallow structure, and the code around it may indent or fold oddly. The keywords inside still color, since
  the error region's tokens are matched by text as well.

The grammar is pinned to the `luwu-zed` branch of
[luwu-community/tree-sitter-luau](https://github.com/luwu-community/tree-sitter-luau), three commits on top of
upstream, each needed for Zed:

1. **The `end` token is named `end_keyword`.** Zed can't match a token named `end` in a grammar loaded as WASM
   (the name collides with tree-sitter's end-of-input symbol; Zed's own Lua extension never colors `end` either).
   Only the generated `src/parser.c` and `src/node-types.json` change, so lexing and parsing are untouched, and
   every query in `languages/` writes `"end_keyword"` where tree-sitter-luau's queries write `"end"`.
2. **No string or comment content while recovering from an error.** With every token valid during recovery, the
   scanner ate code as string content, and a doc comment inside a class body (unparseable today) showed as code.
3. **Block comments give one content node per line, without the indentation.** Markdown treats indented lines as
   a code block, so `languages/*/injections.scm` hands each line to Markdown separately (that styles inline code and
   references) and all of them together as one combined document (that's what makes fences work; Zed's inline
   Markdown doesn't survive a combined layer, hence both).

To change the grammar: clone the fork, edit `grammar.js` or `src/scanner.c`, `npm install` (it extends
tree-sitter-lua from npm), `tree-sitter generate`, then rename the token again in the generated files:

```sh
sed -i 's/^  \[anon_sym_end\] = "end",$/  [anon_sym_end] = "end_keyword",/' src/parser.c
sed -i 's/^      "type": "end",$/      "type": "end_keyword",/' src/node-types.json
```

Commit, push, and update `rev` in `extension.toml`. When a Luwu grammar exists, `extension.toml` points
`[grammars.luwu]` at it and the text-matching block in the highlights goes away.

## What a Zed extension can't do

Extensions register languages, servers, snippets and themes; they can't hook into editing. So, unlike the VS Code
extension:

- Autocompleting `end` can't move the cursor back into the block. The server asks for that with its custom
  `$/command` notification, which Zed ignores.
- There is no tab-out, and no on-type reindent of `else`.

## Layout

```
extension.toml        the extension, its language server, snippets and the pinned grammar
Cargo.toml, src/      the server launcher, compiled to WASM by Zed
languages/luwu/       config.toml and the tree-sitter queries for .luwu files
languages/luau/       the same for .luau files, without the Luwu keyword block
snippets/             one file per language
icon_themes/, icons/  the Luwu icon themes and their SVGs
```

The icons other than the hand-drawn Luau one come from `icons/generate.luau` (run it from this directory with
`seal`); `--large` writes `*-config-*-large.svg`, the configuration icons with the base at three quarters and a
bigger gear, for renderings larger than a tree row. The VS Code extension gets copies of the luwon and luwu-config
icons in `editors/code/assets`.

`target/`, `grammars/` and `*.wasm` are build output from a dev install and are ignored by git.
