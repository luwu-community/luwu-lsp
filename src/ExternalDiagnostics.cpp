#include "LSP/ExternalDiagnostics.hpp"

#include "LSP/LuauExt.hpp"
#include "LSP/TextDocument.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static const SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
static const int SHUTDOWN_BOTH = SD_BOTH;
static void closeSocket(SocketHandle socket)
{
    closesocket(socket);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketHandle = int;
static const SocketHandle INVALID_SOCKET_HANDLE = -1;
static const int SHUTDOWN_BOTH = SHUT_RDWR;
static void closeSocket(SocketHandle socket)
{
    close(socket);
}
#endif

#ifdef MSG_NOSIGNAL
static const int SEND_FLAGS = MSG_NOSIGNAL;
#else
static const int SEND_FLAGS = 0;
#endif

/// The largest request body accepted, which is far more than any file's diagnostics need
static const size_t MAX_BODY_SIZE = 32 * 1024 * 1024;
static const size_t MAX_HEADER_SIZE = 64 * 1024;
/// How long a connection may take to send its request before it's dropped, so a stalled client can't block the next
static const int RECEIVE_TIMEOUT_MS = 5000;

static const char* const KINDS[] = {"SyntaxError", "TypeError", "Typo", "Lint"};

static std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c)
        {
            return char(std::tolower(c));
        });
    return text;
}

static std::string kindNamed(const std::string& name)
{
    for (const char* kind : KINDS)
    {
        if (lowercase(kind) == lowercase(name))
            return kind;
    }
    throw std::invalid_argument("'kind' must be SyntaxError, TypeError, Typo or Lint, not '" + name + "'");
}

static lsp::DiagnosticSeverity severityNamed(const std::string& name)
{
    std::string lower = lowercase(name);
    if (lower == "error")
        return lsp::DiagnosticSeverity::Error;
    if (lower == "warning")
        return lsp::DiagnosticSeverity::Warning;
    if (lower == "information" || lower == "info")
        return lsp::DiagnosticSeverity::Information;
    if (lower == "hint")
        return lsp::DiagnosticSeverity::Hint;
    throw std::invalid_argument("'severity' must be error, warning, information or hint, not '" + name + "'");
}

/// A 1-indexed line or column field as 0-indexed
static unsigned int positionField(const json& diagnostic, const char* field, std::optional<unsigned int> fallback)
{
    auto it = diagnostic.find(field);
    if (it == diagnostic.end())
    {
        if (fallback)
            return *fallback;
        throw std::invalid_argument(std::string("each diagnostic needs '") + field + "'");
    }
    if (!it->is_number_integer() || it->get<long long>() < 1)
        throw std::invalid_argument(std::string("'") + field + "' must be an integer from 1 (lines and columns start at 1)");
    return static_cast<unsigned int>(it->get<long long>() - 1);
}

static std::optional<std::string> stringField(const json& object, const char* field)
{
    auto it = object.find(field);
    if (it == object.end() || it->is_null())
        return std::nullopt;
    if (!it->is_string())
        throw std::invalid_argument(std::string("'") + field + "' must be a string");
    return it->get<std::string>();
}

static ExternalFix parseFix(const json& object)
{
    if (!object.is_object())
        throw std::invalid_argument("each fix must be an object");

    std::optional<std::string> title = stringField(object, "title");
    if (!title)
        throw std::invalid_argument("each fix needs a 'title'");

    auto edits = object.find("edits");
    if (edits == object.end() || !edits->is_array() || edits->empty())
        throw std::invalid_argument("each fix needs 'edits', a list of the changes it makes");

    ExternalFix fix{*title, {}};
    for (const json& edit : *edits)
    {
        if (!edit.is_object())
            throw std::invalid_argument("each edit must be an object");

        // An edit without an end inserts its text. One that ends on another line needs its column.
        unsigned int line = positionField(edit, "line", std::nullopt);
        unsigned int column = positionField(edit, "column", std::nullopt);
        unsigned int endLine = positionField(edit, "endLine", line);
        unsigned int endColumn = positionField(edit, "endColumn", endLine == line ? std::optional<unsigned int>(column) : std::nullopt);
        Luau::Location location{{line, column}, {endLine, endColumn}};
        if (location.end < location.begin)
            throw std::invalid_argument("an edit's end can't come before its start");

        std::optional<std::string> newText = stringField(edit, "newText");
        if (!newText)
            throw std::invalid_argument("each edit needs 'newText'");
        fix.edits.push_back({location, *newText});
    }
    return fix;
}

