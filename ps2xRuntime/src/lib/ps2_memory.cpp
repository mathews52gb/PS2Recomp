#include "ps2_runtime.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace
{
    inline bool isGsPrivReg(uint32_t addr)
    {
        return addr >= PS2_GS_PRIV_REG_BASE && addr < PS2_GS_PRIV_REG_BASE + PS2_GS_PRIV_REG_SIZE;
    }

    inline void logGsWrite(uint32_t addr, uint64_t value)
    {
        static std::unordered_map<uint32_t, int> logCount;
        int &count = logCount[addr];
        if (count < 10)
        {
            std::cout << "[GS] write 0x" << std::hex << addr << " = 0x" << value << std::dec << std::endl;
        }
        ++count;
    }

    constexpr uint32_t kSchedulerBase = 0x00363a10;
    constexpr uint32_t kSchedulerSpan = 0x00000420;
    static int g_schedWriteLogCount = 0;

    inline void logSchedulerWrite(uint32_t physAddr, uint32_t size, uint64_t value)
    {
        if (physAddr < kSchedulerBase || physAddr >= kSchedulerBase + kSchedulerSpan)
        {
            return;
        }
        if (g_schedWriteLogCount >= 64)
        {
            return;
        }
        std::cout << "[sched write" << size << "] addr=0x" << std::hex << physAddr
                  << " val=0x" << value << std::dec << std::endl;
        ++g_schedWriteLogCount;
    }
}

uint64_t *PS2Memory::gsRegPtr(GSRegisters &gs, uint32_t addr)
{
    uint32_t off = addr - PS2_GS_PRIV_REG_BASE;
    switch (off)
    {
    case 0x0000:
        return &gs.pmode;
    case 0x0010:
        return &gs.smode1;
    case 0x0020:
        return &gs.smode2;
    case 0x0030:
        return &gs.srfsh;
    case 0x0040:
        return &gs.synch1;
    case 0x0050:
        return &gs.synch2;
    case 0x0060:
        return &gs.syncv;
    case 0x0070:
        return &gs.dispfb1;
    case 0x0080:
        return &gs.display1;
    case 0x0090:
        return &gs.dispfb2;
    case 0x00A0:
        return &gs.display2;
    case 0x00B0:
        return &gs.extbuf;
    case 0x00C0:
        return &gs.extdata;
    case 0x00D0:
        return &gs.extwrite;
    case 0x00E0:
        return &gs.bgcolor;
    case 0x1000:
        return &gs.csr;
    case 0x1010:
        return &gs.imr;
    case 0x1040:
        return &gs.busdir;
    case 0x1080:
        return &gs.siglblid;
    default:
        return nullptr;
    }
}

// Helpers for GS VRAM addressing (PSMCT32 only in this minimal path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), m_gsvram(nullptr), m_seenGifCopy(false)
{
}

PS2Memory::~PS2Memory()
{
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsvram)
    {
        delete[] m_gsvram;
        m_gsvram = nullptr;
    }
}

bool PS2Memory::initialize() { return initialize(PS2_RAM_SIZE); }

