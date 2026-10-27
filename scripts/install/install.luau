--!strict
-- Installs luwu-lsp and sets up the editors that use it. install.sh and install.ps1 run this with a seal newer than
-- 0.8.0; it's written for that seal, so it's plain Luau. `--help` lists the options.
--
-- Interactive in a terminal: it asks which editors to set up, whether to turn on Roblox support, and before every
-- change to an editor's config. Without a terminal it does what the arguments say, and changes configs only with --yes.
-- Every config it changes is copied to a backup first, and edited in place so its comments and layout survive.

const args = require("@std/args")
const colors = require("@std/colors")
const cursor = require("@std/terminal/cursor")
const env = require("@std/env")
const filesize = require("@std/fs/filesize")
const fs = require("@std/fs")
const http = require("@std/net/http")
const json = require("@std/json")
const output = require("@std/io/output")
const process = require("@std/process")
const prompt = require("@std/io/prompt")
const str = require("@std/str")
const terminal = require("@std/terminal")
const time = require("@std/time")
const zip = require("@std/archive/zip")

const RELEASE = "https://github.com/luwu-community/luwu-lsp/releases/download/luwu-latest"
const SUBLIME_SYNTAX = "https://github.com/luwu-community/luwu-lsp/tree/luwu/editors/sublime"
const NVIM_NOTES = "https://github.com/luwu-community/luwu-lsp/tree/luwu/editors/nvim"

const parsed = args.parse("install.luau", "installs luwu-lsp and sets up your editors", {
    description = "Asks what to set up when run in a terminal; otherwise does what these options say.",
    examples = {
        "",
        "--editors=vscode,zed --no-roblox --yes",
    },
}):simple(
    args.named("--editors", "comma-separated editors to set up: vscode, code-oss, cursor, vscodium, zed, helix"),
    args.flag("--roblox", "turn on Roblox support (types, sourcemaps)"),
    args.flag("--no-roblox", "leave Roblox support off"),
    args.flag("--yes", "change editor configs without asking"),
    args.flag("--keep-seal", "keep the seal downloaded to run this, in ~/.local/bin or %LOCALAPPDATA%\\seal")
)

const interactive = terminal.tty("Stdin") and terminal.tty("Stdout")
const assume_yes = parsed.flags["--yes"] == true

-- Paths changed, and the backups made of them, for the summary
const changed: { string } = {}
const backups: { string } = {}

const function say(text: string)
    print(text)
end

const function warn(text: string)
    output.ewriteln(colors.yellow(text))
end

const function fail(text: string): never
    output.ewriteln(colors.red(`luwu-lsp installer: {text}`))
    return process.exit(1)
end

const function heading(text: string)
    print("")
    print(colors.bold.cyan(text))
end

----------------------------------------------------------------------------------------------------------------------
-- Platform and paths

type Platform = {
    -- The name of this platform's server zip in the release
    server: string,
    -- The name of this platform's .vsix in the release, when VS Code runs here
    vsix: string?,
    exe: string,
}

const function detect_platform(): Platform
    const system = env.os
    const arch = env.arch
    if system == "Windows" then
        if arch == "x86_64" then
            return { server = "windows-x64", vsix = "windows-x64", exe = "luwu-lsp.exe" }
        elseif arch == "aarch64" then
            return { server = "windows-arm64", vsix = "windows-arm64", exe = "luwu-lsp.exe" }
        end
    elseif system == "MacOS" then
        if arch == "aarch64" then
            return { server = "macos-universal", vsix = "macos-arm64", exe = "luwu-lsp" }
        elseif arch == "x86_64" then
            fail("Intel Macs aren't supported, since Apple no longer supports them either")
        end
    elseif system == "Linux" then
        if arch == "x86_64" then
            return { server = "linux-x64", vsix = "linux-x64", exe = "luwu-lsp" }
        elseif arch == "aarch64" then
            return { server = "linux-arm64", vsix = "linux-arm64", exe = "luwu-lsp" }
        end
    elseif system == "Android" then
        if arch == "aarch64" then
            return { server = "android-arm64", exe = "luwu-lsp" }
        elseif arch == "arm" then
            return { server = "android-armv7", exe = "luwu-lsp" }
        end
    end
    return fail(`no luwu-lsp build for {system} on {arch}`)
end

const platform = detect_platform()
const home = fs.path.home()

const function var(name: string): string?
    const value = env.vars.get(name)
    return if value and value ~= "" then value else nil
end

-- Where editors keep their config: %APPDATA%, ~/Library/Application Support or ~/.config
const function config_root(): string
    if env.os == "Windows" then
        return var("APPDATA") or fs.path.join(home, "AppData", "Roaming")
    elseif env.os == "MacOS" then
        return fs.path.join(home, "Library", "Application Support")
    end
    return var("XDG_CONFIG_HOME") or fs.path.join(home, ".config")
