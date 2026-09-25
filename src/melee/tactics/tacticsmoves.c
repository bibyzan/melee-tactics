#include "tactics.h"

#include <melee/ft/forward.h>

/* Reach is deliberately generic: one zone per move shape rather than per
 * character. The engine decides whether the hitbox actually connects; the
 * zone only has to say "worth throwing out from here". Distances are stage
 * units between the two fighters' origins (about 10 for fighters standing
 * shoulder to shoulder). */

#define NO_AIR { 1, 0, 0, 0 }

static const TacticsMoveInfo moves[TM_COUNT] = {
    /* name            input        facing    su  O  L  F   ground zone          air zone */
    [TM_JAB] = { "Jab", TI_GROUND, TF_FRONT, 3, { 3, 1, 0 },
                 { 2, 13, -4, 14 }, NO_AIR },
    [TM_FTILT] = { "Forward Tilt", TI_GROUND, TF_FRONT, 6, { 2, 1, 1 },
                   { 4, 17, -4, 14 }, NO_AIR },
    [TM_UTILT] = { "Up Tilt", TI_GROUND, TF_ANY, 6, { 1, 3, 1 },
                   { 0, 11, 0, 24 }, NO_AIR },
    [TM_DTILT] = { "Down Tilt", TI_GROUND, TF_FRONT, 7, { 3, 2, 0 },
                   { 3, 16, -6, 8 }, NO_AIR },
    [TM_DASH_ATTACK] = { "Dash Attack", TI_DASH, TF_AUTO, 9, { 2, 1, 1 },
                         { 14, 36, -4, 12 }, NO_AIR },
    [TM_FSMASH] = { "Forward Smash", TI_SMASH, TF_AUTO, 14, { 0, 0, 3 },
                    { 4, 20, -4, 14 }, NO_AIR },
    [TM_USMASH] = { "Up Smash", TI_SMASH, TF_ANY, 11, { 0, 1, 3 },
                    { 0, 12, 0, 28 }, NO_AIR },
    [TM_DSMASH] = { "Down Smash", TI_SMASH, TF_ANY, 7, { 1, 0, 3 },
                    { 0, 17, -6, 8 }, NO_AIR },
    [TM_NAIR] = { "Neutral Air", TI_AERIAL, TF_ANY, 5, { 3, 2, 1 },
                  { 0, 14, 0, 26 }, { 0, 14, -12, 14 } },
    [TM_FAIR] = { "Forward Air", TI_AERIAL, TF_FRONT, 7, { 2, 2, 3 },
                  { 2, 20, 0, 26 }, { 2, 20, -14, 12 } },
    [TM_BAIR] = { "Back Air", TI_AERIAL, TF_BACK, 6, { 1, 1, 3 },
                  { 2, 20, 0, 26 }, { 2, 20, -14, 12 } },
    [TM_UAIR] = { "Up Air", TI_AERIAL, TF_ANY, 6, { 0, 3, 3 },
                  { 0, 12, 12, 36 }, { 0, 12, 4, 26 } },
    [TM_DAIR] = { "Down Air", TI_AERIAL, TF_ANY, 7, { 1, 2, 2 },
                  { 0, 10, -2, 12 }, { 0, 10, -26, -2 } },
    /* Specials are placeholders here; see special_shapes below. */
    [TM_NEUTRAL_B] = { "Neutral B", TI_SPECIAL, TF_FRONT, 8, { 2, 1, 1 },
                       { 2, 22, -6, 16 }, { 2, 22, -12, 12 } },
    [TM_SIDE_B] = { "Side B", TI_SPECIAL, TF_AUTO, 8, { 2, 1, 1 },
                    { 2, 22, -6, 16 }, { 2, 22, -12, 12 } },
    [TM_UP_B] = { "Up B", TI_SPECIAL, TF_ANY, 6, { 0, 2, 2 },
                  { 0, 14, 0, 40 }, { 0, 16, 0, 40 } },
    [TM_DOWN_B] = { "Down B", TI_SPECIAL, TF_ANY, 5, { 2, 2, 1 },
                    { 0, 14, -6, 18 }, { 0, 14, -12, 14 } },
    [TM_FTHROW] = { "Grab > Forward Throw", TI_THROW, TF_FRONT, 7, { 3, 0, 1 },
                    { 2, 13, -4, 12 }, NO_AIR },
    [TM_BTHROW] = { "Grab > Back Throw", TI_THROW, TF_FRONT, 7, { 3, 0, 2 },
                    { 2, 13, -4, 12 }, NO_AIR },
    [TM_UTHROW] = { "Grab > Up Throw", TI_THROW, TF_FRONT, 7, { 3, 0, 1 },
                    { 2, 13, -4, 12 }, NO_AIR },
    [TM_DTHROW] = { "Grab > Down Throw", TI_THROW, TF_FRONT, 7, { 3, 0, 0 },
                    { 2, 13, -4, 12 }, NO_AIR },
    /* Reactions move away from the foe, so any distance will do. */
    [TM_AIRDODGE] = { "Air Dodge Away", TI_DODGE, TF_ANY, 0, { 0, 0, 0 },
                      NO_AIR, { 0, 999, -999, 999 } },
    [TM_JUMP] = { "Jump Away", TI_JUMP, TF_ANY, 0, { 0, 0, 0 },
                  NO_AIR, { 0, 999, -999, 999 } },
    [TM_DRIFT] = { "Drift Away", TI_DRIFT, TF_ANY, 0, { 0, 0, 0 },
                   NO_AIR, { 0, 999, -999, 999 } },
};

