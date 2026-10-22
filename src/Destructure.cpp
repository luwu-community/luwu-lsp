#include "LSP/Destructure.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "Luau/Lexer.h"
#include "Luau/Scope.h"
#include "LSP/LuauExt.hpp"
#include "LSP/Utils.hpp"

namespace Luau::LanguageServer::Destructure
{

// Deleting a statement by deleting its lines is only safe when nothing else is on them. A binding in an `if local`
// chain is a declaration inside the `if` line, and a statement in a one-line `do` (a block or an expression) shares its
// line with the code around it: deleting their lines would delete that code too.
bool locationOwnsItsLines(const TextDocument& textDocument, const Luau::Location& location)
{
    std::string first = textDocument.getLine(location.begin.line);
    for (size_t i = 0; i < location.begin.column && i < first.size(); ++i)
    {
        if (!isspace(static_cast<unsigned char>(first[i])))
            return false;
    }

    std::string last = textDocument.getLine(location.end.line);
    for (size_t i = location.end.column; i < last.size(); ++i)
    {
        // a trailing comment goes with the statement
        if (last.compare(i, 2, "--") == 0)
            break;

        if (!isspace(static_cast<unsigned char>(last[i])) && last[i] != ';')
            return false;
    }

    return true;
}

// The leading whitespace of a source line, used to match the document's existing indentation
std::string getLineIndentation(const TextDocument& textDocument, size_t line)
{
    std::string lineText = textDocument.getLine(line);
    size_t indent = 0;
    while (indent < lineText.size() && (lineText[indent] == ' ' || lineText[indent] == '\t'))
        ++indent;
    return lineText.substr(0, indent);
}

// Splits a parameter list's contents on top-level commas, so commas inside a parameter's own type
// (a table type, a generic argument list, a function type) don't split it.
std::vector<std::string> splitParameters(const std::string& parameters)
{
    std::vector<std::string> result;
    int depth = 0;
    char stringDelimiter = '\0';
    size_t start = 0;

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        char c = parameters[i];

        if (stringDelimiter != '\0')
        {
            if (c == '\\')
                ++i;
            else if (c == stringDelimiter)
                stringDelimiter = '\0';
            continue;
        }

        if (c == '"' || c == '\'')
            stringDelimiter = c;
        else if (c == '(' || c == '{' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == '}' || c == ']' || c == '>')
            --depth;
        else if (c == ',' && depth == 0)
        {
            result.push_back(parameters.substr(start, i - start));
            start = i + 1;
        }
    }

    result.push_back(parameters.substr(start));

    for (auto& parameter : result)
        trim(parameter);

    return result;
}

// One level of indentation, as the document already uses it: the leading whitespace of the first
// indented line, falling back to four spaces.
std::string detectIndentUnit(const TextDocument& textDocument)
{
    for (size_t line = 0; line < textDocument.lineCount(); ++line)
    {
        std::string lineText = textDocument.getLine(line);
        size_t indent = 0;
        while (indent < lineText.size() && (lineText[indent] == ' ' || lineText[indent] == '\t'))
            ++indent;

        // Skip blank (or whitespace-only) lines, and continuation lines of an already-wrapped construct
        if (indent == 0 || indent == lineText.size())
            continue;

        if (lineText[0] == '\t')
            return "\t";

        return std::string(indent, ' ');
    }

    return "    ";
}

std::string layOutOnOneLine(const std::vector<std::string>& elements)
{
    std::string text;
    for (size_t i = 0; i < elements.size(); ++i)
        text += (i == 0 ? "" : ", ") + elements[i];
    return text;
}

// A pattern's `.` (or whatever comes before its `{`) and its fields as written, split on top-level commas, with a
// trailing comma dropped. None when it holds a comment, which a new layout could put in front of code.
std::optional<std::pair<std::string, std::vector<std::string>>> splitDestructurePattern(const std::string& text)
{
    size_t open = text.find('{');
    if (open == std::string::npos || text.size() < open + 2 || text.back() != '}' || text.find("--") != std::string::npos)
        return std::nullopt;

    auto fields = splitParameters(text.substr(open + 1, text.size() - open - 2));
    // `elements.{}` has no fields yet, and importing into it fills it
    if (!fields.empty() && fields.back().empty())
        fields.pop_back();

    return std::make_pair(text.substr(0, open), fields);
}

