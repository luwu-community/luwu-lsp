# External diagnostics

Any program on your machine can add errors and warnings to the ones luwu-lsp reports. Set a port in your workspace
settings:

```json
{
    "luwu.diagnostics.externalPort": 7621
}
```

luwu-lsp then takes JSON posted to `http://127.0.0.1:7621/diagnostics`. Its diagnostics appear in the editor and the
Problems panel next to Luwu's own. The VS Code extension gives them badges like Luwu's.

## Sending diagnostics

From a seal script:

```luau
const http = require("@std/net/http")

const response = http.post({
    url = "http://127.0.0.1:7621/diagnostics",
    body = {
        source = "luwu-website",
        files = {
            ["src/home.luwu"] = {
                {
                    line = 39,
                    column = 152,
                    endColumn = 154,
                    code = "ZJoinsWords",
                    message = "Add a space before `\\z` to keep this word apart from the next line's first.",
                },
            },
        },
    },
})
```

A request names the program that sends it (`source`) and lists `files`, each with its diagnostics. A file listed gets
all of `source`'s earlier diagnostics replaced by the new ones, so an empty list clears them. Files left out keep
theirs, unless the request sets `replaceAll = true`.

A file is a path relative to a workspace folder, an absolute path or a `file:` URI.

Each diagnostic has these fields:

| Field | | |
|---|---|---|
| `line`, `column` | required | Where it starts. Lines and columns start at 1 and count UTF-8 bytes, like `string.find`. |
| `endLine`, `endColumn` | optional | Where it ends, exclusive. `endLine` defaults to `line` and `endColumn` to one past `column`. |
| `message` | required | |
| `kind` | optional | `SyntaxError`, `TypeError`, `Typo` or `Lint` (the default). It sets the badge's colour. |
| `code` | optional | A lint's name, shown on its badge. A lint without one is labeled with `source`. |
| `severity` | optional | `error`, `warning`, `information` or `hint`. Lints default to warnings and the rest to errors. |
| `badge` | optional | Text for the badge instead of its label, or `{ text, background, foreground }` with CSS colours, or `false` for no badge. |
| `fixes` | optional | Quick fixes the editor offers, described below. |

Messages start with their label, the way Luwu's do: `ZJoinsWords: Add a space before ...`.

## Quick fixes

Each fix has a `title` and the `edits` it makes:

```luau
fixes = {
    {
        title = "Add a space before `\\z`",
        edits = {
            {
                line = 39,
                column = 159,
                newText = " ",
            },
        },
    },
},
```

An edit has the same positions as a diagnostic. Without `endLine` and `endColumn` it inserts `newText` at its start.
An edit that ends on another line needs `endColumn`.

The first fix is the preferred one, which the editor's auto fix applies. When a file has more than one diagnostic with
the same `code` and fixes, the editor also offers to fix them all at once with each one's first fix. Fixes with the same
code shouldn't overlap.

## Responses

luwu-lsp answers `200` with `{ "ok": true }` once it has queued the diagnostics. A request it can't use gets
`{ "ok": false, "error": "..." }` with a `4xx` status that says what's wrong:

- `400`: the body isn't valid JSON or a field is wrong. The error names the file and field.
- `403`: the request came from a web page (it has an `Origin` header) or its `Host` isn't `127.0.0.1` or `localhost`.
- `404` or `405`: it wasn't a `POST` to `/diagnostics`.
- `415`: the body wasn't sent as `application/json`. seal's `http.post` sends a table as JSON.

When luwu-lsp isn't running, or the port isn't set, the connection is refused. A long-running script should expect
that and post again later.

## Positions and unsaved edits

Positions are taken against the file as luwu-lsp sees it: the editor's text while the file is open, and the file on
disk otherwise. A script that reads files from disk gets positions for the saved file. Unsaved edits above a
diagnostic move it until the script posts again.

## More than one editor window

Only one program can listen on a port. A second luwu-lsp on the same port logs that it can't listen and takes no
external diagnostics.
