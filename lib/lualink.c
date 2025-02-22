#include "lib/lualink.h"

#include <stdio.h>
#include <string.h> // strcmp(), strlen()
#include <stdlib.h>

// Lua itself
//#include "../submodules/lua/src/lua.h" // in header
#include "../submodules/lua/src/lauxlib.h"
#include "../submodules/lua/src/lualib.h"

// Hardware IO
#include "lib/slopes.h"     // S_toward
#include "lib/casl.h"       // C-ASL
#include "lib/ashapes.h"    // AShaper_unset_scale(), AShaper_set_scale()
#include "lib/detect.h"     // Detect*
#include "lib/caw.h"        // Caw_send_*()
#include "lib/ii.h"         // ii_*()
#include "lib/bootloader.h" // bootloader_enter()
#include "lib/metro.h"      // metro_start() metro_stop() metro_set_time()
#include "lib/clock.h"      // clock_*()
#include "lib/io.h"         // IO_GetADC()
#include "../ll/random.h"   // Random_Get()
#include "../ll/adda.h"     // CAL_*()
#include "../ll/cal_ll.h"   // CAL_LL_ActiveChannel()
#include "../ll/system.h"   // getUID_Word()
#include "lib/events.h"     // event_t event_post()
#include "stm32f7xx_hal.h"  // HAL_GetTick()
#include "stm32f7xx_it.h"   // CPU_GetCount()

// Lua libs wrapped in C-headers: Note the extra '.h'
#include "lua/bootstrap.lua.h" // MUST LOAD THIS MANUALLY FIRST
#include "lua/crowlib.lua.h"
#include "lua/asl.lua.h"
#include "lua/asllib.lua.h"
#include "lua/clock.lua.h"
#include "lua/metro.lua.h"
#include "lua/public.lua.h"
#include "lua/input.lua.h"
#include "lua/output.lua.h"
#include "lua/ii.lua.h"
#include "build/iihelp.lua.h"    // generated lua stub for loading i2c modules
#include "lua/calibrate.lua.h"
#include "lua/sequins.lua.h"
#include "lua/quote.lua.h"

#include "build/ii_lualink.h" // generated C header for linking to lua

#define WATCHDOG_FREQ      0x100000 // ~1s how often we run the watchdog
#define WATCHDOG_COUNT     2        // how many watchdogs before 'frozen'

// mark the 3rd arg 'false' if you need to debug that library
const struct lua_lib_locator Lua_libs[] =
    { { "lua_crowlib"   , lua_crowlib   , true}
    , { "lua_asl"       , lua_asl       , true}
    , { "lua_asllib"    , lua_asllib    , true}
    , { "lua_clock"     , lua_clock     , true}
    , { "lua_metro"     , lua_metro     , true}
    , { "lua_input"     , lua_input     , true}
    , { "lua_output"    , lua_output    , true}
    , { "lua_public"    , lua_public    , true}
    , { "lua_ii"        , lua_ii        , true}
    , { "build_iihelp"  , build_iihelp  , true}
    , { "lua_calibrate" , lua_calibrate , true}
    , { "lua_sequins"   , lua_sequins   , true}
    , { "lua_quote"     , lua_quote     , true}
    , { NULL            , NULL          , true}
    };

// Basic crow script
#include "lua/First.lua.h"

// Private prototypes
static void Lua_linkctolua( lua_State* L );
static float Lua_check_memory( void );
static int Lua_call_usercode( lua_State* L, int nargs, int nresults );
static int Lua_handle_error( lua_State* L );
static void timeouthook( lua_State* L, lua_Debug* ar );

// Handler prototypes
void L_handle_asl_done( event_t* e );
void L_handle_metro( event_t* e );
void L_handle_stream( event_t* e );
void L_handle_change( event_t* e );
void L_handle_ii_leadRx( event_t* e );;
void L_handle_ii_followRx( event_t* e );
void L_handle_ii_followRx_cont( uint8_t cmd, int args, float* data );
void L_handle_window( event_t* e );
void L_handle_in_scale( event_t* e );
void L_handle_volume( event_t* e );
void L_handle_peak( event_t* e );
void L_handle_clock_resume( event_t* e );
void L_handle_clock_start( event_t* e );
void L_handle_clock_stop( event_t* e );
void L_handle_freq( event_t* e );

void _printf(char* error_message)
{
    printf("%s\n",error_message);
}

lua_State* L; // global access for 'reset-environment'

// Public functions
lua_State* Lua_Init(void)
{
    L = luaL_newstate();
    luaL_openlibs(L);
    Lua_linkctolua(L);
    Lua_eval(L, lua_bootstrap
              , strlen(lua_bootstrap)
              , "=lib"
              ); // redefine dofile(), print(), load crowlib
    return L;
}

lua_State* Lua_Reset( void )
{
    printf("Lua_Reset\n");
    Metro_stop_all();
    for( int i=0; i<2; i++ ){
        Detect_none( Detect_ix_to_p(i) );
    }
    for( int i=0; i<4; i++ ){
        S_toward( i, 0.0, 0.0, SHAPE_Linear, NULL );
    }
    events_clear();
    clock_cancel_coro_all();
    Lua_DeInit();
    return Lua_Init();
}

void Lua_load_default_script( void )
{
    Lua_eval(L, lua_First
              , strlen(lua_First)
              , "=First.lua"
              );
}