// `.{ a, b.{ c, d } }`: the pattern and every pattern nested in it on one line
std::optional<std::string> layOutDestructureOnOneLine(
    const Luau::Location& location, const Luau::AstDestructurePattern* pattern, const TextDocument& textDocument)
{
    auto split = splitDestructurePattern(textDocument.getText(textDocument.convertLocation(location)));
    if (!split || split->second.empty())
        return std::nullopt;
    auto& [opening, fields] = *split;

    if (pattern && pattern->fields.size != fields.size())
        return std::nullopt;

    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (pattern && pattern->fields.data[i].target.location)
        {
            const Luau::AstDestructurePattern& target = pattern->fields.data[i].target;
            std::string nested = textDocument.getText(textDocument.convertLocation(*target.location));
            size_t at = fields[i].find(nested);
            auto inner = layOutDestructureOnOneLine(*target.location, &target, textDocument);
            if (at == std::string::npos || !inner)
                return std::nullopt;
            fields[i] = fields[i].substr(0, at) + *inner + fields[i].substr(at + nested.size());
        }

        if (fields[i].find('\n') != std::string::npos)
            return std::nullopt;
    }

    return opening + "{ " + layOutOnOneLine(fields) + " }";
}

// The pattern one field per line, each with a trailing comma, and the `}` back at the indentation of the line the
// pattern starts on. Nested patterns keep their layout.
std::string layOutFieldsOnePerLine(const std::string& opening, const std::vector<std::string>& fields, size_t line, const TextDocument& textDocument)
{
    std::string baseIndent = getLineIndentation(textDocument, line);
    std::string fieldIndent = baseIndent + detectIndentUnit(textDocument);

    std::string text = opening + "{\n";
    for (const auto& field : fields)
        text += fieldIndent + field + ",\n";
    return text + baseIndent + "}";
}

std::optional<std::string> layOutDestructureOnePerLine(const Luau::Location& location, const TextDocument& textDocument)
{
    auto split = splitDestructurePattern(textDocument.getText(textDocument.convertLocation(location)));
    if (!split || split->second.empty())
        return std::nullopt;
    return layOutFieldsOnePerLine(split->first, split->second, location.begin.line, textDocument);
}

// The pattern at `location` with `fields` in place of the ones it has, in the layout it already has
std::string layOutFieldsLike(
    const Luau::Location& location, const std::string& opening, const std::vector<std::string>& fields, const TextDocument& textDocument)
{
    if (location.begin.line == location.end.line)
        return opening + "{ " + layOutOnOneLine(fields) + " }";
    return layOutFieldsOnePerLine(opening, fields, location.begin.line, textDocument);
}

bool isHiddenDestructureLocal(const Luau::AstLocal* local)
{
    return local && strcmp(local->name.value, Luau::kDestructuredLocalName) == 0;
}

bool isIdentifier(const std::string& name)
{
    if (name.empty() || isdigit(static_cast<unsigned char>(name[0])))
        return false;
    for (char c : name)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return !Luau::Lexer::isReserved(name);
}

// A class or trait value, which brings its type in along with it
bool bringsAType(std::optional<Luau::TypeId> ty, Luau::NotNull<Luau::BuiltinTypes> builtinTypes)
{
    const auto* et = ty ? Luau::get<Luau::ExternType>(Luau::follow(*ty)) : nullptr;
    return et && (et->parent == builtinTypes->classType || et->parent == builtinTypes->traitType);
}

