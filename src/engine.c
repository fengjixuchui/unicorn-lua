#include <unicorn/unicorn.h>

#include "unicornlua/engine.h"
#include "unicornlua/hooks.h"
#include "unicornlua/lua.h"
#include "unicornlua/unicornlua.h"
#include "unicornlua/utils.h"

const char * const kContextMetatableName = "unicornlua__context_meta";
const char * const kEngineMetatableName = "unicornlua__engine_meta";
const char * const kEnginePointerMapName = "unicornlua__engine_ptr_map";


const luaL_Reg kEngineMetamethods[] = {
    {"__gc", ul_close},
    {NULL, NULL}
};


const luaL_Reg kEngineInstanceMethods[] = {
    {"close", ul_close},
    {"context_restore", ul_context_restore},
    {"context_save", ul_context_save},
    {"emu_start", ul_emu_start},
    {"emu_stop", ul_emu_stop},
    {"errno", ul_errno},
    {"hook_add", ul_hook_add},
    {"hook_del", ul_hook_del},
    {"mem_map", ul_mem_map},
    {"mem_protect", ul_mem_protect},
    {"mem_read", ul_mem_read},
    {"mem_regions", ul_mem_regions},
    {"mem_unmap", ul_mem_unmap},
    {"mem_write", ul_mem_write},
    {"query", ul_query},
    {"reg_read", ul_reg_read},
    {"reg_read_batch", ul_reg_read_batch},
    {"reg_write", ul_reg_write},
    {"reg_write_batch", ul_reg_write_batch},
    {NULL, NULL}
};


void ul_init_engines_lib(lua_State *L) {
    /* Create a table with weak values where the engine pointer to engine object
     * mappings will be stored. */
    ul_create_weak_table(L, "v");
    lua_setfield(L, LUA_REGISTRYINDEX, kEnginePointerMapName);

    luaL_newmetatable(L, kEngineMetatableName);
    luaL_setfuncs(L, kEngineMetamethods, 0);

    lua_newtable(L);
    luaL_setfuncs(L, kEngineInstanceMethods, 0);
    lua_setfield(L, -2, "__index");

    /* Remove the metatables from the stack. */
    lua_pop(L, 2);
}


void ul_create_engine_object(lua_State *L, const uc_engine *engine) {
    UCLuaEngine *engine_object;

    engine_object = lua_newuserdata(L, sizeof(*engine_object));
    engine_object->engine = (uc_engine *)engine;

    luaL_setmetatable(L, kEngineMetatableName);

    /* Add a mapping of the engine pointer to the engine object so that hook
     * callbacks can get the engine object knowing only the pointer. */
    lua_getfield(L, LUA_REGISTRYINDEX, kEnginePointerMapName);
    lua_pushlightuserdata(L, (void *)engine);
    lua_pushvalue(L, -3);   /* Duplicate engine object as value */
    lua_settable(L, -3);
    lua_pop(L, 1);      /* Remove pointer map, engine object at TOS again */

    lua_newtable(L);
    engine_object->hook_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}


void ul_free_engine_object(lua_State *L, int engine_index) {
    UCLuaEngine *engine_object;
    int error, hook_table_index;

    engine_index = lua_absindex(L, engine_index);

    /* Deliberately not using ul_toengine, see below. */
    engine_object = (UCLuaEngine *)luaL_checkudata(L, engine_index, kEngineMetatableName);

    /* If the engine is already closed, don't try closing it again. Since the
     * engine is automatically closed when it gets garbage collected, if the
     * user manually closes it first this will result in an attempt to close an
     * already-closed engine. */
    if (engine_object->engine == NULL)
        return;

    lua_geti(L, LUA_REGISTRYINDEX, engine_object->hook_table_ref);
    hook_table_index = lua_absindex(L, -1);

    /* Release all hooks */
    lua_pushnil(L);
    while ((lua_next(L, hook_table_index)) != 0) {
        /* Hook object at TOS, light userdata used by Lua underneath it. */
        ul_hook_del_by_indexes(L, engine_index, -2);
        lua_pop(L, 1);
    }

    /* Remove hook table from stack and free it. */
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, engine_object->hook_table_ref);
    engine_object->hook_table_ref = LUA_NOREF;

    error = uc_close(engine_object->engine);
    if (error != UC_ERR_OK)
        ul_crash_on_error(L, error);

    /* Garbage collection should remove the engine object from the pointer map
     * table but it might not be doing it soon enough. */
    lua_getfield(L, LUA_REGISTRYINDEX, kEnginePointerMapName);
    lua_pushlightuserdata(L, (void *)engine_object->engine);
    lua_pushnil(L);
    lua_settable(L, -3);
    lua_pop(L, 1);          /* Remove pointer map */

    /* Clear out the engine pointer so we know it's closed now. */
    engine_object->engine = NULL;
}


