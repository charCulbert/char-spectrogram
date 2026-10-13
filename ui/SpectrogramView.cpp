// char-spectrogram's interface, made only of libs/chardraw's parts: a
// gpu::Spectrogram (chardraw/views.h) fills the window, its scales drawn over
// its edges, and the settings drawer's tab (Ui::drawer, an overlay) on its
// top edge. Open, the drawer's panel comes down over the plot, see-through,
// with the analysis and display settings in columns: lists as readouts
// (Ui::readout), numbers as number boxes to drag or type into; it's as big
// as they need. The plot's context menu has the rest, and Pause; the plot
// is in the Tab order, so the Menu key opens it too. Hovering
// shows the frequency, nearest pitch and level under the pointer; holding a
// key of the piano strip (or, with Click tone, the plot) plays its pitch
// (through the plugin) and marks it across the plot. The columns arrive from
// the plugin as "column" messages (see plugin.cpp).
#include "chardraw/ui.h"
#include "chardraw/views.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{

// The plugin's settings, in its order (plugin.cpp's SettingId).
enum Setting
{
    fftSize,
    hopSize,
    window,
    channel,
    low,
    high,
    floorLevel,
    ceiling,
    frequencyScale,
    palette,
    scrollRate,
    piano,
    snap,
    cursor,
    transform,
    cqtBands,
    tuning,
    cqtLow,
    cqtHigh,
    cqtRate,
    drawer,
    cqtReading,
    labels,
    clickTone,
    settingCount
};
const char* const settingNames[settingCount] = { "FFT size", "Hop size", "Window", "Channel", "Low frequency",
    "High frequency", "Floor", "Ceiling", "Scale", "Colour map", "Scroll rate", "Piano", "Snap", "Cursor", "Transform",
    "CQT bands per octave", "A4 tuning", "CQT low frequency", "CQT high frequency", "CQT update rate",
    "Settings drawer", "CQT reading", "Labels", "Click tone" };

// The CQT runs only from 44.1 to 96 kHz (plugin.cpp's cqtSupported).
bool cqtSupported (float sampleRate)
{
    return sampleRate >= 44100 && sampleRate <= 96000;
}

