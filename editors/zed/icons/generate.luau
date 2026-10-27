--!strict
--[=[
    Generates the Luwu file icons: the mask (.luwu), the braces (.luwon) and the two configuration icons (the mask or
    the Luau icon with a gear). Run from editors/zed:

        seal icons/generate.luau            -- the icons Zed and VS Code ship, tuned for a 16px tree row: the base
                                            -- icon fills the box and the gear sits in a corner, clear of the eye
        seal icons/generate.luau --large    -- the configuration icons for bigger renderings: the base icon at
                                            -- three quarters and a bigger gear overlapping its corner. Written
                                            -- beside the small ones with a `-large` suffix.

    The Luau icon itself (luau-dark.svg, luau-light.svg) is hand-drawn and only read here. The VS Code extension
    copies the luwon and luwu-config icons into editors/code/assets.
]=]

const fs = require("@std/fs")
const env = require("@std/env")

const large = table.find(env.args, "--large") ~= nil
const suffix = if large then "-large" else ""

type Variant = {
    name: "dark" | "light",
    --- The other half of the mask, and the right brace
    second: string,
    --- The gear, and the rim around it, in the colour of the panel it sits on
    gear: string,
    rim: string,
}

const variants: { Variant } = {
    { name = "dark", second = "#8b5cf6", gear = "#e3e7ee", rim = "#2b2f36" },
    { name = "light", second = "#862a44", gear = "#3a4049", rim = "#f4f4f6" },
}

const orange = "#f08a2a"
const eye = "#1d1d1f"

--- Replaces each `%{name}` in a template with its value
const function fill(template: string, values: { [string]: string }): string
    return (string.gsub(template, "%%{(%w+)}", function(name: string): string
        return values[name] or error(`no value for %\{{name}}`)
    end))
end

--- One half of the mask, drawn on the left; the right half is its mirror
const half = [====[M 18.5 0
      C 16 6 13.5 13 12.5 21
      C 12 27 11.6 33 11 40
      L 0 45
      L 12.5 49
      L 9 57
      L 17.5 53.5
      L 22 64
      L 25.5 58.5
      L 30.4 59.5
      L 30.4 21.5
      C 28.5 21.5 25.5 16 22.5 9
      Z]====]

const mask_defs = fill([====[<defs>
    <path id="half" d="%{half}" />
    <rect id="eye" x="21" y="36" width="5" height="12" rx="2.5" />
  </defs>]====], { half = half })

const function mask_uses(variant: Variant): string
    return fill([====[  <use href="#half" fill="%{orange}" />
  <use href="#half" fill="%{second}" transform="translate(64 0) scale(-1 1)" />
  <use href="#eye" fill="%{eye}" />
  <use href="#eye" fill="%{eye}" transform="translate(64 0) scale(-1 1)" />]====], {
        orange = orange,
        second = variant.second,
        eye = eye,
    })
end

--- A seven-tooth gear as a filled polygon with a rim in the panel's colour, so it stays apart from what's under it
const function gear(cx: number, cy: number, outer: number, inner: number, hole: number, variant: Variant): string
    const teeth = 7
    const points = {}
    const n = teeth * 2
    for i = 0, n - 1 do
        const a0 = (i / n) * 2 * math.pi
        const a1 = ((i + 1) / n) * 2 * math.pi
        const r = if i % 2 == 0 then outer else inner
        table.insert(points, string.format("%.1f %.1f", cx + r * math.cos(a0 + 0.05), cy + r * math.sin(a0 + 0.05)))
        table.insert(points, string.format("%.1f %.1f", cx + r * math.cos(a1 - 0.05), cy + r * math.sin(a1 - 0.05)))
    end
    return fill([====[  <path d="M %{points} Z" fill="%{gear}" stroke="%{rim}" stroke-width="3" stroke-linejoin="round" />
  <circle cx="%{cx}" cy="%{cy}" r="%{hole}" fill="%{rim}" />]====], {
        points = table.concat(points, " L "),
        gear = variant.gear,
        rim = variant.rim,
        cx = tostring(cx),
        cy = tostring(cy),
        hole = tostring(hole),
    })
