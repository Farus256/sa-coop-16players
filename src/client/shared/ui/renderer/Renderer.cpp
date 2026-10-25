#include "shared/ui/renderer/Renderer.h"

#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/game/Game.h"

namespace coop::ui {

Renderer& Gfx() {
    static Renderer r;
    return r;
}

std::wstring Widen(std::string_view s) { return Utf8ToWide(s); }
std::string Narrow(std::wstring_view s) { return WideToUtf8(s); }

static constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// Characters baked into the atlases.
static void CollectCharset(std::vector<wchar_t>& out) {
    for (wchar_t c = 0x20; c <= 0x7E; ++c) out.push_back(c);
    for (wchar_t c = 0xA0; c <= 0xFF; ++c) out.push_back(c);
    for (wchar_t c = 0x400; c <= 0x45F; ++c) out.push_back(c);  // Cyrillic
    for (wchar_t c = 0x490; c <= 0x491; ++c) out.push_back(c);  // Ґ ґ
    const wchar_t extra[] = {0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026,
                             0x2116, 0x20AC, 0x2190, 0x2191, 0x2192, 0x2193, 0x25B2, 0x25BC, 0x25CF, 0x2713};
    for (wchar_t c : extra) out.push_back(c);
}

bool Renderer::BuildFont(Font& f, const wchar_t* face, int pixelHeight, int weight, float stretch, float tracking,
                         bool upper) {
    std::vector<wchar_t> chars;
    CollectCharset(chars);

    HDC dc = CreateCompatibleDC(nullptr);
    auto create = [&](int width) {
        return ::CreateFontW(-pixelHeight, width, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    HFONT font = create(0);
    if (stretch != 1.0f) {  // wider glyphs: an explicit average width makes GDI scale them horizontally
        HGDIOBJ prev = SelectObject(dc, font);
        TEXTMETRICW tm0;
        GetTextMetricsW(dc, &tm0);
        SelectObject(dc, prev);
        DeleteObject(font);
        font = create((int)std::lround(tm0.tmAveCharWidth * stretch));
    }
    f.tracking = tracking;
    f.upper = upper;
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int lineH = tm.tmHeight;
    int pad = std::max(2, pixelHeight / 10);
    f.pad = (float)pad;

    // pack glyphs in rows
    const int texW = 1024;
    int x = 0, y = 0, rowH = lineH + pad * 2;
    struct Pos {
        wchar_t ch;
        int x, y, cellW, adv;
    };
    std::vector<Pos> pos;
    // white block for rectangles
    pos.push_back({0, 0, 0, 4, 0});
    x = 4 + 2;
    for (wchar_t ch : chars) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        int cellW = sz.cx + pad * 2;
        if (x + cellW > texW) {
            x = 0;
            y += rowH;
        }
        pos.push_back({ch, x, y, cellW, (int)sz.cx});
        x += cellW + 1;
    }
    int texH = 64;
    while (texH < y + rowH) texH <<= 1;
    if (texH > 2048) {
        LOGE("Font atlas too large");
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = texW;
    bi.bmiHeader.biHeight = -texH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)texW * texH * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (auto& p : pos) {
        if (p.ch == 0) continue;
        TextOutW(dc, p.x + pad, p.y + pad, &p.ch, 1);
    }
    GdiFlush();

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(m_dev->CreateTexture(texW, texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        LOGE("CreateTexture for font failed");
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        const uint32_t* src = (const uint32_t*)bits;
        for (int row = 0; row < texH; ++row) {
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + row * lr.Pitch);
            for (int col = 0; col < texW; ++col) {
                uint32_t px = src[row * texW + col];
                uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                uint32_t a = std::max(r, std::max(g, b));
                dst[col] = (a << 24) | 0x00FFFFFF;
            }
        }
        // opaque white block for rectangles
        for (int row = 0; row < 4; ++row) {
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + row * lr.Pitch);
            for (int col = 0; col < 4; ++col) dst[col] = 0xFFFFFFFF;
        }
        tex->UnlockRect(0);
    }

    f.tex = tex;
    f.lineHeight = (float)lineH;
    f.pixelHeight = pixelHeight;
    f.whiteU = 2.0f / texW;
    f.whiteV = 2.0f / texH;
    f.glyphs.clear();
    for (auto& p : pos) {
        if (p.ch == 0) continue;
        Glyph g;
        g.u0 = (float)p.x / texW;
        g.v0 = (float)p.y / texH;
        g.u1 = (float)(p.x + p.cellW) / texW;
        g.v1 = (float)(p.y + rowH) / texH;
        g.w = (float)p.cellW;
        g.h = (float)rowH;
        g.advance = (float)p.adv;
        f.glyphs[p.ch] = g;
    }

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
}

bool Renderer::BuildFonts() {
    Invalidate();
    int h = (int)m_height;
    int normal = std::max(12, (int)std::lround(h / 46.0));
    int small = std::max(11, (int)std::lround(h / 60.0));
    int big = std::max(16, (int)std::lround(h / 30.0));
    int menu = std::max(12, (int)std::lround(h / 45.0));
    int menuSmall = std::max(10, (int)std::lround(h / 66.0));
    // the menu font of the game is wide and spaced out: Bahnschrift stretched by a third, with tracking
    bool ok = BuildFont(m_fonts[FONT_NORMAL], L"Arial", normal, FW_BOLD) &&
              BuildFont(m_fonts[FONT_SMALL], L"Arial", small, FW_BOLD) &&
              BuildFont(m_fonts[FONT_BIG], L"Bahnschrift", big, FW_BOLD, 1.35f, big * 0.12f, true) &&
              BuildFont(m_fonts[FONT_MENU], L"Bahnschrift", menu, FW_BOLD, 1.3f, menu * 0.12f, true) &&
              BuildFont(m_fonts[FONT_MENU_SMALL], L"Bahnschrift", menuSmall, FW_SEMIBOLD, 1.3f, menuSmall * 0.16f, true);
    m_fontHeightFor = h;
    if (ok) LOGI("UI fonts created for %dx%d (%d/%d/%d/%d/%d px)", (int)m_width, h, normal, small, big, menu, menuSmall);
    return ok;
}

int Renderer::AddImage(const uint32_t* argb, int w, int h) {
    Picture p;
    p.px = argb;
    p.w = w;
    p.h = h;
    m_pictures.push_back(p);
    return (int)m_pictures.size() - 1;
}

bool Renderer::ImageSize(int id, int& w, int& h) const {
    if (id < 0 || id >= (int)m_pictures.size()) return false;
    w = m_pictures[(size_t)id].w;
    h = m_pictures[(size_t)id].h;
    return true;
}

bool Renderer::MakePicture(Picture& p) {
    if (p.tex) return true;
    if (!m_dev || !p.px || p.w <= 0 || p.h <= 0) return false;
    p.texW = 1, p.texH = 1;
    while (p.texW < p.w) p.texW <<= 1;
    while (p.texH < p.h) p.texH <<= 1;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(m_dev->CreateTexture(p.texW, p.texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        LOGE("CreateTexture for a picture failed");
        return false;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        for (int row = 0; row < p.texH; ++row) {
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + row * lr.Pitch);
            int srcRow = std::min(row, p.h - 1);  // the edge repeated: no dark seam when filtered
            for (int col = 0; col < p.texW; ++col) {
                uint32_t px = p.px[(size_t)srcRow * p.w + std::min(col, p.w - 1)];
                dst[col] = (row >= p.h || col >= p.w) ? (px & 0x00FFFFFF) : px;
            }
        }
        tex->UnlockRect(0);
    }
    p.tex = tex;
    return true;
}

void Renderer::Image(int id, float x, float y, float w, float h, uint32_t tint) {
    if (!m_inFrame || id < 0 || id >= (int)m_pictures.size()) return;
    Picture& p = m_pictures[(size_t)id];
    if (!MakePicture(p)) return;
    SetTexture(p.tex);
    Quad(x, y, w, h, 0.0f, 0.0f, (float)p.w / (float)p.texW, (float)p.h / (float)p.texH, tint);
}

void Renderer::Invalidate() {
    for (auto& p : m_pictures) {
        if (p.tex) p.tex->Release();
        p.tex = nullptr;
    }
    for (auto& f : m_fonts) {
        if (f.tex) f.tex->Release();
        f.tex = nullptr;
        f.glyphs.clear();
    }
    m_ready = false;
}

bool Renderer::Begin() {
    m_dev = (IDirect3DDevice9*)game::D3DDevice();
    if (!m_dev) return false;

    D3DVIEWPORT9 vp;
    if (SUCCEEDED(m_dev->GetViewport(&vp)) && vp.Width > 0) {
        // The game may render into a viewport smaller than the back buffer (widescreen borders);
        // use the back buffer size instead.
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            bb->Release();
            m_width = (float)d.Width;
            m_height = (float)d.Height;
        } else {
            m_width = (float)vp.Width;
            m_height = (float)vp.Height;
        }
    }
    if (!m_ready || m_fontHeightFor != (int)m_height) {
        m_ready = BuildFonts();
        if (!m_ready) return false;
    }

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(m_dev->CreateStateBlock(D3DSBT_ALL, &sb))) return false;
    m_stateBlock = sb;

    D3DVIEWPORT9 full{0, 0, (DWORD)m_width, (DWORD)m_height, 0.0f, 1.0f};
    m_dev->SetViewport(&full);
    m_dev->SetVertexShader(nullptr);
    m_dev->SetPixelShader(nullptr);
    m_dev->SetFVF(kFvf);
    m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    m_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    m_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    m_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_curTex = nullptr;
    m_dev->SetTexture(0, m_fonts[FONT_NORMAL].tex);
    m_curTex = m_fonts[FONT_NORMAL].tex;
    m_verts.clear();
    m_inFrame = true;
    return true;
}

