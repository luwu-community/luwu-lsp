--!strict
-- run with seal 0.8.x
-- Usage: see seal ./rebuild.luau --help
const args = require("@std/args")
const env = require("@std/env")
const fs = require("@std/fs")
const colors = require("@std/io/colors")
const output = require("@std/io/output")
const process = require("@std/process")
const str = require("@std/str")
const terminal = require("@std/terminal")
const cursor = require("@std/terminal/cursor")
const thread = require("@std/thread")
const time = require("@std/time")

const generate_keywords = require("./scripts/generate_keywords")

const eol4 = "\n    "
const eol2 = "\n  "

const parsed = args.parse(
    "rebuild",
    "Rebuild luwu-lsp easily in development, testing, and release workflows.",
    {
        description = "\z
        - Pass --release if you just want to compile luwu-lsp\n\z
        - Explicitly specify a luwu dir with --luwu or the LUWU_TEST_PATH env variable"
    }
):commands(
    args.default(
        args.named(
            "--luwu",
            `Compile against a specific luwu directory path{eol4}\z
            - defaults to using LUWU_TEST_PATH.{eol4}\z
            - Pass '--luwu=here' to explicitly specify using the luwu path inside this directory if overriden by LUWU_TEST_PATH`
        ):default("test"),
        args.flag("--release", "Compiles the luwu-lsp binary in --config Release mode (defaults to false)"),
        args.flag("--clean", "remove the build dir and reconfigure (do this luwu-lsp doesnt compile and you think partial build is stale)"),
        args.named("--targets", "Which of the CMake build targets should we build? Defaults to CLI (Luwu.LanguageServer.CLI). Specify multiple targets by separating them with commas.")
            :default("CLI")
    ),
    args.command("submodule-update", "update ./luwu submodule with whatever path")
)

const luwu_lsp_dir =
    -- use instead of cwd in case ppl dont run this in correct dir
    fs.dir.from(script:parent())

const build_dir_path = luwu_lsp_dir:join("build")

--- Version of luwu vendored in as a submodule, should always be included
--- we build against this version by default when LUWU_TEST_PATH doesn't exist
const luwu_submodule_path = luwu_lsp_dir:join("luwu")

--- Version of luwu user is actively developing in another directory
--- We compile against this one for rapid iteration between luwu Analysis improvements
--- and the LSP to ensure everything works and leads to better DX for language users
const luwu_test_path = env.vars.get("LUWU_TEST_PATH")

const is_release = parsed.flags["--release"] == true

const function ensure_dependencies()
    const has_cmake = #env.where("cmake") > 0
    const has_git = #env.where("git") > 0
    if has_cmake == false or has_git == false then
        local err_message = "Missing required dependencies in $PATH:"
        if not has_git then
            err_message ..= "\n  - Missing Git for version control"
        end
        if not has_cmake then
            err_message ..= "\n  - Missing CMake to actually compile luwu-lsp."
        end
        err_message ..= "\nPlease install these package(s) via your system package manager or the usual places."
        error(err_message)
    end
end

const function pick_luwu_path(): string
    local resp = parsed:get("--luwu") or (
        if is_release then
            -- seal ./rebuild.luau --release should compile from submodule dir with 0 hassle
            "sub"
        else
            "test"
    )
    const is_submoduley =
        resp == "sub" 
        or resp == "submodule"
        or resp == "./luau"
        or resp == "./luwu"
        or resp == "here"

    if resp == "test" or resp == "LUWU_TEST_PATH" then
        if luwu_test_path then
            const test_path_type = fs.is(luwu_test_path)
            if test_path_type == "Directory" then
                return luwu_test_path
            else
                error(`LUWU_TEST_PATH={luwu_test_path} is not a directory (got: {test_path_type})`)
            end
        end
        error(
            `Environment variable LUWU_TEST_PATH is unset.{eol2}\z
            - If you just want to compile luwu-lsp for general use outside testing mode, run seal ./rebuild.luau --release{eol2}\z
            - For rapid iteration during development, set LUWU_TEST_PATH to your local copy of the luwu repository.{eol2}\z
            - To compile luwu-lsp from the submodule path in this directory instead of LUWU_TEST_PATH, pass --luwu=here`
        )
    elseif is_submoduley then
        return luwu_submodule_path
    else
        const requested_dir = resp :: string
        const requested_path_type = fs.is(requested_dir)
        if requested_path_type == "Directory" then
            return requested_dir
        else
            error(`--luwu {luwu_test_path} is not a directory (got: {requested_path_type})`)
        end
    end
end

const function should_reconfigure(luwu_path: string)
    if fs.path.exists(build_dir_path) == false or parsed.flags["--clean"] == true then
        return true
    end
    -- check if CMakeCache.txt LSP_LUAU_PATH has drifted
    const cmakecache_path = fs.path.join(build_dir_path, "CMakeCache.txt")
    if not fs.path.exists(cmakecache_path) then
        return true
    end
    const content = fs.readfile(cmakecache_path)
    const splits = str.split(content, "LSP_LUAU_PATH:PATH=")
    const current_luwu_path = str.splitfront(splits[2], "\n", 1)[1]
    if current_luwu_path ~= luwu_path then
        return true
    end
    return false
end