void Lua_DeInit(void)
{
    lua_close(L);
}

// C-fns accessible to lua

// NB these static functions are prefixed  with '_'
// to avoid shadowing similar-named extern functions in other modules
// and also to distinguish from extern 'L_' functions.

// this is a somewhat arbitrary size. must be big enough for the biggest library. 8kB was too small
#define SIZED_STRING_LEN 0x4000 // (16kB)
struct sized_string{
    char data[SIZED_STRING_LEN];
    int  len;
};
static int _writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    UNUSED(L);
    struct sized_string* chunkstr = ud; /// need explicit cast?
    if(chunkstr->len + sz >= SIZED_STRING_LEN){
        printf("chunkstr too small.\n");
        return 1;
    }
    memcpy(&chunkstr->data[chunkstr->len], p, sz);
    chunkstr->len += sz;
    return 0;
}
static int _load_chunk(lua_State* L, const char* code, int strip)
{
    int retval = 0;
    struct sized_string chunkstr = {.len = 0};
    { // scope lua_State to destroy it asap
        lua_State* LL=luaL_newstate();
        if( !LL ){ printf("luaL_newstate failed\n"); return 1; }
        if( luaL_loadstring(LL, code) ){
            printf("loadstring error\n");
            retval = 1;
            goto close_LL;
        }
        if( lua_dump(LL, _writer, &chunkstr, strip) ){
            printf("dump error\n");
            retval = 1;
            goto close_LL;
        }
close_LL:
        lua_close(LL);
    }
    luaL_loadbuffer(L, chunkstr.data, chunkstr.len, chunkstr.data); // load our compiled chunk
    return retval;
}

static int _open_lib( lua_State *L, const struct lua_lib_locator* lib, const char* name )
{
    uint8_t i = 0;
    while( lib[i].addr_of_luacode != NULL ){
        if( !strcmp( name, lib[i].name ) ){ // if the strings match
            if( _load_chunk(L, lib[i].addr_of_luacode, lib[i].stripped) ){
                printf("can't load library: %s\n", (char*)lib[i].name );
                printf( "%s\n", (char*)lua_tostring( L, -1 ) );
                lua_pop( L, 1 );
                return -1; // error
            }
            if( lua_pcall(L, 0, LUA_MULTRET, 0) ){
                printf("can't exec library: %s\n", (char*)lib[i].name );
                printf( "%s\n", (char*)lua_tostring( L, -1 ) );
                lua_pop( L, 1 );
                return -1; // error
            }
            return 1; // table is left on the stack as retval
        }
        i++;
    }
    return 0; // not found
}

