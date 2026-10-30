# Detail Maps Pack

Tiling micro detail for the Detail Maps feature in Bottled Shaders. It adds stone grit, wood grain, leather pores, fabric weave and similar surface detail to textures up close, without editing the textures themselves.

Requires Bottled Shaders with Detail Maps enabled. Without this pack the feature has nothing to draw, and its settings page shows a warning.

## What's in the pack

| Folder   | Contents                                                        |
| -------- | --------------------------------------------------------------- |
| `Sets/`  | The detail textures. Each `.dds` is a set named after its file. |
| `Rules/` | JSON files that pick a set for each texture path.               |
| `Tools/` | The script that built the textures, and its settings.           |

Everything installs to `Data/Shaders/DetailMaps/`.

## Adding detail to your own textures

Ship a rule file with your mod instead of editing `Default.json`, so your rules survive updates to this pack:

```json
{
    "rules": [
        {
            "match": ["**/mymod/**/*stone*"],
            "set": "stone",
            "scale": 6,
            "priority": 60
        }
    ]
}
```

Save it as `Shaders/DetailMaps/Rules/MyMod.json`. The highest priority wins, and the highest in `Default.json` is 50. Use `"set": "none"` to keep detail off textures that the default rules would match.

The full format is in `Rules/README.md`. Rules that use these sets need this pack, so list it as a requirement on your mod page.

After changing a file, press **Reload Rules and Sets** in the Detail Maps settings. Any problems with your files are listed under that button.

## Editing or adding a set

A set is not a normal color texture. Its four channels hold albedo, normal and roughness detail, as described in `Sets/CREDITS.md`, so edit each channel on its own and keep it seamless. The sets here are 1024x1024 `BC7_UNORM` with a full mip chain, in linear color rather than sRGB.

To add a set, drop a `.dds` into `Sets/` and use its file name in a rule. Give it a name unique to your mod, such as `mymod_cobble.dds`, so it can't overwrite a set from another mod.

Editing a set that ships in this pack changes every rule that uses it. To change one texture family, add a new set and a rule for it instead.

## Rebuilding the sets

`Tools/make_detail_maps.py` downloads materials from [ambientCG](https://ambientcg.com), filters them down to fine detail and writes the finished DDS files. It needs Python 3 with `numpy` and `Pillow`, plus `texconv.exe` from [DirectXTex](https://github.com/microsoft/DirectXTex).

Run it from the `Tools` folder:

```pwsh
python make_detail_maps.py --texconv <path to texconv.exe> --out ../Sets
```

-   Add `--only stone metal` to rebuild only those sets.
-   Add `--preview sheet.png` to write a contact sheet of the results.
-   Add an entry to `sets.json` to build a set from any other ambientCG material.

## Using these in your mod

You can use, edit and ship these textures and rules in your own mods. All source materials are CC0, so no permission or attribution is needed. The sources are listed in `Sets/CREDITS.md`.
