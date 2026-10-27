import * as assert from "assert";
import * as vscode from "vscode";
import {
  badgeFor,
  badgeText,
  linesAfterChange,
  messagePreview,
} from "../../errorBadges";

const RANGE = new vscode.Range(0, 0, 0, 1);

function diagnostic(
  message: string,
  code: number | string,
  severity = vscode.DiagnosticSeverity.Error,
): vscode.Diagnostic {
  const result = new vscode.Diagnostic(RANGE, message, severity);
  result.source = "Luwu";
  result.code = code;
  return result;
}

suite("Error Badges Test Suite", () => {
  test("each kind of error gets its own badge", () => {
    const kinds = [
      "SyntaxError: Expected 'end'",
      "DocSyntaxError: Unknown name",
      "TypeError: Expected 'x' to be 'number'",
      "DocTypeError: Unknown type",
      "Typo: Did you mean 'print'?",
    ].map((message) => badgeFor(diagnostic(message, 1020)));
    assert.deepStrictEqual(kinds, [
      { kind: 0, label: "SyntaxError" },
      { kind: 0, label: "DocSyntaxError" },
      { kind: 1, label: "TypeError" },
      { kind: 1, label: "DocTypeError" },
      { kind: 2, label: "Typo" },
    ]);
  });

  test("a lint's badge is its name, whatever its severity", () => {
    const warning = diagnostic(
      "LocalUnused: Variable 'x' is never used",
      7,
      vscode.DiagnosticSeverity.Warning,
    );
    assert.deepStrictEqual(badgeFor(warning), {
      kind: 3,
      label: "LocalUnused",
    });
    warning.code = { value: 7, target: vscode.Uri.parse("https://luau.org") };
    assert.deepStrictEqual(badgeFor(warning), {
      kind: 3,
      label: "LocalUnused",
    });
  });

  test("notes and other sources get no badge", () => {
    const note = diagnostic(
      "Note: the error",
      "Note",
      vscode.DiagnosticSeverity.Information,
    );
    assert.strictEqual(badgeFor(note), undefined);
    const other = diagnostic("TypeError: from elsewhere", "1000");
    other.source = "Luau";
    assert.strictEqual(badgeFor(other), undefined);
  });

  test("the preview is the message's first paragraph on one line", () => {
    assert.strictEqual(
      messagePreview(
        "TypeError: 'Square' is missing a field to implement 'Shape':\n" +
          "  • 'name: string'\n\nHelp (traits):\n  • add the field",
        "TypeError",
      ),
      "'Square' is missing a field to implement 'Shape': 'name: string'",
    );
  });

  test("a long preview is cut off with an ellipsis", () => {
    const preview = messagePreview(`Typo: ${"a".repeat(100)}`, "Typo");
    assert.strictEqual(preview.length, 72);
    assert.ok(preview.endsWith("…"));
  });
  test("a badge names each label once and counts repeats", () => {
    assert.strictEqual(badgeText(["TypeError", "TypeError"]), "TypeError ×2");
    assert.strictEqual(
      badgeText(["LocalShadow", "LocalUnused"]),
      "LocalShadow · LocalUnused",
    );
    assert.strictEqual(
      badgeText(["LocalUnused", "LocalShadow", "LocalUnused"]),
      "LocalUnused ×2 · LocalShadow",
    );
  });

  test("lines being edited follow lines added and removed above them", () => {
    const sorted = (lines: Set<number>) => [...lines].sort((a, b) => a - b);
    const typed = linesAfterChange(new Set(), {
      range: new vscode.Range(4, 3, 4, 3),
      text: "x",
    });
    assert.deepStrictEqual(sorted(typed), [4]);

    const enter = linesAfterChange(typed, {
      range: new vscode.Range(4, 4, 4, 4),
      text: "\n    ",
    });
    assert.deepStrictEqual(sorted(enter), [4, 5]);

    const above = linesAfterChange(enter, {
      range: new vscode.Range(1, 0, 3, 0),
      text: "",
    });
    assert.deepStrictEqual(sorted(above), [1, 2, 3]);
  });
});
