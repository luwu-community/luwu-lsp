import * as vscode from "vscode";

/// Upstream's extension, which this one is a fork of.
export const UPSTREAM_EXTENSION_ID = "JohnnyMorganz.luau-lsp";
const UPSTREAM_NAME = "Luau Language Server";

/// Set once the user has chosen to serve .luau/.lua even though upstream is
/// enabled.
const OVERRIDE_KEY = "luwu.runAlongsideUpstream";

/// Set once the user has accepted the split as their setup, so we stop
/// prompting. The status bar item stays either way -- the split is worth
/// showing, it just isn't worth interrupting twice.
const KEEP_BOTH_KEY = "luwu.acceptedUpstreamSplit";

/// Nothing stops the two extensions being installed together, and VSCode has no
/// declarative way to say two extensions conflict, so this is the whole
/// defence. What goes wrong without it:
///
///   - both attach a language client to every `.luau` and `.lua` file, so there
///     are two server processes and the user sees every diagnostic twice,
///     doubled completions, two hovers glued into one popup, and two sets of
///     code actions. This is the one that matters: it reads as *our* extension
///     being broken, since we're the one that just got installed
///   - both contribute the `source.luau` grammar scope, and VSCode keeps a
///     single registration per scope name and logs an error about the other
///   - both contribute the `luau` language, which VSCode merges -- harmless,
///     but it means we can't tell the two apart by language ID
///
/// Command IDs and settings no longer collide: ours were renamed to
/// `luwu.*`.
export function findConflictingExtension<T>(
  ourExtensionId: string,
  lookup: (id: string) => T | undefined = (id) =>
    vscode.extensions.getExtension(id) as T | undefined,
): T | undefined {
  // while this fork still ships under upstream's own extension ID, VSCode
  // treats ours as an *update* of theirs and the two can never be installed
  // together
  if (ourExtensionId.toLowerCase() === UPSTREAM_EXTENSION_ID.toLowerCase()) {
    return undefined;
  }

  return lookup(UPSTREAM_EXTENSION_ID);
}

/// Whether we should hold back from the languages upstream also serves.
export function isConflictActive(context: vscode.ExtensionContext): boolean {
  if (context.globalState.get<boolean>(OVERRIDE_KEY)) {
    return false;
  }

  return findConflictingExtension(context.extension.id) !== undefined;
}

/// The extensions we actually attach to. While upstream is enabled we serve
/// only `.luwu`, which it doesn't know about: better to do one thing correctly
/// than to double every diagnostic in the user's Luau files. The status bar
/// item below is what stops this reading as "the extension I just installed
/// does nothing".
export function effectiveFileExtensions(
  configured: readonly string[],
  conflictActive: boolean,
): string[] {
  if (!conflictActive) {
    return [...configured];
  }

  return configured.filter((extension) => extension === "luwu");
}

function createStatusBarItem(acknowledged: boolean): vscode.StatusBarItem {
  const item = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Right,
    0,
  );
  item.name = "Luwu";
  item.text = "$(file-code) Luwu: .luwu only";
  item.tooltip =
    `${UPSTREAM_NAME} is enabled and handling .luau and .lua, so Luwu ` +
    "Language Server is handling only .luwu. Click to change which files " +
    "each one serves.";
  // loud until they've made a call, quiet once the split is their choice rather
  // than our guess
  item.backgroundColor = acknowledged
    ? undefined
    : new vscode.ThemeColor("statusBarItem.warningBackground");
  item.command = RESOLVE_COMMAND;
  return item;
}

const RESOLVE_COMMAND = "luwu.resolveExtensionConflict";

async function promptReload(reason: string): Promise<void> {
  const reload = "Reload Window";
  const choice = await vscode.window.showInformationMessage(
    reason,
    { modal: false },
    reload,
  );
  if (choice === reload) {
    await vscode.commands.executeCommand("workbench.action.reloadWindow");
  }
}

async function resolveConflict(
  context: vscode.ExtensionContext,
): Promise<void> {
  const keepBoth = "Keep Both";
  const takeOver = "Let Luwu Handle .luau Too";
  const disable = "Disable Luau LSP...";

  const choice = await vscode.window.showWarningMessage(
    `${UPSTREAM_NAME} is installed and enabled alongside Luwu.`,
    {
      modal: true,
      detail:
        "To prevent duplicating diagnostics with Luau Language Server, " +
        "Luwu is currently handling only Luwu (.luwu) " +
        "files.\nMost Luau code works perfectly in Luwu. I recommend " +
        "disabling Luau Language Server so you can benefit from Luwu's " +
        "improved DX in your Luau and Luwu codebases, but note that some " +
        "very recent semantics might diverge between the two languages.",
    },
    keepBoth,
    takeOver,
    disable,
  );

  if (choice === takeOver) {
    await context.globalState.update(OVERRIDE_KEY, true);
    await promptReload(
      `Luwu will handle .luau and .lua as well. While ` +
        `${UPSTREAM_NAME} is also enabled you will see every diagnostic ` +
        "twice; disable it from the Extensions view to stop that. Reload to " +
        "apply.",
    );
  } else if (choice === keepBoth) {
    // the split is already in effect; just stop asking
    await context.globalState.update(KEEP_BOTH_KEY, true);
  } else if (choice === disable) {
    // there is no API for disabling another extension on the user's behalf, so
    // the best we can do is open its detail page, which has the Disable button
    // on it -- hence the ellipsis
    await vscode.commands.executeCommand(
      "extension.open",
      UPSTREAM_EXTENSION_ID,
    );
  }
}

/// Registers the conflict handling. Call before starting the language client --
/// the client's document selector is built from `effectiveFileExtensions`, and
/// it is fixed once the client is constructed, so a change here needs a reload
/// rather than a restart.
export function registerConflictingExtensionCheck(
  context: vscode.ExtensionContext,
): void {
  context.subscriptions.push(
    vscode.commands.registerCommand(RESOLVE_COMMAND, () =>
      resolveConflict(context),
    ),
  );

  let statusBarItem: vscode.StatusBarItem | undefined;
  let promptedThisSession = false;
  let wasConflicting = isConflictActive(context);

  const sync = (initial: boolean) => {
    const conflicting = isConflictActive(context);
    const acknowledged = !!context.globalState.get<boolean>(KEEP_BOTH_KEY);

    statusBarItem?.dispose();
    statusBarItem = undefined;
    if (conflicting) {
      statusBarItem = createStatusBarItem(acknowledged);
      context.subscriptions.push(statusBarItem);
      statusBarItem.show();
    }

    if (conflicting && !acknowledged && !promptedThisSession) {
      promptedThisSession = true;
      // modal, because the alternative is someone installing this on a
      // recommendation and quietly concluding it doesn't work
      void resolveConflict(context);
    }

    if (!initial && wasConflicting && !conflicting) {
      void promptReload(
        "Luwu can now handle .luau and .lua files. " + "Reload to apply.",
      );
    }
    wasConflicting = conflicting;
  };

  sync(/* initial: */ true);
  context.subscriptions.push(
    vscode.extensions.onDidChange(() => sync(/* initial: */ false)),
  );
}
