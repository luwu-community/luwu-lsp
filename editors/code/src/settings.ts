import * as vscode from "vscode";

/// This extension's settings namespace.
export const NAMESPACE = "luwu";

/// The namespace this extension used before it was renamed from upstream's
/// `luau-lsp`. It is not a migration ramp: external tooling (seal, for one)
/// generates `luau-lsp.*` configs, and using this extension purely for
/// Luau/Roblox work is a supported case, so these are read permanently.
export const LEGACY_NAMESPACE = "luau-lsp";

const INHERIT_LEGACY_KEY = "inheritLuauLspSettings";

/// What `WorkspaceConfiguration.inspect` tells us about one key. Only the parts
/// we need, so the resolution below can be tested without a workspace.
export interface InspectedSetting<T> {
  defaultValue?: T;
  globalValue?: T;
  workspaceValue?: T;
  workspaceFolderValue?: T;
}

/// The value someone actually wrote, most specific scope first, or undefined if
/// the setting is untouched and only has its default.
function explicitValue<T>(
  inspected: InspectedSetting<T> | undefined,
): T | undefined {
  if (!inspected) {
    return undefined;
  }

  return (
    inspected.workspaceFolderValue ??
    inspected.workspaceValue ??
    inspected.globalValue
  );
}

/// Resolves one setting across the two namespaces: an explicit `luwu.*`
/// value wins, then an explicit `luau-lsp.*` one if we're inheriting, then the
/// declared default.
///
/// Both namespaces declare every key, so `inspect` separates "written by the
/// user" from "this is just the default" on both sides -- which a plain `get`
/// cannot do, and which is the whole reason a legacy value can outrank a
/// new-namespace default.
export function resolveSetting<T>(
  inspected: InspectedSetting<T> | undefined,
  legacyInspected: InspectedSetting<T> | undefined,
  inheritLegacy: boolean,
): T | undefined {
  const explicit = explicitValue(inspected);
  if (explicit !== undefined) {
    return explicit;
  }

  if (inheritLegacy) {
    const legacy = explicitValue(legacyInspected);
    if (legacy !== undefined) {
      return legacy;
    }
  }

  return inspected?.defaultValue ?? legacyInspected?.defaultValue;
}

export function isInheritingLegacySettings(): boolean {
  return vscode.workspace
    .getConfiguration(NAMESPACE)
    .get<boolean>(INHERIT_LEGACY_KEY, true);
}

/// Reads one setting by its dotted key, e.g. `inlayHints.blockEndHints`,
/// honouring the legacy namespace. Use this instead of
/// `vscode.workspace.getConfiguration("luwu-lsp")` so that a `luau-lsp.*` value
/// is never silently ignored.
export function getSetting<T>(
  key: string,
  scope?: vscode.ConfigurationScope,
): T | undefined {
  return resolveSetting<T>(
    vscode.workspace.getConfiguration(NAMESPACE, scope).inspect<T>(key),
    vscode.workspace.getConfiguration(LEGACY_NAMESPACE, scope).inspect<T>(key),
    isInheritingLegacySettings(),
  );
}

/// Like `getSetting`, but only what someone actually wrote: a setting left at
/// its declared default reads as undefined. Needed where a deprecated spelling
/// of a setting applies only when the current spelling is untouched, which
/// `getSetting` can't express -- it resolves the declared default, so it never
/// returns undefined for a declared key.
export function resolveExplicitSetting<T>(
  inspected: InspectedSetting<T> | undefined,
  legacyInspected: InspectedSetting<T> | undefined,
  inheritLegacy: boolean,
): T | undefined {
  const explicit = explicitValue(inspected);
  if (explicit !== undefined) {
    return explicit;
  }

  return inheritLegacy ? explicitValue(legacyInspected) : undefined;
}

export function getExplicitSetting<T>(
  key: string,
  scope?: vscode.ConfigurationScope,
): T | undefined {
  return resolveExplicitSetting<T>(
    vscode.workspace.getConfiguration(NAMESPACE, scope).inspect<T>(key),
    vscode.workspace.getConfiguration(LEGACY_NAMESPACE, scope).inspect<T>(key),
    isInheritingLegacySettings(),
  );
}

export function getSettingOr<T>(
  key: string,
  fallback: T,
  scope?: vscode.ConfigurationScope,
): T {
  return getSetting<T>(key, scope) ?? fallback;
}

/// Whether a configuration change touched `key` (dotted, unprefixed) in either
/// namespace. Use this rather than `event.affectsConfiguration("luwu.…")`,
/// or an edit to a legacy setting won't be noticed.
export function settingChanged(
  event: vscode.ConfigurationChangeEvent,
  key: string,
  scope?: vscode.ConfigurationScope,
): boolean {
  return (
    event.affectsConfiguration(`${NAMESPACE}.${key}`, scope) ||
    event.affectsConfiguration(`${LEGACY_NAMESPACE}.${key}`, scope)
  );
}

/// Settings the server has no business receiving: `trace.server` belongs to
/// vscode-languageclient and lands in our namespace only because it is named
/// after the client id, and the inherit flag decides how this file resolves
/// everything else.
const CLIENT_ONLY_SETTINGS = new Set(["trace.server", INHERIT_LEGACY_KEY]);

/// Every settable key, without its namespace prefix, taken from our own
/// contributions so that a new setting never has to be registered in two
/// places.
export function settingKeys(packageJSON: unknown): string[] {
  const properties = (
    packageJSON as {
      contributes?: {
        configuration?: { properties?: Record<string, unknown> };
      };
    }
  )?.contributes?.configuration?.properties;

  if (!properties) {
    return [];
  }

  const prefix = `${NAMESPACE}.`;
  return Object.keys(properties)
    .filter((key) => key.startsWith(prefix))
    .map((key) => key.slice(prefix.length))
    .filter((key) => !CLIENT_ONLY_SETTINGS.has(key));
}

/// Nests a dotted key into `target`, so `inlayHints.blockEndHints` becomes `{
/// inlayHints: { blockEndHints: ... } }` -- the shape the server's
/// `ClientConfiguration` deserializes from.
function assignNested(
  target: Record<string, unknown>,
  key: string,
  value: unknown,
): void {
  const segments = key.split(".");
  let node = target;

  for (const segment of segments.slice(0, -1)) {
    const existing = node[segment];
    if (typeof existing !== "object" || existing === null) {
      node[segment] = {};
    }
    node = node[segment] as Record<string, unknown>;
  }

  node[segments[segments.length - 1]] = value;
}

/// Builds the object the server asked for when it requests the `luau-lsp`
/// configuration section. The server is unchanged and still asks under the old
/// section name -- that is the wire name every client uses, including the nvim
/// and zed ones we don't ship -- so the two namespaces are reconciled here,
/// where `inspect` is available.
export function buildServerConfiguration(
  keys: readonly string[],
  read: (key: string) => unknown,
): Record<string, unknown> {
  const configuration: Record<string, unknown> = {};

  for (const key of keys) {
    const value = read(key);
    if (value !== undefined) {
      assignNested(configuration, key, value);
    }
  }

  return configuration;
}
