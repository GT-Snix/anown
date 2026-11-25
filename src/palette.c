// Named palettes (placeholders, docs/layers.md): cream paper, charcoal ink, and
// a riso medium blue for hard shadows, so a shadow never reads as more ink.
#include "anown.h"

#include <string.h>

#define CREAM AN_HEX(0xF3EDE2)
#define CHARCOAL AN_HEX(0x16161A)
#define CYAN AN_HEX(0x00C2FF)
#define MAGENTA AN_HEX(0xFF2E88)
#define LIME AN_HEX(0xC6FF00)
#define ORANGE AN_HEX(0xFF6A13)
#define RISO_BLUE AN_HEX(0x3255A4)

const an_palette AN_PALETTE_RIOT = { "riot", CREAM, CHARCOAL, CYAN, MAGENTA, LIME, RISO_BLUE };
const an_palette AN_PALETTE_HAZARD = { "hazard", CREAM, CHARCOAL, ORANGE, CYAN, LIME, RISO_BLUE };
const an_palette AN_PALETTE_ACID = { "acid", CREAM, CHARCOAL, LIME, MAGENTA, ORANGE, RISO_BLUE };

const an_palette *an_palette_named(const char *name) {
    if (!name) return NULL;
    const an_palette *all[3] = { &AN_PALETTE_RIOT, &AN_PALETTE_HAZARD, &AN_PALETTE_ACID };
    for (int i = 0; i < 3; i++) {
        if (strcmp(all[i]->name, name) == 0) return all[i];
    }
    return NULL;
}
