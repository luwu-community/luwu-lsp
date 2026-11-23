/* eslint-disable @typescript-eslint/naming-convention */
import * as vscode from "vscode";
import {
  LanguageClient,
  RequestType,
  TextDocumentIdentifier,
} from "vscode-languageclient/node";
import * as utils from "./utils";

export const BYTECODE_SCHEME = "luau-bytecode";
export const COMPILER_REMARKS_SCHEME = "luau-remarks";
export const CODEGEN_SCHEME = "luau-codegen";

enum OptimizationLevel {
  None = 0,
  O1 = 1,
  O2 = 2,
}

export type BytecodeParams = {
  textDocument: TextDocumentIdentifier;
  optimizationLevel: OptimizationLevel;
};

export const BytecodeRequest = new RequestType<BytecodeParams, string, void>(
  "luau-lsp/bytecode",
);

export type CompilerRemarksParams = {
  textDocument: TextDocumentIdentifier;
  optimizationLevel: OptimizationLevel;
};

export const ComputeCompilerRemarksRequest = new RequestType<
  CompilerRemarksParams,
  string,
  void
>("luau-lsp/compilerRemarks");

export type CodeGenTarget =
  | "host"
  | "a64"
  | "a64_nofeatures"
  | "x64_windows"
  | "x64_systemv";

export type CodeGenParams = {
  textDocument: TextDocumentIdentifier;
  optimizationLevel: OptimizationLevel;
  codeGenTarget: CodeGenTarget;
};

export const ComputeCodeGenRequest = new RequestType<
  CodeGenParams,
  string,
  void
>("luau-lsp/codeGen");

export const getOptimizationLevel = async (): Promise<OptimizationLevel> => {
  const optimizationLevel = await vscode.window.showQuickPick(
    [
      {
        label: "O2",
        detail:
          "Includes optimizations that harm debuggability such as inlining",
        picked: true,
      },

      {
        label: "O1",
        detail:
          "Baseline optimization level that doesn't prevent debuggability",
      },
      {
        label: "None",
        detail: "No optimization",
      },
    ],
    {
      title: "Select Optimization Level",
      placeHolder: "Select optimization level",
    },
  );

  return optimizationLevel?.label === "None"
    ? OptimizationLevel.None
    : optimizationLevel?.label === "O1"
      ? OptimizationLevel.O1
      : optimizationLevel?.label === "O2"
        ? OptimizationLevel.O2
        : OptimizationLevel.O1;
};

export const getCodeGenTarget = async (): Promise<CodeGenTarget> => {
  const codeGenTargetSelection = await vscode.window.showQuickPick(
    [
      {
        label: "host",
        detail: "Host target",
        picked: true,
      },

      {
        label: "a64",
        detail: "Arm64",
      },
      {
        label: "a64_nofeatures",
        detail: "Arm64 (No Features)",
      },
      {
        label: "x64_windows",
        detail: "x64 Windows",
      },
      {
        label: "x64_systemv",
        detail: "x64 SystemV",
      },
    ],
    {
      title: "Select CodeGen target",
      placeHolder: "Select CodeGen target",
    },
  );

  return (codeGenTargetSelection?.label as CodeGenTarget) ?? "host";
};

let optimizationLevel = OptimizationLevel.O2;
let codeGenTarget: CodeGenTarget = "host";

/**
 * How long typing has to stop before an open view recompiles. A compile holds
 * up every request behind it in the server's queue.
 */
const EDIT_SETTLE_MS = 500;

const isLuauDocument = (document: vscode.TextDocument) => {
  return utils.isSourceDocument(document) && document.uri.scheme === "file";
};

const isLuauEditor = (editor: vscode.TextEditor) => {
  return isLuauDocument(editor.document);
};

const activeLuauEditor = () => {
  const activeEditor = vscode.window.activeTextEditor;
  return activeEditor && isLuauEditor(activeEditor) ? activeEditor : undefined;
};

