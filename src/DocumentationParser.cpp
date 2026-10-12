#include "LSP/DocumentationParser.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/LuauExt.hpp"
#include "LuauFileUtils.hpp"
#include <regex>
#include <unordered_set>
#include <algorithm>

Luau::FunctionParameterDocumentation parseDocumentationParameter(const json& j)
{
    std::string name;
    std::string documentation;
    if (j.contains("name"))
        j.at("name").get_to(name);
    if (j.contains("documentation"))
        j.at("documentation").get_to(documentation);
    return Luau::FunctionParameterDocumentation{name, documentation};
}

void parseDocumentationContents(std::string_view contents, const std::string& sourceName, Luau::DocumentationDatabase& database, const Client* client)
{
    try
    {
        auto docs = json::parse(contents);
        for (auto& [symbol, info] : docs.items())
        {
            std::string documentation;
            std::string learnMoreLink;
            std::string codeSample;
            if (info.contains("documentation"))
                info.at("documentation").get_to(documentation);
            if (info.contains("learn_more_link"))
                info.at("learn_more_link").get_to(learnMoreLink);
            if (info.contains("code_sample"))
                info.at("code_sample").get_to(codeSample);
            if (info.contains("keys"))
            {
                Luau::DenseHashMap<std::string, Luau::DocumentationSymbol> keys{""};
                for (auto& [k, v] : info.at("keys").items())
                {
                    keys[k] = v;
                }
                database[symbol] = Luau::TableDocumentation{documentation, keys, learnMoreLink, codeSample};
            }
            else if (info.contains("overloads"))
            {
                Luau::DenseHashMap<std::string, Luau::DocumentationSymbol> overloads{""};
                for (auto& [sig, sym] : info.at("overloads").items())
                {
                    overloads[sig] = sym;
                }
                database[symbol] = Luau::OverloadedFunctionDocumentation{overloads};
            }
            else if (info.contains("params") || info.contains("returns"))
            {
                std::vector<Luau::FunctionParameterDocumentation> parameters;
                std::vector<std::string> returns;
                for (auto& param : info.at("params"))
                {
                    parameters.push_back(parseDocumentationParameter(param));
                }
                if (info.contains("returns"))
                    info.at("returns").get_to(returns);
                database[symbol] = Luau::FunctionDocumentation{documentation, parameters, returns, learnMoreLink, codeSample};
            }
            else
            {
                database[symbol] = Luau::BasicDocumentation{documentation, learnMoreLink, codeSample};
            }
        }
    }
    catch (const std::exception& e)
    {
        client->sendLogMessage(
            lsp::MessageType::Error, "Failed to load documentation database for " + sourceName + ": " + std::string(e.what()));
        client->sendWindowMessage(lsp::MessageType::Error, "Failed to load documentation database: " + std::string(e.what()));
    }
}

void parseDocumentation(const std::vector<std::string>& documentationFiles, Luau::DocumentationDatabase& database, const Client* client)
{
    if (documentationFiles.empty())
    {
        client->sendLogMessage(lsp::MessageType::Warning, "No documentation file given. Documentation will not be provided");
        return;
    }

    for (auto& documentationFile : documentationFiles)
    {
        auto resolvedFilePath = resolvePath(documentationFile);
        if (auto contents = Luau::FileUtils::readFile(resolvedFilePath))
        {
            parseDocumentationContents(*contents, resolvedFilePath, database, client);
        }
        else
        {
            client->sendLogMessage(
                lsp::MessageType::Error, "Failed to read documentation file for " + resolvedFilePath + ". Documentation will not be provided");
            client->sendWindowMessage(lsp::MessageType::Error, "Failed to read documentation file. Documentation will not be provided");
        }
    }
}

