#include "gs_renderer.h"
#include "ps2_runtime.h"
#include <cstring>
#include <iostream>
#include <algorithm>

GSRenderer::GSRenderer()
    : m_displayWidth(0), m_displayHeight(0), m_framebufferDirty(false)
{
}

GSRenderer::~GSRenderer() = default;

bool GSRenderer::initialize(uint32_t displayWidth, uint32_t displayHeight)
{
    m_displayWidth = displayWidth;
    m_displayHeight = displayHeight;

    // Allocate framebuffer for RGBA32 data
    size_t pixelCount = displayWidth * displayHeight;
    m_framebufferRGBA.resize(pixelCount, 0xFF000000); // Initialize to opaque black

    std::cout << "[GS Renderer] Initialized " << displayWidth << "x" << displayHeight 
              << " framebuffer (" << (pixelCount * 4 / 1024) << " KB)" << std::endl;

    return true;
}

uint32_t GSRenderer::convertPSMCT32ToRGBA(uint32_t psColor) const
{
    // PS2 PSMCT32 format: ABGR (8 bits each)
    // PC RGBA format: RGBA (8 bits each)
    
    uint8_t a = (psColor >> 24) & 0xFF;
    uint8_t b = (psColor >> 16) & 0xFF;
    uint8_t g = (psColor >> 8) & 0xFF;
    uint8_t r = psColor & 0xFF;

    // Convert to RGBA format
    return (r << 24) | (g << 16) | (b << 8) | a;
}

uint32_t GSRenderer::convertPSMCT16ToRGBA(uint16_t psColor) const
{
    // PS2 PSMCT16 format: ABGR5551 (5 bits RGB, 1 bit Alpha)
    // Extract components
    uint8_t r = ((psColor & 0x001F) << 3) | ((psColor & 0x001F) >> 2);  // 5-bit to 8-bit
    uint8_t g = (((psColor >> 5) & 0x001F) << 3) | (((psColor >> 5) & 0x001F) >> 2);
    uint8_t b = (((psColor >> 10) & 0x001F) << 3) | (((psColor >> 10) & 0x001F) >> 2);
    uint8_t a = (psColor & 0x8000) ? 0xFF : 0x00;  // 1-bit alpha

    return (r << 24) | (g << 16) | (b << 8) | a;
}

uint32_t GSRenderer::calculateVRAMOffset(uint32_t x, uint32_t y, uint32_t fbw, PixelFormat format) const
{
    // Calculate offset in VRAM based on framebuffer format
    // FBW is in 64-pixel blocks, so multiply by 64 to get pixel width
    uint32_t pixelWidth = fbw * 64;

    switch (format)
    {
    case PixelFormat::PSMCT32:
        // 32-bit color: 4 bytes per pixel
        return (y * pixelWidth + x) * 4;
    case PixelFormat::PSMCT16:
    case PixelFormat::PSMCT16S:
        // 16-bit color: 2 bytes per pixel
        return (y * pixelWidth + x) * 2;
    case PixelFormat::PSMT8:
        // 8-bit indexed: 1 byte per pixel
        return y * pixelWidth + x;
    case PixelFormat::PSMT4:
        // 4-bit indexed: 0.5 bytes per pixel
        return (y * pixelWidth + x) / 2;
    default:
        return 0;
    }
}

void GSRenderer::updateFramebuffer(const PS2Memory& memory, const FramebufferConfig& config)
{
    if (m_framebufferRGBA.empty())
    {
        return;
    }

    const uint8_t* vram = memory.getGSVRAM();
    if (!vram)
    {
        return;
    }

    uint32_t displayWidth = std::min(m_displayWidth, config.width * 64);
    uint32_t displayHeight = std::min(m_displayHeight, config.height);
    uint32_t fbpOffset = config.basePointer * 2048; // FBP is in 2048-byte units

    std::cout << "[GS Renderer] Updating framebuffer: FBP=0x" << std::hex << config.basePointer
              << " FBW=" << std::dec << config.width << " (" << displayWidth << "x" << displayHeight << ")"
              << std::endl;

    // Copy framebuffer data from VRAM to host memory
    for (uint32_t y = 0; y < displayHeight; ++y)
    {
        for (uint32_t x = 0; x < displayWidth; ++x)
        {
            uint32_t offset = fbpOffset + calculateVRAMOffset(x, y, config.width, config.format);

            // Bounds check
            if (offset + 4 > PS2_GS_VRAM_SIZE)
            {
                continue;
            }

            uint32_t pixelIndex = y * m_displayWidth + x;
            if (pixelIndex >= m_framebufferRGBA.size())
            {
                continue;
            }

            // Read pixel from VRAM and convert to RGBA
            switch (config.format)
            {
            case PixelFormat::PSMCT32:
            {
                uint32_t psColor = *reinterpret_cast<const uint32_t*>(&vram[offset]);
                m_framebufferRGBA[pixelIndex] = convertPSMCT32ToRGBA(psColor);
                break;
            }
            case PixelFormat::PSMCT16:
            case PixelFormat::PSMCT16S:
            {
                uint16_t psColor = *reinterpret_cast<const uint16_t*>(&vram[offset]);
                m_framebufferRGBA[pixelIndex] = convertPSMCT16ToRGBA(psColor);
                break;
            }
            default:
                // Unsupported format, fill with magenta (error color)
                m_framebufferRGBA[pixelIndex] = 0xFF00FFFF;
                break;
            }
        }
    }

    m_framebufferDirty = true;
}