// Whether binding `name` would capture or shadow anything: any other local or global of that name anywhere in the file,
// or, when the binding brings a type in, a type of that name
bool nameIsTaken(const std::string& name, bool withType, const Luau::AstLocal* except, const Luau::Module& module, const DestructureFacts& facts)
{
    if (!isIdentifier(name) || facts.globals.count(name))
        return true;

    for (const auto& [_, scope] : module.scopes)
    {
        for (const auto& [symbol, _] : scope->bindings)
            if (symbol.local != except && name == symbol.c_str())
                return true;
        if (withType && (scope->privateTypeBindings.count(name) || scope->exportedTypeBindings.count(name)))
            return true;
    }
    return false;
}

std::optional<Luau::TypeId> typeOfLocal(Luau::AstLocal* local, const Luau::Module& module, const DestructureFacts& facts)
{
    if (auto it = facts.declarations.find(local); it != facts.declarations.end() && it->second->values.size == 1)
        if (auto ty = module.astTypes.find(it->second->values.data[0]))
            return *ty;
    if (auto scope = Luau::findScopeAtPosition(module, local->location.end))
        return scope->lookup(local);
    return std::nullopt;
}

std::optional<Luau::TypeId> fieldType(std::optional<Luau::TypeId> ty, const std::string& name)
{
    const auto* table = ty ? Luau::get<Luau::TableType>(Luau::follow(*ty)) : nullptr;
    if (!table)
        return std::nullopt;
    auto prop = table->props.find(name);
    return prop == table->props.end() ? std::nullopt : prop->second.readTy;
}

// The local a pattern's fields can be read through as `local.field`: the pattern's own name, or for an unnamed
// `const .{...} = chimera`, the local it destructures
Luau::AstLocal* patternQualifier(const Luau::AstDestructurePattern& pattern, const DestructureFacts& facts)
{
    if (!isHiddenDestructureLocal(pattern.local))
        return pattern.local;
    auto it = facts.declarations.find(pattern.local);
    if (it == facts.declarations.end() || it->second->values.size != 1)
        return nullptr;
    auto* value = it->second->values.data[0]->as<Luau::AstExprLocal>();
    return value ? value->local : nullptr;
}

// Edits turning every `qualifier.name` in the file (and, for a class, every `qualifier.name` type) into `name`. None
// when one is assigned to, which a bare name would turn into assigning the import.
std::optional<std::vector<lsp::TextEdit>> unqualifyEdits(
    Luau::AstLocal* qualifier, const std::string& name, bool withType, const DestructureFacts& facts, const TextDocument& textDocument)
{
    std::vector<lsp::TextEdit> edits;
    for (auto* index : facts.indexes)
    {
        auto* object = index->expr->as<Luau::AstExprLocal>();
        if (!object || object->local != qualifier || index->op != '.' || name != index->index.value)
            continue;
        if (facts.assigned.count(index))
            return std::nullopt;
        edits.push_back({textDocument.convertLocation({index->expr->location.begin, index->indexLocation.begin}), ""});
    }

    if (withType)
        for (auto* ref : facts.typeRefs)
            if (ref->prefix && ref->prefixLocation && name == ref->name.value &&
                (ref->prefixLocal ? ref->prefixLocal == qualifier : *ref->prefix == qualifier->name))
                edits.push_back({textDocument.convertLocation({ref->prefixLocation->begin, ref->nameLocation.begin}), ""});

    return edits;
}

bool isPatternField(const Luau::AstLocal* local, const DestructureFacts& facts)
{
    for (const auto& entry : facts.patterns)
        for (const auto& field : entry.first->fields)
            if (field.target.local == local)
                return true;
    return false;
}

static bool isRead(const Luau::AstLocal* local, const DestructureFacts& facts)
{
    return std::any_of(facts.localRefs.begin(), facts.localRefs.end(),
        [&](const Luau::AstExprLocal* ref)
        {
            return ref->local == local;
        });
}

