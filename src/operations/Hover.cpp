#include "LSP/Workspace.hpp"

#include <algorithm>

#include "Luau/AstQuery.h"
#include "Luau/Module.h"
#include "Luau/ToString.h"
#include "LSP/LuauExt.hpp"
#include "LSP/DocumentationParser.hpp"
#include "LSP/KeywordHovers.hpp"

// Lifted from lutf8lib.cpp
/*
** Decode one UTF-8 sequence, returning NULL if byte sequence is invalid.
*/
static const char* utf8_decode(const char* o, int* val)
{
    static const unsigned int limits[] = {0xFF, 0x7F, 0x7FF, 0xFFFF};
    const unsigned char* s = (const unsigned char*)o;
    unsigned int c = s[0];
    unsigned int res = 0; // final result
    if (c < 0x80)         // ascii?
        res = c;
    else
    {
        int count = 0; // to count number of continuation bytes
        while (c & 0x40)
        {                                   // still have continuation bytes?
            int cc = s[++count];            // read next byte
            if ((cc & 0xC0) != 0x80)        // not a continuation byte?
                return NULL;                // invalid byte sequence
            res = (res << 6) | (cc & 0x3F); // add lower 6 bits from cont. byte
            c <<= 1;                        // to test next bit
        }
        res |= ((c & 0x7F) << (count * 5)); // add first byte
        if (count > 3 || res > 0x10FFFF || res <= limits[count])
            return NULL; // invalid byte sequence
        if (unsigned(res - 0xD800) < 0x800)
            return NULL; // surrogate
        s += count;      // skip continuation bytes read
    }
    if (val)
        *val = res;
    return (const char*)s + 1; // +1 to include first byte
}

static std::optional<size_t> utflen(const char* s, size_t len)
{
    size_t n = 0;
    size_t posi = 0;
    while (posi < len)
    {
        const char* s1 = utf8_decode(s + posi, NULL);
        if (s1 == NULL)
        {
            return std::nullopt;
        }
        posi = (int)(s1 - s);
        n++;
    }
    return n;
}

/// Construct the initial type description from a typeFun, i.e. Foo<T>
static std::string toStringTypeFun(const std::string typeName, const Luau::TypeFun& typeFun)
{
    std::string output = typeName;
    if (!typeFun.typeParams.empty() || !typeFun.typePackParams.empty())
    {
        output += "<";
        bool addComma = false;
        for (const auto& typeParam : typeFun.typeParams)
        {
            if (addComma)
                output += ", ";
            output += Luau::toString(Luau::follow(typeParam.ty));
            if (typeParam.defaultValue)
            {
                output += " = " + Luau::toString(Luau::follow(typeParam.defaultValue.value()));
            }
            addComma = true;
        }
        for (const auto& typePack : typeFun.typePackParams)
        {
            if (addComma)
                output += ", ";
            output += Luau::toString(Luau::follow(typePack.tp));
            if (typePack.defaultValue)
            {
                output += " = " + Luau::toString(Luau::follow(typePack.defaultValue.value()));
            }
            addComma = true;
        }
        output += ">";
    }
    return output;
}


struct DocumentationLocation
{
    Luau::ModuleName moduleName;
    Luau::Location location;
};

// Visitor to find a class statement by name (used to build a hover summary for class values)
struct FindClassStatByName : Luau::AstVisitor
{
    std::string_view targetName;
    Luau::AstStatClass* result = nullptr;

    explicit FindClassStatByName(std::string_view name)
        : targetName(name)
    {
    }

    bool visit(Luau::AstStatClass* node) override
    {
        if (!result && node->name->name.value == targetName)
            result = node;
        return true;
    }
};

// How many fields/functions a class or extern type summary lists (per section) before eliding the
// rest. A summary of a type the hover merely *refers to* uses the smaller cap, so several of them
// fit in one hover alongside the hovered type itself.
static constexpr size_t kMaxSummaryMembers = 5;
// A hover over a type from a wide module can pull in a lot of aliases, and everything below the
// expansion -- the documentation in particular -- gets pushed off the screen. Keep the first few
// whole, and let the `References` links above stand in for the rest.
static constexpr size_t kMaxExpandedReferencedTypes = 5;

// Keeps the first `limit` `type X = ...` clauses of a `where` section, each of which may span
// several lines, and replaces the rest with a count.
std::string truncateWhereClauses(const std::string& whereClauses, size_t limit)
{
    size_t kept = 0;
    size_t cutoff = std::string::npos;
    size_t total = 0;

    for (size_t lineStart = 0; lineStart < whereClauses.size();)
    {
        size_t lineEnd = whereClauses.find('\n', lineStart);

        if (whereClauses.compare(lineStart, 5, "type ") == 0)
        {
            total += 1;
            if (kept == limit)
                cutoff = (cutoff == std::string::npos ? lineStart : cutoff);
            else
                kept += 1;
        }

        if (lineEnd == std::string::npos)
            break;
        lineStart = lineEnd + 1;
    }

    if (cutoff == std::string::npos)
        return whereClauses;

    std::string result = whereClauses.substr(0, cutoff);
    while (!result.empty() && result.back() == '\n')
        result.pop_back();

    return result + "\n\n-- ... and " + std::to_string(total - kept) + " more referenced types";
}

static constexpr size_t kMaxReferencedSummaryMembers = 3;

static bool isDunderName(std::string_view name)
{
    return name.rfind("__", 0) == 0;
}

static bool isStaticMethod(const Luau::AstClassMethod* method)
{
    return method->function->args.size == 0 || method->function->args.data[0]->name != "self";
}

// Formats a method as "function name(...): ret", printing `self` bare (no type annotation) if
// present -- the reader already knows the self type from the class/object header above.
//
// `et` is the class/object we're actually hovering over. For a generic class (`class Box<T> ...
// end`), this is the *instantiated* type (e.g. `T` substituted with `string`), while
// `module->astTypes` only ever holds the type as declared, with `T` unsubstituted -- so we prefer
// looking the method up on `et->props` first and only fall back to the AST-inferred type if it's
// not there.
static std::string formatMethodLine(
    const Luau::ModulePtr& module, const Luau::ExternType* et, const Luau::AstClassMethod* method, const Luau::ScopePtr& scope, bool showTableKinds
)
{
    std::optional<Luau::TypeId> fnTy;
    if (auto it = et->props.find(method->functionName.value); it != et->props.end() && it->second.readTy)
        fnTy = it->second.readTy;
    else if (auto astTy = module->astTypes.find(method->function))
        fnTy = *astTy;

    if (!fnTy)
        return "";

    auto ftv = Luau::get<Luau::FunctionType>(Luau::follow(*fnTy));
    if (!ftv)
        return "";

    types::ToStringNamedFunctionOpts funcOpts;
    funcOpts.hideTableKind = !showTableKinds;
    funcOpts.hideFirstParameterType = method->function->args.size > 0 && method->function->args.data[0]->name == "self";
    // Member lines are printed at "    " indentation (see buildClassFieldSummary) -- match that so
    // a long parameter list's continuation lines line up with the "public"/"private" keyword.
    funcOpts.baseIndent = "    ";
    return types::toStringNamedFunction(module, ftv, method->functionName.value, scope, funcOpts);
}

// Extracts just the "(args)" portion from a named-function string like "function (x: number): Foo",
// dropping the leading "function " keyword/name and the trailing ": ReturnType". Used to fold a
// class's constructor signature directly into its header line (`class Foo(x: number)`), matching
// how the class is actually invoked to construct an instance (`Foo(x)`), rather than printing it
// as if it were a member (`Foo(x: number): Foo`) with a redundant, already-implied return type.
static std::string extractArgList(const std::string& namedFunctionString)
{
    auto openParen = namedFunctionString.find('(');
    if (openParen == std::string::npos)
        return "";

    int depth = 0;
    for (size_t i = openParen; i < namedFunctionString.size(); i++)
    {
        if (namedFunctionString[i] == '(')
            depth++;
        else if (namedFunctionString[i] == ')')
        {
            depth--;
            if (depth == 0)
                return namedFunctionString.substr(openParen, i - openParen + 1);
        }
    }
    return "";
}

