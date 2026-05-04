#ifndef MENU_COMMON_H
#define MENU_COMMON_H

#include "menu.h"

typedef struct menu_state_s {
	enum m_state_e state;
	float mousex;
	float mousey;
	int lastkey;
	qboolean ignoremouseframe;
	enum m_state_e return_state;
	qboolean entersound;
	qboolean recursive_draw;
} menu_state_t;

typedef struct menu_screen_dispatch_s
{
	void (*draw)(void);
	void (*key)(int key);
	void (*mousemove)(float x, float y);
	void (*charinput)(int key);
	textmode_t (*textentry)(void);
} menu_screen_dispatch_t;

menu_state_t *M_MenuState (void);
enum m_state_e M_MenuState_GetState (void);
void M_MenuState_SetState (enum m_state_e state);
enum m_state_e M_MenuState_GetReturnState (void);
void M_MenuState_SetReturnState (enum m_state_e state);

#endif
