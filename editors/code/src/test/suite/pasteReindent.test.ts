import * as assert from "assert";
import * as fs from "fs";
import * as path from "path";
import {
  DECREASE_INDENT_PATTERN,
  INCREASE_INDENT_PATTERN,
  PasteSite,
  reindentPaste,
} from "../../pasteReindent";

const SPACES = { tabSize: 4, insertSpaces: true };
const TABS = { tabSize: 4, insertSpaces: false };

const paste = (
  pasted: string[],
  site: Partial<PasteSite>,
  options = SPACES,
): string[] | undefined =>
  reindentPaste(
    pasted.join("\n"),
    { before: "", after: "", previousLine: undefined, ...site },
    options,
  )?.split("\n");

suite("Paste Reindent Test Suite", () => {
  test("patterns match language-configuration.json", () => {
    const config = fs.readFileSync(
      path.join(__dirname, "../../../language-configuration.json"),
      "utf8",
    );
    const pattern = (name: string) =>
      JSON.parse(
        new RegExp(`"${name}":\\s*("(?:[^"\\\\]|\\\\.)*")`).exec(config)![1],
      );

    assert.strictEqual(
      INCREASE_INDENT_PATTERN.source,
      pattern("increaseIndentPattern"),
    );
    assert.strictEqual(
      DECREASE_INDENT_PATTERN.source,
      pattern("decreaseIndentPattern"),
    );
  });

  test("a block copied from its code, onto a blank line in a table", () => {
    assert.deepStrictEqual(
      paste(
        [
          "function push(self, msg: string)",
          "            table.insert(self, msg)",
          "        end,",
        ],
        { previousLine: "        give attach({}, {" },
      ),
      [
        "            function push(self, msg: string)",
        "                table.insert(self, msg)",
        "            end,",
      ],
    );
  });

  test("whole lines copied from deeper, at the cursor's indent", () => {
    assert.deepStrictEqual(
      paste(["        if x then", "            y()", "        end", ""], {
        before: "    ",
        previousLine: "function f()",
      }),
      ["if x then", "        y()", "    end", ""],
    );
  });

  test("a blank line at column 0 takes the surrounding indent", () => {
    assert.deepStrictEqual(
      paste(["local a = 1", "local b = 2"], {
        previousLine: "    local z = 0",
      }),
      ["    local a = 1", "    local b = 2"],
    );
  });

  test("a closer pasted after a block's body dedents", () => {
    assert.deepStrictEqual(
      paste(["end", "local c = 3"], { previousLine: "        b()" }),
      ["    end", "    local c = 3"],
    );
  });

  test("first line's indent is voted on past a dedent", () => {
    assert.deepStrictEqual(
      paste(
        ["function f()", "            a()", "        end", "        local z"],
        {
          before: "",
          previousLine: "local y",
        },
      ),
      ["function f()", "    a()", "end", "local z"],
    );
  });

  test("lines pasted in front of code keep that line's indent", () => {
    assert.deepStrictEqual(
      paste(["a()", "        b()", ""], {
        before: "",
        after: "    c()",
        previousLine: "do",
      }),
      ["    a()", "    b()", ""],
    );
  });

  test("continuation lines after code follow the line's indent", () => {
    assert.deepStrictEqual(
      paste(["{", "            a = 1,", "        }"], {
        before: "    local t = ",
      }),
      ["{", "        a = 1,", "    }"],
    );
  });

  test("a 2-space source keeps its columns", () => {
    assert.deepStrictEqual(
      paste(["if x then", "      y()", "      z()", "    end"], {
        previousLine: "do",
      }),
      ["    if x then", "      y()", "      z()", "    end"],
    );
  });

  test("indents with tabs when the editor does", () => {
    assert.deepStrictEqual(
      paste(
        ["if x then", "        y()", "    end"],
        { before: "\t", previousLine: "do" },
        TABS,
      ),
      ["if x then", "\t\ty()", "\tend"],
    );
  });

  test("leaves single lines and already-right pastes alone", () => {
    assert.strictEqual(paste(["local x = 1"], {}), undefined);
    assert.strictEqual(
      paste(["local a", "local b"], { previousLine: "local z" }),
      undefined,
    );
  });

  test("blank lines inside the paste lose their whitespace", () => {
    assert.deepStrictEqual(paste(["do", "        ", "    x()", "end"], {}), [
      "do",
      "",
      "    x()",
      "end",
    ]);
  });
});
