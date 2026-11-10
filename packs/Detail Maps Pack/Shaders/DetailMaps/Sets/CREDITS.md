# Detail map sources

All source materials are from [ambientCG](https://ambientcg.com) and licensed under
[Creative Commons CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/).
No attribution is required; it is given here anyway.

| Set     | Source                                           | Normal from  |
| ------- | ------------------------------------------------ | ------------ |
| stone   | [Rock050](https://ambientcg.com/a/Rock050)       | NormalDX     |
| wood    | [Wood058](https://ambientcg.com/a/Wood058)       | NormalDX     |
| leather | [Leather030](https://ambientcg.com/a/Leather030) | NormalDX     |
| metal   | [Metal050C](https://ambientcg.com/a/Metal050C)   | Displacement |
| fabric  | [Fabric030](https://ambientcg.com/a/Fabric030)   | NormalDX     |
| plaster | [Plaster001](https://ambientcg.com/a/Plaster001) | NormalDX     |
| bark    | [Bark012](https://ambientcg.com/a/Bark012)       | NormalDX     |
| generic | [Concrete040](https://ambientcg.com/a/Concrete040) | NormalDX   |

Created using the materials above from ambientCG.com, licensed under the Creative Commons CC0 1.0 Universal License.

## Texture layout

1024x1024 BC7_UNORM, full mip chain, linear (not sRGB), seamless:

| Channel | Meaning                     | Neutral | Shader decode                    |
| ------- | --------------------------- | ------- | -------------------------------- |
| R       | albedo multiplier / 2       | 0.5     | `baseColor *= 2 * R`             |
| G       | tangent normal Y (DirectX)  | 0.5     | `n.y = G * 2 - 1`                |
| B       | roughness offset / 2 + 0.5  | 0.5     | `roughness += 2 * (B - 0.5)`     |
| A       | tangent normal X            | 0.5     | `n.x = A * 2 - 1`, `n.z = sqrt(1 - n.x^2 - n.y^2)` |

Every channel is high-passed with a wrap-around Gaussian, so the tile carries no
low-frequency shading and averages to neutral at low mips. Wood grain runs along V.
