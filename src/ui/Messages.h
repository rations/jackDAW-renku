#pragma once

// BMessage `what` constants shared by the JackDAW UI. One flat namespace so
// MainWindow::MessageReceived can route everything; grouped by source.
enum {
    // Periodic UI refresh (BMessageRunner owned by MainWindow).
    MSG_UI_TICK = 'tick',

    // Transport controls (buttons, menu items, keyboard shortcuts).
    MSG_TRANSPORT_PLAY = 'play',
    MSG_TRANSPORT_STOP = 'stop',
    MSG_TRANSPORT_RECORD = 'recd',
    MSG_TRANSPORT_RTZ = 'rtz ',
    MSG_TRANSPORT_TOGGLE = 'plst',        // space bar: play <-> stop
    MSG_TRANSPORT_PAUSE = 'paus',         // ||: stop playback/recording, keep position
    MSG_TRANSPORT_STEP_BACK = 'stpb',     // |<< : nudge cursor back 10 ms (Left)
    MSG_TRANSPORT_STEP_FWD = 'stpf',      // >>| : nudge cursor forward 10 ms (Right)
    MSG_TRANSPORT_NEXT_BOUNDARY = 'nxtb', // ▶| : jump to next clip boundary
    MSG_TRANSPORT_LOOP = 'loop',          // ⟳ : toggle loop region

    // Right-click context-menu triggers, posted by transport buttons with a
    // BPoint "screen_where"; MainWindow builds and runs the popup there.
    MSG_RECORD_MENU = 'rmnu',
    MSG_METRO_MENU = 'mmnu',
    MSG_MIXER_MENU = 'xmnu',

    // Record-mode right-click menu (record button).
    MSG_RECORD_MODE_NORMAL = 'rmnr',
    MSG_RECORD_MODE_PUNCH = 'rmpn',

    // Timeline zoom (menu / keyboard).
    MSG_ZOOM_IN = 'zmin',
    MSG_ZOOM_OUT = 'zmot',

    // Region editing.
    MSG_SPLIT = 'splt',           // split focused track at the cursor
    MSG_REGION_COPY = 'rcpy',     // copy the section selection / range to the clipboard
    MSG_REGION_PASTE = 'rpst',    // paste the clipboard at the playhead
    MSG_REGION_DELETE = 'rdel',   // delete the selected sections / range
    MSG_REGION_GROUP = 'rgrp',    // merge adjacent same-clip selected sections
    MSG_REGION_SET_GAIN = 'rgan', // apply float "db" (from the gain dialog) to the selection

    // Tempo / grid / display controls (handled by TransportView).
    MSG_SET_BPM = 'sbpm',
    MSG_SET_TIMESIG = 'tsig',
    MSG_TOGGLE_METRONOME = 'metr',
    MSG_TOGGLE_GRID = 'grid',
    MSG_TOGGLE_SNAP = 'snap',
    MSG_TOGGLE_RULER_MODE = 'rulm',
    MSG_CYCLE_TIMEMODE = 'tcyc', // click position readout: cycle timecode format

    // Metronome options (Metro button right-click popup).
    MSG_METRO_VOLUME = 'mtvl',     // open Volume window
    MSG_METRO_COUNTIN = 'mtci',    // open Count-in window
    MSG_METRO_HEADPHONES = 'mthp', // toggle "headphones only" route

    // Value changes posted from the (separate-looper) metronome option windows
    // to MainWindow, which applies them on its looper (single-mutator rule).
    MSG_METRO_SET_VOLUME = 'mtsv',       // float  "db"
    MSG_METRO_SET_COUNTIN_REC = 'mtcr',  // int32  "beats"
    MSG_METRO_SET_COUNTIN_PLAY = 'mtcp', // int32 "beats"

    // Mixer toggle (dock) + right-click "Open in Window".
    MSG_MIXER_TOGGLE = 'mixt',
    MSG_MIXER_OPEN_WINDOW = 'mixw',
    MSG_MIXER_REBUILD = 'mixr', // -> detached MixerWindow: rebuild its strips
    MSG_MIXER_TICK = 'mixk',    // detached MixerWindow's own refresh tick

    // Menu bar — File.
    MSG_FILE_OPEN = 'fopn',
    MSG_FILE_SAVE = 'fsav',
    MSG_FILE_SAVE_AS = 'fsas',
    MSG_FILE_RENDER = 'frnd',
    MSG_FILE_RENDER_REGION = 'frrg',
    MSG_FILE_NEW = 'fnew',
    MSG_OPEN_PROJECT_REFS = 'popn', // refs from the Open-Project panel (vs. load-file)
    MSG_CONFIRM_SAVE = 'cfsv',      // save-before prompt reply: int32 "which" 0 Cancel, 1 No, 2 Yes
    MSG_OPEN_PROJECT_PATH = 'ppth', // from JackDawApp: string "path" given at launch
    // (Quit uses B_QUIT_REQUESTED.)

    // Menu bar — Edit.
    MSG_EDIT_UNDO = 'undo',
    MSG_EDIT_REDO = 'redo',

