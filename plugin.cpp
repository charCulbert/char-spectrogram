// Laid out as nakst's CLAP tutorial: https://nakst.gitlab.io/tutorial/clap-part-1.html
//
// char-spectrogram: passes stereo audio through and shows a scrolling
// spectrogram of it, drawn with libs/chardraw's views
// (ui/SpectrogramView.cpp): no drawing code of its own. Holding a key of its
// piano strip mixes a quiet sine at that pitch into the plugin's output (a
// plugin has no audio device of its own), which the spectrogram never sees.
//
// This file is the whole CLAP plugin: its descriptor, audio ports, state, the
// clap.webview and clap.gui extensions its interface uses, and the audio
// processing; entry.cpp is the entry point that makes it. libs/ holds the
// reusable parts: the message codec and file loader (core/), and the presenter
// that shows a gpu::View in the host's window (gpu/).
//
// Its settings (the spectrogram's, the key tone, and the interface's
// settings drawer) change only the analysis and what the interface shows and
// plays, so they are saved with the plugin's state, not CLAP parameters:
// hosts don't list or automate them. The interface edits them with the same
// messages it would use for parameters.
#include "clap/clap.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/state.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"
#include "clap/ext/draft/webview.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string.h>
#include <string>
#include <vector>
#include "SpectrogramAnalysis.h"
#include "core/messages.h"
#include "core/resources.h"
#include "gpu/gui.h"

// Settings: {name, min, max, default}.
// Their index is their id in messages; the name is their key in the state.
struct Setting
{
    const char* name;
    double min, max, defaultValue;
};
enum SettingId
{
    // The analysis (SpectrogramAnalysis.h), which the plugin does.
    S_FFT_SIZE, // log2 of the FFT size: 8 to 14 is 256 to 16384 samples
    S_HOP_SIZE, // samples, up to the FFT size
    S_WINDOW,   // chardsp::SpectrumWindow: rectangular, Hann, Hamming, Blackman
    S_CHANNEL,  // SpectrogramAnalysis::Channel: mix, left, right
    // How it's shown, which only the interface uses.
    S_LOW,
    S_HIGH, // the frequency range, Hz (the interface stops at Nyquist); the upper from 100 Hz, to zoom in on the bass
    S_FLOOR,
    S_CEILING,     // the level range, dB
    S_SCALE,       // gpu::Spectrogram::Scale: linear, log
    S_PALETTE,     // the colour map: gpu::ColourScheme::presets, 0 magma, 1 viridis
    S_SCROLL_RATE, // points a second, whatever the hop
    S_PIANO,
    S_SNAP,
    S_CURSOR, // on or off: the piano strip, snapping to peaks, and the readout under the pointer
    // The transform, and the constant-Q settings (chardsp::ConstantQSpectrogram).
    S_TRANSFORM, // 0 FFT, 1 CQT (from 44.1 to 96 kHz; the FFT elsewhere)
    S_CQT_BANDS, // bands per octave: 0, 1, 2 for 12, 24, 36
    S_TUNING,    // A4, Hz: the CQT's bands, and the piano strip's keys and their tone
    S_CQT_LOW,
    S_CQT_HIGH,    // the CQT's frequency limits, Hz (the interface stops the upper at 0.4 x the sample rate)
    S_CQT_RATE,    // columns a second: 0, 1, 2 for 15, 30, 60
    S_DRAWER,      // the interface's settings drawer: open or closed
    S_CQT_READING, // how a band's level over a hop becomes one value: 0 mean, 1 peak, 2 centre
    S_LABELS,      // on or off: the frequency and time scales over the plot
    S_CLICK_TONE,  // on or off: holding the pointer on the plot plays the pitch under it
    S_COUNT
};
static const Setting settings[S_COUNT] = {
    { "FFT size", 8, 14, 11 },
    { "Hop size", 128, 16384, 512 }, // and at least a 480th of the sample rate: see on_main_thread
    { "Window", 0, 3, 1 },
    { "Channel", 0, 2, 0 },
    { "Low frequency", 0, 500, 20 },
    { "High frequency", 100, 96000, 24000 },
    { "Floor", -160, -20, -100 }, // the interface keeps it 10 dB under the ceiling
    { "Ceiling", -20, 12, 0 },
    { "Scale", 0, 1, 1 },      // an earlier state's 2 (mel, since removed) loads as log
    { "Colour map", 0, 6, 0 }, // a new name: an earlier state's "Palette" (five maps) is left unread
    { "Scroll rate", 25, 200, 100 },
    { "Piano", 0, 1, 1 },
    { "Snap", 0, 1, 1 }, // a new name: an earlier state's "Peak snap" is left unread
    { "Cursor", 0, 1, 1 },
    { "Transform", 0, 1, 0 },
    { "CQT bands per octave", 0, 2, 1 },
    { "A4 tuning", 400, 480, 440 },
    { "CQT low frequency", 27.5, 220, 55 }, // from A0, a piano's lowest key
    { "CQT high frequency", 1000, 38400, 16000 },
    { "CQT update rate", 0, 2, 1 },
    { "Settings drawer", 0, 1, 0 },
    { "CQT reading", 0, 2, 0 },
    { "Labels", 0, 1, 1 },
    { "Click tone", 0, 1, 1 },
};

