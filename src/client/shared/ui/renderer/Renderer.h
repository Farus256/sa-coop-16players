#pragma once
// Minimal immediate-mode 2D renderer on top of the game's IDirect3DDevice9.
// Text uses glyph atlases rendered with GDI, so any Windows font with full Cyrillic support works.
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace coop::ui {

// NORMAL/SMALL: Arial Bold (SA subtitles / SA-MP chat look); BIG/MENU/MENU_SMALL: wide capitals with tracking,
// close to the game's menu font.
enum FontId { FONT_NORMAL = 0, FONT_SMALL = 1, FONT_BIG = 2, FONT_MENU = 3, FONT_MENU_SMALL = 4, FONT_COUNT };

// The game's own fonts (CFont styles).
enum GameFont { GAME_FONT_GOTHIC = 0, GAME_FONT_SUBTITLES = 1, GAME_FONT_MENU = 2, GAME_FONT_PRICEDOWN = 3 };

enum TextFlags : uint32_t {
    TEXT_SHADOW = 1,
    TEXT_OUTLINE = 2,
    TEXT_CENTER = 4,   // x is the centre
    TEXT_RIGHT = 8,    // x is the right edge
};

// ARGB helpers
constexpr uint32_t Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
inline uint32_t RgbaToArgb(uint32_t rgba) { return (rgba >> 8) | ((rgba & 0xFF) << 24); }
inline uint32_t ArgbToRgba(uint32_t argb) { return (argb << 8) | (argb >> 24); }
inline uint32_t WithAlpha(uint32_t argb, float a) {
    uint32_t al = (uint32_t)((float)(argb >> 24) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (argb & 0x00FFFFFF) | (al << 24);
}

class Renderer {
public:
    // Must be called inside the game's frame (between BeginScene/EndScene).
    bool Begin();
    void End();

    void Rect(float x, float y, float w, float h, uint32_t argb);
    void RectOutline(float x, float y, float w, float h, uint32_t argb, float t = 1.0f);
    // flat shapes: a triangle, a filled ellipse (rx = ry: a circle), a line of some thickness with round ends, a
    // quadrilateral given by its corners in order
    void Tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb);
    void Ellipse(float cx, float cy, float rx, float ry, uint32_t argb, int segments = 32);
    void Line(float x0, float y0, float x1, float y1, float thickness, uint32_t argb, bool roundEnds = true);
    void Poly4(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t argb);
    // Text in UTF-8. Supports inline colour codes {RRGGBB}.
    void Text(FontId f, float x, float y, std::string_view utf8, uint32_t argb, uint32_t flags = TEXT_SHADOW);
    void TextW(FontId f, float x, float y, std::wstring_view text, uint32_t argb, uint32_t flags = TEXT_SHADOW);
    float TextWidth(FontId f, std::string_view utf8);
    float TextWidthW(FontId f, std::wstring_view text);
    float LineHeight(FontId f) const { return m_fonts[f].lineHeight; }
    // Splits text into lines not wider than maxWidth (colour codes are kept).
    std::vector<std::wstring> Wrap(FontId f, std::wstring_view text, float maxWidth);
    // The text as it fits in maxWidth: whole, or cut with "..." at its end
    std::string Fit(FontId f, std::string_view utf8, float maxWidth);
    // Text in one of the game's fonts, drawn by the game (CFont), black outline. ASCII only: the glyphs above it
    // differ between translations. height = character cell height in pixels.
    void GameText(GameFont font, float x, float y, std::string_view ascii, float height, uint32_t argb, uint32_t flags = 0);
    float GameTextWidth(GameFont font, std::string_view ascii, float height);

    float Width() const { return m_width; }
    float Height() const { return m_height; }
    // UI scale relative to 1080p.
    float Scale() const { return m_height / 1080.0f; }
    bool Ready() const { return m_ready; }
    // nothing drawn shows (the frame's callers still run): a clean picture to record
    void SetHidden(bool hidden) { m_hidden = hidden; }

    // A picture: ARGB pixels (straight alpha, rows top-down) that stay the caller's (static data). Its texture is made
    // when it is first drawn, and again after a device reset. Drawn stretched into the rectangle, tinted.
    int AddImage(const uint32_t* argb, int w, int h);
    void Image(int id, float x, float y, float w, float h, uint32_t tint = 0xFFFFFFFF);
    // its size in pixels
    bool ImageSize(int id, int& w, int& h) const;

    void Invalidate();  // destroy textures (device reset / shutdown)
    // Tests: the frame drawn so far (the game's back buffer, never other windows) as a 24-bit BMP, scaled down.
    bool SaveFrame(const std::string& bmpPath, int downscale);

private:
    struct Glyph {
        float u0, v0, u1, v1;
        float w, h;      // quad size in pixels
        float advance;   // pen advance
    };
    struct Font {
        IDirect3DTexture9* tex = nullptr;
        std::unordered_map<wchar_t, Glyph> glyphs;
        float lineHeight = 0;
        float pad = 2;
        float whiteU = 0, whiteV = 0;  // a fully opaque texel (used for rectangles)
        int pixelHeight = 0;
        float tracking = 0;  // extra space after every glyph
        bool upper = false;  // drawn in capitals
    };
    struct Vertex {
        float x, y, z, rhw;
        uint32_t color;
        float u, v;
    };

    bool BuildFonts();
    bool BuildFont(Font& f, const wchar_t* face, int pixelHeight, int weight, float stretch = 1.0f, float tracking = 0,
                   bool upper = false);
    static wchar_t Shape(const Font& f, wchar_t c);
    void Flush();
    void SetTexture(IDirect3DTexture9* t);
    void Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c);
    const Glyph* GetGlyph(Font& f, wchar_t ch);
    void DrawRun(Font& f, float x, float y, std::wstring_view text, uint32_t argb);

    struct Picture {
        const uint32_t* px = nullptr;
        int w = 0, h = 0;
        int texW = 0, texH = 0;  // the texture: powers of two
        IDirect3DTexture9* tex = nullptr;
    };
    std::vector<Picture> m_pictures;
    bool MakePicture(Picture& p);

    IDirect3DDevice9* m_dev = nullptr;
    void* m_stateBlock = nullptr;
    Font m_fonts[FONT_COUNT];
    IDirect3DTexture9* m_curTex = nullptr;
    std::vector<Vertex> m_verts;
    float m_width = 1920, m_height = 1080;
    int m_fontHeightFor = 0;  // screen height the atlases were built for
    bool m_ready = false;
    bool m_inFrame = false;
    bool m_hidden = false;
};

Renderer& Gfx();

// UTF-8 <-> wide helpers used by the UI
std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

}  // namespace coop::ui
