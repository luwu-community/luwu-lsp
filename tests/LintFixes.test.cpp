#include "doctest.h"
#include "Fixture.h"
#include "Flags.hpp"

#include <algorithm>

LUAU_FASTFLAG(DebugLuwuPoperators)

namespace
{

// Every Luwu flag on for the duration of a test, as the language server runs. Restores every flag it touched.
struct ScopedLuwuFlags
{
    std::vector<std::pair<Luau::FValue<bool>*, bool>> saved;

    ScopedLuwuFlags()
    {
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            saved.emplace_back(flag, flag->value);
        applyLuwuFlags(/* luauCompatibilityMode: */ false);
    }

    ~ScopedLuwuFlags()
    {
        for (auto [flag, value] : saved)
            flag->value = value;
    }
};

size_t offsetOf(const std::string& source, const lsp::Position& position)
{
    size_t offset = 0;
    for (size_t line = 0; line < position.line; ++line)
        offset = source.find('\n', offset) + 1;
    return offset + position.character;
}

std::string applyEdits(const std::string& source, std::vector<lsp::TextEdit> edits)
{
    std::sort(edits.begin(), edits.end(),
        [&](const lsp::TextEdit& a, const lsp::TextEdit& b)
        {
            return offsetOf(source, a.range.start) > offsetOf(source, b.range.start);
        });

    std::string result = source;
    for (const auto& edit : edits)
    {
        size_t begin = offsetOf(source, edit.range.start);
        result.replace(begin, offsetOf(source, edit.range.end) - begin, edit.newText);
    }
    return result;
}

struct AppliedFix
{
    std::string text;
    bool preferred;
};

// `source` in a `.luwu` file with the code action titled `title` applied, if the document has one
std::optional<AppliedFix> applyAction(Fixture& fixture, const std::string& source, const std::string& title, lsp::CodeActionKind kind)
{
    auto uri = fixture.newDocument("test.luwu", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {static_cast<size_t>(std::count(source.begin(), source.end(), '\n')) + 1, 0}};
    params.context.only = {kind};

    auto result = fixture.workspace.codeAction(params, nullptr);
    if (!result)
        return std::nullopt;

    for (const auto& action : *result)
    {
        if (action.title == title && action.edit)
            return AppliedFix{applyEdits(source, action.edit->changes.at(uri)), action.isPreferred};
    }
    return std::nullopt;
}

std::optional<AppliedFix> applyQuickFix(Fixture& fixture, const std::string& source, const std::string& title)
{
    return applyAction(fixture, source, title, lsp::CodeActionKind::QuickFix);
}

std::optional<AppliedFix> applyFixAll(Fixture& fixture, const std::string& source, const std::string& lint)
{
    return applyAction(fixture, source, "Fix all '" + lint + "' warnings in this module", lsp::CodeActionKind::Source);
}

} // namespace

TEST_SUITE_BEGIN("LintFixes");

TEST_CASE_FIXTURE(Fixture, "nil_none_comparison")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function get(): number | none
    return none
end
local x = get()
if x ~= nil then
    print(x)
end
)",
        "Compare with 'none'");
    REQUIRE(fix);
    CHECK(fix->text.find("if x ~= none then") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "vararg_cast")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(...: number)
    return ... :: number
end
return f
)",
        "Wrap in parentheses to keep only the first value");
    REQUIRE(fix);
    CHECK(fix->text.find("return (... :: number)") != std::string::npos);
    CHECK_FALSE(fix->preferred);
}

TEST_CASE_FIXTURE(Fixture, "string_index_zero_fixes_byte_only")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(s: string)
    return string.byte(s, 0), string.sub(s, 0, 2)
end
return f
)";
    auto fix = applyFixAll(*this, source, "StringIndexZero");
    REQUIRE(fix);
    CHECK(fix->text.find("string.byte(s, 1), string.sub(s, 0, 2)") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "table_truthiness")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(xs: { number }, m: { [string]: number })
    if xs then
        print("a")
    end
    if not m then
        print("b")
    end
end
return f
)";
    auto fix = applyFixAll(*this, source, "TableTruthiness");
    REQUIRE(fix);
    CHECK(fix->text.find("if next(xs) ~= nil then") != std::string::npos);
    CHECK(fix->text.find("if next(m) == nil then") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "discarded_result")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(s: string)
    s:upper()
    return s
