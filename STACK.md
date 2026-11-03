# The JackDAW audio stack on Haiku

A set of native Haiku projects that together turn Haiku into a JACK-based recording and
production system, with a DAW, plug-ins, and Windows plug-in support through Wine. This
document explains what each piece is, how they depend on each other, and how they are built
and released.

**To install the stack, use the release:** download the zip from the
[jackDAW-haiku releases](https://github.com/rations/jackDAW-haiku/releases) and run
`sh install.sh`. Its README and release notes cover the details. The rest of this document is
for building and maintaining the stack.

Release 0.5 targets **Haiku nightly hrev60256, x86_64**.

## The pieces

```
  rations/haiku (fork)  ──► haiku, haiku_devel   the system package: stock Haiku plus
  + haiku-kernel-usb          (r1~beta6_hrevN-2)   - the UAC2 USB-audio driver
    (build script, tests)                         - 32-bit Windows code (per-team LDT)
        │                                         - user traps and hardware breakpoints
        ▼                                           delivered to programs (iLok/PACE)
  jack-port-haiku  ─────────► jack        the JACK2 server + libjack + backends
        │                                  (foundation; everything below is a JACK client)
        ├─► jack-example-tools ─► jack_tools   jack_lsp / jack_connect / jack_transport / ...
        ├─► jack-graph-haiku   ─► jackgraph    native patchbay + start/stop/settings GUI
        │
  VST3-renku ───────────────► vst3_renku       Media Kit VST3 host (vst3host) + example plug-ins
        │  (VST3 SDK, static, MIT) vst3_renku_jack  JACK VST3 host (vst3jackhost), optional
        │                          (SDK is a build-time dep of the DAW + plug-ins)
        ├─► NAMku  ───────────► namku       Neural Amp Modeler VST3
        ├─► DRUMku ───────────► drumku      MIDI drum-sampler VST3 instrument
        │
  LV2-haiku ────────────────► lv2_haiku   LV2 stack (lilv/serd/sord/sratom/zix,
        │  (static libs, MIT/ISC)           spec bundles), JACK LV2 host + tools
        ├─► hktuner ─────────► hktuner      chromatic tuner LV2 (native GUI)
        │
  jackDAW-haiku ────────────► jackdaw     the multitrack DAW (hosts VST3 + LV2 + VST2)

  haiku-wine-port ──────────► wine        Wine 11.19 with the Haiku patches
        └─► vstbridge-haiku ──► vstbridge   Wine bridge: Windows VST2 + VST3
                                            → native Haiku stub add-ons the DAW loads
```

`lv2_haiku` is both a build-time and a runtime dependency of `jackdaw`: the libraries are
linked statically, but lilv needs the LV2 **specification bundles** it installs to
`add-ons/media/LV2` to load any plug-in.

| Package | Version (0.5) | Repo | Role | License |
|---|---|---|---|---|
| `haiku`, `haiku_devel` | r1~beta6_hrev60256-3 | [rations/haiku](https://github.com/rations/haiku) branch `jackdaw-stack-0.5-hrev60256`, built with [haiku-kernel-usb](https://github.com/rations/haiku-kernel-usb) | Haiku with the USB-audio driver and the kernel support Wine needs | MIT (Haiku) |
| `jack` | 1.9.23-7 | [jack-port-haiku](https://github.com/rations/jack-port-haiku) | JACK2 server, libjack, audio/MIDI backends | GPL v2 / LGPL v2.1 |
| `jack_tools` | 4-2 | [jack-example-tools](https://github.com/rations/jack-example-tools) | standard JACK CLI utilities | GPL v2 |
| `jackgraph` | 0.2-1 | [jack-graph-haiku](https://github.com/rations/jack-graph-haiku) | native patchbay + server control GUI | MIT |
| `vst3_renku` | 0.4.0-1 | [VST3-renku](https://github.com/rations/VST3-renku) | Media Kit VST3 host (`vst3host`) + example plug-ins (+ the SDK); replaces `vst3_haiku` | MIT |
| `vst3_renku_jack` | 0.4.0-1 | [VST3-renku](https://github.com/rations/VST3-renku) | JACK VST3 host (`vst3jackhost`), optional | MIT |
| `lv2_haiku` | 0.3.0-2 | [LV2-haiku](https://github.com/rations/LV2-haiku) | LV2 stack + spec bundles, JACK LV2 host + tools | ISC |
| `namku` | 0.4.0-1 | [NAMku](https://github.com/rations/NAMku) | Neural Amp Modeler VST3 | MIT (+ MPL 2.0, Eigen) |
| `drumku` | 0.4.0-1 | [DRUMku](https://github.com/rations/DRUMku) | MIDI drum-sampler VST3 | MIT |
| `hktuner` | 0.4.0-1 | [hktuner](https://github.com/rations/hktuner) | chromatic tuner LV2 plug-in | MIT |
| `jackdaw` | 0.6.0-1 | [jackDAW-haiku](https://github.com/rations/jackDAW-haiku) | the DAW | MIT |
| `wine` | 11.19-4 | [haiku-wine-port-jackdaw](https://github.com/rations/haiku-wine-port-jackdaw) | Wine with the Haiku patches (required by `vstbridge`) | LGPL v2.1 |
| `vstbridge` | 0.2.0-1 | [vstbridge-haiku](https://github.com/rations/vstbridge-haiku) | Wine bridge for Windows VST2/VST3 → native stubs | GPL v3 (yabridge fork) |

## The system package

Stock Haiku cannot do three things the stack needs, and all three are kernel changes:

- **USB Audio Class 2.0.** The stock `usb_audio` driver enumerates UAC2 interfaces but never
  publishes an audio node for them. The driver branch adds UAC2 support and the `xhci`
  isochronous fixes it depends on (tested with a Behringer UMC204HD).
- **32-bit Windows code.** Wine runs 32-bit code inside 64-bit processes (WoW64). That needs a
  per-thread 32-bit `%fs` segment that survives context switches and signals. The kernel gets
  per-team local descriptor tables, as FreeBSD provides them for Wine.
- **Self-debugging.** Copy protection such as iLok/PACE sets the trap flag and hardware
  breakpoints on its own threads, and expects the exceptions. Without a debugger attached,
  stock Haiku dropped those traps. Now they arrive as `SIGTRAP`, and a thread can set its own
  debug registers.

A Haiku kernel is not a separate package: `haiku` contains the kernel, the drivers and the
rest of the base system, for one revision. So the changes ship as a full rebuild of `haiku`
(and `haiku_devel`, which must match it exactly) for **one nightly**. They are built with
package revision `-2`, which makes them an update to the official `-1` packages of the same
nightly. pkgman installs them in a normal transaction, and the boot menu's *Select state* goes
back to the official ones. The release installer pins the Haiku repository to that nightly,
because a newer one would replace the custom package.

Building it, on the target nightly:

```sh
git clone -b jackdaw-stack-0.5-hrev60256 https://github.com/rations/haiku.git ~/haiku
git clone https://github.com/rations/haiku-kernel-usb
sh haiku-kernel-usb/dist/build-system-package.sh ~/haiku
```

`haiku-kernel-usb/tests/wow64_ldt_test.c` tests the kernel side. The Windows-side tests are in
`haiku-wine-port/tests`. **Moving to a newer nightly** means rebasing the release branch onto
the new `hrev` tag, rebuilding the two packages there, and running both test sets.

Only Wine, among the other packages, needs this system package, and only for 32-bit programs
and PACE-protected plug-ins. On a stock kernel Wine runs 64-bit programs as before, and the
apps do not need it at all (`install.sh --apps-only`).

## Plugin & data path conventions

Every host and plug-in in this stack uses the **canonical Haiku add-on locations** — a
`media/` segment under the add-ons directory, with the format name as the leaf. These are
the *only* directories a host searches and the *only* directories a plug-in installs to:

| Format | Install dir (user, non-packaged)                     | Discovery                                                    |
|--------|------------------------------------------------------|-------------------------------------------------------------|
| VST2   | `~/config/non-packaged/add-ons/media/vstplugins`     | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/vstplugins/")` |
| VST3   | `~/config/non-packaged/add-ons/media/VST3`           | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/VST3/")`  |
| LV2    | `~/config/non-packaged/add-ons/media/LV2`            | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/LV2/")` / `LV2_PATH` = those dirs |
| CLAP   | `~/config/non-packaged/add-ons/media/CLAP`           | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/CLAP/")`  |

Rules, binding across all repos:

- **No dot-folders.** Never `~/.vst`, `~/.vst3`, `~/.clap`, `~/.lv2`, or `~/.jackdaw`. Haiku
  organises per-user files under the `~/config` hierarchy; use it.
- **Discovery goes through `find_paths`/`BPathFinder`** with the subpaths above — never a
  hardcoded `/boot/...` path. One `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, …)` call already
  resolves every install location: system packaged, system non-packaged, and the user's
  `~/config/non-packaged`. The matching packaged dir is `add-ons/media/<fmt>` under each
  install root (`/boot/system`, `/boot/system/non-packaged`, `~/config/non-packaged`).
- **VST2 is loaded as a native Haiku add-on.** Its entry symbol is resolved as
  `VSTPluginMain`, then the BeOS default `main_plugin`, then legacy `main`. VST2/BeOS
  plug-ins usually carry **no file extension** — discovery must not require `.so` (the `.so`
  some carry is an artifact of the vstbridge Wine stubs).
- **User data/settings** go through `find_directory(B_USER_SETTINGS_DIRECTORY)` /
  `B_USER_*` — e.g. JackDAW scratch recordings live under
  `~/config/settings/JackDAW/recordings`, not a dot-folder.

`vstbridge` installs the native stub add-ons it generates for Windows plug-ins into the same
`add-ons/media/{vstplugins,VST3}` locations (under a `vstbridge/` subfolder), so the DAW
finds bridged and native plug-ins through the one discovery path.

## vstbridge and Wine — Windows plug-ins

`vstbridge` (a Haiku port of yabridge) is independent of the rest of the stack: it adds Windows
plug-ins to a working install, and nothing else links against it.

- **Formats: VST2 and VST3 only.** There is no CLAP bridge on Haiku: upstream's CLAP bridge
  builds on the X11 editor layer this port drops, so the package is built with `-Dclap=false`.
  `add-ons/media/CLAP` stays reserved for later.
- **Wine must be this stack's build.** `wine` is a hard dependency, and each bridged plug-in
  runs in a Wine host process (`vstbridge-host.exe`). The bridge host is a C++ Winelib module,
  and only the patched Wine runs its static initialisers; the patches also cover the iLok
  installers, PACE and 32-bit code. The recipe and patchset are in `haiku-wine-port`.
- **Two front-ends, same job.** `VSTBridge` is a native Interface Kit application in
  *Deskbar → Applications*, and `vstbridgectl` is the equivalent CLI. Either one scans the
  Windows plug-in directories you configure and syncs one native stub add-on per plug-in into
  `add-ons/media/{vstplugins,VST3}/vstbridge`. JackDAW then loads them through its normal
  discovery path.
- **Wine graphics driver: use `null`.** The editor is bridged by capturing the plug-in's window
  and repainting it in JackDAW's FX window, so the plug-in's own on-screen window is never
  wanted. With the `null` driver win32u draws offscreen: the window still gets `WM_PAINT` but
  never reaches a display. The driver is chosen once per prefix:

  ```sh
  wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
  ```

  On the Wayland driver (`wayland`) the editor is visible twice: in its own window and in the FX
  window. Keep that driver if the prefix also runs ordinary Windows programs, or for plug-ins
  whose editors are drawn with Direct3D. The software-GL backend lives in `winewayland.drv`,
  and `null` has no GL.
- **No `WINELOADER` export needed.** Wine resolves its loader through Haiku's `find_path()`,
  so a bare `wine` from `PATH` works. vstbridge still honours `WINELOADER`, which is how a
  build-tree Wine is tested without installing it.

## Building from source

Each app repo has a `packaging/make-hpkg.sh` that builds its package on Haiku. Most also have
a `build-from-source.sh` that installs into `/boot/system/non-packaged` instead. The build
order follows the dependencies:

```
jack-port-haiku      # libjack
jack-example-tools   # needs jack
jack-graph-haiku     # needs jack
VST3-renku           # SDK static libs + vst3host (+ vst3jackhost if jack is installed)
LV2-haiku            # lilv/serd/sord/sratom/zix static + spec bundles + lv2jackhost
NAMku                # needs the VST3-haiku SDK
DRUMku               # needs the VST3-haiku SDK
hktuner              # needs the LV2-haiku staged headers
jackDAW-haiku        # needs jack + the VST3-haiku SDK + the LV2-haiku stack
haiku-wine-port      # wine, built with haikuporter (about an hour)
vstbridge-haiku      # needs wine (winegcc) + meson/ninja, vendors its own SDK
```

The system package is built separately, as above. `haikuporter` bakes the build machine's
revision into each package as a minimum, so packages built on an older nightly install on newer
ones, but not the other way round.

## Releasing

`release/` in this repo holds the release tooling:
- `install.sh`, the installer users run
- `README.txt`
- `RELEASE_NOTES-<version>.md`
- `make-release.sh`, which assembles the zip from built packages

`make-release.sh <input dir> <output dir>` expects the stack's `.hpkg` files, `system/` with
the two system packages, and `tools/` with:
- `wow64_ldt_test`, built on Haiku
- `install-driver.sh` from haiku-kernel-usb, which the installer uses to remove the old
  driver override
- Wine Mono's `wine-mono-<version>-x86.msi`

It copies only those files, writes `SHA256SUMS` and `release.conf`, and refuses to package any
other Windows binary.

**Versions:** bump a package's **version** for changes and its **revision** only for a rebuild
of the same version. Never reuse a published version: pkgman compares labels, not contents.

## Notes

- Licensing: the VST3 SDK 3.8 is MIT, and JACK clients link `libjack` (LGPL v2.1), so nothing
  forces copyleft on the MIT projects. `vstbridge` (GPL v3) and Wine (LGPL v2.1) are separate
  packages that link nothing into the others.
- The kernel and Wine changes are maintained in these repos and are not submitted upstream.
- The HaikuPorts `.recipe` files in some repos' `packaging/` are drafts; the release packages
  are built with each repo's `make-hpkg.sh`, except Wine, whose recipe is the real build.