// The CQT runs only from 44.1 to 96 kHz; elsewhere the FFT does.
static bool cqtSupported (float sampleRate)
{
    return sampleRate >= 44100 && sampleRate <= 96000;
}

// The key tone: a sine at -18 dBFS, faded in and out over two of its cycles
// (3 to 20 ms) so it starts and stops without a click.
static const float toneLevel = 0.125893f; // 10^(-18/20)
static float toneFade (float hz)
{
    return std::clamp (2 / hz, 0.003f, 0.02f);
}

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_state_t* hostState;
    float sampleRate;
    double settingValues[S_COUNT]; // main thread only

    // The analysis: the audio thread fills its buffer; the main thread sets
    // up the transform chosen, takes its columns and sends them to the
    // interface (see on_main_thread). The buffer is made once, for the
    // largest block, before anything pushes: 2 MB.
    SpectrogramAnalysis analysis;
    // Main thread: CQT settings it made no bands of, so the FFT runs until they change.
    SpectrogramAnalysis::Cqt cqtFailed;
    bool cqtFailedSet = false;
    std::atomic<bool> columnsPending;
    uint32_t framesSinceColumns = 0; // audio thread only

    // The key tone. The main thread sets toneHz, 0 for none (see PluginSetTone);
    // the audio thread reads it and keeps the rest.
    float toneHeld = 0; // main thread: the pitch the interface holds, Hz
    std::atomic<float> toneHz { 0 };
    double tonePhase = 0;                  // audio thread only: radians
    float toneGain = 0, toneFrequency = 0; // audio thread only: 0..1 of toneLevel, and the pitch sounding

    gpu::Gui gui;
    float
        guiWidth = 720,
        guiHeight =
            480; // points, so the window keeps its size across scales; 480: tall enough for every FFT setting in the drawer
    bool uiReady = false; // the interface has said "ready" and not been closed since
};

static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message);
static void PluginSendValues (MyPlugin* plugin);
static void PluginSetTone (MyPlugin* plugin);

static const char* const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_ANALYZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.char-spectrogram",
    .name = "char-spectrogram",
    .vendor = "Charlie Culbert",
    .url = "https://github.com/charCulbert",
    .manual_url = "https://github.com/charCulbert",
    .support_url = "https://github.com/charCulbert",
    .version = "0.1.0",
    .description = "A scrolling spectrogram drawn with WebGPU.",
    .features = pluginFeatures,
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return 1; },

    .get = [] (const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_audio_port_info_t* info) -> bool {
        if (index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = 0; // output may share the input's buffer
        snprintf (info->name, sizeof (info->name), "%s", isInput ? "Audio Input" : "Audio Output");
        return true;
    },
};

