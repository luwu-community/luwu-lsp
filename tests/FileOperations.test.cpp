#include "doctest.h"
#include "Fixture.h"
#include "LSP/LanguageServer.hpp"

TEST_SUITE_BEGIN("FileOperations");

/// Writes every file to disk before registering any of them, since a require is resolved when the
/// file containing it is parsed, and it only resolves to the right extension if the file it points
/// at is already there.
static std::vector<Uri> addFiles(Fixture* fixture, const std::vector<std::pair<std::string, std::string>>& files)
{
    for (const auto& [name, source] : files)
        fixture->tempDir.write_child(name, source);

    std::vector<Uri> uris;
    for (const auto& [name, source] : files)
        uris.emplace_back(fixture->newDocument(name, source));

    return uris;
}

static std::optional<lsp::WorkspaceEdit> renameFile(Fixture* fixture, const Uri& oldUri, const std::string& newName)
{
    lsp::RenameFilesParams params;
    params.files = {lsp::FileRename{oldUri.toString(), fixture->workspace.rootUri.resolvePath(newName).toString()}};

    return fixture->workspace.willRenameFiles(params);
}

static std::string editedSource(const lsp::WorkspaceEdit& edit, const Uri& uri, const std::string& source)
{
    auto changes = edit.changes.find(uri);
    REQUIRE(changes != edit.changes.end());
    return applyEdit(source, changes->second);
}