    // Menu bar — Track.
    MSG_TRACK_ADD = 'tadd',
    MSG_TRACK_ADD_MIDI = 'tadm',
    MSG_TRACK_LOAD_FILE = 'tldf',
    MSG_TRACK_DELETE = 'tdel',      // delete active track
    MSG_TRACK_DELETE_SLOT = 'tdes', // delete track at int32 "slot"
    MSG_TRACK_CONTEXT = 'tctx',     // strip right-click: int32 "slot" + BPoint "screen_where"
    MSG_TRACK_MOVE = 'tmov',        // reorder: int32 "from", int32 "to"
    // Right-click on the empty strip column below the last track: BPoint
    // "screen_where". Offers Add Track / Add MIDI Track.
    MSG_TRACK_AREA_CONTEXT = 'tacx',

    // Mixer strip -> MainWindow (single-mutator: a detached mixer window routes
    // its edits here rather than calling the engine from its own looper). Each
    // carries int32 "slot"; setters also carry their value. slot == -1 = master.
    MSG_MIX_SET_FADER = 'mxfd',   // float "gain"
    MSG_MIX_SET_PAN = 'mxpn',     // float "pan"
    MSG_MIX_TOGGLE_MUTE = 'mxmu', // bool  "on"
    MSG_MIX_TOGGLE_SOLO = 'mxso', // bool  "on"
    MSG_MIX_FX = 'mxfx',          // open/raise the FX window for int32 "slot"

    // FX window ownership lives in MainWindow (one per track, like the MIDI
    // editors): a track strip / mixer strip asks it to open, and the FX window
    // reports chain edits back so the strips can refresh their blue Fx state.
    MSG_OPEN_FX = 'opfx',          // pointer "track": open/raise that track's FX window
    MSG_FX_CHAIN_CHANGED = 'fxcc', // pointer "track": chain add/remove happened

    // Menu bar — Options.
    MSG_OPT_IO = 'oint',
    MSG_OPT_IO_APPLY = 'ioap', // from IoWindow: int32 "inputs"/"outputs" port counts
    MSG_OPT_PLUGINS = 'oplg',
    MSG_OPT_MIDI_CONTROL = 'omid',
    MSG_OPT_BACKEND = 'obke', // Audio System item: string "backend" "mediakit" | "jack"

    // To JackDawApp: start JackDAW again once it has quit; string "path" =
    // the project to reopen (may be absent).
    MSG_APP_RESTART = 'arst',

    // MIDI control surface (Options -> MIDI Control). The mapping table + engine
    // control_in live on the MainWindow looper (single mutator); the dialog runs
    // its own looper and only edits a local snapshot, posting mutations here and
    // receiving a fresh snapshot back. MainWindow polls control_in on a timer.
    MSG_MIDICTL_POLL = 'mcpl',     // main's own tick: drain control_in + dispatch
    MSG_MIDICTL_SNAPSHOT = 'mcsn', // main -> dialog: full table + ports + track names
    MSG_MIDICTL_ADD = 'mcad',      // dialog -> main: append a blank mapping
    MSG_MIDICTL_REMOVE = 'mcrm',   // dialog -> main: int32 "index"
    MSG_MIDICTL_LEARN = 'mcln',    // dialog -> main: arm learn for int32 "index" (-1 off)
    MSG_MIDICTL_UPDATE = 'mcup',   // dialog -> main: int32 "index" + the 8 mapping fields
    MSG_MIDICTL_SOURCE = 'mcsr',   // dialog -> main: string "port" for control_in ("" = none)

    // Engine event hook -> window (posted from JACK notification threads via
    // BMessenger; handlers re-read engine state, payloads carry no pointers).
    MSG_ENGINE_PORTS_CHANGED = 'eprt',
    MSG_ENGINE_CONNECTIONS_CHANGED = 'econ',
    MSG_ENGINE_SHUTDOWN = 'edwn',
    MSG_ENGINE_TAKE_READY = 'etak',
    MSG_ENGINE_MIDI_TAKE_READY = 'emtk',

    // Piano-roll MIDI editor <-> main window. The editor runs its own looper,
    // so engine/transport mutation is routed here as messages; the main window
    // only ever posts back asynchronously (never locks an editor window).
    MSG_MIDI_OPEN_EDITOR = 'mope',    // pointer "track": open/present the editor
    MSG_MIDI_LOCATE = 'mloc',         // int64 "frame": seek from the editor ruler
    MSG_MIDI_SET_LOOP = 'mslp',       // int64 "start"/"end" [+ bool "disable"]
    MSG_MIDI_PREVIEW = 'mprv',        // pointer "track", int8 "pitch"/"velocity", bool "on"
    MSG_MIDI_EDITOR_PRESENT = 'mwpr', // main -> editor: raise the window
    MSG_MIDI_EDITOR_REFRESH = 'mwrf', // main -> editor: clip replaced (undo/redo)

    // Render/export dialog <-> main window. The render drives the engine's
    // non-RT API (suspend, locate, transport, tap), which is the MAIN looper's
    // to call — so the dialog only posts options/cancel here and the main window
    // owns the render lifecycle + poll tick, posting progress back to the dialog.
    MSG_RENDER_START = 'rnds',    // dialog -> main: int32 format/bit_depth/source/
                                  // scope/method/sample_rate/channels; string out_path
    MSG_RENDER_CANCEL = 'rndc',   // dialog -> main: request abort
    MSG_RENDER_TICK = 'rndt',     // main's own poll tick while a render runs
    MSG_RENDER_PROGRESS = 'rndp', // main -> dialog: double "frac", int32 "state"
                                  // (0 running, 1 done, 2 cancelled, 3 failed)
};
