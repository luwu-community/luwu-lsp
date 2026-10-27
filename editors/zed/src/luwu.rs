//! The Luwu extension for Zed: finds or downloads `luwu-lsp`, builds its command line from the
//! `lsp.luwu-lsp.settings` block, and forwards that block's `luau-lsp` object to the server as its
//! workspace configuration.

use serde::Deserialize;
use std::collections::BTreeMap;
use std::ffi::OsStr;
use std::fs;
use std::path::Path;
use zed::http_client::{HttpMethod, HttpRequest, RedirectPolicy};
use zed::lsp::CompletionKind;
use zed::serde_json::{self, Map, Value};
use zed::settings::LspSettings;
use zed::{CodeLabel, CodeLabelSpan, LanguageServerId};
use zed_extension_api::{self as zed, Result};

mod roblox;

/// The release that the `luwu` branch republishes on every push. Its tag never changes, so the asset's digest
/// tells us when a new build is up.
const RELEASE_API_URL: &str =
    "https://api.github.com/repos/luwu-community/luwu-lsp/releases/tags/luwu-latest";
const BINARIES_DIR: &str = "luwu-lsp-binaries";

/// `lsp.luwu-lsp.settings`. Everything the extension itself reads is a sibling of the `luau-lsp` object that
/// goes to the server.
#[derive(Debug, Deserialize)]
#[serde(default)]
struct Settings {
    /// The server's own configuration, sent as the `luau-lsp` workspace configuration section.
    #[serde(rename = "luau-lsp")]
    luau_lsp: Map<String, Value>,
    binary: BinarySettings,
    /// Definition files to load, `{ "@seal": "./.seal/typedefs/globals.d.luau" }`. Relative paths resolve
    /// against the worktree root.
    definitions: Definitions,
    /// Documentation files (`--docs`) for the definitions.
    documentation: Vec<String>,
    roblox: RobloxSettings,
    fflags: FFlagsSettings,
    /// Turn Luwu's language extensions off (`--luau-compat`), for plain Luau projects.
    luau_compat: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            luau_lsp: Map::new(),
            binary: BinarySettings::default(),
            definitions: Definitions::default(),
            documentation: Vec::new(),
            roblox: RobloxSettings::default(),
            fflags: FFlagsSettings::default(),
            luau_compat: false,
        }
    }
}

/// The VS Code setting is an object of `@name` to path. A plain list is accepted too, for settings copied
/// from zed-luau; the server names those entries itself.
#[derive(Debug, Deserialize)]
#[serde(untagged)]
enum Definitions {
    Named(BTreeMap<String, String>),
    List(Vec<String>),
}

impl Default for Definitions {
    fn default() -> Self {
        Definitions::Named(BTreeMap::new())
    }
}

#[derive(Debug, Default, Deserialize)]
#[serde(default)]
struct BinarySettings {
    /// The server to run instead of the one on PATH or the downloaded one.
    path: Option<String>,
    /// Extra arguments appended to the command line.
    args: Vec<String>,
    /// Skip the `luwu-lsp` on PATH and use the downloaded build.
    ignore_system_version: bool,
}

#[derive(Debug, Deserialize)]
#[serde(default)]
struct RobloxSettings {
    enabled: bool,
    security_level: SecurityLevel,
    download_api_documentation: bool,
    download_definitions: bool,
}

