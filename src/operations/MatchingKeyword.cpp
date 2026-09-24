#include "LSP/Workspace.hpp"

#include "Luau/Ast.h"

#include <set>

/// A block-like construct and the keywords that delimit it, e.g. `if`/`then`/`elseif`/`else`/`end`,
/// `repeat`/`until`, or `function`/`end`. Jumping moves between the opener and the closer; the
/// keywords in the middle of the construct count as part of the opener for that purpose.
struct KeywordGroup
{
    Luau::Location opener;
    std::vector<Luau::Location> middles;
    std::optional<Luau::Location> closer;
};

static bool isIdentifierChar(char c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/// Checks that `keyword` really is written at `begin`, as a whole word. The AST records where the
/// parser thinks a keyword is, but an `end` recovered from a syntax error may not be written at all,
/// and a construct may be closed by something other than the keyword we are looking for.
static bool isKeywordAt(const TextDocument& textDocument, const Luau::Position& begin, const std::string_view keyword)
{
    if (begin.line >= textDocument.lineCount())
        return false;

    auto line = textDocument.getLine(begin.line);
    if (begin.column + keyword.size() > line.size())
        return false;

    if (std::string_view(line).substr(begin.column, keyword.size()) != keyword)
        return false;

    if (begin.column > 0 && isIdentifierChar(line.at(begin.column - 1)))
        return false;

    auto after = begin.column + keyword.size();
    return after >= line.size() || !isIdentifierChar(line.at(after));
}

static std::optional<Luau::Location> keywordAt(const TextDocument& textDocument, const Luau::Position& begin, const std::string_view keyword)
{
    if (!isKeywordAt(textDocument, begin, keyword))
        return std::nullopt;
    return Luau::Location{begin, Luau::Position{begin.line, begin.column + static_cast<unsigned int>(keyword.size())}};
}

/// Finds the keyword that closes `location`. A construct always ends on its closing keyword, so we
/// look for it immediately before the end of the construct rather than trusting the AST to have
/// recorded a location for it.
static std::optional<Luau::Location> trailingKeyword(const TextDocument& textDocument, const Luau::Location& location, const std::string_view keyword)
{
    if (location.end.column < keyword.size())
        return std::nullopt;

    return keywordAt(textDocument, Luau::Position{location.end.line, location.end.column - static_cast<unsigned int>(keyword.size())}, keyword);
}

namespace
{
struct KeywordGroupVisitor : public Luau::AstVisitor
{
    KeywordGroupVisitor(const TextDocument& textDocument, const Luau::AstStatBlock* root)
        : textDocument(textDocument)
        , root(root)
    {
    }

    const TextDocument& textDocument;
    /// The block the whole file is, which is not a construct and has no keywords of its own
    const Luau::AstStatBlock* root;
    std::vector<KeywordGroup> groups;

    /// Every keyword position we have already attributed to a group. A single keyword shows up on
    /// more than one node -- `while`'s `do` also starts its body block, an `elseif` is both the
    /// parent's `else` and the child's `if`, and a function statement and its expression share the
    /// `function` keyword -- and we only want to record it once.
    std::set<Luau::Position> claimed;
    /// `elseif` clauses, which are folded into the `if` they belong to instead of forming a group.
    std::set<const Luau::AstStatIf*> chainedClauses;

    void add(KeywordGroup group)
    {
        if (claimed.count(group.opener.begin) > 0)
            return;

        claimed.insert(group.opener.begin);
        for (const auto& middle : group.middles)
            claimed.insert(middle.begin);
        if (group.closer)
            claimed.insert(group.closer->begin);

        groups.emplace_back(std::move(group));
    }

    void addMiddle(KeywordGroup& group, const Luau::Location& location, const std::string_view keyword)
    {
        if (claimed.count(location.begin) > 0)
            return;
        if (auto middle = keywordAt(textDocument, location.begin, keyword))
            group.middles.emplace_back(*middle);
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        if (block == root)
            return true;

        // A bare `do ... end` block. Every other block (a function body, an `if` branch) starts
        // after the keyword that introduces it, so the `do` test is what distinguishes them.
        if (auto opener = keywordAt(textDocument, block->location.begin, "do"))
            add({*opener, {}, block->hasEnd ? trailingKeyword(textDocument, block->location, "end") : std::nullopt});

        return true;
    }

    bool visit(Luau::AstStatIf* node) override
    {
        if (chainedClauses.count(node) > 0)
            return true;

        KeywordGroup group{node->ifLocation};

        for (auto* clause = node;;)
        {
            if (clause->thenLocation)
                addMiddle(group, *clause->thenLocation, "then");
            if (clause->elseLocation)
                addMiddle(group, *clause->elseLocation, clause->elsebody && clause->elsebody->is<Luau::AstStatIf>() ? "elseif" : "else");

            auto* next = clause->elsebody ? clause->elsebody->as<Luau::AstStatIf>() : nullptr;
            if (!next)
                break;

            chainedClauses.insert(next);
            addMiddle(group, next->ifLocation, "elseif");
            clause = next;
        }

        group.closer = trailingKeyword(textDocument, node->location, "end");
        add(std::move(group));
        return true;
    }

    bool visit(Luau::AstStatWhile* node) override
    {
        KeywordGroup group{node->whileLocation};
        if (node->hasDo)
            addMiddle(group, node->doLocation, "do");
        group.closer = trailingKeyword(textDocument, node->location, "end");
        add(std::move(group));
        return true;
    }

    bool visit(Luau::AstStatRepeat* node) override
    {
        KeywordGroup group{node->repeatLocation};
        group.closer = keywordAt(textDocument, node->untilLocation.begin, "until");
        add(std::move(group));
        return true;
    }

    bool visit(Luau::AstStatFor* node) override
    {
        KeywordGroup group{node->forLocation};
        if (node->hasDo)
            addMiddle(group, node->doLocation, "do");
        group.closer = trailingKeyword(textDocument, node->location, "end");
        add(std::move(group));
        return true;
    }

    bool visit(Luau::AstStatForIn* node) override
    {
        KeywordGroup group{node->forLocation};
        if (node->hasIn)
            addMiddle(group, node->inLocation, "in");
        if (node->hasDo)
            addMiddle(group, node->doLocation, "do");
        group.closer = trailingKeyword(textDocument, node->location, "end");
        add(std::move(group));
        return true;
    }

    bool visit(Luau::AstStatFunction* node) override
    {
        add({node->functionLocation, {}, trailingKeyword(textDocument, node->location, "end")});
        return true;
    }

    bool visit(Luau::AstStatLocalFunction* node) override
    {
        add({node->functionLocation, {}, trailingKeyword(textDocument, node->location, "end")});
        return true;
    }

    bool visit(Luau::AstExprFunction* node) override
    {
        // An anonymous function, unless this is the body of a function statement, in which case the
        // statement has already claimed the `function` keyword.
        if (auto opener = keywordAt(textDocument, node->location.begin, "function"))
            add({*opener, {}, trailingKeyword(textDocument, node->location, "end")});

        return true;
    }

    bool visit(Luau::AstStatClass* node) override
    {
        add({node->keywordLocation, {}, node->hasEnd ? trailingKeyword(textDocument, node->location, "end") : std::nullopt});
        return true;
    }
};
} // namespace

/// Keywords are always on a single line, and we accept the position just past the keyword so that
/// the jump works with the cursor sitting at the end of an `end` you have just typed.
static bool containsPosition(const Luau::Location& location, const Luau::Position& position)
{
    return position.line == location.begin.line && position.column >= location.begin.column && position.column <= location.end.column;
}

lsp::MatchingKeywordResult WorkspaceFolder::matchingKeyword(const lsp::MatchingKeywordParams& params)
{
    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        return std::nullopt;

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return std::nullopt;

    auto position = textDocument->convertPosition(params.position);

    KeywordGroupVisitor visitor{*textDocument, sourceModule->root};
    sourceModule->root->visit(&visitor);

    for (const auto& group : visitor.groups)
    {
        if (group.closer && containsPosition(*group.closer, position))
            return textDocument->convertLocation(group.opener);

        if (!group.closer)
            continue;

        if (containsPosition(group.opener, position))
            return textDocument->convertLocation(*group.closer);

        for (const auto& middle : group.middles)
            if (containsPosition(middle, position))
                return textDocument->convertLocation(*group.closer);
    }

    return std::nullopt;
}
