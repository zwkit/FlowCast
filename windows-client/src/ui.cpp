// FlowCast Windows client - drawing primitives (GDI+).
#include "ui.h"

#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace ui {
namespace {

// --- palettes ---------------------------------------------------------------
// The accent is the icon's own blue: a mid blue in dark, #185FA5 in light. Two
// shades of one hue rather than one colour in two themes, because the mark is
// that pair of blues - a green button under a blue logo reads as clip art.
// The dark accent is deliberately not #85B7EB: a large light-blue slab is the
// brightest thing on the screen and pulls the eye off the status card, so the
// button keeps a deep blue (white label) and the pale blue stays in the mark.
const Theme kDark = {
    /* bgTop        */ Color(255, 26, 31, 38),
    /* bgBottom     */ Color(255, 13, 16, 20),
    /* card         */ Color(255, 28, 34, 42),
    /* cardHover    */ Color(255, 35, 43, 53),
    /* cardDown     */ Color(255, 23, 28, 35),
    /* border       */ Color(255, 44, 53, 65),
    /* borderStrong */ Color(255, 63, 74, 89),
    /* separator    */ Color(255, 40, 48, 59),
    /* text         */ Color(255, 233, 238, 245),
    /* textDim      */ Color(255, 168, 179, 195),
    /* textFaint    */ Color(255, 124, 135, 150),
    /* accent       */ Color(255, 46, 124, 196), // #2E7CC4
    /* accentHover  */ Color(255, 58, 138, 211),
    /* accentDown   */ Color(255, 37, 105, 170),
    /* onAccent     */ Color(255, 255, 255, 255),
    /* track        */ Color(255, 54, 63, 76),
    /* knob         */ Color(255, 255, 255, 255),
    /* danger       */ Color(255, 255, 122, 122),
};

const Theme kLight = {
    /* bgTop        */ Color(255, 250, 251, 253),
    /* bgBottom     */ Color(255, 238, 241, 246),
    /* card         */ Color(255, 255, 255, 255),
    /* cardHover    */ Color(255, 246, 248, 252),
    /* cardDown     */ Color(255, 236, 240, 246),
    /* border       */ Color(255, 224, 229, 237),
    /* borderStrong */ Color(255, 200, 208, 220),
    /* separator    */ Color(255, 232, 236, 243),
    /* text         */ Color(255, 20, 24, 31),
    /* textDim      */ Color(255, 78, 88, 103),
    /* textFaint    */ Color(255, 108, 118, 133),
    /* accent       */ Color(255, 24, 95, 165), // #185FA5, the icon's front panel
    /* accentHover  */ Color(255, 30, 112, 191),
    /* accentDown   */ Color(255, 19, 76, 133),
    /* onAccent     */ Color(255, 255, 255, 255),
    /* track        */ Color(255, 205, 213, 226),
    /* knob         */ Color(255, 255, 255, 255),
    /* danger       */ Color(255, 209, 62, 62),
};

// --- fonts ------------------------------------------------------------------
constexpr int kFontCount = 6;

// GDI+ text gamma. The docs' 0-12 range runs the opposite way to intuition:
// higher values LIGHTEN the antialiased edges (measured: contrast 4 -> ink
// 100.6, contrast 12 -> 90.2 over the same string), so 4 - GDI+'s own default -
// is the heaviest of the lot. Left at 4 unless the shot sweep says otherwise.
int g_textContrast = 4;
// 0 = grayscale grid-fit, 1 = ClearType grid-fit.
//
// ClearType wins on the machine this ships to: Windows renders every other
// window with subpixel AA (FontSmoothingType=2, orientation RGB), so grayscale
// text reads soft and thin next to them, and subpixel positioning lets stems
// land on a third of a pixel instead of a whole one. Measured on the same
// string: grayscale covers 75 dark pixels, ClearType 119.
int g_aaMode = 1;

struct FontSlot {
    Font* font = nullptr;
    float size = 0.f;   // log-free: already multiplied by scale
    int style = 0;
};

FontSlot g_fonts[kFontCount];
std::wstring g_family = L"Segoe UI";
float g_scale = 1.f;

struct FontDef {
    FontRole role;
    float size;
    int style;
};

const FontDef kFontDefs[kFontCount] = {
    {FontRole::Title, 19.0f, FontStyleBold},
    {FontRole::Body, 14.0f, FontStyleRegular},
    {FontRole::Strong, 14.0f, FontStyleBold},
    {FontRole::Small, 12.0f, FontStyleRegular},
    {FontRole::Button, 15.0f, FontStyleBold},
    {FontRole::Stat, 21.0f, FontStyleBold},
};

// Maps the 24x24 icon grid into `box`.
struct Grid {
    RectF box;
    float s;
    Grid(const RectF& b) : box(b), s(b.Width / 24.f) {}
    PointF p(float x, float y) const { return PointF(box.X + x * s, box.Y + y * s); }
    RectF r(float x, float y, float w, float h) const {
        return RectF(box.X + x * s, box.Y + y * s, w * s, h * s);
    }
};

RectF measure(Graphics& g, const std::wstring& s, Font* f) {
    RectF out(0.f, 0.f, 0.f, 0.f);
    if (!f || s.empty()) return out;
    g.MeasureString(s.c_str(), (INT)s.size(), f, PointF(0.f, 0.f), &out);
    return out;
}

} // namespace