/// The shape of a special move, which sets its reach and draft weights.
typedef enum SpecialShape {
    SS_PROJ,   ///< fires forward from range
    SS_FRONT,  ///< melee hit in front
    SS_CLOSE,  ///< hits around the fighter
    SS_LUNGE,  ///< travels toward the target
    SS_UP,     ///< rises; also the recovery
    SS_BELOW,  ///< drops onto the target
    SS_NONE,   ///< not draftable
} SpecialShape;

static const TacticsMoveInfo special_shapes[SS_NONE] = {
    [SS_PROJ] = { NULL, TI_SPECIAL, TF_FRONT, 10, { 3, 1, 1 },
                  { 22, 90, -8, 20 }, { 22, 90, -20, 20 } },
    [SS_FRONT] = { NULL, TI_SPECIAL, TF_FRONT, 10, { 2, 1, 2 },
                   { 2, 22, -6, 16 }, { 2, 22, -12, 12 } },
    [SS_CLOSE] = { NULL, TI_SPECIAL, TF_ANY, 6, { 2, 2, 2 },
                   { 0, 14, -6, 18 }, { 0, 14, -12, 14 } },
    [SS_LUNGE] = { NULL, TI_SPECIAL, TF_AUTO, 10, { 2, 1, 2 },
                   { 10, 50, -6, 14 }, { 10, 50, -14, 14 } },
    [SS_UP] = { NULL, TI_SPECIAL, TF_ANY, 6, { 0, 2, 2 },
                { 0, 14, 0, 40 }, { 0, 16, 0, 40 } },
    [SS_BELOW] = { NULL, TI_SPECIAL, TF_ANY, 8, { 1, 1, 2 },
                   { 0, 12, -6, 12 }, { 0, 12, -30, -2 } },
};

typedef struct SpecialDesc {
    const char* name;
    u8 shape; ///< ::SpecialShape
} SpecialDesc;

