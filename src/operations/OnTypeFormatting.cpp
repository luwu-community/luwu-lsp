#include "LSP/Workspace.hpp"

#include <algorithm>
#include <cctype>

#include "Luau/AstQuery.h"
#include "Luau/Parser.h"

static Luau::AstExprConstantString* findStringNodeAtPosition(const Luau::SourceModule& sourceModule, const Luau::Position& position)
{
    auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, position);
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
        if (auto str = (*it)->as<Luau::AstExprConstantString>())
            return str;

    return nullptr;
}

static std::optional<std::vector<lsp::TextEdit>> convertQuotesForString(
    const Luau::AstExprConstantString* stringNode, const TextDocument* textDocument, const Luau::Position& position)
{
    if (stringNode->quoteStyle != Luau::AstExprConstantString::QuoteStyle::QuotedSimple && stringNode->quoteStyle != Luau::AstExprConstantString::QuoteStyle::QuotedSingle)
        return std::nullopt;

    auto& location = stringNode->location;
    if (location.begin.line != location.end.line)
        return std::nullopt;

    if (position.line != location.begin.line)
        return std::nullopt;
    if (position.column <= location.begin.column || position.column >= location.end.column)
        return std::nullopt;

    std::string_view content(stringNode->value.data, stringNode->value.size);
    if (content.find('`') != std::string_view::npos)
        return std::nullopt;

    std::vector<lsp::TextEdit> edits;

    edits.push_back(lsp::TextEdit{{textDocument->convertPosition(location.begin),
                                      textDocument->convertPosition(Luau::Position{location.begin.line, location.begin.column + 1})},
        "`"});

    edits.push_back(lsp::TextEdit{
        {textDocument->convertPosition(Luau::Position{location.end.line, location.end.column - 1}), textDocument->convertPosition(location.end)},
        "`"});

    return edits;
}

static bool isEscaped(const std::string& line, size_t idx)
{
    size_t backslashCount = 0;
    for (size_t i = idx; i > 0; --i)
    {
        if (line[i - 1] == '\\')
            backslashCount++;
        else
            break;
    }
    return backslashCount % 2 == 1;
}

static std::optional<size_t> findUnclosedChar(const std::string& line, size_t until, char ch)
{
    size_t count = 0;
    std::optional<size_t> last;

    for (size_t i = 0; i < until; ++i)
    {
        if (line[i] == ch && !isEscaped(line, i))
        {
            count++;
            last = i;
        }
    }

    return (count % 2 == 1) ? last : std::nullopt;
}

static std::optional<std::vector<lsp::TextEdit>> convertQuotesByHeuristic(const TextDocument* textDocument, const Luau::Position& position)
{
    auto lineText = textDocument->getLine(position.line);
    size_t column = position.column;

    auto doublePos = findUnclosedChar(lineText, column, '"');
    auto singlePos = findUnclosedChar(lineText, column, '\'');
    if (!doublePos && !singlePos)
        return std::nullopt;

    // Pick the first unclosed quote
    size_t quotePos = std::min(doublePos.value_or(SIZE_MAX), singlePos.value_or(SIZE_MAX));

    // Skip if we are inside an unfinished interpolated string
    if (findUnclosedChar(lineText, quotePos, '`'))
        return std::nullopt;

    size_t contentPos = quotePos + 1;

    auto content = std::string_view(lineText).substr(contentPos);
    if (content.find('`') != std::string_view::npos)
        return std::nullopt;

    std::vector<lsp::TextEdit> edits;

    edits.push_back(lsp::TextEdit{{textDocument->convertPosition(Luau::Position{position.line, static_cast<unsigned int>(quotePos)}),
                                      textDocument->convertPosition(Luau::Position{position.line, static_cast<unsigned int>(contentPos)})},
        "`"});

    return edits;
}