bool PS2Memory::initialize(size_t ramSize)
{
    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        if (!m_rdram)
            return false;
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad (16KB)
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        if (!m_scratchpad)
        {
            delete[] m_rdram;
            m_rdram = nullptr;
            return false;
        }
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&m_gs, 0, sizeof(m_gs));

        // Allocate GS VRAM (4MB)
        m_gsvram = new uint8_t[PS2_GS_VRAM_SIZE];
        if (!m_gsvram)
        {
            delete[] m_rdram;
            delete[] m_scratchpad;
            m_rdram = nullptr;
            m_scratchpad = nullptr;
            return false;
        }
        std::memset(m_gsvram, 0, PS2_GS_VRAM_SIZE);

        // Initialize VIF registers
        memset(vif0_regs, 0, sizeof(vif0_regs));
        memset(vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        m_tlbEntries.clear();

        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return address >= PS2_SCRATCHPAD_BASE &&
           address < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE;
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress) const
{
    if (isScratchpad(virtualAddress))
    {
        return virtualAddress - PS2_SCRATCHPAD_BASE;
    }

    // Direct mapping for KSEG0/KSEG1
    if ((virtualAddress & 0xE0000000) == 0x80000000 || (virtualAddress & 0xE0000000) == 0xA0000000)
    {
        return virtualAddress & 0x1FFFFFFF;
    }

    // User segment mapping (simple identity for now)
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress & 0x1FFFFFFF;
    }

    // TLB translation
    for (const auto &entry : m_tlbEntries)
    {
        if (entry.valid)
        {
            uint32_t vpn_masked = (virtualAddress >> 12) & ~entry.mask;
            uint32_t entry_vpn_masked = entry.vpn & ~entry.mask;

            if (vpn_masked == entry_vpn_masked)
            {
                // Match found
                uint32_t page = entry.pfn | (virtualAddress & entry.mask);
                return (page << 12) | (virtualAddress & 0xFFF);
            }
        }
    }

    // Default to physical address for everything else
    return virtualAddress & 0x1FFFFFFF;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    uint32_t physAddr = translateAddress(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    if (isScratchpad(address))
    {
        return m_scratchpad[address - PS2_SCRATCHPAD_BASE];
    }
    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(m_gs, address);
        uint32_t off = address & 7;
        uint64_t val = reg ? *reg : 0;
        return (uint8_t)(val >> (off * 8));
    }
    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    uint32_t physAddr = translateAddress(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        return *reinterpret_cast<uint16_t *>(&m_rdram[physAddr]);
    }
    if (isScratchpad(address))
    {
        return *reinterpret_cast<uint16_t *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]);
    }
    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(m_gs, address);
        uint32_t off = address & 7;
        uint64_t val = reg ? *reg : 0;
        return (uint16_t)(val >> (off * 8));
    }
    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    uint32_t physAddr = translateAddress(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        return *reinterpret_cast<uint32_t *>(&m_rdram[physAddr]);
    }
    if (isScratchpad(address))
    {
        return *reinterpret_cast<uint32_t *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]);
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        return readIORegister(address);
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(m_gs, address);
        uint32_t off = address & 7;
        uint64_t val = reg ? *reg : 0;
        return (uint32_t)(val >> (off * 8));
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    uint32_t physAddr = translateAddress(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        return *reinterpret_cast<uint64_t *>(&m_rdram[physAddr]);
    }
    if (isScratchpad(address))
    {
        return *reinterpret_cast<uint64_t *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]);
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(m_gs, address);
        return reg ? *reg : 0;
    }

    return 0;
}

