#include "doctest.h"
#include "Fixture.h"

LUAU_FASTFLAG(LuwuClasses)
LUAU_FASTFLAG(LuwuTraits)

TEST_SUITE_BEGIN("Rename");

TEST_CASE_FIXTURE(Fixture, "fail_if_new_name_is_empty")
{
    auto uri = newDocument("foo.luau", "");

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 0};
    params.newName = "";

    REQUIRE_THROWS_WITH_AS(workspace.rename(params, nullptr), "The new name must be a valid identifier", JsonRpcException);
}

TEST_CASE_FIXTURE(Fixture, "fail_if_new_name_does_not_start_as_valid_identifier")
{
    auto uri = newDocument("foo.luau", "");

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 0};
    params.newName = "1234";

    REQUIRE_THROWS_WITH_AS(
        workspace.rename(params, nullptr), "The new name must be a valid identifier starting with a character or underscore", JsonRpcException);
}

TEST_CASE_FIXTURE(Fixture, "fail_if_new_name_is_not_a_valid_identifier")
{
    auto uri = newDocument("foo.luau", "");

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 0};
    params.newName = "testing123!";

    REQUIRE_THROWS_WITH_AS(workspace.rename(params, nullptr),
        "The new name must be a valid identifier composed of characters, digits, and underscores only", JsonRpcException);
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_parameter")
{
    // https://github.com/JohnnyMorganz/luau-lsp/issues/488
    // Renaming: "S" inside of <S>

    auto source = R"(
        type LayerBuilder<S> = S & { ... }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 26};
    params.newName = "State";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK(applyEdit(source, documentEdits) == R"(
        type LayerBuilder<State> = State & { ... }
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_parameter_used_inside_of_type_alias")
{
    // https://github.com/JohnnyMorganz/luau-lsp/issues/488
    // Renaming: "S" inside of assignment `S & { ... }`

    auto source = R"(
        type LayerBuilder<S> = S & { ... }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 31};
    params.newName = "State";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type LayerBuilder<State> = State & { ... }
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_parameter_used_as_another_generic")
{
    auto source = R"(
        type Baz<S...> = Bar<S...>
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 17};
    params.newName = "State";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type Baz<State...> = Bar<State...>
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_parameter_in_function_definition")
{
    auto source = R"(
        function foo<T>(x: T, y: string)
        end
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 21};
    params.newName = "Value";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        function foo<Value>(x: Value, y: string)
        end
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_correctly_handle_shadowing_1")
{
    // Renaming "T" in Foo<T>
    auto source = R"(
        type Foo<T> = {
            x: T,
            fn: <T>(T, T) -> T
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 15};
    params.newName = "Value";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type Foo<Value> = {
            x: Value,
            fn: <T>(T, T) -> T
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_correctly_handle_shadowing_2")
{
    // Renaming first "T" in <T>(T, T) -> T
    auto source = R"(
        type Foo<T> = {
            x: T,
            fn: <T>(T, T) -> T
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 17};
    params.newName = "Value";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type Foo<T> = {
            x: T,
            fn: <Value>(Value, Value) -> Value
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_generic_type_correctly_handle_shadowing_3")
{
    // Renaming second "T" in <T>(T, T) -> T
    auto source = R"(
        type Foo<T> = {
            x: T,
            fn: <T>(T, T) -> T
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 20};
    params.newName = "Value";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type Foo<T> = {
            x: T,
            fn: <Value>(Value, Value) -> Value
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "renaming_required_variable_should_also_rename_imported_type_prefixes")
{
    auto source = R"(
        local Types = require("path/to/types")

        type Foo = Types.Foo
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 14};
    params.newName = "ActualTypes";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        local ActualTypes = require("path/to/types")

        type Foo = ActualTypes.Foo
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_global_function_name")
{
    auto source = R"(
        function Main()
        end

        Main()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 11};
    params.newName = "Test";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        function Test()
        end

        Test()
    )");
}

TEST_CASE_FIXTURE(Fixture, "disallow_renaming_of_global_from_type_definition")
{
    auto source = R"(
        local x = game:GetService("Foo")
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 19}; // 'game'
    params.newName = "game2";

    REQUIRE_THROWS_WITH_AS(workspace.rename(params, nullptr), "Cannot rename a global variable", JsonRpcException);
}

TEST_CASE_FIXTURE(Fixture, "dont_rename_cross_module_usages_of_a_returned_local_function")
{
    auto uri = newDocument("useFunction.luau", R"(
        local function useFunction()
        end

        return useFunction
    )");

    auto user = newDocument("user.luau", R"(
        local useFunction = require("useFunction.luau")

        local value = useFunction()
    )");

    workspace.frontend.parse(workspace.fileResolver.getModuleName(user));

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 27}; // 'useFunction' definition
    params.newName = "useFunction2";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE_EQ(result->changes.size(), 1);
    CHECK_EQ(result->changes.begin()->first, uri);
    CHECK_EQ(result->changes.begin()->second.size(), 2);
}

TEST_CASE_FIXTURE(Fixture, "dont_rename_cross_module_usages_of_a_returned_global_function")
{
    auto uri = newDocument("useFunction.luau", R"(
        function useFunction()
        end

        return useFunction
    )");

    auto user = newDocument("user.luau", R"(
        local useFunction = require("useFunction.luau")

        local value = useFunction()
    )");

    workspace.frontend.parse(workspace.fileResolver.getModuleName(user));

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 20}; // 'useFunction' definition
    params.newName = "useFunction2";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE_EQ(result->changes.size(), 1);
    CHECK_EQ(result->changes.begin()->first, uri);
    CHECK_EQ(result->changes.begin()->second.size(), 2);
}

TEST_CASE_FIXTURE(Fixture, "dont_rename_cross_module_usages_of_a_returned_table")
{
    auto uri = newDocument("tbl.luau", R"(
        local tbl = {}

        return tbl
    )");

    auto user = newDocument("user.luau", R"(
        local tbl = require("tbl.luau")

        local value = tbl
    )");

    workspace.frontend.parse(workspace.fileResolver.getModuleName(user));

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 15}; // 'tbl' definition
    params.newName = "tbl2";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE_EQ(result->changes.size(), 1);
    CHECK_EQ(result->changes.begin()->first, uri);
    CHECK_EQ(result->changes.begin()->second.size(), 2);
}

