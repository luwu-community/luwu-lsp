import * as vscode from "vscode";
import { NAMESPACE, getSettingOr } from "./settings";

const TOGGLE_COMMAND = "luwu.toggleDiagnosticHelp";

/// Flips `luwu.diagnostics.showHelp`. The server reads it like any other
/// setting, so other editors set it in their own configuration; this is only a
/// shortcut. Diagnostics are computed again with or without their help.
export const registerDiagnosticHelp = (): vscode.Disposable[] => [
  vscode.commands.registerCommand(TOGGLE_COMMAND, async () => {
    const showHelp = getSettingOr<boolean>("diagnostics.showHelp", true);
    await vscode.workspace
      .getConfiguration(NAMESPACE)
      .update(
        "diagnostics.showHelp",
        !showHelp,
        vscode.ConfigurationTarget.Global,
      );
  }),
];
