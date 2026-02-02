#include "ps2_runtime.h"
#include "gs_renderer.h"
#include "ps2_syscalls.h"
#include "ps2_runtime_macros.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <thread>
#include <unordered_map>
#include "raylib.h"

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture

struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

#define PT_LOAD 1 // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 448;

std::atomic<int> g_activeThreads{0};

static void UploadFrame(Texture2D &tex, PS2Runtime *rt)
{
    const GSRegisters &gs = rt->memory().gs();
    GSRenderer &renderer = rt->renderer();

    // DISPFBUF1 fields: FBP (bits 0-8) * 2048 bytes, FBW (bits 10-15) blocks of 64 pixels, PSM (bits 16-20)
    uint32_t dispfb = static_cast<uint32_t>(gs.dispfb1 & 0xFFFFFFFFULL);
    uint32_t fbp = dispfb & 0x1FF;
    uint32_t fbw = (dispfb >> 10) & 0x3F;
    uint32_t psm = (dispfb >> 16) & 0x1F;

    // DISPLAY1 fields: DX,DY not used here; DW,DH are width/height minus 1 (11 bits each)
    uint64_t display64 = gs.display1;
    uint32_t dw = static_cast<uint32_t>((display64 >> 23) & 0x7FF);
    uint32_t dh = static_cast<uint32_t>((display64 >> 34) & 0x7FF);

    GSRenderer::FramebufferConfig config;
    config.basePointer = fbp;
    config.width = (fbw ? fbw : (FB_WIDTH / 64));
    config.height = (dh ? (dh + 1) : FB_HEIGHT);
    
    // Map PS2 PSM to Renderer PixelFormat
    switch (psm)
    {
    case 0: config.format = GSRenderer::PixelFormat::PSMCT32; break;
    case 2: config.format = GSRenderer::PixelFormat::PSMCT16; break;
    default: config.format = GSRenderer::PixelFormat::PSMCT32; break;
    }

    // Update renderer's internal buffer
    renderer.updateFramebuffer(rt->memory(), config);

    // If dirty, update the Raylib texture
    if (renderer.isFramebufferDirty())
    {
        UpdateTexture(tex, renderer.getFramebufferRGBA().data());
        renderer.clearFramebufferDirty();
    }
}

PS2Runtime::PS2Runtime()
{
    std::memset(&m_cpuContext, 0, sizeof(m_cpuContext));
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_functionTable.clear();
    m_loadedModules.clear();
}

PS2Runtime::~PS2Runtime()
{
    m_loadedModules.clear();
    m_functionTable.clear();
}