/// Returns a markdown string of the provided documentation
/// If we can't find any documentation for the given symbol, then we return nullopt
std::optional<std::string> printDocumentation(const Luau::DocumentationDatabase& database, const Luau::DocumentationSymbol& symbol)
{
    if (auto documentation = database.find(symbol))
    {
        std::string result;
        if (auto* basic = documentation->get_if<Luau::BasicDocumentation>())
        {
            result = basic->documentation;
            if (!basic->learnMoreLink.empty())
                result += "\n\n[Learn More](" + basic->learnMoreLink + ")";
            if (!basic->codeSample.empty())
                result += "\n\n" + codeBlock(codeBlockLanguage(), basic->codeSample);
        }
        else if (auto* func = documentation->get_if<Luau::FunctionDocumentation>())
        {
            result = func->documentation;
            if (!func->learnMoreLink.empty())
                result += "\n\n[Learn More](" + func->learnMoreLink + ")";
            if (!func->codeSample.empty())
                result += "\n\n" + codeBlock(codeBlockLanguage(), func->codeSample);
        }
        else if (auto* overloaded = documentation->get_if<Luau::OverloadedFunctionDocumentation>())
        {
            if (overloaded->overloads.size() > 0)
            {
                // Use the first overload
                if (auto firstOverloadDocs = printDocumentation(database, overloaded->overloads.begin()->second))
                    result = *firstOverloadDocs;

                auto remainingOverloads = overloaded->overloads.size() - 1;
                result += "\n\n*+" + std::to_string(remainingOverloads) + " overload" + (remainingOverloads == 1 ? "*" : "s*");
            }
        }
        else if (auto* tbl = documentation->get_if<Luau::TableDocumentation>())
        {
            result = tbl->documentation;
            if (!tbl->learnMoreLink.empty())
                result += "\n\n[Learn More](" + tbl->learnMoreLink + ")";
            if (!tbl->codeSample.empty())
                result += "\n\n" + codeBlock(codeBlockLanguage(), tbl->codeSample);
        }
        return result;
    }

    return std::nullopt;
}

std::string printMoonwaveDocumentation(const std::vector<std::string>& comments)
{
    if (comments.empty())
        return "";

    std::string result;
    std::vector<std::string> fields{};
    std::vector<std::string> params{};
    std::vector<std::string> returns{};
    std::vector<std::string> throws{};

    for (auto& comment : comments)
    {
        if (Luau::startsWith(comment, "@param "))
            params.emplace_back(comment);
        else if (Luau::startsWith(comment, "@return "))
            returns.emplace_back(comment);
        else if (Luau::startsWith(comment, "@error "))
            throws.emplace_back(comment);
        else if (Luau::startsWith(comment, "@field "))
            fields.emplace_back(comment);
        else if (comment == "@private")
            result += "**Private**\n";
        else if (comment == "@yields")
            result += "**Yields**\n";
        else if (comment == "@unreleased")
            result += "**Unreleased**\n";
        else if (comment == "@server")
            result += "**Server**\n";
        else if (comment == "@client")
            result += "**Client**\n";
        else if (comment == "@plugin")
            result += "**Plugin**\n";
        else if (comment == "@readonly")
            result += "**Read Only**\n";
        else if (Luau::startsWith(comment, "@deprecated "))
        {
            result += "**Deprecated** ";

            auto description = comment.substr(12);
            auto version = description;

            if (auto space = description.find(' '); space != std::string::npos)
            {
                version = description.substr(0, space);
                description = description.substr(space);
            }

            if (version == description)
                result += "`" + version + "`" + "\n";
            else
                result += "`" + version + "`" + description + "\n";
        }
        else if (Luau::startsWith(comment, "@since "))
            result += "**Since** `" + comment.substr(7) + "`\n";
        else if (comment == "@ignore" || Luau::startsWith(comment, "@tag ") || Luau::startsWith(comment, "@within ") ||
                 Luau::startsWith(comment, "@class ") || Luau::startsWith(comment, "@function ") || Luau::startsWith(comment, "@method ") ||
                 Luau::startsWith(comment, "@prop ") || Luau::startsWith(comment, "@interface ") || Luau::startsWith(comment, "@type ") ||
                 Luau::startsWith(comment, "@__index ") || Luau::startsWith(comment, "@external "))
            // Ignore
            continue;
        else
            result += comment + "\n";
    }

    if (!fields.empty())
    {
        result += "\n\n**Fields**\n";
        for (auto& field : fields)
        {
            auto fieldText = field.substr(7);

            // Parse name
            auto fieldName = fieldText;
            if (auto space = fieldText.find(' '); space != std::string::npos)
            {
                fieldName = fieldText.substr(0, space);
                fieldText = fieldText.substr(space);
            }

            if (fieldText == fieldName)
                result += "\n- `" + fieldName + "`";
            else
                result += "\n- `" + fieldName + "`" + fieldText;
        }
    }

    if (!params.empty())
    {
        result += "\n\n**Parameters**\n";
        for (auto& param : params)
        {
            auto paramText = param.substr(7);

            // Parse name
            auto paramName = paramText;
            if (auto space = paramText.find(' '); space != std::string::npos)
            {
                paramName = paramText.substr(0, space);
                paramText = paramText.substr(space);
            }

            if (paramText == paramName)
                result += "\n- `" + paramName + "`";
            else
                result += "\n- `" + paramName + "`" + paramText;
        }
    }

    if (!returns.empty())
    {
        result += "\n\n**Returns**\n";
        for (auto& ret : returns)
        {
            auto returnText = ret.substr(8);

            // Parse return type
            auto retType = returnText;
            if (auto delim = returnText.find(" --"); delim != std::string::npos)
            {
                retType = returnText.substr(0, delim);
                returnText = returnText.substr(delim);
            }

            if (!retType.empty() && retType != returnText)
                result += "\n- `" + retType + "`" + returnText;
            else
                result += "\n- " + returnText;
        }
    }

    if (!throws.empty())
    {
        result += "\n\n**Throws**\n";
        for (auto& thr : throws)
        {
            auto throwText = thr.substr(7);

            // Parse throw type
            auto throwType = throwText;
            if (auto delim = throwText.find(" --"); delim != std::string::npos)
            {
                throwType = throwText.substr(0, delim);
                throwText = throwText.substr(delim);
            }

            if (!throwType.empty() && throwType != throwText)
                result += "\n- `" + throwType + "`" + throwText;
            else
                result += "\n- " + throwText;
        }
    }

    return result;
}