static int _dofile( lua_State *L )
{
    const char* l_name = luaL_checkstring(L, 1);
    lua_pop( L, 1 );
    switch( _open_lib( L, Lua_libs, l_name ) ){
        case -1: goto fail;
        case 1: return 1;
        default: break;
    }
    switch( _open_lib( L, Lua_ii_libs, l_name ) ){
        case -1: goto fail;
        case 1: return 1;
        default: break;
    }
    printf("can't open library: %s\n", (char*)l_name);
fail:
    lua_pushnil(L);
    return 1;
}
static int _debug( lua_State *L )
{
    const char* msg = luaL_checkstring(L, 1);
    lua_pop( L, 1 );
    printf( "%s\n",(char*)msg);
    lua_settop(L, 0);
    return 0;
}
static int _print_serial( lua_State *L )
{
    Caw_send_luachunk( (char*)luaL_checkstring(L, 1) );
    lua_pop( L, 1 );
    lua_settop(L, 0);
    return 0;
}
static int _print_tell( lua_State *L )
{
    int nargs = lua_gettop(L);
    // nb: luaL_checkstring() will coerce ints & nums into strings
    switch( nargs ){
        case 0:
            return luaL_error(L, "no event to tell.");
        case 1:
            Caw_printf( "^^%s()", luaL_checkstring(L, 1) );
            break;
        case 2:
            Caw_printf( "^^%s(%s)", luaL_checkstring(L, 1)
                                  , luaL_checkstring(L, 2) );
            break;
        case 3:
            Caw_printf( "^^%s(%s,%s)", luaL_checkstring(L, 1)
                                     , luaL_checkstring(L, 2)
                                     , luaL_checkstring(L, 3) );
            break;
        case 4:
            Caw_printf( "^^%s(%s,%s,%s)", luaL_checkstring(L, 1)
                                        , luaL_checkstring(L, 2)
                                        , luaL_checkstring(L, 3)
                                        , luaL_checkstring(L, 4) );
            break;
        case 5:
            Caw_printf( "^^%s(%s,%s,%s,%s)", luaL_checkstring(L, 1)
                                           , luaL_checkstring(L, 2)
                                           , luaL_checkstring(L, 3)
                                           , luaL_checkstring(L, 4)
                                           , luaL_checkstring(L, 5) );
            break;
        default:
            return luaL_error(L, "too many args to tell.");
    }
    lua_pop( L, nargs );
    lua_settop(L, 0);
    return 0;
}
static int _bootloader( lua_State *L )
{
    bootloader_enter();
    return 0;
}
static int _unique_id( lua_State *L )
{
    lua_pushinteger(L, getUID_Word(0));
    lua_pushinteger(L, getUID_Word(4));
    lua_pushinteger(L, getUID_Word(8));
    return 3;
}
static int _time( lua_State *L )
{
    lua_pushinteger(L, HAL_GetTick());
    return 1;
}
static int _cpu_time( lua_State *L )
{
    // returns count of background loops for the last 8ms
    lua_pushinteger(L, CPU_GetCount());
    return 1;
}
static int _get_state( lua_State *L )
{
    float s = AShaper_get_state( luaL_checkinteger(L, 1)-1 );
    lua_pop( L, 1 );
    lua_pushnumber( L, s );
    return 1;
}
static int _set_scale( lua_State *L )
{
    // statically save the mod & scaling options
    // if omitting mod & scaling, they use the most recent value of mod/scaling
    // if no value ever provided, the initial values act as defaults
    // NB: shared between outputs. if you need separate mod/scale, must be explicit
    static float mod = 12.0; // default to 12TET
    static float scaling = 1.0; // default to v/8

    int nargs = lua_gettop(L);
    // first arg is index!

    // special cases:
    if( nargs == 1 ){ // no user arguments
        float divs[1] = {0.0};
        AShaper_set_scale( luaL_checknumber( L, 1 )-1 // index is 1-based in lua
                         , divs
                         , 1
                         , 1
                         , 1.0/12.0 // hack it not to need the array
                         );
        lua_pop( L, 1 ); // pop index
        return 0;
    } else if( lua_isstring( L, 2 ) ){ // if arg1 == 'none' -> disable scaling
        AShaper_unset_scale( luaL_checknumber( L, 1 )-1 ); // lua is 1-based
        lua_pop( L, 2 );
        return 0;
    }

    // arg1 is a list:
        // empty list == chromatic
        // 12TET semitones based at 0
        // just ratios relative to 1/1 in the [1,2) range
    int tlen = lua_rawlen( L, 2 ); // length of the table
    float divs[tlen];
    for( int i=0; i<tlen; i++ ){             // iterate table to get pitch list
        lua_pushnumber( L, i+1 );            // lua is 1-based!
        lua_gettable( L, 2 );                // table is still in index 2
        divs[i] = luaL_checknumber( L, -1 ); // value is now on top of the stack
        lua_pop( L, 1 );                     // remove our introspected value
    }

    if( nargs >= 3 ){
        // TODO allow string = 'just' to select JI mode for note list
        mod = luaL_checknumber( L, 3 );
    }

    if( nargs >= 4 ){
        scaling = luaL_checknumber( L, 4 );
    }

    AShaper_set_scale( luaL_checknumber( L, 1 )-1 // index is 1-based in lua
                     , divs
                     , tlen
                     , mod
                     , scaling
                     );

    lua_pop( L, nargs );
    return 0;
}
static int _io_get_input( lua_State *L )
{
    float adc = IO_GetADC( luaL_checkinteger(L, 1)-1 );
    lua_pop( L, 1 );
    lua_pushnumber( L, adc );
    return 1;
}
static int _set_input_none( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        Detect_none( d );
    }
    lua_pop( L, 1 );
    lua_settop(L, 0);
    return 0;
}
static int _set_input_stream( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        Detect_stream( d
                     , L_queue_stream
                     , luaL_checknumber(L, 2)
                     );
    }
    lua_pop( L, 2 );
    lua_settop(L, 0);
    return 0;
}
static int _set_input_change( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        Detect_change( d
                     , L_queue_change
                     , luaL_checknumber(L, 2)
                     , luaL_checknumber(L, 3)
                     , Detect_str_to_dir( luaL_checkstring(L, 4) )
                     );
    }
    lua_pop( L, 4 );
    lua_settop(L, 0);
    return 0;
}
static int _set_input_window( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        // capture window table from lua
        int wLen = lua_rawlen( L, 2 );           // length of the table
        float wins[wLen];
        for( int i=0; i<wLen; i++ ){             // iterate table to get windows
            lua_pushnumber( L, i+1 );            // lua is 1-based!
            lua_gettable( L, 2 );                // table is in index 2
            wins[i] = luaL_checknumber( L, -1 ); // value is now on top of the stack
            lua_pop( L, 1 );                     // remove our introspected value
        }
        Detect_window( d
                     , L_queue_window
                     , wins
                     , wLen
                     , luaL_checknumber(L, 3) // hysteresis
                     );
    }
    lua_pop( L, 3 );
    return 0;
}
static int _set_input_scale( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        int sLen = lua_rawlen( L, 2 ); // length of the scale table
        float scale[sLen];
        for( int i=0; i<sLen; i++ ){              // iterate table to get pitch list
            lua_pushnumber( L, i+1 );             // lua is 1-based!
            lua_gettable( L, 2 );                 // table is still in index 2
            scale[i] = luaL_checknumber( L, -1 ); // value is now on top of the stack
            lua_pop( L, 1 );                      // remove our introspected value
        }
        Detect_scale( d
                    , L_queue_in_scale
                    , scale
                    , sLen
                    , luaL_checknumber(L, 3) // divs-per-octave
                    , luaL_checknumber(L, 4) // volts-per-octave
                    );
    }
    lua_pop( L, 4 );
    return 0;
}
static int _set_input_volume( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        Detect_volume( d
                     , L_queue_volume
                     , luaL_checknumber(L, 2)
                     );
    }
    lua_pop( L, 2 );
    lua_settop(L, 0);
    return 0;
}
static int _set_input_peak( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        Detect_peak( d
                   , L_queue_peak
                   , luaL_checknumber(L, 2)
                   , luaL_checknumber(L, 3)
                   );
    }
    lua_pop( L, 3 );
    lua_settop(L, 0);
    return 0;
}
static int _set_input_freq( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        Detect_freq( d
                   , L_queue_freq
                   , luaL_checknumber(L, 2)
                   );
    }
    lua_pop( L, 2 );
    lua_settop(L, 0);
    return 0;
}
static int _set_input_clock( lua_State *L )
{
    uint8_t ix = luaL_checkinteger(L, 1)-1;
    Detect_t* d = Detect_ix_to_p( ix ); // Lua is 1-based
    if(d){ // valid index
        clock_set_source(CLOCK_SOURCE_CROW);
        clock_crow_in_div(luaL_checknumber(L, 2));
        Detect_change( d
                     , clock_input_handler
                     , luaL_checknumber(L, 3)
                     , luaL_checknumber(L, 4)
                     , Detect_str_to_dir("r")
                     );
    }
    lua_pop( L, 4 );
    lua_settop(L, 0);
    return 0;
}

