import * as vscode from "vscode";
import { getSettingOr, settingChanged } from "./settings";

/**
 * VSCode colours a diagnostic only by its severity, so every Luwu error gets
 * the same red squiggle. The kind of diagnostic (SyntaxError, TypeError, Typo
 * or a lint's name) is shown as a badge after its line instead, in one of
 * Nyla's colours. An error's badge is followed by the start of its message.
 * A lint's name says what's wrong well enough on its own, so its badge isn't.
 * A note (`Expected 'score: number' because of this`) explains an error on
 * another line, so its `Note` badge is a fainter copy of that error's colour
 * and its preview names the error's line.
 *
 * Two decorations at the same position have no dependable order, so a line
 * gets one badge: for its diagnostics of the first kind in KINDS. It names
 * each of their labels once. The others on that line are still in the hover
 * and the Problems panel.
 *
 * Code being typed is usually broken for a moment, and its errors come and go
 * with each keystroke. So the lines an edit touches, and the line on either
 * side, get no badge until typing has stopped for EDIT_SETTLE_MS.
 */

const SETTING = "diagnostics.errorBadges";
const PREVIEW_SETTING = "diagnostics.errorBadgePreview";

/// How much of a message the preview shows before cutting it off with `…`.
const PREVIEW_LENGTH = 72;

/// How long after the last edit the lines being edited get their badges back.
const EDIT_SETTLE_MS = 500;

/// The labels each badge colour is for, and the colours contributed in
/// package.json, so a theme or `workbench.colorCustomizations` can change them.
/// A kind without labels is for lints, whatever their name.
interface BadgeKind {
  labels?: string[];
  preview: boolean;
  background: string;
  foreground: string;
}

const KINDS: BadgeKind[] = [
  {
    labels: ["SyntaxError", "DocSyntaxError"],
    preview: true,
    background: "luwu.syntaxErrorBadgeBackground",
    foreground: "luwu.syntaxErrorBadgeForeground",
  },
  {
    labels: ["TypeError", "DocTypeError"],
    preview: true,
    background: "luwu.typeErrorBadgeBackground",
    foreground: "luwu.typeErrorBadgeForeground",
  },
  {
    labels: ["Typo"],
    preview: true,
    background: "luwu.typoBadgeBackground",
    foreground: "luwu.typoBadgeForeground",
  },
  {
    preview: false,
    background: "luwu.lintBadgeBackground",
    foreground: "luwu.lintBadgeForeground",
  },
];

const LINT_KIND = KINDS.findIndex((kind) => kind.labels === undefined);

/// Which entry of KINDS a diagnostic's badge is, and the label its message
/// starts with (`TypeError: ...`, `LocalUnused: ...`).
export interface Badge {
  kind: number;
  label: string;
}

/// Lints have codes below this one and type errors have codes from it up
/// (`TypeError::minCode()` in Luwu).
const FIRST_TYPE_ERROR_CODE = 1000;

/// The badge for a diagnostic, or undefined when it gets none. A lint's label
/// can be any lint's name, so a lint is told apart by its code.
export function badgeFor(diagnostic: vscode.Diagnostic): Badge | undefined {
  if (diagnostic.source !== "Luwu") {
    return undefined;
  }
  const colon = diagnostic.message.indexOf(": ");
  if (colon === -1) {
    return undefined;
  }
  const label = diagnostic.message.slice(0, colon);

  const kind = KINDS.findIndex((k) => k.labels?.includes(label));
  if (kind !== -1) {
    return { kind, label };
  }

  const code = codeOf(diagnostic);
  return typeof code === "number" && code < FIRST_TYPE_ERROR_CODE
    ? { kind: LINT_KIND, label }
    : undefined;
}

function codeOf(diagnostic: vscode.Diagnostic): string | number | undefined {
  return typeof diagnostic.code === "object"
    ? diagnostic.code.value
    : diagnostic.code;
}

const NOTE_LABEL = "Note";

function isNote(diagnostic: vscode.Diagnostic): boolean {
  return diagnostic.source === "Luwu" && codeOf(diagnostic) === NOTE_LABEL;
}

/// The error a note explains: the badged diagnostic starting where the note's
/// related location does.
function noteTarget(
  note: vscode.Diagnostic,
  diagnostics: readonly vscode.Diagnostic[],
): { line: number; badge: Badge } | undefined {
  const related = note.relatedInformation?.[0]?.location.range.start;
  if (related === undefined) {
    return undefined;
  }
  for (const diagnostic of diagnostics) {
    const badge = badgeFor(diagnostic);
    if (badge !== undefined && diagnostic.range.start.isEqual(related)) {
      return { line: related.line, badge };
    }
  }
  return undefined;
}

