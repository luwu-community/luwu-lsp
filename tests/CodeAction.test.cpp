#include "doctest.h"
#include "Fixture.h"
#include "RobloxTestConstants.h"
#include "Platform/RobloxPlatform.hpp"

LUAU_FASTFLAG(LuwuClasses)
LUAU_FASTFLAG(LuwuTraits)

TEST_SUITE_BEGIN("CodeAction");

TEST_CASE_FIXTURE(Fixture, "organise_imports_action_is_returned")
{
    auto uri = newDocument("test.luau", R"(
        local b = require("./b.luau")
        local a = require("./a.luau")
    )");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {3, 0}};
    params.context.only = {lsp::CodeActionKind::SourceOrganizeImports};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Sort requires");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::SourceOrganizeImports);
}

TEST_CASE_FIXTURE(Fixture, "global_used_as_local_quick_fix")
{
    // GlobalUsedAsLocal lint triggers when a global is assigned but never read before being written
    // and is not at module scope (i.e., inside a function)
    auto uri = newDocument("test.luau", R"(
local function foo()
    x = 1
    print(x)
end
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{2, 0}, {2, 10}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Add 'local' to global variable");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    CHECK(action->isPreferred == true);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK_EQ(changes[0].newText, "local ");
}

TEST_CASE_FIXTURE(Fixture, "quick_fix_only_returns_for_matching_range")
{
    auto uri = newDocument("test.luau", R"(
local function foo()
    x = 1
    print(x)
end
)");

    // Request code actions for a range that doesn't include the lint (line 4 is "end")
    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{4, 0}, {5, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Add 'local' to global variable");
    CHECK_FALSE(action);
}

TEST_CASE_FIXTURE(Fixture, "local_unused_prefix_fix")
{
    auto uri = newDocument("test.luau", R"(
local unused = 1
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {2, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto prefixAction = findCodeAction(result, "Prefix 'unused' with '_' to silence");
    REQUIRE(prefixAction);
    CHECK(prefixAction->kind == lsp::CodeActionKind::QuickFix);
    CHECK(prefixAction->isPreferred == false);
    REQUIRE(prefixAction->edit);
    auto& prefixChanges = prefixAction->edit->changes.at(uri);
    REQUIRE_EQ(prefixChanges.size(), 1);
    CHECK_EQ(prefixChanges[0].newText, "_");
}

TEST_CASE_FIXTURE(Fixture, "local_unused_delete_fix")
{
    auto uri = newDocument("test.luau", R"(
local unused = 1
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {2, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto deleteAction = findCodeAction(result, "Remove unused variable: 'unused'");
    REQUIRE(deleteAction);
    CHECK(deleteAction->kind == lsp::CodeActionKind::QuickFix);
    CHECK(deleteAction->isPreferred == false);
    REQUIRE(deleteAction->edit);
    auto& deleteChanges = deleteAction->edit->changes.at(uri);
    REQUIRE_EQ(deleteChanges.size(), 1);
    CHECK_EQ(deleteChanges[0].newText, "");
    CHECK_EQ(deleteChanges[0].range.start.line, 1);
    CHECK_EQ(deleteChanges[0].range.end.line, 2);
}

TEST_CASE_FIXTURE(Fixture, "function_unused_prefix_fix")
{
    auto uri = newDocument("test.luau", R"(
local function unused()
    return 1
end
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {4, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto prefixAction = findCodeAction(result, "Prefix 'unused' with '_' to silence");
    REQUIRE(prefixAction);
    CHECK(prefixAction->kind == lsp::CodeActionKind::QuickFix);
    CHECK(prefixAction->isPreferred == false);
    REQUIRE(prefixAction->edit);
    auto& prefixChanges = prefixAction->edit->changes.at(uri);
    REQUIRE_EQ(prefixChanges.size(), 1);
    CHECK_EQ(prefixChanges[0].newText, "_");
}

TEST_CASE_FIXTURE(Fixture, "function_unused_delete_fix")
{
    auto uri = newDocument("test.luau", R"(
local function unused()
    return 1
end
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {4, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto deleteAction = findCodeAction(result, "Remove unused function: 'unused'");
    REQUIRE(deleteAction);
    CHECK(deleteAction->kind == lsp::CodeActionKind::QuickFix);
    CHECK(deleteAction->isPreferred == false);
    REQUIRE(deleteAction->edit);
    auto& deleteChanges = deleteAction->edit->changes.at(uri);
    REQUIRE_EQ(deleteChanges.size(), 1);
    CHECK_EQ(deleteChanges[0].newText, "");
    CHECK_EQ(deleteChanges[0].range.start.line, 1);
    CHECK_EQ(deleteChanges[0].range.end.line, 4);
}

TEST_CASE_FIXTURE(Fixture, "import_unused_prefix_fix")
{
    auto uri = newDocument("test.luau", R"(
local unused = require("./foo.luau")
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {2, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto prefixAction = findCodeAction(result, "Prefix 'unused' with '_' to silence");
    REQUIRE(prefixAction);
    CHECK(prefixAction->kind == lsp::CodeActionKind::QuickFix);
    CHECK(prefixAction->isPreferred == false);
    REQUIRE(prefixAction->edit);
    auto& prefixChanges = prefixAction->edit->changes.at(uri);
    REQUIRE_EQ(prefixChanges.size(), 1);
    CHECK_EQ(prefixChanges[0].newText, "_");
}

TEST_CASE_FIXTURE(Fixture, "import_unused_delete_fix")
{
    auto uri = newDocument("test.luau", R"(
local unused = require("./foo.luau")
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {2, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto deleteAction = findCodeAction(result, "Remove unused import: 'unused'");
    REQUIRE(deleteAction);
    CHECK(deleteAction->kind == lsp::CodeActionKind::QuickFix);
    CHECK(deleteAction->isPreferred == false);
    REQUIRE(deleteAction->edit);
    auto& deleteChanges = deleteAction->edit->changes.at(uri);
    REQUIRE_EQ(deleteChanges.size(), 1);
    CHECK_EQ(deleteChanges[0].newText, "");
    CHECK_EQ(deleteChanges[0].range.start.line, 1);
    CHECK_EQ(deleteChanges[0].range.end.line, 2);
}

TEST_CASE_FIXTURE(Fixture, "unreachable_code_fix")
{
    auto uri = newDocument("test.luau", R"(
local function test()
    error("always errors")
    print("unreachable")
end
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{3, 0}, {4, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Remove unreachable code");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    CHECK(action->isPreferred == false);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK_EQ(changes[0].newText, "");
    CHECK_EQ(changes[0].range.start.line, 3);
    CHECK_EQ(changes[0].range.end.line, 4);
}

TEST_CASE_FIXTURE(Fixture, "remove_all_unused_code_source_action")
{
    auto uri = newDocument("test.luau", R"(
local unused1 = 1
local unused2 = 2
local function unusedFunc()
    return 3
end
print("hello")
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {7, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Remove all unused code");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::Source);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    // Should have 3 edits: unused1, unused2, and unusedFunc
    CHECK_EQ(changes.size(), 3);
}

TEST_CASE_FIXTURE(Fixture, "remove_all_unused_code_not_shown_when_no_unused")
{
    auto uri = newDocument("test.luau", R"(
local used = 1
print(used)
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {3, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Remove all unused code");
    CHECK_FALSE(action);
}

TEST_CASE_FIXTURE(Fixture, "const_local_quick_fix")
{
    auto uri = newDocument("test.luau", R"(--!lint ConstLocal
local rows = 1
print(rows)
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 6}, {1, 10}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Change 'local' to 'const'");
    REQUIRE(action);
    CHECK(action->isPreferred == true);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK_EQ(changes[0].newText, "const");
    CHECK_EQ(changes[0].range.start, lsp::Position{1, 0});
    CHECK_EQ(changes[0].range.end, lsp::Position{1, 5});
}

TEST_CASE_FIXTURE(Fixture, "const_local_fix_all_source_action")
{
    auto uri = newDocument("test.luau", R"(--!lint ConstLocal
local a = 1
local function f() return a end
local b = 2
b = 3
print(a, f(), b)
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {6, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Make all never-reassigned locals 'const'");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    // `a` and `f`, not `b`
    REQUIRE_EQ(changes.size(), 2);
    CHECK_EQ(changes[0].range.start.line, 1);
    CHECK_EQ(changes[1].range.start.line, 2);
}

TEST_CASE_FIXTURE(Fixture, "lua_and_or_quick_fix")
{
    auto uri = newDocument("test.luau", R"(
local c = math.random() > 0.5
local v = c and 1 or 2
print(v)
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{2, 12}, {2, 12}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Rewrite as 'if a then b else c'");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK_EQ(changes[0].newText, "if c then 1 else 2");
}

TEST_CASE_FIXTURE(Fixture, "lua_and_or_fix_all_handles_nesting_and_precedence")
{
    auto uri = newDocument("test.luau", R"(
local c = math.random() > 0.5
local a = c and (c and 1 or 2) or 3
local d = c and 1 or 2 or 3
print(a, d)
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {5, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Rewrite all 'a and b or c' as 'if a then b else c'");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);

    // the nested one is rewritten inside its parent's edit, so the edits don't overlap; the one that is an operand of
    // another `or` keeps parentheses, since an if-then-else would swallow the `or 3`
    REQUIRE_EQ(changes.size(), 2);
    CHECK_EQ(changes[0].newText, "if c then (if c then 1 else 2) else 3");
    CHECK_EQ(changes[1].newText, "(if c then 1 else 2)");
}

TEST_CASE_FIXTURE(Fixture, "binding_keyword_toggle")
{
    auto uri = newDocument("test.luau", R"(
const fixed = 1
local free = 2
local changed = 3
changed = 4
local f = function()
    local inner = 1
    return inner
end
f = nil
print(fixed, free, changed, f)
)");

    auto actionsAt = [&](size_t line, size_t column)
    {
        lsp::CodeActionParams params;
        params.textDocument.uri = uri;
        params.range = {{line, column}, {line, column}};
        params.context.only = {lsp::CodeActionKind::RefactorRewrite};
        return workspace.codeAction(params, nullptr);
    };

    auto toLocal = findCodeAction(actionsAt(1, 7), "Switch to 'local'");
    REQUIRE(toLocal);
    REQUIRE(toLocal->edit);
    CHECK_EQ(toLocal->edit->changes.at(uri)[0].newText, "local");

    auto toConst = findCodeAction(actionsAt(2, 0), "Switch to 'const'");
    REQUIRE(toConst);
    CHECK_EQ(toConst->edit->changes.at(uri)[0].newText, "const");

    // reassigned locals can be switched too: the errors that follow show what reassigns them
    CHECK(findCodeAction(actionsAt(3, 7), "Switch to 'const'"));
    // the innermost binding under the cursor is the one switched
    auto inner = findCodeAction(actionsAt(6, 11), "Switch to 'const'");
    REQUIRE(inner);
    CHECK_EQ(inner->edit->changes.at(uri)[0].range.start, lsp::Position{6, 4});
}

TEST_CASE_FIXTURE(Fixture, "redundant_native_attribute_fix")
{
    auto uri = newDocument("test.luau", R"(--!native
@native
local function foo()
    return 1
end
print(foo())
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {2, 0}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Remove redundant @native attribute");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    CHECK(action->isPreferred == false);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK_EQ(changes[0].newText, "");
    CHECK_EQ(changes[0].range.start.line, 1);
    CHECK_EQ(changes[0].range.end.line, 2);
}

TEST_CASE_FIXTURE(Fixture, "unknown_global_add_require_fix")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;

    auto moduleUri = newDocument("MyModule.luau", R"(
return {}
)");
    workspace.frontend.check(workspace.fileResolver.getModuleName(moduleUri));

    std::string source = dedent(R"(
        local x = MyModule
    )");
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 10}, {0, 18}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add require for 'MyModule' from \"./MyModule\"");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    CHECK(action->isPreferred == true);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local MyModule = require("./MyModule")
        local x = MyModule
    )"));
}

TEST_CASE_FIXTURE(Fixture, "unknown_global_no_fix_when_no_matching_module")
{
    auto uri = newDocument("test.luau", R"(
local x = SomeUnknownGlobal
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 10}, {1, 27}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);
    REQUIRE(result);
    CHECK(result->empty());
}

TEST_CASE_FIXTURE(Fixture, "unknown_global_fix_inserts_at_correct_line")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;

    auto moduleUri = newDocument("OtherModule.luau", R"(
return {}
)");
    workspace.frontend.check(workspace.fileResolver.getModuleName(moduleUri));

    auto uri = newDocument("test.luau", R"(
local foo = require("./foo.luau")
local x = OtherModule
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{2, 10}, {2, 21}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add require for 'OtherModule' from \"./OtherModule\"");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK(changes[0].newText.find("local OtherModule = require") != std::string::npos);
}

TEST_CASE_FIXTURE(RobloxFixture, "unknown_global_offers_service_import_fix")
{
    std::string source = dedent(R"(
        local storage = ReplicatedStorage
    )");
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 16}, {0, 33}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Import service 'ReplicatedStorage'");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    CHECK(action->isPreferred == true);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local ReplicatedStorage = game:GetService("ReplicatedStorage")
        local storage = ReplicatedStorage
    )"));
}

TEST_CASE_FIXTURE(RobloxFixture, "unknown_global_offers_instance_based_require_fix")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage",
                "children": [
                    {
                        "name": "Folder",
                        "className": "Folder",
                        "children": [{ "name": "MyModule", "className": "ModuleScript" }]
                    }
                ]
            },
            {
                "name": "ServerScriptService",
                "className": "ServerScriptService",
                "children": [
                    { "name": "Script", "className": "Script", "filePaths": ["Script.server.luau"] }
                ]
            }
        ]
    }
    )");

    std::string source = dedent(R"(
        local x = MyModule
    )");
    auto uri = newDocument("Script.server.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 10}, {0, 18}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add require for 'MyModule' from \"ReplicatedStorage.Folder.MyModule\"");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 2);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local ReplicatedStorage = game:GetService("ReplicatedStorage")
        local MyModule = require(ReplicatedStorage.Folder.MyModule)
        local x = MyModule
    )"));
}

TEST_CASE_FIXTURE(RobloxFixture, "unknown_global_instance_require_reuses_existing_service")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage",
                "children": [
                    {
                        "name": "Folder",
                        "className": "Folder",
                        "children": [{ "name": "MyModule", "className": "ModuleScript" }]
                    }
                ]
            },
            {
                "name": "ServerScriptService",
                "className": "ServerScriptService",
                "children": [
                    { "name": "Script", "className": "Script", "filePaths": ["Script.server.luau"] }
                ]
            }
        ]
    }
    )");

    std::string source = dedent(R"(
        local ReplicatedStorage = game:GetService("ReplicatedStorage")
        local x = MyModule
    )");
    auto uri = newDocument("Script.server.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 10}, {1, 18}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add require for 'MyModule' from \"ReplicatedStorage.Folder.MyModule\"");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local ReplicatedStorage = game:GetService("ReplicatedStorage")
        local MyModule = require(ReplicatedStorage.Folder.MyModule)
        local x = MyModule
    )"));
}

TEST_CASE_FIXTURE(Fixture, "add_all_missing_requires_source_action")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;

    auto moduleA = newDocument("ModuleA.luau", R"(
return {}
)");
    auto moduleB = newDocument("ModuleB.luau", R"(
return {}
)");
    workspace.frontend.check(workspace.fileResolver.getModuleName(moduleA));
    workspace.frontend.check(workspace.fileResolver.getModuleName(moduleB));

    std::string source = dedent(R"(
        local x = ModuleA
        local y = ModuleB
    )");
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {2, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add all missing requires");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::Source);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 2);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local ModuleA = require("./ModuleA")
        local ModuleB = require("./ModuleB")
        local x = ModuleA
        local y = ModuleB
    )"));
}

TEST_CASE_FIXTURE(Fixture, "add_all_missing_requires_no_action_when_no_missing")
{
    auto uri = newDocument("test.luau", R"(
local x = 1
local y = 2
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {3, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add all missing requires");
    CHECK_FALSE(action);
}

TEST_CASE_FIXTURE(RobloxFixture, "add_all_missing_requires_with_services")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage",
                "children": [
                    { "name": "ModuleA", "className": "ModuleScript" },
                    { "name": "ModuleB", "className": "ModuleScript" }
                ]
            },
            {
                "name": "ServerScriptService",
                "className": "ServerScriptService",
                "children": [
                    { "name": "Script", "className": "Script", "filePaths": ["Script.server.luau"] }
                ]
            }
        ]
    }
    )");

    std::string source = dedent(R"(
        local a = ModuleA
        local b = ModuleB
        local rs = ReplicatedStorage
    )");
    auto uri = newDocument("Script.server.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {3, 0}};
    params.context.only = {lsp::CodeActionKind::Source};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Add all missing requires");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 3);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local ReplicatedStorage = game:GetService("ReplicatedStorage")
        local ModuleA = require(ReplicatedStorage.ModuleA)
        local ModuleB = require(ReplicatedStorage.ModuleB)
        local a = ModuleA
        local b = ModuleB
        local rs = ReplicatedStorage
    )"));
}

TEST_CASE_FIXTURE(Fixture, "misspelled_property_single_candidate_fix")
{
    auto uri = newDocument("test.luau", R"(
local t = {}
function t.Foo() end

t.fOo()
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{4, 2}, {4, 5}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Change 'fOo' to 'Foo'");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::QuickFix);
    CHECK(action->isPreferred == true);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);
    REQUIRE_EQ(changes.size(), 1);
    CHECK_EQ(changes[0].newText, "Foo");
}

TEST_CASE_FIXTURE(Fixture, "misspelled_property_multiple_candidates_fix")
{
    auto uri = newDocument("test.luau", R"(
local t = {}
function t.Foo() end
function t.FOO() end

t.foo()
)");

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{5, 2}, {5, 5}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    // Both candidates should be offered
    auto action1 = findCodeAction(result, "Change 'foo' to 'Foo'");
    auto action2 = findCodeAction(result, "Change 'foo' to 'FOO'");
    REQUIRE(action1);
    REQUIRE(action2);

    // Neither should be preferred when there are multiple candidates
    CHECK(action1->isPreferred == false);
    CHECK(action2->isPreferred == false);
}

TEST_CASE_FIXTURE(Fixture, "misspelled_property_no_fix_when_range_outside")
{
    auto uri = newDocument("test.luau", R"(
local t = {}
function t.Foo() end

t.fOo()
)");

    // Request code action for line 1 (not where the error is)
    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 0}, {1, 10}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);

    auto action = findCodeAction(result, "Change 'fOo' to 'Foo'");
    CHECK_FALSE(action);
}