end
return f
)";
    auto discard = applyQuickFix(*this, source, "Discard the result with 'const _ ='");
    REQUIRE(discard);
    CHECK(discard->text.find("    const _ = s:upper()\n") != std::string::npos);

    auto keep = applyQuickFix(*this, source, "Assign the result back to 's'");
    REQUIRE(keep);
    CHECK(keep->text.find("    s = s:upper()\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "float_index")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local xs = { 1, 2, 3 }
print(xs[#xs / 2])
)",
        "Divide to an integer with '//'");
    REQUIRE(fix);
    CHECK(fix->text.find("print(xs[#xs // 2])") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "pairs_and_ipairs")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local t = { 1, 2 }
for k, v in pairs(t) do
    print(k, v)
end
for i, v in ipairs(t) do
    print(i, v)
end
local p = pairs
)";
    auto pairsFix = applyFixAll(*this, source, "Pairs");
    REQUIRE(pairsFix);
    CHECK(pairsFix->text.find("for k, v in t do") != std::string::npos);
    // Passing `pairs` around isn't something iteration replaces
    CHECK(pairsFix->text.find("local p = pairs") != std::string::npos);

    auto ipairsFix = applyQuickFix(*this, source, "Iterate directly instead of with 'ipairs'");
    REQUIRE(ipairsFix);
    CHECK(ipairsFix->text.find("for i, v in t do") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "self_assignment")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number?, s: string?)
    x = x
    x = x or return nil
    if const s = s then
        print(s)
    end
    return x
end
return f
)";
    auto fix = applyFixAll(*this, source, "SelfAssignment");
    REQUIRE(fix);
    CHECK_EQ(fix->text, R"(
local function f(x: number?, s: string?)
    x or return nil
    if s then
        print(s)
    end
    return x
end
return f
)");
}

TEST_CASE_FIXTURE(Fixture, "floor_division")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(a: number, b: number)
    print(math.floor(a / b))
    print(-math.floor(a / b))
    print(2 / math.floor(a / b))
end
return f
)";
    auto fix = applyFixAll(*this, source, "FloorDivision");
    REQUIRE(fix);
    CHECK(fix->text.find("print(a // b)") != std::string::npos);
    CHECK(fix->text.find("print(-(a // b))") != std::string::npos);
    CHECK(fix->text.find("print(2 / (a // b))") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "discarded_values")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function two(): (number, number)
    return 1, 2
end
local function f(c: boolean)
    print(if c then two() else 0)
end
return f
)",
        "Wrap in parentheses to keep only the first value");
    REQUIRE(fix);
    CHECK(fix->text.find("print(if c then (two()) else 0)") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "any_annotation_and_cast")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: any, y: number)
    print(y :: any)
    return x
end
return f
)";
    auto annotation = applyFixAll(*this, source, "AnyAnnotation");
    REQUIRE(annotation);
    CHECK(annotation->text.find("local function f(x: unknown, y: number)") != std::string::npos);

    auto cast = applyQuickFix(*this, source, "Use 'unknown' instead of 'any'");
    REQUIRE(cast);
    CHECK_FALSE(cast->preferred);
}

TEST_CASE_FIXTURE(Fixture, "return_self")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
trait Aliased
    public function annotated(self): Aliased
        return self
    end
end
)",
        "Return 'Self'");
    REQUIRE(fix);
    CHECK(fix->text.find("public function annotated(self): Self") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "return_on_next_line")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function maybe(): number?
    return nil
end
local function f()
    local a = maybe() or do return
        maybe()
    local b = maybe() or return
        maybe()
    return a, b
end
return f
)";
    auto fix = applyFixAll(*this, source, "ReturnOnNextLine");
    REQUIRE(fix);
    CHECK(fix->text.find("local a = maybe() or (do return)\n") != std::string::npos);
    CHECK(fix->text.find("local b = maybe() or (return)\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "or_continue")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function maybe(): number?
    return nil
end
print(continue)
for i = 1, 3 do
    local x = maybe() or continue
    print(x)
end
)",
        "Continue the loop with 'or do continue'");
    REQUIRE(fix);
    CHECK(fix->text.find("local x = maybe() or do continue\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "binding_swallows_condition")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function find(id: number): number?
    return id
end
if const n = find(1) and n > 1 then
    print(n)
end
)",
        "Test the binding with 'when'");
    REQUIRE(fix);
    CHECK(fix->text.find("if const n = find(1) when n > 1 then") != std::string::npos);
    CHECK(fix->preferred);
}

