/* eslint-disable @typescript-eslint/naming-convention */
import * as vscode from "vscode";
import {
  LanguageClient,
  Position,
  Range,
  RequestType,
  TextDocumentIdentifier,
} from "vscode-languageclient/node";
import * as utils from "./utils";

export type MatchingKeywordParams = {
  textDocument: TextDocumentIdentifier;
  position: Position;
};

export const MatchingKeywordRequest = new RequestType<
  MatchingKeywordParams,
  Range | null,
  void
>("luwu-lsp/matchingKeyword");

const jumpToMatchingKeyword = async (client: LanguageClient) => {
  const editor = vscode.window.activeTextEditor;
  if (!editor || !utils.isSourceDocument(editor.document)) {
    return;
  }

  const range = await client.sendRequest(MatchingKeywordRequest, {
    textDocument: client.code2ProtocolConverter.asTextDocumentIdentifier(
      editor.document,
    ),
    position: client.code2ProtocolConverter.asPosition(editor.selection.active),
  });

  // The cursor isn't on a keyword that has a match, so fall back to the
  // built-in bracket jump, which this command is bound over the top of.
  if (!range) {
    await vscode.commands.executeCommand("editor.action.jumpToBracket");
    return;
  }

  const position = new vscode.Position(range.start.line, range.start.character);
  editor.selection = new vscode.Selection(position, position);
  editor.revealRange(
    new vscode.Range(position, position),
    vscode.TextEditorRevealType.Default,
  );
};

export const registerMatchingKeyword = (
  _context: vscode.ExtensionContext,
  client: LanguageClient,
): vscode.Disposable[] => {
  return [
    vscode.commands.registerCommand("luwu.jumpToMatchingKeyword", () =>
      jumpToMatchingKeyword(client),
    ),
  ];
};
