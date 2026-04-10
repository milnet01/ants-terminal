#include "glrenderer.h"
#include "terminalgrid.h"

#include <QDebug>
#include <QPainter>
#include <QFontMetrics>
#include <QImage>
#include <QTextLayout>

// Vertex shader for colored quads (backgrounds)
// CPU expands quads to 6 vertices each, so shader just passes through
static const char *bgVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;

uniform mat4 projection;

out vec4 vColor;

void main() {
    gl_Position = projection * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char *bgFragmentShader = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)";

// Vertex shader for textured quads (glyphs)
// CPU expands quads to 6 vertices each with pre-computed UVs
static const char *glyphVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

uniform mat4 projection;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    gl_Position = projection * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

static const char *glyphFragmentShader = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D glyphAtlas;

void main() {
    float alpha = texture(glyphAtlas, vTexCoord).a;
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
)";

GlRenderer::GlRenderer()
    : m_quadVBO(QOpenGLBuffer::VertexBuffer) {}

GlRenderer::~GlRenderer() {
    // GL resources should already be cleaned up via cleanup()
    // Do NOT call glDeleteTextures here — context may not be current
}

void GlRenderer::cleanup() {
    if (m_atlasTexture) {
        glDeleteTextures(1, &m_atlasTexture);
        m_atlasTexture = 0;
    }
    m_vao.destroy();
    m_quadVBO.destroy();
    m_glyphCache.clear();
    m_initialized = false;
}

bool GlRenderer::initialize() {
    initializeOpenGLFunctions();

    // Compile background shader
    if (!m_bgShader.addShaderFromSourceCode(QOpenGLShader::Vertex, bgVertexShader) ||
        !m_bgShader.addShaderFromSourceCode(QOpenGLShader::Fragment, bgFragmentShader) ||
        !m_bgShader.link()) {
        qWarning("GlRenderer: background shader failed: %s", qPrintable(m_bgShader.log()));
        return false;
    }

    // Compile glyph shader
    if (!m_glyphShader.addShaderFromSourceCode(QOpenGLShader::Vertex, glyphVertexShader) ||
        !m_glyphShader.addShaderFromSourceCode(QOpenGLShader::Fragment, glyphFragmentShader) ||
        !m_glyphShader.link()) {
        qWarning("GlRenderer: glyph shader failed: %s", qPrintable(m_glyphShader.log()));
        return false;
    }

    // Create glyph atlas texture (single-channel for alpha)
    glGenTextures(1, &m_atlasTexture);
    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_atlasWidth, m_atlasHeight, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Swizzle mask: map red channel to alpha for driver-agnostic single-channel glyphs
    // Prevents tinted glyphs on some Intel/AMD drivers that treat GL_RED as literal red
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);

    // Create VAO and VBO
    m_vao.create();
    m_quadVBO.create();

    m_initialized = true;
    return true;
}

void GlRenderer::setFont(const QFont &regular, const QFont &bold,
                          const QFont &italic, const QFont &boldItalic) {
    m_fontRegular = regular;
    m_fontBold = bold;
    m_fontItalic = italic;
    m_fontBoldItalic = boldItalic;
    // Clear glyph cache when font changes
    m_glyphCache.clear();
    m_ligatureCache.clear();
    m_atlasX = 0;
    m_atlasY = 0;
    m_atlasRowHeight = 0;
    // Clear atlas texture
    if (m_atlasTexture) {
        glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
        std::vector<uint8_t> zeros(m_atlasWidth * m_atlasHeight, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_atlasWidth, m_atlasHeight,
                        GL_RED, GL_UNSIGNED_BYTE, zeros.data());
    }
}

void GlRenderer::setCellSize(int width, int height, int ascent) {
    m_cellWidth = width;
    m_cellHeight = height;
    m_fontAscent = ascent;
}

void GlRenderer::setViewportSize(int width, int height) {
    m_viewWidth = width;
    m_viewHeight = height;
}

