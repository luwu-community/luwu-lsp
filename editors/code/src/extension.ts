import * as vscode from "vscode";
import * as os from "os";
import { fetch } from "undici";
import {
  CloseAction,
  CloseHandlerResult,
  ErrorAction,
  ErrorHandler,
  ErrorHandlerResult,
  Executable,
  LanguageClient,
  LanguageClientOptions,
  Message,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

import {
  registerComputeBytecode,
  registerComputeCodeGen,
  registerComputeCompilerRemarks,
} from "./bytecode";

import { onTypeFormattingMiddleware } from "./onTypeFormattingMiddleware";

import {
  effectiveFileExtensions,
  isConflictActive,
  registerConflictingExtensionCheck,
} from "./conflictingExtension";

import {
  buildServerConfiguration,
  getSetting,
  getSettingOr,
  LEGACY_NAMESPACE,
  NAMESPACE,
  settingChanged,
  settingKeys,
} from "./settings";

import { registerLongBracketAutoClose } from "./longBracketAutoClose";

import { registerRequireGraph } from "./requireGraph";

import { registerViewInternalSource } from "./internalSource";

import * as roblox from "./roblox";
import * as utils from "./utils";
import {
  anyFileIsMissing,
  downloadExternalFiles,
  DownloadFileDefinition,
  isExternalFile,
  outputLocationForDefinition,
  outputLocationForDocumentation,
  shouldFetchDefinitions,
} from "./definitions";

export type PlatformContext = { client: LanguageClient | undefined };
export type AddArgCallback = (
  argument: string,
  mode?: "All" | "Prod" | "Debug",
) => void;

let client: LanguageClient | undefined = undefined;
let platformContext: PlatformContext = { client: undefined };
const clientDisposables: vscode.Disposable[] = [];

const CURRENT_FFLAGS =
  "https://clientsettingscdn.roblox.com/v1/settings/application?applicationName=PCStudioApp";
const FFLAG_KINDS = ["FFlag", "FInt", "DFFlag", "DFInt"];

type FFlags = Record<string, string>;
type FFlagsEndpoint = { applicationSettings: FFlags };

const getFFlags = async () => {
  return vscode.window.withProgress(
    {
      location: vscode.ProgressLocation.Window,
      title: "Luwu: Fetching FFlags",
      cancellable: false,
    },
    () =>
      fetch(CURRENT_FFLAGS)
        .then((r) => r.json() as Promise<FFlagsEndpoint>)
        .then((r) => r.applicationSettings),
  );
};

const isAlphanumericUnderscore = (str: string) => {
  return /^[a-zA-Z0-9_]+$/.test(str);
};

const isCrashReportingEnabled = () => {
  return getSettingOr("server.crashReporting.enabled", false);
};

const DO_NOT_SHOW_CRASH_REPORTING_SUGGESTION_KEY =
  "doNotShowCrashReportingSuggestion";

const shouldShowEnableCrashReportsMessage = (
  context: vscode.ExtensionContext,
) => {
  return !context.globalState.get<boolean>(
    DO_NOT_SHOW_CRASH_REPORTING_SUGGESTION_KEY,
  );
};

class ClientErrorHandler implements ErrorHandler {
  private readonly restarts: number[];
  private recommendedCrashReporting: boolean = false;

  constructor(
    private context: vscode.ExtensionContext,
    private maxRestartCount: number,
  ) {
    this.restarts = [];
  }

  public error(
    _error: Error,
    _message: Message,
    count: number,
  ): ErrorHandlerResult {
    if (count && count <= 3) {
      return { action: ErrorAction.Continue };
    }
    return { action: ErrorAction.Shutdown };
  }

  public closed(): CloseHandlerResult {
    if (
      !this.recommendedCrashReporting &&
      !isCrashReportingEnabled() &&
      shouldShowEnableCrashReportsMessage(this.context)
    ) {
      this.recommendedCrashReporting = true;
      vscode.window
        .showInformationMessage(
          "The Luwu Language server exited unexpected. Would you like to enable crash reporting?",
          "Enable",
          "Not now",
          "Do not show again",
        )
        .then((value) => {
          if (value === "Enable") {
            vscode.workspace
              .getConfiguration(NAMESPACE)
              .update(
                "server.crashReporting.enabled",
                true,
                vscode.ConfigurationTarget.Global,
              );
          } else if (value === "Do not show again") {
            this.context.globalState.update(
              DO_NOT_SHOW_CRASH_REPORTING_SUGGESTION_KEY,
              true,
            );
          }
        });
    }

    this.restarts.push(Date.now());
    if (this.restarts.length <= this.maxRestartCount) {
      return { action: CloseAction.Restart };
    } else {
      const diff = this.restarts[this.restarts.length - 1] - this.restarts[0];
      if (diff <= 3 * 60 * 1000) {
        return {
          action: CloseAction.DoNotRestart,
          message: `The Luwu Language server crashed ${this.maxRestartCount + 1} times in the last 3 minutes. The server will not be restarted. See the output for more information.`,
        };
      } else {
        this.restarts.shift();
        return { action: CloseAction.Restart };
      }
    }
  }
}

const handleExternalFiles = async (
  context: vscode.ExtensionContext,
  definitionFiles: {
    [packageName: string]: string;
  },
  documentationFiles: string[],
  builtinDefinitionFiles: {
    [packageName: string]: DownloadFileDefinition;
  },
  builtinDocumentationFiles: DownloadFileDefinition[],
) => {
  const finalDefinitionFiles = new Map<string, string>();

  // Determine a list of external files to fetch
  const externalFiles: DownloadFileDefinition[] = [];
  for (let [packageName, definitionPath] of Object.entries(definitionFiles)) {
    if (isExternalFile(definitionPath)) {
      const outputLocation = outputLocationForDefinition(context, packageName);
      externalFiles.push({
        url: definitionPath,
        outputUri: outputLocation,
      });
      finalDefinitionFiles.set(packageName, outputLocation.fsPath);
    } else {
      finalDefinitionFiles.set(packageName, definitionPath);
    }
  }
  documentationFiles = documentationFiles.map((documentationPath) => {
    if (isExternalFile(documentationPath)) {
      const outputLocation = outputLocationForDocumentation(
        context,
        documentationPath,
      );
      externalFiles.push({
        url: documentationPath,
        outputUri: outputLocation,
      });
      return outputLocation.fsPath;
    } else {
      return documentationPath;
    }
  });

  for (const [packageName, downloadDefinition] of Object.entries(
    builtinDefinitionFiles,
  )) {
    if (!finalDefinitionFiles.has(packageName)) {
      externalFiles.push(downloadDefinition);
      finalDefinitionFiles.set(
        packageName,
        downloadDefinition.outputUri.fsPath,
      );
    }
  }

  if (builtinDocumentationFiles) {
    externalFiles.push(...builtinDocumentationFiles);
    documentationFiles = documentationFiles.concat(
      builtinDocumentationFiles.map((info) => info.outputUri.fsPath),
    );
  }

  if (externalFiles) {
    const mustUpdate = await anyFileIsMissing(
      externalFiles.map((info) => info.outputUri),
    );
    if (mustUpdate || shouldFetchDefinitions(context)) {
      try {
        await downloadExternalFiles(externalFiles);
      } catch (err) {
        vscode.window.showWarningMessage(
          "Failed to donwload API information: " + err,
        );
      }
    }
  }

  return {
    definitionFiles: finalDefinitionFiles,
    documentationFiles,
    externalFiles,
  };
};

/// The language ID each entry of `luwu.fileExtensions` corresponds to.
/// `.luwu` and `.luau` are our own contributed languages; `.lua` is VSCode's
/// built-in one, which we attach to without claiming (it keeps its own icon
/// and grammar).
const LANGUAGE_ID_BY_FILE_EXTENSION: Record<string, string> = {
  luwu: "luwu",
  luau: "luau",
  lua: "lua",
};

/// The documents the language client attaches to. Dropping an extension from
/// `luwu.fileExtensions` leaves those files to whichever language server
/// does own them -- the server is told separately, so it also stops indexing
/// and diagnosing them.
export function buildDocumentSelector(
  fileExtensions: readonly string[],
): NonNullable<LanguageClientOptions["documentSelector"]> {
  const languages = new Set(
    fileExtensions
      .map((extension) => LANGUAGE_ID_BY_FILE_EXTENSION[extension])
      .filter((language): language is string => language !== undefined),
  );

  return [...languages].flatMap((language) => [
    { language, scheme: "file" },
    { language, scheme: "untitled" },
  ]);
}

const configuredFileExtensions = (): string[] =>
  getSettingOr<string[]>("fileExtensions", ["luwu", "luau", "lua"]);

const CONFIGURE_SERVER_PATH = "Configure luwu.server.path";

/// The language client's own failure for a missing binary is "couldn't create connection to
/// server", which tells nobody anything. This names the path we looked at, why it wasn't
/// there, and the two ways out.
const reportMissingServerBinary = async (
  serverBinPath: string,
  serverBinConfig: string,
): Promise<void> => {
  const reason =
    serverBinConfig === ""
      ? "no luwu.server.path is set and this build ships no bundled binary"
      : "luwu.server.path points at a file that doesn't exist, and there is " +
        "no bundled binary to fall back to";

  const choice = await vscode.window.showErrorMessage(
    `Luwu can't start: the luwu-lsp server binary was not found (${reason}).`,
    {
      modal: false,
      detail:
        `Looked for it at ${serverBinPath}.\n\n` +
        "Build it with `seal ./rebuild.luau` in the luwu-lsp repo, then set " +
        "luwu.server.path to that build/luwu-lsp.",
    },
    CONFIGURE_SERVER_PATH,
  );

  if (choice === CONFIGURE_SERVER_PATH) {
    await vscode.commands.executeCommand(
      "workbench.action.openSettings",
      "luwu.server.path",
    );
  }
};

const startLanguageServer = async (context: vscode.ExtensionContext) => {
  for (const disposable of clientDisposables) {
    disposable.dispose();
  }
  clientDisposables.splice(0, clientDisposables.length); // empty the list
  if (client) {
    await client.stop();
  }

  console.log("Starting Luwu Language Server");

  const args = ["lsp"];
  const debugArgs = ["lsp"];

  const addArg = (argument: string, mode: "All" | "Prod" | "Debug" = "All") => {
    if (mode === "All" || mode === "Prod") {
      args.push(argument);
    }
    if (mode === "All" || mode === "Debug") {
      debugArgs.push(argument);
    }
  };

  const {
    definitions: builtinDefinitionFiles,
    documentation: builtinDocumentationFiles,
  } = await roblox.preLanguageServerStart(context);

  // Load extra type definitions
  // TODO: deprecate and remove support of array-based definitionFiles configuration
  let definitionFilesConfig =
    getSetting<{ [packageName: string]: string } | string[]>(
      "types.definitionFiles",
    ) ?? {};

  if (Array.isArray(definitionFilesConfig)) {
    definitionFilesConfig = Object.fromEntries(
      definitionFilesConfig.map((path, index) => ["roblox" + index, path]),
    );
  }

  const documentationFilesConfig =
    getSetting<string[]>("types.documentationFiles") ?? [];

  const result = await handleExternalFiles(
    context,
    definitionFilesConfig,
    documentationFilesConfig,
    builtinDefinitionFiles ?? {},
    builtinDocumentationFiles ?? [],
  );

  for (let [packageName, definitionPath] of result.definitionFiles) {
    definitionPath = utils.resolvePath(definitionPath);
    let uri;
    if (vscode.workspace.workspaceFolders) {
      uri = utils.resolveUri(
        vscode.workspace.workspaceFolders[0].uri,
        definitionPath,
      );
    } else {
      uri = vscode.Uri.file(definitionPath);
    }
    if (await utils.exists(uri)) {
      addArg(`--definitions:${packageName}=${uri.fsPath}`);
    } else {
      vscode.window.showWarningMessage(
        `Definitions file '${packageName}' at ${definitionPath} does not exist, types will not be provided from this file`,
      );
    }
  }

  // Load extra documentation files
  for (let documentationPath of result.documentationFiles) {
    documentationPath = utils.resolvePath(documentationPath);
    let uri;
    if (vscode.workspace.workspaceFolders) {
      uri = utils.resolveUri(
        vscode.workspace.workspaceFolders[0].uri,
        documentationPath,
      );
    } else {
      uri = vscode.Uri.file(documentationPath);
    }
    if (await utils.exists(uri)) {
      addArg(`--docs=${uri.fsPath}`);
    } else {
      vscode.window.showWarningMessage(
        `Documentations file at ${documentationPath} does not exist`,
      );
    }
  }

  // Luau compatibility mode: turn off Luwu's own language features
  if (getSetting<boolean>("luauCompatibilityMode")) {
    addArg("--luau-compat");
  }

  // Handle FFlags
  const fflags: FFlags = {};
  if (!getSetting<boolean>("fflags.enableByDefault")) {
    addArg("--no-flags-enabled");
  }

  // Sync FFlags with upstream
  if (getSetting<boolean>("fflags.sync")) {
    try {
      const currentFlags = await getFFlags();
      if (currentFlags) {
        for (const [name, value] of Object.entries(currentFlags)) {
          for (const kind of FFLAG_KINDS) {
            if (name.startsWith(`${kind}Luau`)) {
              // Remove the "FFlag" part from the name
              fflags[name.substring(kind.length)] = value;
            }
          }
        }
      }
    } catch (err) {
      vscode.window.showWarningMessage(
        "Failed to fetch current Luau FFlags: " + err,
      );
    }
  }

  // Enable new solver
  if (getSetting<boolean>("fflags.enableNewSolver")) {
    fflags["LuauSolverV2"] = "true";
  }

  // Handle overrides
  const overridenFFlags = getSetting<FFlags>("fflags.override");
  if (overridenFFlags) {
    for (let [name, value] of Object.entries(overridenFFlags)) {
      if (!isAlphanumericUnderscore(name)) {
        vscode.window.showWarningMessage(
          `Invalid FFlag name: '${name}'. It can only contain alphanumeric characters`,
        );
      }

      name = name.trim();
      value = value.trim();

      // Strip kind prefix if it was included
      for (const kind of FFLAG_KINDS) {
        if (name.startsWith(`${kind}`)) {
          name = name.substring(kind.length);
        }
      }

      // Validate that the name and value is valid
      if (name.length > 0 && value.length > 0) {
        fflags[name] = value;
      }
    }
  }

  const serverBinConfig = getSettingOr("server.path", "").trim();
  const serverBinUri =
    vscode.workspace.workspaceFolders &&
    vscode.workspace.workspaceFolders.length > 0
      ? utils.resolveUri(
          vscode.workspace.workspaceFolders[0].uri,
          serverBinConfig,
        )
      : vscode.Uri.file(serverBinConfig);
  let serverBinPath;

  if (serverBinConfig !== "" && (await utils.exists(serverBinUri))) {
    serverBinPath = serverBinUri.fsPath;
  } else {
    const bundledBinPath = vscode.Uri.joinPath(
      context.extensionUri,
      "bin",
      os.platform() === "win32" ? "server.exe" : "server",
    ).fsPath;

    // A locally packaged .vsix carries no bin/server, so with no usable server.path this is
    // a dead end. Say so here: letting the client start would fail with "couldn't create
    // connection to server", which names neither the missing file nor the setting to fix.
    if (!(await utils.exists(vscode.Uri.file(bundledBinPath)))) {
      await reportMissingServerBinary(bundledBinPath, serverBinConfig);
      return;
    }

    if (serverBinConfig !== "") {
      vscode.window.showWarningMessage(
        `Server binary at path \`${serverBinUri.fsPath}\` does not exist, falling back to bundled binary`,
      );
    }
    serverBinPath = bundledBinPath;
  }

  const transport =
    getSettingOr<"stdio" | "pipe">("server.communicationChannel", "stdio") ===
    "pipe"
      ? TransportKind.pipe
      : TransportKind.stdio;

  const delayStartup = getSettingOr<boolean>("server.delayStartup", false);
  if (delayStartup) {
    addArg("--delay-startup");
  }

  if (isCrashReportingEnabled()) {
    addArg("--enable-crash-reporting");
    addArg(
      `--crash-report-directory=${vscode.Uri.joinPath(context.globalStorageUri, "sentry").fsPath}`,
    );
  }

  // Handle base luaurc
  const baseLuaurcConfig = getSetting<string>("server.baseLuaurc");
  if (baseLuaurcConfig) {
    const baseLuaurcPath = utils.resolvePath(baseLuaurcConfig);
    let uri;
    if (vscode.workspace.workspaceFolders) {
      uri = utils.resolveUri(
        vscode.workspace.workspaceFolders[0].uri,
        baseLuaurcPath,
      );
    } else {
      uri = vscode.Uri.file(baseLuaurcPath);
    }
    if (await utils.exists(uri)) {
      addArg(`--base-luaurc=${uri.fsPath}`);
    } else {
      vscode.window
        .showWarningMessage(
          `Base .luaurc file at ${baseLuaurcPath} does not exist`,
          "Configure Settings",
        )
        .then((action) => {
          if (action === "Configure Settings") {
            vscode.commands.executeCommand(
              "workbench.action.openSettings",
              "luwu.server.baseLuaurc",
            );
          }
        });
    }
  }

  const run: Executable = {
    command: serverBinPath,
    args,
    transport,
  };

  // If debugging, run the locally build extension, with local type definitions file
  const debug: Executable = {
    command: process.env["LUAU_LSP_SERVER_PATH"]
      ? vscode.Uri.file(process.env["LUAU_LSP_SERVER_PATH"]).fsPath
      : serverBinPath,
    args: debugArgs,
    transport,
  };

  const serverOptions: ServerOptions = { run, debug };

  const clientOptions: LanguageClientOptions = {
    documentSelector: buildDocumentSelector(
      // while upstream's extension is enabled we serve only .luwu, or
      // every diagnostic in the user's Luau files would appear twice
      effectiveFileExtensions(
        configuredFileExtensions(),
        isConflictActive(context),
      ),
    ),
    diagnosticPullOptions: {
      onChange: getSettingOr("diagnostics.pullOnChange", true),
      onSave: getSettingOr("diagnostics.pullOnSave", true),
    },
    initializationOptions: {
      fflags,
    },
    markdown: {
      supportHtml: true,
    },
    errorHandler: new ClientErrorHandler(context, 4),
    middleware: {
      provideOnTypeFormattingEdits: onTypeFormattingMiddleware,
      workspace: {
        // The server asks for the `luau-lsp` section, which is the wire
        // name every client uses -- including the nvim and zed ones, which
        // we don't ship and can't rename. Our settings live under
        // `luwu-lsp` and fall back to `luau-lsp`, and only the client can
        // tell a value someone wrote from a declared default, so the two
        // namespaces are reconciled here rather than in the server.
        configuration: (params, token, next) => {
          const keys = settingKeys(context.extension.packageJSON);

          return params.items.map((item) => {
            if (
              item.section !== LEGACY_NAMESPACE &&
              item.section !== NAMESPACE
            ) {
              // not ours: let the default handler answer it
              const answered = next({ items: [item] }, token);
              return Array.isArray(answered) ? answered[0] : answered;
            }

            const scope = item.scopeUri
              ? vscode.Uri.parse(item.scopeUri)
              : undefined;

            return buildServerConfiguration(keys, (key) =>
              getSetting(key, scope),
            );
          });
        },
      },
    },
  };

  // the id names the output channel and the `luwu.trace.server` setting
  client = new LanguageClient(
    "luwu",
    "Luwu Language Server",
    serverOptions,
    clientOptions,
  );

  platformContext.client = client;

  // Register commands
  client.onNotification("$/command", (params) => {
    vscode.commands.executeCommand(params.command, params.data);
  });

  clientDisposables.push(
    vscode.commands.registerCommand(
      "luwu.rename",
      async (
        uriString: string,
        position: { line: number; character: number },
      ) => {
        const uri = vscode.Uri.parse(uriString);
        const pos = new vscode.Position(position.line, position.character);
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.uri.toString() === uri.toString()) {
          editor.selection = new vscode.Selection(pos, pos);
          await vscode.commands.executeCommand("editor.action.rename");
        }
      },
    ),
  );

  clientDisposables.push(...registerComputeBytecode(context, client));
  clientDisposables.push(...registerComputeCompilerRemarks(context, client));
  clientDisposables.push(...registerComputeCodeGen(context, client));
  clientDisposables.push(...registerRequireGraph(context, client));
  clientDisposables.push(...registerViewInternalSource(context, client));
  clientDisposables.push(
    vscode.commands.registerCommand("luwu.openWalkthrough", () => {
      return vscode.commands.executeCommand(
        "workbench.action.openWalkthrough",
        "luwu-community.luwu#getting-started",
        false,
      );
    }),
  );
  clientDisposables.push(
    vscode.commands.registerCommand("luwu.updateApi", async () => {
      await downloadExternalFiles(result.externalFiles);
      vscode.window
        .showInformationMessage(
          "API Types have been updated, reload server to take effect.",
          "Reload Language Server",
        )
        .then((command) => {
          if (command === "Reload Language Server") {
            vscode.commands.executeCommand("luwu.reloadServer");
          }
        });
    }),
  );

  console.log("LSP Setup");
  await client.start();
};