struct AttachCommentsVisitor : public Luau::AstVisitor
{
    Luau::Position pos;
    std::vector<Luau::Comment> moduleComments; // A list of all comments in the module
    Luau::Position closestPreviousNode{0, 0};

    explicit AttachCommentsVisitor(const Luau::Location node, std::vector<Luau::Comment> moduleComments)
        : pos(node.begin)
        , moduleComments(std::move(moduleComments))
    {
    }

    std::vector<Luau::Comment> attachComments()
    {
        std::vector<Luau::Comment> candidates{};

        // Gather candidate comments (between previous node and target)
        for (auto& comment : moduleComments)
            // The comment needs to be present before the node for it to be attached
            if (comment.location.begin <= pos)
                // They should be after the closest previous node
                if (comment.location.begin >= closestPreviousNode)
                    candidates.emplace_back(comment);

        if (candidates.empty())
            return {};

        // Sort by end position descending (closest to target first)
        std::sort(candidates.begin(), candidates.end(),
            [](const Luau::Comment& a, const Luau::Comment& b)
            {
                return a.location.end > b.location.end;
            });

        // Filter to only adjacent comments
        // A comment is adjacent if there's no blank line between it and the target
        std::vector<Luau::Comment> result{};
        unsigned int adjacentLine = pos.line;

        for (const auto& comment : candidates)
        {
            if (comment.location.end.line + 1 >= adjacentLine)
            {
                result.emplace_back(comment);
                adjacentLine = comment.location.begin.line;
            }
            else
                break;
        }

        // Reverse to restore chronological order
        std::reverse(result.begin(), result.end());
        return result;
    }

    bool visit(Luau::AstExprTable* tbl) override
    {
        if (tbl->location.begin >= pos)
            return false;
        if (tbl->location.begin > closestPreviousNode)
            closestPreviousNode = tbl->location.begin;

        for (Luau::AstExprTable::Item item : tbl->items)
        {
            if (item.value->location.begin >= pos)
                continue;
            if (item.value->location.begin > closestPreviousNode)
                closestPreviousNode = item.value->location.begin;
            item.value->visit(this);
            if (item.value->location.end <= pos && item.value->location.end > closestPreviousNode)
                closestPreviousNode = item.value->location.end;
        }

        return false;
    }