// Builds a short summary of a class, formatted like a (possibly truncated) class body: the
// constructor folded into the header line (since constructing an instance means literally calling
// the class), then every field, then every function. Fields always precede functions -- a class's
// shape reads better before its behavior -- and each section is truncated independently.
//
// The two callers see different things. The class value's summary is the whole class, private
// members included, plus both static (self-less) functions and the instance methods available on
// objects of it, statics first: it's what you hover while writing the class, from inside the
// lexical scope its privates are reachable in. An object's summary drops the statics (not callable
// on an instance) and the private members (not reachable by whoever is holding the object). E.g.
//
// class Vector2(x: number, y: number)  |  object of Vector2
//     public x: number                 |  class Vector2
//     public y: number                 |      public x: number
//     private scratch: number          |      public y: number
//     public function zero(): Vector2   |      public function add(self, o: Vector2)
//     public function add(self, o)     |      -- ⋯ 2 more members
//     -- ⋯ 2 more members              |  end
// end                                  |
//
// The object case always opens with `class Name ... end` (valid Luau syntax, for highlighting);
// the caller is responsible for prefixing the "object of X" prose label outside the code block.
//
// Works across modules: the summary is built from the AST of whichever module declared the class,
// not the one being hovered in.
// What to call the module a file holds. A module name isn't a file name: the extension isn't part
// of it, and a folder's `init.luau` is the folder's module rather than a module called `init`.
static std::string moduleNameForUri(const lsp::DocumentUri& uri)
{
    std::string name = uri.filename();

    for (const char* extension : {".d.luwu", ".d.luau", ".luwu", ".luau", ".lua"})
    {
        size_t length = strlen(extension);
        if (name.size() > length && name.compare(name.size() - length, length, extension) == 0)
        {
            name.resize(name.size() - length);
            break;
        }
    }

    if (name == "init")
        if (auto parent = uri.parent())
            return parent->filename();

    return name;
}

