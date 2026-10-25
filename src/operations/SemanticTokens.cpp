#include "LSP/SemanticTokens.hpp"
#include "LSP/Workspace.hpp"

#include "Luau/AstQuery.h"
#include "Luau/StringUtils.h"
#include "LSP/LuauExt.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>

static void fillBuiltinGlobals(std::unordered_map<Luau::AstName, Luau::TypeId>& builtins, const Luau::AstNameTable& names, const Luau::ScopePtr& env)
{
    Luau::ScopePtr current = env;
    while (true)
    {
        for (auto& [global, binding] : current->bindings)
        {
            Luau::AstName name = names.get(global.c_str());

            if (name.value)
                builtins.insert_or_assign(name, binding.typeId);
        }

        if (current->parent)
            current = current->parent;
        else
            break;
    }
}

enum struct AstLocalInfo
{
    // local is self
    Self,
    // local is a function parameter
    Parameter,
};

// A name that refers to a Luwu class: either the class value itself, or the type of objects built
// from it, which is what a `: UserId` annotation resolves to. Host-declared extern types (Roblox's
// `Instance` and friends) are ExternTypes too, and aren't classes anyone wrote as one.
// Luwu Traits (rfcs/classes/traits.md): a trait's value is rooted at `trait` rather than `class`.
static bool namesUserDefinedClass(const Luau::TypeId ty, Luau::NotNull<Luau::BuiltinTypes> builtinTypes)
{
    const auto* et = Luau::get<Luau::ExternType>(Luau::follow(ty));
    return et &&
           (et->parent == builtinTypes->classType || et->parent == builtinTypes->objectType || et->parent == builtinTypes->traitType);
}

// The class value itself, as opposed to an object of it: `Player` is a class, `local p = Player(…)`
// is a variable that happens to hold an object.
static bool isUserDefinedClassValue(const Luau::TypeId ty, Luau::NotNull<Luau::BuiltinTypes> builtinTypes)
{
    const auto* et = Luau::get<Luau::ExternType>(Luau::follow(ty));
    return et && (et->parent == builtinTypes->classType || et->parent == builtinTypes->traitType);
}

// Luwu Traits (rfcs/classes/traits.md): the trait value itself, as opposed to an object of a class implementing it.
// The trait info lives on the object type, so the value is recognised through its relation to that type.
static bool isTraitValue(const Luau::TypeId ty)
{
    const auto* et = Luau::get<Luau::ExternType>(Luau::follow(ty));
    const Luau::Obj* obj = et && et->relation ? Luau::get_if<Luau::Obj>(&*et->relation) : nullptr;
    const auto* objectEt = obj ? Luau::get<Luau::ExternType>(Luau::follow(obj->ty)) : nullptr;
    return objectEt && objectEt->traitInfo;
}

// Luwu Traits: a name that refers to a trait -- the trait value, or the trait as a type.
// Traits are coloured as interfaces, so they read differently from the classes that implement them.
static bool namesTrait(const Luau::TypeId ty)
{
    const auto* et = Luau::get<Luau::ExternType>(Luau::follow(ty));
    return (et && et->traitInfo) || isTraitValue(ty);
}

static lsp::SemanticTokenTypes inferTokenType(const Luau::TypeId ty, lsp::SemanticTokenTypes base, Luau::NotNull<Luau::BuiltinTypes> builtinTypes)
{
    if (!ty)
        return base;

    auto followedTy = Luau::follow(ty);

    // A variable holding an object typed as a trait (`const ent = p:entry()`) is just a variable
    if (isTraitValue(followedTy))
        return lsp::SemanticTokenTypes::Interface;

    if (isUserDefinedClassValue(followedTy, builtinTypes))
        return lsp::SemanticTokenTypes::Class;

    if (auto ftv = Luau::get<Luau::FunctionType>(followedTy))
    {
        if (isMethod(ftv))
            return lsp::SemanticTokenTypes::Method;
        else
            return lsp::SemanticTokenTypes::Function;
    }
    else if (Luau::get<Luau::IntersectionType>(followedTy))
    {
        if (isOverloadedMethod(followedTy))
            return lsp::SemanticTokenTypes::Method;
        else if (Luau::isOverloadedFunction(followedTy))
            return lsp::SemanticTokenTypes::Function;
    }
    else if (auto ttv = Luau::get<Luau::TableType>(followedTy))
    {
        if (ttv->name && ttv->name == "RBXScriptSignal")
        {
            return lsp::SemanticTokenTypes::Event;
        }
    }

    return base;
}

struct SemanticTokensVisitor : public Luau::AstVisitor
{
    const Luau::ModulePtr& module;
    const std::unordered_map<Luau::AstName, Luau::TypeId>& builtinGlobals;
    Luau::NotNull<Luau::BuiltinTypes> builtinTypes;
    bool grammarHighlightsNone;
    std::vector<SemanticToken> tokens{};
    std::unordered_map<Luau::AstLocal*, AstLocalInfo> localMap{};
    std::unordered_set<Luau::AstType*> syntheticTypes{};

