import * as vscode from "vscode";

/**
 * A just-typed `else` dedents to the `if` statement whose block it continues, and nowhere else. In a `match` or a
 * multi-line if-else expression, `else` is the last arm and stays level with the arms above it.
 *
 * `decreaseIndentPattern` only sees the one line, so it can't tell those apart: `else` isn't in it, and this does the
 * statement case instead.
 */
const ELSE_LINE = /^(\s*)else\s*$/;

/// An arm of a `match` (`case ... then`) or of a multi-line if-else expression (`then ...`)
const ARM_LINE = /^\s*(?:case|then)\b/;

/// An `if` or `elseif` statement header, whose block an `else` below it continues
const IF_STATEMENT_HEADER = /^\s*(?:if|elseif)\b.*\bthen\s*(?:--.*)?$/;

const BLANK_OR_COMMENT = /^\s*(?:--.*)?$/;

/// Languages using our language configuration, which leaves `else` to this
const ELSE_INDENT_LANGUAGES = new Set(["luwu", "luau"]);

/** How many lines above the `else` are searched for its `if` */
const MAX_LINES_SEARCHED = 500;

function indentWidth(line: string, tabSize: number): number {
  let width = 0;
  for (const ch of line) {
    if (ch === " ") {
      width += 1;
    } else if (ch === "\t") {
      width += tabSize;
    } else {
      break;
    }
  }
  return width;
}

function leadingWhitespace(line: string): string {
  return /^\s*/.exec(line)?.[0] ?? "";
}

/**
 * Returns the indentation the `else` on `line` should have, or undefined to leave it alone.
 *
 * `linesAbove` are the lines before it, nearest last.
 */
export function computeElseIndent(
  linesAbove: string[],
  line: string,
  tabSize: number,
): string | undefined {
  const elseLine = ELSE_LINE.exec(line);
  if (!elseLine) {
    return undefined;
  }
  const width = indentWidth(line, tabSize);

  let previous = linesAbove.length - 1;
  while (previous >= 0 && BLANK_OR_COMMENT.test(linesAbove[previous])) {
    previous--;
  }
  if (previous < 0) {
    return undefined;
  }

  // the next arm of an expression, or already placed further out than the code above it
  const previousLine = linesAbove[previous];
  if (
    ARM_LINE.test(previousLine) ||
    indentWidth(previousLine, tabSize) > width
  ) {
    return undefined;
  }

  // the block the `else` was typed in is the one opened by the nearest line further out
  for (let i = previous; i >= 0; i--) {
    const candidate = linesAbove[i];
    if (
      BLANK_OR_COMMENT.test(candidate) ||
      indentWidth(candidate, tabSize) >= width
    ) {
      continue;
    }
    return IF_STATEMENT_HEADER.test(candidate)
      ? leadingWhitespace(candidate)
      : undefined;
  }

  return undefined;
}

function onDidChangeTextDocument(event: vscode.TextDocumentChangeEvent): void {
  if (
    !ELSE_INDENT_LANGUAGES.has(event.document.languageId) ||
    event.contentChanges.length !== 1
  ) {
    return;
  }

  const change = event.contentChanges[0];
  if (change.text.length === 0 || change.text.includes("\n")) {
    return;
  }

  const editor = vscode.window.activeTextEditor;
  if (
    !editor ||
    editor.document !== event.document ||
    editor.selections.length !== 1
  ) {
    return;
  }

  const lineNumber = change.range.start.line;
  const line = event.document.lineAt(lineNumber).text;
  if (!ELSE_LINE.test(line)) {
    return;
  }

  const firstLine = Math.max(0, lineNumber - MAX_LINES_SEARCHED);
  const linesAbove: string[] = [];
  for (let i = firstLine; i < lineNumber; i++) {
    linesAbove.push(event.document.lineAt(i).text);
  }

  const tabSize =
    typeof editor.options.tabSize === "number" ? editor.options.tabSize : 4;
  const indent = computeElseIndent(linesAbove, line, tabSize);
  if (indent === undefined || indent === leadingWhitespace(line)) {
    return;
  }

  const range = new vscode.Range(
    lineNumber,
    0,
    lineNumber,
    leadingWhitespace(line).length,
  );
  void editor.edit((builder) => builder.replace(range, indent), {
    undoStopBefore: false,
    undoStopAfter: false,
  });
}

export function registerElseIndent(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument(onDidChangeTextDocument),
  );
}
