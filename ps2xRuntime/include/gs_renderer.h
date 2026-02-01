#ifndef GS_RENDERER_H
#define GS_RENDERER_H

#include <cstdint>
#include <vector>
#include <memory>

// Forward declarations
struct R5900Context;
class PS2Memory;

/**
 * @brief Graphics Synthesizer (GS) Renderer
 * 
 * Handles conversion of PS2 VRAM framebuffer data to host-displayable format.
 * Currently supports PSMCT32 (32-bit color) format.
 */
class GSRenderer
{
public:
    enum class PixelFormat
    {
        PSMCT32,  // 32-bit color (RGBA)
        PSMCT16,  // 16-bit color (RGB5A1)
        PSMCT16S, // 16-bit color with special encoding
        PSMT8,    // 8-bit indexed color
        PSMT4,    // 4-bit indexed color
    };

    struct FramebufferConfig
    {
        uint32_t basePointer;  // FBP (Framebuffer Pointer) in 2048-byte units
        uint32_t width;        // FBW (Framebuffer Width) in 64-pixel blocks
        uint32_t height;       // Display height in pixels
        PixelFormat format;
    };

    GSRenderer();
    ~GSRenderer();

    /**
     * @brief Initialize the renderer with display dimensions
     */
    bool initialize(uint32_t displayWidth, uint32_t displayHeight);

    /**
     * @brief Update the framebuffer from VRAM data
     */
    void updateFramebuffer(const PS2Memory& memory, const FramebufferConfig& config);

    /**
     * @brief Get the current framebuffer as RGBA32 data
     */
    const std::vector<uint32_t>& getFramebufferRGBA() const { return m_framebufferRGBA; }

    /**
     * @brief Get framebuffer dimensions
     */
    uint32_t getWidth() const { return m_displayWidth; }
    uint32_t getHeight() const { return m_displayHeight; }

    /**
     * @brief Check if framebuffer has been updated since last call
     */
    bool isFramebufferDirty() const { return m_framebufferDirty; }
    void clearFramebufferDirty() { m_framebufferDirty = false; }

private:
    uint32_t m_displayWidth = 0;
    uint32_t m_displayHeight = 0;
    std::vector<uint32_t> m_framebufferRGBA;
    bool m_framebufferDirty = false;

    /**
     * @brief Convert PSMCT32 pixel format to RGBA32
     */
    uint32_t convertPSMCT32ToRGBA(uint32_t psColor) const;

    /**
     * @brief Convert PSMCT16 pixel format to RGBA32
     */
    uint32_t convertPSMCT16ToRGBA(uint16_t psColor) const;

    /**
     * @brief Calculate VRAM offset from framebuffer coordinates
     */
    uint32_t calculateVRAMOffset(uint32_t x, uint32_t y, uint32_t fbw, PixelFormat format) const;
};

#endif // GS_RENDERER_H