// The state is the settings as a CBOR map (core/messages.h) with a version, so
// later versions can add settings and still read this one (and leave
// unread the ones it no longer has: an earlier state's drawer size).
static const clap_plugin_state_t extensionState = {
    .save = [] (const clap_plugin_t* _plugin, const clap_ostream_t* stream) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        core::Value::Map state { { "version", 2 } };
        for (uint32_t i = 0; i < S_COUNT; i++)
            state[settings[i].name] = plugin->settingValues[i];
        const auto bytes = core::encode (state);
        const unsigned char* data = bytes.data();
        uint64_t remaining = bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write (stream, data, remaining);
            if (written <= 0)
                return false;
            data += written;
            remaining -= (uint64_t) written;
        }
        return true;
    },

    .load = [] (const clap_plugin_t* _plugin, const clap_istream_t* stream) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        std::vector<unsigned char> bytes;
        unsigned char buffer[256];
        for (int64_t read; (read = stream->read (stream, buffer, sizeof (buffer))) > 0;)
        {
            bytes.insert (bytes.end(), buffer, buffer + read);
            if (bytes.size() > core::maxMessageBytes)
                return false;
        }
        auto state = core::decode (bytes.data(), bytes.size());
        const double version = state ? (*state)["version"].number() : 0;
        if (version < 1)
            return false;
        for (uint32_t i = 0; i < S_COUNT; i++)
        {
            const double value = (*state)[settings[i].name].number (settings[i].defaultValue);
            plugin->settingValues[i] = value < settings[i].min   ? settings[i].min
                                       : value > settings[i].max ? settings[i].max
                                                                 : value;
        }
        PluginSendValues (plugin);
        PluginSetTone (plugin);
        return true;
    },
};

// The interface talks to the plugin through clap.webview: natively gpu::Gui
// passes its messages here, and in WCLAP the host shows the interface as a web
// page (the same view compiled to wasm), which it loads through get_uri and
// get_resource.
static const clap_plugin_webview_t extensionWebview = {
    .get_uri = [] (const clap_plugin_t* _plugin, char* uri, uint32_t capacity) -> int32_t {
        static const char start[] = "/page/index.html"; // in resources/
        if (capacity)
            snprintf (uri, capacity, "%s", start);
        return sizeof (start); // including the terminating zero
    },

    .get_resource = [] (const clap_plugin_t* _plugin, const char* path, char* mime, uint32_t mimeCapacity,
                        const clap_ostream_t* stream) -> bool {
        auto resource = core::readResource (path);
        if (!resource || resource->mime.size() >= mimeCapacity)
            return false;
        strcpy (mime, resource->mime.c_str());
        const char* bytes = resource->bytes.data();
        uint64_t remaining = resource->bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write (stream, bytes, remaining);
            if (written <= 0)
                return false;
            bytes += written;
            remaining -= (uint64_t) written;
        }
        return true;
    },

    .receive = [] (const clap_plugin_t* _plugin, const void* buffer, uint32_t size) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        auto message = core::decode (buffer, size);
        return message && PluginReceiveMessage (plugin, *message);
    },
};

// clap.gui: size policy is ours; creating and embedding the view is gpu::Gui's.
// The limits are in points; the minimum leaves room for the plot under the open settings drawer.
#define GUI_MIN_WIDTH (420)
#define GUI_MIN_HEIGHT (270)
#define GUI_MAX_WIDTH (2000)
#define GUI_MAX_HEIGHT (1200)

// Clamps a clap.gui size, which is in pixels outside macOS, to the limits.
static void clampGuiSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    const auto clamp = [scale] (uint32_t value, float low, float high) {
        return std::clamp (value, uint32_t (std::ceil (low * scale)), uint32_t (high * scale));
    };
    width = clamp (width, GUI_MIN_WIDTH, GUI_MAX_WIDTH);
    height = clamp (height, GUI_MIN_HEIGHT, GUI_MAX_HEIGHT);
}

// The size to report to clap.gui, clamped so get_size never reports a size set_size refuses.
static void guiSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    width = uint32_t (std::lround (plugin->guiWidth * scale));
    height = uint32_t (std::lround (plugin->guiHeight * scale));
    clampGuiSize (plugin, width, height);
}