static std::optional<std::string> buildClassFieldSummary(
    Luau::Frontend& frontend, const Luau::ModulePtr& module, const Luau::ModuleName& moduleName, Luau::TypeId typeId, const Luau::ExternType* et,
    const Luau::ScopePtr& scope, bool showTableKinds, bool isClassValue, size_t maxMembers = kMaxSummaryMembers
)
{
    // Everything this summary is built from -- visibility, `const`, the primary constructor, the
    // declaration order of members -- lives in the AST of whichever module declared the class, not
    // in the one being hovered in. A class reached across a require (`require("./list").List`) is
    // the ordinary case rather than an edge case, so resolve the declaring module instead of giving
    // up whenever it isn't the current one.
    const Luau::ModuleName& definitionModuleName = et->definitionModuleName;
    if (definitionModuleName.empty())
        return std::nullopt;

    auto sourceModule = frontend.getSourceModule(definitionModuleName);
    if (!sourceModule)
        return std::nullopt;

    // Needed only to resolve an annotation the ExternType's own props don't already carry a type
    // for. Those lookups are keyed by AST node, so they have to be made against the declaring
    // module's own checked results -- and that module's type graph may not have been retained, in
    // which case the affected lines fall back to `any` rather than the summary disappearing.
    Luau::ModulePtr definitionModule = definitionModuleName == moduleName ? module : frontend.moduleResolver.getModule(definitionModuleName);

    FindClassStatByName finder(et->name);
    sourceModule->root->visit(&finder);
    if (!finder.result)
        return std::nullopt;

    // Fields and functions are collected separately so the summary can always print every field
    // before any function, regardless of the order they appear in the source. A class's fields are
    // its shape and its functions are its behavior; a reader scanning a hover wants the former
    // first, and interleaving the two (or, worse, floating the static functions above the fields)
    // makes the summary read as an unordered pile of whatever the class happened to declare first.
    std::vector<std::string> fieldLines;
    size_t totalFields = 0;
    // Function lines paired with whether they're static (self-less). Collected together, then
    // ordered statics-first at emission: statics are called on the class value itself, which is
    // what the header line above describes, so they belong nearer to it.
    std::vector<std::pair<std::string, bool>> functionEntries;
    size_t totalFunctions = 0;

    auto pushField = [&](std::string line)
    {
        totalFields++;
        if (fieldLines.size() < maxMembers)
            fieldLines.push_back(std::move(line));
    };

    // Use the type's own toString (rather than the bare `et->name`) so that generic parameters
    // display correctly, e.g. "class Box<number>".
    std::string displayName = Luau::toString(Luau::follow(typeId));

    // ...except the class *value* of a generic class has no generics instantiated to print -- it's
    // the factory, not an instance -- so toString gives a bare `List`, leaving the `{T}` in the
    // header's constructor signature and in the field lines below with nothing to refer back to.
    // Fall back to the parameter names as written on the declaration, so it reads `class List<T>`.
    if (displayName.find('<') == std::string::npos)
    {
        std::string genericParams;
        for (const auto* generic : finder.result->generics)
        {
            if (!genericParams.empty())
                genericParams += ", ";
            genericParams += generic->name.value;
        }
        for (const auto* genericPack : finder.result->genericPacks)
        {
            if (!genericParams.empty())
                genericParams += ", ";
            genericParams += std::string(genericPack->name.value) + "...";
        }
        if (!genericParams.empty())
            displayName += "<" + genericParams + ">";
    }
    // The object case's "object of X" label is prose, not valid Luau syntax, so the caller
    // prepends it outside the code block; the code block itself always opens with valid
    // `class Name<Generics> ... end` syntax so it can be syntax-highlighted properly.
    std::string header = "class " + displayName;
    bool hasConstructorLine = false;

    // Whether the class is constructed by calling it with positional arguments -- which is the
    // case both for a custom `function __init` and for a primary constructor (`class Cat(name:
    // string)`), the latter being just a terser spelling of the former. When neither is present,
    // the class instead gets the auto-generated POD table constructor, called as `Name{ ... }`,
    // which is displayed quite differently below.
    bool hasPositionalConstructor = finder.result->primaryConstructor != nullptr;
    // A private constructor -- `class Account private (...)`, or a `private function __init` --
    // means the class cannot be constructed by calling it from outside its own lexical scope, so
    // the header has to say so: otherwise the signature reads as an invitation to call something
    // that would fail at runtime, and the public factory function that exists precisely because the
    // constructor is private looks redundant.
    bool constructorIsPrivate =
        finder.result->primaryConstructor && finder.result->primaryConstructor->visibility == Luau::AstClassMemberVisibility::Private;
    // If nothing in the class is private, the "public " prefix on every member line is just noise
    // -- omit it and let the reader assume public, matching how `private` alone would otherwise
    // stand out on a member line if there were any.
    bool anyPrivateMember = false;
    for (const auto& member : finder.result->members)
    {
        if (const auto* method = member.get_if<Luau::AstClassMethod>(); method && method->functionName == "__init")
        {
            hasPositionalConstructor = true;
            if (method->visibility == Luau::AstClassMemberVisibility::Private)
                constructorIsPrivate = true;
        }

        if (Luau::visit([](auto&& m) -> bool { return m.visibility == Luau::AstClassMemberVisibility::Private; }, member))
            anyPrivateMember = true;
    }

    // A primary constructor parameter implicitly declares a field of the same name, so a `private`
    // written on one makes the class have private members just as much as a `private` in the body
    // does -- without this, a class whose only private members come from its parameter list would
    // drop the "public " prefix from every member line and read as if it had none.
    if (const auto* ctor = finder.result->primaryConstructor)
        for (const auto& qualifiers : ctor->argsQualifiers)
            if (qualifiers.visibility == Luau::AstClassMemberVisibility::Private)
                anyPrivateMember = true;
    std::string publicPrefix = anyPrivateMember ? "public " : "";
    auto visibilityPrefix = [&publicPrefix](bool isPrivate)
    {
        return isPrivate ? std::string("private ") : publicPrefix;
    };
    // Private members are shown on the class value's own summary and hidden on an object's. The
    // class value is what you hover while writing the class, from inside the lexical scope its
    // privates are reachable in -- hiding half the class there just makes the summary lie about
    // its shape. An object is held by whoever received it, typically from outside that scope,
    // where a private member is not something they can read, write, or call.
    auto showsPrivate = [isClassValue](bool isPrivate)
    {
        return isClassValue || !isPrivate;
    };

    // Instance fields (e.g. `const inner = ...` with no annotation) only live in the *object*
    // type's props -- the class value's own `et` only has static members and the constructor. So
    // when summarizing the class value, resolve instance member types off the object type
    // (reached via `relation`) rather than off `et` itself.
    const Luau::ExternType* objectEt = et;
    if (isClassValue)
    {
        if (et->relation)
        {
            if (const auto* obj = Luau::get_if<Luau::Obj>(&*et->relation))
                if (const auto* objectExternType = Luau::get<Luau::ExternType>(Luau::follow(obj->ty)))
                    objectEt = objectExternType;
        }
    }

    if (isClassValue && et->metatable)
    {
        if (auto mt = Luau::get<Luau::TableType>(Luau::follow(*et->metatable)))
        {
            if (auto it = mt->props.find("__call"); it != mt->props.end() && it->second.readTy)
            {
                if (auto ctorFtv = Luau::get<Luau::FunctionType>(Luau::follow(*it->second.readTy)))
                {
                    if (hasPositionalConstructor)
                    {
                        types::ToStringNamedFunctionOpts funcOpts;
                        funcOpts.hideTableKind = !showTableKinds;
                        funcOpts.hideFirstParameter = true;
                        std::string ctorString = types::toStringNamedFunction(module, ctorFtv, std::string(""), scope, funcOpts);
                        if (constructorIsPrivate)
                            header += " private ";
                        header += extractArgList(ctorString);
                        hasConstructorLine = true;
                    }
                    else
                    {
                        // No custom `__init` -- the class gets an auto-generated ("POD")
                        // constructor that takes a single table of the class's fields, called as
                        // `Name{ field = value, ... }` (a table-literal call, not `Name(...)` --
                        // there are no parens to speak of). Show it as a struct-like field list
                        // rather than folding it into the header as a single argument list, which
                        // gets unreadable once there's more than one or two fields.
                        auto [argHead, argTail] = Luau::flatten(ctorFtv->argTypes);
                        if (argHead.size() >= 2)
                        {
                            if (auto ctorArgTable = Luau::get<Luau::TableType>(Luau::follow(argHead[1])))
                            {
                                // Private fields still appear in the `{ ... }` block -- there's no
                                // other way to set them without a custom `__init`, so leaving them
                                // out would hide the fact that they must be passed to construct the
                                // object. But the `{ ... }` table shape itself has no way to express
                                // visibility, so also list private fields again separately below the
                                // table, marked `private`, so their visibility isn't lost.
                                std::vector<std::string> fieldEntries;
                                std::string privateFields;
                                // If any field's own type is itself a nested table/function (has a
                                // brace or paren in its printed form), folding everything onto one
                                // line reads as an ambiguous wall of braces -- always break those
                                // out one field per line instead, regardless of overall length.
                                bool anyComplexFieldType = false;
                                for (const auto& fieldMember : finder.result->members)
                                {
                                    const auto* prop = fieldMember.get_if<Luau::AstClassProperty>();
                                    if (!prop)
                                        continue;

                                    auto propIt = ctorArgTable->props.find(prop->name.value);
                                    if (propIt == ctorArgTable->props.end() || !propIt->second.readTy)
                                        continue;

                                    std::string fieldType = Luau::toString(Luau::follow(*propIt->second.readTy));
                                    std::string constPrefix = prop->isConst ? "const " : "";

                                    if (fieldType.find('{') != std::string::npos || fieldType.find('(') != std::string::npos)
                                        anyComplexFieldType = true;

                                    fieldEntries.push_back(constPrefix + std::string(prop->name.value) + ": " + fieldType);

                                    if (prop->visibility == Luau::AstClassMemberVisibility::Private)
                                        privateFields += "    private " + constPrefix + std::string(prop->name.value) + ": " + fieldType + "\n";
                                }

                                if (!fieldEntries.empty() || !privateFields.empty())
                                {
                                    // Prefer folding the fields into the header on one line -- it
                                    // reads like a constructor call (`Name{ field = value, ... }`)
                                    // -- but only while that line stays reasonably short; beyond
                                    // that it's more readable broken out one field per line.
                                    static constexpr size_t kMaxInlineFieldsWidth = 100;
                                    std::string inlineFields = "{ ";
                                    for (size_t i = 0; i < fieldEntries.size(); i++)
                                    {
                                        if (i > 0)
                                            inlineFields += ", ";
                                        inlineFields += fieldEntries[i];
                                    }
                                    inlineFields += " }";

                                    if (!anyComplexFieldType && header.size() + 1 + inlineFields.size() <= kMaxInlineFieldsWidth)
                                    {
                                        header += " " + inlineFields;
                                        if (!privateFields.empty())
                                        {
                                            header += "\n" + privateFields;
                                            header.pop_back(); // drop the trailing '\n' -- the caller adds its own below
                                        }
                                    }
                                    else
                                    {
                                        std::string fields;
                                        for (const auto& entry : fieldEntries)
                                            fields += "    " + entry + ",\n";

                                        header += " {\n" + fields + "}\n" + privateFields;
                                        header.pop_back(); // drop the trailing '\n' -- the caller adds its own below
                                    }
                                    hasConstructorLine = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    header += "\n";

    // Fields declared by the primary constructor's parameters. These are real fields of the object
    // but they aren't AstClassMembers, so the members loop below never sees them -- without this, an
    // object of a class whose fields all come from its parameter list would summarize as empty. A
    // parameter restated in the class body *is* a member, so it's left to that loop to avoid
    // listing it twice (the body's restatement is the more informative of the two anyway: it can
    // carry an annotation and a derived default value the parameter doesn't have).
    if (const auto* ctor = finder.result->primaryConstructor)
    {
        for (size_t i = 0; i < ctor->args.size; i++)
        {
            const Luau::AstLocal* arg = ctor->args.data[i];
            const bool isPrivate =
                i < ctor->argsQualifiers.size && ctor->argsQualifiers.data[i].visibility == Luau::AstClassMemberVisibility::Private;
            if (!showsPrivate(isPrivate))
                continue;

            bool restatedInBody = false;
            for (const auto& member : finder.result->members)
                if (const auto* prop = member.get_if<Luau::AstClassProperty>(); prop && prop->name == arg->name)
                    restatedInBody = true;
            if (restatedInBody)
                continue;

            bool isConst = i < ctor->argsQualifiers.size && ctor->argsQualifiers.data[i].isConst;
            std::string line = "    " + visibilityPrefix(isPrivate) + (isConst ? "const " : "") + std::string(arg->name.value) + ": ";

            // Same order of preference as a class body field below: the instantiated type first so
            // generics print concretely, then the parameter's own annotation, then `any`. A field
            // whose type can't be resolved is still a field -- dropping the line entirely would
            // silently understate the class's shape, which is worse than printing `any`.
            if (auto it = objectEt->props.find(arg->name.value); it != objectEt->props.end() && it->second.readTy)
                line += Luau::toString(Luau::follow(*it->second.readTy));
            else if (arg->annotation)
            {
                const Luau::TypeId* resolvedTy = definitionModule ? definitionModule->astResolvedTypes.find(arg->annotation) : nullptr;
                line += resolvedTy ? Luau::toString(Luau::follow(*resolvedTy)) : "any";
            }
            else
                line += "any";

            pushField(line);
        }
    }

    for (const auto& member : finder.result->members)
    {
        if (const auto* prop = member.get_if<Luau::AstClassProperty>())
        {
            const bool isPrivate = prop->visibility == Luau::AstClassMemberVisibility::Private;
            if (!showsPrivate(isPrivate))
                continue;

            std::string line = "    " + visibilityPrefix(isPrivate) + (prop->isConst ? "const " : "") + std::string(prop->name.value) + ": ";
            // Prefer the instantiated type from `et->props` over the AST-resolved type of the
            // annotation, so that generic classes (e.g. `class Box<T> ... end`) show `string`
            // rather than `T` when hovering over a `Box<string>`. See formatMethodLine for the
            // equivalent handling of methods.
            if (auto it = objectEt->props.find(prop->name.value); it != objectEt->props.end() && it->second.readTy)
                line += Luau::toString(Luau::follow(*it->second.readTy));
            else if (prop->ty)
            {
                const Luau::TypeId* resolvedTy = definitionModule ? definitionModule->astResolvedTypes.find(prop->ty) : nullptr;
                line += resolvedTy ? Luau::toString(Luau::follow(*resolvedTy)) : "any";
            }
            else
                line += "any";

            // A class with the auto-generated POD constructor already lists every field in the
            // header's `Name{ ... }` block, so repeating them below it is pure noise.
            if (isClassValue && !hasPositionalConstructor)
                continue;

            pushField(line);
            continue;
        }

        const auto* method = member.get_if<Luau::AstClassMethod>();
        if (!method)
            continue;

        const bool isPrivate = method->visibility == Luau::AstClassMemberVisibility::Private;
        if (!showsPrivate(isPrivate))
            continue;

        // Skip dunder methods (e.g. `__init`, `__tostring`) -- they're not really part of the
        // "public API surface" this summary is meant to show, and we don't want them crowding
        // out real members when the summary gets truncated.
        if (isDunderName(method->functionName.value))
            continue;

        bool isStatic = isStaticMethod(method);

        // The object's summary never shows static (self-less) functions -- they're not callable
        // on an instance.
        if (!isClassValue && isStatic)
            continue;

        // Static methods live on the class's own `et`; instance methods (like instance fields)
        // only live on the object type, reached via `objectEt`.
        std::string line = formatMethodLine(module, isStatic ? et : objectEt, method, scope, showTableKinds);
        if (line.empty())
            continue;
        line = "    " + visibilityPrefix(isPrivate) + line;

        totalFunctions++;
        functionEntries.emplace_back(std::move(line), isStatic);
    }

    // Statics first, instance methods after, each group still in source order.
    std::stable_partition(
        functionEntries.begin(),
        functionEntries.end(),
        [](const std::pair<std::string, bool>& entry)
        {
            return entry.second;
        }
    );
    if (functionEntries.size() > maxMembers)
        functionEntries.resize(maxMembers);

    if (fieldLines.empty() && functionEntries.empty() && !hasConstructorLine)
        return std::nullopt;

    auto elidedLine = [](size_t elided)
    {
        return "    -- ⋯ " + std::to_string(elided) + " more member" + (elided == 1 ? "" : "s") + "\n";
    };

    // No separator between the field section and the function section, nor between the static and
    // instance functions within it -- a function line reads as a function on sight, and instance
    // methods all take a leading `self` parameter, so both splits are already obvious without a
    // blank line or a comment header spending a row to say so.
    std::string summary = header;
    for (const auto& line : fieldLines)
        summary += line + "\n";
    if (totalFields > fieldLines.size())
        summary += elidedLine(totalFields - fieldLines.size());

    for (const auto& [line, isStatic] : functionEntries)
        summary += line + "\n";
    if (totalFunctions > functionEntries.size())
        summary += elidedLine(totalFunctions - functionEntries.size());

    summary += "end";

    return summary;
}

// Builds a short summary of a "declare extern type"-style extern type's members, formatted like:
// extern type Instance
//     Name: string
//     function Clone(): Instance
//     -- ⋯ 2 more members
// end
// Extern types have no factory/constructor (they're provided by the host), so unlike
// buildClassFieldSummary, there's only ever the "object" shape -- no separate class-value variant.
// Unlike `class`, `declare extern type` has no public/private visibility syntax, so member lines
// aren't prefixed with either.
static std::string buildExternTypeSummary(
    const Luau::ModulePtr& module, Luau::TypeId typeId, const Luau::ExternType* et, const Luau::ScopePtr& scope, bool showTableKinds,
    size_t maxMembers = kMaxSummaryMembers
)
{
    // Fields before functions, each section truncated independently -- same layout as
    // buildClassFieldSummary. `props` is ordered by name, so without this split the two interleave
    // alphabetically and the type's shape is scattered between its methods.
    std::vector<std::string> fieldLines;
    std::vector<std::string> functionLines;
    size_t totalFields = 0;
    size_t totalFunctions = 0;

    for (const auto& [name, prop] : et->props)
    {
        if (isDunderName(name))
            continue;

        std::optional<Luau::TypeId> ty = prop.readTy ? prop.readTy : prop.writeTy;
        if (!ty)
            continue;

        if (auto ftv = Luau::get<Luau::FunctionType>(Luau::follow(*ty)))
        {
            totalFunctions++;
            if (functionLines.size() >= maxMembers)
                continue;

            types::ToStringNamedFunctionOpts funcOpts;
            funcOpts.hideTableKind = !showTableKinds;
            // Methods are declared as `function Name(self: Instance, ...): ret` -- print the
            // leading `self` parameter bare (no type), matching how class instance methods are
            // displayed; the reader already knows the self type from the header above.
            funcOpts.hideFirstParameterType = !ftv->argNames.empty() && ftv->argNames[0] && ftv->argNames[0]->name == "self";
            funcOpts.baseIndent = "    ";
            functionLines.push_back("    " + types::toStringNamedFunction(module, ftv, name, scope, funcOpts));
        }
        else
        {
            totalFields++;
            if (fieldLines.size() >= maxMembers)
                continue;

            fieldLines.push_back("    " + name + ": " + Luau::toString(Luau::follow(*ty)));
        }
    }

    auto elidedLine = [](size_t elided)
    {
        return "    -- ⋯ " + std::to_string(elided) + " more member" + (elided == 1 ? "" : "s") + "\n";
    };

    // Use the type's own toString (rather than the bare `et->name`) so that generic parameters
    // display correctly, e.g. "extern type Box<number>".
    std::string summary = "extern type " + Luau::toString(Luau::follow(typeId)) + "\n";
    for (const auto& line : fieldLines)
        summary += line + "\n";
    if (totalFields > fieldLines.size())
        summary += elidedLine(totalFields - fieldLines.size());
    for (const auto& line : functionLines)
        summary += line + "\n";
    if (totalFunctions > functionLines.size())
        summary += elidedLine(totalFunctions - functionLines.size());
    summary += "end";

    return summary;
}

struct Primitive
{
    std::string name;
    std::string docs;
};

static std::optional<Primitive> builtinPrimitive(const Luau::Frontend& frontend, Luau::TypeId ty)
{
    const auto& builtins = frontend.builtinTypes;
    Luau::TypeId followed = Luau::follow(ty);

    if (followed == builtins->objectType)
        return Primitive{"object",
            "What gets built when you call a `class`; objects are often referred to as 'instances' of a class.\n\n"
            "Write `object` if you want to allow any object from any class or use a class's name directly to refer to objects of that class."
            " To get the actual `class` type from the name of a class, use `class<ClassName>`. See `class` for further information about nominalness.\n\n"
            "To narrow unspecified or unioned objects in the type system, use the standard library function `class.isinstance(obj, SomeClass)` in an"
            " if statement or expression to narrow `obj` into an object of `SomeClass`."
        };
    if (followed == builtins->classType)
        return Primitive{"class",
            "A value that creates new objects with a specific field and function structure."
            " Write `class<ClassName>` for the class that builds `ClassName` objects or plain `class`"
            " to allow any class at all.\n\nUnlike table types, classes and object types are nominally-typed, meaning you can"
            " have multiple classes/objects with the exact same fields but different names and the type solver knows they're"
            " all unique and can't accidentally be mixed up or casted into one another."
        };
    if (followed == builtins->externType)
        return Primitive{"userdata",
            "A value given to your code by the embedder. Userdata are called extern types in the type system because they're implemented"
            " externally (outside Luwu), often in a language like C, Rust, or C++. Trying to access a nonexistent field on a userdata will result"
            " in a runtime error.\n\n"
            "To narrow an extern type in the type system, use the builtin `typeof(value)` function to compare the userdata with a string that"
            " represents the extern type's name. The extern type's name should be given by the `__type` field set on the extern type's"
            " metatable by the embedder. If that extern type has been registered (via `declare extern type Name` syntax in a definitions file)"
            " the LSP is able to narrow `value` to a specific extern type. If an extern type is extended via inheritance, the `__type` field checked"
            " by the LSP will be the `__type` declared on the root base class of the extern type (not the most specific `__type`)."
        };
    if (followed == builtins->vectorType)
        return Primitive{"vector",
            "An immutable primitive with fields `x`, `y` and `z` that store 32-bit floats. Vectors are highly optimized for"
            " 3D math using the `vector` library, and can be used to represent 2D and 3D coordinates, forces,"
            " RGB colors, edges of directed graphs, or whatever you decide fits into 2 or 3 numbers. Since they live on the"
            " stack, they're copied by value and incredibly cheap to use (about as cheap as numbers). Use the standard library"
            " function `vector.create(x, y, z?)` to create new vectors."
        };

    return std::nullopt;
}

std::optional<lsp::Hover> WorkspaceFolder::hover(const lsp::HoverParams& params, const LSPCancellationToken& cancellationToken)
{
    auto config = client->getConfiguration(rootUri);

    if (!config.hover.enabled)
        return std::nullopt;

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());

    auto position = textDocument->convertPosition(params.position);
    const std::string codeLanguage = codeBlockLanguage(*textDocument);

    // Run the type checker to ensure we are up to date
    // TODO: expressiveTypes - remove "forAutocomplete" once the types have been fixed
    checkStrict(moduleName, cancellationToken, /* forAutocomplete: */ config.hover.strictDatamodelTypes);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    auto module = getModule(moduleName, /* forAutocomplete: */ config.hover.strictDatamodelTypes);
    if (!sourceModule)
        return std::nullopt;

    // A comment directive is documented even though it sits in a comment, so it has to be checked
    // before we give up on comments generally.
    if (auto directiveMatch = findCommentDirectiveDocKeyAtPosition(sourceModule->hotcomments, position);
        directiveMatch && (directiveMatch->docKey != "directive_trust" || isLuwuFile(*textDocument)))
        if (auto docs = getKeywordHoverDocs(directiveMatch->docKey))
            return lsp::Hover{{lsp::MarkupKind::Markdown, *docs}, textDocument->convertLocation(directiveMatch->range)};

    if (Luau::isWithinComment(*sourceModule, position))
        return std::nullopt;

    if (auto hover = platform->handleHover(*textDocument, *sourceModule, position))
        return hover;

    auto exprOrLocal = Luau::findExprOrLocalAtPosition(*sourceModule, position);
    auto node = findNodeOrTypeAtPosition(*sourceModule, position);
    auto scope = Luau::findScopeAtPosition(*module, position);
    if (!node || !scope)
        return std::nullopt;

    auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position);
    if (auto keywordMatch = findKeywordDocKeyAtPosition(ancestry, position))
    {
        if (auto docs = getKeywordHoverDocs(keywordMatch->docKey))
            return lsp::Hover{{lsp::MarkupKind::Markdown, *docs}, textDocument->convertLocation(keywordMatch->range)};
    }

    std::optional<std::pair<std::string, Luau::TypeFun>> typeAliasInformation = std::nullopt;
    std::optional<Luau::TypeId> type = std::nullopt;
    std::optional<std::string> documentationSymbol = getDocumentationSymbolAtPosition(*sourceModule, *module, position);
    std::optional<DocumentationLocation> documentationLocation = std::nullopt;
    std::optional<std::string> classMemberPrefix = std::nullopt;
    std::optional<std::string> classMemberName = std::nullopt;

    if (auto classStat = node->as<Luau::AstStatClass>())
    {
        // Hovering over the class's own name (e.g. `class |Foo ... end`) -- show a summary of the
        // class value itself, same as hovering over a reference to the class elsewhere. Note this
        // is a *value* lookup, not a type lookup: `Foo` in the type namespace refers to instances
        // of the class (the `object` type), while the class statement itself binds `Foo` in the
        // value namespace to the class value (the `class` type, with a `__call` constructor).
        if (classStat->name->location.containsClosed(position))
        {
            if (auto classValueTy = scope->lookup(classStat->name->name))
                type = *classValueTy;
        }

        // Hovering over a primary constructor parameter's name (e.g. `class Particle private (public
        // |position: Vector2)`). Each parameter implicitly declares a field of the same name, so
        // this shows the same `public position: Vector2` shape a field written in the class body
        // would -- but parameters are AstLocals, not AstClassMembers, so the members loop below
        // never sees them and nothing was shown at all here.
        if (const auto* ctor = classStat->primaryConstructor; ctor && !type)
        {
            for (size_t i = 0; i < ctor->args.size; i++)
            {
                const Luau::AstLocal* arg = ctor->args.data[i];
                if (!arg->location.containsClosed(position))
                    continue;

                const Luau::AstClassPrimaryConstructorParamQualifiers* qualifiers =
                    i < ctor->argsQualifiers.size ? &ctor->argsQualifiers.data[i] : nullptr;

                // A parameter that carries no qualifier of its own is only public *by default* --
                // the RFC's other spelling is to leave the parameter list bare and qualify the
                // field where it's restated in the class body (`class List<T> private (inner: {T})`
                // with a `private inner` inside). That restatement is the field's actual
                // declaration, so prefer it; reporting the parameter's default here would state the
                // opposite of what the class says two lines below.
                const Luau::AstClassProperty* restatement = nullptr;
                for (const auto& member : classStat->members)
                    if (const auto* prop = member.get_if<Luau::AstClassProperty>(); prop && prop->name == arg->name)
                        restatement = prop;

                bool isPrivate = qualifiers && qualifiers->visibility == Luau::AstClassMemberVisibility::Private;
                bool isConst = qualifiers && qualifiers->isConst;
                if (restatement)
                {
                    if (!qualifiers || !qualifiers->qualifierLocation)
                        isPrivate = restatement->visibility == Luau::AstClassMemberVisibility::Private;
                    if (!qualifiers || !qualifiers->constLocation)
                        isConst = restatement->isConst;
                }

                classMemberPrefix = isPrivate ? "private " : "public ";
                if (isConst)
                    classMemberPrefix = *classMemberPrefix + "const ";

                classMemberName = arg->name.value;

                if (arg->annotation)
                {
                    if (auto ty = module->astResolvedTypes.find(arg->annotation))
                        type = *ty;
                }
                else if (auto classTypeFun = scope->lookupType(classStat->name->name.value))
                {
                    // Unannotated parameter (`class Vector4(x, y, z, w)`) -- its field's type was
                    // inferred, so read it back off the class's own instance type, same as an
                    // unannotated field written in the class body.
                    if (auto propInfo = lookupProp(classTypeFun->type, arg->name.value); !propInfo.empty())
                        if (propInfo[0].property.readTy)
                            type = propInfo[0].property.readTy;
                }

                documentationLocation = {moduleName, arg->location};
                break;
            }
        }

        for (const auto& member : classStat->members)
        {
            if (type)
                break;

            Luau::Location nameLocation = Luau::visit(
                [](auto&& m) -> Luau::Location
                {
                    return m.nameLocation;
                },
                member
            );

            if (!nameLocation.containsClosed(position))
                continue;

            bool isPrivate = Luau::visit(
                [](auto&& m) -> bool
                {
                    return m.visibility == Luau::AstClassMemberVisibility::Private;
                },
                member
            );
            classMemberPrefix = isPrivate ? "private " : "public ";

            if (const auto* prop = member.get_if<Luau::AstClassProperty>())
            {
                if (prop->isConst)
                    classMemberPrefix = *classMemberPrefix + "const ";

                classMemberName = prop->name.value;
                if (prop->ty)
                {
                    if (auto ty = module->astResolvedTypes.find(prop->ty))
                        type = *ty;
                }
                else if (auto classTypeFun = scope->lookupType(classStat->name->name.value))
                {
                    // No explicit type annotation was given -- fall back to the class's own
                    // (inferred) instance type to find this property's inferred type.
                    if (auto propInfo = lookupProp(classTypeFun->type, prop->name.value); !propInfo.empty())
                        if (propInfo[0].property.readTy)
                            type = propInfo[0].property.readTy;
                }
                documentationLocation = {moduleName, prop->nameLocation};
            }
            else if (const auto* method = member.get_if<Luau::AstClassMethod>())
            {
                classMemberName = method->functionName.value;
                if (auto ty = module->astTypes.find(method->function))
                    type = *ty;
                documentationLocation = {moduleName, method->nameLocation};
            }

            break;
        }
    }
    else if (auto ref = node->as<Luau::AstTypeReference>())
    {
        std::string typeName;
        std::optional<Luau::TypeFun> typeFun;
        if (ref->prefix)
        {
            typeName = std::string(ref->prefix->value) + "." + ref->name.value;
            typeFun = scope->lookupImportedType(ref->prefix->value, ref->name.value);
        }
        else
        {
            typeName = ref->name.value;
            typeFun = scope->lookupType(ref->name.value);
        }
        if (!typeFun)
            return std::nullopt;
        typeAliasInformation = std::make_pair(typeName, *typeFun);
        type = typeFun->type;
    }
    else if (auto alias = node->as<Luau::AstStatTypeAlias>())
    {
        auto typeName = alias->name.value;
        auto typeFun = scope->lookupType(typeName);
        if (!typeFun)
            return std::nullopt;
        typeAliasInformation = std::make_pair(typeName, *typeFun);
        type = typeFun->type;
    }
    else if (auto typeTable = node->as<Luau::AstTypeTable>())
    {
        if (auto tableTy = module->astResolvedTypes.find(typeTable))
        {
            type = *tableTy;

            // Check if we are inside one of the properties
            for (auto& prop : typeTable->props)
            {
                if (prop.location.containsClosed(position))
                {
                    auto parentType = Luau::follow(*tableTy);
                    if (auto definitionModuleName = Luau::getDefinitionModuleName(parentType))
                        documentationLocation = {definitionModuleName.value(), prop.location};
                    auto resolvedProperty = lookupProp(parentType, prop.name.value);
                    if (resolvedProperty.size() == 1 && resolvedProperty[0].property.readTy)
                        type = resolvedProperty[0].property.readTy;
                    break;
                }
            }
        }
    }
    else if (auto astType = node->asType())
    {
        if (auto ty = module->astResolvedTypes.find(astType))
        {
            type = *ty;
        }
    }
    else if (auto local = exprOrLocal.getLocal()) // TODO: can we just use node here instead of also calling exprOrLocal?
    {
        type = scope->lookup(local);
        documentationLocation = {moduleName, local->location};
    }
    else if (auto expr = exprOrLocal.getExpr())
    {
        // findExprOrLocalAtPosition falls back to matching the whole enclosing AstExprFunction
        // whenever no more specific statement/local claims a position inside its body (e.g. blank
        // lines, indentation to the left of a statement) -- which would otherwise show the full
        // function signature when hovering over plain whitespace. `node`, from the
        // block-boundary-aware findNodeOrTypeAtPosition, doesn't have this problem, so only trust
        // this match when the two agree.
        if (expr->is<Luau::AstExprFunction>() && node != expr)
            return std::nullopt;

        // Special case, we want to check if there is a parent in the ancestry, and if it is an AstTable
        // If so, and we are hovering over a prop, we want to give type info for the assigned expression to the prop
        // rather than just "string"
        if (ancestry.size() >= 2 && ancestry.at(ancestry.size() - 2)->is<Luau::AstExprTable>())
        {
            auto parent = ancestry.at(ancestry.size() - 2)->as<Luau::AstExprTable>();
            for (const auto& [kind, key, value] : parent->items)
            {
                if (key && key->location.contains(position))
                {
                    // Return type type of the value
                    if (auto it = module->astTypes.find(value))
                    {
                        type = *it;
                    }
                    break;
                }
            }
        }

        // Handle table properties (so that we can get documentation info)
        if (auto index = expr->as<Luau::AstExprIndexName>())
        {
            if (auto parentIt = module->astTypes.find(index->expr))
            {
                auto parentType = Luau::follow(*parentIt);
                auto indexName = index->index.value;
                if (auto propInformation = lookupProp(parentType, indexName); !propInformation.empty())
                {
                    auto [baseTy, prop] = propInformation[0];
                    if (propInformation.size() == 1 && prop.readTy)
                        type = prop.readTy;
                    if (auto definitionModuleName = Luau::getDefinitionModuleName(baseTy))
                    {
                        if (prop.location)
                            documentationLocation = {definitionModuleName.value(), prop.location.value()};
                        else if (prop.typeLocation)
                            documentationLocation = {definitionModuleName.value(), prop.typeLocation.value()};
                    }
                }
            }
        }

        // Handle local variables separately to retrieve documentation location info
        if (auto local = expr->as<Luau::AstExprLocal>(); !documentationLocation.has_value() && local && local->local)
        {
            documentationLocation = {moduleName, local->local->location};
        }

        if (!type)
        {
            if (auto it = module->astTypes.find(expr))
            {
                type = *it;
            }
            else if (auto global = expr->as<Luau::AstExprGlobal>())
            {
                type = scope->lookup(global->name);
            }
            else if (auto local = expr->as<Luau::AstExprLocal>())
            {
                type = scope->lookup(local->local);
            }
        }
    }

    if (!type)
        return std::nullopt;
    type = Luau::follow(*type);

    if (!documentationSymbol)
        documentationSymbol = type.value()->documentationSymbol;

    Luau::ToStringOptions opts;
    opts.exhaustive = false;
    opts.useLineBreaks = true;
    opts.functionTypeArguments = true;
    opts.hideNamedFunctionTypeParameters = false;
    opts.hideTableKind = !config.hover.showTableKinds;
    opts.scope = scope;
    // `maxTypeLength` truncates by refusing every emit past the limit, closing brackets included, so
    // hitting it mid-type leaves an unbalanced `{`/`(` that breaks highlighting of everything after
    // it in the code block. It applies per `where` clause body too, so one large alias (a library
    // table of functions, say) is enough. `maxTableLength` instead stops between properties with a
    // `... N more ...` entry and still closes the table -- make that the limit a hover normally hits,
    // and keep `maxTypeLength` well above it as a backstop for types that aren't tables.
    opts.maxTableLength = 500;
    opts.maxTypeLength = 2000;
    // show type aliases referred to by this hover
    opts.includeWhereClauses = true;
    // hovering over the top level of alias itself shouldn't show just the alias name, 
    // it needs to expand the alias. otherwise you hover over the variable 'fs' and get 'type fs = fs'
    // which is completely useless.
    opts.alwaysExpandRootAlias = true;
    Luau::ToStringResult typeResult = Luau::toStringDetailed(*type, opts);
    std::string typeString = typeResult.name;

    // Inside a class, the class itself is the one thing the reader doesn't need told: both its
    // summary and a link to it are noise in every hover written within its own body
    Luau::AstStatClass* enclosingClassStat = types::findEnclosingClassStat(sourceModule->root, position);
    auto isEnclosingClass = [&](Luau::TypeId ty) -> bool
    {
        return enclosingClassStat && types::findClassStatFromExternType(sourceModule->root, ty) == enclosingClassStat;
    };

    // A class or extern type referenced by the hovered type prints as a bare name, which says nothing
    // about its shape -- and unlike an alias, ToString has no body to expand it into for a `where`
    // clause. So give each one the same summary hovering the type directly would show, below the
    // alias clauses. `typeSpans` records every ExternType emitted (union elements and alias bodies
    // included), so it's the set of extern types actually visible in the hover, in print order.
    // Like alias clauses this is one level deep: a class mentioned inside a summary isn't expanded.
    std::string externTypeSummaries;
    {
        Luau::DenseHashSet<Luau::TypeId> seen{nullptr};
        const auto& builtins = frontend.builtinTypes;

        // A method's `self` is the class/extern type it's being called on (`x:function1()`) --
        // summarizing the receiver back at the reader just repeats what they're already looking at.
        if (auto ftv = Luau::get<Luau::FunctionType>(*type); ftv && !ftv->argNames.empty() && ftv->argNames[0] && ftv->argNames[0]->name == "self")
        {
            auto [argHead, _] = Luau::flatten(ftv->argTypes);
            if (!argHead.empty())
                seen.insert(Luau::follow(argHead[0]));
        }
        for (const auto& span : typeResult.typeSpans)
        {
            Luau::TypeId spanTy = Luau::follow(span.type);
            const auto* et = Luau::get<Luau::ExternType>(spanTy);
            if (!et || seen.contains(spanTy))
                continue;
            seen.insert(spanTy);

            // A class and its instances are two extern types (`class Foo` and objects of it) tied by
            // the nominal relation, and a hover that mentions both would otherwise summarize the same
            // declaration twice. Whichever one is printed first stands for the pair.
            if (et->relation)
            {
                Luau::TypeId relatedTy = nullptr;
                if (const auto* obj = et->relation->get_if<Luau::Obj>())
                    relatedTy = obj->ty;
                else if (const auto* klass = et->relation->get_if<Luau::Klass>())
                    relatedTy = klass->ty;

                if (relatedTy)
                    seen.insert(Luau::follow(relatedTy));
            }

            if (spanTy == builtins->externType || spanTy == builtins->objectType || spanTy == builtins->classType || spanTy == builtins->vectorType)
                continue;

            if (isEnclosingClass(spanTy))
                continue;

            std::optional<std::string> summary;
            if (et->parent == builtins->classType || et->parent == builtins->objectType)
                summary = buildClassFieldSummary(
                    frontend, module, moduleName, spanTy, et, scope, config.hover.showTableKinds, et->parent == builtins->classType,
                    kMaxReferencedSummaryMembers
                );
            else if (!et->props.empty())
                summary = buildExternTypeSummary(module, spanTy, et, scope, config.hover.showTableKinds, kMaxReferencedSummaryMembers);

            if (!summary)
                continue;
            if (!externTypeSummaries.empty())
                externTypeSummaries += "\n\n";
            externTypeSummaries += *summary;
        }
    }

    // A referenced type prints as a bare name, so collect a link to where each one is defined --
    // including types from this same file, which may still be hundreds of lines away. `typeSpans` gives the named types the hover emitted along with
    // their TypeIds, which covers both the aliases expanded into `where` clauses and the extern
    // types summarized above. These go on one line above the referenced types themselves: a hover
    // over a type from a big module can pull in a lot of them, and the reader's documentation is
    // below all of it.
    // A binding holding a required module is the module, and the hover otherwise shows a wall of
    // its members without ever saying where they came from
    std::string importedModuleLine;
    {
        Luau::AstLocal* bindingLocal = nullptr;
        if (auto local = exprOrLocal.getLocal())
            bindingLocal = local;
        else if (auto localExpr = node->as<Luau::AstExprLocal>())
            bindingLocal = localExpr->local;

        std::optional<std::string> bindingName;
        if (bindingLocal)
            bindingName = bindingLocal->name.value;

        const auto& importedModules = module->getModuleScope()->importedModules;
        if (bindingName)
        {
            if (auto importedModule = importedModules.find(*bindingName); importedModule != importedModules.end())
            {
                // The path the reader wrote (`@std/str`) rather than the resolved module name
                std::string requirePath;
                for (Luau::AstStat* stat : sourceModule->root->body)
                {
                    auto localStat = stat->as<Luau::AstStatLocal>();
                    if (!localStat)
                        continue;

                    for (size_t i = 0; i < localStat->vars.size && i < localStat->values.size; ++i)
                    {
                        if (localStat->vars.data[i] != bindingLocal)
                            continue;

                        if (auto call = localStat->values.data[i]->as<Luau::AstExprCall>())
                            if (auto required = types::matchRequire(*call))
                                if (auto path = (*required)->as<Luau::AstExprConstantString>())
                                    requirePath = std::string(path->value.data, path->value.size);
                    }
                }

                std::string moduleLink = requirePath.empty() ? "" : "`" + requirePath + "`";
                if (auto document = fileResolver.getOrCreateTextDocumentFromModuleName(importedModule->second))
                {
                    std::string label = requirePath.empty() ? moduleNameForUri(document->uri()) : requirePath;
                    moduleLink = "[" + label + "](" + document->uri().toString() + ")";
                }

                // The binding almost always carries the module's own name, so naming it again here
                // just repeats the line below
                if (!moduleLink.empty())
                    importedModuleLine = "*module at* " + moduleLink + "\n";
            }
        }
    }

    // Where a type was declared, and what to call the place it came from. A type from a definitions
    // file isn't a module the file resolver knows about, but the package's loaded document is kept
    // around, so those get a link too -- labelled by package (`@seal global definitions`) rather
    // than by file name, since that's how the reader refers to them.
    struct TypeSource
    {
        lsp::Location location;
        std::string moduleLabel;
    };

    auto resolveTypeSource = [&](Luau::TypeId ty) -> std::optional<TypeSource>
    {
        if (auto location = types::getTypeLocation(ty, &fileResolver))
            return TypeSource{*location, moduleNameForUri(location->uri)};

        auto definitionModuleName = Luau::getDefinitionModuleName(Luau::follow(ty));
        auto definitionLocation = getLocation(Luau::follow(ty));
        if (!definitionModuleName || !definitionLocation)
            return std::nullopt;

        auto definitionsFile = definitionsFileState.find(*definitionModuleName);
        if (definitionsFile == definitionsFileState.end())
            return std::nullopt;

        const TextDocument& document = definitionsFile->second.textDocument;
        return TypeSource{
            lsp::Location{document.uri(), lsp::Range{document.convertPosition(definitionLocation->begin),
                                              document.convertPosition(definitionLocation->end)}},
            *definitionModuleName + " global definitions"};
    };

    // Hovering a member accessed off a class or extern type (`Documentation.from`, `comm:extract`)
    // shows the member alone, which says nothing about what it belongs to or where that lives
    std::string memberOwnerLine;
    std::optional<Luau::TypeId> memberOwnerType;
    if (!classMemberPrefix)
    {
        if (auto indexName = node->as<Luau::AstExprIndexName>())
        {
            if (auto ownerTy = module->astTypes.find(indexName->expr))
            {
                Luau::TypeId owner = Luau::follow(*ownerTy);
                if (Luau::get<Luau::ExternType>(owner))
                {
                    std::string ownerName = Luau::toString(owner);
                    std::string ownerLink = "`" + ownerName + "`";
                    std::string ownerModule;

                    if (auto source = resolveTypeSource(owner))
                    {
                        ownerLink = "[`" + ownerName + "`](" + source->location.uri.toString() + "#L" +
                                    std::to_string(source->location.range.start.line + 1) + ")";

                        // Naming the module is only news when it's a different one
                        if (source->location.uri != textDocument->uri())
                            ownerModule = " *from* [" + source->moduleLabel + "](" + source->location.uri.toString() + ")";
                    }

                    const char* kind = "*Field of*";
                    if (Luau::get<Luau::FunctionType>(Luau::follow(*type)))
                        kind = indexName->op == ':' ? "*Method of*" : "*Function of*";

                    // Two trailing spaces: a footer can follow another one (a field whose type is
                    // an object has both), and a plain newline would run them together as one
                    // paragraph
                    memberOwnerLine = "  \n" + std::string(kind) + " " + ownerLink + ownerModule;
                    memberOwnerType = owner;
                }
            }
        }
    }

    // A summary that stands on its own (a class, an extern type) says nothing about which type it
    // is really showing, or where that came from -- the same class name can be bound to any local
    // name, and two modules can each declare their own `Documentation`
    auto typeIdentityLine = [&](Luau::TypeId ty, const std::string& prefix) -> std::string
    {
        std::string name = Luau::toString(Luau::follow(ty));
        auto source = resolveTypeSource(ty);

        if (!source)
            return "\n" + prefix + "`" + name + "`";

        const lsp::Location* location = &source->location;
        const bool sameModule = location->uri == textDocument->uri();

        // Declared in this file, a bare linked name below the summary says nothing about why it's
        // there; declared elsewhere, naming the module does that job
        std::string lead = prefix;
        if (lead.empty() && sameModule)
            lead = "*Jump to* ";

        std::string line =
            "\n" + lead + "[`" + name + "`](" + location->uri.toString() + "#L" + std::to_string(location->range.start.line + 1) + ")";

        if (!sameModule)
            line += " *from* [" + source->moduleLabel + "](" + location->uri.toString() + ")";

        return line;
    };

    // An object-typed expression says which class it's an object of, the same way a member says what
    // it belongs to -- but only one of the two, since a member hover already names its owner
    std::optional<Luau::TypeId> objectOfType;
    if (memberOwnerLine.empty())
    {
        if (auto et = Luau::get<Luau::ExternType>(Luau::follow(*type)); et && et->parent == frontend.builtinTypes->objectType)
            objectOfType = Luau::follow(*type);
    }

    std::string referencedTypeLinks;
    {
        // A wide type can reference dozens of others; past a handful the links stop being useful
        static constexpr size_t kMaxReferencedTypeLinks = 12;

        struct ReferencedModule
        {
            lsp::DocumentUri uri;
            std::string name;
            std::vector<std::string> typeLinks;
            size_t shown = 0;
        };

        std::unordered_set<std::string> seenNames;
        std::vector<ReferencedModule> modules;
        size_t total = 0;

        for (const auto& span : typeResult.typeSpans)
        {
            auto name = types::getTypeName(span.type);
            if (!name || name->empty())
                continue;

            auto source = resolveTypeSource(span.type);
            if (!source)
                continue;
            const lsp::Location* location = &source->location;

            // A footer below the type already links its owner (or the class it's an object of);
            // repeating that as a reference says nothing new
            if (memberOwnerType && types::getTypeName(*memberOwnerType) == name)
                continue;
            if (objectOfType && types::getTypeName(*objectOfType) == name)
                continue;
            if (isEnclosingClass(Luau::follow(span.type)))
                continue;

            if (!seenNames.insert(*name).second)
                continue;

            auto module = std::find_if(modules.begin(), modules.end(),
                [&](const ReferencedModule& candidate)
                {
                    return candidate.uri == location->uri;
                });

            if (module == modules.end())
            {
                modules.push_back(ReferencedModule{location->uri, source->moduleLabel, {}, 0});
                module = std::prev(modules.end());
            }

            module->typeLinks.push_back(
                "[`" + *name + "`](" + location->uri.toString() + "#L" + std::to_string(location->range.start.line + 1) + ")");
            total += 1;
        }

        // Hand the link budget out a module at a time rather than first-come-first-served, so one
        // wide module can't spend all of it and leave the others unrepresented. Within a module the
        // order is the order the types were printed in, which puts the outermost ones first.
        for (size_t remaining = std::min(total, kMaxReferencedTypeLinks); remaining > 0;)
        {
            bool progressed = false;

            for (auto& module : modules)
            {
                if (module.shown == module.typeLinks.size())
                    continue;

                module.shown += 1;
                remaining -= 1;
                progressed = true;

                if (remaining == 0)
                    break;
            }

            if (!progressed)
                break;
        }

        if (!modules.empty())
        {
            // A bullet per module is only worth the vertical space once there's more than one of
            // them; a single module reads as one line, with the types first and their module named
            // once at the end -- the same "<what> from <module>" shape the origin footers use
            const bool oneModule = modules.size() == 1;
            referencedTypeLinks = oneModule ? "*References* " : "*References*\n";

            for (const auto& module : modules)
            {
                // The module itself links to the top of its file, so a reader can go straight to
                // where all of this is defined rather than to one type within it
                std::string moduleLink = "[" + module.name + "](" + module.uri.toString() + ")";

                if (!oneModule)
                    referencedTypeLinks += "\n- " + moduleLink + ": ";

                for (size_t i = 0; i < module.shown; ++i)
                    referencedTypeLinks += (i == 0 ? "" : " · ") + module.typeLinks[i];

                // Whatever didn't fit is counted against the module it belongs to, rather than as a
                // dangling bullet of its own
                if (size_t omitted = module.typeLinks.size() - module.shown; omitted > 0)
                    referencedTypeLinks += " · *and " + std::to_string(omitted) + " more*";

                if (oneModule && module.uri != textDocument->uri())
                    referencedTypeLinks += " *from* " + moduleLink;
            }
        }
    }

    // The type aliases referred to within this hover (such as type Pathlike = string | Path |
    // FilePath... for (path: Pathlike) -> string | error<info>) and the summaries of the classes and
    // extern types it refers to, in a code block of their own below the hovered type itself
    std::string referencedTypes;
    if (!typeResult.whereClauses.empty())
        referencedTypes = truncateWhereClauses(typeResult.whereClauses, kMaxExpandedReferencedTypes);
    if (!externTypeSummaries.empty())
        referencedTypes += (referencedTypes.empty() ? "" : "\n\n") + externTypeSummaries;

    // Rendered for the hovered type itself; types it referred to follow in their own block. Summaries
    // that stand on their own (a class, an extern type) don't go through this.
    bool showReferencedTypes = false;
    auto typeCodeBlock = [&](const std::string& body) -> std::string
    {
        showReferencedTypes = true;
        return codeBlock(codeLanguage, types::formatLongFunctionTypeLines(body));
    };

    // If we have a function and its corresponding name
    if (classMemberPrefix)
    {
        if (auto ftv = Luau::get<Luau::FunctionType>(*type))
        {
            types::ToStringNamedFunctionOpts funcOpts;
            funcOpts.hideTableKind = !config.hover.showTableKinds;
            funcOpts.multiline = config.hover.multilineFunctionDefinitions;
            typeString =
                typeCodeBlock(*classMemberPrefix + types::toStringNamedFunction(module, ftv, *classMemberName, scope, funcOpts));
        }
        else
        {
            typeString = typeCodeBlock(*classMemberPrefix + *classMemberName + ": " + typeString);
        }
    }
    else if (auto et = Luau::get<Luau::ExternType>(*type); et && et->parent == frontend.builtinTypes->classType)
    {
        // The class value itself: the summary *is* the answer here, rather than something the
        // hovered expression merely has the type of
        if (auto summary = buildClassFieldSummary(frontend, module, moduleName, *type, et, scope, config.hover.showTableKinds, true))
            typeString = codeBlock(codeLanguage, types::formatLongFunctionTypeLines(*summary)) + typeIdentityLine(*type, "");
        else
            typeString = typeCodeBlock(typeString);
    }
    else if (auto primitive = builtinPrimitive(frontend, *type);
             primitive && (!typeAliasInformation || typeAliasInformation->first == primitive->name))
    {
        // A primitive hovered directly, rather than through an alias that happens to resolve to one
        // -- that still reads better as `type Foo = object`
        typeString = codeBlock(codeLanguage, primitive->name) + "\n" + kDocumentationBreaker + primitive->docs;
    }
    else if (auto et = Luau::get<Luau::ExternType>(*type); et && et->parent != frontend.builtinTypes->objectType)
    {
        // A "declare extern type"-style extern type (e.g. a host-provided type like Instance), as
        // opposed to one of our user-defined `class`/`object` types handled above.
        typeString = codeBlock(codeLanguage, types::formatLongFunctionTypeLines(buildExternTypeSummary(module, *type, et, scope, config.hover.showTableKinds))) +
                     typeIdentityLine(*type, "");
    }
    else if (typeAliasInformation)
    {
        auto [typeName, typeFun] = typeAliasInformation.value();
        typeString = typeCodeBlock("type " + toStringTypeFun(typeName, typeFun) + " = " + typeString);
    }
    else if (auto ftv = Luau::get<Luau::FunctionType>(*type))
    {
        types::NameOrExpr name = "";
        if (auto localName = exprOrLocal.getName())
            name = localName->value;
        else if (auto expr = exprOrLocal.getExpr())
            name = expr;

        types::ToStringNamedFunctionOpts funcOpts;
        funcOpts.hideTableKind = !config.hover.showTableKinds;
        funcOpts.multiline = config.hover.multilineFunctionDefinitions;
        typeString = typeCodeBlock(types::toStringNamedFunction(module, ftv, name, scope, funcOpts));
    }
    else if (exprOrLocal.getLocal() || node->as<Luau::AstExprLocal>())
    {
        bool isConst = false;
        if (auto local = exprOrLocal.getLocal())
            isConst = local->isConst;
        else if (auto localExpr = node->as<Luau::AstExprLocal>())
            isConst = localExpr->local->isConst;

        std::string builder = isConst ? "const " : "local ";
        if (auto name = exprOrLocal.getName())
            builder += name->value;
        else
            builder += Luau::getIdentifier(node->asExpr()).value;
        builder += ": " + typeString;
        typeString = typeCodeBlock(builder);
    }
    else if (auto global = node->as<Luau::AstExprGlobal>())
    {
        // TODO: should we indicate this is a global somehow?
        std::string builder = "type ";
        builder += global->name.value;
        builder += " = " + typeString;
        typeString = typeCodeBlock(builder);
    }
    else if (auto string = node->as<Luau::AstExprConstantString>())
    {
        if (config.hover.includeStringLength)
        {
            auto byteLen = string->value.size;
            auto utf8Len = utflen(string->value.data, string->value.size);
            if (utf8Len && utf8Len != byteLen)
                typeString = codeBlock(codeLanguage, "string (" + std::to_string(byteLen) + " bytes, " + std::to_string(utf8Len.value()) + " characters)");
            else
                typeString = codeBlock(codeLanguage, "string (" + std::to_string(byteLen) + " bytes)");
        }
        else
            typeString = codeBlock(codeLanguage, "string");
    }
    else
    {
        typeString = typeCodeBlock(typeString);
    }

    std::string objectOfLine;
    if (objectOfType)
        objectOfLine = typeIdentityLine(*objectOfType, "*Object of* ");

    typeString = importedModuleLine + typeString + objectOfLine + memberOwnerLine;

    // Documentation comes before the types this one referred to: it's what the reader is actually
    // here to read, and a wide type's expansion would otherwise push it off the bottom
    if (std::optional<std::string> docs;
        documentationSymbol && (docs = printDocumentation(client->documentation, *documentationSymbol)) && docs && !docs->empty())
    {
        typeString += "\n" + kDocumentationBreaker;
        typeString += *docs;
    }
    else if (auto documentation = getDocumentationForType(*type); documentation && !documentation->empty())
    {
        typeString += "\n" + kDocumentationBreaker;
        typeString += *documentation;
    }
    else if (auto documentation = getDocumentationForAstNode(moduleName, node, scope); documentation && !documentation->empty())
    {
        typeString += "\n" + kDocumentationBreaker;
        typeString += *documentation;
    }
    else if (documentationLocation)
    {
        if (auto text = printMoonwaveDocumentation(getComments(documentationLocation->moduleName, documentationLocation->location)); !text.empty())
        {
            typeString += "\n" + kDocumentationBreaker;
            typeString += text;
        }
    }

    if (showReferencedTypes && (!referencedTypeLinks.empty() || !referencedTypes.empty()))
    {
        // A rule keeps the referenced types from reading as a continuation of whatever came above --
        // two code blocks butted up against each other look like one
        typeString += "\n" + kDocumentationBreaker;
        if (!referencedTypeLinks.empty())
            typeString += referencedTypeLinks + "\n";
        if (!referencedTypes.empty())
        {
            // With only the rule above it, a code block's first line sits right against the block's
            // top border and reads as clipped, so buy a little room with a blank first line. A
            // `References` line above already provides that room.
            std::string body = types::formatLongFunctionTypeLines(referencedTypes);
            if (referencedTypeLinks.empty())
                body = "\n" + body;

            typeString += "\n" + codeBlock(codeLanguage, body);
        }
    }

    // Without a range, the editor highlights the word under the cursor -- which inside a string
    // literal is one word of its contents rather than the string the hover is actually about
    std::optional<lsp::Range> hoverRange = std::nullopt;
    if (node->is<Luau::AstExprConstantString>() || node->is<Luau::AstExprConstantNumber>() || node->is<Luau::AstExprConstantBool>() ||
        node->is<Luau::AstExprInterpString>())
        hoverRange = textDocument->convertLocation(node->location);

    return lsp::Hover{{lsp::MarkupKind::Markdown, typeString}, hoverRange};
}