TEST_CASE_FIXTURE(RobloxFixture, "sourcemap_unknown_symbol_fix_suggests_string_require")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;
    loadSourcemap(SOURCEMAP_FOR_STRING_REQUIRES);

    std::string source = dedent(R"(
        local x = ModuleB
    )");
    auto uri = newDocument("packages/core/ModuleA.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 10}, {0, 17}};
    params.context.only = {lsp::CodeActionKind::QuickFix};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Add require for 'ModuleB' from \"./ModuleB\"");
    REQUIRE(action);
    REQUIRE(action->edit);
    auto& changes = action->edit->changes.at(uri);

    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, dedent(R"(
        local ModuleB = require("./ModuleB")
        local x = ModuleB
    )"));
}

static std::vector<lsp::CodeAction> codeActionsAt(Fixture& fixture, const Uri& uri, size_t line, size_t column, std::vector<lsp::CodeActionKind> only = {})
{
    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{line, column}, {line, column}};
    params.context.only = std::move(only);
    return fixture.workspace.codeAction(params, nullptr).value_or(std::vector<lsp::CodeAction>{});
}

TEST_CASE_FIXTURE(Fixture, "parameter_list_layout_is_the_first_quick_fix_inside_the_parameters")
{
    auto uri = newDocument("test.luau", R"(
local function foo(a: number, b: { x: number, y: number })
    return a
end
)");

    auto actions = codeActionsAt(*this, uri, 1, 22);
    REQUIRE(!actions.empty());
    CHECK_EQ(actions[0].title, "Put each parameter on its own line");
    CHECK_EQ(actions[0].kind, lsp::CodeActionKind::QuickFix);
    REQUIRE(actions[0].edit);
    auto edits = actions[0].edit->changes.at(uri);
    REQUIRE_EQ(edits.size(), 1);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 18}, {1, 58}});
    CHECK_EQ(edits[0].newText, "(\n    a: number,\n    b: { x: number, y: number }\n)");

    // A client asking only for refactorings still gets it, as a rewrite
    auto rewrites = codeActionsAt(*this, uri, 1, 22, {lsp::CodeActionKind::RefactorRewrite});
    auto rewrite = findCodeAction(rewrites, "Put each parameter on its own line");
    REQUIRE(rewrite);
    CHECK_EQ(rewrite->kind, lsp::CodeActionKind::RefactorRewrite);

    // Outside the parameters there's nothing to lay out
    CHECK_FALSE(findCodeAction(codeActionsAt(*this, uri, 2, 5), "Put each parameter on its own line"));
}

