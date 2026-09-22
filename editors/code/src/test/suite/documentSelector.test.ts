import * as assert from "assert";
import { buildDocumentSelector } from "../../extension";

suite("Document Selector Test Suite", () => {
  test("serves every language by default", () => {
    assert.deepStrictEqual(buildDocumentSelector(["luwu", "luau", "lua"]), [
      { language: "luwu", scheme: "file" },
      { language: "luwu", scheme: "untitled" },
      { language: "luau", scheme: "file" },
      { language: "luau", scheme: "untitled" },
      { language: "lua", scheme: "file" },
      { language: "lua", scheme: "untitled" },
    ]);
  });

  test("leaves lua and luau alone when they are dropped", () => {
    assert.deepStrictEqual(buildDocumentSelector(["luwu"]), [
      { language: "luwu", scheme: "file" },
      { language: "luwu", scheme: "untitled" },
    ]);
  });

  test("ignores an unknown extension", () => {
    assert.deepStrictEqual(buildDocumentSelector(["luwu", "moonscript"]), [
      { language: "luwu", scheme: "file" },
      { language: "luwu", scheme: "untitled" },
    ]);
  });

  test("attaches to nothing when every extension is dropped", () => {
    assert.deepStrictEqual(buildDocumentSelector([]), []);
  });
});
