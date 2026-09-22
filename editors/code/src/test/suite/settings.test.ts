/* eslint-disable @typescript-eslint/naming-convention */
import * as assert from "assert";
import {
  buildServerConfiguration,
  resolveExplicitSetting,
  resolveSetting,
  settingKeys,
} from "../../settings";

suite("Settings Test Suite", () => {
  suite("resolveSetting", () => {
    const declared = { defaultValue: "default" };

    test("prefers an explicit luwu value over everything", () => {
      assert.strictEqual(
        resolveSetting(
          { ...declared, globalValue: "new" },
          { ...declared, globalValue: "legacy" },
          true,
        ),
        "new",
      );
    });

    test("uses an explicit luau-lsp value over the default", () => {
      assert.strictEqual(
        resolveSetting(declared, { ...declared, globalValue: "legacy" }, true),
        "legacy",
      );
    });

    test("ignores the legacy value when inheritance is off", () => {
      assert.strictEqual(
        resolveSetting(declared, { ...declared, globalValue: "legacy" }, false),
        "default",
      );
    });

    test("falls back to the default when neither is written", () => {
      assert.strictEqual(resolveSetting(declared, declared, true), "default");
    });

    test("takes the most specific scope on each side", () => {
      assert.strictEqual(
        resolveSetting(
          declared,
          {
            ...declared,
            globalValue: "user",
            workspaceValue: "workspace",
            workspaceFolderValue: "folder",
          },
          true,
        ),
        "folder",
      );
    });

    test("honours an explicit value equal to the default", () => {
      // this is the case a plain `get` cannot express, and the reason both
      // namespaces are declared: someone writing `false` where `false` is the
      // default must still outrank a legacy `true`
      assert.strictEqual(
        resolveSetting(
          { defaultValue: false, globalValue: false },
          { defaultValue: false, globalValue: true },
          true,
        ),
        false,
      );
    });

    test("survives a key that neither namespace declares", () => {
      assert.strictEqual(resolveSetting(undefined, undefined, true), undefined);
    });
  });

  suite("resolveExplicitSetting", () => {
    const declared = { defaultValue: "default" };

    test("reads through a declared default, unlike resolveSetting", () => {
      // what getStudioPluginValue needs: `plugin.*` is the deprecated
      // spelling of `studioPlugin.*` and applies only when the current one is
      // untouched. Both declare a default, so resolveSetting would always
      // answer with the current one's and the deprecated name would never be
      // read at all
      assert.strictEqual(
        resolveExplicitSetting(declared, declared, true),
        undefined,
      );
      assert.strictEqual(resolveSetting(declared, declared, true), "default");
    });

    test("still prefers an explicit value, new namespace first", () => {
      assert.strictEqual(
        resolveExplicitSetting(
          { ...declared, globalValue: "new" },
          declared,
          true,
        ),
        "new",
      );
      assert.strictEqual(
        resolveExplicitSetting(
          declared,
          { ...declared, globalValue: "legacy" },
          true,
        ),
        "legacy",
      );
    });

    test("ignores the legacy namespace when inheritance is off", () => {
      assert.strictEqual(
        resolveExplicitSetting(
          declared,
          { ...declared, globalValue: "legacy" },
          false,
        ),
        undefined,
      );
    });
  });

  suite("settingKeys", () => {
    test("takes only server-bound luwu keys, without the prefix", () => {
      assert.deepStrictEqual(
        settingKeys({
          contributes: {
            configuration: {
              properties: {
                "luwu.fileExtensions": {},
                "luwu.inlayHints.blockEndHints": {},
                "luau-lsp.fileExtensions": {},
                "luwu.trace.server": {},
                "luwu.inheritLuauLspSettings": {},
              },
            },
          },
        }),
        ["fileExtensions", "inlayHints.blockEndHints"],
      );
    });

    test("returns nothing for a package.json without contributions", () => {
      assert.deepStrictEqual(settingKeys({}), []);
      assert.deepStrictEqual(settingKeys(undefined), []);
    });
  });

  suite("buildServerConfiguration", () => {
    test("nests dotted keys into the shape the server deserializes", () => {
      const values: Record<string, unknown> = {
        "inlayHints.blockEndHints": true,
        "inlayHints.blockEndHintsMinLines": 35,
        "completion.imports.enabled": false,
        fileExtensions: ["luwu"],
      };

      assert.deepStrictEqual(
        buildServerConfiguration(Object.keys(values), (key) => values[key]),
        {
          inlayHints: { blockEndHints: true, blockEndHintsMinLines: 35 },
          completion: { imports: { enabled: false } },
          fileExtensions: ["luwu"],
        },
      );
    });

    test("leaves out keys that resolve to nothing", () => {
      assert.deepStrictEqual(
        buildServerConfiguration(["a", "b"], (key) =>
          key === "a" ? 1 : undefined,
        ),
        { a: 1 },
      );
    });
  });
});
