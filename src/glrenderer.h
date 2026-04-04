#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLTexture>
#include <QFont>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QSize>
#include <vector>
#include <cstdint>

class TerminalGrid;

// Glyph atlas entry
struct GlyphEntry {
    float u0, v0, u1, v1; // UV coordinates in atlas
    int width, height;     // Glyph pixel dimensions
    int bearingX, bearingY;
    int advance;
    bool valid = false;
};

// Key for glyph cache lookup
struct GlyphKey {
    uint32_t codepoint;
    bool bold;
    bool italic;
    bool operator==(const GlyphKey &o) const {
        return codepoint == o.codepoint && bold == o.bold && italic == o.italic;
    }
};

inline size_t qHash(const GlyphKey &k, size_t seed = 0) {
    return qHash(k.codepoint, seed) ^ qHash(k.bold) ^ qHash(k.italic);
}

// Per-cell instance data for GPU rendering
struct CellQuad {
    float x, y, w, h;     // Position and size in pixels
    float r, g, b, a;     // Background color
};

struct GlyphQuad {
    float x, y, w, h;     // Position and size in pixels
    float u0, v0, u1, v1; // Texture coordinates
    float r, g, b, a;     // Foreground color
};

// OpenGL-based terminal renderer with glyph atlas
class GlRenderer : protected QOpenGLFunctions {
public:
    GlRenderer();
    ~GlRenderer();

    bool initialize();
    void setFont(const QFont &regular, const QFont &bold,
                 const QFont &italic, const QFont &boldItalic);
    void setCellSize(int width, int height, int ascent);
    void setViewportSize(int width, int height);

    // Render the terminal grid
    void render(const TerminalGrid *grid, int scrollOffset, int padding,
                const QColor &defaultBg, const QColor &cursorColor,
                int cursorRow, int cursorCol, bool cursorVisible, bool cursorBlink);

    bool isInitialized() const { return m_initialized; }

private:
    void renderBackgrounds(const std::vector<CellQuad> &quads);
    void renderGlyphs(const std::vector<GlyphQuad> &quads);
    const GlyphEntry &getGlyph(uint32_t codepoint, bool bold, bool italic);
    void rasterizeGlyph(uint32_t codepoint, const QFont &font, GlyphEntry &entry);
    void uploadToAtlas(const QImage &img, GlyphEntry &entry);

    bool m_initialized = false;

    // Shaders
    QOpenGLShaderProgram m_bgShader;
    QOpenGLShaderProgram m_glyphShader;

    // Glyph atlas
    GLuint m_atlasTexture = 0;
    int m_atlasWidth = 2048;
    int m_atlasHeight = 2048;
    int m_atlasX = 0;  // Current write position
    int m_atlasY = 0;
    int m_atlasRowHeight = 0;
    QHash<GlyphKey, GlyphEntry> m_glyphCache;

    // Fonts
    QFont m_fontRegular;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;

    // Cell metrics
    int m_cellWidth = 0;
    int m_cellHeight = 0;
    int m_fontAscent = 0;

    // Viewport
    int m_viewWidth = 0;
    int m_viewHeight = 0;

    // Vertex buffers
    QOpenGLBuffer m_quadVBO;
    QOpenGLVertexArrayObject m_vao;
};