// CASL
static int _casl_describe( lua_State *L )
{
    casl_describe( luaL_checkinteger(L, 1)-1 // C is zero-based
                 , L // descriptor is on top of the stack
                 );
    lua_pop( L, 2 );
    lua_settop(L, 0);
    return 0;
}
static int _casl_action( lua_State *L )
{
    casl_action( luaL_checkinteger(L, 1)-1 // C is zero-based
               , luaL_checkinteger(L, 2) );
    lua_pop(L, 2);
    lua_settop(L, 0);
    return 0;
}
static int _casl_defdynamic( lua_State *L )
{
    int c_ix = luaL_checkinteger(L, 1)-1; // lua is 1-based
    lua_pop(L, 1);
    lua_pushinteger( L, casl_defdynamic( c_ix ) );
    return 1;
}
static int _casl_cleardynamics( lua_State *L )
{
    casl_cleardynamics( luaL_checkinteger(L, 1)-1 ); // lua is 1-based
    lua_pop(L, 1);
    return 0;
}
static int _casl_setdynamic( lua_State *L )
{
    casl_setdynamic( luaL_checkinteger(L, 1)-1 // lua is 1-based
                   , luaL_checkinteger(L, 2)
                   , luaL_checknumber(L, 3)
                   );
    lua_pop(L, 3);
    return 0;
}
static int _casl_getdynamic( lua_State *L )
{
    float d = casl_getdynamic( luaL_checkinteger(L, 1)-1 // lua is 1-based
                             , luaL_checkinteger(L, 2)
                             );
    lua_pop(L, 2);
    lua_pushnumber(L, d);
    return 1;
}

static int _send_usb( lua_State *L )
{
    // pattern match on type: handle values vs strings vs chunk
    const char* msg = luaL_checkstring(L, 1);
    lua_pop( L, 1 );
    uint32_t len = strlen(msg);
    Caw_send_raw( (uint8_t*) msg, len );
    lua_settop(L, 0);
    return 0;
}

static int _ii_list_modules( lua_State *L )
{
    Caw_send_luachunk( (char*)ii_list_modules() );
    printf( "printing ii help\n" );
    return 0;
}

static int _ii_list_commands( lua_State *L )
{
    uint8_t address = luaL_checkinteger(L, 1);
    printf("i2c help %i\n", address);
    Caw_stream_constchar( ii_list_cmds(address) );
    lua_pop(L, 1);
    return 0;
}

static int _ii_pullup( lua_State *L )
{
    ii_set_pullups( luaL_checkinteger(L, 1) );
    lua_pop(L, 1);
    return 0;
}

static int _ii_lead( lua_State *L )
{
    float data[4] = {0,0,0,0}; // always zero out data
    int nargs = lua_gettop(L);
    if( nargs > 2
     && nargs <= 6 ){
        for( int i=0; i<(nargs-2); i++ ){
            data[i] = luaL_checknumber(L, i+3);
        }
    }
    if( ii_leader_enqueue( luaL_checkinteger(L, 1) // address
                         , luaL_checkinteger(L, 2) // command
                         , data
                         ) ){ printf("ii_lead failed\n"); }
    lua_settop(L, 0);
    return 0;
}
static int _ii_lead_bytes( lua_State *L )
{
    int nargs = lua_gettop(L);
    if( nargs != 3 ) return 0;
    uint8_t address = luaL_checkinteger(L, 1);
    size_t len;
    uint8_t *data = (uint8_t *)luaL_checklstring(L, 2, &len);
    uint8_t rx_len = (uint8_t)luaL_checkinteger(L, 3);
    if( ii_leader_enqueue_bytes( address
                               , data
                               , (uint8_t)len
                               , rx_len
                               ) ){ printf("ii_lead_bytes failed\n"); }
    lua_settop(L, 0);
    return 0;
}