void ul_get_engine_object(lua_State *L, const uc_engine *engine) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEnginePointerMapName);
    lua_pushlightuserdata(L, (void *)engine);
    lua_gettable(L, -2);

    if (lua_isnil(L, -1)) {
        /* Remove nil and engine pointer map at TOS */
        lua_pop(L, 2);
        luaL_error(L, "No engine object is registered for pointer %p.", engine);
    }

    /* Remove the engine pointer map from the stack. */
    lua_remove(L, -2);
}


UNICORN_EXPORT int ul_context_alloc(lua_State *L) {
    uc_engine *engine;
    uc_context *context;
    int error;

    engine = ul_toengine(L, 1);

    context = (uc_context *)lua_newuserdata(L, sizeof(context));
    luaL_setmetatable(L, kContextMetatableName);

    error = uc_context_alloc(engine, &context);
    if (error != UC_ERR_OK)
        return ul_crash_on_error(L, error);

    return 1;
}


UNICORN_EXPORT int ul_context_save(lua_State *L) {
    uc_engine *engine;
    uc_context *context;
    int error;

    engine = ul_toengine(L, 1);

    if (lua_gettop(L) < 2)
        /* Caller didn't pass a context to update, so create a new one. */
        ul_context_alloc(L);

    context = ul_tocontext(L, 2);

    error = uc_context_save(engine, context);
    if (error != UC_ERR_OK)
        return ul_crash_on_error(L, error);

    return 1;
}


UNICORN_EXPORT int ul_context_restore(lua_State *L) {
    uc_engine *engine;
    uc_context *context;
    int error;

    engine = ul_toengine(L, 1);
    context = ul_tocontext(L, 2);

    error = uc_context_restore(engine, context);
    if (error != UC_ERR_OK)
        return ul_crash_on_error(L, error);
    return 0;
}


UNICORN_EXPORT int ul_close(lua_State *L) {
    ul_free_engine_object(L, 1);
    return 0;
}


UNICORN_EXPORT int ul_query(lua_State *L) {
    uc_engine *engine;
    int query_type, error;
    size_t result;

    engine = ul_toengine(L, 1);
    query_type = luaL_checkinteger(L, 1);

    error = uc_query(engine, query_type, &result);
    if (error != UC_ERR_OK)
        return ul_crash_on_error(L, error);

    lua_pushinteger(L, result);
    return 1;
}


UNICORN_EXPORT int ul_errno(lua_State *L) {
    uc_engine *engine;

    engine = ul_toengine(L, 1);
    lua_pushinteger(L, uc_errno(engine));
    return 1;
}


UNICORN_EXPORT int ul_emu_start(lua_State *L) {
    uc_engine *engine;
    uint64_t start, end, timeout;
    size_t n_instructions;
    int error;

    engine = ul_toengine(L, 1);
    start = (uint64_t)luaL_checkinteger(L, 2);
    end = (uint64_t)luaL_checkinteger(L, 3);
    timeout = (uint64_t)luaL_optinteger(L, 4, 0);
    n_instructions = (size_t)luaL_optinteger(L, 5, 0);

    error = uc_emu_start(engine, start, end, timeout, n_instructions);
    if (error != UC_ERR_OK)
        return ul_crash_on_error(L, error);
    return 0;
}


UNICORN_EXPORT int ul_emu_stop(lua_State *L) {
    uc_engine *engine;
    int error;

    engine = ul_toengine(L, 1);

    error = uc_emu_stop(engine);
    if (error != UC_ERR_OK)
        return ul_crash_on_error(L, error);
    return 0;
}


uc_engine *ul_toengine(lua_State *L, int index) {
    UCLuaEngine *engine_object;

    engine_object = (UCLuaEngine *)luaL_checkudata(L, index, kEngineMetatableName);
    if (engine_object->engine == NULL)
        luaL_error(L, "Attempted to use closed engine.");

    return engine_object->engine;
}
