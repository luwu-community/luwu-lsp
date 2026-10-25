--[=[
    Prints what luwu-lsp shows on hover at each probe.

        seal scripts/lsp-client/hover.luau <dir> <file>:<needle>[:<occurrence>] ... [--bin <luwu-lsp>]

    `dir` is the workspace folder, and every source file directly in it is opened. A probe hovers the last identifier in
    the `occurrence`th match (default 1) of `needle` in `file`:

        seal scripts/lsp-client/hover.luau ./probe 'main.luwu:f()' user.luwu:m.g:2
]=]

const env = require("@std/env")
const fs = require("@std/fs")
const lsp = require("./client")

type Probe = {
    file: string,
    needle: string,
    occurrence: number,
}

const USAGE = "usage: seal scripts/lsp-client/hover.luau <dir> <file>:<needle>[:<occurrence>] ... [--bin <luwu-lsp>]"

--- `file:needle` or `file:needle:occurrence`, or nil when it's neither
const function parse_probe(text: string): Probe?
    const file, needle, occurrence = string.match(text, "^([^:]+):(.+):(%d+)$")
    if file and needle then
        return {
            file = file,
            needle = needle,
            occurrence = tonumber(occurrence) or 1,
        }
    end

    const plain_file, plain_needle = string.match(text, "^([^:]+):(.+)$")
    if plain_file and plain_needle then
        return {
            file = plain_file,
            needle = plain_needle,
            occurrence = 1,
        }
    end

    return nil
end

const args = env.args
if #args < 2 then
    print(USAGE)
    return
end

local bin: string? = nil
const probes: { Probe } = {}
local i = 2
while i <= #args do
    if args[i] == "--bin" then
        bin = args[i + 1]
        i += 2
        continue
    end

    const probe = parse_probe(args[i])
    if not probe then
        error(`'{args[i]}' isn't a probe; {USAGE}`)
    end
    table.insert(probes, probe)
    i += 1
end

const client = lsp.start({
    root = fs.path.canonicalize(args[1]),
    bin = bin,
})
client:open_dir()

for _, probe in probes do
    const position = client:position_of(probe.file, probe.needle, probe.occurrence)
    print(`## {probe.file}:{probe.needle} (line {position.line + 1})`)
    print(client:hover(probe.file, position) or "(no hover)")
    print()
end

client:stop()