void GlRenderer::render(const TerminalGrid *grid, int scrollOffset, int padding,
                         const QColor &defaultBg, const QColor &cursorColor,
                         int cursorRow, int cursorCol, bool cursorVisible, bool cursorBlink) {
    if (!m_initialized || !grid) return;

    glViewport(0, 0, m_viewWidth, m_viewHeight);

    // Clear with default background
    glClearColor(defaultBg.redF(), defaultBg.greenF(), defaultBg.blueF(), defaultBg.alphaF());
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Build orthographic projection matrix (Y-down)
    float proj[16] = {};
    proj[0] = 2.0f / m_viewWidth;
    proj[5] = -2.0f / m_viewHeight;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = 1.0f;
    proj[15] = 1.0f;
    m_projection = QMatrix4x4(proj);

    int rows = grid->rows();
    int cols = grid->cols();
    // Note: GL renderer currently only renders the active screen, not scrollback.
    // Scrollback rendering uses the QPainter path in TerminalWidget.

    std::vector<CellQuad> bgQuads;
    std::vector<GlyphQuad> glyphQuads;
    bgQuads.reserve(rows * cols / 4); // Estimate: 25% non-default bg
    glyphQuads.reserve(rows * cols / 2); // Estimate: 50% non-space

    for (int vr = 0; vr < rows; ++vr) {
        // Accumulate text runs for ligature shaping
        QString runText;
        int runStartCol = 0;
        bool runBold = false, runItalic = false;
        QColor runFg;

        auto flushRun = [&]() {
            if (runText.isEmpty()) return;
            float px_x = padding + runStartCol * m_cellWidth;
            float px_y = padding + vr * m_cellHeight;

            if (runText.length() >= 2) {
                // Use ligature shaping for multi-char runs
                const LigatureEntry &lig = getLigature(runText, runBold, runItalic);
                if (lig.valid) {
                    float gx = px_x + lig.bearingX;
                    float gy = px_y + m_fontAscent - lig.bearingY;
                    float uvW = lig.u1 - lig.u0;
                    float uvH = lig.v1 - lig.v0;
                    glyphQuads.push_back({gx, gy, (float)lig.width, (float)lig.height,
                                          lig.u0, lig.v0, uvW, uvH,
                                          (float)runFg.redF(), (float)runFg.greenF(),
                                          (float)runFg.blueF(), 1.0f});
                }
            } else {
                // Single char — use individual glyph cache
                uint32_t cp = runText.at(0).unicode();
                const GlyphEntry &glyph = getGlyph(cp, runBold, runItalic);
                if (glyph.valid) {
                    float gx = px_x + glyph.bearingX;
                    float gy = px_y + m_fontAscent - glyph.bearingY;
                    float uvW = glyph.u1 - glyph.u0;
                    float uvH = glyph.v1 - glyph.v0;
                    glyphQuads.push_back({gx, gy, (float)glyph.width, (float)glyph.height,
                                          glyph.u0, glyph.v0, uvW, uvH,
                                          (float)runFg.redF(), (float)runFg.greenF(),
                                          (float)runFg.blueF(), 1.0f});
                }
            }
            runText.clear();
        };

        for (int col = 0; col < cols; ++col) {
            const Cell &c = grid->cellAt(vr, col);
            if (c.isWideCont) continue;

            float px_x = padding + col * m_cellWidth;
            float px_y = padding + vr * m_cellHeight;

            QColor fg = c.attrs.fg;
            QColor bg = c.attrs.bg;
            if (c.attrs.inverse) std::swap(fg, bg);
            if (c.attrs.dim) fg = fg.darker(150);

            int cellW = c.isWideChar ? m_cellWidth * 2 : m_cellWidth;

            // Background (only if different from default)
            if (bg != defaultBg) {
                bgQuads.push_back({px_x, px_y, (float)cellW, (float)m_cellHeight,
                                   (float)bg.redF(), (float)bg.greenF(),
                                   (float)bg.blueF(), (float)bg.alphaF()});
            }

            // Accumulate text runs with same attributes
            if (c.codepoint != ' ' && c.codepoint != 0) {
                bool sameAttrs = !runText.isEmpty() &&
                    c.attrs.bold == runBold && c.attrs.italic == runItalic && fg == runFg;
                if (!sameAttrs) {
                    flushRun();
                    runStartCol = col;
                    runBold = c.attrs.bold;
                    runItalic = c.attrs.italic;
                    runFg = fg;
                }
                runText += QString::fromUcs4(reinterpret_cast<const char32_t *>(&c.codepoint), 1);
            } else {
                flushRun();
            }
        }
        flushRun(); // end of row
    }

    // Cursor
    if (cursorVisible && cursorBlink && scrollOffset == 0) {
        float cx = padding + cursorCol * m_cellWidth;
        float cy = padding + cursorRow * m_cellHeight;
        bgQuads.push_back({cx, cy, (float)m_cellWidth, (float)m_cellHeight,
                           (float)cursorColor.redF(), (float)cursorColor.greenF(),
                           (float)cursorColor.blueF(), (float)cursorColor.alphaF()});
    }

    // Render backgrounds
    renderBackgrounds(bgQuads);

    // Render glyphs
    renderGlyphs(glyphQuads);

    glDisable(GL_BLEND);
}

