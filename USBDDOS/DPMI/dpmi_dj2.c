#include "USBDDOS/DPMI/dpmi.h"
#if defined(__DJ2__)
#include <conio.h>
#include <stdlib.h>
#include <dpmi.h>
#include <sys/farptr.h>
#include <sys/segments.h>
#include <sys/exceptn.h>
#include <crt0.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include "USBDDOS/DPMI/xms.h"
#include "USBDDOS/dbgutil.h"

extern DPMI_ADDRESSING DPMI_Addressing;

int _crt0_startup_flags = _CRT0_FLAG_FILL_DEADBEEF | _CRT0_FLAG_UNIX_SBRK | _CRT0_FLAG_LOCK_MEMORY;

static uint32_t DPMI_DSBase = 0;
static uint32_t DPMI_DSLimit = 0;
static BOOL DPMI_TSR_Inited = 0;
static uint16_t DPMI_Selector4G;

typedef struct _AddressMap
{
    uint32_t Handle;
    uint32_t LinearAddr;
    uint32_t PhysicalAddr;
    uint32_t Size;
}AddressMap;

#define ADDRMAP_TABLE_SIZE (256 / sizeof(AddressMap))

static AddressMap AddresMapTable[ADDRMAP_TABLE_SIZE];
static int AddressMapCount = 0;

static void AddAddressMap(const __dpmi_meminfo* info, uint32_t PhysicalAddr)
{
    if(AddressMapCount == ADDRMAP_TABLE_SIZE)
    {
        //error
    }
    AddressMap* map = &AddresMapTable[AddressMapCount++];

    map->Handle = info->handle;
    map->LinearAddr = info->address;
    map->PhysicalAddr = PhysicalAddr;
    map->Size = info->size;
}

static void DPMI_Shutdown(void);

#define NEW_IMPL 1
#if NEW_IMPL
extern uint32_t DPMI_InitTSR(uint32_t base, uint32_t newbase, uint32_t* poffset, uint32_t* psize);
extern BOOL DPMI_ShutdownTSR();
static uint32_t XMS_Bias;
#else
static __dpmi_meminfo XMS_Info;
#endif
#define ONLY_MSPACES 1
#define NO_MALLOC_STATS 1
#define USE_LOCKS 1
#define LACKS_SCHED_H 1
#define HAVE_MMAP 0
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma push_macro("DEBUG")
#undef DEBUG
#define DEBUG 0
#include "dlmalloc.h"
#pragma pop_macro("DEBUG")

#define XMS_HEAP_SIZE (1024*1024*4)  //maxium around 64M (actual size less than 64M)
_Static_assert((uint16_t)(XMS_HEAP_SIZE/1024) == (uint32_t)(XMS_HEAP_SIZE/1024), "XMS_HEAP_SIZE must be < 64M");
static mspace XMS_Space;
static uint32_t XMS_Physical;
#if DEBUG
static uint32_t XMS_Allocated;
#endif
static uint16_t XMS_Handle; //handle to XMS API