end

-- Where the server binary goes, for the editors that need a path to it
const function server_dir(): string
    if env.os == "Windows" then
        return fs.path.join(var("LOCALAPPDATA") or fs.path.join(home, "AppData", "Local"), "luwu-lsp")
    elseif env.os == "MacOS" then
        return fs.path.join(home, "Library", "Application Support", "luwu-lsp")
    end
    return fs.path.join(var("XDG_DATA_HOME") or fs.path.join(home, ".local", "share"), "luwu-lsp")
end

const function temp_dir(): string
    const base = var("TEMP") or var("TMP") or var("TMPDIR") or "/tmp"
    const dir = fs.path.join(base, `luwu-lsp-install-{os.time()}`)
    fs.makedir(dir, { create_missing = true, error_if_exists = false })
    return dir
end

-- The first of `names` on PATH, as a full path (on Windows that finds `code.cmd` for `code`)
const function which(names: { string }): string?
    for _, name in names do
        const found = env.where(name)
        if #found > 0 then
            return found[1]
        end
    end
    return nil
end

----------------------------------------------------------------------------------------------------------------------
-- Prompts

-- A yes/no question; Ctrl+C or Ctrl+D stops the installer
const function ask(question: string, default: boolean): boolean
    const answered, answer = pcall(prompt.confirm, question, default)
    if answered and type(answer) == "boolean" then
        return answer
    end
    print("")
    say("Cancelled.")
    return process.exit(130)
end

const function confirm(question: string, default: boolean): boolean
    if assume_yes then
        return true
    elseif not interactive then
        return false
    end
    return ask(question, default)
end

type Choice = {
    label: string,
    note: string?,
    checked: boolean,
}