__m128i PS2Memory::read128(uint32_t address)
{
    uint32_t physAddr = translateAddress(address);
    const bool scratch = isScratchpad(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(&m_rdram[physAddr]));
    }
    if (scratch)
    {
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]));
    }
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    uint32_t physAddr = translateAddress(address);
    const bool scratch = isScratchpad(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
        markModified(address, 1);
        logSchedulerWrite(physAddr, 8, value);
    }
    else if (scratch)
    {
        m_scratchpad[address - PS2_SCRATCHPAD_BASE] = value;
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    uint32_t physAddr = translateAddress(address);
    const bool scratch = isScratchpad(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        *reinterpret_cast<uint16_t *>(&m_rdram[physAddr]) = value;
        markModified(address, 2);
        logSchedulerWrite(physAddr, 16, value);
    }
    else if (scratch)
    {
        *reinterpret_cast<uint16_t *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]) = value;
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    uint32_t physAddr = translateAddress(address);

    if (address >= 0x10000000 && address < 0x10010000)
    {
        writeIORegister(address, value);
        return;
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(m_gs, address);
        if (reg)
        {
            uint32_t off = address & 7;
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            *reg = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            logGsWrite(address, *reg);
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        *reinterpret_cast<uint32_t *>(&m_rdram[physAddr]) = value;
        markModified(address, 4);
        logSchedulerWrite(physAddr, 32, value);
    }
    else if (scratch)
    {
        *reinterpret_cast<uint32_t *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]) = value;
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    uint32_t physAddr = translateAddress(address);

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(m_gs, address);
        if (reg)
        {
            *reg = value;
            logGsWrite(address, value);
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        *reinterpret_cast<uint64_t *>(&m_rdram[physAddr]) = value;
        markModified(address, 8);
        logSchedulerWrite(physAddr, 64, value);
    }
    else if (scratch)
    {
        *reinterpret_cast<uint64_t *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]) = value;
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    uint32_t physAddr = translateAddress(address);
    const bool scratch = isScratchpad(address);
    if (physAddr < PS2_RAM_SIZE)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
        markModified(address, 16);
    }
    else if (scratch)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[address - PS2_SCRATCHPAD_BASE]), value);
    }
    else if (physAddr < PS2_GS_VRAM_SIZE)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_gsvram[physAddr]), value);
    }
    else
    {
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);
        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    m_ioRegisters[address] = value;

    if (address >= 0x10000000 && address < 0x10010000)
    {
        // DMA channel control registers
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            { // CHCR register
                if (value & 0x100)
                { // STR bit set
                    uint32_t channelBase = address & ~0xFF;
                    uint32_t madr = m_ioRegisters[channelBase + 0x10];
                    uint32_t qwc = m_ioRegisters[channelBase + 0x20] & 0xFFFF;

                    std::cout << "DMA Start - Channel: " << std::hex << ((channelBase >> 8) & 0xF)
                              << ", MADR: " << madr
                              << ", QWC: " << qwc << std::dec << std::endl;

                    // Minimal GIF (channel 2) and VIF1 (channel 1) image transfer: copy from EE memory to GS VRAM.
                    // Only handles simple linear IMAGE transfers; treats destination as current DISPFBUF1 FBP.
                    if ((channelBase == 0x1000A000 || channelBase == 0x10009000) && m_gsvram)
                    {
                        auto doCopy = [&](uint32_t srcAddr, uint32_t qwCount)
                        {
                            uint32_t bytes = qwCount * 16;
                            uint32_t src = translateAddress(srcAddr);
                            uint32_t basePage = static_cast<uint32_t>(m_gs.dispfb1 & 0x1FF);
                            uint32_t dest = basePage * 2048;
                            std::cout << "[GIF] ch=" << ((channelBase == 0x1000A000) ? 2 : 1)
                                      << " IMAGE copy bytes=" << bytes
                                      << " src=0x" << std::hex << srcAddr
                                      << " phys=0x" << src
                                      << " fbp=0x" << basePage
                                      << " dest=0x" << dest << std::dec << std::endl;

                            if (src + bytes > PS2_RAM_SIZE)
                            {
                                bytes = std::min<uint32_t>(bytes, PS2_RAM_SIZE - src);
                            }
                            std::memcpy(m_gsvram + dest, m_rdram + src, bytes);
                            m_seenGifCopy = true;
                            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                        };

                        if (qwc > 0)
                        {
                            doCopy(madr, qwc);
                        }
                        else
                        {
                            // Simple DMA chain walker for one tag from TADR (REF/NEXT).
                            uint32_t tadr = m_ioRegisters[channelBase + 0x30];
                            uint32_t physTag = translateAddress(tadr);
                            if (physTag + 16 <= PS2_RAM_SIZE)
                            {
                                const uint8_t *tp = m_rdram + physTag;
                                uint64_t tag = *reinterpret_cast<const uint64_t *>(tp);
                                uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                                uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                                uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFF);
                                std::cout << "[DMA chain] ch=" << ((channelBase == 0x1000A000) ? 2 : 1)
                                          << " tag id=0x" << std::hex << id
                                          << " qwc=" << tagQwc
                                          << " addr=0x" << addr
                                          << " raw=0x" << tag << std::dec << std::endl;
                                if (id == 0 || id == 1 || id == 2)
                                {
                                    doCopy(addr, tagQwc);
                                }
                            }
                        }
                        m_ioRegisters[address] &= ~0x100;
                    }
                }
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            std::cout << "Interrupt register write: " << std::hex << address << " = " << value << std::dec << std::endl;
            return true;
        }
    }
    else if (address >= 0x12000000 && address < 0x12001000)
    {
        // GS registers
        std::cout << "GS register write: " << std::hex << address << " = " << value << std::dec << std::endl;
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    return false;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        // Timer registers
        if (address >= 0x10000000 && address < 0x10000100)
        {
            if ((address & 0xF) == 0x00)
            {                            // COUNT registers
                uint32_t timerCount = 0; // Should calculate based on elapsed time
                std::cout << "Timer COUNT read: " << std::hex << address << " = " << timerCount << std::dec << std::endl;
                return timerCount;
            }
        }

        // DMA status registers
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {                                                             // CHCR registers
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100; // Clear busy bit
                std::cout << "DMA status read: " << std::hex << address << " = " << channelStatus << std::dec << std::endl;
                return channelStatus;
            }
        }

        // Interrupt status registers
        if (address >= 0x10000200 && address < 0x10000300)
        {
            std::cout << "Interrupt status read: " << std::hex << address << std::dec << std::endl;
            // Should calculate based on pending interrupts
            return 0;
        }
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start) / 4;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    std::cout << "Registered code region: " << std::hex << start << " - " << end << std::dec << std::endl;
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    for (auto &region : m_codeRegions)
    {
        if (address + size <= region.start || address >= region.end)
        {
            continue;
        }

        uint32_t overlapStart = std::max(address, region.start);
        uint32_t overlapEnd = std::min(address + size, region.end);

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                std::cout << "Marked code at " << std::hex << addr << std::dec << " as modified" << std::endl;
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    for (const auto &region : m_codeRegions)
    {
        if (address + size <= region.start || address >= region.end)
        {
            continue;
        }

        uint32_t overlapStart = std::max(address, region.start);
        uint32_t overlapEnd = std::min(address + size, region.end);

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    for (auto &region : m_codeRegions)
    {
        if (address + size <= region.start || address >= region.end)
        {
            continue;
        }

        uint32_t overlapStart = std::max(address, region.start);
        uint32_t overlapEnd = std::min(address + size, region.end);

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