bool PS2Runtime::initialize(const char *title)
{
    if (!m_memory.initialize())
    {
        std::cerr << "Failed to initialize PS2 memory" << std::endl;
        return false;
    }

    if (!m_renderer.initialize(FB_WIDTH, FB_HEIGHT))
    {
        std::cerr << "Failed to initialize GS Renderer" << std::endl;
        return false;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(FB_WIDTH, FB_HEIGHT, title);
    SetTargetFPS(60);

    return true;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    ElfHeader header;
    file.read(reinterpret_cast<char *>(&header), sizeof(header));

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        ProgramHeader ph;
        file.seekg(header.phoff + i * header.phentsize);
        file.read(reinterpret_cast<char *>(&ph), sizeof(ph));

        if (ph.type == PT_LOAD && ph.filesz > 0)
        {
            std::cout << "Loading segment: 0x" << std::hex << ph.vaddr
                      << " - 0x" << (ph.vaddr + ph.memsz)
                      << " (size: 0x" << ph.memsz << ")" << std::dec << std::endl;

            std::vector<uint8_t> buffer(ph.filesz);
            file.seekg(ph.offset);
            file.read(reinterpret_cast<char *>(buffer.data()), ph.filesz);

            uint32_t physAddr = m_memory.translateAddress(ph.vaddr);
            uint8_t *dest = nullptr;
            if (ph.vaddr >= PS2_SCRATCHPAD_BASE && ph.vaddr < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
            {
                dest = m_memory.getScratchpad() + physAddr;
            }
            else
            {
                dest = m_memory.getRDRAM() + physAddr;
            }
            std::memcpy(dest, buffer.data(), ph.filesz);

            if (ph.memsz > ph.filesz)
            {
                std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
            }

            if (ph.flags & 0x1) // PF_X
            {
                m_memory.registerCodeRegion(ph.vaddr, ph.vaddr + ph.memsz);
            }
        }
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = 0x00100000;
    module.size = 0;
    module.active = true;
    m_loadedModules.push_back(module);

    std::cout << "ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec << std::endl;
    return true;
}

void PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    m_functionTable[address] = func;
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    return m_functionTable.find(address) != m_functionTable.end();
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        return it->second;
    }

    std::cerr << "Warning: Function at address 0x" << std::hex << address << std::dec << " not found" << std::endl;

    static RecompiledFunction defaultFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::cerr << "Error: Called unimplemented function at address 0x" << std::hex << ctx->pc << std::dec << std::endl;
    };

    return defaultFunction;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
    }
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    static std::unordered_map<uint32_t, int> seen;
    int &count = seen[address];
    if (count < 3)
    {
        std::cout << "[VU0] microprogram @0x" << std::hex << address
                  << " pc=0x" << ctx->pc
                  << std::dec << std::endl;
    }
    ++count;
    ctx->vu0_clip_flags = 0;
    ctx->vu0_status = 0;
    ctx->vu0_q = 1.0f;
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    // PS2 syscall number is in $v1 (register 3)
    uint32_t syscallNum = GPR_U32(ctx, 3);
    
    static int logCount = 0;
    if (logCount < 20)
    {
        std::cout << "[Syscall] #" << std::dec << syscallNum 
                  << " (0x" << std::hex << syscallNum << std::dec << ")"
                  << " at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
        ++logCount;
    }

    switch (syscallNum)
    {
    case 2:  // SetGsCrt
        ps2_syscalls::GsSetCrt(rdram, ctx, this);
        break;
    case 4:  // Exit
        std::cout << "[Syscall] Exit called" << std::endl;
        setReturnS32(ctx, 0);
        break;
    case 7:  // ExecPS2 / SleepThread
        ps2_syscalls::SleepThread(rdram, ctx, this);
        break;
    case 20: // _EnableIntc
        ps2_syscalls::EnableIntc(rdram, ctx, this);
        break;
    case 21: // _DisableIntc
        ps2_syscalls::DisableIntc(rdram, ctx, this);
        break;
    case 22: // _EnableDmac
        ps2_syscalls::EnableDmac(rdram, ctx, this);
        break;
    case 23: // _DisableDmac
        ps2_syscalls::DisableDmac(rdram, ctx, this);
        break;
    case 24: // _SetAlarm
        ps2_syscalls::SetAlarm(rdram, ctx, this);
        break;
    case 32: // CreateThread
        ps2_syscalls::CreateThread(rdram, ctx, this);
        break;
    case 33: // DeleteThread
        ps2_syscalls::DeleteThread(rdram, ctx, this);
        break;
    case 34: // StartThread
        ps2_syscalls::StartThread(rdram, ctx, this);
        break;
    case 35: // ExitThread
        ps2_syscalls::ExitThread(rdram, ctx, this);
        break;
    case 36: // ExitDeleteThread
        ps2_syscalls::ExitDeleteThread(rdram, ctx, this);
        break;
    case 37: // TerminateThread
        ps2_syscalls::TerminateThread(rdram, ctx, this);
        break;
    case 41: // ChangeThreadPriority
        ps2_syscalls::ChangeThreadPriority(rdram, ctx, this);
        break;
    case 43: // RotateThreadReadyQueue
        ps2_syscalls::RotateThreadReadyQueue(rdram, ctx, this);
        break;
    case 45: // ReleaseWaitThread
        ps2_syscalls::ReleaseWaitThread(rdram, ctx, this);
        break;
    case 47: // GetThreadId
        ps2_syscalls::GetThreadId(rdram, ctx, this);
        break;
    case 48: // ReferThreadStatus
        ps2_syscalls::ReferThreadStatus(rdram, ctx, this);
        break;
    case 50: // SleepThread
        ps2_syscalls::SleepThread(rdram, ctx, this);
        break;
    case 51: // WakeupThread
        ps2_syscalls::WakeupThread(rdram, ctx, this);
        break;
    case 52: // iWakeupThread
        ps2_syscalls::iWakeupThread(rdram, ctx, this);
        break;
    case 55: // SuspendThread
        ps2_syscalls::SuspendThread(rdram, ctx, this);
        break;
    case 57: // ResumeThread
        ps2_syscalls::ResumeThread(rdram, ctx, this);
        break;
    case 60: // SetupThread
        ps2_syscalls::SetupThread(rdram, ctx, this);
        break;
    case 61: // SetupHeap / EndOfHeap
        {
            // SetupHeap: $a0 = heap_start, $a1 = heap_size
            // Returns: heap end address in $v0
            uint32_t heapStart = GPR_U32(ctx, 4);
            uint32_t heapSize = GPR_U32(ctx, 5);
            uint32_t heapEnd = heapStart + heapSize;
            std::cout << "[Syscall] SetupHeap: start=0x" << std::hex << heapStart 
                      << " size=0x" << heapSize 
                      << " end=0x" << heapEnd << std::dec << std::endl;
            setReturnS32(ctx, static_cast<int32_t>(heapEnd));
        }
        break;
    case 62: // EndOfHeap
        {
            uint32_t heapEnd = GPR_U32(ctx, 4);
            std::cout << "[Syscall] EndOfHeap: 0x" << std::hex << heapEnd << std::dec << std::endl;
            setReturnS32(ctx, static_cast<int32_t>(heapEnd));
        }
        break;
    case 64: // CreateSema
        ps2_syscalls::CreateSema(rdram, ctx, this);
        break;
    case 65: // DeleteSema
        ps2_syscalls::DeleteSema(rdram, ctx, this);
        break;
    case 66: // SignalSema
        ps2_syscalls::SignalSema(rdram, ctx, this);
        break;
    case 67: // iSignalSema
        ps2_syscalls::iSignalSema(rdram, ctx, this);
        break;
    case 68: // WaitSema
        ps2_syscalls::WaitSema(rdram, ctx, this);
        break;
    case 69: // PollSema
        ps2_syscalls::PollSema(rdram, ctx, this);
        break;
    case 70: // iPollSema
        ps2_syscalls::iPollSema(rdram, ctx, this);
        break;
    case 71: // ReferSemaStatus
        ps2_syscalls::ReferSemaStatus(rdram, ctx, this);
        break;
    case 100: // FlushCache
        ps2_syscalls::FlushCache(rdram, ctx, this);
        break;
    case 112: // GsGetIMR
        ps2_syscalls::GsGetIMR(rdram, ctx, this);
        break;
    case 113: // GsPutIMR
        ps2_syscalls::GsPutIMR(rdram, ctx, this);
        break;
    default:
        std::cerr << "[Syscall] Unhandled syscall #" << syscallNum 
                  << " at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
        setReturnS32(ctx, 0);
        break;
    }
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    std::cout << "Break encountered at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    std::cout << "Trap encountered at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    std::cout << "TLBR (TLB Read) at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    std::cout << "TLBWI (TLB Write Indexed) at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    std::cout << "TLBWR (TLB Write Random) at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    std::cout << "TLBP (TLB Probe) at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    ctx->cop0_status &= ~0x00000002;
    std::cout << "LL bit cleared at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    std::cerr << "Integer overflow exception at PC: 0x" << std::hex << ctx->pc << std::dec << std::endl;
    m_cpuContext.cop0_epc = ctx->pc;
    m_cpuContext.cop0_cause |= (EXCEPTION_INTEGER_OVERFLOW << 2);
    m_cpuContext.pc = 0x80000000;
}

void PS2Runtime::run()
{
    RecompiledFunction entryPoint = lookupFunction(m_cpuContext.pc);

    m_cpuContext.r[4] = _mm_set1_epi32(0);           // A0 = 0 (argc)
    m_cpuContext.r[5] = _mm_set1_epi32(0);           // A1 = 0 (argv)
    m_cpuContext.r[29] = _mm_set1_epi32(0x02000000); // SP = top of RAM

    std::cout << "Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec << std::endl;

    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLACK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    g_activeThreads.store(1, std::memory_order_relaxed);

    std::thread gameThread([&, entryPoint]()
    {
        try
        {
            entryPoint(m_memory.getRDRAM(), &m_cpuContext, this);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        g_activeThreads.fetch_sub(1, std::memory_order_relaxed);
    });

    while (g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        UploadFrame(frameTex, this);

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexture(frameTex, 0, 0, WHITE);
        EndDrawing();

        if (WindowShouldClose())
        {
            break;
        }
    }

    if (gameThread.joinable())
    {
        gameThread.join();
    }
    
    UnloadTexture(frameTex);
    CloseWindow();
}
