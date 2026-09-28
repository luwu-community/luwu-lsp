import * as assert from "assert";
import * as vscode from "vscode";
import {
  firstLegacyKeyRange,
  legacySettingsNotice,
} from "../../legacySettingsNotice";

suite("Legacy Settings Notice Test Suite", () => {
  suite("legacySettingsNotice", () => {
    test("says nothing once inheritance is explicitly on", () => {
      assert.strictEqual(legacySettingsNotice(true), undefined);
    });

    test("points at the setting when inheritance is left at its default", () => {
      const notice = legacySettingsNotice(undefined);
      assert.strictEqual(
        notice?.severity,
        vscode.DiagnosticSeverity.Information,
      );
      assert.ok(notice.message.includes("luwu.inheritLuauLspSettings"));
    });

    test("warns that the settings are ignored when inheritance is off", () => {
      const notice = legacySettingsNotice(false);
      assert.strictEqual(notice?.severity, vscode.DiagnosticSeverity.Warning);
      assert.ok(notice.message.includes("ignoring"));
    });
  });

  suite("firstLegacyKeyRange", () => {
    test("finds nothing in a file without legacy keys", () => {
      assert.strictEqual(
        firstLegacyKeyRange('{\n  "luwu.platform.type": "roblox"\n}'),
        undefined,
      );
    });

    test("covers the first legacy key only", () => {
      const text =
        '{\n  "luwu.platform.type": "roblox",\n' +
        '  "luau-lsp.fflags.enableByDefault": true,\n' +
        '  "luau-lsp.platform.type": "roblox"\n}';
      const range = firstLegacyKeyRange(text);
      assert.ok(range);
      assert.strictEqual(
        text.slice(range.start, range.end),
        '"luau-lsp.fflags.enableByDefault"',
      );
    });

    test("is not confused by a repeat call", () => {
      const text = '{\n  "luau-lsp.platform.type": "roblox"\n}';
      assert.deepStrictEqual(
        firstLegacyKeyRange(text),
        firstLegacyKeyRange(text),
      );
    });
  });
});