static int _ii_address( lua_State *L )
{
    ii_set_address( luaL_checkinteger(L, 1) );
    lua_pop( L, 1 );
    lua_settop(L, 0);
    return 0;
}
static int _ii_get_address( lua_State *L )
{
    lua_pushinteger( L, ii_get_address() );
    return 1;
}
static int _metro_start( lua_State* L )
{
    static int ix = 0;
    float seconds = -1.0; // metro will re-use previous value
    int count = -1; // default: infinite
    int stage = 0;

    int nargs = lua_gettop(L);
    if( nargs > 0 ){ ix = (int) luaL_checkinteger(L, 1) - 1; } // 1-ix'd
    if( nargs > 1 ){ seconds = (float)luaL_checknumber(L, 2); }
    if( nargs > 2 ){ count = (int)luaL_checkinteger(L, 3); }
    if( nargs > 3 ){ stage = (int)luaL_checkinteger(L, 4) - 1; } // 1-ix'd
    lua_pop( L, 4 );

    if( seconds >= 0.0 ){ // if negative, leave previous time
        // limit to 500uS to avoid crash
        Metro_set_time( ix, (seconds < 0.0005) ? 0.0005 : seconds );
    }
    Metro_set_count( ix, count );
    Metro_set_stage( ix, stage );
    Metro_start( ix );
    lua_settop(L, 0);
    return 0;
}
static int _metro_stop( lua_State* L )
{
    if( lua_gettop(L) != 1 ){ return luaL_error(L, "wrong number of arguments"); }

    int ix = (int)luaL_checkinteger(L, 1) - 1; // 1-ix'd
    lua_pop( L, 1 );
    Metro_stop(ix);
    lua_settop(L, 0);
    return 0;
}
static int _metro_set_time( lua_State* L )
{
    if( lua_gettop(L) != 2 ){ return luaL_error(L, "wrong number of arguments"); }

    int ix = (int)luaL_checkinteger(L, 1) - 1; // 1-ix'd
    float seconds = (float)luaL_checknumber(L, 2);
    lua_pop( L, 2 );

    if( seconds >= 0.0 ){ // if negative, leave previous time
        // limit to 500uS to avoid crash
        Metro_set_time( ix, (seconds < 0.0005) ? 0.0005 : seconds );
    }

    lua_settop(L, 0);
    return 0;
}

static int _random_float( lua_State* L )
{
    lua_pushnumber( L, Random_Float() );
    return 1;
}

static int _random_int( lua_State* L )
{
    int r = Random_Int( luaL_checknumber(L, 1)
                      , luaL_checknumber(L, 2) );
    lua_pop(L, 2);
    lua_pushinteger( L, r);
    return 1;
}

static int _calibrate_source( lua_State* L )
{
    int chan = -1;
    const char* src = luaL_checkstring(L, 1); // get string, or coerce int to string
    if( strlen(src) > 1 ){ // assume string
        switch(src[0]){ case 'g':{ chan=5; break; }
                        case '2':{ chan=4; break; }
        }
    } else {
        switch(src[0]){ case '1':{ chan=3; break; }
                        case '2':{ chan=2; break; }
                        case '3':{ chan=1; break; }
                        case '4':{ chan=0; break; }
        }
    }
    if(chan != -1){
        CAL_LL_ActiveChannel(chan);
    } else {
        Caw_send_luachunk("cal.source: unknown source. use {1,2,3,4,'gnd','2v5'}");
    }
    lua_pop(L, 1);
    return 0;
}
static int _calibrate_get( lua_State* L )
{
    int chan = luaL_checkinteger(L, 1);
    const char* msg = luaL_checkstring(L, 2);
    float r = CAL_Get(chan, (msg[0]=='o') ? CAL_Offset : CAL_Scale);
    lua_pop(L, 2);
    lua_pushnumber(L, r);
    return 1;
}
static int _calibrate_set( lua_State* L )
{
    int chan = luaL_checkinteger(L, 1);
    const char* msg = luaL_checkstring(L, 2);
    float val = luaL_checknumber(L, 3);
    CAL_Set(chan, (msg[0]=='o') ? CAL_Offset : CAL_Scale, val);
    lua_pop(L, 3);
    return 0;
}
static int _calibrate_save( lua_State* L )
{
    CAL_WriteFlash();
    return 0;
}

// clock
static int _clock_cancel( lua_State* L )
{
    int coro_id = (int)luaL_checkinteger(L, 1);
    clock_cancel_coro(coro_id);
    lua_pop(L, 1);
    return 0;
}
static int _clock_schedule_sleep( lua_State* L )
{
    int coro_id = (int)luaL_checkinteger(L, 1);
    float seconds = luaL_checknumber(L, 2);

    if( seconds <= 0 ){
        L_queue_clock_resume(coro_id); // immediate callback
    } else {
        clock_schedule_resume_sleep(coro_id, seconds);
    }
    lua_pop(L, 2);
    return 0;
}
static int _clock_schedule_sync( lua_State* L )
{
    int coro_id = (int)luaL_checkinteger(L, 1);
    float beats = luaL_checknumber(L, 2);

    if (beats <= 0) {
        L_queue_clock_resume(coro_id); // immediate callback
    } else {
        clock_schedule_resume_sync(coro_id, beats);
    }
    lua_pop(L, 2);
    return 0;
}
static int _clock_get_time_beats( lua_State* L )
{
    lua_pushnumber(L, clock_get_time_beats());
    return 1;
}
static int _clock_get_tempo( lua_State* L )
{
    lua_pushnumber(L, clock_get_tempo());
    return 1;
}
static int _clock_set_source( lua_State* L )
{
    clock_set_source( (int)luaL_checkinteger(L, 1)-1 ); // lua is 1-based
    lua_pop(L, 1);
    return 0;
}
static int _clock_internal_set_tempo( lua_State* L )
{
    float bpm = luaL_checknumber(L, 1);
    clock_internal_set_tempo(bpm);
    lua_pop(L, 1);
    return 0;
}
static int _clock_internal_start( lua_State* L )
{
    float new_beat = luaL_checknumber(L, 1);
    clock_set_source(CLOCK_SOURCE_INTERNAL);
    clock_internal_start(new_beat, true);
    lua_pop(L, 1);
    return 0;
}
static int _clock_internal_stop( lua_State* L )
{
    clock_set_source(CLOCK_SOURCE_INTERNAL);
    clock_internal_stop();
    return 0;
}

