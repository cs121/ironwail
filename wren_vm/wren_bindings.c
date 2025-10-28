#include "quakedef.h"
#include "common.h"
#include "cmd.h"
#include "mathlib.h"
#include "server.h"
#include "world.h"
#include "wren_runtime.h"
#include "wren_bindings.h"
#include "wren/include/wren.h"

#include <stdlib.h>
#include <math.h>

// Forward declarations for engine-side AI helpers implemented in C.
extern void PF_changeyaw(void);
extern void SV_MoveToGoal(void);

typedef struct wren_entity_ref_s
{
    edict_t *edict;
} wren_entity_ref_t;

extern cvar_t sv_gameplayfix_random;

static qboolean module_is_engine(const char *module)
{
    return module && !strncmp(module, "q/", 2);
}

static void raise_not_implemented(WrenVM *vm)
{
    wrenEnsureSlots(vm, 1);
    wrenSetSlotString(vm, 0, "NotImplemented");
    wrenAbortFiber(vm, 0);
}

static wren_entity_ref_t *entity_from_slot(WrenVM *vm, int slot)
{
    return (wren_entity_ref_t *)wrenGetSlotForeign(vm, slot);
}

static edict_t *entity_get_edict(WrenVM *vm)
{
    wren_entity_ref_t *ref = entity_from_slot(vm, 0);
    if (!ref || !ref->edict)
    {
        wrenEnsureSlots(vm, 1);
        wrenSetSlotString(vm, 0, "Invalid entity handle");
        wrenAbortFiber(vm, 0);
        return NULL;
    }
    return ref->edict;
}

static qboolean read_vec3(WrenVM *vm, int slot, float *out)
{
    if (wrenGetSlotType(vm, slot) != WREN_TYPE_LIST)
        return false;

    int count = wrenGetListCount(vm, slot);
    if (count < 3)
        return false;

    wrenEnsureSlots(vm, slot + 2);
    for (int i = 0; i < 3; ++i)
    {
        wrenGetListElement(vm, slot, i, slot + 1);
        out[i] = (float)wrenGetSlotDouble(vm, slot + 1);
    }
    return true;
}

static void write_vec3(WrenVM *vm, const float *vec)
{
    wrenEnsureSlots(vm, 2);
    wrenSetSlotNewList(vm, 0);
    for (int i = 0; i < 3; ++i)
    {
        wrenSetSlotDouble(vm, 1, vec[i]);
        wrenInsertInList(vm, 0, i, 1);
    }
}

static void entity_get_origin(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    write_vec3(vm, ed->v.origin);
}

static void entity_set_origin(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    float vec[3];
    if (!read_vec3(vm, 1, vec))
    {
        raise_not_implemented(vm);
        return;
    }
    VectorCopy(vec, ed->v.origin);
}

static void entity_get_angles(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    write_vec3(vm, ed->v.angles);
}

static void entity_set_angles(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    float vec[3];
    if (!read_vec3(vm, 1, vec))
    {
        raise_not_implemented(vm);
        return;
    }
    VectorCopy(vec, ed->v.angles);
}

static void entity_get_velocity(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    write_vec3(vm, ed->v.velocity);
}

static void entity_set_velocity(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    float vec[3];
    if (!read_vec3(vm, 1, vec))
    {
        raise_not_implemented(vm);
        return;
    }
    VectorCopy(vec, ed->v.velocity);
}

static void entity_get_model(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    const char *model = PR_GetString(ed->v.model);
    wrenSetSlotString(vm, 0, model ? model : "");
}

static void entity_set_model(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    const char *model = wrenGetSlotString(vm, 1);
    ed->v.model = PR_SetEngineString(model ? model : "");
}

static void entity_get_solid(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    wrenSetSlotDouble(vm, 0, ed->v.solid);
}

static void entity_set_solid(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    ed->v.solid = (float)wrenGetSlotDouble(vm, 1);
}

static void entity_get_movetype(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    wrenSetSlotDouble(vm, 0, ed->v.movetype);
}

static void entity_set_movetype(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    ed->v.movetype = (float)wrenGetSlotDouble(vm, 1);
}

static void entity_get_health(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    wrenSetSlotDouble(vm, 0, ed->v.health);
}

static void entity_set_health(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    ed->v.health = (float)wrenGetSlotDouble(vm, 1);
}

static void entity_get_takedamage(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    wrenSetSlotDouble(vm, 0, ed->v.takedamage);
}

