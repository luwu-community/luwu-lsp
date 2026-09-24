#include "LSP/Workspace.hpp"
#include "LSP/LanguageServer.hpp"
#include "LSP/LuauExt.hpp"

#include <set>

/// A `require("./some/path")` call: the string that has to be rewritten when either end of it moves,
/// and the expression the module resolver needs to work out what it currently points at.
struct StringRequire
{
    Luau::AstExprConstantString* literal = nullptr;
    Luau::AstExpr* argument = nullptr;
};

struct FindStringRequireVisitor : public Luau::AstVisitor
{
    std::vector<StringRequire> stringRequires{};

    bool visit(Luau::AstExprCall* call) override
    {
        if (auto argument = types::matchRequire(*call))
            if (auto literal = (*argument)->as<Luau::AstExprConstantString>())
                stringRequires.emplace_back(StringRequire{literal, *argument});

        return true;
    }
};

static bool endsWith(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool isRelativeRequire(const std::string& text)
{
    return text.rfind("./", 0) == 0 || text.rfind("../", 0) == 0;
}

/// Only paths written relative to the requiring file can be rewritten by moving a file around.
/// An alias (`@lib/foo`), a Roblox instance path, or anything that isn't a plain string literal is
/// left alone: either the rename doesn't change what it resolves to, or working that out means
/// rewriting the `.luaurc` or the sourcemap instead.
static std::optional<std::string> rewriteRelativeRequire(const Uri& fromFile, const Uri& target, const std::string& original)
{
    if (!isRelativeRequire(original))
        return std::nullopt;

    auto directory = fromFile.parent();
    if (!directory)
        return std::nullopt;

    auto path = target.lexicallyRelative(*directory);
    if (path.empty())
        return std::nullopt;

    // A require keeps the shape it was written in: if it spelled out the file extension, so does
    // the rewritten one.
    auto extension = target.extension();
    if (!extension.empty() && !endsWith(original, extension) && endsWith(path, extension))
        path = path.substr(0, path.size() - extension.size());

    if (path.rfind("..", 0) != 0)
        path = "./" + path;

    return path;
}

static bool isSourceFile(const Uri& uri)
{
    auto extension = uri.extension();
    return extension == ".luau" || extension == ".luwu" || extension == ".lua";
}

std::optional<lsp::WorkspaceEdit> WorkspaceFolder::willRenameFiles(const lsp::RenameFilesParams& params)
{
    if (!platform)
        return std::nullopt;

    std::unordered_map<Uri, Uri, UriHash> renames{};
    std::set<Luau::ModuleName> renamedModules{};

    for (const auto& file : params.files)
    {
        auto oldUri = Uri::parse(file.oldUri);
        auto newUri = Uri::parse(file.newUri);

        // We register for file renames only, but a client is free to send us whatever it likes, and
        // a file moving to or from a non-source extension isn't a move we can fix up requires for.
        if (!isSourceFile(oldUri) || !isSourceFile(newUri) || oldUri.isDirectory())
            continue;

        renames.emplace(oldUri, newUri);
        renamedModules.emplace(fileResolver.getModuleName(oldUri));
    }

    if (renames.empty())
        return std::nullopt;

    // The files that need rewriting are the ones that moved -- their own relative requires are now
    // resolved from somewhere else -- plus everything that requires one of them.
    std::set<Luau::ModuleName> modules = renamedModules;
    for (const auto& [moduleName, sourceNode] : frontend.sourceNodes)
        for (const auto& [requiredModuleName, _] : sourceNode->requireLocations)
            if (renamedModules.count(requiredModuleName) > 0)
                modules.emplace(moduleName);

    lsp::WorkspaceEdit result{};

    for (const auto& moduleName : modules)
    {
        frontend.parse(moduleName);

        auto sourceModule = frontend.getSourceModule(moduleName);
        if (!sourceModule || !sourceModule->root)
            continue;

        auto textDocument = fileResolver.getOrCreateTextDocumentFromModuleName(moduleName);
        if (!textDocument)
            continue;

        auto fileUri = fileResolver.getUri(moduleName);
        auto renamedFile = renames.find(fileUri);
        auto newFileUri = renamedFile != renames.end() ? renamedFile->second : fileUri;

        FindStringRequireVisitor visitor;
        sourceModule->root->visit(&visitor);

        std::vector<lsp::TextEdit> edits{};

        for (const auto& require : visitor.stringRequires)
        {
            if (require.literal->quoteStyle != Luau::AstExprConstantString::QuoteStyle::QuotedSimple &&
                require.literal->quoteStyle != Luau::AstExprConstantString::QuoteStyle::QuotedSingle)
                continue;

            auto location = require.literal->location;
            if (location.begin.line != location.end.line)
                continue;

            auto moduleInfo = frontend.moduleResolver.resolveModuleInfo(moduleName, *require.argument);
            if (!moduleInfo)
                continue;

            auto target = platform->resolveToRealPath(moduleInfo->name);
            if (!target)
                continue;

            auto renamedTarget = renames.find(*target);
            auto newTarget = renamedTarget != renames.end() ? renamedTarget->second : *target;

            // Neither end of this require moved, so it still resolves to the same file
            if (newFileUri == fileUri && newTarget == *target)
                continue;

            std::string original(require.literal->value.data, require.literal->value.size);
            auto replacement = rewriteRelativeRequire(newFileUri, newTarget, original);
            if (!replacement || *replacement == original)
                continue;

            // The edit replaces what is between the quotes, so it doesn't have to care which kind
            // of quote was used
            edits.emplace_back(lsp::TextEdit{{textDocument->convertPosition(Luau::Position{location.begin.line, location.begin.column + 1}),
                                                 textDocument->convertPosition(Luau::Position{location.end.line, location.end.column - 1})},
                *replacement});
        }

        if (!edits.empty())
            result.changes.emplace(fileUri, edits);
    }

    if (result.changes.empty())
        return std::nullopt;

    return result;
}

std::optional<lsp::WorkspaceEdit> LanguageServer::willRenameFiles(const lsp::RenameFilesParams& params)
{
    // A rename can touch several workspaces at once, so ask each of them and merge what comes back
    lsp::WorkspaceEdit result{};

    for (auto& workspace : workspaceFolders)
    {
        // A workspace folder is only set up when something in it is first opened, and an
        // uninitialized one has nothing indexed to fix up anyway
        if (!workspace->isReady)
            continue;

        if (auto edit = workspace->willRenameFiles(params))
            for (auto& [uri, edits] : edit->changes)
                result.changes.emplace(uri, std::move(edits));
    }

    if (result.changes.empty())
        return std::nullopt;

    return result;
}