-- A checklist: arrow keys (or j/k) move, space toggles, enter accepts. Key releases are ignored, since Windows reports
-- them as separate events.
const function choose_many(title: string, choices: { Choice }): { Choice }
    say(title)
    say(colors.style.dim("  up/down to move, space to toggle, enter to accept"))

    local selected = 1
    const function render(first: boolean)
        if not first then
            terminal.execute(cursor.prevline(#choices), terminal.clear("FromCursorDown"))
        end
        for index, choice in choices do
            const pointer = if index == selected then colors.cyan(">") else " "
            const box = if choice.checked then colors.green("[x]") else "[ ]"
            const note = if choice.note then colors.style.dim(` ({choice.note})`) else ""
            output.write(`{pointer} {box} {choice.label}{note}\r\n`)
        end
    end

    terminal.rawmode.enable()
    render(true)
    for event in terminal.events(time.milliseconds(50)) do
        if terminal.interrupt.check(event) then
            terminal.rawmode.disable()
            terminal.reset()
            process.exit(130)
        end
        if event.type == "Key" and event.kind ~= "Release" then
            const key = event.key
            if key == "Up" or key == "k" then
                selected = if selected == 1 then #choices else selected - 1
            elseif key == "Down" or key == "j" then
                selected = if selected == #choices then 1 else selected + 1
            elseif key == "Space" then
                choices[selected].checked = not choices[selected].checked
            elseif key == "Enter" then
                break
            end
            render(false)
        end
    end
    terminal.rawmode.disable()

    const result: { Choice } = {}
    for _, choice in choices do
        if choice.checked then
            table.insert(result, choice)
        end
    end
    return result
end

----------------------------------------------------------------------------------------------------------------------
-- Editing configs in place. seal's json.decode reads JSONC, but encoding drops comments, so a config is only read
-- through json.decode; a change splices the new value into the text where it goes, leaving the rest as it was.

const function backup(path: string)
    if fs.path.exists(path) then
        const copy = `{path}.luwu-backup-{os.date("%Y%m%d-%H%M%S")}`
        fs.copy(path, copy)
        table.insert(backups, copy)
    end
end

const function write_config(path: string, text: string)
    backup(path)
    const parent = fs.path.parent(path)
    if parent then
        fs.makedir(parent, { create_missing = true, error_if_exists = false })
    end
    fs.writefile(path, text)
    table.insert(changed, path)
end

const function char(text: string, i: number): string
    return string.sub(text, i, i)
end

-- The index of the next character that isn't whitespace or a comment
const function skip_trivia(text: string, i: number): number
    while i <= #text do
        const c = char(text, i)
        if c == " " or c == "\t" or c == "\r" or c == "\n" then
            i += 1
        elseif string.sub(text, i, i + 1) == "//" then
            const newline = string.find(text, "\n", i, true)
            i = if newline then newline + 1 else #text + 1
        elseif string.sub(text, i, i + 1) == "/*" then
            const close = string.find(text, "*/", i + 2, true)
            i = if close then close + 2 else #text + 1
        else
            break
        end
    end
    return i
end

-- The index just past the string whose `"` is at `i`
const function skip_string(text: string, i: number): number
    i += 1
    while i <= #text do
        const c = char(text, i)
        if c == "\\" then
            i += 2
        elseif c == "\"" then
            return i + 1
        else
            i += 1
        end
    end
    return i
end

-- The index just past the value starting at `i`
const function skip_value(text: string, i: number): number
    const c = char(text, i)
    if c == "\"" then
        return skip_string(text, i)
    elseif c == "{" or c == "[" then
        local depth = 0
        while i <= #text do
            i = skip_trivia(text, i)
            const d = char(text, i)
            if d == "\"" then
                i = skip_string(text, i)
            else
                if d == "{" or d == "[" then
                    depth += 1
                elseif d == "}" or d == "]" then
                    depth -= 1
                    if depth == 0 then
                        return i + 1
                    end
                end
                i += 1
            end
        end
        return i
    end
    while i <= #text and not string.find(char(text, i), "[,%]}%s]") and string.sub(text, i, i + 1) ~= "//" do
        i += 1
    end
    return i
end

type Member = {
    key: string,
    key_start: number,
    value_start: number,
    value_end: number,
}

-- The members of the object whose `{` is at `open`, and the index of its `}`
const function members_of(text: string, open: number): ({ Member }, number)
    const result: { Member } = {}
    local i = skip_trivia(text, open + 1)
    while i <= #text do
        const c = char(text, i)
        if c == "}" then
            return result, i
        elseif c == "," then
            i = skip_trivia(text, i + 1)
        elseif c == "\"" then
            const key_end = skip_string(text, i)
            const key = string.sub(text, i + 1, key_end - 2)
            local value_start = skip_trivia(text, key_end)
            if char(text, value_start) ~= ":" then
                error(`expected ':' after "{key}"`)
            end
            value_start = skip_trivia(text, value_start + 1)
            const value_end = skip_value(text, value_start)
            table.insert(result, { key = key, key_start = i, value_start = value_start, value_end = value_end })
            i = skip_trivia(text, value_end)
        else
            error(`unexpected '{c}' in an object`)
        end
    end
    error("an object isn't closed")
end

-- The whitespace a line starts with
const function indentation_at(text: string, i: number): string
    local line_start = i
    while line_start > 1 and char(text, line_start - 1) ~= "\n" do
        line_start -= 1
    end
    return string.match(text, "^[ \t]*", line_start) or ""
end

-- One level of indentation, as the file already uses it
const function indent_unit(text: string): string
    const tabs = string.match(text, "\n(\t+)[^%s]")
    if tabs then
        return "\t"
    end
    const spaces = string.match(text, "\n( +)[^%s]")
    return if spaces and #spaces <= 4 then spaces else "    "
end

const function encode_string(s: string): string
    const escaped = string.gsub(s, "[%c\"\\]", function(c: string): string
        if c == "\"" or c == "\\" then
            return "\\" .. c
        elseif c == "\n" then
            return "\\n"
        elseif c == "\t" then
            return "\\t"
        end
        return string.format("\\u%04x", string.byte(c))
    end)
    return `"{escaped}"`
end

-- `value` as JSON, its lines after the first indented to sit at `indent`. Tables with a `1` are arrays.
const function encode(value: any, indent: string, unit: string): string
    if type(value) == "string" then
        return encode_string(value)
    elseif type(value) ~= "table" then
        return tostring(value)
    elseif value[1] ~= nil then
        const items = {}
        for _, item in value do
            table.insert(items, encode(item, indent, unit))
        end
        return `[{table.concat(items, ", ")}]`
    end
    const keys = {}
    for key in value do
        table.insert(keys, key)
    end
    table.sort(keys)
    const lines = {}
    for _, key in keys do
        table.insert(lines, `{indent}{unit}{encode_string(key)}: {encode(value[key], indent .. unit, unit)}`)
    end
    return "{\n" .. table.concat(lines, ",\n") .. "\n" .. indent .. "}"
end

-- `value` as JSON on one line, for an object written on one line
const function encode_inline(value: any): string
    if type(value) ~= "table" or value[1] ~= nil then
        return encode(value, "", "")
    end
    const keys = {}
    for key in value do
        table.insert(keys, key)
    end
    table.sort(keys)
    const parts = {}
    for _, key in keys do
        table.insert(parts, `{encode_string(key)}: {encode_inline(value[key])}`)
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
end

-- `text` with a new member `key: value` at the end of the object whose `{` is at `open`. The comma goes right after the
-- value before it, and the new member on a line of its own after any comment that ends that value's line.
const function insert_member(text: string, open: number, key: string, value: any): string
    const members, close = members_of(text, open)
    const unit = indent_unit(text)
    const outer = indentation_at(text, open)
    const inner = if #members > 0 then indentation_at(text, members[1].key_start) else outer .. unit
    const member = `{encode_string(key)}: {encode(value, inner, unit)}`

    if #members == 0 then
        const between = string.sub(text, open + 1, close - 1)
        if str.trim(between) == "" then
            return string.sub(text, 1, open) .. "\n" .. inner .. member .. "\n" .. outer .. string.sub(text, close)
        end
        return string.sub(text, 1, open) .. "\n" .. inner .. member .. "," .. string.sub(text, open + 1)
    end

    const last = members[#members]

    -- An object written on one line gets the new member on that line too
    if not string.find(string.sub(text, open, close), "\n", 1, true) then
        const inline = `{encode_string(key)}: {encode_inline(value)}`
        const following = skip_trivia(text, last.value_end)
        if char(text, following) == "," then
            return string.sub(text, 1, following) .. " " .. inline .. "," .. string.sub(text, following + 1)
        end
        return string.sub(text, 1, last.value_end - 1) .. ", " .. inline .. string.sub(text, last.value_end)
    end

    local after = last.value_end
    local comma = ","
    const following = skip_trivia(text, after)
    if char(text, following) == "," then
        -- A trailing comma: the new member goes after it, with one of its own
        after = following + 1
        comma = ""
    end
    -- Keeping the trailing comma style
    const trailing = if comma == "" then "," else ""
    const line_end = string.find(text, "\n", after, true)
    const rest_of_line = if line_end then string.sub(text, after, line_end - 1) else nil
    if
        line_end
        and rest_of_line
        and (str.trim(rest_of_line) == "" or string.match(rest_of_line, "^%s*//"))
    then
        return string.sub(text, 1, after - 1) .. comma .. rest_of_line .. "\n" .. inner .. member .. trailing
            .. string.sub(text, line_end)
    end
    return string.sub(text, 1, after - 1) .. comma .. "\n" .. inner .. member .. trailing .. string.sub(text, after)
end

-- `text` with the value at `path` set to `value`, creating the objects on the way
const function set_json(text: string, path: { string }, value: any): string
    if str.trim(text) == "" then
        text = "{}\n"
    end
    local open = skip_trivia(text, 1)
    if char(text, open) ~= "{" then
        error("the file isn't a JSON object")
    end
    for index, key in path do
        const members = members_of(text, open)
        local found: Member? = nil
        for _, member in members do
            if member.key == key then
                found = member
            end
        end
        if not found then
            -- The rest of the path, as nested objects
            local nested = value
            for rest = #path, index + 1, -1 do
                nested = { [path[rest]] = nested }
            end
            return insert_member(text, open, key, nested)
        end
        if index == #path or char(text, found.value_start) ~= "{" then
            local nested = value
            for rest = #path, index + 1, -1 do
                nested = { [path[rest]] = nested }
            end
            const indent = indentation_at(text, found.key_start)
            return string.sub(text, 1, found.value_start - 1) .. encode(nested, indent, indent_unit(text))
                .. string.sub(text, found.value_end)
        end
        open = found.value_start
    end
    return text
end

-- `text` without the member at `path`, if it's there
const function remove_json(text: string, path: { string }): string
    local open = skip_trivia(text, 1)
    for index, key in path do
        const members = members_of(text, open)
        for position, member in members do
            if member.key == key then
                if index < #path then
                    open = member.value_start
                    break
                end
                if position < #members then
                    return string.sub(text, 1, member.key_start - 1) .. string.sub(text, members[position + 1].key_start)
                elseif position > 1 then
                    return string.sub(text, 1, members[position - 1].value_end - 1) .. string.sub(text, member.value_end)
                end
                return string.sub(text, 1, member.key_start - 1) .. string.sub(text, member.value_end)
            end
            if position == #members then
                return text
            end
        end
    end
    return text
end

-- A config's text and what it holds, or an empty object for a file that isn't there
const function read_config(path: string): (string, { [string]: any })
    if not fs.path.exists(path) then
        return "{}\n", {}
    end
    const text = fs.readfile(path)
    const ok, data = pcall(json.decode, text)
    if not ok or type(data) ~= "table" then
        fail(`couldn't read {path}; fix it or move it aside, then run the installer again`)
    end
    return text, data :: any
end

const function get(data: any, path: { string }): any
    for _, key in path do
        if type(data) ~= "table" then
            return nil
        end
        data = data[key]
    end
    return data
end

----------------------------------------------------------------------------------------------------------------------
-- Downloads

local tmp: string? = nil

const function download(name: string): string
    if not tmp then
        tmp = temp_dir()
    end
    const url = `{RELEASE}/{name}`
    const request: http.HttpRequestWithoutBody = {
        url = url,
        headers = { ["User-Agent"] = "luwu-lsp-installer" },
        max_body_size = filesize.megabytes(200),
    }
    const response = http.request("GET", request)
    if response.kind == "Ok" then
        const path = fs.path.join(tmp :: string, name)
        fs.writefile(path, response.body)
        return path
    elseif response.kind == "Error" then
        return fail(`couldn't download {url}: {response.status.code} {response.status.reason}`)
    end
    return fail(`couldn't download {url}: {response.reason}`)
end

local installed_server: string? = nil

-- The server binary, downloaded once into server_dir()
const function install_server(): string
    if installed_server then
        return installed_server
    end
    say(`Downloading luwu-lsp-{platform.server}.zip...`)
    const archive = download(`luwu-lsp-{platform.server}.zip`)
    const unpacked = fs.path.join(tmp :: string, "server")
    zip.extract(archive, unpacked)

    const dir = server_dir()
    fs.makedir(dir, { create_missing = true, error_if_exists = false })
    const target = fs.path.join(dir, platform.exe)
    -- A server an editor is running can't be replaced on Windows
    const removed, status = fs.file.try_remove(target)
    if not removed and status ~= "NotFound" then
        fail(`couldn't replace {target}; close the editors using it and run the installer again`)
    end
    fs.copy(fs.path.join(unpacked, platform.exe), target)
    if env.os ~= "Windows" then
        process.run({ program = "chmod", args = { "+x", target } })
    end
    say(`Installed the server at {colors.style.bold(target)}`)
    installed_server = target
    return target
end

local vsix: string? = nil

const function download_vsix(): string
    if not vsix then
        say(`Downloading luwu-lsp-{platform.vsix}.vsix...`)
        vsix = download(`luwu-lsp-{platform.vsix}.vsix`)
    end
    return vsix :: string
end

----------------------------------------------------------------------------------------------------------------------
-- Editors

type Editor = {
    id: string,
    name: string,
    kind: "vscode" | "zed" | "helix",
    cli: string?,
    -- The config file the installer changes
    config: string,
    present: boolean,
}

const function vscode_like(id: string, name: string, commands: { string }, folder: string): Editor
    const cli = which(commands)
    const editor: Editor = {
        id = id,
        name = name,
        kind = "vscode",
        cli = cli,
        config = fs.path.join(config_root(), folder, "User", "settings.json"),
        present = cli ~= nil,
    }
    return editor
end

const function find_editors(): { Editor }
    const editors: { Editor } = {}
    -- VS Code and its forks run wherever there's a .vsix for them, which isn't Android
    if platform.vsix then
        table.insert(editors, vscode_like("vscode", "VS Code", { "code" }, "Code"))
        table.insert(editors, vscode_like("code-oss", "Code - OSS", { "code-oss" }, "Code - OSS"))
        table.insert(editors, vscode_like("cursor", "Cursor", { "cursor" }, "Cursor"))
        table.insert(editors, vscode_like("vscodium", "VSCodium", { "codium" }, "VSCodium"))
    end

    -- Zed keeps its config in ~/.config/zed on macOS too
    const zed_dir = if env.os == "Windows"
        then fs.path.join(config_root(), "Zed")
        else fs.path.join(var("XDG_CONFIG_HOME") or fs.path.join(home, ".config"), "zed")
    const zed_cli = which({ "zed", "zeditor", "zedit" })
    const zed: Editor = {
        id = "zed",
        name = "Zed",
        kind = "zed",
        cli = zed_cli,
        config = fs.path.join(zed_dir, "settings.json"),
        present = zed_cli ~= nil or fs.path.exists(zed_dir),
    }
    table.insert(editors, zed)

    const helix_dir = if env.os == "Windows"
        then fs.path.join(config_root(), "helix")
        else fs.path.join(var("XDG_CONFIG_HOME") or fs.path.join(home, ".config"), "helix")
    const helix_cli = which({ "hx", "helix" })
    const helix: Editor = {
        id = "helix",
        name = "Helix",
        kind = "helix",
        cli = helix_cli,
        config = fs.path.join(helix_dir, "languages.toml"),
        present = helix_cli ~= nil,
    }
    table.insert(editors, helix)
    return editors
end

-- Editors that highlight with tree-sitter, where Luwu files get Luau's grammar for now
const function treesitter_notice(name: string)
    warn(`Sorry, we don't support syntax highlighting for {name} right now, but we're working on it!`
        .. " Luwu files use Luau's highlighting there until then.")
end

const function setup_vscode(editor: Editor, roblox: boolean)
    heading(editor.name)
    const cli = editor.cli
    if not cli then
        warn(`Couldn't find {editor.name}'s command line tool on PATH, so the extension wasn't installed.`)
        return
    end

    -- JohnnyMorganz's extension would start a second server on the same files
    const listed = process.run({ program = cli, args = { "--list-extensions" } })
    if listed.ok and string.find(string.lower(listed.out), "johnnymorganz.luau-lsp", 1, true) then
        say("The Luau Language Server extension (JohnnyMorganz.luau-lsp) is installed. Both would start a server on the same files.")
        if confirm(`Uninstall it from {editor.name}?`, true) then
            process.run({ program = cli, args = { "--uninstall-extension", "johnnymorganz.luau-lsp" } })
            say("Uninstalled it.")
        else
            warn("Kept it. Disable it in workspaces where you use Luwu.")
        end
    end

    const result = process.run({ program = cli, args = { "--install-extension", download_vsix(), "--force" } })
    if not result.ok then
        warn(`Installing the extension failed: {result.err}`)
        return
    end
    say(`Installed the Luwu extension in {editor.name}.`)

    const platform_type = if roblox then "roblox" else "standard"
    const text, data = read_config(editor.config)
    const current = data["luwu.platform.type"] or data["luau-lsp.platform.type"] or "standard"
    if current ~= platform_type then
        if confirm(`Set "luwu.platform.type" to "{platform_type}" in {editor.config}?`, true) then
            write_config(editor.config, set_json(text, { "luwu.platform.type" }, platform_type))
        else
            warn(`Skipped. Set "luwu.platform.type": "{platform_type}" yourself to turn Roblox support {if roblox then "on" else "off"}.`)
        end
    end
end

-- Where Zed keeps its extensions, each installed one in a folder of `installed/` named for its id
const function zed_extensions_dir(): string
    if env.os == "Windows" then
        return fs.path.join(var("LOCALAPPDATA") or fs.path.join(home, "AppData", "Local"), "Zed", "extensions")
    elseif env.os == "MacOS" then
        return fs.path.join(home, "Library", "Application Support", "Zed", "extensions")
    end
    return fs.path.join(var("XDG_DATA_HOME") or fs.path.join(home, ".local", "share"), "zed", "extensions")
end

-- The Luwu extension's source, unpacked into server_dir(). It isn't published yet, so Zed installs it as a dev
-- extension and compiles it itself. An earlier unpacking goes first, Zed's build output from it included, so Zed
-- builds the new source from scratch.
const function install_zed_extension(): string
    say("Downloading luwu-zed-extension.zip...")
    const archive = download("luwu-zed-extension.zip")
    const target = fs.path.join(server_dir(), "zed-extension")
    fs.dir.try_remove(target)
    zip.extract(archive, target)
    say(`Unpacked the Zed extension at {colors.style.bold(target)}`)
    return target
end

const function setup_zed(editor: Editor, roblox: boolean)
    heading(editor.name)
    const server = install_server()
    const extension = install_zed_extension()
    local text, data = read_config(editor.config)

    -- Zed's own `lsp.luwu-lsp.binary` launches the server with exactly its arguments, and the extension's settings
    -- (definitions, Roblox, fflags) never reach it
    if get(data, { "lsp", "luwu-lsp", "binary" }) ~= nil then
        say("Your settings set lsp.luwu-lsp.binary, which makes Zed skip the extension's settings (definitions, Roblox).")
        if confirm("Remove it? The extension's binary.path, set below, replaces it.", true) then
            text = remove_json(text, { "lsp", "luwu-lsp", "binary" })
        end
    end

    const edits: { { path: { string }, value: any } } = {
        { path = { "lsp", "luwu-lsp", "settings", "binary", "path" }, value = server },
        { path = { "lsp", "luwu-lsp", "settings", "roblox", "enabled" }, value = roblox },
    }
    -- Earlier versions of this installer set Zed up with zed-luau: they turned on its auto-install and opened .luwu
    -- files as Luau. That mapping would override the Luwu extension's own Luwu language.
    if get(data, { "auto_install_extensions", "luau" }) == true then
        table.insert(edits, { path = { "auto_install_extensions", "luau" }, value = false })
    end
    local opens_luwu_as_luau = false
    -- Copied item by item: json.decode's arrays have a protected metatable, which table.clone refuses
    const luau_types: { string } = {}
    const file_types = get(data, { "file_types", "Luau" })
    if type(file_types) == "table" then
        for _, item in file_types do
            if item == "luwu" then
                opens_luwu_as_luau = true
            else
                table.insert(luau_types, item)
            end
        end
    end

    say("This sets lsp.luwu-lsp.settings.binary.path to the server and Roblox support"
        .. ` {if roblox then "on" else "off"}{if opens_luwu_as_luau then ", and stops opening .luwu files as Luau" else ""}.`)
    if confirm(`Update {editor.config}?`, true) then
        for _, edit in edits do
            text = set_json(text, edit.path, edit.value)
        end
        -- Removing the only file type mapping removes the `file_types` it was in
        local mappings = 0
        if opens_luwu_as_luau then
            for _ in get(data, { "file_types" }) do
                mappings += 1
            end
        end
        if opens_luwu_as_luau and #luau_types == 0 and mappings == 1 then
            text = remove_json(text, { "file_types" })
        elseif opens_luwu_as_luau and #luau_types == 0 then
            text = remove_json(text, { "file_types", "Luau" })
        elseif opens_luwu_as_luau then
            text = set_json(text, { "file_types", "Luau" }, luau_types)
        end
        write_config(editor.config, text)
    else
        warn(`Skipped. Point lsp.luwu-lsp.settings.binary.path at {server} yourself.`)
    end

    if get(data, { "lsp", "luau-lsp" }) ~= nil then
        warn("Your lsp.luau-lsp settings are zed-luau's. Move any definitions or fflags you set there to lsp.luwu-lsp,"
            .. " where the Luwu extension reads them.")
    end
    if fs.path.exists(fs.path.join(zed_extensions_dir(), "installed", "luau")) then
        warn("zed-luau is installed. Uninstall it (zed: extensions, then Luau): with both installed, Zed starts two servers"
            .. " for .luau files.")
    end
    if not which({ "cargo" }) then
        warn("cargo isn't on your PATH. Zed needs it to compile the extension: install Rust from https://rustup.rs first.")
    end
    say(`In Zed, run {colors.style.bold("zed: install dev extension")} and pick {colors.style.bold(extension)}.`
        .. ` After later updates, run {colors.style.bold("zed: rebuild dev extension")} instead.`)
end

-- The lines of a TOML table, from its header to the next one
const function toml_table_bounds(lines: { string }, header: string): (number?, number?)
    for index, line in lines do
        if str.trim(line) == header then
            local last = #lines
            for after = index + 1, #lines do
                if string.match(lines[after], "^%s*%[") then
                    last = after - 1
                    break
                end
            end
            while last > index and str.trim(lines[last]) == "" do
                last -= 1
            end
            return index, last
        end
    end
    return nil, nil
end

const function setup_helix(editor: Editor, roblox: boolean)
    heading(editor.name)
    const server = install_server()
    const text = if fs.path.exists(editor.config) then fs.readfile(editor.config) else ""
    const lines = str.splitlines(text)

    const server_block = {
        "[language-server.luwu-lsp]",
        `command = {encode_string(server)}`,
        `args = ["lsp"]`,
        `config = \{ "luau-lsp" = \{ platform = \{ type = "{if roblox then "roblox" else "standard"}" } } }`,
    }
    const first, last = toml_table_bounds(lines, "[language-server.luwu-lsp]")
    if first and last then
        for _ = first, last do
            table.remove(lines, first)
        end
        for offset, line in server_block do
            table.insert(lines, first + offset - 1, line)
        end
    else
        if #lines > 0 and lines[#lines] ~= "" then
            table.insert(lines, "")
        end
        for _, line in server_block do
            table.insert(lines, line)
        end
    end

    -- Helix's Luau language, served by luwu-lsp and opening .luwu files. A `[[language]]` for Luau that's already
    -- there gets the server; anything else in it is left alone.
    local language_line: number? = nil
    for index, line in lines do
        if string.match(line, "^%s*name%s*=%s*\"luau\"") then
            language_line = index
        end
    end
    if language_line then
        -- Lines already in the block change in place; the ones it lacks go right after its `name`
        local has_servers = false
        local has_types = false
        local index = language_line + 1
        while index <= #lines and not string.match(lines[index], "^%s*%[") do
            if string.match(lines[index], "^%s*language%-servers%s*=") then
                lines[index] = `language-servers = ["luwu-lsp"]`
                has_servers = true
            elseif string.match(lines[index], "^%s*file%-types%s*=") then
                if not string.find(lines[index], "\"luwu\"", 1, true) then
                    lines[index] = string.gsub(lines[index], "%]%s*$", ", \"luwu\"]", 1)
                end
                has_types = true
            end
            index += 1
        end
        if not has_servers then
            table.insert(lines, language_line + 1, `language-servers = ["luwu-lsp"]`)
        end
        if not has_types then
            table.insert(lines, language_line + 1, `file-types = ["luau", "luwu"]`)
        end
    else
        table.insert(lines, "")
        table.insert(lines, "[[language]]")
        table.insert(lines, `name = "luau"`)
        table.insert(lines, `file-types = ["luau", "luwu"]`)
        table.insert(lines, `language-servers = ["luwu-lsp"]`)
    end

    say("This adds a luwu-lsp language server and makes it Luau's, opening .luwu files as Luau.")
    if confirm(`Update {editor.config}?`, true) then
        write_config(editor.config, table.concat(lines, "\n") .. "\n")
    else
        warn(`Skipped. Add a language server for Luau with command = "{server}" and args = ["lsp"] yourself.`)
    end
    if roblox then
        warn("Roblox's types need a definitions file in Helix: pass the server --definitions:@roblox=<path> in its args.")
    end
    treesitter_notice(editor.name)
end

----------------------------------------------------------------------------------------------------------------------

const function keep_seal()
    if env.vars.get("LUWU_INSTALL_TEMPORARY_SEAL") ~= "1" then
        say("--keep-seal: the seal running this was already installed.")
        return
    end
    const dir = if env.os == "Windows"
        then fs.path.join(var("LOCALAPPDATA") or fs.path.join(home, "AppData", "Local"), "seal")
        else fs.path.join(home, ".local", "bin")
    fs.makedir(dir, { create_missing = true, error_if_exists = false })
    const target = fs.path.join(dir, if env.os == "Windows" then "seal.exe" else "seal")
    fs.copy(env.executable_path, target)
    if env.os ~= "Windows" then
        process.run({ program = "chmod", args = { "+x", target } })
    end
    say(`Kept seal at {target}.`)
    const path = var("PATH") or ""
    const separator = if env.os == "Windows" then ";" else ":"
    if not table.find(str.split(path, separator), dir) then
        say(`Add {dir} to your PATH to run it as \`seal\`.`)
    end
end

const function main(): boolean
    print(colors.style.bold("luwu-lsp installer"))
    const editors = find_editors()

    -- Which editors to set up
    local chosen: { Editor } = {}
    const requested = parsed:get("editors") :: string?
    if requested then
        for _, id in str.split(requested, ",") do
            local match: Editor? = nil
            for _, editor in editors do
                if editor.id == str.trim(id) then
                    match = editor
                end
            end
            if not match then
                fail(`unknown editor "{id}"; pick from vscode, code-oss, cursor, vscodium, zed, helix`)
            end
            table.insert(chosen, match :: Editor)
        end
    else
        const present: { Editor } = {}
        for _, editor in editors do
            if editor.present then
                table.insert(present, editor)
            end
        end
        if #present == 0 then
            say("No supported editor found (VS Code, Code - OSS, Cursor, VSCodium, Zed or Helix).")
        elseif interactive then
            const choices: { Choice } = {}
            for _, editor in present do
                table.insert(choices, { label = editor.name, checked = true })
            end
            const picked = choose_many("Which editors should use luwu-lsp?", choices)
            for _, choice in picked do
                for _, editor in present do
                    if editor.name == choice.label then
                        table.insert(chosen, editor)
                    end
                end
            end
        else
            chosen = present
        end
    end

    if which({ "nvim" }) then
        warn(`Neovim isn't set up by this installer yet: luau-lsp.nvim only loads a project's definition files with`
            .. ` 'exrc' and 'secure = false', which lets any project run code. See {NVIM_NOTES}.`)
    end

    if #chosen == 0 then
        say("Nothing to set up.")
    else
        -- Roblox support
        local roblox = false
        if parsed.flags["--roblox"] then
            roblox = true
        elseif not parsed.flags["--no-roblox"] and interactive then
            roblox = ask("Turn on Roblox support (Roblox types, Rojo sourcemaps)?", false)
        end

        for _, editor: Editor in chosen do
            if editor.kind == "vscode" then
                setup_vscode(editor, roblox)
            elseif editor.kind == "zed" then
                setup_zed(editor, roblox)
            else
                setup_helix(editor, roblox)
            end
        end
    end

    if parsed.flags["--keep-seal"] then
        keep_seal()
    end

    heading("Done")
    if #changed > 0 then
        say("Changed:")
        for _, path in changed do
            say(`  {path}`)
        end
    end
    if #backups > 0 then
        say("Backups of what was there before:")
        for _, path in backups do
            say(`  {path}`)
        end
    end
    if not interactive and not assume_yes and #chosen > 0 then
        say("Configs were left alone without a terminal to ask in; pass --yes to change them.")
    end
    say(`Sublime Text and bat can highlight Luwu with the Sublime syntax at {SUBLIME_SYNTAX}.`)
    say("Restart your editors to start using luwu-lsp.")
    return true
end

const ok, problem = pcall(main)
if tmp then
    fs.dir.try_remove(tmp)
end
if not ok then
    fail(tostring(problem))
end
