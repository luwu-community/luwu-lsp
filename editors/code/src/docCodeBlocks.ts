import * as vscode from "vscode";
import { getSettingOr, settingChanged } from "./settings";

/**
 * Code fenced inside a doc comment is highlighted as real code by the grammar,
 * so next to actual code it can be hard to tell apart. A TextMate grammar can
 * only pick a token's colour and font style, so the fence gets a left border
 * and its text is faded a little here instead: it reads as part of the comment
 * while keeping its highlighting. No background by default: a background competes
 * with the editor's own (current line, selection, hover) and looks different in
 * every theme. `luwu.docCodeBlockBackground` can still give it one.
 *
 * The fences found are the ones the grammars highlight:
 * - in a `--[[ ]]` / `--[=[ ]=]` block comment, from a line ending in
 *   ```luwu, ```luau or ```lua to the next line that starts with ```;
 * - across `---` doc lines, from `--- ```` (optionally with one of those
 *   languages) to the next `--- ```` line.
 */

const DOC_CODE_BLOCK_LANGUAGES = new Set(["luwu", "luau"]);

const BLOCK_COMMENT_FENCE_OPEN = /```(?:luwu|luau?)\s*$/;
const FENCE_CLOSE = /^\s*```/;
const DOC_LINE_FENCE_OPEN = /^\s*---\s*```(?:luwu|luau?)?\s*$/;
const DOC_LINE = /^\s*---/;
const DOC_LINE_FENCE_CLOSE = /^\s*---\s*```/;

/// Inclusive line range of one fenced block, fence lines included.
export interface LineRange {
  start: number;
  end: number;
}

type ScanState =
  | { kind: "code" }
  | { kind: "longString"; close: string }
  | { kind: "blockComment"; close: string; fenceStart?: number };

/**
 * Scans the code part of `line` from `from`, skipping strings, and returns
 * where a long string or block comment opens if one does and isn't closed on
 * the same line. A line comment ends the scan.
 */
