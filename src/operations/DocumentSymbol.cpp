#include "LSP/LanguageServer.hpp"
#include "LSP/Workspace.hpp"

#include "Luau/PrettyPrinter.h"
#include "Protocol/LanguageFeatures.hpp"

// Luwu Pattern Matching (rfcs/control-flow/pattern-matching.md): a `match` spanning at least this many lines is listed
// as a symbol. Editors that pin the enclosing symbols to the top of the view (VS Code's sticky scroll) then keep its
// header in view, the way they keep a long function's.
static constexpr unsigned int kMatchSymbolMinLines = 20;

// The longest subject text a `match` symbol's name shows before it's cut off with "..."
static constexpr size_t kMatchSubjectMaxLength = 60;

struct DocumentSymbolsVisitor : public Luau::AstVisitor
{
    const TextDocument* textDocument;
    std::vector<lsp::DocumentSymbol> symbols{};
    lsp::DocumentSymbol* parent = nullptr;

    explicit DocumentSymbolsVisitor(const TextDocument* textDocument)
        : textDocument(textDocument)
    {
    }

    void addSymbol(lsp::DocumentSymbol& symbol)
    {
        // Verify that the symbol locations are valid
        // If selection range falls outside of enclosing range, then expand the enclosing range
        if (symbol.selectionRange.start < symbol.range.start)
            symbol.range.start = symbol.selectionRange.start;
        if (symbol.selectionRange.end > symbol.range.end)
            symbol.range.end = symbol.selectionRange.end;

        if (parent)
            parent->children.push_back(symbol);
        else
            symbols.push_back(symbol);
    }

    void createLocalSymbol(Luau::AstLocal* local, Luau::Location enclosingRange)
    {
        lsp::DocumentSymbol symbol;
        symbol.name = local->name.value;
        symbol.kind = lsp::SymbolKind::Variable;
        symbol.range = {textDocument->convertPosition(enclosingRange.begin), textDocument->convertPosition(enclosingRange.end)};
        symbol.selectionRange = {textDocument->convertPosition(local->location.begin), textDocument->convertPosition(local->location.end)};
        addSymbol(symbol);
    }

    bool visit(Luau::AstStatLocal* local) override
    {
        for (size_t i = 0; i < local->vars.size; ++i)
        {
            createLocalSymbol(local->vars.data[i], local->location);
            // TODO: if the value assigned is a table, should we include its properties?
        }

        // A long `match` expression assigned to the local (`const label = match ...`) is listed too
        for (Luau::AstExpr* value : local->values)
        {
            if (auto* match = Luau::withoutGroups(value)->as<Luau::AstExprMatch>())
                match->visit(this);
        }
        return false;
    }

    // `match` followed by the subject's name in `match const name = value`, or else by the subject as written
    std::string matchSymbolName(const Luau::AstMatchSubject& subject)
    {
        bool named = subject.declaration && subject.declaration->keywordLocation;
        if (named)
            return std::string("match ") + subject.local->name.value;

        std::string text = textDocument->getText(textDocument->convertLocation(subject.value->location));
        std::string collapsed;
        for (char c : text)
        {
            bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            if (!space)
                collapsed += c;
            else if (!collapsed.empty() && collapsed.back() != ' ')
                collapsed += ' ';
        }
        if (collapsed.size() > kMatchSubjectMaxLength)
            collapsed = collapsed.substr(0, kMatchSubjectMaxLength) + "...";
        return "match " + collapsed;
    }

    // Lists a long `match` as a symbol holding the symbols in its arms. A shorter one only has its arms visited, so
    // what's in them is still listed under the enclosing symbol.
    void visitMatch(const Luau::Location& location, const Luau::AstMatchSubject& subject, const Luau::AstArray<Luau::AstMatchArm>& arms)
    {
        const auto visitArms = [&]()
        {
            for (const Luau::AstMatchArm& arm : arms)
            {
                if (arm.body)
                    arm.body->visit(this);
                if (arm.value)
                    arm.value->visit(this);
            }
        };

        bool isLong = location.end.line - location.begin.line + 1 >= kMatchSymbolMinLines;
        if (!isLong)
        {
            visitArms();
            return;
        }

        lsp::DocumentSymbol symbol =
            makeSymbol(matchSymbolName(subject), lsp::SymbolKind::Operator, location, Luau::Location(location.begin, subject.value->location.end));

        auto oldParent = parent;
        parent = &symbol;
        visitArms();
        parent = oldParent;

        addSymbol(symbol);
    }

