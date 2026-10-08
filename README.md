# avs-vcamera

A virtual camera for testing software that captures video. It is a kernel
streaming driver (AVStream, from Microsoft's avssamp sample) that gives a
machine with no camera a DirectShow video capture device with:

- a **test pattern with its frame number drawn in**, as text and as a strip of
  32 black and white blocks a program can read back from a recording;
- **six formats** through `IAMStreamConfig`: 320x240, 640x480 and 1280x720,
  each in UYVY and RGB24, at 30 fps;
- an **audio capture pin on the same filter**, as on a TV capture card,
  carrying a 1000 Hz tone on the left channel and 1500 Hz on the right.

It is the capture-side sibling of [vaudio-endpoint](https://github.com/adipose/vaudio-endpoint)
and [idd-vdisplay](https://github.com/adipose/idd-vdisplay): a virtual device
at an OS boundary whose output can be asserted. The frame numbers make a
frozen picture detectable from the file a player recorded: the numbers stop
advancing. A dropped frame shows as a gap.

## What is proven

On a Hyper-V generation 2 guest running Windows 10 22H2 in test-signing mode,
which had no camera beforehand, 2026-10-08:

| | |
|---|---|
| Build with cl/link against the NuGet WDK, no WDK Visual Studio integration | yes |
| Install; DirectShow lists "Virtual Camera (avs-vcamera)" with six video caps and a 48 kHz 16-bit stereo audio cap | yes |
| Both pins found by `ICaptureGraphBuilder2::FindInterface` with `PIN_CATEGORY_CAPTURE` | yes |
| `tests\Test-Recordings.ps1`: 6 s through the AVI mux in each format, video and audio together: 180 or 181 frames, numbers consecutive, no repeats or gaps; 6.0 s of audio at 1000.0 / 1500.0 Hz | yes |
| MPC-HC's capture suite (mpc-hc-tests `suites\capture -VMName`) against it: the build before the clsid2/mpc-hc#4285 fix fails, the fixed build passes, the resolution-changes case runs | yes |

Getting there found two things in Microsoft's sample worth knowing if you
start from it:

- Its audio packets carry a start time and no duration. DirectShow's AVI mux
  then writes none of the audio, and because it waits for audio before it
  writes more video, the recording stalls after one frame. Fixed by setting
  `KSSTREAM_HEADER_OPTIONSF_DURATIONVALID` (`src/driver/audio.cpp`).
- Its RGB24 formats are top-down (negative height), which the AVI mux will not
  take, so the graph builder inserts a colour converter. They are bottom-up
  here.

The sample also truncates the audio sample count on every timer tick (1599
samples a frame instead of 1600 at 48 kHz and 30 fps); here the count is
taken from the start of the stream.

## The picture

Colour bars; across the top, 1/12 of the picture high, 32 blocks encoding the
frame number, most significant bit on the left, white for 1. The first frame
after the graph runs is 1, and stopping the graph resets it. In the middle,
`FRAME 000123` and the size, format and skipped-frame count. The same number
is in `KS_FRAME_INFO.PictureNumber`.

`tests/avicheck.py` reads an uncompressed AVI (UYVY, YUY2 or RGB24; PCM
audio) and reports the frame numbers per video stream (first, last, repeats,
gaps) and the frequency on each audio channel. With `--min-seconds`,
`--expect-tones 1000,1500` or `--max-gap` it asserts them and exits 1 on
failure. Plain Python 3.

`vcamprobe.exe` (built alongside the driver) is the matching client: `list`
prints every video capture device with its formats as JSON; `record` captures
to an AVI at a chosen size and format.

## Build

Needs Visual Studio's C++ tools and PowerShell. It does **not** need the WDK
installer or its Visual Studio extension: `Build.ps1` applies by hand what the
kernel-mode toolset would (`/kernel`, `/DRIVER`, `GsDriverEntry`).

```powershell
.\tools\Install-Toolchain.ps1   # once; about 2.6 GB under build\toolchain
.\tools\Build.ps1               # build\out\x64: signed driver package + vcamprobe
```

If idd-vdisplay or vaudio-endpoint has already fetched the same packages, set
`WDK_TOOLCHAIN_DIR` to that directory instead of downloading them again.

## Install

```powershell
. .\tools\GuestTransport.ps1
$s = Connect-TestGuest -Guest MyTestVM          # or any PSSession
.\tools\Install-VCamera.ps1 -Session $s
.\tests\Test-Recordings.ps1 -Session $s         # optional: record and check every format
```

The target must be in test-signing mode (`bcdedit /set testsigning on`, then
reboot); the script checks first. `-Remove` takes the device and the driver
package out again. Built against the Windows 10 2004 DDI, so that is the
oldest target.

The device is root-enumerated (`ROOT\VCamera`) in the Camera class, and
registered for DirectShow only (`KSCATEGORY_CAPTURE` and `KSCATEGORY_VIDEO`).
It is deliberately not a `KSCATEGORY_VIDEO_CAMERA`: that would hand it to the
Windows Camera Frame Server, which could hold the one video pin instance a
test player needs. Media Foundation and the Camera app do not see it.

## Licence

Two licences, by directory -- see `NOTICE`. The driver sources in
`src/driver` derive from Microsoft's avssamp sample and remain under the
Microsoft Public License (`LICENSE.microsoft-sample`), modifications included;
the first commit is the pristine sample, so the history shows exactly what
changed. Everything else -- `src/tools`, `tools`, `tests`, documentation -- is
MIT (`LICENSE`). GitHub's licence badge can only show one of the two.