static void entity_set_takedamage(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    ed->v.takedamage = (float)wrenGetSlotDouble(vm, 1);
}

static void entity_spawn(WrenVM *vm)
{
    wren_entity_ref_t *ref = (wren_entity_ref_t *)wrenSetSlotNewForeign(vm, 0, 0, sizeof(wren_entity_ref_t));
    ref->edict = ED_Alloc();
}

static void entity_remove(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    ED_Free(ed);
    wren_entity_ref_t *ref = entity_from_slot(vm, 0);
    if (ref)
        ref->edict = NULL;
}

static void entity_link(WrenVM *vm)
{
    edict_t *ed = entity_get_edict(vm);
    if (!ed)
        return;
    SV_LinkEdict(ed, true);
}

static void entity_change_yaw_internal(edict_t *ent)
{
    if (!ent)
        return;

    int old_self = pr_global_struct->self;
    pr_global_struct->self = EDICT_TO_PROG(ent);
    PF_changeyaw();
    pr_global_struct->self = old_self;
}

static void entity_change_yaw(WrenVM *vm)
{
    edict_t *ent = entity_get_edict(vm);
    if (!ent)
        return;

    entity_change_yaw_internal(ent);
    wrenSetSlotNull(vm, 0);
}

static void entity_walk_move(WrenVM *vm)
{
    edict_t *ent = entity_get_edict(vm);
    if (!ent)
        return;

    double yaw = wrenGetSlotDouble(vm, 1);
    double dist = wrenGetSlotDouble(vm, 2);

    if (!((int)ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
    {
        wrenSetSlotBool(vm, 0, false);
        return;
    }

    vec3_t move;
    double yaw_rad = yaw * (M_PI * 2.0) / 360.0;
    move[0] = (float)(cos(yaw_rad) * dist);
    move[1] = (float)(sin(yaw_rad) * dist);
    move[2] = 0.0f;

    int old_self = pr_global_struct->self;
    dfunction_t *old_func = qcvm->xfunction;
    pr_global_struct->self = EDICT_TO_PROG(ent);
    qboolean result = SV_movestep(ent, move, true);
    qcvm->xfunction = old_func;
    pr_global_struct->self = old_self;

    wrenSetSlotBool(vm, 0, result ? true : false);
}

static void entity_move_to_goal(WrenVM *vm)
{
    edict_t *ent = entity_get_edict(vm);
    if (!ent)
        return;

    double dist = wrenGetSlotDouble(vm, 1);

    if (!((int)ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
    {
        wrenSetSlotBool(vm, 0, false);
        return;
    }

    int old_self = pr_global_struct->self;
    dfunction_t *old_func = qcvm->xfunction;
    float old_parm0 = G_FLOAT(OFS_PARM0);
    float old_return = G_FLOAT(OFS_RETURN);

    pr_global_struct->self = EDICT_TO_PROG(ent);
    G_FLOAT(OFS_PARM0) = (float)dist;
    SV_MoveToGoal();

    qcvm->xfunction = old_func;
    pr_global_struct->self = old_self;
    G_FLOAT(OFS_PARM0) = old_parm0;
    G_FLOAT(OFS_RETURN) = old_return;

    wrenSetSlotBool(vm, 0, true);
}

static void engine_time(WrenVM *vm)
{
    wrenSetSlotDouble(vm, 0, sv.qcvm.time);
}

static void engine_set_skill(WrenVM *vm)
{
    double skill_value = wrenGetSlotDouble(vm, 1);
    int new_skill = (int)(skill_value + 0.5);
    new_skill = CLAMP(0, new_skill, 3);
    Cvar_SetValueQuick(&skill, (float)new_skill);
    current_skill = new_skill;
    WRENVM_NotifySkillChanged(new_skill);
    wrenSetSlotDouble(vm, 0, (double)new_skill);
}

static void engine_get_skill(WrenVM *vm)
{
    wrenSetSlotDouble(vm, 0, (double)current_skill);
}

static void engine_load_map(WrenVM *vm)
{
    const char *map = wrenGetSlotString(vm, 1);
    if (!map)
    {
        raise_not_implemented(vm);
        return;
    }
    Cbuf_AddText(va("map %s\n", map));
}

static void engine_spawn_from_map(WrenVM *vm)
{
    (void)vm;
    WRENVM_SpawnEntitiesFallback();
}

static void engine_randf(WrenVM *vm)
{
    float num;
    if (sv_gameplayfix_random.value)
        num = ((rand() & 0x7fff) + 0.5f) * (1.f / 0x8000);
    else
        num = (rand() & 0x7fff) / ((float)0x7fff);
    wrenSetSlotDouble(vm, 0, num);
}

static void engine_randi(WrenVM *vm)
{
    int max = (int)wrenGetSlotDouble(vm, 1);
    if (max <= 0)
        max = 1;
    int value = rand() % max;
    wrenSetSlotDouble(vm, 0, (double)value);
}

static void engine_crandom(WrenVM *vm)
{
    engine_randf(vm);
    double val = wrenGetSlotDouble(vm, 0);
    wrenSetSlotDouble(vm, 0, val - 0.5);
}

static void entity_allocate(WrenVM *vm)
{
    wren_entity_ref_t *ref = (wren_entity_ref_t *)wrenSetSlotNewForeign(vm, 0, 0, sizeof(wren_entity_ref_t));
    ref->edict = NULL;
}

static void entity_finalize(void *data)
{
    (void)data;
}

static WrenForeignClassMethods bind_foreign_class(WrenVM *vm, const char *module, const char *className)
{
    WrenForeignClassMethods methods = {0};
    (void)vm;
    if (!module_is_engine(module))
        return methods;

    if (!strcmp(className, "Entity"))
    {
        methods.allocate = entity_allocate;
        methods.finalize = entity_finalize;
    }

    return methods;
}

static WrenForeignMethodFn bind_foreign_method(WrenVM *vm, const char *module, const char *className, bool isStatic, const char *signature)
{
    (void)vm;
    if (!module_is_engine(module))
        return NULL;

    if (!strcmp(className, "Engine"))
    {
        if (isStatic)
        {
            if (!strcmp(signature, "time")) return engine_time;
            if (!strcmp(signature, "setSkill(_)")) return engine_set_skill;
            if (!strcmp(signature, "getSkill")) return engine_get_skill;
            if (!strcmp(signature, "loadMap(_)")) return engine_load_map;
            if (!strcmp(signature, "spawnFromMap")) return engine_spawn_from_map;
            if (!strcmp(signature, "randf")) return engine_randf;
            if (!strcmp(signature, "randi(_)")) return engine_randi;
            if (!strcmp(signature, "crandom")) return engine_crandom;
        }
        return raise_not_implemented;
    }

    if (!strcmp(className, "Entity"))
    {
        if (isStatic)
        {
            if (!strcmp(signature, "spawn")) return entity_spawn;
        }
        else
        {
            if (!strcmp(signature, "remove")) return entity_remove;
            if (!strcmp(signature, "link")) return entity_link;
            if (!strcmp(signature, "changeYaw")) return entity_change_yaw;
            if (!strcmp(signature, "origin")) return entity_get_origin;
            if (!strcmp(signature, "origin=(_)")) return entity_set_origin;
            if (!strcmp(signature, "angles")) return entity_get_angles;
            if (!strcmp(signature, "angles=(_)")) return entity_set_angles;
            if (!strcmp(signature, "velocity")) return entity_get_velocity;
            if (!strcmp(signature, "velocity=(_)")) return entity_set_velocity;
            if (!strcmp(signature, "model")) return entity_get_model;
            if (!strcmp(signature, "model=(_)")) return entity_set_model;
            if (!strcmp(signature, "solid")) return entity_get_solid;
            if (!strcmp(signature, "solid=(_)")) return entity_set_solid;
            if (!strcmp(signature, "movetype")) return entity_get_movetype;
            if (!strcmp(signature, "movetype=(_)")) return entity_set_movetype;
            if (!strcmp(signature, "health")) return entity_get_health;
            if (!strcmp(signature, "health=(_)")) return entity_set_health;
            if (!strcmp(signature, "takedamage")) return entity_get_takedamage;
            if (!strcmp(signature, "takedamage=(_)")) return entity_set_takedamage;
            if (!strcmp(signature, "walkMove(_,_)")) return entity_walk_move;
            if (!strcmp(signature, "moveToGoal(_)")) return entity_move_to_goal;
        }
        return raise_not_implemented;
    }

    return NULL;
}

void WRENBindings_Configure(WrenConfiguration *config)
{
    config->bindForeignClassFn = bind_foreign_class;
    config->bindForeignMethodFn = bind_foreign_method;
}

void WRENBindings_BindRuntime(WrenVM *vm)
{
    (void)vm;
}

void WRENBindings_Shutdown(void)
{
}