    bool visit(Luau::AstStatMatch* match) override
    {
        visitMatch(match->location, match->subject, match->arms);
        return false;
    }

    bool visit(Luau::AstExprMatch* match) override
    {
        visitMatch(match->location, match->subject, match->arms);
        return false;
    }

    bool visit(Luau::AstStatFunction* function) override
    {
        lsp::DocumentSymbol symbol;
        symbol.name = Luau::toString(function->name);
        symbol.kind = lsp::SymbolKind::Function;
        symbol.range = {textDocument->convertPosition(function->location.begin), textDocument->convertPosition(function->location.end)};
        symbol.selectionRange = {
            textDocument->convertPosition(function->name->location.begin), textDocument->convertPosition(function->name->location.end)};
        trim(symbol.name);
        visitFunction(function->func, symbol);
        addSymbol(symbol);
        return false;
    }

    bool visit(Luau::AstStatLocalFunction* func) override
    {
        lsp::DocumentSymbol symbol;
        symbol.name = Luau::toString(func->name);
        symbol.kind = lsp::SymbolKind::Function;
        symbol.range = {textDocument->convertPosition(func->location.begin), textDocument->convertPosition(func->location.end)};
        symbol.selectionRange = {textDocument->convertPosition(func->name->location.begin), textDocument->convertPosition(func->name->location.end)};
        visitFunction(func->func, symbol);
        addSymbol(symbol);
        return false;
    }

    bool visit(Luau::AstStatTypeAlias* alias) override
    {
        lsp::DocumentSymbol symbol;
        symbol.name = alias->name.value;
        symbol.kind = lsp::SymbolKind::Interface;
        symbol.range = {textDocument->convertPosition(alias->location.begin), textDocument->convertPosition(alias->location.end)};
        symbol.selectionRange = {textDocument->convertPosition(alias->nameLocation.begin), textDocument->convertPosition(alias->nameLocation.end)};
        addSymbol(symbol);
        return false;
    }

    void visitFunction(Luau::AstExprFunction* func, lsp::DocumentSymbol& symbol)
    {
        auto oldParent = parent;
        parent = &symbol;

        // Create parameter symbols + build function detail
        std::string detail = "function (";
        bool comma = false;
        for (auto* arg : func->args)
        {
            createLocalSymbol(arg, func->argLocation.value_or(func->location));
            if (comma)
                detail += ", ";
            detail += arg->name.value;
            comma = true;
        }
        if (func->vararg)
        {
            auto enclosingPosition = func->argLocation.value_or(func->location);
            lsp::DocumentSymbol symbol;
            symbol.name = "...";
            symbol.kind = lsp::SymbolKind::Variable;
            symbol.range = {textDocument->convertPosition(enclosingPosition.begin), textDocument->convertPosition(enclosingPosition.end)};
            symbol.selectionRange = {
                textDocument->convertPosition(func->varargLocation.begin), textDocument->convertPosition(func->varargLocation.end)};

            addSymbol(symbol);

            if (comma)
                detail += ", ";
            detail += "...";
        }
        detail += ")";
        symbol.detail = detail;

        // Create symbols for body
        for (Luau::AstStat* stat : func->body->body)
        {
            stat->visit(this);
        }

        parent = oldParent;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* stat : block->body)
        {
            stat->visit(this);
        }