TEST_CASE_FIXTURE(Fixture, "response_json_is_valid_structure")
{
    auto uri = newDocument("tbl.luau", R"(
        local value = 1
    )");

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 15}; // 'value' definition
    params.newName = "value2";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE_EQ(result->changes.size(), 1);

    json response = result;
    CHECK_EQ(response.dump(), R"({"changes":{")" + uri.toString() +
                                  R"(":[{"newText":"value2","range":{"end":{"character":19,"line":1},"start":{"character":14,"line":1}}}]}})");
}

TEST_CASE_FIXTURE(Fixture, "rename_respects_cancellation")
{
    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();

    auto document = newDocument("a.luau", "local x = 1");
    CHECK_THROWS_AS(workspace.rename(lsp::RenameParams{{{document}, lsp::Position{}}, "y"}, cancellationToken), RequestCancelledException);
}

TEST_CASE_FIXTURE(Fixture, "rename_property_from_bracket_notation")
{
    auto source = R"(
        type Tbl = {
            name: string
        }

        local x: Tbl
        local v = x["name"]
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{6, 22}; // cursor on 'name' inside brackets
    params.newName = "title";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type Tbl = {
            title: string
        }

        local x: Tbl
        local v = x["title"]
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_property_affects_both_dot_and_bracket_notation")
{
    auto source = R"(
        type Tbl = {
            name: string
        }

        local x: Tbl
        local v1 = x.name
        local v2 = x["name"]
    )";

    auto uri = newDocument("foo.luau", source);

    // Rename from dot notation should also update bracket notation
    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{6, 22}; // cursor on 'name' in x.name
    params.newName = "title";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        type Tbl = {
            title: string
        }

        local x: Tbl
        local v1 = x.title
        local v2 = x["title"]
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_property_from_bracket_notation_definition_in_table_literal")
{
    // When table is defined using bracket notation keys: {["key"] = value}
    auto source = R"(
        local T = {
            ["name"] = "string"
        }

        local v1 = T.name
        local v2 = T["name"]
    )";

    auto uri = newDocument("foo.luau", source);

    // Rename from the bracket notation definition in the table literal
    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 15}; // cursor on 'name' inside ["name"] definition
    params.newName = "title";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
        local T = {
            ["title"] = "string"
        }

        local v1 = T.title
        local v2 = T["title"]
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_method_through_metatable_inheritance")
{
    auto source = R"(
local Foo = {}
Foo.__index = Foo
export type Foo = typeof(setmetatable({}, Foo))

function Foo.Test(self: Foo, a)
    print(a)
end

local Bar = setmetatable({}, Foo)
Bar.__index = Bar
export type Bar = typeof(setmetatable({}, Foo))

function Bar.new()
    local self = setmetatable({}, Bar)
    return self
end

function Bar.DoSomething(self: Bar)
    self:Test("Hello, World!")
end

return Bar
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{5, 13}; // cursor on 'Test' in function Foo.Test
    params.newName = "Run";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
local Foo = {}
Foo.__index = Foo
export type Foo = typeof(setmetatable({}, Foo))

function Foo.Run(self: Foo, a)
    print(a)
end

local Bar = setmetatable({}, Foo)
Bar.__index = Bar
export type Bar = typeof(setmetatable({}, Foo))

function Bar.new()
    local self = setmetatable({}, Bar)
    return self
end

function Bar.DoSomething(self: Bar)
    self:Run("Hello, World!")
end

return Bar
    )");
}