void Renderer::End() {
    if (!m_inFrame) return;
    Flush();
    auto* sb = (IDirect3DStateBlock9*)m_stateBlock;
    if (sb) {
        sb->Apply();
        sb->Release();
    }
    m_stateBlock = nullptr;
    m_inFrame = false;
}

void Renderer::Flush() {
    if (m_verts.empty() || !m_dev) return;
    m_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(m_verts.size() / 3), m_verts.data(), sizeof(Vertex));
    m_verts.clear();
}

void Renderer::SetTexture(IDirect3DTexture9* t) {
    if (t == m_curTex) return;
    Flush();
    m_dev->SetTexture(0, t);
    m_curTex = t;
}

void Renderer::Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c) {
    if (m_hidden) return;
    x -= 0.5f;
    y -= 0.5f;
    Vertex a{x, y, 0, 1, c, u0, v0}, b{x + w, y, 0, 1, c, u1, v0}, d{x, y + h, 0, 1, c, u0, v1},
        e{x + w, y + h, 0, 1, c, u1, v1};
    m_verts.push_back(a);
    m_verts.push_back(b);
    m_verts.push_back(d);
    m_verts.push_back(b);
    m_verts.push_back(e);
    m_verts.push_back(d);
}

void Renderer::Rect(float x, float y, float w, float h, uint32_t argb) {
    if (!m_inFrame || (argb >> 24) == 0) return;
    Font& f = m_fonts[FONT_NORMAL];
    SetTexture(f.tex);
    Quad(x, y, w, h, f.whiteU, f.whiteV, f.whiteU, f.whiteV, argb);
}

