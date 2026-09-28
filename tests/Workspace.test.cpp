#include "doctest.h"

#include <filesystem>
#include "Fixture.h"
#include "TempDir.h"
#include "LSP/WorkspaceFileResolver.hpp"
#include "Platform/RobloxPlatform.hpp"

TEST_SUITE_BEGIN("WorkspaceTest");

TEST_CASE_FIXTURE(Fixture, "managed_files_correctly_resolves_for_untitled_uris")
{
    Uri uri = Uri::parse("untitled:Untitled-1");
    workspace.openTextDocument(uri, {{uri, "luau", 0, "Hello, World!"}});

    auto textDocumentFromUri = workspace.fileResolver.getTextDocument(uri);
    REQUIRE(textDocumentFromUri);
    CHECK_EQ(textDocumentFromUri->getText(), "Hello, World!");

    auto moduleName = workspace.fileResolver.getModuleName(uri);
    auto textDocumentFromModuleName = workspace.fileResolver.getTextDocumentFromModuleName(moduleName);
    REQUIRE(textDocumentFromModuleName);
    CHECK_EQ(textDocumentFromModuleName->getText(), "Hello, World!");
    CHECK_EQ(textDocumentFromUri, textDocumentFromModuleName);
}

TEST_CASE_FIXTURE(Fixture, "managed_files_correctly_resolves_for_non_file_uris")
{
    Uri uri = Uri::parse("inmemory://model/1");
    workspace.openTextDocument(uri, {{uri, "luau", 0, "Hello, World!"}});

    auto textDocumentFromUri = workspace.fileResolver.getTextDocument(uri);
    REQUIRE(textDocumentFromUri);
    CHECK_EQ(textDocumentFromUri->getText(), "Hello, World!");

    auto moduleName = workspace.fileResolver.getModuleName(uri);
    auto textDocumentFromModuleName = workspace.fileResolver.getTextDocumentFromModuleName(moduleName);
    REQUIRE(textDocumentFromModuleName);
    CHECK_EQ(textDocumentFromModuleName->getText(), "Hello, World!");
    CHECK_EQ(textDocumentFromUri, textDocumentFromModuleName);
}

TEST_CASE_FIXTURE(Fixture, "managed_files_correctly_resolves_for_file_uris")
{
    Uri uri = Uri::parse("file:///source.luau");
    workspace.openTextDocument(uri, {{uri, "luau", 0, "Hello, World!"}});

    auto textDocumentFromUri = workspace.fileResolver.getTextDocument(uri);
    REQUIRE(textDocumentFromUri);
    CHECK_EQ(textDocumentFromUri->getText(), "Hello, World!");

    auto moduleName = workspace.fileResolver.getModuleName(uri);
    auto textDocumentFromModuleName = workspace.fileResolver.getTextDocumentFromModuleName(moduleName);
    REQUIRE(textDocumentFromModuleName);
    CHECK_EQ(textDocumentFromModuleName->getText(), "Hello, World!");
    CHECK_EQ(textDocumentFromUri, textDocumentFromModuleName);
}

TEST_CASE_FIXTURE(RobloxFixture, "managed_files_correctly_resolves_for_virtual_paths")
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
                        "name": "File",
                        "className": "ModuleScript",
                        "filePaths": ["source.luau"]
                    }
                ]
            }
        ]
    })");

    Uri uri = workspace.rootUri.resolvePath("source.luau");
    workspace.openTextDocument(uri, {{uri, "luau", 0, "Hello, World!"}});

    auto textDocumentFromUri = workspace.fileResolver.getTextDocument(uri);
    REQUIRE(textDocumentFromUri);
    CHECK_EQ(textDocumentFromUri->getText(), "Hello, World!");

    auto moduleName = workspace.fileResolver.getModuleName(uri);
    CHECK_EQ(moduleName, "game/ReplicatedStorage/File");

    auto textDocumentFromModuleName = workspace.fileResolver.getTextDocumentFromModuleName(moduleName);
    REQUIRE(textDocumentFromModuleName);
    CHECK_EQ(textDocumentFromModuleName->getText(), "Hello, World!");
    CHECK_EQ(textDocumentFromUri, textDocumentFromModuleName);
}

TEST_CASE_FIXTURE(Fixture, "isIgnoredFile")
{
    client->globalConfig.ignoreGlobs = {"**/_Index/**"};

    CHECK_EQ(workspace.isIgnoredFile(workspace.rootUri.resolvePath("source.luau")), false);
    CHECK_EQ(workspace.isIgnoredFile(workspace.rootUri.resolvePath("Packages/_Index/source.luau")), true);

    // Test upper vs. lower case drive letter
#ifdef _WIN32
    auto path = workspace.rootUri.fsPath();
    REQUIRE(path.size() >= 2);
    REQUIRE(path[1] == ':');
    auto lowercasedDriveLetter = std::filesystem::path(std::string(1, toupper(path[0])) + path.substr(1));
    auto uppercasedDriveLetter = std::filesystem::path(std::string(1, tolower(path[0])) + path.substr(1));
    CHECK_EQ(workspace.isIgnoredFile(Uri::file((lowercasedDriveLetter / "Packages/_Index/source.luau").generic_string())), true);
    CHECK_EQ(workspace.isIgnoredFile(Uri::file((uppercasedDriveLetter / "Packages/_Index/source.luau").generic_string())), true);
#endif
}

TEST_CASE_FIXTURE(Fixture, "isDefinitionsFile")
{
    client->globalConfig.types.definitionFiles = {{"@roblox", "globalTypes.d.luau"}};

    CHECK_EQ(workspace.isDefinitionFile(workspace.rootUri.resolvePath("source.luau")), false);
    CHECK_EQ(workspace.isDefinitionFile(workspace.rootUri.resolvePath("globalTypes.d.luau")), true);
}

