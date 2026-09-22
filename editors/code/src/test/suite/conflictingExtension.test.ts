import * as assert from "assert";
import {
  UPSTREAM_EXTENSION_ID,
  effectiveFileExtensions,
  findConflictingExtension,
} from "../../conflictingExtension";

const upstreamInstalled = (id: string) =>
  id === UPSTREAM_EXTENSION_ID ? { id } : undefined;

const nothingInstalled = () => undefined;

suite("Conflicting Extension Test Suite", () => {
  suite("findConflictingExtension", () => {
    test("reports upstream when it is enabled alongside us", () => {
      assert.deepStrictEqual(
        findConflictingExtension("luwu-community.luwu", upstreamInstalled),
        { id: UPSTREAM_EXTENSION_ID },
      );
    });

    test("reports nothing when upstream is absent or disabled", () => {
      assert.strictEqual(
        findConflictingExtension("luwu-community.luwu", nothingInstalled),
        undefined,
      );
    });

    test("stays quiet while we ship under upstream's own extension ID", () => {
      // in that state VSCode treats ours as an update of theirs, so they
      // can never both be installed and the lookup must not even run
      let lookedUp = false;
      const result = findConflictingExtension(UPSTREAM_EXTENSION_ID, (id) => {
        lookedUp = true;
        return upstreamInstalled(id);
      });

      assert.strictEqual(result, undefined);
      assert.strictEqual(lookedUp, false);
    });

    test("compares extension IDs case-insensitively", () => {
      assert.strictEqual(
        findConflictingExtension(
          UPSTREAM_EXTENSION_ID.toUpperCase(),
          upstreamInstalled,
        ),
        undefined,
      );
    });
  });

  suite("effectiveFileExtensions", () => {
    const configured = ["luwu", "luau", "lua"];

    test("serves everything configured when there is no conflict", () => {
      assert.deepStrictEqual(effectiveFileExtensions(configured, false), [
        "luwu",
        "luau",
        "lua",
      ]);
    });

    test("holds back to .luwu while upstream is enabled", () => {
      // upstream doesn't know about .luwu, so this is the one thing we can
      // serve without duplicating its diagnostics
      assert.deepStrictEqual(effectiveFileExtensions(configured, true), [
        "luwu",
      ]);
    });

    test("does not add luwu back when the user removed it", () => {
      assert.deepStrictEqual(effectiveFileExtensions(["luau"], true), []);
    });

    test("does not mutate the configured list", () => {
      const original = [...configured];
      effectiveFileExtensions(configured, true);
      assert.deepStrictEqual(configured, original);
    });
  });
});
