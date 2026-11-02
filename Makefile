# Builds on Haiku only (uses the non-packaged JACK install and glib2 from pkgman).
# Engine/model code is C (gnu99, glib2 — no GTK); UI code is C++ (Interface Kit).
# VST3 hosting builds against the sibling VST3-haiku checkout (Haiku-patched
# Steinberg SDK sources + its cmake-built static libs); LV2 hosting builds
# against the sibling LV2-haiku checkout (lilv/zix/serd/sord/sratom built as
# static libs into its build/stage prefix).
CC  = gcc
CXX = g++
NONPACKAGED = /boot/system/non-packaged
VST3_SDK ?= $(HOME)/VST3-haiku/vst3sdk
VST3_LIB ?= $(HOME)/VST3-haiku/build/lib/Release
LV2_STAGE ?= $(HOME)/LV2-haiku/build/stage
LV2_PC     = PKG_CONFIG_PATH=$(LV2_STAGE)/lib/pkgconfig pkg-config
LV2_CFLAGS := $(shell $(LV2_PC) --cflags lilv-0 zix-0 lv2)
LV2_LIBS   := $(shell $(LV2_PC) --static --libs lilv-0 zix-0)

# gio-2.0: project save/load copies bundle audio via GFile (g_file_copy).
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0 gobject-2.0 gio-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0 gobject-2.0 gio-2.0)
# Audio file I/O (libsndfile) + sample-rate conversion (libsamplerate), used by
# the clip peak tables and the playback feeder thread.
AUDIO_CFLAGS := $(shell pkg-config --cflags sndfile samplerate)
AUDIO_LIBS   := $(shell pkg-config --libs sndfile samplerate)

COMMON   = -Wall -Wextra -MMD -MP -I$(NONPACKAGED)/include -Isrc
CFLAGS   = -std=gnu99 $(COMMON) $(GLIB_CFLAGS) $(AUDIO_CFLAGS)
CXXFLAGS = -std=c++17 $(COMMON) $(GLIB_CFLAGS)
# SDK translation units (ours in src/host/ + the three SDK sources below) need
# the SDK include root and a build-mode define (fdebug.h insists on one).
HOST_CXXFLAGS = $(CXXFLAGS) -I$(VST3_SDK) -DRELEASE=1
# -ltracker: BFilePanel (open/save panels) lives in libtracker, not libbe.
LDFLAGS  = -L$(NONPACKAGED)/lib -L$(VST3_LIB) -ljack -lbe -ltracker \
           -lsdk_hosting -lsdk_common -lbase -lpluginterfaces \
           $(LV2_LIBS) $(GLIB_LIBS) $(AUDIO_LIBS)

C_SOURCES   = src/engine/glib_check.c src/engine/message.c src/engine/settings.c \
              src/engine/track.c src/engine/project.c src/engine/jackdaw-engine.c \
              src/engine/tempomap.c src/engine/timeruler.c \
              src/engine/audio_clip.c src/engine/clipregion.c src/engine/undo.c \
              src/engine/midiclip.c src/engine/render.c src/engine/midicontrol.c \
              src/engine/rt_ringbuffer.c src/engine/audio_backend.c \
              src/backend/jack/jack_backend.c
CXX_SOURCES = src/ui/main.cpp src/ui/JackDawApp.cpp src/ui/MainWindow.cpp \
              src/ui/TransportView.cpp src/ui/TimelineView.cpp src/ui/RulerView.cpp \
              src/ui/TrackAreaView.cpp src/ui/StepperControl.cpp \
              src/ui/MetronomeWindows.cpp src/ui/KnobView.cpp src/ui/FaderView.cpp \
              src/ui/VuView.cpp src/ui/StateButton.cpp src/ui/TrackStripView.cpp src/ui/MixerStripView.cpp \
              src/ui/MixerView.cpp src/ui/MixerWindow.cpp src/ui/RegionGainWindow.cpp \
              src/ui/MidiWindow.cpp src/ui/FxWindow.cpp src/ui/RenderWindow.cpp \
              src/ui/IoWindow.cpp src/ui/MidiControlWindow.cpp src/ui/ScanWindow.cpp
HOST_SOURCES = src/host/pluginhost.cpp src/host/pluginhost_lv2.cpp src/host/pluginhost_vst2.cpp

# SDK sources compiled directly (they are not part of the SDK's static libs;
# same set the VST3-haiku hosts compile): the Haiku module loader, the
# component/controller provider, and the IBStream-on-memory used for state.
SDK_OBJECTS = src/host/sdk_module_haiku.o src/host/sdk_plugprovider.o \
              src/host/sdk_memorystream.o

# VERSION (repo root) is the only place the version lives: the link stamps it
# into the app's version resource (Tracker's Get Info), and make-hpkg.sh names
# the package and writes its PackageInfo from it. A bump re-stamps the app.
VERSION       := $(shell cat VERSION)
VERSION_PARTS := $(subst ., ,$(VERSION))

OBJECTS = $(C_SOURCES:.c=.o) $(CXX_SOURCES:.cpp=.o) $(HOST_SOURCES:.cpp=.o) $(SDK_OBJECTS)
DEPS    = $(OBJECTS:.o=.d)
TARGET  = JackDAW

$(TARGET): $(OBJECTS) VERSION
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	setversion $(TARGET) -app $(word 1,$(VERSION_PARTS)) $(word 2,$(VERSION_PARTS)) \
		$(word 3,$(VERSION_PARTS)) -short $(VERSION) -long "JackDAW $(VERSION)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The LV2 backend TU gets the lilv/zix include paths and never the VST3 SDK's
# (explicit rule, so it wins over the src/host/%.o pattern below).
src/host/pluginhost_lv2.o: src/host/pluginhost_lv2.cpp
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -c $< -o $@

# The VST2 backend TU drives the plain AEffect ABI (clean-room vst2_abi.h) and
# must never see the VST3 SDK headers (both typedef int8/int16 — they cannot
# share a TU). Plain CXXFLAGS only; -lbe (for the Part B editor view) is already
# in LDFLAGS. Explicit rule so it wins over the src/host/%.o pattern below.
src/host/pluginhost_vst2.o: src/host/pluginhost_vst2.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/host/%.o: src/host/%.cpp
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

# UI files that talk to the plugin host only need pluginhost.h (plain C API),
# so they build with the normal CXXFLAGS — no SDK include path leaks into the
# Interface Kit code.

src/host/sdk_module_haiku.o: $(VST3_SDK)/public.sdk/source/vst/hosting/module_haiku.cpp
	$(CXX) $(HOST_CXXFLAGS) -w -c $< -o $@

src/host/sdk_plugprovider.o: $(VST3_SDK)/public.sdk/source/vst/hosting/plugprovider.cpp
	$(CXX) $(HOST_CXXFLAGS) -w -c $< -o $@

src/host/sdk_memorystream.o: $(VST3_SDK)/public.sdk/source/common/memorystream.cpp
	$(CXX) $(HOST_CXXFLAGS) -w -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJECTS) $(DEPS) $(TARGET)

.PHONY: clean
