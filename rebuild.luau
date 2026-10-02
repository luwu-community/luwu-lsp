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

const generate_hovers = require("./scripts/generate_hovers")

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
        args.flag("--static", "Linux only: fully statically link luwu-lsp into ./build-static, so it runs on distros with an older glibc than yours"),
        args.flag("--clean", "remove the build dir and reconfigure (do this luwu-lsp doesnt compile and you think partial build is stale)"),
        args.named("--targets", "Which of the CMake build targets should we build? Defaults to CLI (Luwu.LanguageServer.CLI). Specify multiple targets by separating them with commas.")
            :default("CLI")
    ),
    args.command("submodule-update", "update ./luwu to the latest commit on the branch .gitmodules tracks (cloning it if missing)"):args(
        args.named("--branch", "switch which luwu branch ./luwu tracks (written to .gitmodules) before updating, e.g. --branch=traits")
    ),
    args.command("vsix", "package editors/code into an installable .vsix (does not build the server binary)"):args(
        args.flag("--install", "install the freshly packaged .vsix into VSCode with `code --install-extension`")
    )
)

const luwu_lsp_dir =
    -- use instead of cwd in case ppl dont run this in correct dir
    fs.dir.from(script:parent())

const is_static = parsed.flags["--static"] == true

--- Static builds get their own dir: switching the linker flags back and forth in ./build would
--- relink everything each time, and ./build/luwu-lsp is usually the binary your editor is running.
const build_dir_name = if is_static then "build-static" else "build"
const build_dir_path = luwu_lsp_dir:join(build_dir_name)

--- Version of luwu vendored in as a submodule, should always be included
--- we build against this version by default when LUWU_TEST_PATH doesn't exist
const luwu_submodule_path = luwu_lsp_dir:join("luwu")

--- Version of luwu user is actively developing in another directory
--- We compile against this one for rapid iteration between luwu Analysis improvements
--- and the LSP to ensure everything works and leads to better DX for language users
const luwu_test_path = env.vars.get("LUWU_TEST_PATH")

const is_release = parsed.flags["--release"] == true

if is_static and env.os ~= "Linux" then
    error("--static is only supported on Linux; macOS doesn't allow fully static binaries and Windows already links the CRT statically in CI")
end

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
                if str.trim(exe_path) == fs.path.join(build_dir_path, "luwu-lsp") then
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
                if str.trim(p) == fs.path.join(build_dir_path, "luwu-lsp.exe") then
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
    -- switching between --release (warnings aren't errors) and a dev build (they are)
    const werror_splits = str.split(content, "LSP_WERROR:BOOL=")
    const current_werror = if werror_splits[2] then str.splitfront(werror_splits[2], "\n", 1)[1] else nil
    if current_werror ~= (if is_release then "OFF" else "ON") then
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
            error(`Unexpected ./{build_dir_name} directory removal status {res}`)

    fs.dir.ensure(build_dir_path)
    --[[
        We always want to configure cmake with DCMAKE_BUILD_TYPE=Release otherwise
        luwu itself can throw LUAU_ASSERT for things the luau team said are TODO
        and thereby crash the entire lsp instead of silently ignoring the TODO items
    ]]
    const configure_args = {
        "..",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        `-DLSP_LUAU_PATH={luwu_path}`,
        -- people running --release just want a working binary, so a warning their compiler raises
        -- and ours doesn't (e.g. Ubuntu gcc's default _FORTIFY_SOURCE) is logged, not fatal
        if is_release then "-DLSP_WERROR=OFF" else "-DLSP_WERROR=ON",
    }
    if is_static then
        -- glibc, libstdc++ and libgcc all baked in. Safe because nothing here uses NSS
        -- (getpwnam, getaddrinfo) or dlopen, which static glibc can't do
        table.insert(configure_args, "-DCMAKE_EXE_LINKER_FLAGS=-static")
    end
    const configure_result = process.run {
        program = "cmake",
        args = configure_args,
        cwd = build_dir_path
    }
    if not configure_result.ok then
        error(`Could not configure the luwu-lsp cmake build dir due to error: {configure_result.err}`)
    end
    print(`{if rebuilt then "Cleaned" else "Made"} and {if rebuilt then "re" else ""}configured ./{build_dir_name} directory with CMake.`)
end

--- Runs git from the luwu-lsp repo root (or `cwd`), not wherever the user happened to invoke this script
const function git(git_args: { string }, cwd: string?): process.RunResult
    return process.run {
        program = "git",
        args = git_args,
        cwd = cwd or luwu_lsp_dir.path,
    }
end