/// The message's first paragraph without its label, on one line and cut to
/// PREVIEW_LENGTH. A first line often ends in a colon with the point on the
/// lines under it (`... to implement 'Shape':` then `• 'name: string'`), so
/// the whole paragraph is joined. The `Help (...)` paragraphs after it aren't.
export function messagePreview(message: string, label: string): string {
  const text = message
    .slice(label.length + 2)
    .split("\n\n", 1)[0]
    .split("\n")
    .map((line) => line.trim().replace(/^• /, ""))
    .filter((line) => line !== "")
    .join(" ");
  return text.length > PREVIEW_LENGTH
    ? `${text.slice(0, PREVIEW_LENGTH - 1).trimEnd()}…`
    : text;
}

/// A badge's text: each label once, in the order given, with `×n` after one
/// that appears n times (`TypeError ×2`, `LocalShadow · LocalUnused`).
export function badgeText(labels: readonly string[]): string {
  const counts = new Map<string, number>();
  for (const label of labels) {
    counts.set(label, (counts.get(label) ?? 0) + 1);
  }
  return [...counts]
    .map(([label, count]) => (count > 1 ? `${label} ×${count}` : label))
    .join(" · ");
}

/// The lines in `lines` moved to where they are after `change`: a line below
/// the replaced range moves by the number of lines added or removed, and one
/// inside it moves to its first line. The lines the inserted text covers are
/// added.
export function linesAfterChange(
  lines: ReadonlySet<number>,
  change: { range: vscode.Range; text: string },
): Set<number> {
  const start = change.range.start.line;
  const end = change.range.end.line;
  const inserted = change.text.split("\n").length - 1;
  const delta = inserted - (end - start);
  const moved = new Set<number>();
  for (const line of lines) {
    if (line > end) {
      moved.add(line + delta);
    } else if (line > start) {
      moved.add(start);
    } else {
      moved.add(line);
    }
  }
  for (let line = start; line <= start + inserted; line++) {
    moved.add(line);
  }
  return moved;
}

/// A line's diagnostics of the kind its badge is for. Notes are only badged on
/// lines with no error or lint.
interface LineBadges {
  kind: number;
  diagnostics: vscode.Diagnostic[];
  notes: boolean;
}

/// One decoration type per entry of KINDS for the badges and another for the
/// note badges, and one for every preview. Each is text after the end of the
/// line: text before it would sit inside the range of a diagnostic that ends
/// the line, and the diagnostic's squiggle would run under it.
interface DecorationTypes {
  badges: vscode.TextEditorDecorationType[];
  noteBadges: vscode.TextEditorDecorationType[];
  preview: vscode.TextEditorDecorationType;
}

function decorate(
  editor: vscode.TextEditor,
  types: DecorationTypes,
  editing: ReadonlySet<number>,
): void {
  const document = editor.document;
  const enabled = getSettingOr<boolean>(SETTING, true, document);
  const preview = getSettingOr<boolean>(PREVIEW_SETTING, true, document);

  const lines = new Map<number, LineBadges>();
  const diagnostics = enabled
    ? vscode.languages.getDiagnostics(document.uri)
    : [];
  for (const diagnostic of diagnostics) {
    const badge = badgeFor(diagnostic);
    if (badge === undefined) {
      continue;
    }
    const line = diagnostic.range.start.line;
    const existing = lines.get(line);
    if (existing === undefined || badge.kind < existing.kind) {
      lines.set(line, {
        kind: badge.kind,
        diagnostics: [diagnostic],
        notes: false,
      });
    } else if (badge.kind === existing.kind) {
      existing.diagnostics.push(diagnostic);
    }
  }
  // the line of the error each note explains
  const noteTargets = new Map<vscode.Diagnostic, number>();
  for (const note of diagnostics) {
    const target = isNote(note) ? noteTarget(note, diagnostics) : undefined;
    const line = note.range.start.line;
    const existing = lines.get(line);
    if (target === undefined || (existing !== undefined && !existing.notes)) {
      continue;
    }
    noteTargets.set(note, target.line);
    if (existing === undefined) {
      lines.set(line, {
        kind: target.badge.kind,
        diagnostics: [note],
        notes: true,
      });
    } else {
      existing.diagnostics.push(note);
    }
  }

  const badgeOptions = KINDS.map((): vscode.DecorationOptions[] => []);
  const noteBadgeOptions = KINDS.map((): vscode.DecorationOptions[] => []);
  const previewOptions: vscode.DecorationOptions[] = [];
  for (const [line, badges] of lines) {
    if (
      line >= document.lineCount ||
      editing.has(line - 1) ||
      editing.has(line) ||
      editing.has(line + 1)
    ) {
      continue;
    }
    badges.diagnostics.sort((a, b) => a.range.start.compareTo(b.range.start));
    const first = badges.diagnostics[0];
    const label = badges.notes ? NOTE_LABEL : (badgeFor(first)?.label ?? "");
    const labels = badges.notes
      ? badges.diagnostics.map(() => NOTE_LABEL)
      : badges.diagnostics.map(
          (diagnostic) => badgeFor(diagnostic)?.label ?? "",
        );
    const end = document.lineAt(line).range.end;
    const range = new vscode.Range(end, end);
    (badges.notes ? noteBadgeOptions : badgeOptions)[badges.kind].push({
      range,
      renderOptions: {
        after: { contentText: badgeText(labels) },
      },
    });
    if (preview && (badges.notes || KINDS[badges.kind].preview)) {
      const text = messagePreview(first.message, label);
      const target = noteTargets.get(first);
      previewOptions.push({
        range,
        renderOptions: {
          after: {
            contentText:
              target === undefined ? text : `${text} (line ${target + 1})`,
          },
        },
      });
    }
  }
  types.badges.forEach((type, kind) =>
    editor.setDecorations(type, badgeOptions[kind]),
  );
  types.noteBadges.forEach((type, kind) =>
    editor.setDecorations(type, noteBadgeOptions[kind]),
  );
  editor.setDecorations(types.preview, previewOptions);
}

