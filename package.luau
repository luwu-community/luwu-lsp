local args = require("@std/args")
local entry = require("@std/archive/entry")
local tar = require("@std/archive/tar")
local env = require("@std/env")
local fs = require("@std/fs")
local process = require("@std/process")

const parsed = args.parse(
    "package",
    "Bundle a stripped luwu-lsp and the Luwu .vsix into a .tar.gz"
):simple(
    args.flag("--static", "Package the fully static binary from ./build-static (seal ./rebuild.luau --static) instead of ./build")
)

const dir = fs.dir.from(script:parent())
const build_dir_name = if parsed.flags["--static"] then "build-static" else "build"
const server_path = dir:join(build_dir_name, "luwu-lsp")
const vsix_path = dir:join("editors", "code", "luwu-1.69.0.vsix")

if not fs.path.exists(server_path) then
    error(`No luwu-lsp binary in ./{build_dir_name}; build it first with seal ./rebuild.luau{
        if parsed.flags["--static"] then " --static" else ""
    }`)
end

if #env.where("strip") == 0 then
    error("Missing strip in $PATH; it's needed to drop the debug info (~275MB) from luwu-lsp")
end

-- Strip a copy: the binary in ./build is usually the one your editor is running
const stripped_path = dir:join(build_dir_name, "luwu-lsp.stripped")
const strip_result = process.run {
    program = "strip",
    args = { "-o", stripped_path, server_path },
}
if not strip_result.ok then
    error(`strip failed:\n{strip_result.stdout}{strip_result.stderr}`)
end

const new = tar.gz.create()
new:append(
    entry.read(stripped_path, "luwu-lsp"),
    entry.read(vsix_path)
)

print(new)
tar.gz.writefile(`./luwu-linux-{env.arch}.tar.gz`, new, {
    compression_level = tar.gz.compression(8)
})

fs.file.try_remove(stripped_path)