// The text `pattern` becomes without the fields binding the locals in `drop`, nested patterns included: empty when no
// field is left, and none when it can't be rewritten
static std::optional<std::string> patternWithout(const Luau::AstDestructurePattern& pattern, const std::unordered_set<const Luau::AstLocal*>& drop,
    const std::unordered_set<const Luau::AstLocal*>& alsoRead, const DestructureFacts& facts, const TextDocument& textDocument)
{
    std::string original = textDocument.getText(textDocument.convertLocation(*pattern.location));
    auto split = splitDestructurePattern(original);
    if (!split || split->second.size() != pattern.fields.size)
        return std::nullopt;

    std::vector<std::string> kept;
    bool changed = false;

    for (size_t i = 0; i < pattern.fields.size; ++i)
    {
        const auto& field = pattern.fields.data[i];
        std::string text = split->second[i];

        if (field.target.location)
        {
            std::string nested = textDocument.getText(textDocument.convertLocation(*field.target.location));
            auto inner = patternWithout(field.target, drop, alsoRead, facts, textDocument);
            size_t at = text.find(nested);
            if (!inner || at == std::string::npos)
                return std::nullopt;

            if (*inner != nested)
            {
                changed = true;

                // `components.{}` left: `components` stays bound on its own while something reads it
                const Luau::AstLocal* key = field.target.local;
                if (inner->empty() && (!key || isHiddenDestructureLocal(key) || drop.count(key) || (!isRead(key, facts) && !alsoRead.count(key))))
                    continue;

                text = text.substr(0, at) + *inner + text.substr(at + nested.size());
            }
        }
        else if (field.target.local && drop.count(field.target.local))
        {
            changed = true;
            continue;
        }

        kept.push_back(text);
    }

    if (!changed)
        return original;
    if (kept.empty())
        return "";
    return layOutFieldsLike(*pattern.location, split->first, kept, textDocument);
}

std::vector<lsp::TextEdit> removeFieldsEdits(const std::unordered_set<const Luau::AstLocal*>& drop, const DestructureFacts& facts,
    const TextDocument& textDocument, std::unordered_set<const Luau::AstLocal*>* removed, const std::unordered_set<const Luau::AstLocal*>& alsoRead)
{
    std::vector<lsp::TextEdit> edits;

    for (const auto& [pattern, holder] : facts.topPatterns)
    {
        if (!pattern->location)
            continue;

        std::vector<const Luau::AstLocal*> inside;
        for (const auto* local : drop)
            if (pattern->location->encloses(local->location))
                inside.push_back(local);
        if (inside.empty())
            continue;

        lsp::Range range = textDocument.convertLocation(*pattern->location);
        auto text = patternWithout(*pattern, drop, alsoRead, facts, textDocument);
        if (!text || *text == textDocument.getText(range))
            continue;

        std::optional<lsp::TextEdit> edit;
        if (!text->empty())
            edit = lsp::TextEdit{range, *text};
        else if (!isHiddenDestructureLocal(pattern->local) && (isRead(pattern->local, facts) || alsoRead.count(pattern->local)))
            // `const c.{ Card } = v` with `c` still read: `const c = v`
            edit = lsp::TextEdit{range, ""};
        else if (auto* statement = holder->as<Luau::AstStatLocal>();
            statement && statement->destructure == pattern && locationOwnsItsLines(textDocument, statement->location))
            edit = lsp::TextEdit{{{statement->location.begin.line, 0}, {statement->location.end.line + 1, 0}}, ""};

        if (!edit)
            continue;

        edits.push_back(*edit);
        if (removed)
            removed->insert(inside.begin(), inside.end());
    }

    return edits;
}

static bool isRequireCall(const Luau::AstExpr* expr)
{
    const auto* call = expr->as<Luau::AstExprCall>();
    const auto* func = call ? call->func->as<Luau::AstExprGlobal>() : nullptr;
    return func && strcmp(func->name.value, "require") == 0;
}