/* Neutral, side, up, down; indexed by CharacterKind. */
static const SpecialDesc specials[CKind_Playable_Count][4] = {
    [CKind_Captain] = { { "Falcon Punch", SS_FRONT }, { "Raptor Boost", SS_LUNGE },
                        { "Falcon Dive", SS_UP }, { "Falcon Kick", SS_LUNGE } },
    [CKind_Donkey] = { { "Giant Punch", SS_FRONT }, { "Headbutt", SS_FRONT },
                       { "Spinning Kong", SS_CLOSE }, { "Hand Slap", SS_CLOSE } },
    [CKind_Fox] = { { "Blaster", SS_PROJ }, { "Fox Illusion", SS_LUNGE },
                    { "Fire Fox", SS_UP }, { "Reflector", SS_CLOSE } },
    [CKind_GameWatch] = { { "Chef", SS_PROJ }, { "Judgement", SS_FRONT },
                          { "Fire", SS_UP }, { "Oil Panic", SS_CLOSE } },
    [CKind_Kirby] = { { "Inhale", SS_FRONT }, { "Hammer", SS_FRONT },
                      { "Final Cutter", SS_UP }, { "Stone", SS_BELOW } },
    [CKind_Koopa] = { { "Fire Breath", SS_FRONT }, { "Koopa Klaw", SS_FRONT },
                      { "Whirling Fortress", SS_CLOSE }, { "Bowser Bomb", SS_BELOW } },
    [CKind_Link] = { { "Hero's Bow", SS_PROJ }, { "Boomerang", SS_PROJ },
                     { "Spin Attack", SS_CLOSE }, { "Bomb", SS_CLOSE } },
    [CKind_Luigi] = { { "Fireball", SS_PROJ }, { "Green Missile", SS_LUNGE },
                      { "Super Jump Punch", SS_UP }, { "Luigi Cyclone", SS_CLOSE } },
    [CKind_Mario] = { { "Fireball", SS_PROJ }, { "Cape", SS_FRONT },
                      { "Super Jump Punch", SS_UP }, { "Mario Tornado", SS_CLOSE } },
    [CKind_Mars] = { { "Shield Breaker", SS_FRONT }, { "Dancing Blade", SS_FRONT },
                     { "Dolphin Slash", SS_UP }, { "Counter", SS_CLOSE } },
    [CKind_Mewtwo] = { { "Shadow Ball", SS_PROJ }, { "Confusion", SS_FRONT },
                       { "Teleport", SS_UP }, { "Disable", SS_FRONT } },
    [CKind_Ness] = { { "PK Flash", SS_FRONT }, { "PK Fire", SS_PROJ },
                     { "PK Thunder", SS_UP }, { "PSI Magnet", SS_CLOSE } },
    [CKind_Peach] = { { "Toad", SS_CLOSE }, { "Peach Bomber", SS_LUNGE },
                      { "Peach Parasol", SS_UP }, { "Vegetable", SS_CLOSE } },
    [CKind_Pikachu] = { { "Thunder Jolt", SS_PROJ }, { "Skull Bash", SS_LUNGE },
                        { "Quick Attack", SS_UP }, { "Thunder", SS_CLOSE } },
    [CKind_PopoNana] = { { "Ice Shot", SS_PROJ }, { "Squall Hammer", SS_CLOSE },
                         { "Belay", SS_UP }, { "Blizzard", SS_FRONT } },
    [CKind_Purin] = { { "Rollout", SS_LUNGE }, { "Pound", SS_FRONT },
                      { "Sing", SS_CLOSE }, { "Rest", SS_CLOSE } },
    [CKind_Samus] = { { "Charge Shot", SS_PROJ }, { "Missile", SS_PROJ },
                      { "Screw Attack", SS_UP }, { "Bomb", SS_CLOSE } },
    [CKind_Yoshi] = { { "Egg Lay", SS_FRONT }, { "Egg Roll", SS_LUNGE },
                      { "Egg Throw", SS_PROJ }, { "Yoshi Bomb", SS_BELOW } },
    [CKind_Zelda] = { { "Nayru's Love", SS_CLOSE }, { "Din's Fire", SS_PROJ },
                      { "Farore's Wind", SS_UP }, { "Transform", SS_NONE } },
    [CKind_Seak] = { { "Needle Storm", SS_PROJ }, { "Chain", SS_FRONT },
                     { "Vanish", SS_UP }, { "Transform", SS_NONE } },
    [CKind_Falco] = { { "Blaster", SS_PROJ }, { "Falco Phantasm", SS_LUNGE },
                      { "Fire Bird", SS_UP }, { "Reflector", SS_CLOSE } },
    [CKind_CLink] = { { "Fire Bow", SS_PROJ }, { "Boomerang", SS_PROJ },
                      { "Spin Attack", SS_CLOSE }, { "Bomb", SS_CLOSE } },
    [CKind_DrMario] = { { "Megavitamin", SS_PROJ }, { "Super Sheet", SS_FRONT },
                        { "Super Jump Punch", SS_UP }, { "Dr. Tornado", SS_CLOSE } },
    [CKind_Emblem] = { { "Flare Blade", SS_FRONT }, { "Double-Edge Dance", SS_FRONT },
                       { "Blazer", SS_UP }, { "Counter", SS_CLOSE } },
    [CKind_Pichu] = { { "Thunder Jolt", SS_PROJ }, { "Skull Bash", SS_LUNGE },
                      { "Agility", SS_UP }, { "Thunder", SS_CLOSE } },
    [CKind_Ganon] = { { "Warlock Punch", SS_FRONT }, { "Gerudo Dragon", SS_LUNGE },
                      { "Dark Dive", SS_UP }, { "Wizard's Foot", SS_LUNGE } },
};