void Renderer::Tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb) {
    if (!m_inFrame || (argb >> 24) == 0) return;
    Font& f = m_fonts[FONT_NORMAL];
    SetTexture(f.tex);
    float u = f.whiteU, v = f.whiteV;
    m_verts.push_back({x0 - 0.5f, y0 - 0.5f, 0, 1, argb, u, v});
    m_verts.push_back({x1 - 0.5f, y1 - 0.5f, 0, 1, argb, u, v});
    m_verts.push_back({x2 - 0.5f, y2 - 0.5f, 0, 1, argb, u, v});
}

void Renderer::Poly4(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t argb) {
    Tri(x0, y0, x1, y1, x2, y2, argb);
    Tri(x0, y0, x2, y2, x3, y3, argb);
}

void Renderer::Ellipse(float cx, float cy, float rx, float ry, uint32_t argb, int segments) {
    for (int i = 0; i < segments; ++i) {
        float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
        Tri(cx, cy, cx + std::cos(a0) * rx, cy + std::sin(a0) * ry, cx + std::cos(a1) * rx, cy + std::sin(a1) * ry, argb);
    }
}

void Renderer::Line(float x0, float y0, float x1, float y1, float t, uint32_t argb, bool roundEnds) {
    float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.001f) {
        float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
        Poly4(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, argb);
    }
    if (roundEnds) {
        Ellipse(x0, y0, t * 0.5f, t * 0.5f, argb, 16);
        Ellipse(x1, y1, t * 0.5f, t * 0.5f, argb, 16);
    }
}