TEST_CASE_FIXTURE(Fixture, "rename_method_from_metatable_call_site")
{
    auto source = R"(
local Foo = {}
Foo.__index = Foo
export type Foo = typeof(setmetatable({}, Foo))

function Foo.Test(self: Foo, a)
    print(a)
end

local Bar = setmetatable({}, Foo)
Bar.__index = Bar
export type Bar = typeof(setmetatable({}, Foo))

function Bar.new()
    local self = setmetatable({}, Bar)
    return self
end

function Bar.DoSomething(self: Bar)
    self:Test("Hello, World!")
end

return Bar
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{19, 9}; // cursor on 'Test' in self:Test()
    params.newName = "Run";

    auto result = workspace.rename(params, nullptr);
    REQUIRE(result);
    REQUIRE(result->changes.size() == 1);

    auto documentEdits = result->changes.begin()->second;
    CHECK_EQ(applyEdit(source, documentEdits), R"(
local Foo = {}
Foo.__index = Foo
export type Foo = typeof(setmetatable({}, Foo))

function Foo.Run(self: Foo, a)
    print(a)
end

local Bar = setmetatable({}, Foo)
Bar.__index = Bar
export type Bar = typeof(setmetatable({}, Foo))

function Bar.new()
    local self = setmetatable({}, Bar)
    return self
end

function Bar.DoSomething(self: Bar)
    self:Run("Hello, World!")
end

return Bar
    )");
}

// The position of the `occurrence`th (0-based) `needle` in `source`
static lsp::Position positionOf(const std::string& source, const std::string& needle, size_t occurrence = 0)
{
    size_t offset = source.find(needle);
    for (size_t i = 0; i < occurrence; i++)
        offset = source.find(needle, offset + 1);
    REQUIRE(offset != std::string::npos);

    size_t line = std::count(source.begin(), source.begin() + offset, '\n');
    size_t lineStart = source.rfind('\n', offset);
    size_t column = lineStart == std::string::npos ? offset : offset - lineStart - 1;
    return lsp::Position{line, column};
}

