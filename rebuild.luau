--!strict
-- run with seal 0.8.1+
-- see https://github.com/seal-runtime/seal/releases

-- Usage: see seal ./rebuild.luau --help
const args = require("@std/args")
const env = require("@std/env")
const fs = require("@std/fs")
const colors = require("@std/io/colors")
const output = require("@std/io/output")
const prompt = require("@std/io/prompt")
const process = require("@std/process")
const semver = require("@std/semver")
const str = require("@std/str")
const terminal = require("@std/terminal")
const cursor = require("@std/terminal/cursor")
const thread = require("@std/thread")
const time = require("@std/time")

const function check_seal_semver(minimum: string): boolean
    if not _RUNTIME then -- 0.0.x seals dont have _RUNTIME
        return false
    end
    local min = semver.from(minimum)
    const success, seal_version = pcall(semver.from, _RUNTIME.version.display)
    if not success or typeof(seal_version) == "error" then
        error(`Can't parse your seal version (got {seal_version})`)
    end
    if seal_version >= min then
        return true
    end
    return false
end

if not check_seal_semver("0.8.1") then
    error(`Need seal version >= 0.8.1 so we can keep track of child process exit status, download from https://github.com/seal-runtime/seal/releases\n\z
    (got seal semver {if _RUNTIME then _RUNTIME.version.display else _VERSION})`)
end

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

const function can_crash_editor()
    -- TODO: need process.find in seal stdlib; this sucks and is something seal is made to fix for cross-platform scripting
    const function find_exe_path_for_pid(pid: string): string?
        if env.os == "Linux" then
            const _, path = pcall(fs.readlink, `/proc/{pid}/exe`)
            if typeof(path) ~= "string" then
                return nil
            end
            return str.trimback(path, " %(deleted%)")
        elseif env.os == "MacOS" then
            const res = process.shell(`ps -o comm= -p {pid}`)
            if res.ok then
                return str.trim(res.out)
            end
        elseif env.os == "Windows" then
            error("Windows not supported for PID lookup")
        end
        return nil
    end
    if env.os == "Linux" or env.os == "MacOS" or env.os == "Android" then
        if #env.where("pgrep") == 0 then
            return false -- idk what this system is doing very few systems are missing pgrep
        end
        const result = process.run {
            program = "pgrep",
            args = { "-x", "luwu-lsp" },
        }
        if result.ok then
            const pids = str.splitlines(result.out)
            for _, pid in pids do
                local exe_path = find_exe_path_for_pid(pid)
                if not exe_path then
                    continue
                end
                if str.trim(exe_path) == luwu_lsp_dir:join("build", "luwu-lsp") then
                    return true
                end
            end
        end
    elseif env.os == "Windows" then
        const res = process.run {
            program = "powershell",
            args = { "-NoProfile", "-Command", "(Get-Process -Name luwu-lsp -ErrorAction SilentlyContinue).Path" },
        }
        if res.ok then
            for _, p in str.splitlines(res.out) do
                if str.trim(p) == luwu_lsp_dir:join("build", "luwu-lsp.exe") then
                    return true
                end
            end
        end
    end
    return false
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
    print("Submodule updated or already up-to-date.")
end

