# Starter art: sources and hashes

Every binary source under `content/` (WP-0.20b): where it came from, under which licence, and the SHA-256 of
the committed file. Each file's `.meta` sidecar records the same provenance (origin `cc0`, the page URL, the
download, the archive's SHA-256 and the licence as stated). The files are Git LFS objects
(`content/.gitattributes`); a checkout without them has pointer files whose `oid` is the same SHA-256, and
`lint_content_assets` (tools/lint/content_assets.cmake) checks every row against the file or its pointer.

All 35 files are CC0-1.0, retrieved 2026-10-06, and committed byte for byte as downloaded (no file was
edited). CC0 needs no licence text next to the files. They are placeholders for Phase 1 (01 §4.2: the
Kestrel, Saltmarch, Harrow and the Scree belt), not final art.

## Review

Before a file was added: its source page states CC0 (quoted below) and its download says the same; its
preview was viewed (the kits' own preview renders, ambientCG's material spheres, Poly Haven's sky
thumbnail) for franchise designs, logos, insignia, text and people, and none were found; every name and
string inside it (glTF node, mesh, material and image names, PNG text chunks, OpenEXR header attributes)
was matched against the IP-name lint's franchise lists with no hit; and it was opened with a decoder
(every model loads as glTF 2.0 with its external texture found, every PNG decodes, the EXR decodes as
1024 x 512 RGB). The files keep their embedded metadata: the models name their exporter, some PNGs carry
an XMP packet naming their authoring tool, and the EXR keeps its render stamps (render times and the
photographer's local scene path). None of the sources discloses generative AI; Poly Haven states that
its assets are the original work of its staff or contributing artists, and the sky is a photograph.
Rejected while selecting: night-sky HDRIs whose page gives no creation method (when in doubt, leave it
out), and the kits' own License.txt, which CC0 does not require and which names social-media brands.

## Sources

| Source | Author | Page | Download | Archive SHA-256 | Licence as stated |
|---|---|---|---|---|---|
| Qwantani Dusk 2 (Pure Sky) | Greg Zaal (photography) and Jarod Guest (processing), Poly Haven | <https://polyhaven.com/a/qwantani_dusk_2_puresky> | <https://dl.polyhaven.org/file/ph-assets/HDRIs/exr/1k/qwantani_dusk_2_puresky_1k.exr> | `9374eec747b84f76bb8535250a0756ca4dcbecfe0cb67abcfb0558ca2fbd2037` | The asset page's License field says "CC0", and https://polyhaven.com/license says "Our assets are all licensed as CC0, which is effectively Public Domain even in jurisdictions that do not support the Public Domain." |
| Ground 054 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Ground054> | <https://ambientcg.com/get?file=Ground054_1K-PNG.zip> | `19aafb7d257cff80eca5374b097abacb714149aa899891be82572e82fbac1326` | The asset page says "All assets are released under the Creative Commons CC0 license", and https://ambientcg.com/license says "All ambientCG assets are provided under the Creative Commons CC0 1.0 Universal License." |
| Rock 035 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Rock035> | <https://ambientcg.com/get?file=Rock035_1K-PNG.zip> | `e745b558d754962ac44162ccee8805d7dba84ecdc428a543c3e552bcb28f8b85` | The asset page says "All assets are released under the Creative Commons CC0 license", and https://ambientcg.com/license says "All ambientCG assets are provided under the Creative Commons CC0 1.0 Universal License." |
| Metal Plates 006 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates006> | <https://ambientcg.com/get?file=MetalPlates006_1K-PNG.zip> | `ecf94450f608bf03bfdbefe18a740e7599f17bc199cf082e3c88387b2e7404a0` | The asset page says "All assets are released under the Creative Commons CC0 license", and https://ambientcg.com/license says "All ambientCG assets are provided under the Creative Commons CC0 1.0 Universal License." |
| Metal Plates 017 A (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates017A> | <https://ambientcg.com/get?file=MetalPlates017A_1K-PNG.zip> | `10a146661c3ce17a5c9c8587a30de5de5f193f8c9215eb4af08b4804cc52c308` | The asset page says "All assets are released under the Creative Commons CC0 license", and https://ambientcg.com/license says "All ambientCG assets are provided under the Creative Commons CC0 1.0 Universal License." |
| Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | <https://kenney.nl/media/pages/assets/space-station-kit/6475288f2e-1712749919/kenney_space-station-kit.zip> | `215e79bd5415cff93665183390f0343ed9acf87780306331013b78520170c6d8` | The page says "License: Creative Commons CC0", and the archive's License.txt says "License: (Creative Commons Zero, CC0) http://creativecommons.org/publicdomain/zero/1.0/" |
| Space Kit 2.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-kit> | <https://kenney.nl/media/pages/assets/space-kit/20874c75ac-1677698978/kenney_space-kit.zip> | `d5d7cdf2635ed5a43a9187deaf409b6f47484e402321128341d3c3698e9ef4d9` | The page says "License: Creative Commons CC0", and the archive's License.txt says "License: (Creative Commons Zero, CC0) http://creativecommons.org/publicdomain/zero/1.0/" |

## Files

| Path (under `content/`) | Title | Author | Source page | Licence | Bytes | SHA-256 |
|---|---|---|---|---|---|---|
| `art/environment/qwantani_dusk_2_puresky_1k.exr` | Qwantani Dusk 2 (Pure Sky) | Greg Zaal (photography) and Jarod Guest (processing), Poly Haven | <https://polyhaven.com/a/qwantani_dusk_2_puresky> | CC0-1.0 | 4983112 | `9374eec747b84f76bb8535250a0756ca4dcbecfe0cb67abcfb0558ca2fbd2037` |
| `art/materials/harrow_regolith/Ground054_1K-PNG_Color.png` | Ground 054 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Ground054> | CC0-1.0 | 2088335 | `bb3b8b28e2a03f6a448a042880532da732e0b00bb3978b912be98001e85c2151` |
| `art/materials/harrow_regolith/Ground054_1K-PNG_NormalGL.png` | Ground 054 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Ground054> | CC0-1.0 | 5766891 | `331258541860948a77d1b47f6a2435344fcd0100e6b789208f5637e77b3bc53f` |
| `art/materials/harrow_regolith/Ground054_1K-PNG_Roughness.png` | Ground 054 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Ground054> | CC0-1.0 | 630483 | `c4a9057826c1f723cc029159c750fd592fe980f406299a61d179ec40f49c1de8` |
| `art/materials/harrow_rock/Rock035_1K-PNG_Color.png` | Rock 035 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Rock035> | CC0-1.0 | 1983124 | `dbc4eee8993b141fae0912ea2401971c16ae283737798f396eae1f9b2ca327b7` |
| `art/materials/harrow_rock/Rock035_1K-PNG_NormalGL.png` | Rock 035 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Rock035> | CC0-1.0 | 6069943 | `cab0ec9898eb57243bf3eee98552aa745966d959193d822ec3be99c973082e87` |
| `art/materials/harrow_rock/Rock035_1K-PNG_Roughness.png` | Rock 035 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=Rock035> | CC0-1.0 | 622442 | `6d1550fe6030a866478e69d703aea79de032bfc59bd41ff5751c41a5d166ba39` |
| `art/materials/hull_plates/MetalPlates006_1K-PNG_Color.png` | Metal Plates 006 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates006> | CC0-1.0 | 474847 | `37b50aa3635cde294572c8e96441630de28e6ca27b9b28956a1a70974a252e99` |
| `art/materials/hull_plates/MetalPlates006_1K-PNG_Metalness.png` | Metal Plates 006 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates006> | CC0-1.0 | 8055 | `d13865fac3a43f7ec28a2c0adace823f29ceb4732b3bbacbbed25b0370c6a078` |
| `art/materials/hull_plates/MetalPlates006_1K-PNG_NormalGL.png` | Metal Plates 006 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates006> | CC0-1.0 | 4079408 | `e8a7bafa75388df8309ffc08f2c62d19d2d0a10264d538764300f2a9cd5a23e5` |
| `art/materials/hull_plates/MetalPlates006_1K-PNG_Roughness.png` | Metal Plates 006 (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates006> | CC0-1.0 | 244791 | `36467c677bb08fe5685634b4f181a9cc7e7752fd9166f9ed1cef4dbe833e20b7` |
| `art/materials/station_panels/MetalPlates017A_1K-PNG_Color.png` | Metal Plates 017 A (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates017A> | CC0-1.0 | 705714 | `626c7f4dee3cd99e7d80dc3d93654ccc05d9140a680a5ebe1a7c453dff8309a0` |
| `art/materials/station_panels/MetalPlates017A_1K-PNG_Metalness.png` | Metal Plates 017 A (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates017A> | CC0-1.0 | 89744 | `1637dc7c6f4d5485075501d47ef097275ac25603d82f3fc5cb8259bbccc06d0e` |
| `art/materials/station_panels/MetalPlates017A_1K-PNG_NormalGL.png` | Metal Plates 017 A (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates017A> | CC0-1.0 | 4379337 | `a6408408be7dced91aa949186e5ffe65e30f995733e9adb7ac45b903fb10698f` |
| `art/materials/station_panels/MetalPlates017A_1K-PNG_Roughness.png` | Metal Plates 017 A (1K-PNG) | ambientCG (ambientcg.com) | <https://ambientcg.com/view?id=MetalPlates017A> | CC0-1.0 | 351206 | `85a96af84f1e00bb62c9be9d66c6feea4cdf71d820effedbfe8695313a78f01b` |
| `art/saltmarch/kit/Textures/colormap.png` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 7440 | `6ca023be9f12e8c2358fcf94320c8a894b4c05a3dd0bad1f5c268312053caca1` |
| `art/saltmarch/kit/computer-wide.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 17136 | `62420ce928f288f91ce85e2245e80e6c9efbee9b6e91b7ea9b83d0289fa647c4` |
| `art/saltmarch/kit/computer.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 13804 | `dede29d81fd122c484af3e0dbf566f34893ed0f3c80182586feb19b23c33c0b7` |
| `art/saltmarch/kit/container-tall.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 13052 | `e790f12cdab2e50d266d54e1bbb8a05a1cd77629a065254be3734a67b6053358` |
| `art/saltmarch/kit/container-wide.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 12212 | `3bed318d187ce26b816b7f84b706ecb57cffef94ab1bb8266ca91aa35073e6f0` |
| `art/saltmarch/kit/container.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 9716 | `719b77f62979e9e02047489fcc1e86432be300ac35ed02fe496a2dfc6a3fcdd6` |
| `art/saltmarch/kit/display-wall.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 8776 | `64eb414b4ae9c665b8ef95711d8bade6285b0a7f6dfb8f1f8bd51a67ab4dec89` |
| `art/saltmarch/kit/door-single.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 5376 | `85f80cd0ef327f322694b46bebb4d07996ceccaaf10b14fd0ccc6585d252c8c3` |
| `art/saltmarch/kit/floor-panel.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 8900 | `5b9f1689347a1c490b8ddbf79f8741a6fcab1140ebcf62eb5853b36fbbdab53e` |
| `art/saltmarch/kit/floor.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 2836 | `bf61eafc5c4159e1103d88a2973664f55b9e1531a0b3451d90b42d3387132449` |
| `art/saltmarch/kit/pipe.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 3536 | `b33fe37fee9d1dfe92dc75d5af7e54877199b9d865cb988afa25f40b7bed49ef` |
| `art/saltmarch/kit/stairs.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 5832 | `c288069628036c179f030e8669e020a94d40efa932bf8610a08dc81fec6b9e60` |
| `art/saltmarch/kit/structure.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 29828 | `ef12f1eaec019116ece1aebb0c512ce19a5cbc385f7a393c62d9f32f129cf088` |
| `art/saltmarch/kit/wall-corner.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 9332 | `95dffaca238fbb3bc59a7209ef24fb624aa03998ccf9091e9df1c5cd907a22be` |
| `art/saltmarch/kit/wall-door.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 9892 | `e1692da0f0a0ebd2dfd36787dbe9a47748141d82bbbb12b0381919d52e271603` |
| `art/saltmarch/kit/wall-window.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 9720 | `55053731040bad9e0a02cd5cbef4d6ce4516f40019d41488eb36a5bdebcda6f3` |
| `art/saltmarch/kit/wall.glb` | Space Station Kit 1.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-station-kit> | CC0-1.0 | 5252 | `561253375e765f825899d13031e52c07b84304d394e1d69579e0749e23fba42f` |
| `art/scree/meteor_detailed.glb` | Space Kit 2.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-kit> | CC0-1.0 | 14208 | `f30379abd7d772a2fdc52ebb72ad2b5fc94768343cb8f6788614820f632c5e63` |
| `art/scree/rock_largeA.glb` | Space Kit 2.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-kit> | CC0-1.0 | 13224 | `13b2bf393fcd7ca8c73d8a2a731da0be6c2c2b4e67ef0ae826e816c0d8733f2f` |
| `art/ships/kestrel/craft_speederD.glb` | Space Kit 2.0 | Kenney (www.kenney.nl) | <https://kenney.nl/assets/space-kit> | CC0-1.0 | 22876 | `a5780b9f6cc40d6f348f4211bb43c9501fa199729197361be6bfb522a42c56ad` |

35 files, 32700380 bytes (31.2 MiB) in Git LFS.