const function clean_and_configure(luwu_path: string)
    -- replace build dir if build dir alr exists
    const _, res = fs.dir.try_remove(build_dir_path)
    const rebuilt =
        if res == "Ok" then
            true
        elseif res == "NotFound" then
            false
        else
            error(`Unexpected ./build directory removal status {res}`)

    fs.dir.ensure(build_dir_path)
    --[[
        We always want to configure cmake with DCMAKE_BUILD_TYPE=Release otherwise
        luwu itself can throw LUAU_ASSERT for things the luau team said are TODO
        and thereby crash the entire lsp instead of silently ignoring the TODO items
    ]]
    const configure_result = process.run {
        program = "cmake",
        args = {
            "..",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            `-DLSP_LUAU_PATH={luwu_path}`,
        },
        cwd = build_dir_path
    }
    if not configure_result.ok then
        error(`Could not configure the luwu-lsp cmake build dir due to error: {configure_result.err}`)
    end
    print(`{if rebuilt then "Cleaned" else "Made"} and {if rebuilt then "re" else ""}configured ./build directory with CMake.`)
end

const function update_submodules()
    const subdate_result = process.run {
        program = "git",
        args = {
            "submodule",
            "update",
            "--init",
            "--recursive"
        }
    }
    if not subdate_result.ok then
        error(`Unable to update submodules due to error {subdate_result.err}`)
    end
end

ensure_dependencies()

if parsed.command == "submodule-update" then
    update_submodules()
    process.exit(0)
end

assert(parsed.command == "default", `Unknown command {parsed.command}, see --help`)

const luwu_path = pick_luwu_path()
if should_reconfigure(luwu_path) then
    clean_and_configure(luwu_path)
end

-- update hover docs
generate_keywords()

const function get_targets(): { string }
    const passed_targets = parsed:expect<<string>>("--targets")
    const targets = 
        if string.find(passed_targets, ",") then
            str.split(passed_targets, ", ", ",")
        else
            { passed_targets }
    
    const normalized = {}
    for _, t in targets do
        const lowered = string.lower(t)
        if lowered == "cli" or lowered == "test" then
            table.insert(normalized, `Luwu.LanguageServer.{t}`)
        else
            error(`Unknown target {t}`)
        end
    end

    assert(#normalized > 0, "Expected 1 or more targets, got 0")
    
    return normalized
end

const targets = get_targets()

const children = setmetatable({}, {
    __index = {
        any_alive = function(self: { process.PipedChild }): boolean
            for _, child in self do
                if child:alive() then
                    return true
                end
            end
            return false
        end
    }
})

if luwu_path ~= luwu_submodule_path then
    print(`Using luwu path {colors.blue(luwu_path)}`)
else
    print("Using the luwu inside luwu-lsp")
end

const cpus = thread.parallelism()

--[[
    The linker writes the executable in place, so relinking a binary an editor is currently running
    replaces the pages under the running process and takes it down with SIGBUS -- messily, since
    the language server dying mid-request can take the editor with it.

    Unlinking first is what makes a rebuild safe: the running process keeps the old inode (and keeps
    working until it's restarted), while the linker writes a brand new file at the same path.
]]
const function unlink_running_binaries()
    const binaries = {
        ["Luwu.LanguageServer.CLI"] = fs.path.join(build_dir_path, "luwu-lsp"),
        ["Luwu.LanguageServer.Test"] = fs.path.join(build_dir_path, "Luwu.LanguageServer.Test"),
    }

    for _, target in targets do
        const binary_path = binaries[target]
        if binary_path and fs.path.exists(binary_path) then
            fs.file.try_remove(binary_path)
        end
    end
end

unlink_running_binaries()

for _, target in targets do
    const spawned = process.spawn {
        program = "cmake",
        args = {
            "--build", "build",
            "--target", target,
            "--config", if is_release then "Release" else "Debug",
            "-j", tostring(cpus // #targets),
        }
    } :: process.PipedChild
    table.insert(children, spawned)
end

for i = 1, #targets do
    print() -- make extra lines we can jump to
end

const is_tty = terminal.tty()
const some_errors = {}

const size = terminal.size()

while children:any_alive() and #some_errors == 0 do
    for index, child in children do
        local content = child.stdout:read_to("\n", false, time.milliseconds(1))
        if content then
            if is_tty then
                content = str.trimback(content) -- strip \n or \r\n
                -- strip CMakeFiles/Luwu.LanguageServer.dir
                content = str.replace(content, "CMakeFiles/Luwu.LanguageServer.dir", "")
                content = str.replace(content, "luwu/CMakeFiles", "")
                -- make sure fits in terminal
                if str.width(content) > size.x then
                    content = str.slice(content, 1, size.x - 4)
                end
                terminal.execute(
                    cursor.prevline(index),
                    terminal.clear("CurrentLine"),
                    terminal.write(content),
                    cursor.nextline(index)
                )
            else
                print(content)
            end
        end
        const errors = child.stderr:read(nil, 0)
        if errors then
            table.insert(some_errors, errors)
            const more_errors = child.stderr:read(nil, time.seconds(2))
            if more_errors then
                table.insert(some_errors, more_errors)
            end
        end
    end
end

if #some_errors > 0 then
    -- uh oh
    for _, child in children do
        if child:alive() then
            child:kill()
        end
    end
    const build_errors = table.concat(some_errors)
    fs.writefile("./build_errors.log", build_errors)
    output.ewriteln(build_errors)
    output.ewriteln(`If this failure looks like it's from stale artifacts, try clearing the build directory by running this script with --clean.`)
    error(`At least one target threw an error or warning (printed above and saved to file ./build_errors.log)`)
end

print("Rebuilt luwu lsp binary! Please reload your current editor")