    explicit SemanticTokensVisitor(
        const Luau::ModulePtr& module, const std::unordered_map<Luau::AstName, Luau::TypeId>& builtinGlobals,
        Luau::NotNull<Luau::BuiltinTypes> builtinTypes, bool grammarHighlightsNone)
        : module(module)
        , builtinGlobals(builtinGlobals)
        , builtinTypes(builtinTypes)
        , grammarHighlightsNone(grammarHighlightsNone)
    {
    }

    // HACK: Luau introduces some synthetic tokens in the AST for types
    // { T } gets converted to { [number]: T } (where number is an introduced AstTypeReference)
    // string? gets converted to string | nil (where nil is an introduced AstTypeReference)
    // We do not want to highlight these synthetic tokens, as they don't exist in real code.
    // We apply visitors in these locations to try and check for synthetic tokens, and mark them as such
    bool visit(Luau::AstTypeTable* table) override
    {
        // Prevent overlapping tokens for `{ T }` sugar (e.g. `{ string }`): the parser gives the synthetic
        // `number` index type a zero-width location, which we use to detect and skip it here.
        if (table->indexer && table->indexer->indexType->location.begin == table->indexer->indexType->location.end)
            syntheticTypes.emplace(table->indexer->indexType);
        return true;
    }

    bool visit(Luau::AstTypeUnion* unionType) override
    {
        // If the union contains a "nil" part, but it has the the size of only 1 column, then it is synthetic
        // Note that the union could contain > 2 parts:
        //  T? -> T | nil
        //  U | V? -> U | V | nil
        for (const auto& ty : unionType->types)
            if (auto ref = ty->as<Luau::AstTypeReference>())
                if (!ref->prefix && !ref->hasParameterList && ref->name == "nil" && ref->location.end.column == ref->location.begin.column + 1)
                    syntheticTypes.emplace(ref);
        return true;
    }


    bool visit(Luau::AstType* type) override
    {
        return true;
    }

    bool visit(Luau::AstTypeReference* ref) override
    {
        // Ignore synthetic type references
        if (syntheticTypes.find(ref) != syntheticTypes.end())
            return false;

        // Highlight the prefix and name by their own locations, so the punctuation between them
        // (Module.Type<Param> should not highlight . < >) stays uncoloured.
        // Luwu Destructuring: `type .{A} = module` desugars to `type A = module.A`, whose reference sits on the key
        // with the prefix written after it. That prefix is the `= module` value, an ordinary name, so leave it be.
        if (ref->prefix && ref->prefixLocation && ref->prefixLocation->begin < ref->nameLocation.begin)
            tokens.emplace_back(SemanticToken{
                ref->prefixLocation->begin, ref->prefixLocation->end, lsp::SemanticTokenTypes::Namespace, lsp::SemanticTokenModifiers::None});

        // Highlight name -- as a class when the annotation names one, so `userid: UserId` colors
        // `UserId` the same way its `class UserId` declaration is colored
        auto tokenType = lsp::SemanticTokenTypes::Type;
        if (auto resolvedTy = module->astResolvedTypes.find(ref); resolvedTy && namesTrait(*resolvedTy))
            tokenType = lsp::SemanticTokenTypes::Interface;
        else if (resolvedTy && namesUserDefinedClass(*resolvedTy, builtinTypes))
            tokenType = lsp::SemanticTokenTypes::Class;

        // A reference the parser made up sits somewhere its name isn't written (the `any` of an untyped `declare
        // function` parameter, the error alias of an empty `type .{} = m`), so only a name spanning exactly its
        // own text is coloured.
        const Luau::Location& nameLocation = ref->nameLocation;
        if (nameLocation.begin.line == nameLocation.end.line && nameLocation.end.column - nameLocation.begin.column == strlen(ref->name.value))
            tokens.emplace_back(SemanticToken{nameLocation.begin, nameLocation.end, tokenType, lsp::SemanticTokenModifiers::None});

        // Do not highlight parameters as they will be visited later
        return true;
    }

    bool visit(Luau::AstTypePack* type) override
    {
        return true;
    }

    bool visit(Luau::AstTypePackGeneric* type) override
    {
        // HACK: do not highlight punctuation
        Luau::Position endPosition{
            type->location.begin.line, type->location.begin.column + static_cast<unsigned int>(strlen(type->genericName.value))};
        tokens.emplace_back(SemanticToken{type->location.begin, endPosition, lsp::SemanticTokenTypes::Namespace, lsp::SemanticTokenModifiers::None});
        return true;
    }

    bool visit(Luau::AstStatLocal* local) override
    {
        auto scope = Luau::findScopeAtPosition(*module, local->location.begin);
        if (!scope)
            return true;

        for (auto var : local->vars)
        {
            // Luwu Destructuring: an unnamed pattern's value is bound to a hidden local spanning the whole `.{...}`
            if (strcmp(var->name.value, Luau::kDestructuredLocalName) == 0)
                continue;

            auto ty = scope->lookup(var);
            if (ty)
            {
                auto type = inferTokenType(*ty, lsp::SemanticTokenTypes::Variable, builtinTypes);
                if (type == lsp::SemanticTokenTypes::Variable)
                    continue; // No special semantic token needed, fall back to syntax highlighting
                tokens.emplace_back(SemanticToken{var->location.begin, var->location.end, type, lsp::SemanticTokenModifiers::None});
            }
        }

        if (local->destructure)
            addDestructuredKeyTokens(*local->destructure);

        // Luwu Destructuring: a field desugars to `h1 = (destructured).h1`, whose made-up read sits on the key; the
        // bound name's own token is the only one it gets
        return !local->isDesugaredField();
    }