static ExternalDiagnostic parseDiagnostic(const json& object)
{
    if (!object.is_object())
        throw std::invalid_argument("each diagnostic must be an object");

    ExternalDiagnostic diagnostic;

    unsigned int line = positionField(object, "line", std::nullopt);
    unsigned int column = positionField(object, "column", std::nullopt);
    unsigned int endLine = positionField(object, "endLine", line);
    unsigned int endColumn = positionField(object, "endColumn", column + 1);
    diagnostic.location = Luau::Location{{line, column}, {endLine, endColumn}};
    if (diagnostic.location.end < diagnostic.location.begin)
        throw std::invalid_argument("a diagnostic's end can't come before its start");

    std::optional<std::string> message = stringField(object, "message");
    if (!message)
        throw std::invalid_argument("each diagnostic needs a 'message'");
    diagnostic.message = *message;

    diagnostic.kind = kindNamed(stringField(object, "kind").value_or("Lint"));
    diagnostic.code = stringField(object, "code");

    if (std::optional<std::string> severity = stringField(object, "severity"))
        diagnostic.severity = severityNamed(*severity);
    else
        diagnostic.severity = diagnostic.kind == "Lint" ? lsp::DiagnosticSeverity::Warning : lsp::DiagnosticSeverity::Error;

    if (auto badge = object.find("badge"); badge != object.end())
    {
        if (badge->is_object())
        {
            stringField(*badge, "text");
            stringField(*badge, "background");
            stringField(*badge, "foreground");
        }
        else if (!badge->is_null() && !badge->is_string() && *badge != json(false))
            throw std::invalid_argument("'badge' must be a string, an object with 'text', 'background' and 'foreground', or false");
        diagnostic.badge = *badge;
    }

    if (auto fixes = object.find("fixes"); fixes != object.end())
    {
        if (!fixes->is_array())
            throw std::invalid_argument("'fixes' must be a list");
        for (const json& fix : *fixes)
            diagnostic.fixes.push_back(parseFix(fix));
    }

    return diagnostic;
}

ExternalDiagnosticsReport parseExternalDiagnosticsReport(const json& body)
{
    if (!body.is_object())
        throw std::invalid_argument("the request body must be a JSON object");

    ExternalDiagnosticsReport report;

    std::optional<std::string> source = stringField(body, "source");
    if (!source || source->empty())
        throw std::invalid_argument("the request needs a 'source' naming the program that sends it");
    report.source = *source;

    if (auto replaceAll = body.find("replaceAll"); replaceAll != body.end())
    {
        if (!replaceAll->is_boolean())
            throw std::invalid_argument("'replaceAll' must be true or false");
        report.replaceAll = replaceAll->get<bool>();
    }

    auto files = body.find("files");
    // An empty table is `[]` to most JSON encoders, seal's included
    bool noFiles = files != body.end() && files->is_array() && files->empty();
    if (files == body.end() || (!files->is_object() && !noFiles))
        throw std::invalid_argument("the request needs 'files', an object from each file to its diagnostics");

    for (const auto& [file, diagnostics] : files->items())
    {
        if (!diagnostics.is_array())
            throw std::invalid_argument("the diagnostics of '" + file + "' must be an array");

        ExternalDiagnosticsFile entry{file, {}};
        for (const json& diagnostic : diagnostics)
        {
            try
            {
                entry.diagnostics.push_back(parseDiagnostic(diagnostic));
            }
            catch (const std::invalid_argument& e)
            {
                throw std::invalid_argument("in '" + file + "': " + e.what());
            }
        }
        report.files.push_back(std::move(entry));
    }

    return report;
}