impl Default for RobloxSettings {
    fn default() -> Self {
        Self {
            enabled: false,
            security_level: SecurityLevel::Plugin,
            download_api_documentation: true,
            download_definitions: true,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "snake_case")]
enum SecurityLevel {
    RobloxScript,
    LocalUser,
    Plugin,
    None,
}

#[derive(Debug, Deserialize)]
#[serde(default)]
struct FFlagsSettings {
    /// luwu-lsp turns every non-experimental flag on unless this is false (`--no-flags-enabled`).
    enable_by_default: bool,
    /// Flags to set explicitly, `{ "LuauSolverV2": "true" }`. These win over the defaults.
    #[serde(rename = "override")]
    overrides: BTreeMap<String, String>,
}

impl Default for FFlagsSettings {
    fn default() -> Self {
        Self {
            enable_by_default: true,
            overrides: BTreeMap::new(),
        }
    }
}

/// One `--definitions` argument. Unnamed entries come from a list-shaped setting.
struct Definition {
    name: Option<String>,
    path: String,
}

#[derive(Debug, Deserialize)]
struct Release {
    assets: Vec<ReleaseAsset>,
}

#[derive(Debug, Deserialize)]
struct ReleaseAsset {
    name: String,
    browser_download_url: String,
    /// `sha256:<hex>`, when GitHub has computed it.
    digest: Option<String>,
    updated_at: String,
}

struct LuwuExtension {
    cached_binary_path: Option<String>,
}

fn is_file(path: &str) -> bool {
    fs::metadata(path).is_ok_and(|stat| stat.is_file())
}

fn is_dir(path: &str) -> bool {
    fs::metadata(path).is_ok_and(|stat| stat.is_dir())
}

fn is_path_absolute(path: &str) -> bool {
    let (platform, _) = zed::current_platform();
    match platform {
        // The WASM environment is UNIX-like, so Windows paths need their own check
        zed::Os::Windows => {
            let mut chars = path.chars();
            match (chars.next(), chars.next(), chars.next()) {
                (Some(drive), Some(':'), Some(sep)) => {
                    drive.is_ascii_alphabetic() && (sep == '\\' || sep == '/')
                }
                // UNC path
                _ => path.starts_with("//") || path.starts_with("\\\\"),
            }
        }
        _ => Path::new(OsStr::new(path)).is_absolute(),
    }
}

fn resolve_against(root: &str, path: &str) -> String {
    if is_path_absolute(path) {
        path.to_string()
    } else {
        format!("{root}/{path}")
    }
}

fn read_settings(
    language_server_id: &LanguageServerId,
    worktree: &zed::Worktree,
) -> Result<Settings> {
    let lsp_settings = LspSettings::for_worktree(language_server_id.as_ref(), worktree)?;
    let Some(value) = lsp_settings.settings else {
        return Ok(Settings::default());
    };
    if !value.is_object() {
        return Err("invalid luwu-lsp settings: `settings` must be an object".into());
    }
    serde_path_to_error::deserialize(value)
        .map_err(|error| format!("invalid luwu-lsp settings: {error}"))
}

/// The server-side setting `luau-lsp.types.definitionFiles`, read like `definitions` so that both places work.
fn server_definitions(luau_lsp: &Map<String, Value>) -> Vec<Definition> {
    let Some(files) = luau_lsp
        .get("types")
        .and_then(|types| types.get("definitionFiles"))
    else {
        return Vec::new();
    };
    match files {
        Value::Object(named) => named
            .iter()
            .filter_map(|(name, path)| {
                path.as_str().map(|path| Definition {
                    name: Some(name.clone()),
                    path: path.to_string(),
                })
            })
            .collect(),
        Value::Array(list) => list
            .iter()
            .filter_map(|path| {
                path.as_str().map(|path| Definition {
                    name: None,
                    path: path.to_string(),
                })
            })
            .collect(),
        _ => Vec::new(),
    }
}

/// Every definition file to load, with paths resolved against the worktree root: the `definitions` setting
/// first, then `luau-lsp.types.definitionFiles` entries that don't repeat one of them.
fn collect_definitions(settings: &Settings, root: &str) -> Vec<Definition> {
    let mut definitions: Vec<Definition> = match &settings.definitions {
        Definitions::Named(named) => named
            .iter()
            .map(|(name, path)| Definition {
                name: Some(name.clone()),
                path: resolve_against(root, path),
            })
            .collect(),
        Definitions::List(list) => list
            .iter()
            .map(|path| Definition {
                name: None,
                path: resolve_against(root, path),
            })
            .collect(),
    };
    for mut definition in server_definitions(&settings.luau_lsp) {
        definition.path = resolve_against(root, &definition.path);
        if !definitions
            .iter()
            .any(|known| known.path == definition.path)
        {
            definitions.push(definition);
        }
    }
    definitions
}

/// The `luau-lsp` configuration the server receives: the user's object, with `types.definitionFiles` set to
/// the resolved definition files so the server knows not to check them as modules.
fn server_configuration(settings: &Settings, root: &str) -> Value {
    let mut luau_lsp = settings.luau_lsp.clone();
    let definitions = collect_definitions(settings, root);
    if !definitions.is_empty() {
        let mut files = Map::new();
        for (index, definition) in definitions.iter().enumerate() {
            let name = definition
                .name
                .clone()
                .unwrap_or_else(|| format!("@definitions{index}"));
            files.insert(name, Value::String(definition.path.clone()));
        }
        let types = luau_lsp
            .entry("types")
            .or_insert_with(|| Value::Object(Map::new()));
        if !types.is_object() {
            *types = Value::Object(Map::new());
        }
        types
            .as_object_mut()
            .unwrap()
            .insert("definitionFiles".into(), Value::Object(files));
    }
    Value::Object(luau_lsp)
}

fn release_asset_name(platform: zed::Os, arch: zed::Architecture) -> &'static str {
    match (platform, arch) {
        (zed::Os::Mac, _) => "luwu-lsp-macos-universal.zip",
        (zed::Os::Windows, zed::Architecture::Aarch64) => "luwu-lsp-windows-arm64.zip",
        (zed::Os::Windows, _) => "luwu-lsp-windows-x64.zip",
        (zed::Os::Linux, zed::Architecture::Aarch64) => "luwu-lsp-linux-arm64.zip",
        (zed::Os::Linux, _) => "luwu-lsp-linux-x64.zip",
    }
}