    bool visit(Luau::AstTypeTable* tbl) override
    {
        if (tbl->location.begin >= pos)
            return false;
        if (tbl->location.begin > closestPreviousNode)
            closestPreviousNode = tbl->location.begin;

        for (Luau::AstTableProp item : tbl->props)
        {
            if (item.type->location.begin >= pos)
                continue;
            if (item.type->location.begin > closestPreviousNode)
                closestPreviousNode = item.type->location.begin;
            item.type->visit(this);
            if (item.type->location.end <= pos && item.type->location.end > closestPreviousNode)
                closestPreviousNode = item.type->location.end;
        }

        return false;
    }

    bool visit(Luau::AstStatDeclareExternType* klass) override
    {
        if (klass->location.begin >= pos)
            return false;
        if (klass->location.begin > closestPreviousNode)
            closestPreviousNode = klass->location.begin;

        for (const auto& item : klass->props)
        {
            if (item.ty->location.begin >= pos)
                continue;
            closestPreviousNode = std::max(closestPreviousNode, item.ty->location.begin);
            item.ty->visit(this);
            if (item.ty->location.end <= pos)
                closestPreviousNode = std::max(closestPreviousNode, item.ty->location.end);
        }

        return false;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        // If the position is after the block, then it can be ignored
        // If the position is within the block, then we know we can cut anything before the block,
        // so set the previous node location to the block entry
        if (block->location.begin >= pos)
            return false;
        if (block->location.begin > closestPreviousNode)
            closestPreviousNode = block->location.begin;

        for (Luau::AstStat* stat : block->body)
        {
            if (stat->location.begin >= pos)
                continue;
            stat->visit(this);
            if (stat->location.end <= pos && stat->location.end > closestPreviousNode)
                closestPreviousNode = stat->location.end;
        }

        return false;
    }

    bool visit(Luau::AstType* ty) override
    {
        return true;
    }

    bool visit(Luau::AstTypePack* ty) override
    {
        return true;
    }
};

std::vector<Luau::Comment> getCommentLocations(const Luau::SourceModule* module, const Luau::Location& node)
{
    if (!module)
        return {};

    AttachCommentsVisitor visitor{node, module->commentLocations};
    visitor.visit(module->root);
    std::vector<Luau::Comment> comments = visitor.attachComments();

    // Luwu: a comment to the right of the node's first line belongs to it too (`bar: string, --- the bar`). Only the
    // first one: anything after it on the line isn't next to the node.
    for (const Luau::Comment& comment : module->commentLocations)
    {
        if (comment.location.begin.line == node.begin.line && comment.location.begin > node.begin)
        {
            comments.push_back(comment);
            break;
        }
    }

    return comments;
}

// Luwu doc references: (offset, length to replace, text) edits that link references in doc text
using DocLinkEdits = std::vector<std::tuple<size_t, size_t, std::string>>;

// Luau's library docs for a reference to a builtin (`[string]`, `[string.split]`, `[print]`), which has no source to
// link to
static std::optional<std::string> builtinDocsLink(std::string_view reference)
{
    static const std::unordered_set<std::string_view> libraries{
        "string", "table", "math", "os", "bit32", "utf8", "coroutine", "debug", "buffer", "vector",
    };
    static const std::unordered_set<std::string_view> globals{
        "assert", "error", "gcinfo", "getfenv", "getmetatable", "ipairs", "next", "newproxy", "pairs", "pcall", "print", "rawequal",
        "rawget", "rawlen", "rawset", "require", "select", "setfenv", "setmetatable", "tonumber", "tostring", "type", "typeof", "unpack",
        "xpcall",
    };

    std::string_view root = reference.substr(0, reference.find_first_of(".:"));
    if (libraries.count(root))
        return "https://luau.org/library#" + std::string(root) + "-library";
    if (root.size() == reference.size() && globals.count(root))
        return "https://luau.org/library#global-functions";
    return std::nullopt;
}