TEST_CASE_FIXTURE(Fixture, "renaming_a_file_updates_requires_pointing_at_it")
{
    auto source = R"(local b = require("./b"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});
    auto renamed = files.at(0);
    auto dependent = files.at(1);

    auto edit = renameFile(this, renamed, "c.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, dependent, source), R"(local b = require("./c"))");
}

TEST_CASE_FIXTURE(Fixture, "renaming_a_file_keeps_an_explicit_extension")
{
    auto source = R"(local b = require("./b.luau"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});

    auto edit = renameFile(this, files.at(0), "c.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, files.at(1), source), R"(local b = require("./c.luau"))");
}

TEST_CASE_FIXTURE(Fixture, "renaming_a_file_keeps_the_quote_style")
{
    auto source = "local b = require('./b')";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});

    auto edit = renameFile(this, files.at(0), "c.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, files.at(1), source), "local b = require('./c')");
}

TEST_CASE_FIXTURE(Fixture, "moving_a_file_into_a_directory_updates_requires_pointing_at_it")
{
    auto source = R"(local b = require("./b"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});

    auto edit = renameFile(this, files.at(0), "sub/b.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, files.at(1), source), R"(local b = require("./sub/b"))");
}

TEST_CASE_FIXTURE(Fixture, "moving_a_file_updates_the_requires_written_inside_it")
{
    auto source = R"(local b = require("./b"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});
    auto moved = files.at(1);

    auto edit = renameFile(this, moved, "sub/a.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, moved, source), R"(local b = require("../b"))");
}

TEST_CASE_FIXTURE(Fixture, "moving_a_file_out_of_a_directory_updates_the_requires_written_inside_it")
{
    auto source = R"(local b = require("../b"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"sub/a.luau", source}});
    auto moved = files.at(1);

    auto edit = renameFile(this, moved, "a.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, moved, source), R"(local b = require("./b"))");
}

TEST_CASE_FIXTURE(Fixture, "moving_a_file_and_the_file_it_requires_together_leaves_the_require_alone")
{
    auto source = R"(local b = require("./b"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});
    auto movedTarget = files.at(0);
    auto movedDependent = files.at(1);

    lsp::RenameFilesParams params;
    params.files = {
        lsp::FileRename{movedDependent.toString(), workspace.rootUri.resolvePath("sub/a.luau").toString()},
        lsp::FileRename{movedTarget.toString(), workspace.rootUri.resolvePath("sub/b.luau").toString()},
    };

    auto edit = workspace.willRenameFiles(params);
    CHECK(!edit);
}

TEST_CASE_FIXTURE(Fixture, "a_require_that_does_not_point_at_the_renamed_file_is_left_alone")
{
    auto source = R"(
        local b = require("./b")
        local c = require("./c")
    )";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"c.luau", "return {}"}, {"a.luau", source}});
    auto renamed = files.at(1);
    auto dependent = files.at(2);

    auto edit = renameFile(this, renamed, "d.luau");
    REQUIRE(edit);

    auto changes = edit->changes.find(dependent);
    REQUIRE(changes != edit->changes.end());
    CHECK_EQ(changes->second.size(), 1);
    CHECK_EQ(changes->second.at(0).newText, "./d");
}

TEST_CASE_FIXTURE(Fixture, "renaming_an_init_file_rewrites_the_directory_require_that_reached_it")
{
    auto source = R"(local lib = require("./lib"))";
    auto files = addFiles(this, {{"lib/init.luau", "return {}"}, {"a.luau", source}});

    auto edit = renameFile(this, files.at(0), "lib/main.luau");
    REQUIRE(edit);
    CHECK_EQ(editedSource(*edit, files.at(1), source), R"(local lib = require("./lib/main"))");
}

TEST_CASE_FIXTURE(Fixture, "an_aliased_require_is_left_alone")
{
    loadLuaurc(R"({"aliases": {"lib": "."}})");

    auto source = R"(local b = require("@lib/b"))";
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", source}});

    auto edit = renameFile(this, files.at(0), "c.luau");
    CHECK(!edit);
}

TEST_CASE_FIXTURE(Fixture, "renaming_to_a_non_source_extension_is_ignored")
{
    auto files = addFiles(this, {{"b.luau", "return {}"}, {"a.luau", R"(local b = require("./b"))"}});

    auto edit = renameFile(this, files.at(0), "b.txt");
    CHECK(!edit);
}

TEST_CASE_FIXTURE(Fixture, "renaming_a_file_nothing_requires_produces_no_edits")
{
    auto files = addFiles(this, {{"a.luau", "return {}"}, {"b.luau", "return {}"}});

    auto edit = renameFile(this, files.at(1), "c.luau");
    CHECK(!edit);
}

TEST_CASE("the_server_asks_to_be_told_about_source_file_renames")
{
    // Without this the client never sends us workspace/willRenameFiles in the first place
    auto capabilities = LanguageServer::getServerCapabilities();

    REQUIRE(capabilities.workspace);
    REQUIRE(capabilities.workspace->fileOperations);
    REQUIRE(capabilities.workspace->fileOperations->willRename);

    auto filters = capabilities.workspace->fileOperations->willRename->filters;
    REQUIRE_EQ(filters.size(), 1);
    CHECK_EQ(filters.at(0).pattern.glob, "**/*.{luau,luwu,lua}");
    CHECK_EQ(filters.at(0).pattern.matches, "file");
}

TEST_CASE("the_server_routes_a_will_rename_files_request")
{
    TestClient client;
    LanguageServer server(&client, std::nullopt);

    // Indexing throws errors as the workspace doesn't exist
    client.globalConfig.index.enabled = false;

    auto workspaceUri = Uri::file("project");

    lsp::InitializeParams initializeParams;
    initializeParams.workspaceFolders = std::vector<lsp::WorkspaceFolder>{{workspaceUri, "project"}};

    server.onRequest(0, "initialize", initializeParams);
    server.onNotification("initialized", std::make_optional(lsp::InitializedParams{}));

    lsp::RenameFilesParams params;
    params.files = {lsp::FileRename{workspaceUri.resolvePath("a.luau").toString(), workspaceUri.resolvePath("b.luau").toString()}};

    CHECK_NOTHROW(server.onRequest(1, "workspace/willRenameFiles", params));

    server.shutdown();
}

TEST_SUITE_END();