TEST_CASE_FIXTURE(Fixture, "one_line_statement")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number)
    if x > 1 then print(x) elseif x < 0 then print(-x) else print(0) end
    for i = 1, x do print(i); print(i * 2) end
end
return f
)";
    auto fix = applyFixAll(*this, source, "OneLineStatement");
    REQUIRE(fix);
    CHECK_EQ(fix->text, R"(
local function f(x: number)
    if x > 1 then
        print(x)
    elseif x < 0 then
        print(-x)
    else
        print(0)
    end
    for i = 1, x do
        print(i);
        print(i * 2)
    end
end
return f
)");
}

TEST_CASE_FIXTURE(Fixture, "lua_guard")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number?, y: number, v: { ok: boolean })
    if not x then return end
    if x == y then return 1 end
    if x > 1 then return end
    if not v.ok then return x, y end
    return x
end
return f
)";
    auto fix = applyFixAll(*this, source, "LuaGuard");
    REQUIRE(fix);
    CHECK(fix->text.find("    x or return;\n") != std::string::npos);
    // a condition without `not` is kept as written, with `and`
    CHECK(fix->text.find("    x == y and return 1\n") != std::string::npos);
    CHECK(fix->text.find("    x > 1 and return;\n") != std::string::npos);
    CHECK(fix->text.find("    v.ok or (return x, y)\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "lua_guard_splits_several_conditions_into_one_guard_each")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(r: { ok: boolean, out: string }, t: { { skip: boolean, ready: boolean } })
    if not r.ok or r.out == "" then return nil end
    for _, v in t do
        if v.skip or not v.ready then continue end
        print(v)
    end
    return r.out
end
return f
)";
    auto fix = applyFixAll(*this, source, "LuaGuard");
    REQUIRE(fix);
    // each condition is kept as written: `not x` becomes `x or`, anything else `x and`
    CHECK(fix->text.find("    r.ok or return nil\n    r.out == \"\" and return nil\n") != std::string::npos);
    CHECK(fix->text.find("        v.skip and continue\n        v.ready or continue\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "lua_guard_leaves_conditions_that_would_continue_the_line_above")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number, s: string)
    if -x > 1 then return end
    if "a" == s then return end
    if x + 1 then return end
    return x
end
return f
)";
    auto fix = applyFixAll(*this, source, "LuaGuard");
    REQUIRE(fix);
    // `-x > 1 and return` and `"a" == s and return` would join the statement above them
    CHECK(fix->text.find("    if -x > 1 then return end\n") != std::string::npos);
    CHECK(fix->text.find("    if \"a\" == s then return end\n") != std::string::npos);
    CHECK(fix->text.find("    x + 1 and return;\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "ambiguous_continuation")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(xs: { number }, y: number)
    for _, x in xs do
        local a = y
        -x < 0 or break
        print(a)
    end
end
return f
)",
        "End the line above with ';'");
    REQUIRE(fix);
    CHECK(fix->text.find("        local a = y;\n        -x < 0 or break\n") != std::string::npos);
    CHECK_FALSE(fix->preferred);
}

TEST_CASE_FIXTURE(Fixture, "unflipped_guard")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(xs: { string }, n: number?, m: number?, k: number?)
    for _, name in xs do
        name == "" or continue
        print(name)
    end
    not n or return 1
    m ~= nil and return 2
    if k then
        return 3
    end
    print(n, m, k)
    return 0
end
return f
)";
    auto fix = applyFixAll(*this, source, "UnflippedGuard");
    REQUIRE(fix);
    CHECK(fix->text.find("        name ~= \"\" or continue\n") != std::string::npos);
    CHECK(fix->text.find("    n or return 1\n") != std::string::npos);
    CHECK(fix->text.find("    m ~= nil or return 2\n") != std::string::npos);
    CHECK(fix->text.find("    if not k then\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "loop_concat")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(words: { string }): string
    local sentence: string = "> "
    local count = 0
    for i, word in words do
        if i > 1 then
            sentence ..= " "
        end
        sentence = sentence .. word:upper()
        count += 1
    end
    return sentence .. tostring(count)
end
return f
)",
        "Build a table of parts and join it with 'table.concat' after the loop");
    REQUIRE(fix);
    CHECK_EQ(fix->text, R"(
local function f(words: { string }): string
    local sentence_parts = { "> " }
    local count = 0
    for i, word in words do
        if i > 1 then
            table.insert(sentence_parts, " ")
        end
        table.insert(sentence_parts, (word:upper()))
        count += 1
    end
    local sentence: string = table.concat(sentence_parts)
    return sentence .. tostring(count)
end
return f
)");
    CHECK_FALSE(fix->preferred);
}

