import * as vscode from "vscode";
import {
  HoverParams,
  HoverRequest,
  LanguageClient,
  Hover as LspHover,
  ProvideHoverSignature,
} from "vscode-languageclient/node";
import { getSettingOr, settingChanged } from "./settings";

/// How much a hover shows; the server reads it from the `verbosity` field we
/// add to `textDocument/hover`.
export type HoverVerbosity = "low" | "medium" | "high";
const LEVELS: HoverVerbosity[] = ["low", "medium", "high"];

const INCREASE_COMMAND = "luwu.increaseHoverVerbosity";
const DECREASE_COMMAND = "luwu.decreaseHoverVerbosity";

type VerbosityHover = LspHover & {
  canIncreaseVerbosity?: boolean;
  canDecreaseVerbosity?: boolean;
};

/// Set by the hover's + and -, for the rest of the session, like TypeScript's.
/// Unset, hovers use `luwu.hover.verbosity`, and changing that setting unsets
/// it again.
let sessionVerbosity: HoverVerbosity | undefined;

const currentVerbosity = (scope?: vscode.Uri): HoverVerbosity =>
  sessionVerbosity ??
  getSettingOr<HoverVerbosity>("hover.verbosity", "medium", scope);

/// The `- Less · + More` row under a hover. Each link carries the hovered
/// position, so the hover can be shown again where it was.
const verbosityControls = (
  hover: VerbosityHover,
  document: vscode.TextDocument,
  position: vscode.Position,
): vscode.MarkdownString | undefined => {
  const args = encodeURIComponent(
    JSON.stringify([
      document.uri.toString(),
      position.line,
      position.character,
    ]),
  );

  const links = [];
  if (hover.canDecreaseVerbosity) {
    links.push(
      `[$(remove) Less](command:${DECREASE_COMMAND}?${args} ` +
        `"Decrease hover verbosity")`,
    );
  }
  if (hover.canIncreaseVerbosity) {
    links.push(
      `[$(add) More](command:${INCREASE_COMMAND}?${args} ` +
        `"Increase hover verbosity")`,
    );
  }

  if (links.length === 0) {
    return undefined;
  }

  const markdown = new vscode.MarkdownString(links.join(" · "), true);
  markdown.isTrusted = {
    enabledCommands: [INCREASE_COMMAND, DECREASE_COMMAND],
  };
  return markdown;
};

/// Sends hovers with the current verbosity, and adds the +/- row when the
/// server says another verbosity would show more or less.
export const hoverMiddleware =
  (getClient: () => LanguageClient | undefined) =>
  async (
    document: vscode.TextDocument,
    position: vscode.Position,
    token: vscode.CancellationToken,
    next: ProvideHoverSignature,
  ): Promise<vscode.Hover | undefined | null> => {
    const client = getClient();
    if (!client) {
      return next(document, position, token);
    }

    const params: HoverParams & { verbosity: HoverVerbosity } = {
      ...client.code2ProtocolConverter.asTextDocumentPositionParams(
        document,
        position,
      ),
      verbosity: currentVerbosity(document.uri),
    };

    let result: VerbosityHover | null;
    try {
      result = await client.sendRequest(HoverRequest.type, params, token);
    } catch (error) {
      return client.handleFailedRequest(HoverRequest.type, token, error, null);
    }

    if (!result || token.isCancellationRequested) {
      return null;
    }

    const hover = client.protocol2CodeConverter.asHover(result);
    const controls = verbosityControls(result, document, position);
    if (controls) {
      hover.contents.push(controls);
    }
    return hover;
  };

const changeVerbosity = async (
  step: number,
  uri?: string,
  line?: number,
  character?: number,
) => {
  const editor = vscode.window.activeTextEditor;
  const index = LEVELS.indexOf(currentVerbosity(editor?.document.uri));
  sessionVerbosity =
    LEVELS[Math.min(Math.max(index + step, 0), LEVELS.length - 1)];

  if (!editor) {
    return;
  }

  // From a hover's link: `showHover` shows the hover at the cursor, which is
  // not where a mouse hover was, so the cursor moves to the hovered word
  if (uri !== undefined && line !== undefined && character !== undefined) {
    if (editor.document.uri.toString() !== uri) {
      return;
    }

    const position = new vscode.Position(line, character);
    if (
      !editor.selection.isEmpty ||
      !editor.selection.active.isEqual(position)
    ) {
      editor.selection = new vscode.Selection(position, position);
    }
  }

  await vscode.commands.executeCommand("editor.action.showHover");
};

export const registerHoverVerbosity = (): vscode.Disposable[] => [
  vscode.commands.registerCommand(INCREASE_COMMAND, (...args) =>
    changeVerbosity(1, ...args),
  ),
  vscode.commands.registerCommand(DECREASE_COMMAND, (...args) =>
    changeVerbosity(-1, ...args),
  ),
  vscode.workspace.onDidChangeConfiguration((e) => {
    if (settingChanged(e, "hover.verbosity")) {
      sessionVerbosity = undefined;
    }
  }),
];
