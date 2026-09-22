import * as vscode from "vscode";
import {
  getExplicitSetting,
  getSetting,
  getSettingOr,
  LEGACY_NAMESPACE,
  NAMESPACE,
  settingChanged,
} from "./settings";
import { Server } from "http";
import express, { ErrorRequestHandler } from "express";
import { format as bytesFormat } from "bytes";
import { fetch } from "undici";
import { spawn } from "child_process";
import { LanguageClient } from "vscode-languageclient/node";
import { AddArgCallback, PlatformContext } from "./extension";

import * as utils from "./utils";

let pluginServer: Server | undefined = undefined;

const getStudioPluginValue = <T>(key: string, defaultValue: T): T => {
  // `plugin` is the deprecated spelling of `studioPlugin`, and applies only
  // when the current spelling is untouched -- hence getExplicitSetting, which
  // ignores the declared default. Both spellings declare one, so a plain
  // getSetting here would always answer with studioPlugin's, and the
  // deprecated name would never be read at all.
  return (
    getExplicitSetting<T>(`studioPlugin.${key}`) ??
    getSetting<T>(`plugin.${key}`) ??
    defaultValue
  );
};

const API_DOCS = "https://luau-lsp.pages.dev/api-docs/en-us.json";
const LUAU_API_DOCS = "https://luau-lsp.pages.dev/api-docs/luau-en-us.json";
const STUDIO_PLUGIN_URL =
  "https://www.roblox.com/library/10913122509/Luau-Language-Server-Companion";

const setupStudioPlugin = async (client: LanguageClient | undefined) => {
  // Enable the plugin server
  await vscode.workspace
    .getConfiguration(NAMESPACE)
    .update("studioPlugin.enabled", true);
  startPluginServer(client);
  // Open the studio plugin in the browser for the user to install
  vscode.env.openExternal(vscode.Uri.parse(STUDIO_PLUGIN_URL));
};

const globalTypesEndpointForSecurityLevel = (securityLevel: string) => {
  return `https://luau-lsp.pages.dev/type-definitions/globalTypes.${securityLevel}.d.luau`;
};

const globalTypesUri = (
  context: vscode.ExtensionContext,
  securityLevel: string,
  mode: "Prod" | "Debug",
) => {
  if (mode === "Prod") {
    return vscode.Uri.joinPath(
      context.globalStorageUri,
      `globalTypes.${securityLevel}.d.luau`,
    );
  } else {
    return vscode.Uri.joinPath(
      context.extensionUri,
      "..",
      "..",
      `scripts/globalTypes.${securityLevel}.d.luau`,
    );
  }
};

const apiDocsUri = (context: vscode.ExtensionContext) => {
  return vscode.Uri.joinPath(context.globalStorageUri, "api-docs.json");
};

const luauApiDocsUri = (context: vscode.ExtensionContext) => {
  return vscode.Uri.joinPath(context.globalStorageUri, "luau-api-docs.json");
};

/// Writes back the setting `getRojoProjectFile` reads, in whichever namespace
/// the workspace is already using: rewriting a `luau-lsp.*` config into
/// `luwu.*` behind the user's back would break the tooling that generated
/// it.
const updateRojoProjectFile = (
  workspaceFolder: vscode.WorkspaceFolder,
  projectFile: string,
) => {
  const namespace = vscode.workspace
    .getConfiguration(LEGACY_NAMESPACE, workspaceFolder)
    .inspect<string>("sourcemap.rojoProjectFile")?.workspaceValue
    ? LEGACY_NAMESPACE
    : NAMESPACE;

  return vscode.workspace
    .getConfiguration(namespace, workspaceFolder)
    .update("sourcemap.rojoProjectFile", projectFile);
};