static const clap_plugin_gui_t extensionGui = {
    .is_api_supported = [] (const clap_plugin_t* _plugin, const char* api, bool isFloating) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.isApiSupported (api, isFloating);
    },

    .get_preferred_api = [] (const clap_plugin_t* _plugin, const char** api, bool* isFloating) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.getPreferredApi (api, isFloating);
    },

    .create = [] (const clap_plugin_t* _plugin, const char* api, bool isFloating) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        if (!plugin->gui.create (api, isFloating))
            return false;
        uint32_t width, height;
        guiSize (plugin, width, height);
        plugin->gui.setSize (width, height);
        return true;
    },

    .destroy =
        [] (const clap_plugin_t* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            plugin->gui.destroy();
            plugin->uiReady = false;
            plugin->toneHeld = 0; // no interface holds a key now
            PluginSetTone (plugin);
        },

    .set_scale = [] (const clap_plugin_t* _plugin, double scale) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.setScale (scale);
    },

    .get_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        guiSize (plugin, *width, *height);
        return true;
    },

    .can_resize = [] (const clap_plugin_t* _plugin) -> bool { return true; },

    .get_resize_hints = [] (const clap_plugin_t* _plugin, clap_gui_resize_hints_t* hints) -> bool {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    },

    .adjust_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool {
        clampGuiSize ((MyPlugin*) _plugin->plugin_data, *width, *height);
        return true;
    },

    .set_size = [] (const clap_plugin_t* _plugin, uint32_t width, uint32_t height) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        uint32_t allowedWidth = width, allowedHeight = height;
        clampGuiSize (plugin, allowedWidth, allowedHeight);
        if (allowedWidth != width || allowedHeight != height)
            return false;
        plugin->guiWidth = width / plugin->gui.pixelsPerPoint();
        plugin->guiHeight = height / plugin->gui.pixelsPerPoint();
        plugin->gui.setSize (width, height);
        return true;
    },

    .set_parent = [] (const clap_plugin_t* _plugin, const clap_window_t* window) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.setParent (window);
    },

    .set_transient = [] (const clap_plugin_t* _plugin, const clap_window_t* window) -> bool { return false; },

    .suggest_title = [] (const clap_plugin_t* _plugin, const char* title) {},

    .show = [] (const clap_plugin_t* _plugin) -> bool { return ((MyPlugin*) _plugin->plugin_data)->gui.show(); },

    .hide = [] (const clap_plugin_t* _plugin) -> bool { return ((MyPlugin*) _plugin->plugin_data)->gui.hide(); },
};

// On Linux the host's timer drives the view's events and frames; see gpu::Gui::onTimer.
static const clap_plugin_timer_support_t extensionTimerSupport = {
    .on_timer = [] (const clap_plugin_t* _plugin,
                    clap_id timerId) { ((MyPlugin*) _plugin->plugin_data)->gui.onTimer (timerId); },
};

// Messages between the interface and the plugin (main thread only):
//   UI -> plugin  {type: "ready"}
//                 {type: "change", id, value}     a setting changed
//                 {type: "begin" | "end", id}     a drag starts or ends (unused here)
//                 {type: "tone", hz}              a piano key held at hz, or 0 when let go
//   plugin -> UI  {type: "params", params: [{id, name, min, max, default}]}  the settings
//                 {type: "values", values: [value per setting id]}
//                 {type: "column", rate, seconds, db: [FFT size / 2 bins, bin k at k * rate / FFT size Hz], fallback}
//                                                  the FFT's; fallback 1 when it runs in place of the CQT asked for
//                     seconds: the audio since the last column, so the interface scrolls in time
//                 {type: "column", rate, seconds, db, hz: [each band's centre], delay}   the CQT's
//                     delay: seconds from a sound to its column's centre (half the block)
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        core::Value::Array list;
        for (uint32_t i = 0; i < S_COUNT; i++)
            list.push_back (core::Value::Map { { "id", (double) i }, { "name", settings[i].name },
                { "min", settings[i].min }, { "max", settings[i].max }, { "default", settings[i].defaultValue } });
        plugin->gui.send (core::Value::Map { { "type", "params" }, { "params", list } });
        PluginSendValues (plugin);
        plugin->uiReady = true;
        return true;
    }
    if (type == "tone")
    {
        plugin->toneHeld = (float) message["hz"].number (0);
        PluginSetTone (plugin);
        return true;
    }

    const double id = message["id"].number (-1);
    if (!(id >= 0 && id < (int) S_COUNT))
        return false;
    if (type == "change")
    {
        const Setting& setting = settings[(int) id];
        const double value = message["value"].number (setting.defaultValue);
        plugin->settingValues[(int) id] = value < setting.min ? setting.min : value > setting.max ? setting.max : value;
        if (plugin->hostState)
            plugin->hostState->mark_dirty (plugin->host); // the project has unsaved changes
    }
    return type == "change" || type == "begin" || type == "end";
}

// Main thread: tells the audio thread the pitch to play, if any: the key or
// the plot held, and only below Nyquist (with room for the fade).
static void PluginSetTone (MyPlugin* plugin)
{
    const float hz = plugin->toneHeld;
    const bool play = hz >= 10 && hz < plugin->sampleRate * 0.45f;
    plugin->toneHz.store (play ? hz : 0, std::memory_order_relaxed);
}

