--!strict
-- run with seal 0.8.x https://github.com/seal-runtime/seal
const env = require("@std/env")
const fs = require("@std/fs")
const str = require("@std/str")

const home = fs.dir.home()
const here_path = fs.path.join(script:parent(), "Luwu.tmLanguage.json")

-- our grammar is scoped `source.luwu`; upstream's is `source.luau`, and the installed
-- upstream extension declares that scope name, so it has to be swapped in both directions
const function to_upstream(contents: string): string
    return str.replace(contents, "source.luwu", "source.luau")
end

const function from_upstream(contents: string): string
    return str.replace(contents, "source.luau", "source.luwu")
end

const function find_luau_lsp_extension_dir(path: string): string?
    const extensions = fs.listdir(path, false)
    for _, ext in extensions do
        const child = assert(fs.path.child(ext))
        if str.startswith(child, "johnnymorganz.luau-lsp") then
            return ext
        end
    end
    return nil
end

const function get_tmlanguage_path()
    const extensions_dir = home:join(".vscode-oss", "extensions")
    assert(fs.path.exists(extensions_dir), "extensions dir doesn't exist")

    const luau_lsp_dir = find_luau_lsp_extension_dir(extensions_dir)
    assert(luau_lsp_dir, "can't find luau lsp dir")

    const syntaxes_dir = fs.path.join(luau_lsp_dir, "syntaxes")
    assert(fs.path.exists(syntaxes_dir), "missing syntaxes dir")

    return fs.path.join(syntaxes_dir, "Luau.tmLanguage.json")
end

const function pull_from_current_vscode()
    if env.os == "Linux" then
        const luau_tmlanguage_json_path = get_tmlanguage_path()
        if fs.path.exists(luau_tmlanguage_json_path) then
            fs.writefile(here_path, from_upstream(fs.readfile(luau_tmlanguage_json_path)))
            print("succesfully overwrote local with vscode")
        end
    else
        error("uh os not supported yet")
    end
end

const function push_to_vscode()
    if env.os == "Linux" then
        const luau_tmlanguage_json_path = get_tmlanguage_path()
        fs.writefile(luau_tmlanguage_json_path, to_upstream(fs.readfile(here_path)))
        print("successfully overwrite vscode with local")
    else
        error("uh os not supported yet")
    end
end

push_to_vscode()
