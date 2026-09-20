-- Usage: seal scripts/luwu_docs.luau
-- needs seal 0.8.x or up

const json = require("@std/serde/json")
const str = require("@std/str")

class Table
    public documentation: string
    public keys: {
        [string]: string | Function
    }
end
class Function
    public documentation: string
    public params: { Parameter }?
    public returns: { Return }?
end
class Parameter
    public name: string
    public documentation: string
end
class Return
    public documentation: string
end

const function un(n: number, s: string)
    return str.unindent(s, "Spaces", 4 * n)
end

const luwu_libraries: { [string]: Table | Function } = {
    ["require"] = Function {
        documentation = un(3, [[
            Imports the module at `path`, returning what the module returned. Require paths use require-by-string syntax, which is described below.

            ```luau
            -- sibling of the requiring module/file.
            const utils = require("./utils")
            -- sibling of the parent of the requiring file/module.
            const doc = require("../doc")
            -- `@packages` is a require alias.
            const html = require("@packages/html")
            -- `@self` is a hardcoded alias only visible in `init` modules.
            const li = require("@self/li")
            ```
            On the filesystem, the term 'module' is mostly synonymous with 'file'. The term 'require-by-string module' may refer to either a file or
            a folder with a file named `init.luau`.

            ### Caching

            Requires are cached. The first time a module's required in your codebase, all top-level code in the module will be evaluated.
            Subsequent requires to the same module will return the cached value without re-evaluating the entire module. This allows modules
            to be used to keep track of state between multiple files in a codebase.

            ### Require-By-String syntax

            Luwu uses Luau's require-by-string syntax for module paths.
            
            Assume you have the following directory structure:

            ```
            project/
            ├── .luaurc
            ├── utils/
            │   ├── list.luau
            │   └── html/
            │       ├── elements.luau
            │       ├── serde.luau
            │       └── init.luau
            └── src/
                ├── parse_html.luau
                └── main.luau
            ```

            - To reach `parse_html.luau` from `main.luau`, use `require("./parse_html")`.
            - To reach `list.luau` from `main.luau` via relative path, use `require("../utils/list")`
            - To reach `html` from `parse_html.luau`, use `require("../utils/html")`. This is allowed because you can directly require a directory if it contains a file named `init.luau`.
            - To reach `elements.luau` from `init.luau`, use `require("@self/elements)`. You need `@self` here because `init.luau` is treated *as* its enclosing directory.
            - To reach `list.luau` from `html/init.luau`, use `require("./list")`.
            - To reach `elements.luau` from `serde.luau`, use `require("./elements")`.
            
            If `.luaurc` defines a require alias `"utils": "./utils"`:
            - To reach `list.luau` from `main.luau`, use `require("@utils/list)`
            - To reach `html` from `parse_html.luau`, use `require("@utils/html)`

            Keep in mind that not all embedders may support the full require-by-string specification, but relative requires and require-by-alias are typically supported.
            Require aliases that depend on other require aliases are in the spec but not yet commonly supported.

            On the filesystem, you can define your own require aliases by making a `.luaurc` (jsonc-syntax) or `.config.luau` file.
            Keep in mind some require aliases may be hardcoded by the embedder and cannot be overriden: for example, in *seal*,
            `@std` and `@interop` are hardcoded aliases.
        ]]),
        params = {
            Parameter {
                name = "path",
                documentation = "The require-by-string path of the module you want to require. These should start with `.`, `..`, or `@`."
            }
        },
        returns = {
            Return {
                documentation = "The value the module returned; this may be a cached value."
            }
        }
    },
    ["typeof"] = Function {
        documentation = un(3, [[
            Returns `value`'s embedder-defined or primitive type tag as a string.

            On primitives, this is generally the same string you get with `type`, except for `userdata` and `integer`.
            - For userdata (extern types), this returns the `__type` metafield which can be customized by the embedder.
            - For integers, this returns the specific integer representation: `type(19u16) == "integer"; typeof(19u16) == "u16"`.

            In the type system, you can use `typeof` to narrow on extern types:

            ```luau
            const t: number | Duration = get_dur()
            if typeof(t) == "Duration" then
                -- typechecker refines t to Duration here, no type error
                print(t:display())
            else
                -- typechecker knows t is a number here
                print(`{t} seconds`)
            end
            ```
        ]]),
        params = {
            Parameter {
                name = "value",
                documentation = "The value to get the typeof tag of."
            }
        },
        returns = {
            Return {
                documentation = "The typeof tag of the value"
            }
        }
    },
    ["class"] = Table {
        documentation = "Refinement and runtime reflection of Luwu classes and objects.",
        keys = {
            fields = Function {
                documentation = un(5, [[
                    Returns a table mapping the passed class or object's public fields with their values, as well
                    as a boolean describing whether the returned map is a 'complete' representation of the
                    fields available.

                    This function operates slightly differently on classes and objects. If the passed value
                    is an object, returns a map of the object's current fields and values. If any field has a
                    `nil` value, assigns `none` instead of `nil` so the field's name can be stored in the table.
                    Values in the returned map are shallow clones.

                    If the passed value is a `class`, all values of the map will be `none`, including those that have
                    default values. This is because we don't want `class.fields` to unintentionally initialize default values
                    (which could cause side effects).
                ]]),
                params = {
                    Parameter {
                        name = "o",
                        documentation = "An object or a class."
                    }
                },
                returns = {
                    Return {
                        documentation = "A map of public fields of the object or class, keyed by field name."
                    },
                    Return {
                        documentation = "Whether the map returned is a complete representation of the object's fields; this will be `true` if the class only has public fields and `false` if the class has any private fields."
                    }
                }
            },
            isinstance = Function {
                documentation = un(5, [[
                    Returns `true` if `o` is an object of class `c`.

                    If `o` is not an object, returns `false`. Raises a runtime error if `c` is not a `class`.

                    In the type system, you can use this to refine upon an object that could be of one or more classes:

                    ```luau
                    type Animal = Cat | Dog
                    class Cat(name: string, age: number, meows = 2) end
                    class Dog(name: string, age: number, barks = 3) end

                    const animal: Animal = get_animal()
                    if class.isinstance(animal, Cat) then
                        print(`found a Cat! {animal.name} with {animal.meows} meows`)
                    elseif class.isinstance(animal, Dog) then
                        print(`found a Dog! {animal.name} with {animal.barks} barks`)
                    end
                    ```
                    
                    Since Luwu doesn't support is-a inheritance relationships between classes, you can treat
                    this as an exact check that an object was instantiated from class `c`.
                ]]),
                params = {
                    Parameter {
                        name = "o",
                        documentation = "The object you want to test. Anything is accepted here, passing a non-object just causes this function to return `false`."
                    },
                    Parameter {
                        name = "c",
                        documentation = "The class you want to check against. Must be a `class`."
                    }
                },
                returns = {
                    Return {
                        documentation = "Whether the object `o` is an object of class `c.`"
                    }
                }
            },
            name = Function {
                documentation = "Returns the identifier name of the class or the class the object is an instance of. Keep in mind this name isn't globally unique, rely on class.isinstance tests instead.",
                params = {
                    Parameter {
                        name = "o",
                        documentation = "An object or a class you want the name of."
                    },
                },
                returns = {
                    Return {
                        documentation = "The identifier name of the class or object."
                    }
                }
            },
            of = Function {
                documentation = "Returns the `class` that `o` is an object of.",
                params = {
                    Parameter {
                        name = "o",
                        documentation = "An object."
                    },
                },
                returns = {
                    Return {
                        documentation = "If `o` is an object, then the object's class. Returns `nil` if `o` is not an object."
                    }
                }
            }
        }
    }
}