        return false;
    }

    lsp::DocumentSymbol makeSymbol(const std::string& name, lsp::SymbolKind kind, const Luau::Location& range, const Luau::Location& selection)
    {
        lsp::DocumentSymbol symbol;
        symbol.name = name;
        symbol.kind = kind;
        symbol.range = {textDocument->convertPosition(range.begin), textDocument->convertPosition(range.end)};
        symbol.selectionRange = {textDocument->convertPosition(selection.begin), textDocument->convertPosition(selection.end)};
        return symbol;
    }

    // Luwu Classes: a class and its members. A declared class (Luwu Declare Statements) is listed the same way.
    void addClassSymbol(Luau::AstStatClass* classStat, const Luau::Location& range)
    {
        // Luwu Traits (rfcs/classes/traits.md): a trait is listed as an interface, its factory as its constructor
        lsp::SymbolKind classKind = classStat->isTrait ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
        lsp::DocumentSymbol symbol = makeSymbol(classStat->name->name.value, classKind, range, classStat->name->location);

        auto oldParent = parent;
        parent = &symbol;

        for (const auto& member : classStat->members)
        {
            if (const auto* prop = member.get_if<Luau::AstClassProperty>())
            {
                lsp::DocumentSymbol field = makeSymbol(prop->name.value, lsp::SymbolKind::Field, prop->nameLocation, prop->nameLocation);
                addSymbol(field);
            }
            else if (const auto* method = member.get_if<Luau::AstClassMethod>())
            {
                bool isConstructor = method->functionName == (classStat->isTrait ? "__create" : "__init");
                lsp::SymbolKind kind = isConstructor ? lsp::SymbolKind::Constructor : lsp::SymbolKind::Method;
                lsp::DocumentSymbol methodSymbol = makeSymbol(method->functionName.value, kind, method->function->location, method->nameLocation);
                visitFunction(method->function, methodSymbol);
                addSymbol(methodSymbol);
            }
        }

        parent = oldParent;
        addSymbol(symbol);
    }

    bool visit(Luau::AstStatClass* classStat) override
    {
        addClassSymbol(classStat, classStat->location);
        return false;
    }

    // Luwu Declare Statements: declarations in a source file
    bool visit(Luau::AstStatDeclareClass* declaredClass) override
    {
        addClassSymbol(declaredClass->shape, declaredClass->location);
        return false;
    }

    bool visit(Luau::AstStatDeclareGlobal* global) override
    {
        lsp::DocumentSymbol symbol = makeSymbol(global->name.value, lsp::SymbolKind::Variable, global->location, global->nameLocation);
        addSymbol(symbol);
        return false;
    }

    bool visit(Luau::AstStatDeclareFunction* function) override
    {
        lsp::DocumentSymbol symbol = makeSymbol(function->name.value, lsp::SymbolKind::Function, function->location, function->nameLocation);
        addSymbol(symbol);
        return false;
    }

    bool visit(Luau::AstStatDeclareExternType* externType) override
    {
        lsp::DocumentSymbol symbol = makeSymbol(externType->name.value, lsp::SymbolKind::Class, externType->location, externType->nameLocation);

        auto oldParent = parent;
        parent = &symbol;

        for (const Luau::AstDeclaredExternTypeProperty& prop : externType->props)
        {
            lsp::SymbolKind kind = prop.isMethod ? lsp::SymbolKind::Method : lsp::SymbolKind::Field;
            lsp::DocumentSymbol member = makeSymbol(prop.name.value, kind, prop.location, prop.nameLocation);
            addSymbol(member);
        }

        parent = oldParent;
        addSymbol(symbol);
        return false;
    }
};

std::optional<std::vector<lsp::DocumentSymbol>> WorkspaceFolder::documentSymbol(const lsp::DocumentSymbolParams& params)
{
    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());

    frontend.parse(moduleName);

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return std::nullopt;

    DocumentSymbolsVisitor visitor{textDocument};
    visitor.visit(sourceModule->root);
    return visitor.symbols;
}

std::optional<std::vector<lsp::DocumentSymbol>> LanguageServer::documentSymbol(const lsp::DocumentSymbolParams& params)
{
    auto workspace = findWorkspace(params.textDocument.uri);
    return workspace->documentSymbol(params);
}
