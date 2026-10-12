import * as vscode from "vscode";

/**
 * Re-indents multi-line pastes so the block lands at the indentation of the
 * place it's pasted into, keeping the lines' indentation relative to each
 * other.
 *
 * Plain paste keeps the pasted lines' columns exactly as they were copied, so a
 * block copied from a different depth ends up too far in or out. Worse, a
 * selection that started at the first line's code rather than at the start of
 * its line leaves that line with no indentation at all while the rest keep
 * theirs. In that case we work out where the first line originally was from the
 * lines after it, using the language's indentation rules.
 */
const PASTE_REINDENT_LANGUAGES = ["luwu", "luau"];

/// Copied from `indentationRules` in language-configuration.json; a test makes
/// sure they stay the same.
export const INCREASE_INDENT_PATTERN = new RegExp(
  "(?:^((?!(\\-\\-)).)*((\\b(function|do|repeat)\\b((?!\\b(end|until)\\b).)*)|(\\{\\s*)|(\\b(then|else)\\b[;\\s]*))(\\-\\-.*)*$)|(?:^\\s*(?:export\\s+)?(?:declare\\s+(?:class|trait)\\s+type|class|trait)\\s+[A-Za-z_]((?!\\-\\-|\\bend\\b).)*(\\-\\-.*)?$)",
);
export const DECREASE_INDENT_PATTERN = new RegExp(
  "^\\s*((\\b(end|until)\\b)|(\\})|(\\))|(\\b(else)\\b[;\\s]*))",
);

/// `elseif` closes the branch before it as well as opening its own, but
/// `decreaseIndentPattern` only knows `else`.
function decreasesIndent(line: string): boolean {
  return DECREASE_INDENT_PATTERN.test(line) || /^\s*elseif\b/.test(line);
}

function increasesIndent(line: string): boolean {
  return INCREASE_INDENT_PATTERN.test(line);
}

export interface IndentOptions {
  tabSize: number;
  insertSpaces: boolean;
}

export interface PasteSite {
  /// Text on the line before the pasted range.
  before: string;
  /// Text on the line after the pasted range.
  after: string;
  /// The nearest non-blank line above the pasted range, if there is one.
  previousLine: string | undefined;
}

function leadingWhitespace(line: string): string {
  return /^[ \t]*/.exec(line)![0];
}

function isBlank(line: string): boolean {
  return /^\s*$/.test(line);
}

/// Width of leading whitespace in columns, with tabs going to the next stop.
function columns(whitespace: string, tabSize: number): number {
  let column = 0;
  for (const char of whitespace) {
    column = char === "\t" ? column + tabSize - (column % tabSize) : column + 1;
  }
  return column;
}

function indentString(width: number, options: IndentOptions): string {
  if (options.insertSpaces) {
    return " ".repeat(width);
  }
  return (
    "\t".repeat(Math.floor(width / options.tabSize)) +
    " ".repeat(width % options.tabSize)
  );
}

function gcd(a: number, b: number): number {
  return b === 0 ? a : gcd(b, a % b);
}

/**
 * The indent step the pasted lines were written with, from the differences
 * between their indents. Falls back to the editor's when the lines don't say,
 * or when the step looks like several of the editor's levels at once.
 */
function sourceIndentUnit(indents: number[], editorUnit: number): number {
  let step = 0;
  for (let i = 1; i < indents.length; i++) {
    step = gcd(step, Math.abs(indents[i] - indents[i - 1]));
  }

  if (step === 0 || step % editorUnit === 0) {
    return editorUnit;
  }
  return editorUnit % step === 0 ? step : editorUnit;
}

/**
 * Where the first pasted line was indented before it was copied without its
 * leading whitespace. Each later line votes: its indent minus the levels the
 * indentation rules put it below the first line. The most common vote wins,
 * so a line in a multi-line string or a misindented line doesn't throw it off.
 *
 * Returns undefined when there are no later lines to go by.
 */
export function inferFirstLineIndent(
  lines: string[],
  editorUnit: number,
  tabSize: number,
): number | undefined {
  const rest = lines.slice(1).filter((line) => !isBlank(line));
  if (rest.length === 0) {
    return undefined;
  }

  const indents = rest.map((line) => columns(leadingWhitespace(line), tabSize));
  const unit = sourceIndentUnit(indents, editorUnit);

  const votes = new Map<number, number>();
  let best: number | undefined = undefined;
  let depth = increasesIndent(lines[0]) ? 1 : 0;
  rest.forEach((line, i) => {
    if (decreasesIndent(line)) {
      depth--;
    }

    const vote = Math.max(0, indents[i] - depth * unit);
    const count = (votes.get(vote) ?? 0) + 1;
    votes.set(vote, count);
    // ties go to the earliest vote, being closest to the first line
    if (best === undefined || count > votes.get(best)!) {
      best = vote;
    }

    if (increasesIndent(line)) {
      depth++;
    }
  });

  return best;
}

/// The indent a new line gets after `previousLine`, if it starts with
/// `firstLine`.
function contextIndent(
  previousLine: string | undefined,
  firstLine: string,
  options: IndentOptions,
): number {
  if (previousLine === undefined) {
    return 0;
  }

  let width = columns(leadingWhitespace(previousLine), options.tabSize);
  if (increasesIndent(previousLine)) {
    width += options.tabSize;
  }
  if (decreasesIndent(firstLine)) {
    width -= options.tabSize;
  }
  return Math.max(0, width);
}

