#include <optional>
#include <deque>
#include <condition_variable>

#include "LSP/JsonRpc.hpp"
#include "nlohmann/json.hpp"

#include "Protocol/Structures.hpp"
#include "Protocol/LanguageFeatures.hpp"

#include "LSP/Client.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/ExternalDiagnostics.hpp"

#include "Thread.hpp"

using json = nlohmann::json;
using namespace json_rpc;
using WorkspaceFolderPtr = std::shared_ptr<WorkspaceFolder>;

#define JSON_REQUIRED_PARAMS(params, method) \
    (!(params) ? throw json_rpc::JsonRpcException(lsp::ErrorCode::InvalidParams, "params not provided for " method) : (params).value())

inline lsp::PositionEncodingKind& positionEncoding()
{
    static lsp::PositionEncodingKind encoding = lsp::PositionEncodingKind::UTF16;
    return encoding;
}

struct InitializationOptions
{
    std::unordered_map<std::string, std::string> fflags{};
};
NLOHMANN_DEFINE_OPTIONAL(InitializationOptions, fflags)

class LanguageServer
{
private:
    // Client is guaranteed to live for the duration of the whole program
    LSPClient* client;
    std::optional<Luau::Config> defaultConfig;
    // A "in memory" workspace folder which doesn't actually have a root.
    // Any files which aren't part of a workspace but are opened will be handled here.
    // This is common if the client has not yet opened a folder
    WorkspaceFolderPtr nullWorkspace;
    std::vector<WorkspaceFolderPtr> workspaceFolders;

    std::vector<json_rpc::JsonRpcMessage> configPostponedMessages;

public:
    explicit LanguageServer(LSPClient* aClient, std::optional<Luau::Config> aDefaultConfig);

    static lsp::ServerCapabilities getServerCapabilities();

    /// Finds the workspace which the file belongs to.
    /// If no workspace is found, the file is attached to the null workspace
    WorkspaceFolderPtr findWorkspace(const lsp::DocumentUri& file, bool shouldInitialize = true);

    void onRequest(const id_type& id, const std::string& method, std::optional<json> params);
    void onNotification(const std::string& method, std::optional<json> params);
    void processInputLoop();
    bool requestedShutdown();

    // Visible for testing only
    void handleMessage(const json_rpc::JsonRpcMessage& msg);

    // Visible for testing only
    void shutdown();

    /// Queues a notification for the message thread as if the client had sent it
    void postNotification(const std::string& method, const json& params);

    // Visible for testing only
    void onExternalDiagnostics(const json& params);

    // Dispatch handlers
private:
    bool allWorkspacesReceivedConfiguration() const;
    void clearCancellationToken(const json_rpc::JsonRpcMessage& msg);
    std::optional<json_rpc::JsonRpcMessage> popMessage();
    /// Stops the message processor after its current request, or exits the server when that request runs past SHUTDOWN_TIMEOUT
    void stopMessageProcessor();

    lsp::InitializeResult onInitialize(const lsp::InitializeParams& params);
    void onInitialized([[maybe_unused]] const lsp::InitializedParams& params);

    void onDidOpenTextDocument(const lsp::DidOpenTextDocumentParams& params);
    void onDidChangeTextDocument(const lsp::DidChangeTextDocumentParams& params);
    void onDidSaveTextDocument(const lsp::DidSaveTextDocumentParams& params);
    void onDidCloseTextDocument(const lsp::DidCloseTextDocumentParams& params);
    void onDidChangeConfiguration(const lsp::DidChangeConfigurationParams& params);
    void onDidChangeWorkspaceFolders(const lsp::DidChangeWorkspaceFoldersParams& params);
    void onDidChangeWatchedFiles(const lsp::DidChangeWatchedFilesParams& params);

    /// Starts, moves or stops the external diagnostics server to match the workspaces' `diagnostics.externalPort`
    void updateExternalDiagnosticsServer();
    /// The file an external diagnostics report names: a `file:` URI, an absolute path, or a path relative to the first
    /// workspace folder that has it
    Uri resolveExternalFile(const std::string& file) const;

    std::vector<lsp::DocumentLink> documentLink(const lsp::DocumentLinkParams& params);
    std::optional<lsp::WorkspaceEdit> willRenameFiles(const lsp::RenameFilesParams& params);
    lsp::DocumentColorResult documentColor(const lsp::DocumentColorParams& params);
    lsp::ColorPresentationResult colorPresentation(const lsp::ColorPresentationParams& params);

    std::optional<std::vector<lsp::DocumentSymbol>> documentSymbol(const lsp::DocumentSymbolParams& params);
    lsp::PartialResponse<lsp::WorkspaceDiagnosticReport> workspaceDiagnostic(const lsp::WorkspaceDiagnosticParams& params);

private:
    bool isInitialized = false;
    bool shutdownRequested = false;
    /// Set when stdin closes without a shutdown request
    bool inputClosed = false;

    std::mutex messagesMutex;
    std::condition_variable messagesCv;
    std::deque<json_rpc::JsonRpcMessage> messages;
    Thread messageProcessorThread;
    /// Set under `messagesMutex` once the message processor has handled its last message
    bool messageProcessorStopped = false;
    std::condition_variable messageProcessorStoppedCv;
    std::unordered_map<id_type, LSPCancellationToken> cancellationTokens;

    // Declared last so it's destroyed first: its thread posts to the message queue above
    std::unique_ptr<ExternalDiagnosticsServer> externalDiagnosticsServer;
};