// Whether `expr` holds what a `require` loads: the call itself, a local declared with one (`chimera`), or a field read
// from either (`chimera.components`)
static bool holdsRequiredValue(const Luau::AstExpr* expr, const DestructureFacts& facts, size_t depth = 0)
{
    if (depth > 8)
        return false;
    if (isRequireCall(expr))
        return true;
    if (const auto* index = expr->as<Luau::AstExprIndexName>())
        return holdsRequiredValue(index->expr, facts, depth + 1);

    const auto* ref = expr->as<Luau::AstExprLocal>();
    auto declaration = ref ? facts.declarations.find(ref->local) : facts.declarations.end();
    if (declaration == facts.declarations.end())
        return false;
    const Luau::AstStatLocal* statement = declaration->second;
    for (size_t i = 0; i < statement->vars.size && i < statement->values.size; ++i)
        if (statement->vars.data[i] == ref->local)
            return holdsRequiredValue(statement->values.data[i], facts, depth + 1);
    return false;
}

bool isImportedByPattern(const Luau::AstLocal* field, const DestructureFacts& facts)
{
    for (const auto& [pattern, holder] : facts.topPatterns)
    {
        if (!pattern->location || !pattern->location->encloses(field->location))
            continue;
        const auto* statement = holder->as<Luau::AstStatLocal>();
        return statement && statement->values.size == 1 && holdsRequiredValue(statement->values.data[0], facts);
    }
    return false;
}

static const Luau::TableType* tableOf(std::optional<Luau::TypeId> ty)
{
    return ty ? Luau::get<Luau::TableType>(Luau::follow(*ty)) : nullptr;
}

// Completion filters by what's typed, and its first character has to match, so nothing else is worth sending
static bool startsLike(const std::string& name, const std::string& typed)
{
    return !typed.empty() && !name.empty() && tolower(static_cast<unsigned char>(name[0])) == tolower(static_cast<unsigned char>(typed[0]));
}

// Whether `pattern` or a pattern nested in it binds a local named `name`
static bool patternBinds(const Luau::AstDestructurePattern& pattern, const std::string& name)
{
    if (pattern.local && !isHiddenDestructureLocal(pattern.local) && name == pattern.local->name.value)
        return true;
    for (const auto& field : pattern.fields)
        if (patternBinds(field.target, name))
            return true;
    return false;
}

// A require's path as a name to give the value it loads: `chimera` for "@chimera", `elements` for "./src/elements"
static std::string nameForRequire(const Luau::AstExpr* call)
{
    const auto* args = call->as<Luau::AstExprCall>();
    const auto* path = args && args->args.size == 1 ? args->args.data[0]->as<Luau::AstExprConstantString>() : nullptr;
    if (!path)
        return "";

    std::string text(path->value.data, path->value.size);
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    text = text.substr(text.find_last_of('/') == std::string::npos ? 0 : text.find_last_of('/') + 1);
    if (size_t dot = text.find('.'); dot != std::string::npos && dot > 0)
        text = text.substr(0, dot);

    std::string name;
    for (char c : text)
        if (isalnum(static_cast<unsigned char>(c)) || c == '_')
            name += c;
        else if (c != '@' && !name.empty())
            name += '_';
    return name;
}