static int _pub_view_in( lua_State* L )
{
    int chan = luaL_checkinteger(L, 1)-1; // lua is 1-based
    bool state;
    if(lua_isboolean(L, 2)){ state = lua_toboolean(L, 2); }
    else{ state = (bool)lua_tointeger(L, 2); }
    IO_public_set_view(chan+4, state);
    lua_pop(L, 2);
    return 0;
}
static int _pub_view_out( lua_State* L )
{
    int chan = luaL_checkinteger(L, 1)-1; // lua is 1-based
    bool state;
    if(lua_isboolean(L, 2)){ state = lua_toboolean(L, 2); }
    else{ state = (bool)lua_tointeger(L, 2); }
    IO_public_set_view(chan, state);
    lua_pop(L, 2);
    return 0;
}


// array of all the available functions
static const struct luaL_Reg libCrow[]=
        // bootstrap
    { { "c_dofile"         , _dofile           }
    , { "debug_usart"      , _debug            }
    , { "print_serial"     , _print_serial     }
    , { "tell"             , _print_tell       }
        // system
    , { "sys_bootloader"   , _bootloader       }
    , { "unique_id"        , _unique_id        }
    , { "time"             , _time             }
    , { "cputime"          , _cpu_time         }
    //, { "sys_cpu_load"     , _sys_cpu          }
        // io
    , { "get_state"        , _get_state        }
    , { "set_output_scale" , _set_scale        }
    , { "io_get_input"     , _io_get_input     }
    , { "set_input_none"   , _set_input_none   }
    , { "set_input_stream" , _set_input_stream }
    , { "set_input_change" , _set_input_change }
    , { "set_input_scale"  , _set_input_scale  }
    , { "set_input_window" , _set_input_window }
    , { "set_input_volume" , _set_input_volume }
    , { "set_input_peak"   , _set_input_peak   }
    , { "set_input_freq"   , _set_input_freq   }
    , { "set_input_clock"  , _set_input_clock  }
        // casl
    , { "casl_describe"    , _casl_describe    }
    , { "casl_action"      , _casl_action      }
    , { "casl_defdynamic"  , _casl_defdynamic  }
    , { "casl_cleardynamics", _casl_cleardynamics }
    , { "casl_setdynamic"  , _casl_setdynamic  }
    , { "casl_getdynamic"  , _casl_getdynamic  }
        // usb
    , { "send_usb"         , _send_usb         }
        // i2c
    , { "ii_list_modules"  , _ii_list_modules  }
    , { "ii_list_commands" , _ii_list_commands }
    , { "ii_pullup"        , _ii_pullup        }
    , { "ii_lead"          , _ii_lead          }
    , { "ii_lead_bytes"    , _ii_lead_bytes    }
    , { "ii_set_add"       , _ii_address       }
    , { "ii_get_add"       , _ii_get_address   }
        // metro
    , { "metro_start"      , _metro_start      }
    , { "metro_stop"       , _metro_stop       }
    , { "metro_set_time"   , _metro_set_time   }
        // random
    , { "random_float"     , _random_float     }
    , { "random_int"       , _random_int       }
        // calibration
    , { "calibrate_source" , _calibrate_source }
    , { "calibrate_get"    , _calibrate_get    }
    , { "calibrate_set"    , _calibrate_set    }
    , { "calibrate_save"   , _calibrate_save   }
        // clock
    , { "clock_cancel"             , _clock_cancel             }
    , { "clock_schedule_sleep"     , _clock_schedule_sleep     }
    , { "clock_schedule_sync"      , _clock_schedule_sync      }
    , { "clock_get_time_beats"     , _clock_get_time_beats     }
    , { "clock_get_tempo"          , _clock_get_tempo          }
    , { "clock_set_source"         , _clock_set_source         }
        // clock.internal
    , { "clock_internal_set_tempo" , _clock_internal_set_tempo }
    , { "clock_internal_start"     , _clock_internal_start     }
    , { "clock_internal_stop"      , _clock_internal_stop      }
        // public
    , { "pub_view_in"       , _pub_view_in      }
    , { "pub_view_out"      , _pub_view_out     }

    , { NULL               , NULL              }
    };
// make functions available to lua
static void Lua_linkctolua( lua_State *L )
{
    // Make C fns available to Lua
    uint8_t fn = 0;
    while( libCrow[fn].func != NULL ){
        lua_register( L, libCrow[fn].name, libCrow[fn].func );
        fn++;
    }
}