/// Makes the badge and preview decoration types. The badge and the preview
/// are both drawn after the end of the line, and VSCode orders the two by
/// their types' `key`s compared as strings (`TextEditorDecorationType14`
/// before `TextEditorDecorationType6`), not by when they were made. While a
/// badge's key wouldn't sort before the preview's, the types are made again:
/// keys count up, so this ends once they all have the same number of digits.
function createDecorationTypes(): DecorationTypes {
  // The decoration API has no padding, radius, size or opacity, but CSS
  // appended to `textDecoration` is passed through as is. The badge is a
  // little smaller and fainter than the code and the preview fainter still,
  // so neither competes with the line they're on. A note's badge is fainter
  // still, so it doesn't read as an error of its own.
  const badgeType = (kind: BadgeKind, opacity: number) =>
    vscode.window.createTextEditorDecorationType({
      after: {
        backgroundColor: new vscode.ThemeColor(kind.background),
        color: new vscode.ThemeColor(kind.foreground),
        margin: "0 0 0 3ch",
        textDecoration:
          "none; padding: 0 0.5ch; border-radius: 3px; font-size: 0.85em; " +
          `opacity: ${opacity}`,
      },
    });
  for (;;) {
    const types = {
      badges: KINDS.map((kind) => badgeType(kind, 0.8)),
      noteBadges: KINDS.map((kind) => badgeType(kind, 0.45)),
      preview: vscode.window.createTextEditorDecorationType({
        after: {
          color: new vscode.ThemeColor("luwu.errorBadgePreviewForeground"),
          fontStyle: "italic",
          margin: "0 0 0 1ch",
          textDecoration: "none; font-size: 0.9em; opacity: 0.7",
        },
      }),
    };
    const badgeTypes = [...types.badges, ...types.noteBadges];
    if (badgeTypes.every((type) => type.key < types.preview.key)) {
      return types;
    }
    [...badgeTypes, types.preview].forEach((type) => type.dispose());
  }
}

export function registerErrorBadges(context: vscode.ExtensionContext): void {
  const { badges, noteBadges, preview } = createDecorationTypes();

  // The lines being edited in each document (by URI), and the timer that
  // gives them their badges back
  const editing = new Map<string, Set<number>>();
  const settleTimers = new Map<string, ReturnType<typeof setTimeout>>();

  const decorateEditor = (editor: vscode.TextEditor) =>
    decorate(
      editor,
      { badges, noteBadges, preview },
      editing.get(editor.document.uri.toString()) ?? new Set(),
    );

  const decorateDocument = (uri: string) => {
    for (const editor of vscode.window.visibleTextEditors) {
      if (editor.document.uri.toString() === uri) {
        decorateEditor(editor);
      }
    }
  };

  context.subscriptions.push(
    ...badges,
    ...noteBadges,
    preview,
    vscode.languages.onDidChangeDiagnostics((event) => {
      const changed = new Set(event.uris.map((uri) => uri.toString()));
      for (const editor of vscode.window.visibleTextEditors) {
        if (changed.has(editor.document.uri.toString())) {
          decorateEditor(editor);
        }
      }
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (event.contentChanges.length === 0) {
        return;
      }
      const uri = event.document.uri.toString();
      // Every change's range is in the document from before the event, so
      // they're applied from the bottom up
      const changes = [...event.contentChanges].sort((a, b) =>
        b.range.start.compareTo(a.range.start),
      );
      let lines: Set<number> = editing.get(uri) ?? new Set();
      for (const change of changes) {
        lines = linesAfterChange(lines, change);
      }
      editing.set(uri, lines);
      decorateDocument(uri);

      clearTimeout(settleTimers.get(uri));
      settleTimers.set(
        uri,
        setTimeout(() => {
          settleTimers.delete(uri);
          editing.delete(uri);
          decorateDocument(uri);
        }, EDIT_SETTLE_MS),
      );
    }),
    { dispose: () => settleTimers.forEach((timer) => clearTimeout(timer)) },
    vscode.window.onDidChangeVisibleTextEditors((editors) => {
      editors.forEach(decorateEditor);
    }),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (
        settingChanged(event, SETTING) ||
        settingChanged(event, PREVIEW_SETTING)
      ) {
        vscode.window.visibleTextEditors.forEach(decorateEditor);
      }
    }),
  );

  vscode.window.visibleTextEditors.forEach(decorateEditor);
}