void Renderer::RectOutline(float x, float y, float w, float h, uint32_t argb, float t) {
    Rect(x, y, w, t, argb);
    Rect(x, y + h - t, w, t, argb);
    Rect(x, y + t, t, h - 2 * t, argb);
    Rect(x + w - t, y + t, t, h - 2 * t, argb);
}

wchar_t Renderer::Shape(const Font& f, wchar_t c) {
    return f.upper ? (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c) : c;  // one character in the low word
}

const Renderer::Glyph* Renderer::GetGlyph(Font& f, wchar_t ch) {
    auto it = f.glyphs.find(ch);
    if (it != f.glyphs.end()) return &it->second;
    if (ch == 0x401) return GetGlyph(f, 0x415);  // Ё -> Е if missing
    it = f.glyphs.find(L'?');
    return it != f.glyphs.end() ? &it->second : nullptr;
}

static bool ParseColorCode(std::wstring_view t, size_t i, uint32_t& rgb) {
    if (i + 7 >= t.size() + 0 || t[i] != L'{' || i + 7 >= t.size() || t[i + 7] != L'}') return false;
    uint32_t v = 0;
    for (size_t k = 1; k <= 6; ++k) {
        wchar_t c = t[i + k];
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= c - L'0';
        else if (c >= L'a' && c <= L'f') v |= c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v |= c - L'A' + 10;
        else return false;
    }
    rgb = v;
    return true;
}

void Renderer::DrawRun(Font& f, float x, float y, std::wstring_view text, uint32_t argb) {
    SetTexture(f.tex);
    float pen = x;
    uint32_t color = argb;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t rgb;
        if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
            color = (argb & 0xFF000000) | rgb;
            i += 7;
            continue;
        }
        const Glyph* g = GetGlyph(f, Shape(f, text[i]));
        if (!g) continue;
        if (text[i] != L' ') Quad(std::floor(pen - f.pad), std::floor(y - f.pad), g->w, g->h, g->u0, g->v0, g->u1, g->v1, color);
        pen += g->advance + f.tracking;
    }
}

float Renderer::TextWidthW(FontId fid, std::wstring_view text) {
    Font& f = m_fonts[fid];
    float w = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t rgb;
        if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
            i += 7;
            continue;
        }
        if (const Glyph* g = GetGlyph(f, Shape(f, text[i]))) w += g->advance + f.tracking;
    }
    return w;
}

float Renderer::TextWidth(FontId f, std::string_view utf8) { return TextWidthW(f, Widen(utf8)); }