    bool visit(Luau::AstStatForIn* forIn) override
    {
        for (const auto* pattern : forIn->destructure)
            if (pattern)
                addDestructuredKeyTokens(*pattern);
        return true;
    }

    // Luwu Destructuring: the key of `h1 as H1` or `tar as .{gz}` is only the field being read, and what follows the
    // `as` is what gets bound. The key is coloured as a property, like the `t.h1` it reads, so it isn't mistaken for
    // a local. A key without `as` is the binding itself and gets the bound local's token.
    void addDestructuredKeyTokens(const Luau::AstDestructurePattern& pattern)
    {
        for (const auto& field : pattern.fields)
        {
            if (field.asLocation && field.keyLocation.begin != field.keyLocation.end)
                tokens.emplace_back(SemanticToken{
                    field.keyLocation.begin, field.keyLocation.end, lsp::SemanticTokenTypes::Property, lsp::SemanticTokenModifiers::None});
            addDestructuredKeyTokens(field.target);
        }
    }

    // bool visit(Luau::AstStatFunction* func) override
    // {
    //     tokens.emplace_back(SemanticToken{
    //         func->name->location.begin, func->name->location.end, lsp::SemanticTokenTypes::Function, lsp::SemanticTokenModifiers::None});
    //     return true;
    // }

    // bool visit(Luau::AstStatLocalFunction* func) override
    // {
    //     tokens.emplace_back(SemanticToken{
    //         func->name->location.begin, func->name->location.end, lsp::SemanticTokenTypes::Function, lsp::SemanticTokenModifiers::None});
    //     return true;
    // }

    bool visit(Luau::AstExprFunction* func) override
    {
        if (func->self)
            localMap.insert_or_assign(func->self, AstLocalInfo::Self);

        bool first = true;
        for (auto arg : func->args)
        {
            if (first && !func->self && arg->name == "self")
            {
                tokens.emplace_back(SemanticToken{
                    arg->location.begin, arg->location.end, lsp::SemanticTokenTypes::Property, lsp::SemanticTokenModifiers::DefaultLibrary});
                localMap.insert_or_assign(arg, AstLocalInfo::Self);
            }
            else
            {
                tokens.emplace_back(
                    SemanticToken{arg->location.begin, arg->location.end, lsp::SemanticTokenTypes::Parameter, lsp::SemanticTokenModifiers::None});
                localMap.insert_or_assign(arg, AstLocalInfo::Parameter);
            }

            first = false;
        }
        return true;
    }

    // Luwu Classes: a class body may restate a primary constructor parameter to give it a type or a
    // visibility -- `class Cat(name, kind) public name; private kind: number end`. That field *is* the
    // parameter, so colour it like one; a field the constructor knows nothing about (`public age = 0`)
    // stays a property. The AST records no link between the two, so they are matched by name, the same
    // way Hover and CodeAction do it.
    bool visit(Luau::AstStatClass* klass) override
    {
        if (!klass->primaryConstructor)
            return true;

        for (const auto& member : klass->members)
        {
            const auto* prop = member.get_if<Luau::AstClassProperty>();
            if (!prop)
                continue;

            for (const auto* arg : klass->primaryConstructor->args)
            {
                if (arg->name == prop->name)
                {
                    tokens.emplace_back(SemanticToken{prop->nameLocation.begin, prop->nameLocation.end,
                        lsp::SemanticTokenTypes::Parameter, lsp::SemanticTokenModifiers::None});
                    break;
                }
            }
        }

        return true;
    }

    // Luwu Declare Statements: a declared class's shape is not visited as a class, and its methods are signatures, so
    // their parameters and restated fields are coloured from here. Its type annotations are visited as usual.
    bool visit(Luau::AstStatDeclareClass* declared) override
    {
        for (const auto& member : declared->shape->members)
        {
            if (const auto* method = member.get_if<Luau::AstClassMethod>())
                visit(method->function);
        }

        visit(declared->shape);
        return true;
    }

    bool visit(Luau::AstExprLocal* local) override
    {
        auto defaultType = lsp::SemanticTokenTypes::Variable;
        if (localMap.find(local->local) != localMap.end())
        {
            auto localInfo = localMap.at(local->local);
            if (localInfo == AstLocalInfo::Self)
            {
                tokens.emplace_back(SemanticToken{
                    local->location.begin, local->location.end, lsp::SemanticTokenTypes::Property, lsp::SemanticTokenModifiers::DefaultLibrary});

                return true;
            }
            else if (localInfo == AstLocalInfo::Parameter)
            {
                defaultType = lsp::SemanticTokenTypes::Parameter;
            }
        }

        auto type = defaultType;
        if (auto ty = module->astTypes.find(local))
            type = inferTokenType(*ty, defaultType, builtinTypes);

        if (type == lsp::SemanticTokenTypes::Variable)
            return true;

        tokens.emplace_back(SemanticToken{local->location.begin, local->location.end, type, lsp::SemanticTokenModifiers::None});
        return true;
    }

