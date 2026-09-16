const fs = require("@std/fs")
const format = require("@std/io/format")
const json = require("@std/serde/json")
const str = require("@std/str")
format.defaults {
    max_elements_in_array = 200
}

const function regen()
    const markdown_path = fs.path.join(script:parent(), "keywords.md")
    const commstart = "<!-- "
    const commend = " -->\n"
    const contents = fs.readfile(markdown_path)
    const splits = str.splitaround(contents, commstart, commend)
    
    const keywords = json.readfile("./keyword_hovers.json") :: { [string]: string }
    
    local last_was_commstart = false
    local last_was_commend = false
    local current_keyword: string?
    local current_docs: string?
    for _, sp in splits do
        if sp == commstart then
            last_was_commstart = true
            continue
        elseif sp == commend then
            last_was_commend = true
            continue
        end
        if last_was_commstart then
            if sp == "!stop parsing" then
                break
            end
            const keyw = string.match(sp, "keyword: ([%w_]+)")
            if keyw then
                current_keyword = keyw
            elseif sp == "/keyword" then
                if current_keyword and current_docs and #str.trim(current_docs) > 0 then
                    keywords[current_keyword] = current_docs
                    current_keyword = nil
                    current_docs = nil
                    continue
                end
            end
            last_was_commstart = false
        end
        if last_was_commend and not str.startswith(sp, "keyword:") then
            current_docs = sp
        end
    end
    
    -- print(keywords)
    -- sorted: Luau table iteration order isn't stable between runs, so without this every
    -- regeneration reshuffles the keys and shows up as a diff of the whole file
    json.writefile("./keyword_hovers.json", keywords, { sorted = true })
end

if script:path() == script.entry_path then
    regen()
end

return regen