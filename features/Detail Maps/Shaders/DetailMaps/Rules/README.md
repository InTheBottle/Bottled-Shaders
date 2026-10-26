# Detail Maps rules

Every `*.json` file in this folder is read in file name order. Add your own file
rather than editing `Default.json`.

```json
{
    "exclude": ["**/effects/**"],
    "rules": [
        {
            "match": ["**/architecture/**/*wood*"],
            "exclude": ["**/*door*"],
            "set": "wood",
            "scale": 6,
            "albedo": 1.0,
            "normal": 1.0,
            "roughness": 1.0,
            "transpose": false,
            "priority": 20
        }
    ]
}
```

-   Patterns match the diffuse texture path in lower case, starting at `textures/`.
    PBR paths (`textures/pbr/...`) are matched with the `pbr/` removed.
-   `*` matches inside one folder or file name, `**` matches across folders,
    `?` matches one character.
-   A path matched by a top-level `exclude` gets no detail from any rule.
-   `set` is the name of a `.dds` in `../Sets` without the extension. Drop a new
    DDS there to add a set (layout in `../Sets/CREDITS.md`). `"none"` gives the
    matched paths no detail, at that rule's priority.
-   `scale` is how many detail tiles fit in one repeat of the texture.
    `albedo`, `normal` and `roughness` scale the set's strength (0 to 4).
-   `transpose` swaps U and V, for grain that runs the wrong way.
-   The highest `priority` wins. On a tie the rule from the later file wins,
    then the rule earlier in its file.