//http://www.delorie.com/djgpp/v2faq/faq18_13.html choice 3
//choice 1 (DOS alloc) is not suitable here since we need near ptr but DJGPP's DS base is larger than that, (we may need __djgpp_nearptr_enable)
//to do that. for most DOS Extenders like DOS/4G is OK to use choice 1 since there DS have a base of 0, thus DOS memory are near ptr.
static void Init_XMS()
{
    #if NEW_IMPL
    //use XMS memory since we need know the physical base for some non-DPMI DOS handlers, or drivers.
    //it is totally physically mapped, and doesn't support realloc, making malloc/brk/sbrk not working.
    //it will work if djgpp support srbk hook. stdout buffer will be pre-alloced for msg output & debug use.
    //doesn't matter, this is a driver anyway.
    uint32_t size = 0;
    uint32_t offset = 0;
    DPMI_InitTSR(0, 0, &offset, &size);

    //notify runtime of brk change
    sbrk(XMS_HEAP_SIZE);
    __dpmi_get_segment_base_address(_my_ds(), &DPMI_DSBase); //_CRT0_FLAG_UNIX_SBRK may have re-allocation

    assert((uint16_t)((size+XMS_HEAP_SIZE)/1024) == (uint32_t)((size+XMS_HEAP_SIZE)/1024)); //check overflow
    XMS_Handle = XMS_Alloc((uint16_t)((size+XMS_HEAP_SIZE)/1024), &XMS_Physical);
    if(XMS_Handle == 0)
        exit(1);

    XMS_Bias = offset >= 4096 ? 4096 : 0; //re-enable null pointer page fault
    uint32_t XMSBase = DPMI_MapMemory(XMS_Physical, size + XMS_HEAP_SIZE);
    DPMI_TSR_Inited = DPMI_InitTSR(DPMI_DSBase, XMSBase - XMS_Bias, &offset, &size);
    _LOG("TSR inited.\n");
    int ds; //reload ds incase this function inlined and ds optimized as previous
    asm __volatile__("mov %%ds, %0":"=r"(ds)::"memory");
    //_LOG("ds: %02x, limit: %08lx, new limit: %08lx\n", ds, __dpmi_get_segment_limit(ds), size-1);
    assert(__dpmi_get_segment_limit(ds) == size-1);
    __dpmi_set_segment_limit(ds, size + XMS_HEAP_SIZE - 1);
    XMS_Space = create_mspace_with_base((void*)size, XMS_HEAP_SIZE, 0);
    _LOG("XMS init done.\n");
    //update mapping
    DPMI_DSBase = XMSBase - XMS_Bias;
    DPMI_DSLimit = size + XMS_HEAP_SIZE;
    _LOG("XMS base %08lx, XMS lbase %08lx XMS heap base %08lx\n", XMS_Physical, XMSBase, XMS_Physical+size);
    #else
    //the idea is to allocate XMS memory in physical addr and mapped it after current ds's limit region,
    //then expand current ds' limit so that the mapped addr are within the current ds segment,
    //and the mapped data can be directly accessed as normal pointer (near ptr)
    //another trick is to use dlmalloc with mapped based ptr to allocate arbitary memory.
    XMS_Handle = XMS_Alloc((XMS_HEAP_SIZE+4096)/1024, &XMS_Physical);
    if(XMS_Handle == 0)
        exit(1);
    XMS_Physical = align(XMS_Physical, 4096);
    __dpmi_meminfo info = {0};
    info.size = XMS_HEAP_SIZE;
    #if 0     //Not supported by CWSDPMI and Windows, but by DPMIONE or HDPMI
    info.address = (DPMI_DSBase + DPMI_DSLimit + 4095) / 4096 * 4096;
    if( __dpmi_allocate_linear_memory(&info, 0) == -1)
    #else
    if(__dpmi_allocate_memory(&info) == -1)
    #endif
    {
        XMS_Free(XMS_Handle);
        printf("Failed to allocate linear memory (%08lx). \n", info.address);
        exit(1);
    }

    __dpmi_meminfo info2 = info;
    info2.address = 0;
    info2.size = XMS_HEAP_SIZE / 4096;
    if( __dpmi_map_device_in_memory_block(&info2, XMS_Physical) == -1) //supported by CWSDPMI and others
    {
        XMS_Free(XMS_Handle);
        printf("Error: Failed to map XMS memory %08lx, %08lx.\n", info.address, info.size);
        exit(1);
    }
    uint32_t XMSBase = info.address;
    XMS_Info = info;
    info.handle = -1;
    AddAddressMap(&info, XMS_Physical);
    __dpmi_set_segment_limit(_my_ds(), XMSBase - DPMI_DSBase + XMS_HEAP_SIZE - 1);
    __dpmi_set_segment_limit(__djgpp_ds_alias, XMSBase - DPMI_DSBase + XMS_HEAP_SIZE - 1); //interrupt used.
    _LOG("XMS base %08lx, XMS lbase %08lx offset %08lx\n", XMS_Physical, XMSBase, XMSBase - DPMI_DSBase);
    XMS_Space = create_mspace_with_base((void*)(XMSBase - DPMI_DSBase), XMS_HEAP_SIZE, 0);
    #endif
}

static void sig_handler(int signal)
{
    exit(-1);   //perform DPMI clean up on atexit
}

static void DPMI_InitFlat()
{
    DPMI_Selector4G = (uint16_t)__dpmi_allocate_ldt_descriptors(1);
    __dpmi_set_segment_base_address(DPMI_Selector4G, 0);
    __dpmi_set_segment_limit(DPMI_Selector4G, 0xFFFFFFFF);
    DPMI_Addressing.selector = DPMI_Selector4G;
    DPMI_Addressing.physical = FALSE;

    __dpmi_get_segment_base_address(_my_ds(), &DPMI_DSBase);
    DPMI_DSLimit = __dpmi_get_segment_limit(_my_ds());
}

