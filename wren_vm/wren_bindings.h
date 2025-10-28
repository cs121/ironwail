#ifndef WREN_BINDINGS_H
#define WREN_BINDINGS_H

#include "wren/include/wren.h"
#include "q_stdinc.h"

void WRENBindings_Configure(WrenConfiguration *config);
void WRENBindings_BindRuntime(WrenVM *vm);
void WRENBindings_Shutdown(void);

#endif // WREN_BINDINGS_H