void GlRenderer::renderBackgrounds(const std::vector<CellQuad> &quads) {
    if (quads.empty()) return;

    m_bgShader.bind();
    m_bgShader.setUniformValue("projection", m_projection);

    // Expand quads to 6 vertices each (two triangles)
    struct BgVertex { float x, y, r, g, b, a; };
    std::vector<BgVertex> verts;
    verts.reserve(quads.size() * 6);

    for (const auto &q : quads) {
        float x0 = q.x, y0 = q.y, x1 = q.x + q.w, y1 = q.y + q.h;
        BgVertex v = {0, 0, q.r, q.g, q.b, q.a};
        v.x = x0; v.y = y0; verts.push_back(v);
        v.x = x1; v.y = y0; verts.push_back(v);
        v.x = x1; v.y = y1; verts.push_back(v);
        v.x = x0; v.y = y0; verts.push_back(v);
        v.x = x1; v.y = y1; verts.push_back(v);
        v.x = x0; v.y = y1; verts.push_back(v);
    }

    m_vao.bind();
    m_quadVBO.bind();
    m_quadVBO.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(BgVertex)));

    // location 0: vec2 aPos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(BgVertex), nullptr);
    // location 1: vec4 aColor
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(BgVertex),
                          reinterpret_cast<void *>(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(verts.size()));

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    m_quadVBO.release();
    m_vao.release();
    m_bgShader.release();
}

void GlRenderer::renderGlyphs(const std::vector<GlyphQuad> &quads) {
    if (quads.empty()) return;

    m_glyphShader.bind();
    m_glyphShader.setUniformValue("projection", m_projection);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    m_glyphShader.setUniformValue("glyphAtlas", 0);

    // Expand quads to 6 vertices each with pre-computed UVs
    struct GlyphVertex { float x, y, u, v, r, g, b, a; };
    std::vector<GlyphVertex> verts;
    verts.reserve(quads.size() * 6);

    for (const auto &q : quads) {
        float x0 = q.x, y0 = q.y, x1 = q.x + q.w, y1 = q.y + q.h;
        float u0 = q.u0, v0 = q.v0, u1 = q.u0 + q.u1, v1 = q.v0 + q.v1;
        GlyphVertex gv = {0, 0, 0, 0, q.r, q.g, q.b, q.a};
        gv.x = x0; gv.y = y0; gv.u = u0; gv.v = v0; verts.push_back(gv);
        gv.x = x1; gv.y = y0; gv.u = u1; gv.v = v0; verts.push_back(gv);
        gv.x = x1; gv.y = y1; gv.u = u1; gv.v = v1; verts.push_back(gv);
        gv.x = x0; gv.y = y0; gv.u = u0; gv.v = v0; verts.push_back(gv);
        gv.x = x1; gv.y = y1; gv.u = u1; gv.v = v1; verts.push_back(gv);
        gv.x = x0; gv.y = y1; gv.u = u0; gv.v = v1; verts.push_back(gv);
    }

    m_vao.bind();
    m_quadVBO.bind();
    m_quadVBO.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(GlyphVertex)));

    // location 0: vec2 aPos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex), nullptr);
    // location 1: vec2 aTexCoord
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    // location 2: vec4 aColor
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex),
                          reinterpret_cast<void *>(4 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(verts.size()));

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    m_quadVBO.release();
    m_vao.release();
    m_glyphShader.release();
}