const Theme& theme(bool dark) { return dark ? kDark : kLight; }

void set_text_aa(int aaMode, int contrast) {
    if (aaMode >= 0) g_aaMode = aaMode == 0 ? 0 : 1;
    if (contrast >= 0) g_textContrast = contrast > 12 ? 12 : contrast;
}

bool system_prefers_dark() {
    // AppsUseLightTheme: 1 = light. Missing key means light on Win10/11, but a
    // missing key also happens on very old builds, where dark is the sane guess
    // for a media app.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                        KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG rc = ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&value, &size);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return true;
    return value == 0;
}

void fonts_init(float scale, const wchar_t* family) {
    fonts_release();
    g_scale = scale > 0.f ? scale : 1.f;
    if (family && *family) g_family = family;
    for (int i = 0; i < kFontCount; ++i) {
        const FontDef& d = kFontDefs[i];
        FontSlot& slot = g_fonts[i];
        // Integer pixel sizes. GDI+ hints glyphs onto the pixel grid, and a
        // fractional em box makes every stem land between pixels - that is the
        // single biggest reason small Chinese text looks muddy.
        slot.size = std::floor(d.size * g_scale + 0.5f);
        slot.style = d.style;
        slot.font = new Font(g_family.c_str(), slot.size, d.style, UnitPixel);
        if (slot.font->GetLastStatus() != Ok) {
            delete slot.font;
            slot.font = new Font(L"Segoe UI", slot.size, d.style, UnitPixel);
        }
    }
}

void fonts_release() {
    for (FontSlot& slot : g_fonts) {
        delete slot.font;
        slot.font = nullptr;
    }
}

Font* font(FontRole role) {
    const int i = (int)role;
    if (i < 0 || i >= kFontCount) return nullptr;
    return g_fonts[i].font;
}

void prepare(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
}

void round_path(GraphicsPath& p, const RectF& r, float radius) {
    p.Reset();
    if (r.Width <= 0.f || r.Height <= 0.f) return;
    float d = radius * 2.f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 0.01f) {
        p.AddRectangle(r);
        p.CloseFigure();
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}