// Luwu Destructuring: the type aliases the modules held at the top of the file export, each imported by a type
// destructuring of its module. An existing `type .{ ... } = m` takes it first, then a new `type .{ Content } = m` under a
// local holding the module, then a pattern on a `require` that gets a name for it: `const chimera.{ ... } = require(...)`.
static void offerTypeAliases(const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::Position& position,
    const std::string& typed, const TextDocument& textDocument, const DestructureFacts& facts,
    const std::function<Luau::ModulePtr(const Luau::Location&)>& requiredModule,
    const std::function<bool(const std::string&, bool)>& available, std::map<std::string, ImportCandidate>& best)
{
    auto moduleScope = module.getModuleScope();
    auto scope = Luau::findScopeAtPosition(module, position);

    auto offer = [&](const std::string& name, const std::string& from, lsp::TextEdit edit)
    {
        if (best.count(name) || !isIdentifier(name) || !startsLike(name, typed) || (scope && scope->lookupType(name)))
            return;
        if (moduleScope->exportedTypeBindings.count(name) || moduleScope->privateTypeBindings.count(name))
            return;
        best.emplace(name, ImportCandidate{name, from, std::nullopt, std::move(edit), /* isType= */ true});
    };

    auto exportedBy = [&](const std::string& local) -> const std::unordered_map<Luau::Name, Luau::TypeFun>*
    {
        auto it = moduleScope->importedTypeBindings.find(local);
        return it == moduleScope->importedTypeBindings.end() ? nullptr : &it->second;
    };

    auto beforeCursor = [&](const Luau::AstStat* stat)
    {
        return stat->location.end < position;
    };

    // `type .{ Style } = chimera` already written
    for (Luau::AstStat* stat : sourceModule.root->body)
    {
        auto* alias = stat->as<Luau::AstStatTypeAlias>();
        if (!alias || !alias->destructure || !alias->destructure->closed || !beforeCursor(alias))
            continue;
        const Luau::AstTypeDestructure& destructure = *alias->destructure;
        const auto* types = exportedBy(destructure.module.value ? destructure.module.value : "");
        auto split = splitDestructurePattern(textDocument.getText(textDocument.convertLocation(destructure.location)));
        if (!types || !split)
            continue;
        for (const auto& [name, _] : *types)
        {
            auto fields = split->second;
            fields.push_back(name);
            offer(name, destructure.module.value,
                {textDocument.convertLocation(destructure.location), layOutFieldsLike(destructure.location, split->first, fields, textDocument)});
        }
    }

    // A local holding a module: `const chimera = require(...)`, or one a pattern binds, like `elements` in `elements.{}`
    for (Luau::AstStat* stat : sourceModule.root->body)
    {
        auto* declaration = stat->as<Luau::AstStatLocal>();
        if (!declaration || declaration->isDesugaredField() || !beforeCursor(declaration))
            continue;

        std::vector<std::string> names;
        if (declaration->destructure)
        {
            for (const auto& [local, types] : moduleScope->importedTypeBindings)
                if (isIdentifier(local) && patternBinds(*declaration->destructure, local))
                    names.push_back(local);
        }
        else
            for (Luau::AstLocal* var : declaration->vars)
                names.push_back(var->name.value);

        std::string indent = getLineIndentation(textDocument, declaration->location.begin.line);
        lsp::Position at{declaration->location.end.line + 1, 0};
        for (const auto& local : names)
            if (const auto* types = exportedBy(local))
                for (const auto& [name, _] : *types)
                    offer(name, local, {{at, at}, indent + "type .{ " + name + " } = " + local + "\n"});
    }

    // `const .{ ... } = require(...)`, whose value has no name to read types from yet
    if (!requiredModule)
        return;
    for (const auto& [pattern, holder] : facts.topPatterns)
    {
        auto* declaration = holder->as<Luau::AstStatLocal>();
        if (!declaration || !pattern->location || !isHiddenDestructureLocal(pattern->local) || !beforeCursor(declaration) ||
            declaration->values.size != 1 || !isRequireCall(declaration->values.data[0]) ||
            std::find(sourceModule.root->body.begin(), sourceModule.root->body.end(), declaration) == sourceModule.root->body.end())
            continue;

        std::string local = nameForRequire(declaration->values.data[0]);
        auto required = requiredModule(declaration->values.data[0]->location);
        if (!isIdentifier(local) || !available(local, false) || !required)
            continue;

        Luau::Location rest{pattern->location->begin, declaration->location.end};
        std::string text = local + textDocument.getText(textDocument.convertLocation(rest)) + "\n" +
                           getLineIndentation(textDocument, declaration->location.begin.line) + "type .{ ";
        for (const auto& [name, _] : required->exportedTypeBindings)
            offer(name, local, {textDocument.convertLocation(rest), text + name + " } = " + local});
    }
}