void DPMI_Init(void)
{
    atexit(&DPMI_Shutdown);
    //signal(SIGINT, sig_handler);
    signal(SIGABRT, sig_handler);

    DPMI_InitFlat();
    
    Init_XMS();

    __dpmi_meminfo info;    //1:1 map DOS memory. (0~640K). TODO: get 640K~1M mapping from VCPI
    info.handle = -1;
    info.address = 1024;    //skip IVT and expose NULL ptr
    info.size = 640L*1024L - 1024;
    AddAddressMap(&info, 1024);

    /*
    int32_t* ptr = (int32_t*)DPMI_DMAMalloc(256,16);
    *ptr = 0xDEADBEEF;
    int32_t addr = DPMI_PTR2L(ptr);
    int32_t val = DPMI_LoadD(addr);
    printf("%08lx:%08lx\n",addr, val);

    int32_t addr2 = DPMI_MapMemory(DPMI_L2P(addr), 256);
    printf("%08lx:%08lx", addr2, DPMI_LoadD(addr2));
    exit(0);
    */
}

static void DPMI_Shutdown(void)
{
    #if NEW_IMPL
    _LOG("Cleanup TSR...\n");
    uint32_t size = mspace_mallinfo(XMS_Space).uordblks;
    unused(size); //make compiler happy. log not always enabled.
    #if DEBUG
    size = XMS_Allocated;
    #endif
    _LOG("XMS heap allocated: %d\n", size);
    if(DPMI_TSR_Inited)
    {
        DPMI_ShutdownTSR();
        asm __volatile__("":::"memory");
        DPMI_TSR_Inited = FALSE;
    }
    #else
    //libc may expand this limit, if we restore it to a smaller value, it may cause crash
    //__dpmi_set_segment_limit(_my_ds(), DPMI_DSLimit);
    _LOG("Free mapped XMS space...\n");
    if(XMS_Info.handle != 0)
    {
        __dpmi_free_memory(XMS_Info.handle);
        XMS_Info.handle = 0;
    }
    #endif

    _LOG("Free mapped space...\n");
    for(int i = 0; i < AddressMapCount; ++i)
    {
        AddressMap* map = &AddresMapTable[i];
        if(map->Handle == ~0UL)//XMS mapped
            continue;
        __dpmi_meminfo info;
        info.handle = map->Handle;
        info.address = map->LinearAddr;
        info.size = map->Size;
        __dpmi_free_physical_address_mapping(&info);
    }
    AddressMapCount = 0;
    _LOG("Free XMS memory...\n");
    if(XMS_Handle != 0)
    {
        XMS_Free(XMS_Handle);
        XMS_Handle = 0;
    }
}

uint32_t DPMI_L2P(uint32_t vaddr)
{
    for(int i = 0; i < AddressMapCount; ++i)
    {
        AddressMap* map = &AddresMapTable[i];
        if(map->LinearAddr <= vaddr && vaddr <= map->LinearAddr + map->Size)
        {
            int32_t offset = vaddr - map->LinearAddr;
            return map->PhysicalAddr + offset;
        }
    }
    printf("Error mapping linear address to physical: %08lx (%08lx,%08lx).\n", vaddr, DPMI_DSBase, DPMI_DSBase+DPMI_DSLimit);
    printf("Exit\n");
    exit(1);
    return 0; //make compiler happy
}

uint32_t DPMI_P2L(uint32_t paddr)
{
    for(int i = 0; i < AddressMapCount; ++i)
    {
        AddressMap* map = &AddresMapTable[i];
        if(map->PhysicalAddr <= paddr && paddr <= map->PhysicalAddr + map->Size)
        {
            int32_t offset = paddr - map->PhysicalAddr;
            return map->LinearAddr + offset;
        }
    }
    printf("Error mapping physical address to linear: %08lx.\n", paddr);
    assert(FALSE);
    exit(1);
    return 0; //make compiler happy
}

uint32_t DPMI_PTR2L(void* ptr)
{
    return ptr ? DPMI_DSBase + (uint32_t)ptr : 0;
}

void* DPMI_L2PTR(uint32_t addr)
{
    return addr > DPMI_DSBase ? (void*)(addr - DPMI_DSBase) : NULL;
}