    // A builtin global's token: `name` is one of `builtinGlobals`, and `ty` its type
    void addBuiltinGlobalToken(Luau::AstName name, const Luau::Location& location, Luau::TypeId ty)
    {
        // SPECIAL CASE: if name is "Enum", classify it as an enum
        if (name == "Enum")
        {
            tokens.emplace_back(SemanticToken{location.begin, location.end, lsp::SemanticTokenTypes::Enum, lsp::SemanticTokenModifiers::DefaultLibrary});
        }
        // If it starts with an uppercase letter, flag it as a class
        // Otherwise, flag it as a builtin
        else if (isupper(name.value[0]))
        {
            tokens.emplace_back(SemanticToken{location.begin, location.end, lsp::SemanticTokenTypes::Class, lsp::SemanticTokenModifiers::DefaultLibrary});
        }
        else
        {
            auto type = inferTokenType(ty, lsp::SemanticTokenTypes::Variable, builtinTypes);
            tokens.emplace_back(
                SemanticToken{location.begin, location.end, type, lsp::SemanticTokenModifiers::DefaultLibrary | lsp::SemanticTokenModifiers::Readonly});
        }
    }

    bool visit(Luau::AstExprGlobal* global) override
    {
        auto it = builtinGlobals.find(global->name);
        if (it != builtinGlobals.end() && strlen(global->name.value) > 0)
        {
            // The Luwu grammar scopes `none` like `nil`; a variable token would override that
            if (grammarHighlightsNone && global->name == "none")
                return true;

            addBuiltinGlobalToken(global->name, global->location, it->second);
        }
        else
        {
            auto ty = module->astTypes.find(global);
            if (!ty)
                return true;

            auto type = inferTokenType(*ty, lsp::SemanticTokenTypes::Variable, builtinTypes);
            if (type == lsp::SemanticTokenTypes::Variable)
                return true;

            tokens.emplace_back(SemanticToken{global->location.begin, global->location.end, type, lsp::SemanticTokenModifiers::None});
        }

        return true;
    }

    bool visit(Luau::AstExprIndexName* index) override
    {
        auto parentTy = module->astTypes.find(index->expr);
        if (!parentTy)
            return true;

        auto parentIsBuiltin = false;
        auto parentIsEnum = false;
        if (auto global = index->expr->as<Luau::AstExprGlobal>())
        {
            parentIsBuiltin = builtinGlobals.find(global->name) != builtinGlobals.end();
            if (parentIsBuiltin && global->name == "Enum")
                parentIsEnum = true;
        }

        auto ty = Luau::follow(*parentTy);
        if (auto propInformation = lookupProp(ty, std::string(index->index.value)); propInformation.size() == 1 && propInformation[0].property.readTy)
        {
            auto prop = propInformation[0].property;

            auto defaultType = lsp::SemanticTokenTypes::Property;
            if (parentIsEnum)
                defaultType = lsp::SemanticTokenTypes::Enum;
            else if (Luau::hasTag(prop.tags, "EnumItem"))
                defaultType = lsp::SemanticTokenTypes::EnumMember;

            auto type = inferTokenType(*prop.readTy, defaultType, builtinTypes);
            auto modifiers = lsp::SemanticTokenModifiers::None;
            if (parentIsBuiltin)
            {
                modifiers = modifiers | lsp::SemanticTokenModifiers::DefaultLibrary | lsp::SemanticTokenModifiers::Readonly;
            }
            else if (types::isMetamethod(std::string(index->index.value)))
            {
                modifiers = modifiers | lsp::SemanticTokenModifiers::DefaultLibrary;
            }
            tokens.emplace_back(SemanticToken{index->indexLocation.begin, index->indexLocation.end, type, modifiers});
        }

        return true;
    }

    bool visit(Luau::AstExprTable* tbl) override
    {
        for (const auto& item : tbl->items)
        {
            if (item.kind == Luau::AstExprTable::Item::Kind::Record)
            {
                if (auto ty = module->astTypes.find(item.value))
                {
                    auto type = inferTokenType(*ty, lsp::SemanticTokenTypes::Property, builtinTypes);
                    tokens.emplace_back(SemanticToken{item.key->location.begin, item.key->location.end, type, lsp::SemanticTokenModifiers::None});
                }
            }
        }

        return true;
    }

    // Luwu If-Local, Do Expressions and Table Comprehensions: `when` and `give` are contextual keywords, so the
    // grammar only highlights one when the next token is on the same line. The AST knows exactly where they are,
    // which also covers a `when`/`give` that ends its line without catching a variable named `give` or `when`
    void addKeywordToken(const Luau::Location& location, unsigned int length)
    {
        // A missing `give` leaves its location on whatever token stood there instead
        if (location.begin.line != location.end.line || location.end.column - location.begin.column != length)
            return;
        tokens.emplace_back(SemanticToken{location.begin, location.end, lsp::SemanticTokenTypes::Keyword, lsp::SemanticTokenModifiers::None});
    }

    bool visit(Luau::AstStatIf* statIf) override
    {
        for (const auto& clause : statIf->clauses)
            if (clause.whenLocation)
                addKeywordToken(*clause.whenLocation, 4);

        return true;
    }

    bool visit(Luau::AstExprIfElse* ifElse) override
    {
        for (const auto& clause : ifElse->clauses)
            if (clause.whenLocation)
                addKeywordToken(*clause.whenLocation, 4);

        return true;
    }