uint8_t Lua_eval( lua_State*     L
                , const char*    script
                , size_t         script_len
                , const char*    chunkname
                ){
    int error = luaL_loadbuffer( L, script, script_len, chunkname );
    if( error != LUA_OK ){
        Caw_send_luachunk( (char*)lua_tostring( L, -1 ) );
        lua_pop( L, 1 );
        return 1;
    }

    if( (error |= Lua_call_usercode( L, 0, 0 )) != LUA_OK ){
        lua_pop( L, 1 );
        switch( error ){
            case LUA_ERRSYNTAX: Caw_send_luachunk("syntax error."); break;
            case LUA_ERRMEM:    Caw_send_luachunk("not enough memory."); break;
            case LUA_ERRRUN:    Caw_send_luachunk("runtime error."); break;
            case LUA_ERRERR:    Caw_send_luachunk("error in error handler."); break;
            default: break;
        }
        return 1;
    }
    return 0;
}

static float Lua_check_memory( void )
{
    lua_getglobal(L,"collectgarbage");
    lua_pushstring(L, "count");
    lua_pcall(L,1,1,0); // NOT PROTECTED (called from watchdog)
    float mem = luaL_checknumber(L, 1);
    lua_pop(L,1);
    return mem;
}

void Lua_crowbegin( void )
{
    printf("init()\n"); // call in C to avoid user seeing in lua
    lua_getglobal(L,"init");
    if( Lua_call_usercode(L,0,0) != LUA_OK ){
        lua_pop(L, 1);
    }
    Caw_send_luachunk("^^ready()"); // inform host that script is initialized
}


// Watchdog timer for infinite looped Lua scripts
volatile int watchdog = WATCHDOG_COUNT;
static void timeouthook( lua_State* L, lua_Debug* ar )
{
    if( --watchdog <= 0 ){
        Caw_send_luachunk("CPU timed out.");
        lua_sethook(L, timeouthook, LUA_MASKLINE, 0); // error until top
        luaL_error(L, "user code timeout exceeded");
    }
}

static int Lua_handle_error( lua_State *L )
{
    const char *msg = lua_tostring( L, 1 );
    if( msg == NULL ){
        if( luaL_callmeta( L, 1, "__tostring" )
         && lua_type ( L, -1 ) == LUA_TSTRING ) {
            return 1;
        } else {
            msg = lua_pushfstring( L
                                 , "(error object is a %s value)"
                                 , luaL_typename( L, 1 ) );
        }
    }
    luaL_traceback( L, L, msg, 1 );
    char* traceback = (char*)lua_tostring( L, -1 );
    Caw_send_luachunk( traceback );
    _printf( traceback );
    return 1;
}

static int Lua_call_usercode( lua_State* L, int nargs, int nresults )
{
    lua_sethook(L, timeouthook, LUA_MASKCOUNT, WATCHDOG_FREQ); // reset timeout hook
    watchdog = WATCHDOG_COUNT; // reset timeout hook counter

    int errFunc = lua_gettop(L) - nargs;
    lua_pushcfunction( L, Lua_handle_error );
    lua_insert( L, errFunc );
    int status = lua_pcall(L, nargs, nresults, errFunc);
    lua_remove( L, errFunc );

    lua_sethook(L, timeouthook, 0, 0);

    return status;
}


