#ifndef MENU_MAIN_H
#define MENU_MAIN_H
#include "menu_common.h"
const menu_screen_dispatch_t *MenuMain_GetDispatch (void);
const menu_screen_dispatch_t *MenuSinglePlayer_GetDispatch (void);
const menu_screen_dispatch_t *MenuLoad_GetDispatch (void);
const menu_screen_dispatch_t *MenuSave_GetDispatch (void);
const menu_screen_dispatch_t *MenuMaps_GetDispatch (void);
const menu_screen_dispatch_t *MenuSkill_GetDispatch (void);
const menu_screen_dispatch_t *MenuHelp_GetDispatch (void);
const menu_screen_dispatch_t *MenuQuit_GetDispatch (void);
#endif
