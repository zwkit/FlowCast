// FlowCast Windows client - drawing primitives (GDI+).
//
// GDI alone gives hard-cornered rectangles and monochrome glyphs; the app is a
// small, quiet window and rounding plus antialiasing is most of what makes it
// look deliberate. GDI+ ships with Windows, so this costs no dependency.
#pragma once
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h> // IStream / PROPID: gdiplus.h needs them and LEAN_AND_MEAN drops them
#include <gdiplus.h>

namespace ui {

using namespace Gdiplus;

struct Theme {
    Color bgTop;
    Color bgBottom;
    Color card;
    Color cardHover;
    Color cardDown;
    Color border;
    Color borderStrong;
    Color separator;
    Color text;
    Color textDim;
    Color textFaint;
    Color accent;
    Color accentHover;
    Color accentDown;
    Color onAccent;
    Color track;
    Color knob;
    Color danger;
};

const Theme& theme(bool dark);
bool system_prefers_dark();

enum class FontRole { Title, Body, Strong, Small, Button, Stat };
enum class AlignH { Left, Center, Right };
enum class AlignV { Top, Middle, Bottom };

void fonts_init(float scale, const wchar_t* family);
void fonts_release();
Font* font(FontRole role);

// Diagnostic hook, driven by `--shot-contrast=N` / `--shot-aa=clearType` so the
// text weight can be compared on screen without editing this file. Contrast is
// GDI+'s text gamma (0-12); pass -1 to leave it alone.
void set_text_aa(int aaMode, int contrast);

// Shared setup: antialiased geometry, subpixel text. Expects a 32bpp surface -
// ClearType is silently downgraded to grayscale on anything else.
void prepare(Graphics& g);

void round_path(GraphicsPath& p, const RectF& r, float radius);
void fill_round(Graphics& g, const RectF& r, float radius, const Color& c);
void stroke_round(Graphics& g, const RectF& r, float radius, const Color& c, float width);
void fill_round_vertical(Graphics& g, const RectF& r, float radius, const Color& top,
                         const Color& bottom);
void fill_polygon(Graphics& g, const PointF* pts, int count, const Color& c);

// Draws `s` inside `box`; returns the width actually used.
float draw_text(Graphics& g, const std::wstring& s, const RectF& box, FontRole role,
                const Color& c, AlignH h, AlignV v);
float text_width(Graphics& g, const std::wstring& s, FontRole role);

// Icons are drawn on a 24x24 grid scaled into `box`, `width` is the stroke size
// in output pixels.
void icon_screen(Graphics& g, const RectF& box, const Color& c, float width);
// Three rising bars: "level", used for the quality row now that the monitor
// outline means the captured screen.
void icon_levels(Graphics& g, const RectF& box, const Color& c, float width);
void icon_tv(Graphics& g, const RectF& box, const Color& c, float width);

// The FlowCast mark: two overlapping panels with a play triangle. `box` is the
// mark's bounding box (104x72 aspect), not a square - the geometry is ported
// verbatim from the Android launcher icon so both platforms show one product.
void icon_app_logo(Graphics& g, const RectF& box);
// Same mark in one colour, for small sizes where the two-tone version mushes.
void icon_app_logo_mono(Graphics& g, const RectF& box, const Color& c, const Color& tri);
void icon_chevron(Graphics& g, const RectF& box, const Color& c, float width, bool up);
void icon_speaker(Graphics& g, const RectF& box, const Color& c, float width, bool on);
void icon_globe(Graphics& g, const RectF& box, const Color& c, float width);
void icon_refresh(Graphics& g, const RectF& box, const Color& c, float width);
void icon_check(Graphics& g, const RectF& box, const Color& c, float width);
void icon_alert(Graphics& g, const RectF& box, const Color& c, float width);
void icon_wifi(Graphics& g, const RectF& box, const Color& c, float width);
void icon_close(Graphics& g, const RectF& box, const Color& c, float width);
void icon_minus(Graphics& g, const RectF& box, const Color& c, float width);
void icon_stop(Graphics& g, const RectF& box, const Color& c, float width);

} // namespace ui