// A reference's rendering. `open` and `close` are the target's brackets in `text`. With a link: `[target]` ->
// `[target](link)`, `[text][target]` -> `[text](link)`. Without one, the brackets go: `[target]` -> `` `target` ``,
// `[text][target]` -> `text`.
static void addDocLinkEdit(
    DocLinkEdits& edits,
    const std::string& text,
    bool displayText,
    size_t open,
    size_t close,
    std::optional<std::string> link
)
{
    std::string_view target = std::string_view(text).substr(open + 1, close - open - 1);
    if (!link)
        link = builtinDocsLink(target);

    if (link && displayText)
        edits.emplace_back(open, close - open + 1, "(" + *link + ")");
    else if (link)
        edits.emplace_back(close + 1, 0, "(" + *link + ")");
    else if (displayText)
    {
        // the display text's own brackets, `]` right before `open`
        size_t textOpen = text.rfind('[', open - 1);
        if (textOpen != std::string::npos)
            edits.emplace_back(textOpen, close - textOpen + 1, text.substr(textOpen + 1, open - 1 - textOpen - 1));
    }
    else
        edits.emplace_back(open, close - open + 1, "`" + std::string(target) + "`");
}

// Applies the edits, and drops binding lines (`[json = require("@std/json")]`), which only matter to the references
static void applyDocLinkEdits(std::string& text, DocLinkEdits& edits)
{
    std::sort(
        edits.begin(),
        edits.end(),
        [](const auto& a, const auto& b)
        {
            return std::get<0>(a) > std::get<0>(b);
        }
    );
    for (const auto& [offset, length, replacement] : edits)
        text.replace(offset, length, replacement);

    static const std::regex binding(R"(^[ \t]*(---)?[ \t]*\[[ \t]*[A-Za-z_]\w*[ \t]*=[^=].*\][ \t]*$)");
    std::string kept;
    bool firstLine = true;
    for (std::string_view line : Luau::split(text, '\n'))
    {
        if (std::regex_match(line.begin(), line.end(), binding))
            continue;
        if (!firstLine)
            kept += '\n';
        kept += line;
        firstLine = false;
    }
    text = std::move(kept);
}

// Luwu doc references: turns each reference in a source comment's text (`[fs.file.read]`, `[text][fs.file.read]`) into a
// markdown link to where it's declared
void WorkspaceFolder::linkDocRefs(
    std::string& commentText,
    const Luau::Location& commentLocation,
    const Luau::SourceModule& sourceModule,
    const Luau::Module& module,
    const TextDocument& textDocument
)
{
    // most doc comments have no references; don't look for them (this runs for every completion item shown)
    if (commentText.find('[') == std::string::npos)
        return;

    // a byte offset into `commentText`
    auto offsetOf = [&](Luau::Position position)
    {
        return textDocument.getText(lsp::Range{textDocument.convertPosition(commentLocation.begin), textDocument.convertPosition(position)}).size();
    };

    DocLinkEdits edits;
    for (const Luau::DocComment& doc : sourceModule.docComments)
    {
        if (!commentLocation.encloses(doc.location) && !doc.location.encloses(commentLocation))
            continue;

        for (const Luau::DocRef& ref : doc.refs)
        {
            if (!commentLocation.encloses(ref.location))
                continue;

            // the reference points where its last name does
            Luau::Position end = ref.methodLocation.end;
            if (!ref.method)
                end = ref.expr ? ref.expr->location.end : ref.type->location.end;
            std::optional<std::string> link;
            for (const Luau::DocRefTarget& candidate : module.docRefTargets)
            {
                if (candidate.location.end != end)
                    continue;
                if (auto uri = docRefTargetLink(candidate))
                    link = uri->toString();
            }

            size_t open = commentText.rfind('[', offsetOf(ref.location.begin));
            size_t close = commentText.find(']', offsetOf(ref.location.end));
            if (open == std::string::npos || close == std::string::npos)
                continue;
            addDocLinkEdit(edits, commentText, open > 0 && commentText[open - 1] == ']', open, close, link);
        }
    }

    applyDocLinkEdits(commentText, edits);
}