lsp::Diagnostic toLspDiagnostic(const ExternalDiagnostic& diagnostic, const std::string& source, const TextDocument* textDocument)
{
    // A lint is labeled by its name, like the linter's are
    std::string label = diagnostic.kind == "Lint" ? diagnostic.code.value_or(source) : diagnostic.kind;

    lsp::Diagnostic result;
    result.source = source;
    if (diagnostic.code)
        result.code = *diagnostic.code;
    result.message = label + ": " + diagnostic.message;
    result.severity = diagnostic.severity;
    result.range = {toUTF16(textDocument, diagnostic.location.begin), toUTF16(textDocument, diagnostic.location.end)};

    json luwu = {{"kind", diagnostic.kind}, {"label", label}};
    if (!diagnostic.badge.is_null())
        luwu["badge"] = diagnostic.badge;
    result.data = json{{"luwu", luwu}};

    return result;
}

static const char* statusText(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 415:
        return "Unsupported Media Type";
    default:
        return "Error";
    }
}

static json errorBody(const std::string& message)
{
    return json{{"ok", false}, {"error", message}};
}

static void sendAll(SocketHandle socket, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        int count = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), SEND_FLAGS);
        if (count <= 0)
            return;
        sent += static_cast<size_t>(count);
    }
}

/// Reads the request line and headers, and as much of the body as arrived with them. Returns false when the
/// connection closes, times out or sends too much first.
static bool receiveHead(SocketHandle socket, std::string& head, std::string& bodyStart)
{
    std::string data;
    char buffer[4096];
    while (true)
    {
        if (size_t end = data.find("\r\n\r\n"); end != std::string::npos)
        {
            head = data.substr(0, end);
            bodyStart = data.substr(end + 4);
            return true;
        }
        if (data.size() > MAX_HEADER_SIZE)
            return false;

        int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0)
            return false;
        data.append(buffer, static_cast<size_t>(count));
    }
}

static std::optional<std::string> header(const std::string& head, const std::string& name);

/// Reads a whole request into `head` and `body`. Returns the error to answer with when it can't.
static std::optional<std::pair<int, json>> receiveRequest(SocketHandle socket, std::string& head, std::string& body)
{
    if (!receiveHead(socket, head, body))
        return std::pair<int, json>{400, errorBody("the request ended before its headers did")};

    std::optional<std::string> contentLength = header(head, "content-length");
    if (!contentLength)
    {
        if (head.compare(0, 5, "POST ") == 0)
            return std::pair<int, json>{411, errorBody("the request needs a Content-Length")};
        body.clear();
        return std::nullopt;
    }

    size_t length = 0;
    try
    {
        length = std::stoul(*contentLength);
    }
    catch (const std::exception&)
    {
        return std::pair<int, json>{400, errorBody("the request's Content-Length isn't a number")};
    }
    if (length > MAX_BODY_SIZE)
        return std::pair<int, json>{413, errorBody("the request body is larger than 32 MB")};

    char buffer[16384];
    while (body.size() < length)
    {
        int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0)
            return std::pair<int, json>{400, errorBody("the request body ended early")};
        body.append(buffer, static_cast<size_t>(count));
    }
    body.resize(length);
    return std::nullopt;
}

static std::optional<std::string> header(const std::string& head, const std::string& name)
{
    size_t lineStart = head.find("\r\n");
    while (lineStart != std::string::npos)
    {
        lineStart += 2;
        size_t lineEnd = head.find("\r\n", lineStart);
        std::string line = head.substr(lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
        size_t colon = line.find(':');
        if (colon != std::string::npos && lowercase(line.substr(0, colon)) == name)
        {
            size_t valueStart = line.find_first_not_of(" \t", colon + 1);
            return valueStart == std::string::npos ? "" : line.substr(valueStart);
        }
        lineStart = lineEnd;
    }
    return std::nullopt;
}

ExternalDiagnosticsServer::ExternalDiagnosticsServer(uint16_t port, ReportHandler onReport)
    : listenPort(port)
    , onReport(std::move(onReport))
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        throw std::runtime_error("couldn't start Windows sockets");
#endif

    SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET_HANDLE)
        throw std::runtime_error("couldn't create a socket");

    int reuse = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Only programs on this machine can reach it
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(handle, 16) != 0)
    {
        closeSocket(handle);
        throw std::runtime_error("couldn't listen on 127.0.0.1:" + std::to_string(port) + " (is another program using the port?)");
    }

    // Port 0 lets the system pick one
    socklen_t addressLength = sizeof(address);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0)
        listenPort = ntohs(address.sin_port);

    listenSocket = static_cast<std::intptr_t>(handle);
    acceptThread = std::thread(
        [this]
        {
            acceptLoop();
        });
}