void fill_round(Graphics& g, const RectF& r, float radius, const Color& c) {
    GraphicsPath p;
    round_path(p, r, radius);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

void stroke_round(Graphics& g, const RectF& r, float radius, const Color& c, float width) {
    GraphicsPath p;
    // Inset by half the stroke so the outline stays inside the given box.
    round_path(p, RectF(r.X + width / 2.f, r.Y + width / 2.f, r.Width - width, r.Height - width),
               radius - width / 2.f);
    Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

void fill_round_vertical(Graphics& g, const RectF& r, float radius, const Color& top,
                         const Color& bottom) {
    GraphicsPath p;
    round_path(p, r, radius);
    LinearGradientBrush b(r, top, bottom, LinearGradientModeVertical);
    g.FillPath(&b, &p);
}

void fill_polygon(Graphics& g, const PointF* pts, int count, const Color& c) {
    if (!pts || count < 3) return;
    GraphicsPath p;
    p.AddPolygon(pts, count);
    p.CloseFigure();
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

float draw_text(Graphics& g, const std::wstring& s, const RectF& box, FontRole role,
                const Color& c, AlignH h, AlignV v) {
    Font* f = font(role);
    if (!f || s.empty()) return 0.f;
    // Type needs the opposite settings from shapes. PixelOffsetModeHalf makes a
    // 1px stroke sit on the grid, but it also shifts hinted glyphs by half a
    // pixel, which forces a resample and turns every stem grey.
    const PixelOffsetMode savedOffset = g.GetPixelOffsetMode();
    const TextRenderingHint savedHint = g.GetTextRenderingHint();
    const int savedContrast = g.GetTextContrast();
    g.SetPixelOffsetMode(PixelOffsetModeNone);
    g.SetTextRenderingHint(g_aaMode == 0 ? TextRenderingHintAntiAliasGridFit
                                         : TextRenderingHintClearTypeGridFit);
    g.SetTextContrast(g_textContrast);

    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetAlignment(h == AlignH::Left ? StringAlignmentNear
                                     : (h == AlignH::Center ? StringAlignmentCenter : StringAlignmentFar));
    fmt.SetLineAlignment(v == AlignV::Top ? StringAlignmentNear
                                         : (v == AlignV::Middle ? StringAlignmentCenter : StringAlignmentFar));
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, box, &fmt, &b);
    const float w = measure(g, s, f).Width;
    g.SetTextContrast(savedContrast);
    g.SetTextRenderingHint(savedHint);
    g.SetPixelOffsetMode(savedOffset);
    return w;
}

float text_width(Graphics& g, const std::wstring& s, FontRole role) {
    return measure(g, s, font(role)).Width;
}

// --- icons ------------------------------------------------------------------

void icon_screen(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath p;
    round_path(p, k.r(2.f, 3.f, 20.f, 13.f), 2.6f * k.s);
    g.DrawPath(&pen, &p);
    // AirPlay's triangle sits below the panel, apex touching it.
    g.DrawLine(&pen, k.p(12.f, 15.f), k.p(15.6f, 20.6f));
    g.DrawLine(&pen, k.p(15.6f, 20.6f), k.p(8.4f, 20.6f));
    g.DrawLine(&pen, k.p(8.4f, 20.6f), k.p(12.f, 15.f));
}

// Levels: three bars of rising height, all growing from the same baseline. Read
// as "how much" rather than "which thing", which is what a quality row needs.
void icon_levels(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    const float base = 19.f;
    const float tops[3] = {13.f, 9.f, 5.f};
    const float xs[3] = {5.5f, 12.f, 18.5f};
    for (int i = 0; i < 3; i++) g.DrawLine(&pen, k.p(xs[i], base), k.p(xs[i], tops[i]));
}

void icon_tv(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath p;
    round_path(p, k.r(2.f, 4.f, 20.f, 13.f), 2.4f * k.s);
    g.DrawPath(&pen, &p);
    g.DrawLine(&pen, k.p(12.f, 17.f), k.p(12.f, 19.4f));
    g.DrawLine(&pen, k.p(8.2f, 20.6f), k.p(15.8f, 20.6f));
}

// Ported from the Android launcher icon (res/drawable/ic_launcher_foreground.xml).
// That file uses a 182x182 viewport where the mark occupies 104x72; here `box`
// *is* that 104x72 box, so the caller decides how much of the square the mark
// fills. Keeping the authored numbers (panels 84x56 r12, back panel offset
// (20,-16), triangle bbox centred on the front panel) means the Windows icon and
// the Android icon stay one drawing rather than two lookalikes.
namespace {
struct MarkBox {
    float u = 0.f;
    float ox = 0.f;
    float oy = 0.f;
    explicit MarkBox(const RectF& box) {
        if (box.Width <= 0.f || box.Height <= 0.f) return;
        u = box.Width / 104.f < box.Height / 72.f ? box.Width / 104.f : box.Height / 72.f;
        ox = box.X + (box.Width - 104.f * u) / 2.f;
        oy = box.Y + (box.Height - 72.f * u) / 2.f;
    }
    float x(float v) const { return ox + v * u; }
    float y(float v) const { return oy + v * u; }
    RectF back() const { return RectF(x(20.f), y(0.f), 84.f * u, 56.f * u); }
    RectF front() const { return RectF(x(0.f), y(16.f), 84.f * u, 56.f * u); }
    void triangle(PointF out[3]) const {
        out[0] = PointF(x(33.f), y(30.f));
        out[1] = PointF(x(33.f), y(58.f));
        out[2] = PointF(x(51.f), y(44.f));
    }
};
} // namespace

void icon_app_logo(Graphics& g, const RectF& box) {
    const MarkBox m(box);
    if (m.u <= 0.f) return;
    // Back panel first: only a 16px top strip and a 20px right strip stay visible.
    fill_round(g, m.back(), 12.f * m.u, Color(255, 133, 183, 235)); // #85B7EB
    fill_round(g, m.front(), 12.f * m.u, Color(255, 24, 95, 165));  // #185FA5
    PointF tri[3];
    m.triangle(tri);
    fill_polygon(g, tri, 3, Color(255, 255, 255, 255));
}

// Single-colour variant for the window header, where the mark is ~20 px wide.
// An outlined two-panel drawing collapses into mud at that size, so the same
// geometry is filled instead and the back panel fades to read as "behind".
void icon_app_logo_mono(Graphics& g, const RectF& box, const Color& c, const Color& tri) {
    const MarkBox m(box);
    if (m.u <= 0.f) return;
    fill_round(g, m.back(), 12.f * m.u,
               Color((BYTE)((int)c.GetA() * 45 / 100), c.GetR(), c.GetG(), c.GetB()));
    fill_round(g, m.front(), 12.f * m.u, c);
    PointF pts[3];
    m.triangle(pts);
    fill_polygon(g, pts, 3, tri);
}

void icon_chevron(Graphics& g, const RectF& box, const Color& c, float width, bool up) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    if (up) {
        g.DrawLine(&pen, k.p(8.f, 14.f), k.p(12.f, 10.f));
        g.DrawLine(&pen, k.p(12.f, 10.f), k.p(16.f, 14.f));
    } else {
        g.DrawLine(&pen, k.p(8.f, 10.f), k.p(12.f, 14.f));
        g.DrawLine(&pen, k.p(12.f, 14.f), k.p(16.f, 10.f));
    }
}

void icon_speaker(Graphics& g, const RectF& box, const Color& c, float width, bool on) {
    const Grid k(box);
    PointF body[7] = {k.p(3.f, 9.2f),  k.p(7.f, 9.2f),  k.p(11.2f, 5.f),
                      k.p(11.2f, 19.f), k.p(7.f, 14.8f), k.p(3.f, 14.8f),
                      k.p(3.f, 9.2f)};
    SolidBrush b(c);
    GraphicsPath p;
    p.AddPolygon(body, 7);
    p.CloseFigure();
    g.FillPath(&b, &p);
    if (on) {
        Pen pen(c, width);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawArc(&pen, k.r(8.6f, 8.4f, 7.2f, 7.2f), -55.f, 110.f);
        g.DrawArc(&pen, k.r(8.6f, 5.4f, 13.2f, 13.2f), -55.f, 110.f);
    } else {
        Pen pen(c, width);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, k.p(15.f, 9.5f), k.p(20.f, 14.5f));
        g.DrawLine(&pen, k.p(20.f, 9.5f), k.p(15.f, 14.5f));
    }
}