static std::vector<lsp::Position> renamedPositions(const lsp::RenameResult& result, const Uri& uri)
{
    std::vector<lsp::Position> positions;
    REQUIRE(result);
    if (auto it = result->changes.find(uri); it != result->changes.end())
        for (const auto& edit : it->second)
            positions.push_back(edit.range.start);
    std::sort(positions.begin(), positions.end(), [](auto& a, auto& b) { return a.line < b.line || (a.line == b.line && a.character < b.character); });
    return positions;
}

TEST_CASE_FIXTURE(Fixture, "rename_class_method_across_modules")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ENABLE_NEW_SOLVER();

    std::string libSource = R"(
export class Dog
    public function __init(self) end
    public function bark(self)
    end
end
return { Dog = Dog }
)";
    std::string userSource = R"(
local lib = require("lib.luau")
local dog = lib.Dog()
dog:bark()
)";
    auto lib = newDocument("lib.luau", libSource);
    auto user = newDocument("user.luau", userSource);
    workspace.checkStrict(workspace.fileResolver.getModuleName(user), nullptr);

    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{user};
    params.position = positionOf(userSource, "bark");
    params.newName = "woof";

    auto result = workspace.rename(params, nullptr);
    CHECK_EQ(renamedPositions(result, lib), std::vector{positionOf(libSource, "bark")});
    CHECK_EQ(renamedPositions(result, user), std::vector{positionOf(userSource, "bark")});
}

TEST_CASE_FIXTURE(Fixture, "rename_trait_method_from_usage_in_another_module")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    std::string libSource = R"(
export trait Fs
    @[truthy(self, Dir)]
    public function is_dir(self)
        return class.isinstance(self, Dir)
    end
end

export class Dir implements Fs
    public function __init(self) end
    public function list(self): { Fs }
        return { self }
    end
end

return { Dir = Dir }
)";
    std::string userSource = R"(
local lib = require("lib.luau")
local d = lib.Dir()
if d:is_dir() then
    for _, x in d:list() do
        if x:is_dir() then end
    end
end
)";
    auto lib = newDocument("lib.luau", libSource);
    auto user = newDocument("user.luau", userSource);
    workspace.checkStrict(workspace.fileResolver.getModuleName(user), nullptr);

    // on a trait-typed value, and on a value of a class implementing the trait
    for (size_t occurrence : {0, 1})
    {
        lsp::RenameParams params;
        params.textDocument = lsp::TextDocumentIdentifier{user};
        params.position = positionOf(userSource, "is_dir", occurrence);
        params.newName = "is_directory";

        auto result = workspace.rename(params, nullptr);
        CHECK_EQ(renamedPositions(result, lib), std::vector{positionOf(libSource, "is_dir")});
        CHECK_EQ(renamedPositions(result, user), std::vector{positionOf(userSource, "is_dir", 0), positionOf(userSource, "is_dir", 1)});
    }
}

TEST_CASE_FIXTURE(Fixture, "rename_trait_expected_function_renames_implementations")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    std::string source = R"(
trait A
    expect function speak(self): string
    function a(self): string return self:speak() end
end

trait B
    expect function speak(self): string
end

class C implements A, B
    function speak(self): string return "hi" end
end

class D implements B
    function speak(self): string return "bye" end
end

class Unrelated
    function speak(self): string return "?" end
end

local c = C()
local x = c:speak()
local u = Unrelated():speak()
)";
    auto uri = newDocument("test.luau", source);

    // from A's declaration: C fulfills A and B, so B and D's go too
    lsp::RenameParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = positionOf(source, "speak");
    params.newName = "talk";

    auto result = workspace.rename(params, nullptr);
    CHECK_EQ(renamedPositions(result, uri),
        std::vector{
            positionOf(source, "speak", 0), // A
            positionOf(source, "speak", 1), // self:speak()
            positionOf(source, "speak", 2), // B
            positionOf(source, "speak", 3), // C
            positionOf(source, "speak", 4), // D
            positionOf(source, "speak", 6), // c:speak()
        });
}

TEST_SUITE_END();
