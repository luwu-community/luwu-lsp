//! Roblox's type definitions and API docs, served from luau-lsp's CDN like the VS Code extension and zed-luau do.

use zed_extension_api::{self as zed, Result};

const API_DOCS_URL: &str = "https://luau-lsp.pages.dev/api-docs/en-us.json";
pub const API_DOCS_FILE_NAME: &str = "api-docs.json";

pub const SECURITY_LEVEL_NONE: &str = "None";
pub const SECURITY_LEVEL_LOCAL_USER: &str = "LocalUserSecurity";
pub const SECURITY_LEVEL_PLUGIN: &str = "PluginSecurity";
pub const SECURITY_LEVEL_ROBLOX_SCRIPT: &str = "RobloxScriptSecurity";
pub const SECURITY_LEVELS: [&str; 4] = [
    SECURITY_LEVEL_NONE,
    SECURITY_LEVEL_LOCAL_USER,
    SECURITY_LEVEL_PLUGIN,
    SECURITY_LEVEL_ROBLOX_SCRIPT,
];

pub fn definitions_file_for_level(level: &str) -> String {
    format!("globalTypes.{level}.d.luau")
}

pub fn download_api_docs() -> Result<()> {
    zed::download_file(
        API_DOCS_URL,
        API_DOCS_FILE_NAME,
        zed::DownloadedFileType::Uncompressed,
    )
    .map_err(|error| format!("failed to download the Roblox API docs: {error}"))
}

pub fn download_definitions(level: &str) -> Result<()> {
    let url = format!("https://luau-lsp.pages.dev/type-definitions/globalTypes.{level}.d.luau");
    zed::download_file(
        &url,
        &definitions_file_for_level(level),
        zed::DownloadedFileType::Uncompressed,
    )
    .map_err(|error| format!("failed to download the Roblox definitions: {error}"))
}