ExternalDiagnosticsServer::~ExternalDiagnosticsServer()
{
    stopping = true;
    // Shutting the socket down wakes the blocked accept
    SocketHandle handle = static_cast<SocketHandle>(listenSocket);
    shutdown(handle, SHUTDOWN_BOTH);
    closeSocket(handle);
    if (acceptThread.joinable())
        acceptThread.join();
#ifdef _WIN32
    WSACleanup();
#endif
}

void ExternalDiagnosticsServer::acceptLoop()
{
    SocketHandle handle = static_cast<SocketHandle>(listenSocket);
    while (!stopping)
    {
        SocketHandle connection = accept(handle, nullptr, nullptr);
        if (connection == INVALID_SOCKET_HANDLE)
        {
            if (stopping)
                return;
            continue;
        }

#ifdef _WIN32
        DWORD timeout = RECEIVE_TIMEOUT_MS;
#else
        timeval timeout{RECEIVE_TIMEOUT_MS / 1000, (RECEIVE_TIMEOUT_MS % 1000) * 1000};
#endif
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int noSigpipe = 1;
        setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif

        std::string head;
        std::string body;
        std::optional<std::pair<int, json>> error = receiveRequest(connection, head, body);
        std::pair<int, json> response = error ? *error : respond(head, body);

        std::string payload = response.second.dump();
        std::string reply = "HTTP/1.1 " + std::to_string(response.first) + " " + statusText(response.first) +
                            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(payload.size()) +
                            "\r\nConnection: close\r\n\r\n" + payload;
        sendAll(connection, reply);

        shutdown(connection, SHUTDOWN_BOTH);
        closeSocket(connection);
    }
}

std::pair<int, json> ExternalDiagnosticsServer::respond(const std::string& head, const std::string& body) const
{
    // A web page can send requests to localhost too. Browsers always add Origin to them, and checking Host stops a
    // page from reaching the port through a domain that resolves to 127.0.0.1.
    if (header(head, "origin"))
        return {403, errorBody("requests from web pages aren't accepted")};
    std::string port = std::to_string(listenPort);
    std::string host = lowercase(header(head, "host").value_or(""));
    if (host != "127.0.0.1:" + port && host != "localhost:" + port)
        return {403, errorBody("the Host header must be 127.0.0.1:" + port + " or localhost:" + port)};

    std::string requestLine = head.substr(0, head.find("\r\n"));
    size_t methodEnd = requestLine.find(' ');
    size_t pathEnd = requestLine.find(' ', methodEnd + 1);
    if (methodEnd == std::string::npos || pathEnd == std::string::npos)
        return {400, errorBody("the request line isn't valid HTTP")};
    std::string method = requestLine.substr(0, methodEnd);
    std::string path = requestLine.substr(methodEnd + 1, pathEnd - methodEnd - 1);

    if (path != "/diagnostics")
        return {404, errorBody("post diagnostics to /diagnostics")};
    if (method != "POST")
        return {405, errorBody("send diagnostics with POST")};

    std::string contentType = lowercase(header(head, "content-type").value_or(""));
    if (contentType.compare(0, 16, "application/json") != 0)
        return {415, errorBody("the request body must be JSON, sent with Content-Type: application/json")};

    json parsed = json::parse(body, nullptr, /* allow_exceptions= */ false);
    if (parsed.is_discarded())
        return {400, errorBody("the request body isn't valid JSON")};

    try
    {
        parseExternalDiagnosticsReport(parsed);
    }
    catch (const std::invalid_argument& e)
    {
        return {400, errorBody(e.what())};
    }

    onReport(parsed);
    return {200, json{{"ok", true}}};
}