// Luwu doc references: links the references in the doc comments a type's metadata holds, resolved where they were
// written (a type function's call, or the declaration)
void WorkspaceFolder::linkMetadataDocRefs(std::vector<std::string>& lines, const Luau::TypeMetadataTable& metadata)
{
    if (!metadata.docContext)
        return;

    std::string text;
    for (const std::string& line : lines)
        text += line + "\n";

    // most docs have no references; resolving them is the costly part, and this runs for every completion item shown
    if (text.find('[') == std::string::npos)
        return;

    const Luau::MetadataDocContext& context = *metadata.docContext;
    const Luau::SourceModule* sourceModule = frontend.getSourceModule(context.module);
    Luau::ModulePtr module = getModule(context.module);
    if (!sourceModule || !module)
        return;

    Luau::TypeCheckLimits limits;
    std::vector<Luau::DocTextLink> links = Luau::resolveDocText(text, *sourceModule, *module, context.location, frontend.builtinTypes,
        Luau::NotNull{frontend.fileResolver}, Luau::NotNull<Luau::ModuleResolver>{&frontend.moduleResolver}, limits, nullptr);

    DocLinkEdits edits;
    for (const Luau::DocTextLink& link : links)
    {
        std::optional<std::string> destination;
        if (link.target)
        {
            if (auto uri = docRefTargetLink(*link.target))
                destination = uri->toString();
        }
        addDocLinkEdit(edits, text, link.displayText, link.open, link.close, destination);
    }
    applyDocLinkEdits(text, edits);

    lines.clear();
    for (std::string_view line : Luau::split(text, '\n'))
        lines.emplace_back(line);
    // the text ended with a newline
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
}

/// Get all moonwave-style documentation comments
/// Performs transformations so that the comments are normalised to lines inside of it (i.e., trimming whitespace, removing comment start/end)
std::vector<std::string> WorkspaceFolder::getComments(const Luau::ModuleName& moduleName, const Luau::Location& node)
{
    Luau::SourceModule* sourceModule;
    TextDocumentPtr textDocument{nullptr};

    if (auto it = definitionsFileState.find(moduleName); it != definitionsFileState.end())
    {
        sourceModule = &it->second.sourceModule;
        textDocument = TextDocumentPtr(&it->second.textDocument);
    }
    else
    {
        sourceModule = frontend.getSourceModule(moduleName);
        if (!sourceModule)
            return {};

        textDocument = fileResolver.getOrCreateTextDocumentFromModuleName(moduleName);
        if (!textDocument)
            return {};
    }

    auto commentLocations = getCommentLocations(sourceModule, node);
    if (commentLocations.empty())
        return {};

    // Luwu doc references: the module's checked references, to link them in the rendered docs
    Luau::ModulePtr checkedModule = definitionsFileState.count(moduleName) ? nullptr : getModule(moduleName);

    std::vector<std::string> comments{};
    for (auto& comment : commentLocations)
    {
        if (comment.type == Luau::Lexeme::Type::BrokenComment)
            continue;

        auto commentText = textDocument->getText(
            lsp::Range{textDocument->convertPosition(comment.location.begin), textDocument->convertPosition(comment.location.end)});
        if (checkedModule)
            linkDocRefs(commentText, comment.location, *sourceModule, *checkedModule, **textDocument);

        // Trim whitespace
        trim(commentText);

        // Parse the comment text for information
        if (comment.type == Luau::Lexeme::Type::Comment)
        {
            if (Luau::startsWith(commentText, "--- "))
            {
                comments.emplace_back(commentText.substr(4));
            }
            else if (commentText == "---")
            {
                comments.emplace_back("\n");
            }
        }
        else if (comment.type == Luau::Lexeme::Type::BlockComment)
        {
            std::regex comment_regex("^--\\[(=*)\\[");
            std::smatch comment_match;

            if (std::regex_search(commentText, comment_match, comment_regex))
            {
                LUAU_ASSERT(comment_match.size() == 2);
                // Construct "--[=[" and "--]=]" which will be ignored
                std::string start_string = "--[" + std::string(comment_match[1].length(), '=') + "[";
                std::string end_string = "]" + std::string(comment_match[1].length(), '=') + "]";

                // Parse each line separately
                for (auto& line : Luau::split(commentText, '\n'))
                {
                    auto lineText = std::string(line);
                    trim_end(lineText);

                    auto trimmedLineText = std::string(line);
                    trim(trimmedLineText);
                    if (trimmedLineText == start_string || trimmedLineText == end_string)
                        continue;
                    comments.emplace_back(lineText);
                }

                // Trim common indentation, but ignore empty lines
                size_t indentLevel = std::string::npos;
                for (auto& line : comments)
                {
                    if (line == "")
                        continue;
                    indentLevel = std::min(indentLevel, line.find_first_not_of(" \n\r\t"));
                }
                for (auto& line : comments)
                {
                    if (line == "")
                        continue;
                    line.erase(0, indentLevel);
                }
            }
        }
    }

    return comments;
}

