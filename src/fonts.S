// The two built-in fonts (assets/fonts, OFL 1.1), embedded in the library so
// programs don't depend on the working directory. Paths are relative to the
// repository root, where make runs.
    .section __TEXT,__const
    .p2align 4
    .globl _ani_font_display_data, _ani_font_display_end
_ani_font_display_data:
    .incbin "assets/fonts/ArchivoBlack-Regular.ttf"
_ani_font_display_end:
    .p2align 4
    .globl _ani_font_mono_data, _ani_font_mono_end
_ani_font_mono_data:
    .incbin "assets/fonts/JetBrainsMono-Regular.ttf"
_ani_font_mono_end:
