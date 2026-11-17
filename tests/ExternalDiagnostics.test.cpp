#include "doctest.h"
#include "LSP/ExternalDiagnostics.hpp"
#include "LSP/LanguageServer.hpp"
#include "TempDir.h"
#include "TestClient.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <stdexcept>

TEST_SUITE_BEGIN("ExternalDiagnostics");

static json zDiagnostic()
{
    return json{
        {"line", 3},
        {"column", 10},
        {"endColumn", 12},
        {"message", "add a space before \\z"},
        {"code", "ZContinuationSpacing"},
        {"badge", {{"text", "\\z"}, {"background", "#2E0A40"}}},
    };
}

TEST_CASE("external_diagnostics_are_1_indexed_and_labeled_by_lint_name")
{
    ExternalDiagnosticsReport report = parseExternalDiagnosticsReport(json{
        {"source", "luwu-website"},
        {"files", {{"src/home.luwu", json::array({zDiagnostic()})}}},
    });

    REQUIRE_EQ(report.files.size(), 1);
    REQUIRE_EQ(report.files[0].diagnostics.size(), 1);
    const ExternalDiagnostic& diagnostic = report.files[0].diagnostics[0];
    CHECK_EQ(diagnostic.location.begin.line, 2);
    CHECK_EQ(diagnostic.location.begin.column, 9);
    CHECK_EQ(diagnostic.location.end.column, 11);
    CHECK_EQ(diagnostic.kind, "Lint");
    CHECK_EQ(diagnostic.severity, lsp::DiagnosticSeverity::Warning);

    lsp::Diagnostic converted = toLspDiagnostic(diagnostic, report.source, nullptr);
    CHECK_EQ(converted.message, "ZContinuationSpacing: add a space before \\z");
    CHECK_EQ(converted.source, "luwu-website");
    CHECK_EQ(converted.range.start, lsp::Position{2, 9});
    REQUIRE(converted.data);
    CHECK_EQ((*converted.data)["luwu"]["kind"], "Lint");
    CHECK_EQ((*converted.data)["luwu"]["badge"]["text"], "\\z");
}

TEST_CASE("external_diagnostics_of_an_error_kind_are_errors_labeled_by_kind")
{
    json diagnostic = zDiagnostic();
    diagnostic["kind"] = "typeerror";
    ExternalDiagnosticsReport report = parseExternalDiagnosticsReport(json{
        {"source", "checker"},
        {"files", {{"a.luwu", json::array({diagnostic})}}},
    });

    const ExternalDiagnostic& parsed = report.files[0].diagnostics[0];
    CHECK_EQ(parsed.kind, "TypeError");
    CHECK_EQ(parsed.severity, lsp::DiagnosticSeverity::Error);
    CHECK_EQ(toLspDiagnostic(parsed, report.source, nullptr).message, "TypeError: add a space before \\z");
}

TEST_CASE("external_diagnostics_reports_that_are_malformed_are_rejected")
{
    auto reportWith = [](const json& diagnostic)
    {
        return json{{"source", "s"}, {"files", {{"a.luwu", json::array({diagnostic})}}}};
    };

    CHECK_THROWS_AS(parseExternalDiagnosticsReport(json{{"files", json::object()}}), std::invalid_argument);
    CHECK_THROWS_AS(parseExternalDiagnosticsReport(json{{"source", "s"}}), std::invalid_argument);
    // An empty table encoded as an array
    CHECK_NOTHROW(parseExternalDiagnosticsReport(json{{"source", "s"}, {"files", json::array()}, {"replaceAll", true}}));

    json zeroLine = zDiagnostic();
    zeroLine["line"] = 0;
    CHECK_THROWS_AS(parseExternalDiagnosticsReport(reportWith(zeroLine)), std::invalid_argument);

    json backwards = zDiagnostic();
    backwards["endColumn"] = 1;
    CHECK_THROWS_AS(parseExternalDiagnosticsReport(reportWith(backwards)), std::invalid_argument);

    json unknownKind = zDiagnostic();
    unknownKind["kind"] = "Warning";
    CHECK_THROWS_AS(parseExternalDiagnosticsReport(reportWith(unknownKind)), std::invalid_argument);

    json noMessage = zDiagnostic();
    noMessage.erase("message");
    CHECK_THROWS_AS(parseExternalDiagnosticsReport(reportWith(noMessage)), std::invalid_argument);
}