void Renderer::TextW(FontId fid, float x, float y, std::wstring_view text, uint32_t argb, uint32_t flags) {
    if (!m_inFrame || text.empty()) return;
    Font& f = m_fonts[fid];
    if (flags & (TEXT_CENTER | TEXT_RIGHT)) {
        float w = TextWidthW(fid, text);
        x -= (flags & TEXT_CENTER) ? w * 0.5f : w;
    }
    x = std::floor(x);
    y = std::floor(y);
    uint32_t shadow = WithAlpha(0xFF000000, (float)(argb >> 24) / 255.0f * 0.85f);
    // shadows must not take inline colours: strip codes by drawing with a fixed colour
    auto drawShadow = [&](float dx, float dy) {
        std::wstring plain;
        plain.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            uint32_t rgb;
            if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
                i += 7;
                continue;
            }
            plain += text[i];
        }
        DrawRun(f, x + dx, y + dy, plain, shadow);
    };
    if (flags & TEXT_OUTLINE) {
        drawShadow(-1, 0);
        drawShadow(1, 0);
        drawShadow(0, -1);
        drawShadow(0, 1);
    } else if (flags & TEXT_SHADOW) {
        drawShadow(1, 1);
    }
    DrawRun(f, x, y, text, argb);
}

void Renderer::Text(FontId f, float x, float y, std::string_view utf8, uint32_t argb, uint32_t flags) {
    TextW(f, x, y, Widen(utf8), argb, flags);
}

std::string Renderer::Fit(FontId f, std::string_view utf8, float maxWidth) {
    std::wstring w = Widen(utf8);
    if (TextWidthW(f, w) <= maxWidth) return std::string(utf8);
    while (!w.empty() && TextWidthW(f, w + L"...") > maxWidth) w.pop_back();
    return Narrow(w + L"...");
}

std::vector<std::wstring> Renderer::Wrap(FontId fid, std::wstring_view text, float maxWidth) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    std::wstring lastColor;
    float curW = 0;
    Font& f = m_fonts[fid];
    size_t lastSpace = std::wstring::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t rgb;
        if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
            lastColor = std::wstring(text.substr(i, 8));
            cur += lastColor;
            i += 7;
            continue;
        }
        wchar_t ch = text[i];
        const Glyph* g = GetGlyph(f, Shape(f, ch));
        float adv = g ? g->advance + f.tracking : 0;
        if (curW + adv > maxWidth && !cur.empty()) {
            if (lastSpace != std::wstring::npos && lastSpace > 0) {
                std::wstring rest = cur.substr(lastSpace + 1);
                cur.resize(lastSpace);
                lines.push_back(cur);
                cur = lastColor + rest;
                curW = TextWidthW(fid, rest);
            } else {
                lines.push_back(cur);
                cur = lastColor;
                curW = 0;
            }
            lastSpace = std::wstring::npos;
        }
        if (ch == L' ') lastSpace = cur.size();
        cur += ch;
        curW += adv;
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) lines.emplace_back();
    return lines;
}

bool Renderer::SaveFrame(const std::string& bmpPath, int downscale) {
    auto* dev = (IDirect3DDevice9*)game::D3DDevice();
    if (!dev || downscale < 1) return false;
    IDirect3DSurface9 *bb = nullptr, *plain = nullptr, *sys = nullptr;
    bool ok = false;
    D3DSURFACE_DESC d;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && SUCCEEDED(bb->GetDesc(&d)) &&
        SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &plain, nullptr)) &&
        SUCCEEDED(dev->StretchRect(bb, nullptr, plain, nullptr, D3DTEXF_NONE)) &&  // resolves a multisampled buffer
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(plain, sys))) {
        D3DLOCKED_RECT lr;
        if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            int w = (int)d.Width / downscale, h = (int)d.Height / downscale, stride = (w * 3 + 3) & ~3;
            std::vector<uint8_t> file(54 + (size_t)stride * h);
            BITMAPFILEHEADER* fh = (BITMAPFILEHEADER*)file.data();
            BITMAPINFOHEADER* ih = (BITMAPINFOHEADER*)(file.data() + 14);
            fh->bfType = 0x4D42;
            fh->bfSize = (DWORD)file.size();
            fh->bfOffBits = 54;
            ih->biSize = 40;
            ih->biWidth = w;
            ih->biHeight = h;  // bottom-up
            ih->biPlanes = 1;
            ih->biBitCount = 24;
            for (int y = 0; y < h; ++y) {
                const uint8_t* src = (const uint8_t*)lr.pBits + (size_t)(y * downscale) * lr.Pitch;
                uint8_t* dst = file.data() + 54 + (size_t)(h - 1 - y) * stride;
                for (int x = 0; x < w; ++x) memcpy(dst + x * 3, src + (size_t)x * downscale * 4, 3);  // B, G, R
            }
            sys->UnlockRect();
            if (FILE* f = fopen(bmpPath.c_str(), "wb")) {
                ok = fwrite(file.data(), 1, file.size(), f) == file.size();
                fclose(f);
            }
        }
    }
    if (sys) sys->Release();
    if (plain) plain->Release();
    if (bb) bb->Release();
    return ok;
}