export async function activate(context: vscode.ExtensionContext) {
  console.log("Luwu LSP activated");

  await roblox.onActivate(platformContext, context);

  registerConflictingExtensionCheck(context);
  registerLongBracketAutoClose(context);

  context.subscriptions.push(
    vscode.commands.registerCommand("luwu.reloadServer", async () => {
      vscode.window.showInformationMessage("Reloading Language Server");
      await startLanguageServer(context);
    }),
    vscode.commands.registerCommand("luwu.flushTimeTrace", async () => {
      if (client) {
        client.sendNotification("$/flushTimeTrace");
      }
    }),
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (
        settingChanged(e, "server") ||
        // the document selector is fixed when the client is constructed
        settingChanged(e, "fileExtensions")
      ) {
        vscode.window
          .showInformationMessage(
            "Luwu LSP server configuration has changed, reload server for this to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand("luwu.reloadServer");
            }
          });
      } else if (
        settingChanged(e, "fflags") ||
        settingChanged(e, "completion.enableFragmentAutocomplete")
      ) {
        vscode.window
          .showInformationMessage(
            "Luwu FFlags have been changed, reload server for this to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand("luwu.reloadServer");
            }
          });
      } else if (
        settingChanged(e, "types") ||
        settingChanged(e, "platform.type")
      ) {
        vscode.window
          .showInformationMessage(
            "Luwu type definitions have been changed, reload server for this to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand("luwu.reloadServer");
            }
          });
      }
    }),
  );

  await startLanguageServer(context);

  await roblox.postLanguageServerStart(platformContext, context);
}

export async function deactivate() {
  return Promise.allSettled([
    ...roblox.onDeactivate(),
    client?.stop(),
    clientDisposables.map((disposable) => disposable.dispose()),
  ]);
}
