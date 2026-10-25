--[=[
    A small LSP client for driving luwu-lsp from seal scripts. It starts the server over stdio, opens files and sends
    requests, so a script can check what an editor would show.

    ```luau
    const lsp = require("./client")

    const client = lsp.start({ root = "/path/to/project" })
    client:open_dir()
    print(client:hover("main.luwu", client:position_of("main.luwu", "foo()")))
    client:stop()
    ```

    Files are named relative to `root`. Positions are LSP positions, with zero-based lines and characters.
]=]

const process = require("@std/process")
const fs = require("@std/fs")
const json = require("@std/serde/json")
const str = require("@std/str")
const time = require("@std/time")

--- This checkout's build of the server
const DEFAULT_BIN = fs.path.join(script:parent(), "..", "..", "build", "luwu-lsp")
--- How long to wait for the server before giving up on it
const DEFAULT_TIMEOUT = time.seconds(30)

export type StartOptions = {
    --- The workspace folder the server opens
    root: string,
    --- The luwu-lsp binary, by default this checkout's build/luwu-lsp
    bin: string?,
    --- Extra arguments after `lsp`, such as `--flag:Name=true`
    args: { string }?,
    --- How long to wait for each message before giving up on the server
    timeout: Duration?,
}

export type Position = {
    line: number,
    character: number,
}

export type Range = {
    start: Position,
    ["end"]: Position,
}

export type Location = {
    uri: string,
    range: Range,
}

export type Diagnostic = {
    range: Range,
    message: string,
    code: (string | number)?,
}

--- A message's fields, as JSON
type Params = { [string]: unknown }

--- The server's answer to one of the client's requests
type Response = {
    id: number,
    method: nil,
    result: unknown,
}

--- A request from the server, which the client has to answer
type ServerRequest = {
    id: number,
    method: string,
    params: unknown,
}

--- A notification from the server, which needs no answer
type Notification = {
    id: nil,
    method: string,
    params: unknown,
}

type Message = Response | ServerRequest | Notification

--- Formatted text, which is what luwu-lsp's hovers hold
type MarkupContent = {
    value: string,
}

type Hover = {
    contents: MarkupContent | string,
}

type CompletionItem = {
    label: string,
}

type DiagnosticReport = {
    items: { Diagnostic },
}

type PublishDiagnostics = {
    uri: string,
    diagnostics: { Diagnostic },
}

--- One setting the server asks for in workspace/configuration. The client answers each with an empty table.
type ConfigurationItem = unknown

type ConfigurationParams = {
    items: { ConfigurationItem },
}

--- The diagnostics the server last published for one file
type FileDiagnostics = { Diagnostic }

type ClientData = {
    root: string,
    root_uri: string,
    child: process.ChildProcess,
    timeout: Duration,
    next_id: number,
    --- The text of each open file
    texts: { [string]: string },
    --- Published diagnostics by file uri
    published: { [string]: FileDiagnostics },
}

const function write_message(self: ClientData, message: Params)
    const body = json.encode(message)
    const stdin = assert(self.child.stdin, "luwu-lsp's stdin isn't piped")
    stdin:write(`Content-Length: {#body}\r\n\r\n{body}`)
    stdin:flush()
end

--- A message's header, which ends at a blank line, or nil when the server stopped or went quiet
const function read_header(self: ClientData): string?
    const stdout = assert(self.child.stdout, "luwu-lsp's stdout isn't piped")
    const bytes: { string } = {}
    while #bytes < 4 or table.concat(bytes, "", #bytes - 3) ~= "\r\n\r\n" do
        const byte = stdout:read_exact(1, self.timeout)
        if not byte then
            return nil
        end
        table.insert(bytes, byte)
    end
    return table.concat(bytes)
end

--- A message's body. read_exact gives up as soon as part of a long body has arrived, so it's read in pieces.
const function read_body(self: ClientData, length: number): string?
    const stdout = assert(self.child.stdout, "luwu-lsp's stdout isn't piped")
    const parts: { string } = {}
    local remaining = length
    while remaining > 0 do
        const part = stdout:read(remaining, self.timeout)
        if not part then
            return nil
        end
        table.insert(parts, part)
        remaining -= #part
    end
    return table.concat(parts)
end

--- The next message from the server, or nil when it stopped or went quiet for longer than the timeout
const function read_message(self: ClientData): Message?
    const header = read_header(self)
    const length = if header then tonumber(string.match(header, "Content%-Length: (%d+)")) else nil
    if not length then
        return nil
    end

    const body = read_body(self, length)
    return if body then json.decode(body) :: Message else nil
end

--- Handles a message from the server that isn't a response. Published diagnostics are kept for `diagnostics`, and a
--- request from the server gets an empty answer.
const function handle_server_message(self: ClientData, message: ServerRequest | Notification)
    if message.method == "textDocument/publishDiagnostics" then
        const published = message.params :: PublishDiagnostics
        self.published[published.uri] = published.diagnostics
    end

    if message.id == nil then
        return
    end

    -- workspace/configuration wants one answer per item it asks for
    const answers = {}
    if message.method == "workspace/configuration" then
        for _ in (message.params :: ConfigurationParams).items do
            table.insert(answers, {})
        end
    end

    write_message(self, {
        jsonrpc = "2.0",
        id = message.id,
        result = answers,
    })
end

const function text_document_position(uri: string, position: Position): Params
    return {
        textDocument = { uri = uri },
        position = position,
    }
end

