import * as vscode from "vscode";

import {
  explicitInheritLegacySettings,
  INHERIT_LEGACY_KEY,
  LEGACY_NAMESPACE,
  NAMESPACE,
} from "./settings";

const INHERIT_SETTING = `${NAMESPACE}.${INHERIT_LEGACY_KEY}`;

/// A `luau-lsp.*` key written in a settings file. The keys are a supported
/// spelling rather than a mistake, so they are not marked deprecated in
/// `package.json` -- that would squiggle every one of them, permanently. This
/// says the same thing once per file, and only while it is still news.
const LEGACY_KEY = /"luau-lsp\.[^"]+"\s*:/g;

/// What to say about the legacy keys in a file, given what the user has said
/// about `luwu.inheritLuauLspSettings`. Nothing at all once they've turned it
/// on: they know, and they've chosen it.
export function legacySettingsNotice(
  explicitInherit: boolean | undefined,
): { message: string; severity: vscode.DiagnosticSeverity } | undefined {
  if (explicitInherit === true) {
    return undefined;
  }

  if (explicitInherit === false) {
    return {
      message:
        `Luwu is ignoring the ${LEGACY_NAMESPACE}.* settings in this ` +
        `file, because ${INHERIT_SETTING} is off.`,
      severity: vscode.DiagnosticSeverity.Warning,
    };
  }

  return {
    message:
      `Luwu reads the ${LEGACY_NAMESPACE}.* settings in this file as its ` +
      `own. Set ${INHERIT_SETTING} to true to hide this notice, or to false ` +
      `to ignore them.`,
    severity: vscode.DiagnosticSeverity.Information,
  };
}

/// Where the first `luau-lsp.*` key in a settings file starts, so the notice
/// has somewhere to point.
export function firstLegacyKeyRange(
  text: string,
): { start: number; end: number } | undefined {
  LEGACY_KEY.lastIndex = 0;
  const match = LEGACY_KEY.exec(text);
  if (!match) {
    return undefined;
  }

  // the key itself, without the colon the pattern needed to find it
  const key = match[0].slice(0, match[0].indexOf(":")).trimEnd();
  return { start: match.index, end: match.index + key.length };
}

/// The files worth looking at: a `settings.json` from any scope (user,
/// workspace, folder, profile) or a `.code-workspace`, which carries the same
/// settings inline.
function isSettingsFile(document: vscode.TextDocument): boolean {
  const path = document.uri.path;
  return path.endsWith("/settings.json") || path.endsWith(".code-workspace");
}

export function registerLegacySettingsNotice(
  context: vscode.ExtensionContext,
): void {
  const diagnostics = vscode.languages.createDiagnosticCollection(
    "luwu-legacy-settings",
  );

  const refresh = (document: vscode.TextDocument) => {
    if (!isSettingsFile(document)) {
      return;
    }

    const notice = legacySettingsNotice(explicitInheritLegacySettings());
    const range = notice && firstLegacyKeyRange(document.getText());
    if (!notice || !range) {
      diagnostics.delete(document.uri);
      return;
    }

    const diagnostic = new vscode.Diagnostic(
      new vscode.Range(
        document.positionAt(range.start),
        document.positionAt(range.end),
      ),
      notice.message,
      notice.severity,
    );
    diagnostic.source = "Luwu";

    diagnostics.set(document.uri, [diagnostic]);
  };

  const refreshAll = () => vscode.workspace.textDocuments.forEach(refresh);

  refreshAll();

  context.subscriptions.push(
    diagnostics,
    vscode.workspace.onDidOpenTextDocument(refresh),
    vscode.workspace.onDidChangeTextDocument((event) =>
      refresh(event.document),
    ),
    vscode.workspace.onDidCloseTextDocument((document) =>
      diagnostics.delete(document.uri),
    ),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration(INHERIT_SETTING)) {
        refreshAll();
      }
    }),
  );
}
