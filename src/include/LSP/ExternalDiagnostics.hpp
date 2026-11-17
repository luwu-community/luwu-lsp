#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Luau/Location.h"
#include "Protocol/Diagnostics.hpp"

class TextDocument;

/// A text edit in an external diagnostic's quick fix. Its location is 0-indexed in UTF-8 bytes.
struct ExternalEdit
{
    Luau::Location location;
    std::string newText;

    bool operator==(const ExternalEdit& other) const
    {
        return location == other.location && newText == other.newText;
    }
};

/// A quick fix the editor offers for an external diagnostic
struct ExternalFix
{
    std::string title;
    std::vector<ExternalEdit> edits;

    bool operator==(const ExternalFix& other) const
    {
        return title == other.title && edits == other.edits;
    }
};

/// A diagnostic another program sent to luwu-lsp over `luwu.diagnostics.externalPort`. Its location is 0-indexed in
/// UTF-8 bytes like a Luau::Location, and becomes the editor's UTF-16 positions when it's reported.
struct ExternalDiagnostic
{
    Luau::Location location;
    lsp::DiagnosticSeverity severity = lsp::DiagnosticSeverity::Warning;
    /// `SyntaxError`, `TypeError`, `Typo` or `Lint`. It picks the badge's colour, and labels anything but a lint.
    std::string kind;
    /// The lint's name (`ZContinuationSpacing`), which labels a lint
    std::optional<std::string> code;
    std::string message;
    /// The badge's text as a string, an object with `text`, `background` and `foreground`, false for no badge, or null
    /// for the default badge
    json badge;
    /// The quick fixes the editor offers. The first is the preferred one.
    std::vector<ExternalFix> fixes;

    bool operator==(const ExternalDiagnostic& other) const
    {
        return location == other.location && severity == other.severity && kind == other.kind && code == other.code &&
               message == other.message && badge == other.badge && fixes == other.fixes;
    }
};

/// One file's diagnostics in a report
struct ExternalDiagnosticsFile
{
    /// The file as the request wrote it: a path (absolute, or relative to a workspace folder) or a `file:` URI
    std::string file;
    std::vector<ExternalDiagnostic> diagnostics;
};

/// What one request sends. Each file listed gets `source`'s diagnostics replaced by the new ones.
struct ExternalDiagnosticsReport
{
    /// The program sending them (`luwu-website`). The editor shows it next to each diagnostic.
    std::string source;
    std::vector<ExternalDiagnosticsFile> files;
    /// Whether every file the report leaves out loses `source`'s diagnostics too
    bool replaceAll = false;
};

/// Reads a request body. Throws std::invalid_argument saying what's wrong with it.
ExternalDiagnosticsReport parseExternalDiagnosticsReport(const json& body);

lsp::Diagnostic toLspDiagnostic(const ExternalDiagnostic& diagnostic, const std::string& source, const TextDocument* textDocument);

/// Takes diagnostics from other programs as JSON posted to `http://127.0.0.1:<port>/diagnostics`. It answers each
/// request on its own thread and hands every valid body to `onReport`.
class ExternalDiagnosticsServer
{
public:
    using ReportHandler = std::function<void(const json& body)>;

    /// Throws std::runtime_error when it can't listen on the port. Port 0 listens on one the system picks.
    ExternalDiagnosticsServer(uint16_t port, ReportHandler onReport);
    ~ExternalDiagnosticsServer();

    ExternalDiagnosticsServer(const ExternalDiagnosticsServer&) = delete;
    ExternalDiagnosticsServer& operator=(const ExternalDiagnosticsServer&) = delete;

    uint16_t port() const
    {
        return listenPort;
    }

private:
    void acceptLoop();
    /// The status and JSON body to answer a request with
    std::pair<int, json> respond(const std::string& request, const std::string& body) const;

    uint16_t listenPort;
    ReportHandler onReport;
    std::intptr_t listenSocket;
    std::atomic<bool> stopping{false};
    std::thread acceptThread;
};