// ---------------------------------------------------------------- the game's own fonts
namespace {
// CFont keeps its settings in statics (colour, scale, alignment, wrap, style, outline, render state): they are
// saved and restored around our text so the game's own printing is not affected.
constexpr uintptr_t kFontStatics = 0xC71A60;
constexpr size_t kFontStaticsSize = 0x70;
constexpr float kCellAspect = 0.55f;  // glyph cell width : height, close to the game's menu titles

uint32_t ToRgba(uint32_t argb) {  // CRGBA in memory: r, g, b, a
    return ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000);
}

void SetupGameFont(GameFont font, float height, uint32_t argb) {
    float sy = height / 16.0f;  // CFont::PrintChar: cell height = 16 * scale.y
    game::Call<0x719490>((short)font);                                          // SetFontStyle
    game::Call<0x719380>(sy * kCellAspect, sy);                                  // SetScale
    game::Call<0x719430>(ToRgba(argb));                                         // SetColor
    game::Call<0x719510>(ToRgba(argb & 0xFF000000));                            // SetDropColor
    game::Call<0x719590>((short)std::max(1, (int)std::lround(height / 28.0f)));  // SetOutlinePosition
    game::Call<0x7195B0>(true);                                                 // SetProp
    game::Call<0x7195C0>(false, false);                                         // SetBackground
    game::Call<0x719610>(1);                                                    // SetAlignment(ALIGN_LEFT)
    game::Call<0x7194D0>(100000.0f);                                            // SetWrapx: no wrapping
    game::Call<0x719420>(0.0f);                                                 // SetSlant
    game::Call<0x719500>(255.0f);                                               // SetAlphaFade
}
}  // namespace

float Renderer::GameTextWidth(GameFont font, std::string_view ascii, float height) {
    std::string s(ascii);
    uint8_t saved[kFontStaticsSize];
    memcpy(saved, (void*)kFontStatics, sizeof(saved));
    SetupGameFont(font, height, 0xFFFFFFFF);
    float w = game::Call<0x71A0E0, float>(s.data(), true, false);  // CFont::GetStringWidth
    memcpy((void*)kFontStatics, saved, sizeof(saved));
    return w;
}

void Renderer::GameText(GameFont font, float x, float y, std::string_view ascii, float height, uint32_t argb,
                        uint32_t flags) {
    if (!m_inFrame || m_hidden || ascii.empty()) return;
    if (flags & (TEXT_CENTER | TEXT_RIGHT)) {
        float w = GameTextWidth(font, ascii, height);
        x -= (flags & TEXT_CENTER) ? w * 0.5f : w;
    }
    std::string s(ascii);  // CFont wants a writable, NUL-terminated string
    End();                 // the game draws with its own device state
    uint8_t saved[kFontStaticsSize];
    memcpy(saved, (void*)kFontStatics, sizeof(saved));
    SetupGameFont(font, height, argb);
    game::Call<0x71A700>(x, y, (unsigned char*)s.data());  // CFont::PrintString
    game::Call<0x71A210>();                                 // CFont::DrawFonts
    memcpy((void*)kFontStatics, saved, sizeof(saved));
    Begin();
}

}  // namespace coop::ui
