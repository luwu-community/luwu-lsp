#include "LSP/Workspace.hpp"

#include <algorithm>
#include <cctype>

#include "Luau/AstQuery.h"

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

lsp::DocumentOnTypeFormattingResult WorkspaceFolder::onTypeFormatting(const lsp::DocumentOnTypeFormattingParams& params)
{
    auto config = client->getConfiguration(rootUri);

    if (params.ch != "{")
        return std::nullopt;

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        return std::nullopt;

    auto position = textDocument->convertPosition(params.position);

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