static void PluginSendValues (MyPlugin* plugin)
{
    core::Value::Array values;
    for (uint32_t i = 0; i < S_COUNT; i++)
        values.push_back (plugin->settingValues[i]);
    plugin->gui.send (core::Value::Map { { "type", "values" }, { "values", values } });
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,

    .init = [] (const clap_plugin* _plugin) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        plugin->hostState = (const clap_host_state_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_STATE);
        plugin->gui.init (_plugin, plugin->host);
        for (uint32_t i = 0; i < S_COUNT; i++)
            plugin->settingValues[i] = settings[i].defaultValue;
        plugin->analysis.prepare();
        return true;
    },

    .destroy =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            plugin->gui.destroy();
            delete plugin;
        },

    .activate = [] (const clap_plugin* _plugin, double sampleRate, uint32_t minimumFramesCount,
                    uint32_t maximumFramesCount) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        plugin->sampleRate = (float) sampleRate;
        plugin->tonePhase = 0, plugin->toneGain = 0, plugin->toneFrequency = 0;
        PluginSetTone (plugin); // its limit is the sample rate's
        return true;
    },

    .deactivate = [] (const clap_plugin* _plugin) {},
    .start_processing = [] (const clap_plugin* _plugin) -> bool { return true; },
    .stop_processing = [] (const clap_plugin* _plugin) {},
    .reset = [] (const clap_plugin* _plugin) {},

    .process = [] (const clap_plugin* _plugin, const clap_process_t* process) -> clap_process_status {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        const uint32_t frameCount = process->frames_count;
        float* const* input = process->audio_inputs[0].data32;
        float* const* output = process->audio_outputs[0].data32;

        // Pass the audio through (the host may give us one buffer for both).
        for (uint32_t channel = 0; channel < 2; channel++)
            if (output[channel] != input[channel])
                memcpy (output[channel], input[channel], sizeof (float) * frameCount);

        // Hand it to the analysis, and every 1/60 s of audio ask the main
        // thread to send the columns that are ready. (Counting samples needs no clock.)
        // This is the analysis's tap: the input, taken before the key tone is
        // mixed into the output below, so the spectrogram never shows the tone
        // (when the host gives one buffer for both, it isn't in it yet).
        plugin->analysis.push (input[0], input[1], frameCount);
        plugin->framesSinceColumns += frameCount;
        if (plugin->framesSinceColumns >= plugin->sampleRate / 60)
        {
            plugin->framesSinceColumns = 0;
            if (!plugin->columnsPending.exchange (true, std::memory_order_acq_rel))
                plugin->host->request_callback (plugin->host);
        }

        // The key tone, mixed into the output: it fades in to the pitch held,
        // follows it from key to key (the phase carries on, so no click), and
        // fades out at the last pitch when let go.
        const float target = plugin->toneHz.load (std::memory_order_relaxed);
        if (target > 0 || plugin->toneGain > 0)
        {
            if (target > 0)
                plugin->toneFrequency = target;
            const float step = 1 / (toneFade (plugin->toneFrequency) * plugin->sampleRate);
            const double twoPi = 6.283185307179586;
            const double advance = twoPi * plugin->toneFrequency / plugin->sampleRate;
            for (uint32_t i = 0; i < frameCount; i++)
            {
                plugin->toneGain =
                    target > 0 ? std::min (1.0f, plugin->toneGain + step) : std::max (0.0f, plugin->toneGain - step);
                const float sample = toneLevel * plugin->toneGain * (float) std::sin (plugin->tonePhase);
                output[0][i] += sample;
                output[1][i] += sample;
                plugin->tonePhase += advance;
                if (plugin->tonePhase >= twoPi)
                    plugin->tonePhase -= twoPi;
            }
        }
        return CLAP_PROCESS_CONTINUE;
    },

    .get_extension = [] (const clap_plugin* plugin, const char* id) -> const void* {
        if (0 == strcmp (id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp (id, CLAP_EXT_STATE))
            return &extensionState;
        if (0 == strcmp (id, CLAP_EXT_WEBVIEW))
            return &extensionWebview;
        if (0 == strcmp (id, CLAP_EXT_GUI))
            return &extensionGui;
        if (0 == strcmp (id, CLAP_EXT_TIMER_SUPPORT))
            return &extensionTimerSupport;
        return nullptr;
    },

    .on_main_thread =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            // The audio thread asks for this 60 times a second; skip it while the interface is closed.
            if (!plugin->columnsPending.exchange (false, std::memory_order_acq_rel) || !plugin->uiReady)
                return;
            // The analysis runs here, on the main thread, never the audio thread:
            // new settings set up a new transform first.
            const double* v = plugin->settingValues;
            const float rate = plugin->sampleRate;
            std::vector<float> column;
            uint32_t advanced = 0;
            const auto channel = (SpectrogramAnalysis::Channel) std::clamp ((int) std::lround (v[S_CHANNEL]), 0, 2);
            if (v[S_TRANSFORM] >= 0.5 && cqtSupported (rate))
            {
                static const int bands[] = { 12, 24, 36 }, columnsPerSecond[] = { 15, 30, 60 };
                static const chardsp::ConstantQReading readings[] = { chardsp::ConstantQReading::mean,
                    chardsp::ConstantQReading::peak, chardsp::ConstantQReading::centre };
                SpectrogramAnalysis::Cqt cqt;
                cqt.design.sampleRate = rate;
                cqt.design.bandsPerOctave = bands[std::clamp ((int) std::lround (v[S_CQT_BANDS]), 0, 2)];
                cqt.design.referenceFrequency = v[S_TUNING];
                cqt.design.minFrequency = v[S_CQT_LOW];
                cqt.design.maxFrequency = std::max (cqt.design.minFrequency, std::min (v[S_CQT_HIGH], 0.4 * rate));
                cqt.hop =
                    (int) std::lround (rate / columnsPerSecond[std::clamp ((int) std::lround (v[S_CQT_RATE]), 0, 2)]);
                cqt.reading = readings[std::clamp ((int) std::lround (v[S_CQT_READING]), 0, 2)];
                cqt.channel = channel;
                // Settings it makes no bands of leave the FFT running, as outside 44.1 to 96 kHz.
                if (!(plugin->cqtFailedSet && cqt == plugin->cqtFailed))
                {
                    if (plugin->analysis.configure (cqt))
                    {
                        plugin->cqtFailedSet = false;
                        const auto& transform = plugin->analysis.cqtTransform();
                        const auto& centres = transform.getFrequencies();
                        const core::Value::Array hz (centres.begin(), centres.end());
                        const double delay = 0.5 * transform.getBlockSize() / rate;
                        // At most 8 columns a call, as the FFT's.
                        for (int sent = 0; sent < 8 && plugin->analysis.next (column, advanced); sent++)
                            plugin->gui.send (core::Value::Map { { "type", "column" }, { "rate", (double) rate },
                                { "seconds", advanced / (double) rate },
                                { "db", core::Value::Array (column.begin(), column.end()) }, { "hz", hz },
                                { "delay", delay } });
                        return;
                    }
                    plugin->cqtFailed = cqt, plugin->cqtFailedSet = true;
                }
            }
            SpectrogramAnalysis::Fft fft;
            fft.size = 1 << (int) std::lround (v[S_FFT_SIZE]);
            // At most 8 columns a call, about 60 calls a second: a hop shorter than
            // a 480th of the sample rate would only skip columns, in bursts.
            fft.hop = std::min (std::max ((int) std::lround (v[S_HOP_SIZE]), (int) std::ceil (rate / 480)), fft.size);
            fft.window = (chardsp::SpectrumWindow) std::clamp ((int) std::lround (v[S_WINDOW]), 0, 3);
            fft.channel = channel;
            plugin->analysis.configure (fft);
            // At most 8 columns a call (the analysis skips older ones), each sent
            // with how much audio it moves on: about 1.5 a call at 48 kHz with a
            // hop of 512.
            for (int sent = 0; sent < 8 && plugin->analysis.next (column, advanced); sent++)
                plugin->gui.send (core::Value::Map { { "type", "column" }, { "rate", (double) rate },
                    { "seconds", advanced / (double) rate },
                    { "db", core::Value::Array (column.begin(), column.end()) },
                    { "fallback", v[S_TRANSFORM] >= 0.5 ? 1.0 : 0.0 } }); // the CQT asked for, and not running
        },
};

// What entry.cpp needs.
const clap_plugin_descriptor_t* getPluginDescriptor()
{
    return &pluginDescriptor;
}

const clap_plugin_t* createPlugin (const clap_host_t* host)
{
    MyPlugin* plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    return &plugin->plugin;
}