const GlyphEntry &GlRenderer::getGlyph(uint32_t codepoint, bool bold, bool italic) {
    GlyphKey key{codepoint, bold, italic};
    auto it = m_glyphCache.find(key);
    if (it != m_glyphCache.end())
        return it.value();

    // Rasterize the glyph
    GlyphEntry entry{};
    const QFont *font = &m_fontRegular;
    if (bold && italic) font = &m_fontBoldItalic;
    else if (bold) font = &m_fontBold;
    else if (italic) font = &m_fontItalic;

    rasterizeGlyph(codepoint, *font, entry);
    m_glyphCache.insert(key, entry);
    return m_glyphCache[key];
}

void GlRenderer::rasterizeGlyph(uint32_t codepoint, const QFont &font, GlyphEntry &entry) {
    QString ch = QString::fromUcs4(reinterpret_cast<const char32_t *>(&codepoint), 1);
    QFontMetrics fm(font);

    int advance = fm.horizontalAdvance(ch);
    if (advance <= 0) {
        entry.valid = false;
        return;
    }

    // Render glyph to a QImage (bound to prevent pathological allocations)
    int imgW = std::clamp(std::max(advance, m_cellWidth * 2), 1, 512);
    int imgH = std::clamp(m_cellHeight, 1, 512);
    QImage img(imgW, imgH, QImage::Format_Grayscale8);
    img.fill(0);

    QPainter painter(&img);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(0, fm.ascent(), ch);
    painter.end();

    // Find bounding box of the rendered glyph
    int minX = imgW, maxX = 0, minY = imgH, maxY = 0;
    for (int y = 0; y < imgH; ++y) {
        const uint8_t *line = img.constScanLine(y);
        for (int x = 0; x < imgW; ++x) {
            if (line[x] > 0) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }

    if (minX > maxX) {
        // Empty glyph (space-like)
        entry.valid = false;
        return;
    }

    int glyphW = maxX - minX + 1;
    int glyphH = maxY - minY + 1;

    // Extract just the glyph region
    QImage cropped = img.copy(minX, minY, glyphW, glyphH);

    entry.width = glyphW;
    entry.height = glyphH;
    entry.bearingX = minX;
    entry.bearingY = fm.ascent() - minY;
    entry.advance = advance;

    uploadToAtlas(cropped, entry);
}

void GlRenderer::uploadToAtlas(const QImage &img, GlyphEntry &entry) {
    int w = img.width();
    int h = img.height();

    // Check if we need to move to next row
    if (m_atlasX + w > m_atlasWidth) {
        m_atlasX = 0;
        m_atlasY += m_atlasRowHeight + 1;
        m_atlasRowHeight = 0;
    }

    // Check if atlas is full — double size if possible, else clear and restart
    if (m_atlasY + h > m_atlasHeight) {
        if (m_atlasWidth < 4096) {
            // Double the atlas size to reduce stall frequency
            m_atlasWidth = 4096;
            m_atlasHeight = 4096;
            glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_atlasWidth, m_atlasHeight, 0,
                         GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
        m_glyphCache.clear();
        m_atlasX = 0;
        m_atlasY = 0;
        m_atlasRowHeight = 0;
        std::vector<uint8_t> zeros(m_atlasWidth * m_atlasHeight, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_atlasWidth, m_atlasHeight,
                        GL_RED, GL_UNSIGNED_BYTE, zeros.data());
    }

    // Upload glyph to atlas
    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, m_atlasX, m_atlasY, w, h,
                    GL_RED, GL_UNSIGNED_BYTE, img.constBits());

    entry.u0 = static_cast<float>(m_atlasX) / m_atlasWidth;
    entry.v0 = static_cast<float>(m_atlasY) / m_atlasHeight;
    entry.u1 = static_cast<float>(m_atlasX + w) / m_atlasWidth;
    entry.v1 = static_cast<float>(m_atlasY + h) / m_atlasHeight;
    entry.valid = true;

    m_atlasX += w + 1;
    m_atlasRowHeight = std::max(m_atlasRowHeight, h);
}

// --- Ligature support ---

const LigatureEntry &GlRenderer::getLigature(const QString &text, bool bold, bool italic) {
    LigatureKey key{text, bold, italic};
    auto it = m_ligatureCache.find(key);
    if (it != m_ligatureCache.end())
        return it.value();

    LigatureEntry entry{};
    const QFont *font = &m_fontRegular;
    if (bold && italic) font = &m_fontBoldItalic;
    else if (bold) font = &m_fontBold;
    else if (italic) font = &m_fontItalic;

    rasterizeLigature(text, *font, entry);
    m_ligatureCache.insert(key, entry);
    return m_ligatureCache[key];
}

void GlRenderer::rasterizeLigature(const QString &text, const QFont &font, LigatureEntry &entry) {
    if (text.isEmpty()) { entry.valid = false; return; }

    // Use QTextLayout for HarfBuzz-powered ligature shaping
    QTextLayout layout(text, font);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) { entry.valid = false; return; }
    line.setLineWidth(text.length() * m_cellWidth * 3); // generous
    line.setPosition(QPointF(0, 0));
    layout.endLayout();

    QRectF br = layout.boundingRect();
    int imgW = std::clamp(static_cast<int>(std::ceil(br.width())) + 2, 1, 1024);
    int imgH = std::clamp(m_cellHeight, 1, 256);

    QImage img(imgW, imgH, QImage::Format_Grayscale8);
    img.fill(0);

    QPainter painter(&img);
    painter.setFont(font);
    painter.setPen(Qt::white);
    layout.draw(&painter, QPointF(0, 0));
    painter.end();

    // Find bounding box
    int minX = imgW, maxX = 0, minY = imgH, maxY = 0;
    for (int y = 0; y < imgH; ++y) {
        const uint8_t *scanline = img.constScanLine(y);
        for (int x = 0; x < imgW; ++x) {
            if (scanline[x] > 0) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }

    if (minX > maxX) { entry.valid = false; return; }

    QImage cropped = img.copy(minX, minY, maxX - minX + 1, maxY - minY + 1);

    QFontMetrics fm(font);
    entry.width = cropped.width();
    entry.height = cropped.height();
    entry.bearingX = minX;
    entry.bearingY = fm.ascent() - minY;

    uploadLigatureToAtlas(cropped, entry);
}

void GlRenderer::uploadLigatureToAtlas(const QImage &img, LigatureEntry &entry) {
    int w = img.width();
    int h = img.height();

    if (m_atlasX + w > m_atlasWidth) {
        m_atlasX = 0;
        m_atlasY += m_atlasRowHeight + 1;
        m_atlasRowHeight = 0;
    }

    if (m_atlasY + h > m_atlasHeight) {
        if (m_atlasWidth < 4096) {
            m_atlasWidth = 4096;
            m_atlasHeight = 4096;
            glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_atlasWidth, m_atlasHeight, 0,
                         GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
        m_glyphCache.clear();
        m_ligatureCache.clear();
        m_atlasX = 0;
        m_atlasY = 0;
        m_atlasRowHeight = 0;
        std::vector<uint8_t> zeros(m_atlasWidth * m_atlasHeight, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_atlasWidth, m_atlasHeight,
                        GL_RED, GL_UNSIGNED_BYTE, zeros.data());
    }

    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, m_atlasX, m_atlasY, w, h,
                    GL_RED, GL_UNSIGNED_BYTE, img.constBits());

    entry.u0 = static_cast<float>(m_atlasX) / m_atlasWidth;
    entry.v0 = static_cast<float>(m_atlasY) / m_atlasHeight;
    entry.u1 = static_cast<float>(m_atlasX + w) / m_atlasWidth;
    entry.v1 = static_cast<float>(m_atlasY + h) / m_atlasHeight;
    entry.valid = true;

    m_atlasX += w + 1;
    m_atlasRowHeight = std::max(m_atlasRowHeight, h);
}
