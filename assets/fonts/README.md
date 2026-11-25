# Fonts

Two fonts, checked in as content (`make` doesn't fetch them). Both are under the
SIL Open Font License 1.1; each license text sits next to its font, with the
font's copyright notice as its first line. The files are unmodified copies of
upstream. The OFL allows bundling and redistribution with software, provided
the license and copyright notice travel with the font (they do: these files)
and the font isn't sold by itself.

| file | font | version | copyright | license |
|---|---|---|---|---|
| `ArchivoBlack-Regular.ttf` | Archivo Black Regular (display) | 1.006 | Copyright 2017 The Archivo Black Project Authors (https://github.com/Omnibus-Type/ArchivoBlack) | `ArchivoBlack-OFL.txt` |
| `JetBrainsMono-Regular.ttf` | JetBrains Mono Regular (code) | 2.305 | Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono) | `JetBrainsMono-OFL.txt` |

## Sources

Downloaded on 2026-11-25 from these commits:

- Archivo Black: google/fonts at `94a7d81318e438525a5285e07ab72c050fdfeb44`
  (2026-02-26), `ofl/archivoblack/ArchivoBlack-Regular.ttf` and
  `ofl/archivoblack/OFL.txt`.
- JetBrains Mono: JetBrains/JetBrainsMono at
  `19371302b95d218af43299bce79ddbddd0bc364d` (2025-01-31),
  `fonts/ttf/JetBrainsMono-Regular.ttf` (the static instance) and `OFL.txt`.
  Google Fonts only ships the variable font, and stb_truetype can't apply
  variation axes.

SHA-256:

```
dd9a89a019b4849f66ab75455fe7bdf931311042cbb0f0f97acc061539703180  ArchivoBlack-Regular.ttf
3173acd82f8c6159b5b1037b539fcbd4edff68e65c2ea8b9412b5a5ca97b08ff  ArchivoBlack-OFL.txt
e6fd0d7e91550b3ed2b735d4312474362c4716edc4fc0577a0f61ed782d5aed1  JetBrainsMono-Regular.ttf
a76abf002c49097d146e86740a3105a5d00450b1592e820a1109a8c5680cd697  JetBrainsMono-OFL.txt
```

## Exported videos

A rendered video contains pictures of glyphs, not the font files, and the OFL
places no conditions on that. Credit is welcome but not required.