fn fetch_release() -> Result<Release> {
    let request = HttpRequest::builder()
        .method(HttpMethod::Get)
        .url(RELEASE_API_URL)
        .header("Accept", "application/vnd.github+json")
        .header("User-Agent", "zed-luwu")
        .redirect_policy(RedirectPolicy::FollowAll)
        .build()?;
    let response = request
        .fetch()
        .map_err(|error| format!("failed to look up the luwu-latest release: {error}"))?;
    serde_json::from_slice(&response.body)
        .map_err(|error| format!("failed to read the luwu-latest release: {error}"))
}

/// A short string that changes whenever the asset is rebuilt, for the directory the build is unpacked into.
fn asset_stamp(asset: &ReleaseAsset) -> String {
    if let Some(hex) = asset
        .digest
        .as_deref()
        .and_then(|digest| digest.strip_prefix("sha256:"))
    {
        return hex.chars().take(16).collect();
    }
    asset
        .updated_at
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect()
}

/// The newest build already unpacked under `BINARIES_DIR`, used when GitHub can't be reached.
fn any_downloaded_binary(binary_name: &str) -> Option<String> {
    let entries = fs::read_dir(BINARIES_DIR).ok()?;
    let mut paths: Vec<String> = entries
        .filter_map(|entry| entry.ok())
        .map(|entry| {
            format!(
                "{BINARIES_DIR}/{}/{binary_name}",
                entry.file_name().to_string_lossy()
            )
        })
        .filter(|path| is_file(path))
        .collect();
    paths.sort();
    paths.pop()
}

impl LuwuExtension {
    /// The server to run: `binary.path`, then `luwu-lsp` on PATH, then the current `luwu-latest` build.
    fn language_server_binary_path(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
        settings: &Settings,
    ) -> Result<String> {
        if let Some(path) = &settings.binary.path {
            return Ok(path.clone());
        }
        if !settings.binary.ignore_system_version
            && let Some(path) = worktree.which("luwu-lsp")
        {
            return Ok(path);
        }
        if let Some(path) = &self.cached_binary_path
            && is_file(path)
        {
            return Ok(path.clone());
        }

        let (platform, arch) = zed::current_platform();
        let binary_name = match platform {
            zed::Os::Windows => "luwu-lsp.exe",
            _ => "luwu-lsp",
        };

        zed::set_language_server_installation_status(
            language_server_id,
            &zed::LanguageServerInstallationStatus::CheckingForUpdate,
        );
        let release = match fetch_release() {
            Ok(release) => release,
            // Offline: keep using what's there
            Err(error) => match any_downloaded_binary(binary_name) {
                Some(path) => {
                    self.cached_binary_path = Some(path.clone());
                    return Ok(path);
                }
                None => return Err(error),
            },
        };

        let wanted = release_asset_name(platform, arch);
        let asset = release
            .assets
            .iter()
            .find(|asset| asset.name == wanted)
            // Windows on ARM runs the x64 build when the arm64 one didn't get built
            .or_else(|| {
                release.assets.iter().find(|asset| {
                    asset.name == "luwu-lsp-windows-x64.zip" && platform == zed::Os::Windows
                })
            })
            .ok_or_else(|| format!("the luwu-latest release has no asset named {wanted:?}"))?;

        let dir_name = format!("luwu-lsp-{}", asset_stamp(asset));
        let version_dir = format!("{BINARIES_DIR}/{dir_name}");
        let binary_path = format!("{version_dir}/{binary_name}");

        if !is_dir(BINARIES_DIR) {
            fs::create_dir(BINARIES_DIR)
                .map_err(|error| format!("failed to create {BINARIES_DIR}: {error}"))?;
        }

        if !is_file(&binary_path) {
            zed::set_language_server_installation_status(
                language_server_id,
                &zed::LanguageServerInstallationStatus::Downloading,
            );
            zed::download_file(
                &asset.browser_download_url,
                &version_dir,
                zed::DownloadedFileType::Zip,
            )
            .map_err(|error| format!("failed to download {}: {error}", asset.name))?;
            zed::make_file_executable(&binary_path)?;

            // Drop the builds this one replaces
            let entries = fs::read_dir(BINARIES_DIR)
                .map_err(|error| format!("failed to list {BINARIES_DIR}: {error}"))?;
            for entry in entries.flatten() {
                if entry.file_name().to_str() != Some(&dir_name) {
                    fs::remove_dir_all(entry.path()).ok();
                }
            }
        }

        self.cached_binary_path = Some(binary_path.clone());
        Ok(binary_path)
    }
}