const function git_or_error(git_args: { string }, cwd: string?): string
    const result = git(git_args, cwd)
    if not result.ok then
        error(`git {table.concat(git_args, " ")} failed:\n{result.err}`)
    end
    return str.trim(result.out)
end

--- Makes sure ./luwu is an actual checkout, cloning it at the commit luwu-lsp pins if it's missing or empty.
--- Leaves an existing checkout alone: `git submodule update` would reset it to the pinned commit,
--- undoing a `submodule-update` that hasn't been committed yet.
const function ensure_submodule()
    const path_type = fs.is(luwu_submodule_path)
    if path_type ~= "NotFound" and path_type ~= "Directory" then
        error(`./luwu exists but isn't a directory (got: {path_type}); move it out of the way and rerun`)
    end
    -- a submodule checkout has a .git file (or dir, for ones cloned by old gits) at its root
    if path_type == "Directory" and fs.path.exists(fs.path.join(luwu_submodule_path, ".git")) then
        return
    end
    if path_type == "Directory" and #fs.listdir(luwu_submodule_path) > 0 then
        error("./luwu has files in it but isn't a git checkout, so git can't clone the submodule into it; \z
        move it out of the way and rerun")
    end

    print("./luwu is missing or empty (did you clone without --recurse-submodules?), checking it out...")
    -- picks up a changed url in .gitmodules; a no-op if the submodule was never initialized
    git_or_error({ "submodule", "sync", "--recursive", "--", "luwu" })
    git_or_error({ "submodule", "update", "--init", "--recursive", "--", "luwu" })
    print(`Checked out ./luwu at {git_or_error({ "rev-parse", "--short", "HEAD" }, luwu_submodule_path)}.`)
end

--- Moves ./luwu to the tip of the branch .gitmodules tracks, switching that branch to `new_branch` first if passed.
--- Leaves the new pointer (and .gitmodules) for the user to commit
const function update_submodule(new_branch: string?)
    ensure_submodule()
    if git_or_error({ "status", "--porcelain" }, luwu_submodule_path) ~= "" then
        error("./luwu has uncommitted changes; commit, stash or discard them before updating it")
    end

    if new_branch then
        -- check before touching .gitmodules so a typo doesn't leave it pointing at a branch that doesn't exist
        const exists = git({ "ls-remote", "--exit-code", "--heads", "origin", new_branch }, luwu_submodule_path)
        if not exists.ok then
            error(`luwu has no branch '{new_branch}' on its origin remote`)
        end
        git_or_error { "submodule", "set-branch", "--branch", new_branch, "--", "luwu" }
    end

    const branch_result = git { "config", "-f", ".gitmodules", "--get", "submodule.luwu.branch" }
    const branch = if branch_result.ok then str.trim(branch_result.out) else "the remote's default branch"

    const old_commit = git_or_error({ "rev-parse", "HEAD" }, luwu_submodule_path)
    git_or_error({ "submodule", "sync", "--recursive", "--", "luwu" })
    git_or_error({ "submodule", "update", "--init", "--remote", "--recursive", "--", "luwu" })
    const new_commit = git_or_error({ "rev-parse", "HEAD" }, luwu_submodule_path)

    if old_commit == new_commit then
        print(`./luwu is already at the latest {branch} ({string.sub(new_commit, 1, 8)}).`)
        return
    end
    print(`Updated ./luwu to the latest {branch} ({string.sub(new_commit, 1, 8)}):`)
    const added = git_or_error({ "log", "--oneline", `{old_commit}..{new_commit}` }, luwu_submodule_path)
    if added ~= "" then
        print(added)
    end
    -- switching to a branch that's behind (or diverged from) the old one drops commits
    const dropped = git_or_error({ "rev-list", "--count", `{new_commit}..{old_commit}` }, luwu_submodule_path)
    if dropped ~= "0" then
        print(colors.yellow(`{dropped} commit(s) from the old ./luwu ({string.sub(old_commit, 1, 8)}) aren't on {branch}`))
    end
    print(`Commit the new pointer with {colors.blue(
        if new_branch then "git add luwu .gitmodules && git commit" else "git add luwu && git commit"
    )} to keep it.`)
end

const extension_dir_path = luwu_lsp_dir:join("editors", "code")

--- Packages the VSCode extension. Deliberately does not touch the server binary: the .vsix
--- carries no bin/server, so an installed extension uses whatever luau-lsp.server.path points at.
const function package_vsix(): string
    if #env.where("npm") == 0 then
        error("Missing npm in $PATH; it's needed to typecheck, bundle and package the extension.")
    end

    if not fs.path.exists(fs.path.join(extension_dir_path, "node_modules")) then
        print("Installing extension dependencies...")
        const install_result = process.run {
            program = "npm",
            args = { "install" },
            cwd = extension_dir_path,
        }
        if not install_result.ok then
            error(`npm install failed:\n{install_result.err}`)
        end
        -- npm 12 blocks install scripts, and esbuild's postinstall is what fetches its binary
        process.run {
            program = "npm",
            args = { "install-scripts", "approve", "esbuild" },
            cwd = extension_dir_path,
        }
    end

    --[[
        vsce runs the vscode:prepublish script itself, so typechecking here is redundant --
        except that vsce swallows the compiler's output and reports only "npm failed with
        exit code 2", which tells you nothing about which file is broken.
    ]]
    print("Typechecking the extension...")
    const typecheck_result = process.run {
        program = "npm",
        args = { "run", "check-types" },
        cwd = extension_dir_path,
    }
    if not typecheck_result.ok then
        -- tsc writes its diagnostics to stdout, not stderr
        error(`The extension doesn't typecheck:\n{typecheck_result.stdout}{typecheck_result.stderr}`)
    end

    print("Packaging the VSCode extension...")
    const package_result = process.run {
        program = "npm",
        args = { "run", "vsix" },
        cwd = extension_dir_path,
    }
    if not package_result.ok then
        error(`Packaging the extension failed:\n{package_result.stdout}{package_result.stderr}`)
    end

    local packaged: string? = nil
    for _, entry in fs.listdir(extension_dir_path, false) do
        if str.endswith(entry, ".vsix") then
            packaged = entry
        end
    end

    if not packaged then
        error("vsce reported success but no .vsix landed in editors/code")
    end

    print(`Packaged {colors.green(packaged)}`)
    print("The .vsix contains no server binary; the extension uses your server.path setting.")
    return packaged
end

--- Installs a packaged .vsix into VSCode. `--force` so reinstalling the same version replaces it.
const function install_vsix(packaged: string)
    if #env.where("code") == 0 then
        error("Missing the VSCode `code` CLI in $PATH; install it or run `code --install-extension` yourself.")
    end

    print(`Installing {colors.green(packaged)}...`)
    const install_result = process.run {
        program = "code",
        args = { "--install-extension", packaged, "--force" },
        cwd = extension_dir_path,
    }
    if not install_result.ok then
        error(`Installing the extension failed:\n{install_result.stdout}{install_result.stderr}`)
    end

    print("Installed. Reload your VSCode windows to pick it up.")
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
        ensure_submodule()
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
    update_submodule(parsed:get<<string>>("--branch"))
    process.exit(0)
end

if parsed.command == "vsix" then
    const packaged = package_vsix()
    if parsed.flags["--install"] then
        install_vsix(packaged)
    else
        print(`Install it with {colors.blue(`code --install-extension {packaged} --force`)}, or pass --install next time`)
    end
    process.exit(0)
end

assert(parsed.command == "default", `Unknown command {parsed.command}, see --help`)

if can_crash_editor() then
    const scare_message = `{colors.bold.yellow("[WARN]")} \z
    luwu-lsp in ./{build_dir_name} dir is already running; rebuilding it now *may* crash your entire editor.\n\z
    Save everything you're working on before building!!\n\z
    Continue building?`
    if not terminal.tty() then
        const is_agentic_tool = (env.vars.get("AI_AGENT") or env.vars.get("CLAUDECODE") or env.vars.get("CURSOR_AGENT")) ~= nil
        error(`The luwu-lsp binary in ./{build_dir_name}/luwu-lsp(.exe) is already running; rebuilding it now can crash any open editors. {
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
generate_hovers()

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

--- GB of memory available right now, from /proc/meminfo; nil when we can't tell (not Linux/Android)
const function available_memory_gb(): number?
    if env.os ~= "Linux" and env.os ~= "Android" then
        return nil
    end
    const meminfo = fs.file.try_read("/proc/meminfo")
    if not meminfo then
        return nil
    end
    -- MemAvailable:   12345678 kB
    const kb = tonumber(string.match(meminfo, "MemAvailable:%s*(%d+)"))
    if not kb then
        return nil
    end
    return kb / (1024 * 1024)
end

--- leave a core free so the machine stays usable; each clang on Luwu's Analysis sources can take
--- up to ~2 GB, so running more jobs than that fits gets the build (or the whole terminal) OOM-killed;
--- never more than 16, since -j24 has frozen the desktop pc
const function job_count(): number
    local jobs = math.min(thread.parallelism() - 1, 16)
    const mem_gb = available_memory_gb()
    if mem_gb then
        jobs = math.min(jobs, mem_gb // 2)
    end
    return math.max(jobs, 1)
end

const cpus = job_count()
print(`Building with -j{math.max(cpus // #targets, 1)} per target`)

remove_existing_binaries()

const children = create_child_pool()

for _, target in targets do
    const spawned = process.spawn {
        program = "cmake",
        args = {
            "--build", build_dir_name,
            "--target", target,
            "--config", if is_release then "Release" else "Debug",
            "-j", tostring(math.max(cpus // #targets, 1)),
        }
    } :: process.PipedChild
    table.insert(children, spawned)
end

for i = 1, #targets do
    print() -- make extra lines we can jump to
end

const is_tty = terminal.tty()
--- everything the compilers wrote to stderr, plus diagnostic lines from stdout (ninja writes them there)
const diagnostics: { string } = {}
--- set on the first problem that fails the build, so we can kill every target right away
--- instead of waiting for the jobs already in flight
local stop_now = false

const function is_error(text: string): boolean
    return string.find(text, ": error", 1, true) ~= nil or string.find(text, ": fatal error", 1, true) ~= nil
end

const function is_diagnostic_line(line: string): boolean
    return string.find(line, ": warning", 1, true) ~= nil or is_error(line)
end

const function record_diagnostics(text: string)
    table.insert(diagnostics, text)
    -- dev builds fail on anything the compiler says (warnings are errors there);
    -- --release only on errors
    if not is_release or is_error(text) then
        stop_now = true
    end
end

const size = terminal.size()

--- keep track of stdout history because ninja writes errors to stdout instead of stderr
const history: { string } = {}
while children:any_alive() and not children:any_failed() and not stop_now do
    for index, child in children do
        local content = child.stdout:read_to("\n", false, time.milliseconds(1))
        if content then
            content = str.trimback(content) -- strip \n or \r\n
            -- strip redundant stuff CMakeFiles/Luwu.LanguageServer.dir
            content = str.replace(content, "CMakeFiles/Luwu.LanguageServer.dir", "")
            content = str.replace(content, "luwu/CMakeFiles", "")
            table.insert(history, content)
            if is_diagnostic_line(content) then
                record_diagnostics(content)
            end
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
        -- those are picked out of stdout above
        const errors = child.stderr:read(nil, 0)
        if errors then
            record_diagnostics(errors)
            if stop_now then
                -- give the compiler a moment to finish printing the whole diagnostic
                const more_errors = child.stderr:read(nil, time.seconds(2))
                if more_errors then
                    record_diagnostics(more_errors)
                end
                break
            end
        end
    end
end

-- whatever the targets wrote after the loop's last read
for _, child in children do
    const rest_out = child.stdout:read(nil, 0)
    if rest_out then
        for _, line in str.split(rest_out, "\n") do
            table.insert(history, line)
            if is_diagnostic_line(line) then
                record_diagnostics(line)
            end
        end
    end
    const rest_err = child.stderr:read(nil, 0)
    if rest_err then
        record_diagnostics(rest_err)
    end
end

if stop_now or children:any_failed() then
    -- uh oh
    children:abort_all()
    if #diagnostics > 0 then
        const build_errors = table.concat(diagnostics, "\n")
        fs.writefile("./build_errors.log", build_errors)
        output.ewriteln(build_errors)
        output.ewriteln(`If this failure looks like it's from stale artifacts, try clearing the build directory by running this script with --clean.`)
        if is_release then
            error(`At least one target threw an error (printed above and saved to file ./build_errors.log)`)
        else
            error(
                `At least one target threw an error or warning (printed above and saved to file ./build_errors.log).\n\z
                If you just want a working binary, run with --release, which logs warnings without failing on them.`
            )
        end
    else
        output.ewriteln(`If this failure looks like it's from stale artifacts, try clearing the build directory by running this script with --clean.`)
        const build_history = table.concat(history, "\n")
        fs.writefile("./build_errors.log", build_history)
        error("At least one target exited with a failing status code (without writing to stderr)..., dumping stdout history to ./build_errors.log")
    end
end

if #diagnostics > 0 then
    -- only --release gets here with diagnostics: warnings that didn't stop the build but are worth seeing
    const build_warnings = table.concat(diagnostics, "\n")
    fs.writefile("./build_errors.log", build_warnings)
    output.ewriteln(build_warnings)
    output.ewriteln("The build succeeded with the warnings above (saved to ./build_errors.log); --release doesn't fail on warnings.")
end

print("Rebuilt luwu lsp binary! Please reload your current editor")
