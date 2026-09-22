import * as assert from "assert";
import { computeLongBracketLevelFixup } from "../../longBracketAutoClose";

suite("Long Bracket Auto Close Test Suite", () => {
  suite("computeLongBracketLevelFixup", () => {
    test("adds the level markers to a long comment", () => {
      assert.strictEqual(computeLongBracketLevelFixup("--[=[", "]]"), "=");
      assert.strictEqual(computeLongBracketLevelFixup("--[==[", "]]"), "==");
      assert.strictEqual(
        computeLongBracketLevelFixup("    --[===[", "]]"),
        "===",
      );
    });

    test("adds the level markers to a long string", () => {
      assert.strictEqual(
        computeLongBracketLevelFixup("local s = [=[", "]]"),
        "=",
      );
    });

    test("leaves a level 0 long bracket alone", () => {
      assert.strictEqual(computeLongBracketLevelFixup("--[[", "]]"), undefined);
      assert.strictEqual(
        computeLongBracketLevelFixup("local s = [[", "]]"),
        undefined,
      );
    });

    test("does nothing when the bracket didn't auto-close", () => {
      assert.strictEqual(computeLongBracketLevelFixup("--[=[", ""), undefined);
      assert.strictEqual(
        computeLongBracketLevelFixup("--[=[", "] hello"),
        undefined,
      );
    });

    test("does nothing for an ordinary bracket", () => {
      assert.strictEqual(
        computeLongBracketLevelFixup("local x = t[", "]"),
        undefined,
      );
      assert.strictEqual(
        computeLongBracketLevelFixup("-- see t[", "]"),
        undefined,
      );
      assert.strictEqual(
        computeLongBracketLevelFixup("local x = t[a][", "]]"),
        undefined,
      );
    });
  });
});
