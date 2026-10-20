import * as assert from "assert";
import { computeElseIndent } from "../../elseIndent";

suite("Else Indent Test Suite", () => {
  suite("computeElseIndent", () => {
    test("dedents an if statement's else to its if", () => {
      assert.strictEqual(
        computeElseIndent(["if a then", "    print(1)"], "    else", 4),
        "",
      );
      assert.strictEqual(
        computeElseIndent(
          ["    elseif b then", "        print(1)", ""],
          "        else",
          4,
        ),
        "    ",
      );
    });

    test("dedents an else typed right under its if", () => {
      assert.strictEqual(computeElseIndent(["if a then"], "    else", 4), "");
    });

    test("finds the if past a nested block", () => {
      assert.strictEqual(
        computeElseIndent(
          ["if a then", "    if b then", "        x()", "    end"],
          "    else",
          4,
        ),
        "",
      );
    });

    test("leaves a match's else level with its arms", () => {
      assert.strictEqual(
        computeElseIndent(
          ["const which = match x", '    case "p" then "patch"'],
          "    else",
          4,
        ),
        undefined,
      );
    });

    test("leaves a multi-line if-else expression's else level with its then", () => {
      assert.strictEqual(
        computeElseIndent(["local x = if a", "    then 1"], "    else", 4),
        undefined,
      );
    });

    test("leaves an else that's already placed", () => {
      assert.strictEqual(
        computeElseIndent(
          ["if a then", "    if b then", "        x()"],
          "    else",
          4,
        ),
        undefined,
      );
      assert.strictEqual(
        computeElseIndent(["if a then", "    x()"], "else", 4),
        undefined,
      );
    });

    test("only applies to a line that is just else", () => {
      assert.strictEqual(
        computeElseIndent(["if a then", "    x()"], "    elseif", 4),
        undefined,
      );
      assert.strictEqual(
        computeElseIndent(["if a then", "    x()"], "    else y()", 4),
        undefined,
      );
    });
  });
});