end

--- The shapes of the hand-drawn Luau icon, without the Inkscape attributes
const function luau_shapes(variant: Variant): string
    const source = fs.readfile(`icons/luau-{variant.name}.svg`)
    const shapes = {}
    for shape in string.gmatch(source, "<path.-/>") do
        table.insert(shapes, (string.gsub(shape, '%s+[a-z]+:[%w%-]+="[^"]*"', "")))
    end
    return table.concat(shapes, "\n    ")
end

const function write(name: string, content: string)
    fs.writefile(`icons/{name}{suffix}.svg`, content .. "\n")
    print(`wrote icons/{name}{suffix}.svg`)
end

const mask_template = [====[<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <!-- Luwu's mask: a two-tone cat face split down the middle, with pointed ears, whisker spikes and two eye
       slits. After the contributor's icon concept (luau-svg-icons-concept.fig), with flatter sides. -->
  %{defs}
%{uses}
</svg>]====]

const brace = "M 24 6 C 14 6 18 20 16 27 C 15 30 10 32 7 32 C 10 32 15 34 16 37 C 18 44 14 58 24 58"
const luwon_template = [====[<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <!-- A .luwon data file: braces like a JSON icon, in the two colours of the Luwu mask -->
  <path d="%{brace}" fill="none" stroke="%{orange}" stroke-width="6.5" stroke-linecap="round" stroke-linejoin="round" />
  <path d="%{brace}" fill="none" stroke="%{second}" stroke-width="6.5" stroke-linecap="round" stroke-linejoin="round" transform="translate(64 0) scale(-1 1)" />
</svg>]====]

const luau_config_template = [====[<svg xmlns="http://www.w3.org/2000/svg" viewBox="%{view}" width="64" height="64">
  <!-- A Luau configuration file (.luaurc, .config.luau): the Luau icon with a gear -->
  <svg x="0" y="0" width="%{size}" height="%{size}" viewBox="0 0 60 60">
    %{shapes}
  </svg>
%{gear}
</svg>]====]

const luwu_config_template = [====[<svg xmlns="http://www.w3.org/2000/svg" viewBox="%{view}" width="64" height="64">
  <!-- A Luwu configuration file (.config.luwon, luwu.luwon): the Luwu mask with a gear -->
  <svg x="0" y="0" width="%{size}" height="%{size}" viewBox="0 0 64 64">
    %{defs}
%{uses}
  </svg>
%{gear}
</svg>]====]

for _, variant in variants do
    -- The mask and the braces have no gear, so they have no large variant
    if not large then
        write(`luwu-{variant.name}`, fill(mask_template, { defs = mask_defs, uses = mask_uses(variant) }))
        write(`luwon-{variant.name}`, fill(luwon_template, { brace = brace, orange = orange, second = variant.second }))
    end

    -- Small: the base icon fills a 64 box and the gear sits in the corner of a slightly wider canvas, clear of the
    -- eye. Large: the base icon takes three quarters of the box and a bigger gear overlaps its corner.
    const view = if large then "0 0 64 64" else "0 0 72 72"
    const the_gear = if large then gear(46, 46, 13.5, 9.5, 4, variant) else gear(57.5, 57.5, 16, 12, 5, variant)

    write(`luau-config-{variant.name}`, fill(luau_config_template, {
        view = view,
        size = if large then "48" else "64",
        shapes = luau_shapes(variant),
        gear = the_gear,
    }))
    write(`luwu-config-{variant.name}`, fill(luwu_config_template, {
        view = view,
        size = if large then "50" else "64",
        defs = mask_defs,
        uses = mask_uses(variant),
        gear = the_gear,
    }))
end