bool blackKey (int note)
{
    const int n = (note % 12 + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// A note's name and octave, and how far off it the frequency is, with A4 at
// a4 Hz: "A4 +0¢".
void pitchName (float hz, double a4, char* out, size_t size)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double pitch = 69 + 12 * std::log2 (hz / a4);
    const int note = int (std::lround (pitch)), cents = int (std::lround ((pitch - note) * 100));
    std::snprintf (out, size, "%s%d %s%d\xc2\xa2", names[(note % 12 + 12) % 12], int (std::floor (note / 12.0)) - 1,
        cents < 0 ? "\xe2\x88\x92" : "+", std::abs (cents)); // U+2212 minus, U+00A2 cent sign
}

class SpectrogramView : public gpu::View
{
public:
    void prepare (const wgpu::Device& device, wgpu::TextureFormat format) override
    {
        ui.draw.prepare (device, format, gpu::loadAsset ("/fonts/IBMPlexMono-Medium.ttf"));
        ui.theme.textSize = 12;
        params.send = send;
        ui.lockPointer = lockPointer;
        ui.copyText = copyText;
        ui.resizeTo = resizeTo;
        ui.setCursor = setCursor;
        ui.menuOpacity = 0.9f; // the plot shows faintly through its menu
        send (core::Value::Map { { "type", "ready" } });
    }

    void receive (const core::Value& message) override
    {
        if (params.receive (message) || message["type"].text() != "column")
            return;
        auto* db = std::get_if<core::Value::Array> (&message["db"].data);
        const float rate = float (message["rate"].number (48000));
        if (!db || db->empty())
            return;
        binRate = rate;
        // The CQT's columns bring their bands' centres and its delay; the FFT's
        // bins are evenly spaced.
        auto* centres = std::get_if<core::Value::Array> (&message["hz"].data);
        cqtShown = centres && centres->size() == db->size();
        cqtFellBack = !cqtShown && message["fallback"].number (0) >= 0.5;
        delay = float (message["delay"].number (0));
        if (paused)
            return; // the analysis runs on; the picture holds still
        if (cqtShown)
        {
            hz.resize (centres->size());
            for (size_t k = 0; k < hz.size(); ++k)
                hz[k] = float ((*centres)[k].number (0));
            hzRate = 0;
        }
        else if (hz.size() != db->size()
                 || rate != hzRate) // bin k is at k * rate / FFT size; the FFT size is twice the bins
        {
            hz.resize (db->size());
            for (size_t k = 0; k < hz.size(); ++k)
                hz[k] = float (k) * rate / float (2 * hz.size());
            hzRate = rate;
        }
        column.resize (db->size());
        for (size_t k = 0; k < column.size(); ++k)
            column[k] = float ((*db)[k].number (-160));
        spectrogram.add (column.data(), hz.data(), column.size(), float (message["seconds"].number (0)));
    }

    void pointer (const PointerEvent& event) override
    {
        if (event.type == Pointer::leave)
            hovering = false;
        else if (event.type != Pointer::wheel)
            hovering = true, pointerX = event.x, pointerY = event.y;
        modifiers = event.modifiers;
        ui.pointer (event);
    }
    bool key (Key key, unsigned mods) override
    {
        return ui.key (key, mods);
    }
    bool text (const char* utf8) override
    {
        return ui.text (utf8);
    }
    bool animating() const override
    {
        return ui.animating();
    }
    const gpu::Accessibility* accessibility() const override
    {
        return ui.accessibility();
    }
    void accessAction (uint64_t id, gpu::Accessible::Action action, double value) override
    {
        ui.accessAction (id, action, value);
    }

    // The frame is built in record, so the presenter can skip one that looks
    // like the last; draw only flushes it.
    void draw (const wgpu::RenderPassEncoder& pass, float, float, float) override
    {
        ui.flush (pass);
    }
    bool record (float width, float height, float scale) override
    {
        for (int i = 0; i < settingCount; ++i)
            if (!(p[i] = params.find (settingNames[i])))
                return false; // until the plugin has sent its settings
        // The plugin keeps what was chosen; shown here, the hop stops at the
        // FFT size and the ranges at Nyquist (the CQT's at 0.4 x the sample
        // rate), as the analysis has them.
        p[hopSize]->max = double (1 << int (std::lround (p[fftSize]->value)));
        p[high]->max = std::max (p[high]->min, double (binRate) / 2);
        p[cqtHigh]->max = std::max (p[cqtHigh]->min, double (binRate) * 0.4);
        // The floor up to -20 dB, and 10 dB under the ceiling at least.
        p[floorLevel]->max = std::max (p[floorLevel]->min, std::min (-20.0, p[ceiling]->value - 10));
        for (Setting s : { hopSize, high, cqtHigh, floorLevel })
            p[s]->value = std::min (p[s]->value, p[s]->max);

        gpu::Spectrogram::Options o = spectrogram.settings();
        o.minFrequency = float (p[low]->value);
        o.maxFrequency = float (p[high]->value);
        o.minFrequency =
            std::min (o.minFrequency, o.maxFrequency / 2); // an octave at least, with the upper as low as 100 Hz
        if (cqtShown && !hz.empty())                       // no further than its bands go
        {
            o.minFrequency = std::clamp (o.minFrequency, hz.front(), hz.back());
            o.maxFrequency = std::clamp (o.maxFrequency, o.minFrequency, hz.back());
        }
        o.minDecibels = float (p[floorLevel]->value);
        o.maxDecibels = float (p[ceiling]->value);
        o.scale = gpu::Spectrogram::Scale (std::clamp (int (std::lround (p[frequencyScale]->value)), 0, 1));
        o.colours = gpu::ColourScheme::preset (int (std::lround (p[palette]->value)));
        // A new scroll rate starts the history again, so paused it waits for
        // the resume, which starts it again anyway.
        if (!paused)
            o.pixelsPerSecond = float (p[scrollRate]->value);
        // A column a point of time: as many as the window is wide, and some.
        o.columns = int (std::ceil ((width / scale + 64) / 128) * 128);
        o.rows = std::clamp (int (std::lround (height / 64)) * 64, 128, 1024);
        spectrogram.configure (o); // cheap when nothing changed: it only redraws when its picture would

        ui.begin (width, height, scale);
        const Clay_ElementId plot = ui.id ("Plot"), panel = ui.id ("Settings");
        const Clay_ElementId tab = ui.id ("DrawerHeader", panel.id); // the drawer's tab, as Ui names it
        // The settings drawer comes down over the plot from its top edge, see-
        // through so the spectrogram shows under it.
        const Clay_ElementId badge = ui.id ("Paused");
        // The plot is a stop in the Tab order, first, so the keys reach its
        // menu: the Menu key or Shift+F10 opens it.
        plotFocused = ui.tabStop (plot, gpu::Accessible::Role::image, "Spectrogram");
        CLAY ({ .id = plot,
            .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) } },
            .custom = { .customData = ui.paint ([this, plot, panel, tab, badge] (gpu::Rect r) {
                spectrogram.draw (ui, "Spectrogram", r);
                if (p[piano]->value >= 0.5f)
                    drawPiano (r);
                else if (heldNote >= 0)
                    playNote (-1); // the strip went away with a key held
                if (p[labels]->value >= 0.5f)
                    drawScales (r);
                if (p[clickTone]->value >= 0.5f)
                    clickPlot (r, panel, tab, badge);
                else if (heldHz > 0)
                    playHz (0);
                // A key held: its guide. Else the cursor readout, a click's
                // tone shown by it (its line in the accent), so it stays put.
                const bool readout = p[cursor]->value >= 0.5f
                                     && (heldHz > 0
                                         || (hovering && ui.pointerOver (plot) && !ui.pointerOver (panel)
                                             && !ui.pointerOver (tab) && !ui.pointerOver (badge)));
                if (heldNote >= 0 || (heldHz > 0 && !readout))
                    drawGuide (r);
                else if (readout)
                    drawCursor (r);
                if (plotFocused)
                    ui.draw.rect (r.x, r.y, r.w, r.h, 0, { 0, 0, 0, 0 }, 2 * ui.s, ui.theme.accent); // its focus ring
            }) } })
        {
            // Paused (from the plot's menu): a button in the top right corner,
            // left of the drawer's tab (below its panel while it's open), that
            // resumes it.
            if (paused)
            {
                const float below = p[drawer]->value >= 0.5 ? ui.bounds (panel).h : 0;
                CLAY ({ .id = badge,
                    .layout = { .sizing = { CLAY_SIZING_FIT (0), CLAY_SIZING_FIT (0) } },
                    .floating = { .offset = { below > 0 ? -8.0f : -8 - 36 - 8, 8 + below },
                        .attachPoints = { CLAY_ATTACH_POINT_RIGHT_TOP, CLAY_ATTACH_POINT_RIGHT_TOP },
                        .attachTo = CLAY_ATTACH_TO_PARENT } })
                {
                    if (ui.button ("Paused"))
                        setPaused (false);
                    ui.tooltip ("Resume: the display starts again");
                }
            }
            // The drawer comes down over the plot right of the piano strip,
            // declared in a box that leaves the strip out.
            CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) },
                        .padding = { uint16_t (stripWidth() / ui.s), 0, 0, 0 } } })
            {
                CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) } } })
                {
                    settingsDrawer (width / scale);
                }
            }
        }
        plotMenu (plot);
        ui.resizeGrip (width, height);
        return ui.record();
    }