// Public Callbacks from C to Lua
void L_queue_asl_done( int id )
{
    event_t e = { .handler = L_handle_asl_done
                , .index.i = id
                };
    event_post(&e);
}
void L_handle_asl_done( event_t* e )
{
    lua_getglobal(L, "asl_done_handler");
    lua_pushinteger(L, e->index.i + 1); // 1-ix'd
    if( Lua_call_usercode(L, 1, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_metro( int id, int state )
{
    event_t e = { .handler = L_handle_metro
                , .index.i = id
                , .data.i  = state
                };
    event_post(&e);
}
void L_handle_metro( event_t* e )
{
    lua_getglobal(L, "metro_handler");
    lua_pushinteger(L, e->index.i +1); // 1-ix'd
    lua_pushinteger(L, e->data.i +1);  // 1-ix'd
    if( Lua_call_usercode(L, 2, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_stream( int id, float state )
{
    event_t e = { .handler = L_handle_stream
                , .index.i = id
                , .data.f  = state
                };
    event_post(&e);
}
void L_handle_stream( event_t* e )
{
    lua_getglobal(L, "stream_handler");
    lua_pushinteger(L, e->index.i +1); // 1-ix'd
    lua_pushnumber(L, e->data.f);
    if( Lua_call_usercode(L, 2, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_change( int id, float state )
{
    event_t e = { .handler = L_handle_change
                , .index.i = id
                , .data.f  = state
                };
    event_post(&e);
}
void L_handle_change( event_t* e )
{
    lua_getglobal(L, "change_handler");
    lua_pushinteger(L, e->index.i +1); // 1-ix'd
    lua_pushnumber(L, e->data.f);
    if( Lua_call_usercode(L, 2, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_ii_leadRx( uint8_t address, uint8_t cmd, float data, uint8_t arg )
{
    event_t e = { .handler = L_handle_ii_leadRx
                , .data.f  = data
                };
    e.index.u8s[0] = address;
    e.index.u8s[1] = cmd;
    e.index.u8s[2] = arg;
    event_post(&e);
}
void L_handle_ii_leadRx( event_t* e )
{
    lua_getglobal(L, "ii_LeadRx_handler");
    lua_pushinteger(L, e->index.u8s[0]); // address
    lua_pushinteger(L, e->index.u8s[1]); // command
    lua_pushinteger(L, e->index.u8s[2]); // arg
    lua_pushnumber(L, e->data.f);
    if( Lua_call_usercode(L, 4, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_ii_followRx( void )
{
    event_t e = { .handler = L_handle_ii_followRx };
    event_post(&e);
}
void L_handle_ii_followRx( event_t* e )
{
    // FIXME: should pass the 'cont' as a fnptr continuation
    ii_process_dequeue_decode();
}
void L_handle_ii_followRx_cont( uint8_t cmd, int args, float* data )
{
    lua_getglobal(L, "ii_followRx_handler");
    lua_pushinteger(L, cmd);
    int a = args;
    while(a-- > 0){
        lua_pushnumber(L, *data++);
    }
    if( Lua_call_usercode(L, 1+args, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

// FIXME called directly from ii lib for now
float L_handle_ii_followRxTx( uint8_t cmd, int args, float* data )
{
    lua_getglobal(L, "ii_followRxTx_handler");
    lua_pushinteger(L, cmd);
    int a = args;
    while(a-- > 0){
        lua_pushnumber(L, *data++);
    }
    if( Lua_call_usercode(L, 1+args, 1) != LUA_OK ){
        lua_pop( L, 1 );
    }
    float n = luaL_checknumber(L, 1);
    lua_pop( L, 1 );
    return n;
}

void L_queue_in_scale( int id, float note )
{
    event_t e = { .handler = L_handle_in_scale
                , .index.i = id
                };
    event_post(&e);
}
void L_handle_in_scale( event_t* e )
{
    lua_getglobal(L, "scale_handler");
    Detect_t* d = Detect_ix_to_p( e->index.i );
    // TODO these should be wrapped in a table here rather than lua
    lua_pushinteger(L, e->index.i +1); // 1-ix'd
    lua_pushinteger(L, d->scale.lastIndex +1); // 1-ix'd
    lua_pushinteger(L, d->scale.lastOct);
    lua_pushnumber(L, d->scale.lastNote);
    lua_pushnumber(L, d->scale.lastVolts);
    if( Lua_call_usercode(L, 5, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_window( int id, float window )
{
    event_t e = { .handler = L_handle_window
                , .index.i = id
                };
    if( window >= 0.0 ){
        e.data.u8s[0] = window;
        e.data.u8s[1] = 1;
    } else {
        e.data.u8s[0] = -window; // flip sign for positive index
        e.data.u8s[1] = 0;
    }
    event_post(&e);
}
void L_handle_window( event_t* e )
{
    lua_getglobal(L, "window_handler");
    lua_pushinteger(L, e->index.i+1); // 1-ix'd
    lua_pushinteger(L, e->data.u8s[0]);
    lua_pushnumber(L, e->data.u8s[1]);
    if( Lua_call_usercode(L, 3, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_volume( int id, float level )
{
    event_t e = { .handler = L_handle_volume
                , .index.i = id
                , .data.f  = level
                };
    event_post(&e);
}
void L_handle_volume( event_t* e )
{
    lua_getglobal(L, "volume_handler");
    lua_pushinteger(L, e->index.i+1); // 1-ix'd
    lua_pushnumber(L, e->data.f);
    if( Lua_call_usercode(L, 2, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_peak( int id, float ignore )
{
    event_t e = { .handler = L_handle_peak
                , .index.i = id
                };
    event_post(&e);
}
void L_handle_peak( event_t* e )
{
    lua_getglobal(L, "peak_handler");
    lua_pushinteger(L, e->index.i +1); // 1-ix'd
    if( Lua_call_usercode(L, 1, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_freq( int id, float freq )
{
    event_t e = { .handler = L_handle_freq
                , .index.i = id
                , .data.f  = freq
                };
    event_post(&e);
}
void L_handle_freq( event_t* e )
{
    lua_getglobal(L, "freq_handler");
    lua_pushinteger(L, e->index.i +1); // 1-ix'd
    lua_pushnumber(L, e->data.f);
    if( Lua_call_usercode(L, 2, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_clock_resume( int coro_id )
{
    event_t e = { .handler = L_handle_clock_resume
                , .index.i = coro_id
                };
    event_post(&e);
}
void L_handle_clock_resume( event_t* e )
{
    lua_getglobal(L, "clock_resume_handler");
    lua_pushinteger(L, e->index.i);
    if( Lua_call_usercode(L, 1, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_clock_start( void )
{
    event_t e = { .handler = L_handle_clock_start };
    event_post(&e);
}
void L_handle_clock_start( event_t* e )
{
    lua_getglobal(L, "clock_start_handler");
    if( Lua_call_usercode(L, 0, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}

void L_queue_clock_stop( void )
{
    event_t e = { .handler = L_handle_clock_stop };
    event_post(&e);
}
void L_handle_clock_stop( event_t* e )
{
    lua_getglobal(L, "clock_stop_handler");
    if( Lua_call_usercode(L, 0, 0) != LUA_OK ){
        lua_pop( L, 1 );
    }
}