const getRojoProjectFile = async (
  workspaceFolder: vscode.WorkspaceFolder,
  client: LanguageClient | undefined,
) => {
  let projectFile = getSettingOr<string>(
    "sourcemap.rojoProjectFile",
    "default.project.json",
    workspaceFolder,
  );
  const projectFileUri = utils.resolveUri(workspaceFolder.uri, projectFile);

  if (await utils.exists(projectFileUri)) {
    return projectFile;
  }

  // Search if there is a *.project.json file present in this workspace.
  const foundProjectFiles = await vscode.workspace.findFiles(
    new vscode.RelativePattern(workspaceFolder.uri, "*.project.json"),
  );

  if (foundProjectFiles.length === 0) {
    // If the plugin is not enabled, provide a one-click setup button
    let options: string[] = [];
    if (!getStudioPluginValue("enabled", false)) {
      options.push("Setup Plugin");
    }
    options.push("Configure Settings");
    vscode.window
      .showWarningMessage(
        `Unable to find project file ${projectFile} for Rojo sourcemap generation. Configure a file in settings, or use the Studio Plugin for DataModel info instead`,
        ...options,
      )
      .then((value) => {
        if (value === "Setup Plugin") {
          setupStudioPlugin(client);
        } else if (value === "Configure Settings") {
          vscode.commands.executeCommand(
            "workbench.action.openWorkspaceSettings",
            "luwu.sourcemap",
          );
        }
      });
    return undefined;
  } else if (foundProjectFiles.length === 1) {
    const fileName = utils.basenameUri(foundProjectFiles[0]);
    const option = await vscode.window.showWarningMessage(
      `Unable to find project file ${projectFile} for Rojo sourcemap generation. We found ${fileName} available`,
      `Set project file to ${fileName}`,
      "Cancel",
    );

    if (option === `Set project file to ${fileName}`) {
      updateRojoProjectFile(workspaceFolder, fileName);
      return fileName;
    } else {
      return undefined;
    }
  } else {
    const option = await vscode.window.showWarningMessage(
      `Unable to find project file ${projectFile} for Rojo sourcemap generation. We found ${foundProjectFiles.length} files available`,
      "Select project file",
      "Cancel",
    );
    if (option === "Select project file") {
      const files = foundProjectFiles.map((file) => utils.basenameUri(file));
      const selectedFile = await vscode.window.showQuickPick(files);
      if (selectedFile) {
        updateRojoProjectFile(workspaceFolder, selectedFile);
        selectedFile;
      } else {
        return undefined;
      }
    } else {
      return undefined;
    }
  }

  return undefined;
};

const sourcemapDisposables: Map<
  vscode.WorkspaceFolder,
  Array<vscode.Disposable>
> = new Map();

const addSourcemapDisposable = (
  workspaceFolder: vscode.WorkspaceFolder,
  disposable: vscode.Disposable,
) => {
  if (!sourcemapDisposables.get(workspaceFolder)) {
    sourcemapDisposables.set(workspaceFolder, []);
  }
  sourcemapDisposables.get(workspaceFolder)!.push(disposable);
};

const cleanupSourcemapDisposables = async (
  workspaceFolder: vscode.WorkspaceFolder,
) => {
  const disposables = sourcemapDisposables.get(workspaceFolder);
  if (disposables) {
    for (const disposable of disposables) {
      disposable.dispose();
    }
  }
  sourcemapDisposables.delete(workspaceFolder);
};