const function pick_luwu_path(): string
    const resp = parsed:get("--luwu") or (
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
        local release_command = "seal ./rebuild.luau --release"
        if colors.enabled() then
            release_command = colors.codes.BOLD_WHITE .. release_command .. colors.codes.RED
        end
        error(
            `Script is in testing mode but environment variable LUWU_TEST_PATH is unset.{eol2}\z
            - If you just want to compile luwu-lsp for general use outside testing mode, run {release_command}{eol2}\z
            - For rapid iteration during development, set LUWU_TEST_PATH to your local copy of the luwu repository.{eol2}\z
            - To compile luwu-lsp from the submodule path in this directory instead of LUWU_TEST_PATH, pass --luwu=here`
        )
    elseif is_submoduley then
        -- make sure user didnt forget to clone submodules before erroring out somewhere else
        if not fs.path.exists(luwu_submodule_path) or #fs.listdir(luwu_submodule_path) < 4 then
            print("./luwu dir is missing or empty; you probably forgot to clone with submodules... updating submodules")
            update_submodules()
        end
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

const function create_child_pool()
    return setmetatable({} :: { process.PipedChild }, {
        __index = {
            any_alive = function(self: { process.PipedChild }): boolean
                for _, child in self do
                    if child:alive() then
                        return true
                    end
                end
                return false
            end,
            any_failed = function(self: { process.PipedChild }): boolean
                local failed = false
                for _, child in self do
                    if child:alive() then
                        continue
                    end
                    const status = child:status()
                    if status and status.ok then
                        continue
                    end
                    failed = true
                end
                return failed
            end,
            abort_all = function(self: { process.PipedChild })
                for _, child in self do
                    if child:alive() then
                        child:kill()
                    end
                end
            end
        }
    })
end

ensure_dependencies()

if parsed.command == "submodule-update" then
    update_submodules()
    process.exit(0)
end

assert(parsed.command == "default", `Unknown command {parsed.command}, see --help`)

if can_crash_editor() then
    const scare_message = `{colors.bold.yellow("[WARN]")} \z
    luwu-lsp in ./build dir is already running; rebuilding it now *may* crash your entire editor.\n\z
    Save everything you're working on before building!!\n\z
    Continue building?`
    if not terminal.tty() then
        const is_agentic_tool = (env.vars.get("AI_AGENT") or env.vars.get("CLAUDECODE") or env.vars.get("CURSOR_AGENT")) ~= nil
        error(`The luwu-lsp binary in ./build/luwu-lsp(.exe) is already running; rebuilding it now can crash any open editors. {
            if is_agentic_tool then
                "Detected you are an AI agent; please inform the user to save their work, close any open editors, \z
                or run seal rebuild.luau themselves to agree to the interactive prompt and override."
            else 
                ""
        }`)
    end
    if not prompt.confirm(scare_message) then
        output.ewriteln("Rebuild cancelled, exiting...")
        process.exit(1)
    end
end

const luwu_path = pick_luwu_path()
if should_reconfigure(luwu_path) then
    clean_and_configure(luwu_path)
end

-- update hover docs
generate_keywords()

const targets = get_targets()
--- Gives us a slightly better chance at not crashing a running editor
--- ideally removing the existing binaries should force an unlink and
--- get handled properly but it doesn't
const function remove_existing_binaries()
    const binaries: { [string]: string? } = {
        ["Luwu.LanguageServer.CLI"] = fs.path.join(build_dir_path, "luwu-lsp"),
        ["Luwu.LanguageServer.Test"] = fs.path.join(build_dir_path, "Luwu.LanguageServer.Test"),
    }
    for _, target in targets do
        const binary_path = binaries[target]
        if binary_path then
            fs.file.try_remove(binary_path)
        end
    end
end

if luwu_path ~= luwu_submodule_path then
    print(`Using luwu path {colors.blue(luwu_path)}`)
else
    print("Using the luwu inside luwu-lsp")
end

const cpus = math.min(thread.parallelism(), 16) -- prevent compiling w/ j24 which can freeze my desktop pc

remove_existing_binaries()

const children = create_child_pool()

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

--- keep track of stdout history because ninja writes errors to stdout instead of stderr
const history: { string } = {}
while children:any_alive() and #some_errors == 0 and not children:any_failed() do
    for index, child in children do
        local content = child.stdout:read_to("\n", false, time.milliseconds(1))
        if content then
            content = str.trimback(content) -- strip \n or \r\n
            -- strip redundant stuff CMakeFiles/Luwu.LanguageServer.dir
            content = str.replace(content, "CMakeFiles/Luwu.LanguageServer.dir", "")
            content = str.replace(content, "luwu/CMakeFiles", "")
            table.insert(history, content)
            if is_tty then
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
        -- note ninja (one of cmake's possible underlying build systems) in its infinite wisdom writes errors to stdout instead of stderr
        -- not caught by this but caught by history + any_failed (hopefully it gives failing status code)
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

if #some_errors > 0 or children:any_failed() then
    -- uh oh
    children:abort_all()
    if #some_errors > 0 then
        const build_errors = table.concat(some_errors, "\n")
        fs.writefile("./build_errors.log", build_errors)
        output.ewriteln(build_errors)
        output.ewriteln(`If this failure looks like it's from stale artifacts, try clearing the build directory by running this script with --clean.`)
        error(`At least one target threw an error or warning (printed above and saved to file ./build_errors.log)`)
    else
        output.ewriteln(`If this failure looks like it's from stale artifacts, try clearing the build directory by running this script with --clean.`)
        const build_history = table.concat(history, "\n")
        fs.writefile("./build_errors.log", build_history)
        error("At least one target exited with a failing status code (without writing to stderr)..., dumping stdout history to ./build_errors.log")
    end
end

print("Rebuilt luwu lsp binary! Please reload your current editor")