    bool visit(Luau::AstStatGive* give) override
    {
        Luau::Position keywordEnd{give->location.begin.line, give->location.begin.column + 4};
        addKeywordToken(Luau::Location{give->location.begin, keywordEnd}, 4);
        return true;
    }

    bool visit(Luau::AstExprTableComprehension* comprehension) override
    {
        // The comprehension is stored as the loops it runs, each one's body holding only the next clause. A single
        // plain `when` is an ordinary if whose `if` is that `when`; a chain records its `when`s in its clauses,
        // which visit(AstStatIf) covers. Every loop's `doLocation` is the one `give`, or the `do` written in place of
        // `give do`. That `do` is a reserved word, which the length check in addKeywordToken leaves to the grammar.
        std::optional<Luau::Location> giveLocation;
        Luau::AstStat* stat = comprehension->loop;
        while (stat)
        {
            Luau::AstStatBlock* body = nullptr;
            if (auto* statFor = stat->as<Luau::AstStatFor>())
            {
                giveLocation = statFor->doLocation;
                body = statFor->body;
            }
            else if (auto* statForIn = stat->as<Luau::AstStatForIn>())
            {
                giveLocation = statForIn->doLocation;
                body = statForIn->body;
            }
            else if (auto* guard = stat->as<Luau::AstStatIf>())
            {
                if (guard->clauses.size == 0)
                    addKeywordToken(guard->ifLocation, 4);
                body = guard->thenbody;
            }

            stat = body && body->body.size == 1 ? body->body.data[0] : nullptr;
        }

        if (giveLocation)
            addKeywordToken(*giveLocation, 4);

        return true;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* stat : block->body)
        {
            stat->visit(this);
        }

        return false;
    }
};

// Luwu: Moonwave's tags, as luwu-lsp's hover reads them (printMoonwaveDocumentation)
static const std::unordered_set<std::string_view> kMoonwaveTags{
    "param", "return", "error", "field", "private", "yields", "unreleased", "server", "client", "plugin", "readonly", "deprecated", "since",
    "ignore", "tag", "within", "class", "function", "method", "prop", "interface", "type", "__index", "external",
};

// Luwu: a Moonwave tag starting a doc line (`--- @param name ...`, or a line of a block comment), coloured as a decorator,
// and the name a `@param`/`@field`/`@prop` names. Luau files only, with `documentation.highlightMoonwaveTags` on (the
// default); the grammar doesn't colour `@`, so a tag anywhere else (`@std/fs` in prose) is left as comment text.
void addMoonwaveTagTokens(std::vector<SemanticToken>& tokens, const Luau::SourceModule* sourceModule, const TextDocument* textDocument)
{
    auto isWordChar = [](char c)
    {
        return isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    for (const Luau::Comment& comment : sourceModule->commentLocations)
    {
        bool block = comment.type == Luau::Lexeme::BlockComment;
        if (!block && comment.type != Luau::Lexeme::Comment)
            continue;

        std::string text = textDocument->getText(
            lsp::Range{textDocument->convertPosition(comment.location.begin), textDocument->convertPosition(comment.location.end)}
        );
        bool docLine = Luau::startsWith(text, "---") && !Luau::startsWith(text, "----");
        if (!block && !docLine)
            continue;

        std::vector<std::string_view> lines = Luau::split(text, '\n');
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string_view line = lines[i];

            // past the comment's opener on its first line: `---`, or `--[==[`
            size_t at = 0;
            if (i == 0)
            {
                size_t opener = block ? line.find('[', 3) : 2;
                at = opener == std::string_view::npos ? line.size() : opener + 1;
            }

            while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
                ++at;
            if (at >= line.size() || line[at] != '@')
                continue;

            size_t tagEnd = at + 1;
            while (tagEnd < line.size() && isWordChar(line[tagEnd]))
                ++tagEnd;
            std::string_view tag = line.substr(at + 1, tagEnd - at - 1);
            if (!kMoonwaveTags.count(tag))
                continue;

            unsigned lineNumber = comment.location.begin.line + unsigned(i);
            unsigned column = i == 0 ? comment.location.begin.column : 0;
            tokens.emplace_back(SemanticToken{
                Luau::Position{lineNumber, column + unsigned(at)},
                Luau::Position{lineNumber, column + unsigned(tagEnd)},
                lsp::SemanticTokenTypes::Decorator,
                lsp::SemanticTokenModifiers::None
            });

            bool namesSomething = tag == "param" || tag == "field" || tag == "prop";
            size_t nameStart = tagEnd;
            while (nameStart < line.size() && (line[nameStart] == ' ' || line[nameStart] == '\t'))
                ++nameStart;
            size_t nameEnd = nameStart;
            while (nameEnd < line.size() && isWordChar(line[nameEnd]))
                ++nameEnd;
            if (!namesSomething || nameEnd == nameStart)
                continue;

            tokens.emplace_back(SemanticToken{
                Luau::Position{lineNumber, column + unsigned(nameStart)},
                Luau::Position{lineNumber, column + unsigned(nameEnd)},
                tag == "param" ? lsp::SemanticTokenTypes::Parameter : lsp::SemanticTokenTypes::Property,
                lsp::SemanticTokenModifiers::None
            });
        }
    }
}