const function is_source(name: string): boolean
    return str.endswith(name, ".luwu") or str.endswith(name, ".luau") or str.endswith(name, ".lua")
end

--- Where the `occurrence`th match of `needle` starts in `text`, or nil when there are fewer
const function find_occurrence(text: string, needle: string, occurrence: number): number?
    local at: number? = nil
    for _ = 1, occurrence do
        at = string.find(text, needle, if at then at + 1 else 1, true)
        if not at then
            return nil
        end
    end
    return at
end

--- The LSP position of the byte at `index` in `text`
const function position_at(text: string, index: number): Position
    const before = string.sub(text, 1, index - 1)
    const _, line = string.gsub(before, "\n", "")
    const line_start = (string.find(before, "\n[^\n]*$") or 0) + 1
    return {
        line = line,
        character = index - line_start,
    }
end

const Client = {}
Client.__index = Client
export type Client = setmetatable<ClientData, typeof(Client)>

--- Sends a request and returns its result, or nil when there's none or the server stopped answering
function Client.request(self: Client, method: string, params: Params?): unknown
    self.next_id += 1
    const id = self.next_id
    write_message(self, {
        jsonrpc = "2.0",
        id = id,
        method = method,
        params = params,
    })

    while true do
        const message = read_message(self)
        if not message then
            return nil
        end

        if message.method == nil then
            -- a response, possibly to a request that timed out earlier
            if message.id == id then
                return message.result
            end
            continue
        end

        handle_server_message(self, message)
    end
end

function Client.notify(self: Client, method: string, params: Params?)
    write_message(self, {
        jsonrpc = "2.0",
        method = method,
        params = params,
    })
end

function Client.uri(self: Client, file: string): string
    return `{self.root_uri}/{file}`
end

--- Opens `file` (relative to the root) in the server, as an editor does, and returns its text
function Client.open(self: Client, file: string): string
    const text = fs.readfile(fs.path.join(self.root, file))
    self.texts[file] = text

    const language = if str.endswith(file, ".luwu") then "luwu" else "luau"
    self:notify("textDocument/didOpen", {
        textDocument = {
            uri = self:uri(file),
            languageId = language,
            version = 1,
            text = text,
        },
    })
    return text
end

--- Opens every `.luwu`, `.luau` and `.lua` file directly in the root
function Client.open_dir(self: Client)
    for _, entry in fs.entries(self.root) do
        if entry.type == "File" and is_source(entry.name) then
            self:open(entry.name)
        end
    end
end

--[=[
    The position of the last identifier in the `occurrence`th match of `needle` in a file, which is what a hover or a
    definition request usually wants: `"m.g()"` gives the position of `g`.
]=]
function Client.position_of(self: Client, file: string, needle: string, occurrence: number?): Position
    const text = self.texts[file] or self:open(file)
    const at = find_occurrence(text, needle, occurrence or 1)
    if not at then
        error(`'{needle}' occurs fewer than {occurrence or 1} times in {file}`)
    end

    const identifier_offset = tonumber(string.match(needle, "()[%a_][%w_]*[^%w_]*$")) or 1
    return position_at(text, at + identifier_offset - 1)
end

--- The text of the hover at a position, or nil when there is none
function Client.hover(self: Client, file: string, position: Position): string?
    const hover = self:request("textDocument/hover", text_document_position(self:uri(file), position)) :: Hover?
    if not hover then
        return nil
    end
    return if type(hover.contents) == "string" then hover.contents else hover.contents.value
end

--- The labels of the completions at a position. luwu-lsp answers with a plain list of items.
function Client.completion(self: Client, file: string, position: Position): { string }
    const items = self:request("textDocument/completion", text_document_position(self:uri(file), position)) :: { CompletionItem }?
    if not items then
        return {}
    end

    const labels = {}
    for _, item in items do
        table.insert(labels, item.label)
    end
    return labels
end

--- Where the symbol at a position is defined, or nil
function Client.definition(self: Client, file: string, position: Position): Location?
    const locations = self:request("textDocument/definition", text_document_position(self:uri(file), position)) :: { Location }?
    return if locations then locations[1] else nil
end

--- The diagnostics for an open file, pulled from the server, or the last ones it published when it doesn't support pulls
function Client.diagnostics(self: Client, file: string): { Diagnostic }
    const report = self:request("textDocument/diagnostic", { textDocument = { uri = self:uri(file) } }) :: DiagnosticReport?
    if report then
        return report.items
    end
    return self.published[self:uri(file)] or {}
end

--- Asks the server to shut down, then stops it
function Client.stop(self: Client)
    self:request("shutdown")
    self:notify("exit")
    self.child:kill()
end

--- Starts luwu-lsp over stdio with `root` as its workspace folder
const function start(options: StartOptions): Client
    const args = { "lsp" }
    for _, arg in options.args or {} do
        table.insert(args, arg)
    end

    const child = process.spawn({
        program = options.bin or DEFAULT_BIN,
        args = args,
        cwd = options.root,
    })
    const root_uri = `file://{options.root}`
    const self = setmetatable({
        root = options.root,
        root_uri = root_uri,
        child = child,
        timeout = options.timeout or DEFAULT_TIMEOUT,
        next_id = 0,
        texts = {},
        published = {},
    }, Client)

    const workspace_folder = {
        uri = root_uri,
        name = "lsp-client",
    }
    self:request("initialize", {
        rootUri = root_uri,
        capabilities = {},
        workspaceFolders = { workspace_folder },
    })
    self:notify("initialized", {})
    return self
end

return {
    start = start,
}