uint32_t DPMI_MapMemory(uint32_t physicaladdr, uint32_t size)
{
    __dpmi_meminfo info;
    info.address = physicaladdr;
    info.size = size;
    if( __dpmi_physical_address_mapping(&info) != -1)
    {
        AddAddressMap(&info, physicaladdr);
        return info.address;
    }
    assert(FALSE);
    exit(-1);
    return 0;
}

uint32_t DPMI_UnmapMemory(uint32_t linearaddr)
{
    for(int i = 0; i < AddressMapCount; ++i)
    {
        AddressMap* map = &AddresMapTable[i];
        if(map->Handle == ~0UL)//XMS mapped
            continue;
        if(map->LinearAddr != linearaddr)
            continue;
        __dpmi_meminfo info;
        info.handle = map->Handle;
        info.address = map->LinearAddr;
        info.size = map->Size;
        __dpmi_free_physical_address_mapping(&info);
        map->Handle = ~0UL; //hack for prevent release again
        return 0;
    }
    return -1;
}

void* DPMI_DMAMalloc(unsigned int size, unsigned int alignment/* = 4*/)
{
    #if DEBUG
    CLIS();
    XMS_Allocated += size;
    uint8_t* ptr = (uint8_t*)mspace_malloc(XMS_Space, size+alignment+8) + 8;
    uintptr_t addr = (uintptr_t)ptr;
    uint32_t offset = align(addr, alignment) - addr;
    uint32_t* uptr = (uint32_t*)(ptr + offset);
    uptr[-1] = size;
    uptr[-2] = offset + 8;
    STIL();
    assert(align((uintptr_t)uptr, alignment) == (uintptr_t)uptr);
    return uptr;
    #else//DEBUG
    return mspace_memalign(XMS_Space, alignment, size);
    #endif//DEBUG
}

void* DPMI_DMAMallocNCPB(unsigned int size, unsigned int alignment/* = 4*/)
{
    #if DEBUG
    CLIS();
    XMS_Allocated += size;
    uint8_t* ptr = (uint8_t*)mspace_malloc(XMS_Space, size+alignment+8) + 8;
    uintptr_t addr = (uintptr_t)ptr;
    uint32_t offset = align(addr, alignment) - addr;
    uint32_t* uptr = (uint32_t*)(ptr + offset);
    uptr[-1] = size;
    uptr[-2] = offset + 8;
    STIL();
    assert(align((uintptr_t)uptr, alignment) == (uintptr_t)uptr);
    return uptr;
    #else//DEBUG

    #define TRY 64
    void* tries[TRY] = {0};
    tries[0] = mspace_memalign(XMS_Space, alignment, size);
    int i = 0;
    while(i<TRY && ((align((uintptr_t)tries[i],alignment))&~0xFFFUL) != ((align((uintptr_t)tries[i],alignment)+size-1)&~0xFFFUL))
        tries[++i] = mspace_memalign(XMS_Space, alignment, size);
    if(i == TRY)
    {
        assert(FALSE);
        return NULL;
    }
    for(int j = 0; j < i; ++j)
        mspace_free(XMS_Space, tries[j]);
    return tries[i];
    #undef TRY

    #endif//DEBUG
}

void DPMI_DMAFree(void* ptr)
{
    #if DEBUG
    uint32_t* uptr = (uint32_t*)ptr;
    XMS_Allocated -= uptr[-1];
    mspace_free(XMS_Space, (uint8_t*)ptr - uptr[-2]);
    #else
    mspace_free(XMS_Space, ptr);
    #endif
}

uint32_t DPMI_DOSMalloc(uint16_t size)
{
    int selector = 0;
    uint16_t segment = (uint16_t)__dpmi_allocate_dos_memory(size, &selector);
    if(segment != -1)
        return (selector << 16) | segment;
    else
        return 0;
}

void DPMI_DOSFree(uint32_t segment)
{
    __dpmi_free_dos_memory((uint16_t)(segment>>16));
}

uint16_t DPMI_CallRealModeRETF(DPMI_REG* reg)
{
    reg->d._reserved = 0;
    return (uint16_t)__dpmi_simulate_real_mode_procedure_retf((__dpmi_regs*)reg);
}

uint16_t DPMI_CallRealModeINT(uint8_t i, DPMI_REG* reg)
{
    reg->d._reserved = 0;
    return (uint16_t)__dpmi_simulate_real_mode_interrupt(i, (__dpmi_regs*)reg);
}