// Luwu doc references: the code in a doc comment (`[str.startswith]`, `[Parser:advance]`, `[type Options]`,
// `[json = require("@std/json")]`) is coloured like the same code outside one, resolved the way Luwu's DocChecker
// resolves it. What the docref parser didn't take as code (markdown links, `[optional]` prose) stays comment text.
struct DocRefTokens
{
    SemanticTokensVisitor& visitor;
    const Luau::Frontend& frontend;
    const Luau::SourceModule& sourceModule;

    using Bindings = std::unordered_map<std::string, Luau::AstExpr*>;
    Bindings moduleBindings;

    // What one doc comment's code can name
    struct Names
    {
        Luau::ScopePtr scope;
        Bindings own;
    };

    // A binding that names another is resolved through it; this bounds `[a = b]` `[b = a]`
    static constexpr int kMaxDepth = 16;

    void run()
    {
        for (const Luau::DocComment& doc : sourceModule.docComments)
        {
            if (doc.moduleDoc)
            {
                for (const Luau::DocBinding& binding : doc.bindings)
                    moduleBindings[binding.name.value] = binding.value;
            }
        }

        for (const Luau::DocComment& doc : sourceModule.docComments)
        {
            Names names{scopeFor(doc), {}};
            if (!doc.moduleDoc)
            {
                for (const Luau::DocBinding& binding : doc.bindings)
                    names.own[binding.name.value] = binding.value;
            }

            for (const Luau::DocBinding& binding : doc.bindings)
            {
                size_t first = visitor.tokens.size();
                auto type = lsp::SemanticTokenTypes::Variable;
                if (auto ty = typeOf(binding.value, names, 0))
                    type = inferTokenType(*ty, type, visitor.builtinTypes);
                visitor.tokens.emplace_back(
                    SemanticToken{binding.nameLocation.begin, binding.nameLocation.end, type, lsp::SemanticTokenModifiers::Declaration});
                addExprTokens(binding.value, names);
                finishSpan(first, Luau::Location{binding.nameLocation.begin, binding.value->location.end});
            }

            for (const Luau::DocRef& ref : doc.refs)
            {
                size_t first = visitor.tokens.size();
                if (ref.type)
                {
                    addTypeTokens(ref, names);
                    finishSpan(first, ref.location);
                    continue;
                }

                addExprTokens(ref.expr, names);
                if (ref.method)
                {
                    auto type = lsp::SemanticTokenTypes::Method;
                    if (auto ty = memberType(typeOf(ref.expr, names, 0), ref.method->value))
                        type = inferTokenType(*ty, type, visitor.builtinTypes);
                    visitor.tokens.emplace_back(
                        SemanticToken{ref.methodLocation.begin, ref.methodLocation.end, type, lsp::SemanticTokenModifiers::None});
                }
                finishSpan(first, ref.location);
            }
        }
    }

    // The tokens from `first` on are the code in `[...]`, `code` being what's inside the brackets. They're marked
    // `documentation`, which the extension un-italicises (a comment's italics outlast a semantic token's colour), and
    // the brackets and what's between the tokens (`.`, `:`, parentheses) become punctuation, so the whole `[...]`
    // reads as one piece of code rather than code with comment bits in it.
    void finishSpan(size_t first, const Luau::Location& code)
    {
        std::vector<std::pair<unsigned, unsigned>> covered;
        for (size_t i = first; i < visitor.tokens.size(); ++i)
        {
            SemanticToken& token = visitor.tokens[i];
            token.tokenModifiers = token.tokenModifiers | lsp::SemanticTokenModifiers::Documentation;
            if (token.start.line == code.begin.line && token.end.line == code.begin.line)
                covered.emplace_back(token.start.column, token.end.column);
        }

        // a doc reference is on one line
        if (code.begin.line != code.end.line || code.begin.column == 0)
            return;

        std::sort(covered.begin(), covered.end());
        unsigned line = code.begin.line;
        unsigned at = code.begin.column - 1;
        // past the `]`
        unsigned end = code.end.column + 1;
        auto addPunctuation = [&](unsigned from, unsigned to)
        {
            if (from < to)
                visitor.tokens.emplace_back(SemanticToken{
                    Luau::Position{line, from}, Luau::Position{line, to}, lsp::SemanticTokenTypes::Punctuation,
                    lsp::SemanticTokenModifiers::Documentation});
        };
        for (const auto& [begin, tokenEnd] : covered)
        {
            addPunctuation(at, std::min(begin, end));
            at = std::max(at, tokenEnd);
        }
        addPunctuation(at, end);
    }

    Luau::ScopePtr scopeFor(const Luau::DocComment& doc) const
    {
        if (doc.functionBody)
        {
            if (auto scope = Luau::findScopeAtPosition(*visitor.module, *doc.functionBody))
                return scope;
        }
        if (auto scope = Luau::findScopeAtPosition(*visitor.module, doc.location.begin))
            return scope;
        return visitor.module->getModuleScope();
    }

    Luau::AstExpr* boundValue(const std::string& name, const Names& names) const
    {
        if (auto it = names.own.find(name); it != names.own.end())
            return it->second;
        if (auto it = moduleBindings.find(name); it != moduleBindings.end())
            return it->second;
        return nullptr;
    }