#ifndef _WIN32
/// Sends `request` to the server and returns the status code it answers with
static int post(uint16_t port, const std::string& request)
{
    int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    send(socket, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = recv(socket, buffer, sizeof(buffer), 0)) > 0)
        response.append(buffer, static_cast<size_t>(count));
    close(socket);

    REQUIRE(response.compare(0, 9, "HTTP/1.1 ") == 0);
    return std::stoi(response.substr(9, 3));
}

static std::string request(uint16_t port, const std::string& body, const std::string& extraHeaders = "",
    const std::string& contentType = "application/json")
{
    return "POST /diagnostics HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: " + contentType +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
}

TEST_CASE("external_diagnostics_server_accepts_only_valid_json_from_local_programs")
{
    std::vector<json> reports;
    ExternalDiagnosticsServer server(0,
        [&](const json& body)
        {
            reports.push_back(body);
        });
    uint16_t port = server.port();
    REQUIRE_NE(port, 0);

    std::string valid = json{{"source", "s"}, {"files", {{"a.luwu", json::array({zDiagnostic()})}}}}.dump();

    CHECK_EQ(post(port, request(port, valid)), 200);
    CHECK_EQ(post(port, request(port, valid, "Origin: https://example.com\r\n")), 403);
    CHECK_EQ(post(port, request(port, valid, "", "text/plain")), 415);
    CHECK_EQ(post(port, request(port, "{ not json")), 400);
    CHECK_EQ(post(port, request(port, json{{"source", "s"}}.dump())), 400);

    // Only the first request reached the handler
    REQUIRE_EQ(reports.size(), 1);
    CHECK_EQ(reports[0]["source"], "s");
}
#endif

TEST_CASE("external_diagnostics_are_published_with_the_file_and_replaced_by_the_next_report")
{
    TestClient client;
    client.globalConfig.index.enabled = false;
    LanguageServer server(&client, std::nullopt);

    TempDir dir("external_diagnostics");
    std::string file = dir.write_child("home.luwu", "print(\"a\\z\nb\")\n");
    Uri workspaceUri = Uri::file(dir.path());

    lsp::InitializeParams initializeParams;
    initializeParams.workspaceFolders = std::vector<lsp::WorkspaceFolder>{{workspaceUri, "project"}};
    server.onRequest(0, "initialize", initializeParams);
    server.onNotification("initialized", std::make_optional(lsp::InitializedParams{}));

    Uri uri = Uri::file(file);
    lsp::DidOpenTextDocumentParams openParams;
    openParams.textDocument = {uri, "luau", 0, "print(\"a\\z\nb\")\n"};
    server.onNotification("textDocument/didOpen", std::make_optional(openParams));

    auto lastPublished = [&]() -> std::vector<lsp::Diagnostic>
    {
        for (auto it = client.notificationQueue.rbegin(); it != client.notificationQueue.rend(); ++it)
        {
            if (it->first == "textDocument/publishDiagnostics")
                return it->second->get<lsp::PublishDiagnosticsParams>().diagnostics;
        }
        FAIL("nothing was published");
        return {};
    };

    json diagnostic = {{"line", 1}, {"column", 9}, {"endColumn", 11}, {"message", "add a space before \\z"}};
    // A path relative to the workspace folder
    server.onExternalDiagnostics(json{{"source", "website"}, {"files", {{"home.luwu", json::array({diagnostic})}}}});

    std::vector<lsp::Diagnostic> published = lastPublished();
    auto external = std::find_if(published.begin(), published.end(),
        [](const lsp::Diagnostic& d)
        {
            return d.source == "website";
        });
    REQUIRE(external != published.end());
    CHECK_EQ(external->message, "website: add a space before \\z");
    CHECK_EQ(external->range.start, lsp::Position{0, 8});

    // Sending the same diagnostics again publishes nothing
    size_t notifications = client.notificationQueue.size();
    server.onExternalDiagnostics(json{{"source", "website"}, {"files", {{"home.luwu", json::array({diagnostic})}}}});
    CHECK_EQ(client.notificationQueue.size(), notifications);

    // A later report replaces the source's diagnostics for the file
    server.onExternalDiagnostics(json{{"source", "website"}, {"files", {{uri.toString(), json::array()}}}});
    published = lastPublished();
    CHECK(std::none_of(published.begin(), published.end(),
        [](const lsp::Diagnostic& d)
        {
            return d.source == "website";
        }));

    server.shutdown();
}