static const char* const fighter_names[CKind_Playable_Count] = {
    [CKind_Captain] = "Captain Falcon", [CKind_Donkey] = "Donkey Kong",
    [CKind_Fox] = "Fox", [CKind_GameWatch] = "Mr. Game & Watch",
    [CKind_Kirby] = "Kirby", [CKind_Koopa] = "Bowser",
    [CKind_Link] = "Link", [CKind_Luigi] = "Luigi",
    [CKind_Mario] = "Mario", [CKind_Mars] = "Marth",
    [CKind_Mewtwo] = "Mewtwo", [CKind_Ness] = "Ness",
    [CKind_Peach] = "Peach", [CKind_Pikachu] = "Pikachu",
    [CKind_PopoNana] = "Ice Climbers", [CKind_Purin] = "Jigglypuff",
    [CKind_Samus] = "Samus", [CKind_Yoshi] = "Yoshi",
    [CKind_Zelda] = "Zelda", [CKind_Seak] = "Sheik",
    [CKind_Falco] = "Falco", [CKind_CLink] = "Young Link",
    [CKind_DrMario] = "Dr. Mario", [CKind_Emblem] = "Roy",
    [CKind_Pichu] = "Pichu", [CKind_Ganon] = "Ganondorf",
};

/* Specials resolved per character on first use: the shape's reach and
 * weights under the character's own name. */
static TacticsMoveInfo special_moves[CKind_Playable_Count][4];
static bool special_moves_ready;

static void buildSpecials(void)
{
    int c;
    int i;

    for (c = 0; c < CKind_Playable_Count; c++) {
        for (i = 0; i < 4; i++) {
            const SpecialDesc* d = &specials[c][i];
            TacticsMoveInfo* m = &special_moves[c][i];
            *m = d->shape < SS_NONE ? special_shapes[d->shape]
                                    : moves[TM_NEUTRAL_B + i];
            m->name = d->name;
        }
    }
    special_moves_ready = true;
}

const TacticsMoveInfo* tactics_GetMove(int ckind, int move)
{
    if (move < 0 || move >= TM_COUNT) {
        return NULL;
    }
    if (move >= TM_NEUTRAL_B && move <= TM_DOWN_B && ckind >= 0 &&
        ckind < CKind_Playable_Count)
    {
        if (!special_moves_ready) {
            buildSpecials();
        }
        return &special_moves[ckind][move - TM_NEUTRAL_B];
    }
    return &moves[move];
}

bool tactics_MoveAllowed(int ckind, int move)
{
    if (move < 0 || move >= TM_COUNT) {
        return false;
    }
    if (move >= TM_NEUTRAL_B && move <= TM_DOWN_B && ckind >= 0 &&
        ckind < CKind_Playable_Count)
    {
        return specials[ckind][move - TM_NEUTRAL_B].shape != SS_NONE;
    }
    return true;
}

const char* tactics_FighterName(int ckind)
{
    if (ckind < 0 || ckind >= CKind_Playable_Count) {
        return "?";
    }
    return fighter_names[ckind];
}