function scanCode(
  line: string,
  from: number,
): { state: ScanState; openedAt: number } | undefined {
  let i = from;
  while (i < line.length) {
    const c = line[i];
    if (c === '"' || c === "'" || c === "`") {
      i++;
      while (i < line.length && line[i] !== c) {
        i += line[i] === "\\" ? 2 : 1;
      }
      i++;
      continue;
    }

    const isComment = line.startsWith("--", i);
    const bracketAt = isComment ? i + 2 : i;
    const longBracket = /^\[(=*)\[/.exec(line.slice(bracketAt));
    if (longBracket) {
      const close = `]${longBracket[1]}]`;
      const bodyAt = bracketAt + longBracket[0].length;
      const closeAt = line.indexOf(close, bodyAt);
      if (closeAt === -1) {
        const state: ScanState = isComment
          ? { kind: "blockComment", close }
          : { kind: "longString", close };
        return { state, openedAt: bodyAt };
      }
      i = closeAt + close.length;
      continue;
    }

    if (isComment) {
      return undefined;
    }
    i++;
  }
  return undefined;
}

export function findDocCodeBlocks(lines: readonly string[]): LineRange[] {
  const blocks: LineRange[] = [];
  let state: ScanState = { kind: "code" };
  // the `---` fence currently open, if any
  let docFenceStart: number | undefined;

  for (let lineNumber = 0; lineNumber < lines.length; lineNumber++) {
    const line = lines[lineNumber];

    if (docFenceStart !== undefined) {
      if (!DOC_LINE.test(line)) {
        // the doc comment ended without closing its fence
        blocks.push({ start: docFenceStart, end: lineNumber - 1 });
        docFenceStart = undefined;
      } else {
        if (DOC_LINE_FENCE_CLOSE.test(line)) {
          blocks.push({ start: docFenceStart, end: lineNumber });
          docFenceStart = undefined;
        }
        continue;
      }
    }

    // where on this line ordinary code resumes, if it does
    let codeFrom: number | undefined = 0;

    if (state.kind === "longString") {
      const closeAt = line.indexOf(state.close);
      codeFrom = closeAt === -1 ? undefined : closeAt + state.close.length;
      if (codeFrom !== undefined) {
        state = { kind: "code" };
      }
    } else if (state.kind === "blockComment") {
      const closeAt = line.indexOf(state.close);
      if (state.fenceStart !== undefined) {
        if (FENCE_CLOSE.test(line)) {
          blocks.push({ start: state.fenceStart, end: lineNumber });
          state.fenceStart = undefined;
        } else if (closeAt !== -1) {
          blocks.push({ start: state.fenceStart, end: lineNumber - 1 });
          state.fenceStart = undefined;
        }
      } else if (closeAt === -1 && BLOCK_COMMENT_FENCE_OPEN.test(line)) {
        state.fenceStart = lineNumber;
      }

      codeFrom = closeAt === -1 ? undefined : closeAt + state.close.length;
      if (codeFrom !== undefined) {
        state = { kind: "code" };
      }
    } else if (DOC_LINE_FENCE_OPEN.test(line)) {
      docFenceStart = lineNumber;
      continue;
    }

    if (codeFrom === undefined) {
      continue;
    }

    const opened = scanCode(line, codeFrom);
    if (opened) {
      state = opened.state;
      // `--[=[ ```luwu` opens a fence on the comment's first line
      if (
        state.kind === "blockComment" &&
        BLOCK_COMMENT_FENCE_OPEN.test(line.slice(opened.openedAt))
      ) {
        state.fenceStart = lineNumber;
      }
    }
  }

  if (docFenceStart !== undefined) {
    blocks.push({ start: docFenceStart, end: lines.length - 1 });
  } else if (state.kind === "blockComment" && state.fenceStart !== undefined) {
    blocks.push({ start: state.fenceStart, end: lines.length - 1 });
  }

  return blocks;
}

const SETTING = "documentation.codeBlockBackground";

function decorate(
  editor: vscode.TextEditor,
  decorationType: vscode.TextEditorDecorationType,
): void {
  const document = editor.document;
  if (
    !DOC_CODE_BLOCK_LANGUAGES.has(document.languageId) ||
    !getSettingOr<boolean>(SETTING, true, document)
  ) {
    editor.setDecorations(decorationType, []);
    return;
  }

  const lines: string[] = [];
  for (let i = 0; i < document.lineCount; i++) {
    lines.push(document.lineAt(i).text);
  }

  editor.setDecorations(
    decorationType,
    findDocCodeBlocks(lines).map(
      (block) =>
        new vscode.Range(
          block.start,
          0,
          block.end,
          document.lineAt(block.end).text.length,
        ),
    ),
  );
}

export function registerDocCodeBlocks(context: vscode.ExtensionContext): void {
  // the colours are contributed in package.json, so a theme or
  // `workbench.colorCustomizations` can change them
  const decorationType = vscode.window.createTextEditorDecorationType({
    isWholeLine: true,
    backgroundColor: new vscode.ThemeColor("luwu.docCodeBlockBackground"),
    borderStyle: "solid",
    borderWidth: "0 0 0 2px",
    borderColor: new vscode.ThemeColor("luwu.docCodeBlockBorder"),
    opacity: "0.8",
  });
  const pending = new Map<vscode.TextDocument, NodeJS.Timeout>();

  const decorateEditor = (editor: vscode.TextEditor) =>
    decorate(editor, decorationType);
  const decorateDocument = (document: vscode.TextDocument) => {
    for (const editor of vscode.window.visibleTextEditors) {
      if (editor.document === document) {
        decorateEditor(editor);
      }
    }
  };

  context.subscriptions.push(
    decorationType,
    vscode.window.onDidChangeVisibleTextEditors((editors) => {
      editors.forEach(decorateEditor);
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      const document = event.document;
      if (!DOC_CODE_BLOCK_LANGUAGES.has(document.languageId)) {
        return;
      }
      clearTimeout(pending.get(document));
      pending.set(
        document,
        setTimeout(() => {
          pending.delete(document);
          decorateDocument(document);
        }, 100),
      );
    }),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (settingChanged(event, SETTING)) {
        vscode.window.visibleTextEditors.forEach(decorateEditor);
      }
    }),
    { dispose: () => pending.forEach(clearTimeout) },
  );

  vscode.window.visibleTextEditors.forEach(decorateEditor);
}