TEST_CASE("external_diagnostics_offer_their_fixes_as_quick_fixes")
{
    TestClient client;
    client.globalConfig.index.enabled = false;
    LanguageServer server(&client, std::nullopt);

    TempDir dir("external_diagnostics_fixes");
    std::string source = "print(\"a\\z\nb\")\n";
    std::string file = dir.write_child("home.luwu", source);
    Uri workspaceUri = Uri::file(dir.path());

    lsp::InitializeParams initializeParams;
    initializeParams.workspaceFolders = std::vector<lsp::WorkspaceFolder>{{workspaceUri, "project"}};
    server.onRequest(0, "initialize", initializeParams);
    server.onNotification("initialized", std::make_optional(lsp::InitializedParams{}));

    Uri uri = Uri::file(file);
    lsp::DidOpenTextDocumentParams openParams;
    openParams.textDocument = {uri, "luau", 0, source};
    server.onNotification("textDocument/didOpen", std::make_optional(openParams));

    json fix = {{"title", "Add a space before \\z"}, {"edits", json::array({{{"line", 1}, {"column", 9}, {"newText", " "}}})}};
    json diagnostic = {{"line", 1}, {"column", 9}, {"endColumn", 11}, {"message", "add a space"}, {"fixes", json::array({fix})}};
    server.onExternalDiagnostics(json{{"source", "website"}, {"files", {{"home.luwu", json::array({diagnostic})}}}});

    lsp::CodeActionParams params;
    params.textDocument = {uri};
    params.range = {{0, 9}, {0, 9}};
    auto workspace = server.findWorkspace(uri);
    std::vector<lsp::CodeAction> actions = workspace->codeAction(params, nullptr).value_or(std::vector<lsp::CodeAction>{});

    auto action = std::find_if(actions.begin(), actions.end(),
        [](const lsp::CodeAction& a)
        {
            return a.title == "Add a space before \\z";
        });
    REQUIRE(action != actions.end());
    CHECK(action->isPreferred);
    REQUIRE(action->edit);
    const std::vector<lsp::TextEdit>& edits = action->edit->changes.at(uri);
    REQUIRE_EQ(edits.size(), 1);
    CHECK_EQ(edits[0].range.start, lsp::Position{0, 8});
    CHECK_EQ(edits[0].range.end, lsp::Position{0, 8});
    CHECK_EQ(edits[0].newText, " ");

    server.shutdown();
}

TEST_CASE("external_diagnostics_with_the_same_code_get_a_fix_all")
{
    TestClient client;
    client.globalConfig.index.enabled = false;
    LanguageServer server(&client, std::nullopt);

    TempDir dir("external_diagnostics_fix_all");
    std::string source = "print(\"a\\z\nb\", \"c\\z\nd\")\n";
    std::string file = dir.write_child("home.luwu", source);
    lsp::InitializeParams initializeParams;
    initializeParams.workspaceFolders = std::vector<lsp::WorkspaceFolder>{{Uri::file(dir.path()), "project"}};
    server.onRequest(0, "initialize", initializeParams);
    server.onNotification("initialized", std::make_optional(lsp::InitializedParams{}));

    Uri uri = Uri::file(file);
    lsp::DidOpenTextDocumentParams openParams;
    openParams.textDocument = {uri, "luau", 0, source};
    server.onNotification("textDocument/didOpen", std::make_optional(openParams));

    auto spaceBefore = [](int line, int column)
    {
        json fix = {{"title", "Add a space"}, {"edits", json::array({{{"line", line}, {"column", column}, {"newText", " "}}})}};
        return json{{"line", line}, {"column", column}, {"endColumn", column + 2}, {"code", "ZJoinsWords"}, {"message", "add a space"},
            {"fixes", json::array({fix})}};
    };
    server.onExternalDiagnostics(json{{"source", "website"}, {"files", {{"home.luwu", json::array({spaceBefore(1, 9), spaceBefore(2, 7)})}}}});

    lsp::CodeActionParams params;
    params.textDocument = {uri};
    params.range = {{0, 9}, {0, 9}};
    std::vector<lsp::CodeAction> actions = server.findWorkspace(uri)->codeAction(params, nullptr).value_or(std::vector<lsp::CodeAction>{});

    auto fixAll = std::find_if(actions.begin(), actions.end(),
        [](const lsp::CodeAction& a)
        {
            return a.title == "Fix all 'ZJoinsWords' warnings in this file";
        });
    REQUIRE(fixAll != actions.end());
    CHECK_FALSE(fixAll->isPreferred);
    CHECK_EQ(fixAll->diagnostics.size(), 2);
    CHECK_EQ(fixAll->edit->changes.at(uri).size(), 2);

    server.shutdown();
}