// Luwu Attributes (rfcs/attributes-for-types-variables-fields-classes.md): `@deprecated {` becomes
// `@[deprecated {}]`, since a bare attribute can't take arguments. Only for an attribute that takes them.
static std::optional<std::vector<lsp::TextEdit>> bracketAttributeArguments(const TextDocument* textDocument, const Luau::Position& position)
{
    const std::string lineText = textDocument->getLine(position.line);
    const size_t brace = position.column - 1;
    if (position.column == 0 || brace >= lineText.size() || lineText[brace] != '{')
        return std::nullopt;

    auto isNameChar = [](char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    size_t nameEnd = brace;
    while (nameEnd > 0 && lineText[nameEnd - 1] == ' ')
        --nameEnd;

    size_t nameBegin = nameEnd;
    while (nameBegin > 0 && isNameChar(lineText[nameBegin - 1]))
        --nameBegin;

    const bool bareAttribute = nameBegin < nameEnd && nameBegin > 0 && lineText[nameBegin - 1] == '@';
    if (!bareAttribute)
        return std::nullopt;

    const std::string name = lineText.substr(nameBegin, nameEnd - nameBegin);
    const auto attributes = Luau::getKnownAttributes();
    const bool takesArguments = std::any_of(
        attributes.begin(),
        attributes.end(),
        [&name](const Luau::AttributeInfo& info)
        {
            return info.argumentFields && name == info.name;
        }
    );
    if (!takesArguments)
        return std::nullopt;

    auto at = [&](size_t column)
    {
        return textDocument->convertPosition(Luau::Position{position.line, static_cast<unsigned int>(column)});
    };

    const size_t atSign = nameBegin - 1;
    std::vector<lsp::TextEdit> edits;
    edits.push_back(lsp::TextEdit{{at(atSign), at(atSign + 1)}, "@["});

    // Most editors have already closed the brace; close the brackets after it.
    const size_t cursor = position.column;
    if (cursor < lineText.size() && lineText[cursor] == '}')
        edits.push_back(lsp::TextEdit{{at(cursor + 1), at(cursor + 1)}, "]"});
    else
        edits.push_back(lsp::TextEdit{{at(cursor), at(cursor)}, "}]"});

    return edits;
}

// Finds a table element whose value starts before `cursor` and ends at `end`
struct TableElementEndingAt : Luau::AstVisitor
{
    Luau::Position cursor;
    Luau::Position end;
    bool found = false;

    TableElementEndingAt(const Luau::Position& cursor, const Luau::Position& end)
        : cursor(cursor)
        , end(end)
    {
    }

    bool visit(Luau::AstExprTable* table) override
    {
        for (const auto& item : table->items)
        {
            if (item.value->location.begin < cursor && item.value->location.end == end)
                found = true;
        }
        return !found;
    }
};

// Typing `(`, `{` or a quote at the end of a table element (`Paragraph()` or `""` on its own line) leaves the element
// without the comma that separates it from the next one, once the editor auto-closes the bracket or quote. Inserts
// that comma after the closing brackets when they end a table element. Call arguments don't get one.
static std::optional<std::vector<lsp::TextEdit>> insertMissingComma(
    const TextDocument* textDocument, const Luau::Position& position, const Luau::ParseOptions& options)
{
    const std::string lineText = textDocument->getLine(position.line);
    if (position.column == 0 || position.column >= lineText.size())
        return std::nullopt;

    const char open = lineText[position.column - 1];
    const char close = open == '(' ? ')' : open == '{' ? '}' : open;
    if ((open != '(' && open != '{' && open != '"' && open != '\'' && open != '`') || lineText[position.column] != close)
        return std::nullopt;

    // The rest of the line holds only the auto-closed bracket and the brackets it sits in, and maybe a comment
    size_t commaColumn = position.column + 1;
    while (commaColumn < lineText.size() && (lineText[commaColumn] == ')' || lineText[commaColumn] == '}' || lineText[commaColumn] == ']'))
        ++commaColumn;

    const size_t restBegin = lineText.find_first_not_of(" \t\r\n", commaColumn);
    if (restBegin != std::string::npos && lineText.compare(restBegin, 2, "--") != 0)
        return std::nullopt;

    const std::string source = textDocument->getText();
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseResult result = Luau::Parser::parse(source.data(), source.size(), names, allocator, options);
    if (!result.root)
        return std::nullopt;

    const Luau::Position commaPosition{position.line, static_cast<unsigned int>(commaColumn)};
    TableElementEndingAt finder{position, commaPosition};
    result.root->visit(&finder);
    if (!finder.found)
        return std::nullopt;

    const lsp::Position lspCommaPosition = textDocument->convertPosition(commaPosition);
    return std::vector<lsp::TextEdit>{lsp::TextEdit{{lspCommaPosition, lspCommaPosition}, ","}};
}

lsp::DocumentOnTypeFormattingResult WorkspaceFolder::onTypeFormatting(const lsp::DocumentOnTypeFormattingParams& params)
{
    auto config = client->getConfiguration(rootUri);

    if (params.ch != "{" && params.ch != "(" && params.ch != "\"" && params.ch != "'" && params.ch != "`")
        return std::nullopt;

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        return std::nullopt;

    auto position = textDocument->convertPosition(params.position);

    if (config.format.insertMissingComma)
    {
        const Luau::Config& luauConfig = frontend.configResolver->getConfig(moduleName, limits);
        Luau::ParseOptions options = luauConfig.parseOptions;
        options.language = luauConfig.language;
        options.luwon = params.textDocument.uri.extension() == ".luwon";
        options.noErrorLimit = true;

        if (auto edits = insertMissingComma(textDocument, position, options))
            return edits;
    }

    if (params.ch != "{")
        return std::nullopt;

    if (config.format.bracketAttributeArguments)
    {
        if (auto edits = bracketAttributeArguments(textDocument, position))
            return edits;
    }

    if (!config.format.convertQuotes)
        return std::nullopt;

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return std::nullopt;

    if (auto* stringNode = findStringNodeAtPosition(*sourceModule, position))
        return convertQuotesForString(stringNode, textDocument, position);

    return convertQuotesByHeuristic(textDocument, position);
}