private:
    // Paused, the picture holds still while the analysis runs on (columns
    // that arrive are dropped), and the settings that only change how it
    // looks still apply to it; resuming starts a new history.
    void setPaused (bool on)
    {
        if (on == paused)
            return;
        paused = on;
        if (!paused)
            spectrogram.clear();
        ui.announce (paused ? "Display paused" : "Display resumed");
    }

    // The plot's context menu: what's shown over it, checked when on, which
    // choosing switches; the colours and the scale, each a submenu with the
    // one in use checked; and Pause (or Resume).
    void plotMenu (Clay_ElementId plot)
    {
        using Item = gpu::Ui::MenuItem;
        static const Setting switches[] = { piano, clickTone, snap, cursor, labels };
        static const char* const names[] = { "Piano strip", "Click tone", "Snap", "Cursor readout", "Labels" };
        static const char* const scaleNames[] = { "Linear", "Log" };
        constexpr int count = int (std::size (switches)), schemes = gpu::ColourScheme::presetCount;
        const int scheme = std::clamp (int (std::lround (p[palette]->value)), 0, schemes - 1);
        const int scale = std::clamp (int (std::lround (p[frequencyScale]->value)), 0, 1);
        // Ids: a switch its index, a scheme 100 on, a scale 200 on, Pause 300.
        Item colours[schemes], scales[2], items[count + 5];
        for (int i = 0; i < schemes; ++i)
            colours[i] = { gpu::ColourScheme::presets[i].name, 100 + i, i == scheme };
        for (int i = 0; i < 2; ++i)
            scales[i] = { scaleNames[i], 200 + i, i == scale };
        for (int i = 0; i < count; ++i)
            items[i] = { names[i], i, p[switches[i]]->value >= 0.5 };
        items[count] = {};
        items[count + 1] = { "Colours", -1, -1, colours, schemes };
        items[count + 2] = { "Scale", -1, -1, scales, 2 };
        items[count + 3] = {};
        items[count + 4] = { paused ? "Resume" : "Pause", 300 };
        const int chosen = ui.contextMenu (plot, items, count + 5);
        auto set = [this] (gpu::Param& q, double value) { params.begin (q), params.change (q, value), params.end (q); };
        if (chosen >= 0 && chosen < count)
            set (*p[switches[chosen]], p[switches[chosen]]->value >= 0.5 ? 0 : 1);
        else if (chosen >= 100 && chosen < 100 + schemes)
            set (*p[palette], double (chosen - 100));
        else if (chosen >= 200 && chosen < 202)
            set (*p[frequencyScale], double (chosen - 200));
        else if (chosen == 300)
            setPaused (!paused);
    }

    // The settings drawer, down from the plot's top edge (see record), as
    // big as what's in it, which keeps whether it's open in the plugin's
    // state. In it the settings in columns, a group to each; the CQT's only
    // while it's the transform. Each has a context menu to reset it.
    void settingsDrawer (float widthPoints)
    {
        bool open = p[drawer]->value >= 0.5;
        float size = 0; // (fitted: unused)
        ui.drawer ("Settings", open, size, [&] { readouts (widthPoints); },
            { .edge = gpu::DrawerOptions::Edge::top,
                .overlay = true,
                .opacity = 0.75f,
                .tab = 18,
                .fit = true,
                .align = gpu::DrawerOptions::Align::end,
                .quietTab = true }); // tab and panel in the top right corner
        auto keep = [this] (gpu::Param& q, double value) {
            if (value == q.value)
                return;
            params.begin (q), params.change (q, value), params.end (q);
        };
        keep (*p[drawer], open ? 1 : 0);
    }

    void readouts (float widthPoints)
    {
        static const char* const transforms[] = { "FFT", "CQT" };
        static const char* const fftSizes[] = { "256", "512", "1024", "2048", "4096", "8192", "16384" };
        static const char* const windows[] = { "Rect", "Hann", "Hamming", "Blackman" };
        static const char* const channels[] = { "Mix", "Left", "Right" };
        static const char* const bands[] = { "12", "24", "36" };
        static const char* const rates[] = { "15/s", "30/s", "60/s" };
        static const char* const menu[] = { "Reset to Default" };
        // Each: its setting, label, and a format (with log dragging) or items.
        struct Readout
        {
            Setting setting;
            const char *label, *format;
            bool log;
            const char* const* items;
            int count;
        };
        static const char* const readings[] = { "Mean", "Peak", "Centre" };
        static const Readout analysis[] = {
            { transform, "Transform", nullptr, false, transforms, 2 },
            { fftSize, "FFT", nullptr, false, fftSizes, 7 },
            { hopSize, "Hop", "%.0f", true },
            { window, "Window", nullptr, false, windows, 4 },
            { channel, "Channel", nullptr, false, channels, 3 },
        };
        static const Readout analysisCqt[] = {
            // the FFT's own settings left out
            { transform, "Transform", nullptr, false, transforms, 2 },
            { channel, "Channel", nullptr, false, channels, 3 },
        };
        static const Readout cqt[] = {
            { cqtBands, "Bands/oct", nullptr, false, bands, 3 },
            { tuning, "A4", "%.1f Hz" },
            { cqtLow, "CQT low", "%.0f Hz", true },
            { cqtHigh, "CQT high", "%.0f Hz", true },
            { cqtRate, "Updates", nullptr, false, rates, 3 },
            { cqtReading, "Reading", nullptr, false, readings, 3 },
        };
        static const Readout display[] = {
            { low, "Low", "%.0f Hz" },
            { high, "High", "%.0f Hz", true },
            { floorLevel, "Floor", "%.0f dB" },
            { ceiling, "Ceiling", "%.0f dB" },
            { scrollRate, "Scroll", "%.0f px/s" },
        };
        // A column a group, side by side across the window: one a row, its
        // label left and value right (a list's a readout, a number's a
        // number box), rows 24 points apart (each readout as
        // tall: WCAG 2.5.8) under the group's heading.
        const bool wantsCqt = p[transform]->value >= 0.5, runsCqt = wantsCqt && cqtSupported (binRate);
        const int columns = wantsCqt ? 3 : 2;
        const float cell =
            std::clamp (std::floor ((widthPoints - 12 - 16 * float (columns - 1)) / float (columns)), 110.0f, 220.0f);
        auto group = [&] (const char* title, const Readout* all, int count) {
            CLAY ({ .layout = { .padding = { 4, 0, 2, 2 } } })
            {
                CLAY_TEXT (ui.layout.text (title),
                    CLAY_TEXT_CONFIG ({ .textColor = gpu::clayColor (ui.theme.dim), .fontSize = 11 }));
            }
            for (int i = 0; i < count; ++i)
            {
                const Readout& r = all[i];
                gpu::Param* q = p[r.setting];
                if (r.items)
                    ui.readout (params, q, r.label, r.items, r.count, cell);
                else // its label, and a number box: drag it, or type a value
                    CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIXED (cell), CLAY_SIZING_FIXED (24) },
                                .padding = { 4, 0, 0, 0 },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } })
                    {
                        CLAY_TEXT (
                            ui.layout.text (r.label), CLAY_TEXT_CONFIG ({ .textColor = gpu::clayColor (ui.theme.dim),
                                                          .fontSize = uint16_t (ui.theme.textSize) }));
                        CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIT (0) } } }) {}
                        // The hop the plugin uses is a 480th of the sample rate
                        // at least (plugin.cpp): when that's more, it shows too.
                        char raised[32];
                        const int used =
                            std::min (std::max (int (std::lround (q->value)), int (std::ceil (binRate / 480))),
                                int (std::lround (q->max)));
                        const bool more = r.setting == hopSize && used > int (std::lround (q->value));
                        if (more)
                            std::snprintf (
                                raised, sizeof raised, "%%.0f \xe2\x86\x92 %d", used); // U+2192 rightwards arrow
                        ui.numberBox (params, q, more ? raised : r.format, r.log, 84);
                        if (more)
                            ui.tooltip ("At this sample rate the hop is a 480th of it at least");
                    }
                if (ui.contextMenu (menu, 1) == 0)
                    params.begin (*q), params.change (*q, q->defaultValue), params.end (*q);
            }
        };
        const auto stack = Clay_LayoutConfig { .sizing = { CLAY_SIZING_FIXED (cell), CLAY_SIZING_FIT (0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM };
        CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIT (0) }, .childGap = 16 } })
        {
            // CQT chosen: the CQT's settings in place of the FFT's, unless the
            // sample rate leaves the FFT running.
            CLAY ({ .layout = stack })
            {
                if (runsCqt)
                    group ("Analysis", analysisCqt, int (std::size (analysisCqt)));
                else
                    group ("Analysis", analysis, int (std::size (analysis)));
            }
            if (wantsCqt)
                CLAY ({ .layout = stack })
                {
                    group ("CQT", cqt, int (std::size (cqt)));
                    // What it costs in time, or why it isn't running.
                    char text[96];
                    // The plugin says when it fell back; until the first column
                    // either way (switched, paused, no audio yet) it's starting.
                    if (!runsCqt)
                        std::snprintf (text, sizeof text, "Needs 44.1-96 kHz: FFT shown");
                    else if (cqtFellBack)
                        std::snprintf (text, sizeof text, "No bands from these: FFT shown");
                    else if (!cqtShown)
                        std::snprintf (text, sizeof text, "Starting");
                    else
                        std::snprintf (text, sizeof text, "Delay %.0f ms", delay * 1000);
                    // (Text the page shows reaches screen readers by itself.)
                    CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIXED (24) },
                                .padding = { 4, 4, 0, 0 },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } })
                    {
                        CLAY_TEXT (
                            ui.layout.text (text), CLAY_TEXT_CONFIG ({ .textColor = gpu::clayColor (ui.theme.text),
                                                       .fontSize = uint16_t (ui.theme.textSize) }));
                    }
                }
            CLAY ({ .layout = stack })
            {
                group ("Display", display, int (std::size (display)));
            }
        }
    }

    // Text on whole pixels, which keeps it sharp; y is the baseline, size in points.
    void label (
        float x, float y, std::string_view text, gpu::Color color, gpu::Draw2D::Align align = gpu::Draw2D::Align::left)
    {
        ui.draw.text (std::round (x), std::round (y), 11 * ui.s, text, color, align);
    }
    // A label on a backing, so it reads over any palette.
    void tag (float x, float y, std::string_view text, gpu::Draw2D::Align align)
    {
        const float s = ui.s, w = ui.draw.measure (11 * s, text);
        const float left = align == gpu::Draw2D::Align::left    ? x
                           : align == gpu::Draw2D::Align::right ? x - w
                                                                : x - w / 2;
        const gpu::Color& b = ui.theme.background;
        ui.draw.rect (left - 3 * s, y - 10 * s, w + 6 * s, 14 * s, 2 * s, { b.r, b.g, b.b, 0.7f });
        label (x, y, text, ui.theme.text, align);
    }
    float stripWidth() const
    {
        return p[piano]->value >= 0.5f ? 12 * ui.s : 0;
    }

    // A piano's keys up the left edge, each at its pitch on the frequency
    // scale with A4 at the tuning set. Hold a key
    // to hear it, drag to another to change note, let go to stop: the held
    // key is lit and drawGuide marks its pitch. To screen readers a picture
    // (its keys are for the pointer only).
    void drawPiano (gpu::Rect r)
    {
        const float s = ui.s, w = stripWidth();
        const double a4 = p[tuning]->value;
        const float top = spectrogram.settings().maxFrequency,
                    bottom = std::max (spectrogram.settings().minFrequency, 8.0f);
        auto y = [&] (double note) { return spectrogram.yAt (r, float (a4 * std::pow (2.0, (note - 69) / 12))); };
        const int first = int (std::floor (69 + 12 * std::log2 (bottom / a4))) - 1,
                  last = int (std::ceil (69 + 12 * std::log2 (top / a4))) + 1;

        // A press and release within one frame (a quick tap, or a slow
        // host) plays the key for that frame, stopped from the next, which
        // it asks for: as ui.piano sends a tap's note-off.
        const gpu::Ui::Area keys = ui.area (&heldNote, r.x, r.y, w, r.h, gpu::Ui::eachPress);
        if (keys.held && (!keys.released || keys.pressed))
        {
            if (keys.released)
                ui.animate();
            // The key under the pointer: the nearest note, but beside a black
            // key (past its 0.6 of the width) the white key on that side of its middle.
            const float at = std::clamp (keys.y, r.y, r.y + r.h - 1);
            const double pitch = 69 + 12 * std::log2 (spectrogram.frequencyAt (r, at) / a4);
            int note = int (std::lround (pitch));
            if (blackKey (note) && keys.x > r.x + w * 0.6f)
                note += pitch < note ? -1 : 1;
            playNote (note);
        }
        else if (heldNote >= 0)
            playNote (-1);

        ui.draw.pushClip (r.x, r.y, w, r.h);
        ui.draw.rect (r.x, r.y, w, r.h, 0, ui.theme.whiteKey);
        const gpu::Color line { ui.theme.blackKey.r, ui.theme.blackKey.g, ui.theme.blackKey.b, 0.35f };
        for (int note = first; note <= last; ++note)
        {
            if (blackKey (note))
            {
                const float y0 = y (note + 0.5), y1 = y (note - 0.5);
                ui.draw.rect (r.x, y0, w * 0.6f, std::max (1.0f, y1 - y0), 0, ui.theme.blackKey);
            }
            else if (!blackKey (note + 1)
                     && y (note) - y (note + 1) > 3 * s) // where two white keys meet (E to F, B to C)
                ui.draw.line (r.x, y (note + 0.5), r.x + w, y (note + 0.5), 1 * s, line);
        }
        if (heldNote >= 0)
        {
            const float y0 = y (heldNote + 0.5), y1 = y (heldNote - 0.5);
            ui.draw.rect (r.x, y0, blackKey (heldNote) ? w * 0.6f : w, std::max (2 * s, y1 - y0), 0, ui.theme.accent);
        }
        ui.draw.popClip();
        ui.draw.line (r.x + w, r.y, r.x + w, r.y + r.h, 1 * s, line);
        ui.accessible (gpu::Accessible::Role::image, "Piano strip", nullptr, { r.x, r.y, w, r.h },
            "Piano keys up the side, each at its pitch on the frequency scale");
    }

    // Holds a piano key (-1: none) or a pitch clicked on the plot (0: none),
    // and tells the plugin the pitch to play, 0 for none. A key held wins.
    void playNote (int note)
    {
        if (note == heldNote)
            return;
        heldNote = note;
        sendTone();
    }
    void playHz (double freq)
    {
        if (freq == heldHz)
            return;
        heldHz = freq;
        sendTone();
    }
    double toneHz() const
    {
        return heldNote >= 0 ? p[tuning]->value * std::pow (2.0, (heldNote - 69) / 12.0) : heldHz;
    }
    void sendTone()
    {
        const double tone = toneHz();
        if (tone == sentHz)
            return;
        sentHz = tone;
        send (core::Value::Map { { "type", "tone" }, { "hz", tone } });
    }

    // Click tone: held on the plot (right of the piano strip, off the
    // drawer), the pitch under the pointer plays, snapped to a peak as
    // the cursor is; dragging changes it.
    void clickPlot (gpu::Rect r, Clay_ElementId panel, Clay_ElementId tab, Clay_ElementId badge)
    {
        const float x0 = r.x + stripWidth();
        const gpu::Ui::Area click =
            ui.area (&heldHz, x0, r.y, std::max (0.0f, r.x + r.w - x0), r.h, gpu::Ui::eachPress);
        if (click.pressed)
            clickOnDrawer =
                ui.pointerOver (panel) || ui.pointerOver (tab) || ui.pointerOver (badge); // theirs, not the plot's
        if (!click.held || click.released || heldNote >= 0 || clickOnDrawer)
        {
            if (heldHz > 0)
                playHz (0);
            return;
        }
        gpu::Spectrogram::Probe at;
        const bool snapping = p[snap]->value >= 0.5f && !(modifiers & gpu::View::command);
        const float y = std::clamp (click.y, r.y, r.y + r.h - 1);
        playHz (spectrogram.probe (std::clamp (click.x, x0, r.x + r.w - 1), y, snapping, at)
                    ? at.frequency
                    : spectrogram.frequencyAt (r, y));
    }

    // A line across the plot at the pitch playing (a held key's or a click's), and its name.
    void drawGuide (gpu::Rect r)
    {
        const double tone = toneHz();
        const float y = spectrogram.yAt (r, float (tone)), s = ui.s;
        if (y < r.y || y > r.y + r.h)
            return;
        ui.draw.line (r.x + stripWidth(), y, r.x + r.w, y, 1.5f * s, ui.theme.accent);
        char pitch[32], text[48];
        pitchName (float (tone), p[tuning]->value, pitch, sizeof pitch);
        std::snprintf (text, sizeof text, "%s  %.1f Hz", pitch, tone);
        tag (r.x + r.w - 8 * s, y > r.y + r.h - 40 * s ? y - 6 * s : y + 16 * s, text,
            gpu::Draw2D::Align::right); // below it, above it near the bottom
    }

    // Frequencies up the left edge and seconds along the bottom, over the plot.
    void drawScales (gpu::Rect r)
    {
        const float s = ui.s, x0 = r.x + stripWidth();
        const gpu::Color tick { ui.theme.text.r, ui.theme.text.g, ui.theme.text.b, 0.7f };
        // Round frequencies, 28 points apart at least, more as the range narrows.
        const std::vector<float> marks = spectrogram.frequencyMarks (r, 28 * s);
        float lastY = 1e9f;
        for (float f : marks)
        {
            const float y = spectrogram.yAt (r, f);
            if (y > r.y + r.h - 22 * s || y < r.y + 8 * s || lastY - y < 18 * s)
                continue; // clear of the edges (and each other)
            char text[16];
            if (f >= 1000)
                std::snprintf (text, sizeof text, "%gk", f / 1000);
            else
                std::snprintf (text, sizeof text, "%g", f);
            ui.draw.line (x0, y, x0 + 5 * s, y, 1 * s, tick);
            tag (x0 + 9 * s, y + 4 * s, text, gpu::Draw2D::Align::left);
            lastY = y;
        }
        // Seconds back from the newest, at a round step at least 60 points apart.
        const float rate = spectrogram.settings().pixelsPerSecond;
        if (rate <= 0)
            return;
        float step = 0.25f;
        for (float candidate : { 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f })
            if ((step = candidate) * rate >= 60)
                break;
        const float right = r.x + r.w, bottom = r.y + r.h;
        for (int i = 1;; ++i)
        {
            const float x = right - i * step * rate * s;
            if (x < x0 + 48 * s)
                break;
            char text[24];
            std::snprintf (text, sizeof text, "\xe2\x88\x92%g s", i * step);
            ui.draw.line (x, bottom, x, bottom - 5 * s, 1 * s, tick);
            tag (x, bottom - 9 * s, text, gpu::Draw2D::Align::center);
        }
    }

    // What's under the pointer: a line at its frequency, a dot on a peak it
    // snapped to, and a label: "440.0 Hz  A4 +0¢  -12.3 dB".
    void drawCursor (gpu::Rect r)
    {
        gpu::Spectrogram::Probe at;
        const bool snapping = p[snap]->value >= 0.5f && !(modifiers & gpu::View::command); // Command bypasses it
        if (pointerX < r.x + stripWidth() || !spectrogram.probe (pointerX, pointerY, snapping, at))
            return;
        const float s = ui.s;
        const gpu::Color guide { ui.theme.text.r, ui.theme.text.g, ui.theme.text.b, 0.45f };
        if (heldHz > 0)
            ui.draw.line (
                r.x + stripWidth(), at.y, r.x + r.w, at.y, 1.5f * s, ui.theme.accent); // sounding (Click tone)
        else
            ui.draw.line (r.x + stripWidth(), at.y, r.x + r.w, at.y, 1 * s, guide);
        if (at.peak)
            ui.draw.rect (pointerX - 3 * s, at.y - 3 * s, 6 * s, 6 * s, 3 * s, ui.theme.accent);
        char pitch[32], text[80];
        pitchName (at.frequency, p[tuning]->value, pitch, sizeof pitch);
        std::snprintf (text, sizeof text, at.frequency < 10000 ? "%.1f Hz  %s  %.1f dB" : "%.0f Hz  %s  %.1f dB",
            at.frequency, pitch, at.decibels);
        std::string shown = text; // a minus sign, not a hyphen, before the level
        if (const size_t minus = shown.rfind ("  -"); minus != std::string::npos)
            shown.replace (minus + 2, 1, "\xe2\x88\x92");
        const float w = ui.draw.measure (11 * s, shown);
        float x = pointerX + 14 * s, y = at.y - 8 * s;
        if (x + w + 4 * s > r.x + r.w)
            x = pointerX - 14 * s - w; // flipped to stay inside
        if (y - 12 * s < r.y)
            y = at.y + 18 * s;
        tag (x, y, shown, gpu::Draw2D::Align::left);
    }

    gpu::Spectrogram spectrogram;
    std::vector<float> hz, column;     // the bins' frequencies, and the column being added
    float binRate = 48000, hzRate = 0; // the sample rate, and the one hz was made for (0: the CQT's bands)
    bool cqtShown = false;             // the last column was the CQT's
    bool cqtFellBack = false;          // or the FFT's, in place of the CQT asked for (it made no bands)
    float delay = 0;                   // its delay, seconds
    bool paused = false;
    bool plotFocused = false;      // from the keys: its focus ring shows
    int heldNote = -1;             // the piano key held, or -1
    double heldHz = 0, sentHz = 0; // the pitch held on the plot (Click tone), and the last sent to the plugin
    bool clickOnDrawer = false;    // the press held began on the drawer, so it plays nothing
    gpu::Params params;            // the plugin's settings (not CLAP parameters; see plugin.cpp)
    gpu::Param* p[settingCount] = {};
    gpu::Ui ui;
    bool hovering = false;
    float pointerX = 0, pointerY = 0; // pixels
    unsigned modifiers = 0;
};

} // namespace

std::unique_ptr<gpu::View> gpu::createView()
{
    return std::make_unique<SpectrogramView>();
}
