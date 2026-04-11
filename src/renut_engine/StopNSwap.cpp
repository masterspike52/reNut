#include <rex/ppc.h>
#include "renut_logging.h"
#include "rex_macros.h"
#include <rex/cvar.h>


#if defined(_MSC_VER)
#include <stdlib.h>
#define REX_BSWAP32(x) _byteswap_ulong(x)
#else
#define REX_BSWAP32(x) __builtin_bswap32(x)
#endif

REX_PPC_EXTERN_IMPORT(gameFlagsSetFlag_82363ED0);

static std::once_flag g_StopNSwap_once;
static uint32_t a1 = 0;

static void StopNSwap()
{
    std::call_once(g_StopNSwap_once, []()
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(0x100000000);
            uint32_t flags_ptr = REX_BSWAP32(*reinterpret_cast<uint32_t*>(base + a1 + 92));

            *(base + flags_ptr + 236) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10E, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x703, 1);

            *(base + flags_ptr + 237) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x110, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x705, 1);

            *(base + flags_ptr + 238) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10D, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x702, 1);

            *(base + flags_ptr + 239) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10C, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x701, 1);

            *(base + flags_ptr + 240) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10B, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x700, 1);

            *(base + flags_ptr + 241) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10F, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x704, 1);

            *(base + flags_ptr + 242) = 1;
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x111, 1);
            REX_PPC_INVOKE2(void, gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x706, 1);
        });
}

REXCVAR_DEFINE_COMMAND(StopNSwap, StopNSwap, "Nuts&Bolts/Events", "Triggers StopNSwop");

void StopNSwap_hook(PPCRegister& r22)
{
   a1 = r22.u32 - 76;
}