impl zed::Extension for LuwuExtension {
    fn new() -> Self {
        // Roblox's definitions and docs are re-downloaded every time Zed starts, so they stay current
        fs::remove_file(roblox::API_DOCS_FILE_NAME).ok();
        for level in roblox::SECURITY_LEVELS {
            fs::remove_file(roblox::definitions_file_for_level(level)).ok();
        }
        Self {
            cached_binary_path: None,
        }
    }

    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let settings = read_settings(language_server_id, worktree)?;
        let binary_path =
            self.language_server_binary_path(language_server_id, worktree, &settings)?;
        let root = worktree.root_path();

        // The extension runs with its work directory as cwd, and downloaded files live there
        let work_dir = std::env::current_dir()
            .map_err(|error| format!("failed to read the work directory: {error}"))?;
        let work_dir = work_dir.display();

        let mut args: Vec<String> = vec!["lsp".into()];

        if !settings.fflags.enable_by_default {
            args.push("--no-flags-enabled".into());
        }
        if settings.luau_compat {
            args.push("--luau-compat".into());
        }
        for (name, value) in &settings.fflags.overrides {
            if name.is_empty() || value.is_empty() {
                return Err("invalid luwu-lsp settings: every fflags.override entry needs a name and a value".into());
            }
            args.push(format!("--flag:{name}={value}"));
        }

        // Roblox's files go first so the user's definitions can refer to their types
        if settings.roblox.enabled {
            if settings.roblox.download_api_documentation {
                if !is_file(roblox::API_DOCS_FILE_NAME) {
                    roblox::download_api_docs()?;
                }
                args.push(format!("--docs={work_dir}/{}", roblox::API_DOCS_FILE_NAME));
            }
            if settings.roblox.download_definitions {
                let level = match settings.roblox.security_level {
                    SecurityLevel::None => roblox::SECURITY_LEVEL_NONE,
                    SecurityLevel::RobloxScript => roblox::SECURITY_LEVEL_ROBLOX_SCRIPT,
                    SecurityLevel::LocalUser => roblox::SECURITY_LEVEL_LOCAL_USER,
                    SecurityLevel::Plugin => roblox::SECURITY_LEVEL_PLUGIN,
                };
                let file_name = roblox::definitions_file_for_level(level);
                if !is_file(&file_name) {
                    roblox::download_definitions(level)?;
                }
                args.push(format!("--definitions:@roblox={work_dir}/{file_name}"));
            }
        }

        for definition in collect_definitions(&settings, &root) {
            match definition.name {
                Some(name) => args.push(format!("--definitions:{name}={}", definition.path)),
                None => args.push(format!("--definitions={}", definition.path)),
            }
        }
        for path in &settings.documentation {
            args.push(format!("--docs={}", resolve_against(&root, path)));
        }
        args.extend(settings.binary.args.iter().cloned());

        Ok(zed::Command {
            command: binary_path,
            args,
            env: Default::default(),
        })
    }

    fn language_server_initialization_options(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Option<Value>> {
        let lsp_settings = LspSettings::for_worktree(language_server_id.as_ref(), worktree)?;
        Ok(lsp_settings.initialization_options)
    }

    fn language_server_workspace_configuration(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Option<Value>> {
        let settings = read_settings(language_server_id, worktree)?;
        Ok(Some(serde_json::json!({
            "luau-lsp": server_configuration(&settings, &worktree.root_path())
        })))
    }

    /// Shows a function's signature next to its name in the completion list.
    fn label_for_completion(
        &self,
        _language_server_id: &LanguageServerId,
        completion: zed::lsp::Completion,
    ) -> Option<CodeLabel> {
        match completion.kind? {
            CompletionKind::Method | CompletionKind::Function => {
                let mut code = completion.label.clone();
                match completion.label_details.and_then(|details| details.detail) {
                    Some(detail) => code.push_str(&detail),
                    None => code.push_str("()"),
                }
                Some(CodeLabel {
                    spans: vec![CodeLabelSpan::code_range(0..code.len())],
                    filter_range: (0..completion.label.len()).into(),
                    code,
                })
            }
            _ => None,
        }
    }
}

zed::register_extension!(LuwuExtension);