const startSourcemapGeneration = async (
  client: LanguageClient | undefined,
  workspaceFolder: vscode.WorkspaceFolder,
) => {
  cleanupSourcemapDisposables(workspaceFolder);

  if (
    !getSetting<boolean>("sourcemap.enabled", workspaceFolder) ||
    !getSetting<boolean>("sourcemap.autogenerate", workspaceFolder)
  ) {
    return;
  }

  const customGeneratorCommand = getSetting<string>(
    "sourcemap.generatorCommand",
    workspaceFolder,
  );
  const useVSCodeWatcher = getSettingOr<boolean>(
    "sourcemap.useVSCodeWatcher",
    false,
    workspaceFolder,
  );

  const loggingFunc = client ? client.info.bind(client) : console.log;
  loggingFunc(
    `Starting sourcemap generation for ${
      workspaceFolder.name
    } (${workspaceFolder.uri.toString(true)})`,
  );

  const cwd = workspaceFolder.uri.fsPath;

  const spawnChildProcess = async () => {
    loggingFunc(
      `Spawning sourcemap generator for ${
        workspaceFolder.name
      } (${workspaceFolder.uri.toString(true)})`,
    );

    let childProcess;

    if (customGeneratorCommand && customGeneratorCommand.trim() !== "") {
      // TODO: should we support shell execution here?
      // It allows us to delegate to the shell for argument parsing
      // but it causes issues when VSCode shuts down, leaving a zombie process
      childProcess = spawn(customGeneratorCommand, {
        cwd,
        shell: true,
      });
    } else {
      // Check if the project file exists
      const projectFile = await getRojoProjectFile(workspaceFolder, client);
      if (!projectFile) {
        return;
      }
      const rojoPath = getSettingOr<string>(
        "sourcemap.rojoPath",
        "rojo",
        workspaceFolder,
      );
      const sourcemapFileName = getSettingOr<string>(
        "sourcemap.sourcemapFile",
        "sourcemap.json",
        workspaceFolder,
      );
      const args = ["sourcemap", projectFile, "--output", sourcemapFileName];

      if (getSetting<boolean>("sourcemap.includeNonScripts", workspaceFolder)) {
        args.push("--include-non-scripts");
      }

      if (!useVSCodeWatcher) {
        args.push("--watch");
      }

      childProcess = spawn(rojoPath, args, { cwd });
    }

    let stderr = "";
    childProcess.stderr.on("data", (data) => {
      stderr += data;
    });

    childProcess.on("error", (err) => {
      stderr += err.message;
    });

    childProcess.on("close", (code, signal) => {
      if (childProcess.killed) {
        return;
      }
      if (code !== 0) {
        let output = `Failed to update sourcemap for ${workspaceFolder.name}: `;
        let options = ["Retry"];

        if (customGeneratorCommand) {
          output += stderr;
          if (stderr === "") {
            output += "<no output>";
          }
          options.push("Configure Settings");
        } else {
          if (
            stderr.includes("Found argument 'sourcemap' which wasn't expected")
          ) {
            output +=
              "Your Rojo version doesn't have sourcemap support. Upgrade to Rojo v7.3.0+";
          } else if (
            stderr.includes("Found argument '--watch' which wasn't expected")
          ) {
            output +=
              "Your Rojo version doesn't have sourcemap watching support. Upgrade to Rojo v7.3.0+";
          } else if (
            stderr.includes("is not recognized") ||
            stderr.includes("ENOENT")
          ) {
            output +=
              "Rojo not found. Configure your Rojo path in settings, or use the Studio Plugin for DataModel info instead";
            if (!getStudioPluginValue("enabled", false)) {
              options.push("Setup Plugin");
            }
            options.push("Configure Settings");
          } else {
            output += stderr;
          }
        }

        vscode.window.showWarningMessage(output, ...options).then((value) => {
          if (value === "Retry") {
            startSourcemapGeneration(client, workspaceFolder);
          } else if (value === "Setup Plugin") {
            setupStudioPlugin(client);
          } else if (value === "Configure Settings") {
            vscode.commands.executeCommand(
              "workbench.action.openWorkspaceSettings",
              "luwu.sourcemap",
            );
          }
        });
      }
    });

    return childProcess;
  };

  if (useVSCodeWatcher) {
    spawnChildProcess();

    const watcher = vscode.workspace.createFileSystemWatcher(
      new vscode.RelativePattern(workspaceFolder, "**/*.{lua,luau,luwu}"),
      /* ignoreCreateEvents = */ false,
      /* ignoreChangeEvents = */ true,
      /* ignoreDeleteEvents = */ false,
    );

    let debounceTimer: NodeJS.Timeout;
    watcher.onDidCreate(() => {
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(spawnChildProcess, 1000);
    });
    watcher.onDidDelete(() => {
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(spawnChildProcess, 1000);
    });

    addSourcemapDisposable(workspaceFolder, watcher);
  } else {
    const childProcess = await spawnChildProcess();
    if (childProcess) {
      childProcess.on("close", (code) => {
        cleanupSourcemapDisposables(workspaceFolder);

        if (code === 0) {
          vscode.window
            .showWarningMessage(
              "Sourcemap generator ended. No further updates will be tracked. If the generator does not support file watching, enable luwu.sourcemap.useVSCodeWatcher",
              "Restart",
              "Configure Settings",
            )
            .then((value) => {
              if (value === "Restart") {
                startSourcemapGeneration(client, workspaceFolder);
              } else if (value === "Configure Settings") {
                vscode.commands.executeCommand(
                  "workbench.action.openWorkspaceSettings",
                  "luwu.sourcemap",
                );
              }
            });
        }
      });
      addSourcemapDisposable(
        workspaceFolder,
        new vscode.Disposable(() => {
          if (childProcess.killed) {
            return;
          }
          childProcess.kill();
        }),
      );
    }
  }
};