TEST_CASE_FIXTURE(Fixture, "parameter_list_layout_puts_a_multiline_list_on_one_line")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto uri = newDocument("test.luau", R"(
trait Element(
    tag: string,
    id: number
)
end
)");

    auto actions = codeActionsAt(*this, uri, 2, 5);
    REQUIRE(!actions.empty());
    CHECK_EQ(actions[0].title, "Put parameters on one line");
    CHECK_EQ(actions[0].edit->changes.at(uri)[0].newText, "(tag: string, id: number)");
}

TEST_CASE_FIXTURE(Fixture, "trait_list_layout_puts_each_trait_on_its_own_line")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto uri = newDocument("test.luau", R"(
trait Named
    name = "x"
end
trait Aged end
class Person implements Named, Aged
end
trait Both needs Named, Aged end
)");

    for (auto [line, column] : std::vector<std::pair<size_t, size_t>>{{5, 15}, {5, 25}, {5, 34}})
    {
        auto actions = codeActionsAt(*this, uri, line, column);
        REQUIRE(!actions.empty());
        CHECK_EQ(actions[0].title, "Put each trait on its own line");
        CHECK_EQ(actions[0].kind, lsp::CodeActionKind::QuickFix);
        auto edits = actions[0].edit->changes.at(uri);
        REQUIRE_EQ(edits.size(), 1);
        CHECK_EQ(edits[0].range, lsp::Range{{5, 24}, {5, 35}});
        CHECK_EQ(edits[0].newText, "(\n    Named,\n    Aged\n)");
    }

    auto needs = codeActionsAt(*this, uri, 7, 12);
    REQUIRE(!needs.empty());
    CHECK_EQ(needs[0].title, "Put each trait on its own line");
    CHECK_EQ(needs[0].edit->changes.at(uri)[0].range, lsp::Range{{7, 17}, {7, 28}});

    // The class name isn't part of the list
    CHECK_FALSE(findCodeAction(codeActionsAt(*this, uri, 5, 8), "Put each trait on its own line"));
}