std::optional<std::string> WorkspaceFolder::getDocumentationForType(const Luau::TypeId ty)
{
    auto followedTy = Luau::follow(ty);

    // Luwu: docs a type carries in its metadata come first: a type a type function made has no source to read them from,
    // and one it changed may have new ones
    // (from `ty`, not `followedTy`: `Cat` in `type Cat = Explain<CatBase>` has docs of its own on the way to CatBase)
    if (const Luau::TypeMetadataTable* metadata = Luau::docMetadataOf(ty))
    {
        if (std::vector<std::string> metadataDocs = Luau::metadataDocLines(*metadata); !metadataDocs.empty())
        {
            linkMetadataDocRefs(metadataDocs, *metadata);
            return printMoonwaveDocumentation(metadataDocs);
        }
    }

    if (auto ftv = Luau::get<Luau::FunctionType>(followedTy); ftv && ftv->definition && ftv->definition->definitionModuleName)
    {
        return printMoonwaveDocumentation(getComments(ftv->definition->definitionModuleName.value(), ftv->definition->definitionLocation));
    }
    else if (auto ttv = Luau::get<Luau::TableType>(followedTy); ttv && !ttv->definitionModuleName.empty() && (ttv->name || ttv->syntheticName))
    {
        // Only a *named* table type documents itself. An anonymous one (`{ string }` written as a
        // function's return annotation, say) still records where it was written, and the comment
        // above that spot belongs to whatever declaration it was written inside -- which is how a
        // function's documentation used to end up on a local holding the result of calling it.
        return printMoonwaveDocumentation(getComments(ttv->definitionModuleName, ttv->definitionLocation));
    }
    else if (auto etv = Luau::get<Luau::ExternType>(followedTy); etv && !etv->definitionModuleName.empty() && etv->definitionLocation)
    {
        return printMoonwaveDocumentation(getComments(etv->definitionModuleName, *etv->definitionLocation));
    }
    return std::nullopt;
}

std::optional<std::string> WorkspaceFolder::getDocumentationForTypeReference(const Luau::ModuleName& moduleName, const Luau::ScopePtr& scope,
    const std::optional<Luau::AstName>& prefix, const Luau::Name& typeName, bool forAutocomplete)
{
    if (prefix)
    {
        auto importedModuleName = lookupImportedModule(*scope, prefix->value);
        if (!importedModuleName)
            return std::nullopt;
        auto importedModule = getModule(*importedModuleName, /* forAutocomplete: */ forAutocomplete);
        if (!importedModule)
            return std::nullopt;
        if (const auto it = importedModule->exportedTypeBindings.find(typeName);
            it != importedModule->exportedTypeBindings.end() && it->second.definitionLocation)
            return printMoonwaveDocumentation(getComments(*importedModuleName, *it->second.definitionLocation));
        return std::nullopt;
    }
    else
    {
        auto typeLocation = lookupTypeLocation(*scope, typeName);
        if (!typeLocation)
            return std::nullopt;
        return printMoonwaveDocumentation(getComments(moduleName, *typeLocation));
    }
}

std::optional<std::string> WorkspaceFolder::getDocumentationForAstNode(
    const Luau::ModuleName& moduleName, const Luau::AstNode* node, const Luau::ScopePtr scope)
{
    if (auto ref = node->as<Luau::AstTypeReference>())
    {
        auto config = client->getConfiguration(rootUri);
        return getDocumentationForTypeReference(
            moduleName, scope, ref->prefix, ref->name.value, /* forAutocomplete= */ config.hover.strictDatamodelTypes);
    }
    else if (auto alias = node->as<Luau::AstStatTypeAlias>())
    {
        return printMoonwaveDocumentation(getComments(moduleName, alias->location));
    }
    return std::nullopt;
}
