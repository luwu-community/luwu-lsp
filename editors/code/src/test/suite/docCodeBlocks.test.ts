import * as assert from "assert";
import { findDocCodeBlocks } from "../../docCodeBlocks";

const blocks = (source: string) => findDocCodeBlocks(source.split("\n"));

suite("Doc Code Blocks Test Suite", () => {
  test("a fence in a block comment, fence lines included", () => {
    assert.deepStrictEqual(
      blocks(
        [
          "--[=[",
          "    Example:",
          "    ```luwu",
          "    local x = 1",
          "    ```",
          "]=]",
          "local y = 2",
        ].join("\n"),
      ),
      [{ start: 2, end: 4 }],
    );
  });

  test("a fence across --- lines", () => {
    assert.deepStrictEqual(
      blocks(
        [
          "--- Example:",
          "--- ```",
          "--- local x = 1",
          "--- ```",
          "local function f() end",
        ].join("\n"),
      ),
      [{ start: 1, end: 3 }],
    );
  });

  test("a --- fence the doc comment never closes stops at its last line", () => {
    assert.deepStrictEqual(
      blocks(["--- ```luau", "--- local x = 1", "local y = 2"].join("\n")),
      [{ start: 0, end: 1 }],
    );
  });

  test("an untagged fence in a block comment is prose, like the grammar", () => {
    assert.deepStrictEqual(
      blocks(["--[[", "```", "text", "```", "]]"].join("\n")),
      [],
    );
  });

  test("fences outside comments don't count", () => {
    assert.deepStrictEqual(
      blocks(
        [
          'local s = "--[[ ```luwu"',
          "local t = [==[",
          "```luwu",
          "x",
          "```",
          "]==]",
          "local u = `--[[`",
        ].join("\n"),
      ),
      [],
    );
  });

  test("a block comment opened after code on the same line", () => {
    assert.deepStrictEqual(
      blocks(
        ["local x = 1 --[[ ```luwu", "local y = 2", "```", "]]"].join("\n"),
      ),
      [{ start: 0, end: 2 }],
    );
  });

  test("a one-line block comment doesn't swallow the next lines", () => {
    assert.deepStrictEqual(
      blocks(
        ["--[[ ```luwu ]] local x = 1", "--- ```", "--- y", "--- ```"].join(
          "\n",
        ),
      ),
      [{ start: 1, end: 3 }],
    );
  });
});