-- dont need to worry abt globaltype yet
const out: { [string]: unknown } = {}
const function parse(object: object, current_path: string)
    if class.isinstance(object, Table) then
        const res = {
            documentation = object.documentation,
            keys = {} :: { [string]: string }
        }
        for key, value in object.keys do
            if type(value) == "string" then
                res.keys[key] = value
                continue
            end
            const field_key = current_path .. "." .. key
            res.keys[key] = field_key
            -- this should add everything inside before we push table
            parse(value, field_key)
        end
        out[current_path] = res
    elseif class.isinstance(object, Function) then
        const res = {
            documentation = object.documentation,
            params = {},
            returns = {}
        }
        if object.params then
            for index, param in object.params do
                const param_path = current_path .. "/params/" .. (index - 1)
                table.insert(res.params, {
                    name = param.name,
                    documentation = param_path,
                })
                parse(param, param_path)
            end
        end
        if object.returns then
            for index, ret in object.returns do
                const ret_path = current_path .. "/returns/" .. (index - 1)
                table.insert(res.returns, ret_path)
                parse(ret, ret_path)
            end
        end
        out[current_path] = res
    elseif class.isinstance(object, Parameter) then
        const res = {
            documentation = object.documentation
        }
        out[current_path] = res
    elseif class.isinstance(object, Return) then
        const res = {
            documentation = object.documentation
        }
        out[current_path] = res
    end
end

const globals_path_prefix = "@luau/global/"
for key, object in luwu_libraries do
    parse(object, globals_path_prefix .. key)
end

json.writefile("./luwu_docs.json", out, {
    sorted = true,
})