const startPluginServer = async (client: LanguageClient | undefined) => {
  if (pluginServer) {
    return;
  }

  const app = express();
  app.use(
    express.json({
      limit: getStudioPluginValue("maximumRequestBodySize", "3mb"),
    }),
  );

  app.post("/full", (req, res) => {
    if (!client) {
      return res.sendStatus(500);
    }

    if (req.body.tree) {
      client.sendNotification("$/plugin/full", req.body.tree);
      res.sendStatus(200);
    } else {
      res.sendStatus(400);
    }
  });

  app.post("/clear", (_req, res) => {
    if (!client) {
      return res.sendStatus(500);
    }

    client.sendNotification("$/plugin/clear");
    res.sendStatus(200);
  });

  app.get("/get-file-paths", async (_req, res) => {
    try {
      const uris = await vscode.workspace.findFiles("**/*.{lua,luau,luwu}");
      res.json({
        files: uris.map((uri: vscode.Uri) => uri.fsPath),
      });
    } catch (error) {
      console.error("Error getting file paths:", error);
      res.status(500).json({ error: "Failed to get file paths" });
    }
  });

  const errorHandler: ErrorRequestHandler = (err, req, res, next) => {
    if (res.headersSent) {
      return next(err);
    }

    if (err && err.type === "entity.too.large") {
      res
        .status(413)
        .send(
          `Result is too large. Limit: ${bytesFormat(err.limit)}, Received: ${bytesFormat(err.received)}.\n` +
            `Increase your available limits by updating the 'luwu.studioPlugin.maximumRequestBodySize' property in VSCode, or by reducing the include list in the Studio Plugin settings`,
        );
    }
  };

  app.use(errorHandler);

  const port = getStudioPluginValue("port", 3667);
  pluginServer = app
    .listen(port, () => {
      vscode.window.showInformationMessage(
        `Luau Language Server Studio Plugin is now listening on port ${port}`,
      );
    })
    .on("error", (err) => {
      if ((err as any).code === "EADDRINUSE") {
        vscode.window
          .showErrorMessage(
            `Failed to start Luau Language Server Studio Plugin on port ${port}: Port already in use. Check there are no other servers running on this port, or change the port in settings`,
            "Reconnect",
            "Change Port Configuration",
          )
          .then((value) => {
            if (value === "Reconnect") {
              stopPluginServer(true);
              startPluginServer(client);
            } else if (value === "Change Port Configuration") {
              vscode.commands.executeCommand(
                "workbench.action.openWorkspaceSettings",
                "luwu.studioPlugin.port",
              );
            }
          });
      } else {
        vscode.window.showErrorMessage(
          `Failed to start Luau Language Server Studio Plugin on port ${port}: ${err}`,
        );
      }
    });
};

const stopPluginServer = async (isDeactivating = false) => {
  if (pluginServer) {
    pluginServer.close();
    pluginServer = undefined;

    if (!isDeactivating) {
      vscode.window.showInformationMessage(
        `Luau Language Server Studio Plugin has disconnected`,
      );
    }
  }
};

export const onActivate = async (
  platformContext: PlatformContext,
  context: vscode.ExtensionContext,
) => {
  const startSourcemapGenerationForAllFolders = () => {
    if (vscode.workspace.workspaceFolders) {
      for (const folder of vscode.workspace.workspaceFolders) {
        startSourcemapGeneration(platformContext.client, folder);
      }
    }
  };

  context.subscriptions.push(
    vscode.commands.registerCommand(
      "luwu.regenerateSourcemap",
      startSourcemapGenerationForAllFolders,
    ),
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("luwu.setupStudioPlugin", () =>
      setupStudioPlugin(platformContext.client),
    ),
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (settingChanged(e, "sourcemap")) {
        if (vscode.workspace.workspaceFolders) {
          for (const folder of vscode.workspace.workspaceFolders) {
            if (
              !getSetting<boolean>("sourcemap.enabled", folder) ||
              !getSetting<boolean>("sourcemap.autogenerate", folder)
            ) {
              cleanupSourcemapDisposables(folder);
            } else {
              startSourcemapGeneration(platformContext.client, folder);
            }
          }
        }
      } else if (
        settingChanged(e, "studioPlugin") ||
        settingChanged(e, "plugin")
      ) {
        if (getStudioPluginValue("enabled", false)) {
          stopPluginServer(true);
          startPluginServer(platformContext.client);
        } else {
          stopPluginServer();
        }
      }
    }),
  );

  startSourcemapGenerationForAllFolders();
};

export const preLanguageServerStart = async (
  context: vscode.ExtensionContext,
) => {
  // Load roblox type definitions
  const platformType = getSetting<string>("platform.type");
  const robloxTypes = getSetting<boolean>("types.roblox");

  // TODO: Cleanup when deprecated luwu.types.roblox is deleted
  // We need to respect the new setting as well as the old setting. We check for "&&" since they are on by default
  if (platformType === "roblox" && robloxTypes) {
    const securityLevel = getSettingOr<string>(
      "types.robloxSecurityLevel",
      "PluginSecurity",
    );

    return {
      definitions: {
        ["@roblox"]: {
          url: globalTypesEndpointForSecurityLevel(securityLevel),
          outputUri: globalTypesUri(context, securityLevel, "Prod"),
        },
      },
      documentation: [{ url: API_DOCS, outputUri: apiDocsUri(context) }],
    };
  } else {
    return {
      definitions: undefined,
      documentation: [
        { url: LUAU_API_DOCS, outputUri: luauApiDocsUri(context) },
      ],
    };
  }
};

export const postLanguageServerStart = async (
  platformContext: PlatformContext,
  _: vscode.ExtensionContext,
) => {
  if (getStudioPluginValue("enabled", false)) {
    startPluginServer(platformContext.client);
  }
};

export const onDeactivate = () => {
  return [
    ...Array.from(sourcemapDisposables.keys()).map((workspace) =>
      cleanupSourcemapDisposables(workspace),
    ),
    stopPluginServer(true),
  ];
};
