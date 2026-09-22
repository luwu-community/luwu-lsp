import * as vscode from "vscode";

/**
 * A long bracket opener typed by hand -- `--[=[`, `[==[` -- auto-closes as a
 * plain `]]`, because all VSCode sees is the single `[`/`]` pair firing twice.
 * That never parses: `--[=[` has to be closed by `]=]`.
 *
 * We can't fix this with an `autoClosingPairs` entry, because the level markers
 * are only known once the closing `[` is typed, and suppressing the single `[`
 * pair inside comments would stop `]` closing `[` in ordinary comment text. So
 * the pair stays as it is and the markers are put back here, right after the
 * `[` is typed.
 */
const LONG_BRACKET_OPEN = /\[(=+)\[$/;

/// Long brackets are Lua syntax, so this applies to every dialect we contribute
/// a language for. `lua` is VSCode's built-in language, whose own configuration
/// auto-closes `[` the same way.
const LONG_BRACKET_LANGUAGES = new Set(["luwu", "luau", "lua"]);

/**
 * Returns the `=` markers to insert between the two auto-closed `]`, or
 * undefined if this isn't a long bracket that auto-closed to the wrong level.
 *
 * `textBeforeCursor`/`textAfterCursor` are the parts of the line either side of
 * the cursor, taken just after the `[` was typed.
 */
export function computeLongBracketLevelFixup(
  textBeforeCursor: string,
  textAfterCursor: string,
): string | undefined {
  const match = LONG_BRACKET_OPEN.exec(textBeforeCursor);
  if (!match) {
    return undefined;
  }

  // only correct what auto-closing actually inserted; if it's off, or the user
  // typed the opener in front of something else, leave the text alone
  if (!textAfterCursor.startsWith("]]")) {
    return undefined;
  }

  return match[1];
}

/**
 * VSCode types the opener and its auto-closed partner as one edit, so an
 * auto-closed `[` arrives as the text `[]`. A bare `[` means auto-closing
 * didn't fire (it is off, or the next character isn't in `autoCloseBefore`) and
 * there is nothing to correct.
 */
function isAutoClosedBracketInsertion(
  event: vscode.TextDocumentChangeEvent,
): vscode.TextDocumentContentChangeEvent | undefined {
  if (event.contentChanges.length !== 1) {
    return undefined;
  }

  const change = event.contentChanges[0];
  if (change.text !== "[]" || change.rangeLength !== 0) {
    return undefined;
  }

  return change;
}

async function onDidChangeTextDocument(
  event: vscode.TextDocumentChangeEvent,
): Promise<void> {
  if (!LONG_BRACKET_LANGUAGES.has(event.document.languageId)) {
    return;
  }

  const change = isAutoClosedBracketInsertion(event);
  if (!change) {
    return;
  }

  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document !== event.document) {
    return;
  }

  // position just after the `[` that was typed
  const typedAt = event.document.positionAt(change.rangeOffset);
  const position = typedAt.translate(0, 1);
  if (editor.selections.length !== 1 || !editor.selection.isEmpty) {
    return;
  }
  // the document change reaches us before the cursor move that goes with it,
  // so the selection is usually still in front of the `[` at this point
  const cursor = editor.selection.active;
  if (!cursor.isEqual(typedAt) && !cursor.isEqual(position)) {
    return;
  }

  const line = event.document.lineAt(position.line).text;
  const markers = computeLongBracketLevelFixup(
    line.slice(0, position.character),
    line.slice(position.character),
  );
  if (!markers) {
    return;
  }

  // insert between the two auto-closed `]`, so `]]` becomes `]==]`. Everything
  // we touch is after the cursor, which therefore stays where the user left it
  const insertAt = position.translate(0, 1);
  await editor.edit((builder) => builder.insert(insertAt, markers), {
    undoStopBefore: false,
    undoStopAfter: false,
  });
}

export function registerLongBracketAutoClose(
  context: vscode.ExtensionContext,
): void {
  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument((event) => {
      void onDidChangeTextDocument(event);
    }),
  );
}