std::vector<ImportCandidate> importCandidates(const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::Position& position,
    const std::string& typed, bool typesOnly, Luau::NotNull<Luau::BuiltinTypes> builtinTypes, const TextDocument& textDocument,
    const std::vector<UnimportedModule>& unimported, const std::function<Luau::ModulePtr(const Luau::Location&)>& requiredModule)
{
    DestructureFacts facts;
    sourceModule.root->visit(&facts);

    auto scope = Luau::findScopeAtPosition(module, position);
    auto moduleScope = module.getModuleScope();

    // Free to bind at the top of the module: not visible here already, not declared at the top level, not a global
    // the file uses, and for a class, no type of that name
    auto available = [&](const std::string& name, bool withType)
    {
        // the name being typed is a global until it is imported
        if (!isIdentifier(name) || (name != typed && facts.globals.count(name)) || (scope && scope->linearSearchForBinding(name)))
            return false;
        for (const auto& binding : moduleScope->bindings)
            if (name == binding.first.c_str())
                return false;
        return !withType || (!moduleScope->privateTypeBindings.count(name) && !moduleScope->exportedTypeBindings.count(name));
    };

    // By name, the first offer kept: fields of patterns already written come before new declarations
    std::map<std::string, ImportCandidate> best;
    // False once the offers come from modules not required yet
    bool offeringRequired = true;
    auto offer = [&](const std::string& name, const std::string& from, std::optional<Luau::TypeId> type, lsp::TextEdit edit)
    {
        bool withType = bringsAType(type, builtinTypes);
        if (best.count(name) || !startsLike(name, typed) || (typesOnly && !withType) || !available(name, withType))
            return;
        if (Luau::Location{textDocument.convertPosition(edit.range.start), textDocument.convertPosition(edit.range.end)}.containsClosed(position))
            return;
        best.emplace(name, ImportCandidate{name, from, type, std::move(edit), /* isType= */ false, offeringRequired});
    };

    auto atTopBeforeCursor = [&](const Luau::AstNode* node)
    {
        return node->location.end < position &&
               std::find(sourceModule.root->body.begin(), sourceModule.root->body.end(), node) != sourceModule.root->body.end();
    };

    // A field of `table` and, through it, the fields of a table it holds, two levels down: `name` or `name.{ inner }`
    auto offerFields =
        [&](const Luau::TableType* table, const std::string& from, const std::function<std::optional<lsp::TextEdit>(const std::string&)>& add)
    {
        for (const auto& [name, prop] : table->props)
        {
            if (auto edit = add(name))
                offer(name, from, prop.readTy, *edit);

            const Luau::TableType* inner = tableOf(prop.readTy);
            if (!inner || !available(name, false))
                continue;
            for (const auto& [innerName, innerProp] : inner->props)
                if (startsLike(innerName, typed))
                    if (auto edit = add(name + ".{ " + innerName + " }"))
                        offer(innerName, from + "." + name, innerProp.readTy, *edit);
        }
    };

    if (typesOnly)
        offerTypeAliases(sourceModule, module, position, typed, textDocument, facts, requiredModule, available, best);

    // Patterns at the top of the module, nested ones included, each offering what it doesn't bind yet
    for (const auto& entry : facts.patterns)
    {
        const Luau::AstDestructurePattern* pattern = entry.first;
        const Luau::AstNode* holder = entry.second;
        if (!holder->is<Luau::AstStatLocal>() || !atTopBeforeCursor(holder))
            continue;

        const Luau::TableType* table = tableOf(typeOfLocal(pattern->local, module, facts));
        auto split = splitDestructurePattern(textDocument.getText(textDocument.convertLocation(*pattern->location)));
        if (!table || !split)
            continue;

        std::string from;
        if (Luau::AstLocal* qualifier = patternQualifier(*pattern, facts))
            from = qualifier->name.value;
        else if (auto declaration = facts.declarations.find(pattern->local); declaration != facts.declarations.end())
            from = textDocument.getText(textDocument.convertLocation(declaration->second->values.data[0]->location));

        auto field = [&](const std::string& key) -> const Luau::AstDestructureField*
        {
            for (const auto& f : pattern->fields)
                if (key == f.key.value)
                    return &f;
            return nullptr;
        };

        offerFields(table, from,
            [&](const std::string& text) -> std::optional<lsp::TextEdit>
            {
                std::string key = text.substr(0, text.find(".{"));
                const Luau::AstDestructureField* existing = field(key);
                if (!existing)
                {
                    auto fields = split->second;
                    fields.push_back(text);
                    return lsp::TextEdit{
                        textDocument.convertLocation(*pattern->location), layOutFieldsLike(*pattern->location, split->first, fields, textDocument)};
                }

                // `elements` already bound on its own gets a pattern: `elements.{ Paragraph }`. One with a pattern of its
                // own offers its fields itself.
                const Luau::AstLocal* local = existing->target.local;
                if (key == text || existing->target.location || existing->asLocation || !local || local->annotation)
                    return std::nullopt;
                lsp::Position at = textDocument.convertPosition(local->location.end);
                return lsp::TextEdit{{at, at}, text.substr(key.size())};
            });
    }

    // `const chimera = require(...)` at the top, not destructured yet: it gets a pattern of its own in place,
    // `const chimera.{...} = require(...)`. One with a type annotation gets a new `const .{...} = chimera` under it.
    for (Luau::AstStat* stat : sourceModule.root->body)
    {
        auto* declaration = stat->as<Luau::AstStatLocal>();
        if (!declaration || declaration->destructure || declaration->isDesugaredField() || declaration->vars.size != 1 ||
            declaration->values.size != 1 || !isRequireCall(declaration->values.data[0]) || !atTopBeforeCursor(declaration))
            continue;

        Luau::AstLocal* var = declaration->vars.data[0];
        bool destructured = std::any_of(facts.patterns.begin(), facts.patterns.end(),
            [&](const auto& entry)
            {
                return patternQualifier(*entry.first, facts) == var;
            });
        const Luau::TableType* table = tableOf(typeOfLocal(var, module, facts));
        if (destructured || !table)
            continue;

        lsp::Position after = textDocument.convertPosition(var->location.end);
        lsp::Position below{declaration->location.end.line + 1, 0};
        std::string keyword = declaration->isConst ? "const " : "local ";
        offerFields(table, var->name.value,
            [&](const std::string& text) -> std::optional<lsp::TextEdit>
            {
                if (!var->annotation)
                    return lsp::TextEdit{{after, after}, ".{ " + text + " }"};
                return lsp::TextEdit{{below, below}, keyword + ".{ " + text + " } = " + var->name.value + "\n"};
            });
    }

    // Modules not required yet: a new `const .{...} = require(...)` where the require would go. A field that is a module
    // of its own (`fs` of "@std", when "@std/fs" can be required) is left to that module's require: requiring all of
    // "@std" for it loads every module it holds.
    std::unordered_set<std::string> requirable(facts.requirePaths.begin(), facts.requirePaths.end());
    for (const auto& candidate : unimported)
        requirable.insert(candidate.requirePath);
    offeringRequired = false;

    for (const auto& candidate : unimported)
    {
        const Luau::TableType* table = tableOf(candidate.type);
        if (!table || facts.requirePaths.count(candidate.requirePath))
            continue;

        offerFields(table, candidate.requirePath,
            [&](const std::string& text) -> std::optional<lsp::TextEdit>
            {
                if (requirable.count(candidate.requirePath + "/" + text.substr(0, text.find(".{"))))
                    return std::nullopt;
                std::string declaration =
                    std::string(candidate.useConst ? "const " : "local ") + ".{ " + text + " } = require(\"" + candidate.requirePath + "\")\n";
                return lsp::TextEdit{candidate.range, (candidate.prependNewline ? "\n" : "") + declaration};
            });
    }

    std::vector<ImportCandidate> result;
    for (auto& [_, candidate] : best)
        result.push_back(std::move(candidate));
    return result;
}

} // namespace Luau::LanguageServer::Destructure