TEST_CASE_FIXTURE(Fixture, "files_in_alias_directories_are_indexed")
{
    TempDir library("index_files_resolve_aliases_library");
    auto module = library.write_child("module.luau", R"(
        return {}
    )");

    auto luaurc = std::string(R"(
    {
        "aliases": {
            "library": "{filepath}"
        }
    }
    )");
    replace(luaurc, "{filepath}", library.path());
    loadLuaurc(luaurc);
    auto moduleName = workspace.fileResolver.getModuleName(Uri::file(module));

    CHECK_FALSE(workspace.frontend.getSourceModule(moduleName));

    client->globalConfig.index.enabled = true;
    workspace.indexFiles(client->globalConfig);

    CHECK(workspace.frontend.getSourceModule(moduleName));
}

TEST_CASE_FIXTURE(Fixture, "ignored_files_are_marked_as_dirty_when_changed_externally")
{
    client->globalConfig.ignoreGlobs = {"**/ignored/**"};

    auto ignoredFile = newDocument("sample/ignored/main.luau", R"(
         --!strict
         return {}
     )");

    REQUIRE(workspace.isIgnoredFile(ignoredFile));

    auto moduleName = workspace.fileResolver.getModuleName(ignoredFile);
    workspace.frontend.check(moduleName);
    REQUIRE_FALSE(workspace.frontend.isDirty(moduleName));

    // Simulate external file change
    lsp::FileEvent event{ignoredFile, lsp::FileChangeType::Changed};
    workspace.onDidChangeWatchedFiles({event});

    CHECK(workspace.frontend.isDirty(moduleName));
}

/// The event a client sends once a file is gone from disk
static lsp::FileEvent fileEvent(const Uri& uri, lsp::FileChangeType type)
{
    lsp::FileEvent event;
    event.uri = uri;
    event.type = type;
    return event;
}

TEST_CASE_FIXTURE(Fixture, "deleting_a_file_removes_its_module_from_the_frontend")
{
    // A module used to stay in the frontend for the rest of the session once its file was gone,
    // because a deletion only marked it dirty. Anything that walks sourceNodes -- auto-imports,
    // the require graph, workspace symbols -- kept offering a file that no longer existed.
    tempDir.write_child("temporary.luau", "return {}");
    auto uri = newDocument("temporary.luau", "return {}");
    auto moduleName = workspace.fileResolver.getModuleName(uri);

    REQUIRE(workspace.frontend.sourceNodes.count(moduleName) == 1);

    std::filesystem::remove(std::filesystem::path(uri.fsPath()));
    workspace.closeTextDocument(uri);
    workspace.onDidChangeWatchedFiles({fileEvent(uri, lsp::FileChangeType::Deleted)});

    CHECK(workspace.frontend.sourceNodes.count(moduleName) == 0);
}

TEST_CASE_FIXTURE(Fixture, "deleting_a_file_makes_the_files_that_required_it_check_again")
{
    // The dependents have to be dirtied, or a module that required the deleted file keeps its
    // cached result and never reports the require that is now broken.
    tempDir.write_child("dependency.luau", "return {}");
    tempDir.write_child("dependent.luau", R"(local dependency = require("./dependency"))");
    auto dependency = newDocument("dependency.luau", "return {}");
    auto dependent = newDocument("dependent.luau", R"(local dependency = require("./dependency"))");

    auto dependentModule = workspace.fileResolver.getModuleName(dependent);
    workspace.frontend.check(dependentModule);
    REQUIRE(workspace.frontend.isDirty(dependentModule) == false);

    std::filesystem::remove(std::filesystem::path(dependency.fsPath()));
    workspace.closeTextDocument(dependency);
    workspace.onDidChangeWatchedFiles({fileEvent(dependency, lsp::FileChangeType::Deleted)});

    CHECK(workspace.frontend.isDirty(dependentModule));
}

TEST_CASE_FIXTURE(Fixture, "a_deleted_file_stops_showing_up_in_workspace_symbols")
{
    // The symptom of a module outliving its file: workspace symbols walks frontend.sourceModules,
    // so every deleted file keeps answering Ctrl+T for the rest of the session.
    tempDir.write_child("temporary.luau", "local temporarySymbol = 1");
    auto uri = newDocument("temporary.luau", "local temporarySymbol = 1");

    lsp::WorkspaceSymbolParams params;
    params.query = "temporarySymbol";

    auto before = workspace.workspaceSymbol(params);
    REQUIRE(before);
    REQUIRE_FALSE(before->empty());

    std::filesystem::remove(std::filesystem::path(uri.fsPath()));
    workspace.closeTextDocument(uri);
    workspace.onDidChangeWatchedFiles({fileEvent(uri, lsp::FileChangeType::Deleted)});

    auto after = workspace.workspaceSymbol(params);
    REQUIRE(after);
    CHECK(after->empty());
}

TEST_CASE_FIXTURE(Fixture, "a_file_deleted_and_written_again_in_one_batch_keeps_its_module")
{
    // What a test run's temporary files look like from here: the file is gone and back before we
    // are told about either event, so the batch must not leave us having forgotten a file that is
    // sitting on disk.
    tempDir.write_child("temporary.luau", "return {}");
    auto uri = newDocument("temporary.luau", "return {}");
    auto moduleName = workspace.fileResolver.getModuleName(uri);

    workspace.onDidChangeWatchedFiles({
        fileEvent(uri, lsp::FileChangeType::Deleted),
        fileEvent(uri, lsp::FileChangeType::Created),
    });

    CHECK(workspace.frontend.sourceNodes.count(moduleName) == 1);
}

TEST_SUITE_END();
