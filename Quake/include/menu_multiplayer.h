#ifndef MENU_MULTIPLAYER_H
#define MENU_MULTIPLAYER_H
#include "menu_common.h"
const menu_screen_dispatch_t *MenuMultiplayer_GetDispatch (void);
const menu_screen_dispatch_t *MenuSetup_GetDispatch (void);
const menu_screen_dispatch_t *MenuNet_GetDispatch (void);
const menu_screen_dispatch_t *MenuLanConfig_GetDispatch (void);
const menu_screen_dispatch_t *MenuGameOptions_GetDispatch (void);
const menu_screen_dispatch_t *MenuSearch_GetDispatch (void);
const menu_screen_dispatch_t *MenuServerList_GetDispatch (void);
#endif