TEST_CASE_FIXTURE(Fixture, "trait_list_layout_puts_a_parenthesized_list_on_one_line")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto uri = newDocument("test.luau", R"(
trait Named end
trait Aged end
class Person implements (
    Named,
    Aged
)
end
class Single implements (Named, Aged) end
)");

    auto actions = codeActionsAt(*this, uri, 4, 6);
    REQUIRE(!actions.empty());
    CHECK_EQ(actions[0].title, "Put traits on one line");
    auto edits = actions[0].edit->changes.at(uri);
    REQUIRE_EQ(edits.size(), 1);
    CHECK_EQ(edits[0].range, lsp::Range{{3, 24}, {6, 1}});
    CHECK_EQ(edits[0].newText, "Named, Aged");

    // A parenthesized list on one line goes one trait per line, keeping its parentheses
    auto single = codeActionsAt(*this, uri, 8, 30);
    REQUIRE(!single.empty());
    CHECK_EQ(single[0].title, "Put each trait on its own line");
    CHECK_EQ(single[0].edit->changes.at(uri)[0].range, lsp::Range{{8, 24}, {8, 37}});
    CHECK_EQ(single[0].edit->changes.at(uri)[0].newText, "(\n    Named,\n    Aged\n)");
}

TEST_SUITE_END();