    static std::optional<Luau::TypeId> memberType(std::optional<Luau::TypeId> parent, const char* name)
    {
        if (!parent)
            return std::nullopt;
        auto props = lookupProp(Luau::follow(*parent), name);
        if (props.size() != 1 || !props[0].property.readTy)
            return std::nullopt;
        return Luau::follow(*props[0].property.readTy);
    }

    // `[@std/str]` stands for `require("@std/str")`, a call the parser made up: all of it is the path
    static bool isRequirePath(const Luau::AstExprCall* call)
    {
        return call->func->location == call->location;
    }

    std::optional<Luau::TypeId> requireType(const Luau::AstExprCall* call) const
    {
        if (call->args.size != 1 || !call->args.data[0]->is<Luau::AstExprConstantString>())
            return std::nullopt;
        Luau::ModuleInfo context{sourceModule.name};
        Luau::TypeCheckLimits limits;
        auto info = frontend.fileResolver->resolveModule(&context, call->args.data[0], limits);
        if (!info)
            return std::nullopt;
        auto required = frontend.moduleResolverForAutocomplete.getModule(info->name);
        if (!required || !required->returnType)
            return std::nullopt;
        return Luau::first(required->returnType);
    }

    std::optional<Luau::TypeId> typeOf(Luau::AstExpr* expr, const Names& names, int depth) const
    {
        if (depth > kMaxDepth)
            return std::nullopt;
        if (auto global = expr->as<Luau::AstExprGlobal>())
        {
            if (auto value = boundValue(global->name.value, names))
                return typeOf(value, names, depth + 1);
            if (auto binding = names.scope->linearSearchForBindingPair(global->name.value, true))
                return Luau::follow(binding->second.typeId);
            return std::nullopt;
        }
        if (auto index = expr->as<Luau::AstExprIndexName>())
            return memberType(typeOf(index->expr, names, depth + 1), index->index.value);
        if (auto call = expr->as<Luau::AstExprCall>())
        {
            auto func = call->func->as<Luau::AstExprGlobal>();
            if (func && func->name == "require")
                return requireType(call);
            return std::nullopt;
        }
        if (auto group = expr->as<Luau::AstExprGroup>())
            return typeOf(group->expr, names, depth + 1);
        return std::nullopt;
    }

    void addToken(const Luau::Location& location, lsp::SemanticTokenTypes type, lsp::SemanticTokenModifiers modifiers = lsp::SemanticTokenModifiers::None)
    {
        visitor.tokens.emplace_back(SemanticToken{location.begin, location.end, type, modifiers});
    }

    void addNameToken(Luau::AstExprGlobal* global, const Names& names)
    {
        if (auto value = boundValue(global->name.value, names))
        {
            auto type = lsp::SemanticTokenTypes::Variable;
            if (auto ty = typeOf(value, names, 1))
                type = inferTokenType(*ty, type, visitor.builtinTypes);
            addToken(global->location, type);
            return;
        }

        if (auto binding = names.scope->linearSearchForBindingPair(global->name.value, true))
        {
            Luau::AstLocal* local = binding->first.local;
            if (!local)
            {
                if (auto it = visitor.builtinGlobals.find(global->name); it != visitor.builtinGlobals.end())
                {
                    visitor.addBuiltinGlobalToken(global->name, global->location, it->second);
                    return;
                }
            }

            auto base = lsp::SemanticTokenTypes::Variable;
            if (auto it = visitor.localMap.find(local); local && it != visitor.localMap.end())
            {
                if (it->second == AstLocalInfo::Self)
                {
                    addToken(global->location, lsp::SemanticTokenTypes::Property, lsp::SemanticTokenModifiers::DefaultLibrary);
                    return;
                }
                base = lsp::SemanticTokenTypes::Parameter;
            }
            addToken(global->location, inferTokenType(binding->second.typeId, base, visitor.builtinTypes));
            return;
        }

        // a type name is a reference too: `[Pathlike]`
        if (auto typeFun = names.scope->lookupType(global->name.value))
        {
            addToken(global->location, typeNameTokenType(typeFun->type));
            return;
        }

        addToken(global->location, lsp::SemanticTokenTypes::Variable);
    }

    lsp::SemanticTokenTypes typeNameTokenType(Luau::TypeId ty) const
    {
        if (namesTrait(ty))
            return lsp::SemanticTokenTypes::Interface;
        if (namesUserDefinedClass(ty, visitor.builtinTypes))
            return lsp::SemanticTokenTypes::Class;
        return lsp::SemanticTokenTypes::Type;
    }