/**
 * The text to insert in place of `pasted`, or undefined if it should be pasted
 * as it is.
 */
export function reindentPaste(
  pasted: string,
  site: PasteSite,
  options: IndentOptions,
): string | undefined {
  const lines = pasted.split(/\r?\n/);
  if (lines.length < 2) {
    return undefined;
  }

  const { tabSize } = options;
  const first = lines[0];
  const firstIndent = leadingWhitespace(first);
  const inLeadingWhitespace = isBlank(site.before);

  // the column the first pasted line ends up at, which the rest are placed
  // relative to, and what to put in front of the first line to get it there
  let target: number;
  let firstLine: string;
  if (inLeadingWhitespace) {
    const here = columns(site.before, tabSize);
    // on a blank line, the cursor is often at the start of the line rather
    // than where code goes, so fall back to the indentation the code around
    // it calls for. In front of code, line up with that code
    const wanted = isBlank(site.after)
      ? contextIndent(site.previousLine, first, options)
      : here + columns(leadingWhitespace(site.after), tabSize);

    target = here;
    let prefix = "";
    const indent = indentString(wanted, options);
    if (wanted > here && indent.startsWith(site.before)) {
      target = wanted;
      prefix = indent.slice(site.before.length);
    }
    firstLine = prefix + first.slice(firstIndent.length);
  } else {
    // pasting after code: the first line continues it as it is
    target = columns(leadingWhitespace(site.before), tabSize);
    firstLine = first;
  }

  const base =
    firstIndent.length > 0
      ? columns(firstIndent, tabSize)
      : inferFirstLineIndent(lines, tabSize, tabSize);

  const out = [firstLine];
  for (const line of lines.slice(1)) {
    if (isBlank(line) || base === undefined) {
      out.push(isBlank(line) ? "" : line);
      continue;
    }

    const indent = columns(leadingWhitespace(line), tabSize);
    const width = Math.max(0, target + indent - base);
    out.push(indentString(width, options) + line.trimStart());
  }

  // pasted whole lines in front of code: give the line that the code is now on
  // back the indentation in front of the cursor, which went above the paste
  if (inLeadingWhitespace && !isBlank(site.after) && out.at(-1) === "") {
    out[out.length - 1] = site.before;
  }

  const result = out.join("\n");
  return result === lines.join("\n") ? undefined : result;
}

function indentOptions(document: vscode.TextDocument): IndentOptions {
  const editor = vscode.window.visibleTextEditors.find(
    (e) => e.document === document,
  );
  const config = vscode.workspace.getConfiguration("editor", document);
  const tabSize = editor?.options.tabSize ?? config.get("tabSize", 4);
  const insertSpaces =
    editor?.options.insertSpaces ?? config.get("insertSpaces", true);
  return {
    tabSize: typeof tabSize === "number" ? tabSize : 4,
    insertSpaces: insertSpaces !== false,
  };
}

function previousNonBlankLine(
  document: vscode.TextDocument,
  line: number,
): string | undefined {
  for (let i = line - 1; i >= 0; i--) {
    const text = document.lineAt(i).text;
    if (!isBlank(text)) {
      return text;
    }
  }
  return undefined;
}

const REINDENT_KIND = vscode.DocumentDropOrPasteEditKind.Text.append(
  "luwu",
  "reindent",
);

class PasteReindentProvider implements vscode.DocumentPasteEditProvider {
  async provideDocumentPasteEdits(
    document: vscode.TextDocument,
    ranges: readonly vscode.Range[],
    dataTransfer: vscode.DataTransfer,
  ): Promise<vscode.DocumentPasteEdit[] | undefined> {
    // multiple cursors get the clipboard split between them, which this
    // doesn't attempt
    if (ranges.length !== 1) {
      return undefined;
    }

    // a line copied with nothing selected pastes as a whole line above the
    // cursor; leave that to VSCode
    const editorData = await dataTransfer.get("vscode-editor-data")?.asString();
    if (editorData) {
      try {
        if (JSON.parse(editorData).isFromEmptySelection) {
          return undefined;
        }
      } catch {
        // not ours to understand; carry on
      }
    }

    const pasted = await dataTransfer.get("text/plain")?.asString();
    if (!pasted) {
      return undefined;
    }

    const range = ranges[0];
    const text = reindentPaste(
      pasted,
      {
        before: document
          .lineAt(range.start.line)
          .text.slice(0, range.start.character),
        after: document.lineAt(range.end.line).text.slice(range.end.character),
        previousLine: previousNonBlankLine(document, range.start.line),
      },
      indentOptions(document),
    );
    if (text === undefined) {
      return undefined;
    }

    return [
      new vscode.DocumentPasteEdit(text, "Paste reindented", REINDENT_KIND),
    ];
  }
}

export function registerPasteReindent(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.languages.registerDocumentPasteEditProvider(
      PASTE_REINDENT_LANGUAGES.map((language) => ({ language })),
      new PasteReindentProvider(),
      {
        providedPasteEditKinds: [REINDENT_KIND],
        pasteMimeTypes: ["text/plain"],
      },
    ),
  );
}