TEST_CASE_FIXTURE(Fixture, "loop_concat_pushes_with_the_push_operator")
{
    ScopedLuwuFlags luwu;
    ScopedFastFlag poperators{FFlag::DebugLuwuPoperators, true};
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(words: { string }): string
    local text = ""
    for _, word in words do
        text ..= word
        text = text .. word:gsub("a", "b")
    end
    return text
end
return f
)",
        "Build a table of parts and join it with 'table.concat' after the loop");
    REQUIRE(fix);
    // the parentheses keep gsub's count out of the push
    CHECK(fix->text.find("        text_parts <+ word\n        text_parts <+ (word:gsub(\"a\", \"b\"))\n") != std::string::npos);
    CHECK(fix->text.find("    local text = table.concat(text_parts)\n    return text\n") != std::string::npos);
}

// The shapes seal writes into every project's .seal directory: tt.maptostring, csv's parse_line and the guided tour
TEST_CASE_FIXTURE(Fixture, "loop_concat_fixes_the_shapes_seal_generates")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();
    const char* title = "Build a table of parts and join it with 'table.concat' after the loop";

    // loops in both branches of an `if`
    auto branches = applyQuickFix(*this, R"(
local function maptostring(t: { [string]: string }, normal: boolean, sep: string): string
    local result = ""
    if normal then
        for k, v in t do
            result ..= v .. sep
        end
    else
        for k, v in pairs(t) do
            result ..= v .. sep
        end
    end
    return result
end
return maptostring
)",
        title);
    REQUIRE(branches);
    CHECK(branches->text.find("            table.insert(result_parts, v .. sep)\n        end\n    else\n") != std::string::npos);
    CHECK(branches->text.find("    end\n    local result = table.concat(result_parts)\n    return result\n") != std::string::npos);

    // reset and read while it's built
    auto resets = applyQuickFix(*this, R"(
local function parse_line(graphemes: { string }): { string }
    local result: { string } = {}
    local current_word = ""
    local quotes = 0
    for _, grapheme in graphemes do
        if grapheme == "," then
            table.insert(result, current_word)
            current_word = ""
            continue
        end
        if quotes == 2 and current_word == "" then
            continue
        end
        current_word ..= grapheme
    end
    table.insert(result, current_word)
    return result
end
return parse_line
)",
        title);
    REQUIRE(resets);
    CHECK(resets->text.find("    local current_word_parts = {}\n") != std::string::npos);
    CHECK(resets->text.find("            table.insert(result, table.concat(current_word_parts))\n"
                            "            current_word_parts = {}\n") != std::string::npos);
    CHECK(resets->text.find("        if quotes == 2 and table.concat(current_word_parts) == \"\" then\n") != std::string::npos);
    CHECK(resets->text.find("        table.insert(current_word_parts, grapheme)\n") != std::string::npos);
    CHECK(resets->text.find("    end\n"
                            "    local current_word = table.concat(current_word_parts)\n"
                            "    table.insert(result, current_word)\n") != std::string::npos);

    // a `while` loop, with the string used after it
    auto loop = applyQuickFix(*this, R"(
local function read(next_text: () -> string?)
    local text = ""
    while true do
        local new_text = next_text()
        if not new_text then
            break
        end
        text ..= new_text
    end
    print(text)
end
return read
)",
        title);
    REQUIRE(loop);
    CHECK(loop->text.find("        table.insert(text_parts, new_text)\n") != std::string::npos);
    CHECK(loop->text.find("    end\n    local text = table.concat(text_parts)\n    print(text)\n") != std::string::npos);

    // the guided tour never reads its string afterwards, so no joined local is added for LocalUnused to report
    auto unread = applyQuickFix(*this, R"(
local function drain(next_text: () -> string?)
    local text = ""
    while true do
        local new_text = next_text()
        if not new_text then
            break
        end
        text ..= new_text
    end
end
return drain
)",
        title);
    REQUIRE(unread);
    CHECK(unread->text.find("table.concat") == std::string::npos);
    CHECK(unread->text.find("    local text_parts = {}\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "loop_concat_only_fixes_the_common_shape")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    // Each reads the string inside an appended value, uses it before the loop, doesn't start from a string literal, or
    // appends in a closure
    const char* sources[] = {
        "local function f(xs: { string })\n    local s = \"\"\n    for _, x in xs do\n        s ..= x .. s\n    end\n    return s\nend\nreturn f\n",
        "local function f(xs: { string })\n"
            "    local s = \"\"\n"
            "    print(s)\n"
            "    for _, x in xs do\n"
            "        s ..= x\n"
            "    end\n"
            "    return s\n"
            "end\n"
            "return f\n",
        "local function f(xs: { string }, t: string)\n"
            "    local s = t\n"
            "    for _, x in xs do\n"
            "        s ..= x\n"
            "    end\n"
            "    return s\n"
            "end\n"
            "return f\n",
        "local function f(xs: { string })\n"
            "    local s = \"\"\n"
            "    for _, x in xs do\n"
            "        local g = function() s ..= x end\n"
            "        g()\n"
            "    end\n"
            "    return s\n"
            "end\n"
            "return f\n",
    };

    for (const char* source : sources)
    {
        INFO(source);
        CHECK_FALSE(applyQuickFix(*this, source, "Build a table of parts and join it with 'table.concat' after the loop"));
    }
}

TEST_CASE_FIXTURE(Fixture, "inline_or_leave")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(x, path)
    local a = 1
    local b = 2
    local found = locate_the_thing_we_are_looking_for(path, x, "with a long argument") or return nil
    local c = 3
    local d = 4
    return a, b, c, d, found
end
return f
)",
        "Move to its own line");
    REQUIRE(fix);
    CHECK(fix->text.find("    local found = locate_the_thing_we_are_looking_for(path, x, \"with a long argument\")\n        or return nil\n") !=
          std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "or_return_comma")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x, a, b)
    local q, r = x or return a, b
    return q, r
