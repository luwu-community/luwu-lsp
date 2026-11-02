import * as vscode from "vscode";
import * as path from "path";
import * as os from "os";

/// Every language ID we serve: Luwu's own `.luwu`, our `.luau`, VSCode's
/// built-in `lua`, and `.luwon` data files (Luwu configuration files are
/// `luwon-config`). Note this is what the extension *understands* -- whether the
/// language server actually attaches to a given one is
/// `luwu.fileExtensions`.
export const SOURCE_LANGUAGE_IDS: ReadonlySet<string> = new Set([
  "luwu",
  "luau",
  "lua",
  "luwon",
  "luwon-config",
]);

export const isSourceDocument = (document: vscode.TextDocument): boolean => {
  return SOURCE_LANGUAGE_IDS.has(document.languageId);
};

export const basenameUri = (uri: vscode.Uri): string => {
  return path.basename(uri.fsPath);
};

export const exists = (uri: vscode.Uri): Thenable<boolean> => {
  return vscode.workspace.fs.stat(uri).then(
    () => true,
    () => false,
  );
};

export const resolveUri = (uri: vscode.Uri, ...paths: string[]): vscode.Uri => {
  return vscode.Uri.file(path.resolve(uri.fsPath, ...paths));
};

/// Expand ~ at the start of the path
export const resolvePath = (mainPath: string): string => {
  if (mainPath.startsWith("~/") || mainPath.startsWith("~\\")) {
    return path.join(os.homedir(), mainPath.slice(2));
  }
  return mainPath;
};