    void addExprTokens(Luau::AstExpr* expr, const Names& names)
    {
        struct Walker : Luau::AstVisitor
        {
            DocRefTokens& self;
            const Names& names;

            Walker(DocRefTokens& self, const Names& names)
                : self(self)
                , names(names)
            {
            }

            bool visit(Luau::AstExprGlobal* global) override
            {
                self.addNameToken(global, names);
                return false;
            }

            bool visit(Luau::AstExprIndexName* index) override
            {
                auto parent = self.typeOf(index->expr, names, 0);
                auto type = lsp::SemanticTokenTypes::Property;
                auto modifiers = lsp::SemanticTokenModifiers::None;
                if (auto ty = memberType(parent, index->index.value))
                    type = inferTokenType(*ty, type, self.visitor.builtinTypes);
                if (auto global = index->expr->as<Luau::AstExprGlobal>(); global && !self.boundValue(global->name.value, names))
                {
                    auto binding = names.scope->linearSearchForBindingPair(global->name.value, true);
                    if ((!binding || !binding->first.local) && self.visitor.builtinGlobals.count(global->name))
                        modifiers = lsp::SemanticTokenModifiers::DefaultLibrary | lsp::SemanticTokenModifiers::Readonly;
                }
                self.addToken(index->indexLocation, type, modifiers);
                return true;
            }

            bool visit(Luau::AstExprCall* call) override
            {
                if (isRequirePath(call))
                {
                    self.addToken(call->location, lsp::SemanticTokenTypes::String);
                    return false;
                }
                return true;
            }

            bool visit(Luau::AstExprConstantString* string) override
            {
                self.addToken(string->location, lsp::SemanticTokenTypes::String);
                return false;
            }

            bool visit(Luau::AstExprConstantNumber* number) override
            {
                self.addToken(number->location, lsp::SemanticTokenTypes::Number);
                return false;
            }
        };

        Walker walker{*this, names};
        expr->visit(&walker);
    }

    // `[type T]`, `[type mod.T]`
    void addTypeTokens(const Luau::DocRef& ref, const Names& names)
    {
        auto reference = ref.type->as<Luau::AstTypeReference>();
        if (!reference)
            return;

        Luau::Position keywordEnd{ref.location.begin.line, ref.location.begin.column + 4};
        addToken(Luau::Location{ref.location.begin, keywordEnd}, lsp::SemanticTokenTypes::Keyword);

        std::optional<Luau::TypeFun> typeFun;
        if (reference->prefix)
        {
            if (reference->prefixLocation)
                addToken(*reference->prefixLocation, lsp::SemanticTokenTypes::Namespace);
            typeFun = names.scope->lookupImportedType(reference->prefix->value, reference->name.value);
        }
        else
            typeFun = names.scope->lookupType(reference->name.value);

        addToken(reference->nameLocation, typeFun ? typeNameTokenType(typeFun->type) : lsp::SemanticTokenTypes::Type);
    }
};

std::vector<SemanticToken> getSemanticTokens(
    const Luau::Frontend& frontend, const Luau::ModulePtr& module, const Luau::SourceModule* sourceModule, bool grammarHighlightsNone)
{
    std::unordered_map<Luau::AstName, Luau::TypeId> builtinGlobals{};
    fillBuiltinGlobals(builtinGlobals, *sourceModule->names, frontend.globals.globalScope);

    SemanticTokensVisitor visitor{module, builtinGlobals, frontend.builtinTypes, grammarHighlightsNone};
    visitor.visit(sourceModule->root);

    DocRefTokens docRefs{visitor, frontend, *sourceModule};
    docRefs.run();

    return visitor.tokens;
}

size_t convertTokenType(const lsp::SemanticTokenTypes tokenType)
{
    return static_cast<size_t>(tokenType);
}

std::vector<size_t> packTokens(const TextDocument* textDocument, std::vector<SemanticToken>& tokens)
{
    // Sort the tokens into the correct ordering
    std::sort(tokens.begin(), tokens.end(),
        [](const SemanticToken& a, const SemanticToken& b)
        {
            return a.start < b.start;
        });

    // Pack the tokens
    std::vector<size_t> result{};
    result.reserve(tokens.size() * 5); // Each token will take up 5 slots in the result

    size_t lastLine = 0;
    size_t lastChar = 0;

    for (auto& token : tokens)
    {
        auto start = textDocument->convertPosition(token.start);
        auto end = textDocument->convertPosition(token.end);

        auto line = start.line;
        auto startChar = start.character;

        auto deltaLine = line - lastLine;
        auto deltaStartChar = deltaLine == 0 ? startChar - lastChar : startChar;
        auto length = end.character - start.character;

        result.insert(
            result.end(), {deltaLine, deltaStartChar, length, convertTokenType(token.tokenType), static_cast<size_t>(token.tokenModifiers)});

        lastLine = line;
        lastChar = startChar;
    }

    return result;
}

std::optional<lsp::SemanticTokens> WorkspaceFolder::semanticTokens(
    const lsp::SemanticTokensParams& params, const LSPCancellationToken& cancellationToken)
{
    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());

    // Run the type checker to ensure we are up to date
    // TODO: this relies on the autocomplete typechecker, which we don't really need for semantic tokens
    checkStrict(moduleName, cancellationToken);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    auto module = getModule(moduleName, /* forAutocomplete: */ true);
    if (!sourceModule || !module)
        return std::nullopt;

    auto tokens = getSemanticTokens(frontend, module, sourceModule, /* grammarHighlightsNone= */ isLuwuFile(*textDocument));
    // Luwu: Moonwave can't parse Luwu, so its tags are only coloured in Luau files
    if (!isLuwuFile(*textDocument) && client->getConfiguration(rootUri).documentation.highlightMoonwaveTags)
        addMoonwaveTagTokens(tokens, sourceModule, textDocument);
    lsp::SemanticTokens result;
    result.data = packTokens(textDocument, tokens);
    return result;
}