end
return f
)";
    auto both = applyQuickFix(*this, source, "Return both values");
    REQUIRE(both);
    CHECK(both->text.find("local q, r = x or (return a, b)") != std::string::npos);

    auto keep = applyQuickFix(*this, source, "Keep the 'return' to one value");
    REQUIRE(keep);
    CHECK(keep->text.find("local q, r = (x or return a), b") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "lua_string_helper_fix_all")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(s: string, p: string)
    if s:sub(1, #p) == p then
        return s:gsub("%.luau", ".luwu")
    end
    if string.find(s, "x", 1, true) == nil then
        return s
    end
    return s
end
return f
)";
    auto fix = applyFixAll(*this, source, "LuaStringHelper");
    REQUIRE(fix);
    CHECK(fix->text.find("if s:startswith(p) then") != std::string::npos);
    CHECK(fix->text.find("if not s:contains(\"x\") then") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "fix_all_is_a_quick_fix_when_the_module_has_more_than_one_warning")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string two = R"(
local t = { 1, 2 }
for k, v in pairs(t) do
    print(k, v)
end
for k, v in pairs(t) do
    print(k, v)
end
)";
    auto fix = applyQuickFix(*this, two, "Fix all 'Pairs' warnings in this module");
    REQUIRE(fix);
    CHECK(fix->text.find("pairs") == std::string::npos);

}

TEST_CASE_FIXTURE(Fixture, "fix_all_is_not_a_quick_fix_for_a_single_warning")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string one = R"(
local t = { 1, 2 }
for k, v in pairs(t) do
    print(k, v)
end
)";
    CHECK_FALSE(applyQuickFix(*this, one, "Fix all 'Pairs' warnings in this module"));
}

TEST_CASE_FIXTURE(Fixture, "lua_and_or_fix_all_is_a_quick_fix_too")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string andOr = R"(
local c = math.random() > 0.5
local a = c and 1 or 2
local b = c and 3 or 4
print(a, b)
)";
    auto fix = applyQuickFix(*this, andOr, "Rewrite all 'a and b or c' as 'if a then b else c'");
    REQUIRE(fix);
    CHECK(fix->text.find("local b = if c then 3 else 4") != std::string::npos);
}

TEST_SUITE_END();