const getBytecodeInfo = (
  context: vscode.ExtensionContext,
  client: LanguageClient,
  command: string,
  scheme: string,
  documentName: string,
  /** The language the view is highlighted as */
  language: string,
  requestType: RequestType<
    CompilerRemarksParams | BytecodeParams | CodeGenParams,
    string,
    void
  >,
) => {
  const tdcp = new (class implements vscode.TextDocumentContentProvider {
    readonly uri = vscode.Uri.parse(`${scheme}://bytecode/${documentName}`);
    readonly eventEmitter = new vscode.EventEmitter<vscode.Uri>();
    private settleTimer: ReturnType<typeof setTimeout> | undefined;

    constructor() {
      vscode.workspace.onDidChangeTextDocument(
        this.onDidChangeTextDocument,
        this,
        context.subscriptions,
      );
      vscode.window.onDidChangeActiveTextEditor(
        this.onDidChangeActiveTextEditor,
        this,
        context.subscriptions,
      );
      context.subscriptions.push({
        dispose: () => clearTimeout(this.settleTimer),
      });
    }

    /** Whether this view is showing in an editor */
    private isVisible(): boolean {
      return vscode.window.visibleTextEditors.some(
        (editor) => editor.document.uri.toString() === this.uri.toString(),
      );
    }

    private onDidChangeTextDocument(event: vscode.TextDocumentChangeEvent) {
      if (
        event.contentChanges.length === 0 ||
        !isLuauDocument(event.document) ||
        !this.isVisible()
      ) {
        return;
      }

      clearTimeout(this.settleTimer);
      this.settleTimer = setTimeout(() => {
        this.settleTimer = undefined;
        this.eventEmitter.fire(this.uri);
      }, EDIT_SETTLE_MS);
    }
    private onDidChangeActiveTextEditor(editor: vscode.TextEditor | undefined) {
      if (editor && isLuauEditor(editor) && this.isVisible()) {
        this.eventEmitter.fire(this.uri);
      }
    }

    /** Gives the view its language */
    async applyLanguage(): Promise<void> {
      const view = vscode.workspace.textDocuments.find(
        (document) => document.uri.toString() === this.uri.toString(),
      );
      if (view && view.languageId !== language) {
        await vscode.languages.setTextDocumentLanguage(view, language);
      }
    }

    async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
      const editor = activeLuauEditor();
      if (!editor) {
        return "";
      }

      const params = {
        textDocument: client.code2ProtocolConverter.asTextDocumentIdentifier(
          editor.document,
        ),
        optimizationLevel,
        codeGenTarget,
      };

      return client.sendRequest(requestType, params);
    }

    get onDidChange(): vscode.Event<vscode.Uri> {
      return this.eventEmitter.event;
    }
  })();

  return [
    vscode.workspace.registerTextDocumentContentProvider(scheme, tdcp),
    vscode.commands.registerTextEditorCommand(
      command,
      async (textEditor: vscode.TextEditor) => {
        if (isLuauEditor(textEditor)) {
          optimizationLevel = await getOptimizationLevel();

          if (command === "luwu.computeCodeGen") {
            codeGenTarget = await getCodeGenTarget();
          }

          const doc = await vscode.workspace.openTextDocument(tdcp.uri);
          await tdcp.applyLanguage();
          tdcp.eventEmitter.fire(tdcp.uri);
          await vscode.window.showTextDocument(doc, {
            viewColumn: vscode.ViewColumn.Two,
            preserveFocus: true,
          });
        }
      },
    ),
  ];
};

export const registerComputeBytecode = (
  context: vscode.ExtensionContext,
  client: LanguageClient,
): vscode.Disposable[] => {
  return getBytecodeInfo(
    context,
    client,
    "luwu.computeBytecode",
    BYTECODE_SCHEME,
    "bytecode",
    "luwu-bytecode",
    BytecodeRequest,
  );
};

export const registerComputeCompilerRemarks = (
  context: vscode.ExtensionContext,
  client: LanguageClient,
): vscode.Disposable[] => {
  return getBytecodeInfo(
    context,
    client,
    "luwu.computeCompilerRemarks",
    COMPILER_REMARKS_SCHEME,
    // the source with a comment for each remark, highlighted as Luwu (a
    // superset of Luau) with the remarks marked
    "compiler-remarks.luwu",
    "luwu-remarks",
    ComputeCompilerRemarksRequest,
  );
};

export const registerComputeCodeGen = (
  context: vscode.ExtensionContext,
  client: LanguageClient,
): vscode.Disposable[] => {
  return getBytecodeInfo(
    context,
    client,
    "luwu.computeCodeGen",
    CODEGEN_SCHEME,
    "codeGen",
    "plaintext",
    ComputeCodeGenRequest,
  );
};