void icon_globe(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    g.DrawEllipse(&pen, k.r(2.5f, 2.5f, 19.f, 19.f));
    g.DrawEllipse(&pen, k.r(7.5f, 2.5f, 9.f, 19.f));
    g.DrawLine(&pen, k.p(2.5f, 12.f), k.p(21.5f, 12.f));
}

void icon_refresh(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    const float cx = 12.f, cy = 12.f, radius = 8.f;
    const float startDeg = 50.f;
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawArc(&pen, k.r(cx - radius, cy - radius, radius * 2.f, radius * 2.f), startDeg, 290.f);

    // Arrowhead on the arc's start, pointing along the sweep direction.
    const float rad = startDeg * 3.14159265f / 180.f;
    const float px = cx + radius * std::cos(rad);
    const float py = cy + radius * std::sin(rad);
    const float tx = -std::sin(rad), ty = std::cos(rad);   // clockwise tangent
    const float nx = std::cos(rad), ny = std::sin(rad);    // outward normal
    PointF tri[3] = {
        k.p(px + tx * 1.2f, py + ty * 1.2f),
        k.p(px - tx * 3.4f + nx * 2.6f, py - ty * 3.4f + ny * 2.6f),
        k.p(px - tx * 3.4f - nx * 2.6f, py - ty * 3.4f - ny * 2.6f),
    };
    fill_polygon(g, tri, 3, c);
}

void icon_check(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    g.DrawLine(&pen, k.p(6.f, 12.5f), k.p(10.4f, 17.f));
    g.DrawLine(&pen, k.p(10.4f, 17.f), k.p(18.f, 8.f));
}

void icon_alert(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    g.DrawEllipse(&pen, k.r(3.f, 3.f, 18.f, 18.f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, k.p(12.f, 7.5f), k.p(12.f, 13.2f));
    SolidBrush b(c);
    g.FillEllipse(&b, k.r(10.9f, 15.8f, 2.2f, 2.2f));
}

void icon_wifi(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    const float cx = 12.f, cy = 19.f;
    const float radii[3] = {5.2f, 9.4f, 13.6f};
    for (const float r : radii) {
        g.DrawArc(&pen, k.r(cx - r, cy - r, r * 2.f, r * 2.f), 222.f, 96.f);
    }
    SolidBrush b(c);
    g.FillEllipse(&b, k.r(10.8f, 17.8f, 2.4f, 2.4f));
}

void icon_close(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, k.p(8.f, 8.f), k.p(16.f, 16.f));
    g.DrawLine(&pen, k.p(16.f, 8.f), k.p(8.f, 16.f));
}

void icon_minus(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, k.p(7.5f, 12.f), k.p(16.5f, 12.f));
}

void icon_stop(Graphics& g, const RectF& box, const Color& c, float width) {
    const Grid k(box);
    (void)width;
    fill_round(g, k.r(6.5f, 6.5f, 11.f, 11.f), 2.6f * k.s, c);
}

} // namespace ui
