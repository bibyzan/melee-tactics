#ifndef MELEE_TACTICS_MODE_H
#define MELEE_TACTICS_MODE_H
#include <melee/gm/types.h>
extern GameModeState gm_Mode_Tactics_States[];
void tactics_DraftEnter(void*);
void tactics_DraftExit(void*);
void tactics_DraftFrame(void);
#endif