uint16_t DPMI_CallRealModeIRET(DPMI_REG* reg)
{
    reg->d._reserved = 0;
    return (uint16_t)__dpmi_simulate_real_mode_procedure_iret((__dpmi_regs*)reg);
}

#define DPMI_ISR_CHAINED 1

uint16_t DPMI_InstallISR(uint8_t i, void(*ISR)(void), DPMI_ISR_HANDLE* outputp handle)
{
    if(i < 0 || i > 255 || handle == NULL)
        return -1;
        
    _go32_dpmi_seginfo go32pa = {0};
    go32pa.pm_selector = (uint16_t)_my_cs();
    go32pa.pm_offset = (uintptr_t)ISR;
    #if !DPMI_ISR_CHAINED
    //_go32_interrupt_stack_size = 2048; //512 minimal, default 16K
    if( _go32_dpmi_allocate_iret_wrapper(&go32pa) != 0)
        return -1;
    #endif

    __dpmi_raddr ra;
    __dpmi_get_real_mode_interrupt_vector(i, &ra);
    __dpmi_paddr pa;
    __dpmi_get_protected_mode_interrupt_vector(i, &pa);

    handle->offset = pa.offset32;
    handle->cs = pa.selector;
    handle->rm_cs = ra.segment;
    handle->rm_offset = ra.offset16;
    handle->extra = go32pa.pm_offset;
    handle->n = i;

#if DPMI_ISR_CHAINED
    return (uint16_t)_go32_dpmi_chain_protected_mode_interrupt_vector(i, &go32pa);
#else
    return (uint16_t)_go32_dpmi_set_protected_mode_interrupt_vector(i, &go32pa);
#endif
}

uint16_t DPMI_UninstallISR(DPMI_ISR_HANDLE* inputp handle)
{
     _go32_dpmi_seginfo go32pa;
     go32pa.pm_selector = handle->cs;
     go32pa.pm_offset = handle->offset;
     int result = _go32_dpmi_set_protected_mode_interrupt_vector(handle->n, &go32pa);

    //__dpmi_raddr ra;
    //ra.segment = handle->rm_cs;
    //ra.offset16 = handle->rm_offset;
    //result = __dpmi_set_real_mode_interrupt_vector(handle->n, &ra) | result;

    go32pa.pm_offset = handle->extra;
#if DPMI_ISR_CHAINED
    return (uint16_t)result;
#else
    return (uint16_t)(_go32_dpmi_free_iret_wrapper(&go32pa) | result);
#endif
}

uint32_t DPMI_AllocateRMCB_RETF(void(*Fn)(void), DPMI_REG* reg)
{
    _go32_dpmi_seginfo info;
    info.pm_selector = (uint16_t)_my_cs();
    info.pm_offset = (uintptr_t)Fn;
    if(_go32_dpmi_allocate_real_mode_callback_retf(&info, (_go32_dpmi_registers*)reg) == 0)
    {
        return (((uint32_t)info.rm_segment)<<16) | (info.rm_offset);
    }
    else
        return 0;
}

uint32_t DPMI_AllocateRMCB_IRET(void(*Fn)(void), DPMI_REG* reg)
{
    _go32_dpmi_seginfo info;
    info.pm_selector = (uint16_t)_my_cs();
    info.pm_offset = (uintptr_t)Fn;
    if(_go32_dpmi_allocate_real_mode_callback_iret(&info, (_go32_dpmi_registers*)reg) == 0)
    {
        return (((uint32_t)info.rm_segment)<<16) | (info.rm_offset);
    }
    else
        return 0;
}

void DPMI_GetPhysicalSpace(DPMI_SPACE* outputp spc)
{
    #if NEW_IMPL//doesn't work with old method.
    spc->baseds = XMS_Physical - XMS_Bias;
    spc->limitds = __dpmi_get_segment_limit(_my_ds());
    spc->basecs = spc->baseds;
    spc->limitcs = __dpmi_get_segment_limit(_my_cs());

    extern uint32_t __djgpp_stack_top;
    _LOG("DPMI_GetPhysicalSpace: physical ds base: %08lx, limit: %08lx, esp %08lx\n", spc->baseds, __dpmi_get_segment_limit(_my_ds()), __djgpp_stack_top);
    spc->stackpointer = __djgpp_stack_top;
    #else
    #error not supported. cannot get physical base from DPMI.
    #endif
}

#endif
