---
name: dslogic
description: Capture digital signals with a DreamSourceLab DSLogic logic analyzer (dslcap) and decode them headlessly with DSView's protocol decoders (dsldecode) - USB low/full-speed, UART, I2C, SPI, CAN, SWD, JTAG and ~150 more. Use for logic-analyzer captures, decoding .dsl files and checking signal timing; not for high-speed USB bus sniffing or analog measurements.
---

# DSLogic capture and decode

Two command-line tools from the DSView fork (github.com/HiFiPhile/DSView):
`dslcap` captures with a DSLogic into a DSView session file (`.dsl`), and
`dsldecode` runs DSView's decoders on a `.dsl` file. Both print JSON on
stdout and diagnostics on stderr. This skill covers operating them; it does
not authorize driving the target device or changing its wiring.

## Locate the tools

The local checkout is `E:\code\DSView`; the tools' sources are in
`tools/dslcap` and `tools/dsldecode`, each documented in its header comment.
Prefer the user's executable if they name one. The official DreamSourceLab
DSView install does not include these tools.

- Prefer a self-contained package: the newest `build-dist\DSView-*-ucrt64\`
  in the checkout that contains `dslcap.exe` and `dsldecode.exe`, or the
  Windows package built by the fork's CI. The tools find the firmware (`res\`),
  the decoders (`decoders\`) and Python (`pylib\`) next to themselves.
- Linux installs put both in `bin/`, with the firmware and decoders under
  `share/`.
- A build tree (`build.dir\`) also works, but on Windows needs
  `C:\msys64\ucrt64\bin` on `PATH`, and dsldecode needs
  `PYTHONHOME=C:\msys64\ucrt64`.
- Explicit locations: `--res DIR` or `DSLCAP_RES` for the firmware,
  `--decoders DIR` or `DSLDECODE_DECODERS` for the decoders.

## Before using the hardware

- Only one program can own the analyzer. Check that no DSView is running
  (`tasklist /FI "IMAGENAME eq DSView.exe"`, or `pgrep -x DSView`). If one is,
  ask the user to close it, and do not kill it yourself. Run one dslcap at a
  time.
- `dslcap --list` lists the devices. dslcap uses the first one whose name
  contains "DSLogic", never the demo device.
- If the analyzer is missing, stops answering, or fails twice the same way,
  ask the user to replug it. Never simulate a replug by power-cycling or
  resetting the hub port (IOCTL_USB_HUB_CYCLE_PORT, usbreset, driver unbind):
  on a U3Pro16 that left the device dead until a physical replug.
- Know the wiring: which analyzer channel goes to which signal, the logic
  voltage, and a shared ground. Ask if the task does not say. Channel numbers
  are the probe labels, 0-15 on a U3Pro16.

## Plan a capture

- **Sample rate** (`--samplerate`, Hz). Use at least 4 times the fastest bit
  or clock rate, and about 10 times for clean timing. Examples: USB full-speed
  (12 Mbit/s) at 100 MHz, which has been checked on hardware; USB low-speed at
  25 MHz or more; UART 115200 at 2 MHz or more; SPI or I2C at 4 to 10 times
  the clock. Only rates the device offers in DSView's menu for that mode work;
  any other rate fails with `samplerate unavailable in operation mode` or
  `unsupported samplerate`.
- **Depth** (`--samples`, per channel). The duration is samples / rate.
  - In the default `--mode buffer`, the capture goes to the device memory: high
    rates work, but depth is limited. When the request is too deep, dslcap
    reports `cannot verify device configuration` with setting `samples`.
  - `--mode stream` sends samples live over USB, so it allows long captures at
    a lower rate times channel count. Too much data ends in
    `device buffer overflow`.
  - At high rates fewer channels exist: `channel/rate combination unavailable`
    gives `max_channels`.
- **Threshold** (`--vth`, volts, default 1.6, range 0-5): about half the logic
  high level. Use 1.6 for 3.3 V logic and USB LS/FS, and 0.9 for 1.8 V logic.
- **Trigger** (`--trigger CH[:T]`). T is R for a rising edge (the default),
  F for falling, C for either edge, 1 for high, or 0 for low. CH must be one of
  the `--channels`.
  - `--trigpos P` places P% of the samples before the trigger (default 10).
  - Without a trigger, the capture starts at once.
  - `--timeout` (seconds, default 30) bounds the whole capture, including the
    wait for the trigger.
- **Output**: always pass `--dsl --out BASE` to get `BASE.dsl`, which DSView
  opens and dsldecode reads. dslcap never overwrites, so choose a fresh BASE in
  a scratch or ignored directory, not the source tree. While capturing, it
  spools next to BASE, so the directory needs room for
  samples / 8 bytes per channel.

## Run and validate

USB full-speed enumeration on D+ = channel 0 and D- = channel 1: the idle
bus has D+ high, so a falling edge on D+ marks the first bus activity.

```sh
dslcap --channels 0,1 --samplerate 100000000 --samples 50000000 \
       --trigger 0:F --trigpos 5 --timeout 60 --dsl --out C:/tmp/caps/usb-enum-001
```

To capture an event you cause, such as a plug, reset or command:

1. Start dslcap in the background with a trigger and a timeout long enough
   for the event.
2. It prints nothing until it finishes, and has no ready signal. Wait about
   3 s for it to configure the device, then cause the event, or ask the user
   to (for example, "plug the device in now").
3. Wait for dslcap to exit.

An event that comes too early is missed, and the capture then triggers on a
later event or times out.

Validate the result before decoding:
- Exit 0 and a JSON object without `"error"` whose `"dsl"` holds the path
  mean a complete capture.
- Useful report fields: `samples`, `samplerate`, `trigger_pos` (the sample
  index of the trigger, -1 if none), `elapsed_s`, `vth` and `format`.

| Exit | Meaning | Action |
|---|---|---|
| 1 | runtime or I/O error, including `no DSLogic found` | check the device and DSView; ask for a replug if it persists |
| 2 | bad arguments, or settings the device cannot do | fix the rate, depth or channels; nothing was captured |
| 3 | the capture failed: `capture timed out`, overflow, detached, short | no file was written; a timeout usually means the trigger never fired |

A complete capture of an idle bus is still complete. Before drawing
conclusions, confirm in the decode that the expected activity is there.

## Decode

```sh
dsldecode --list > decoders.json   # ids, channels, options with defaults and values, annotations
dsldecode -P ID:CH=N...:OPT=VALUE... [-P ID[:OPT=VALUE...]]... [--all] BASE.dsl > out.jsonl
```

**Arguments**
- The first `-P` decoder reads the capture: each of its channels CH is
  mapped to capture channel N (the dslcap channel number).
- Each further `-P` is stacked on the previous one. Its `inputs` must include
  the previous decoder's `outputs`.
- An option value takes the type of its default. Look up ids, channel names
  and allowed values in `--list` rather than guessing.
- Ids with a `0:` or `1:` prefix are DSView's variants of a decoder. For
  example, only `1:uart` can carry a stacked decoder and annotate data bits
  and breaks.

**Output**
- Without `--all`, only the top decoder's annotations are printed.
- Each annotation is one line:
  `{"pd","ann","start","end","t","hex","value","texts"}`.
  - `start` and `end` are sample numbers.
  - `t` is seconds from the start of the capture. Subtract
    `trigger_pos / samplerate` to get the time from the trigger.
  - `texts` go from longest to shortest.
  - `hex` and `value` appear on numeric annotations, such as UART bytes.
- The last line is `{"done":true,"samples","samplerate","annotations"}`.
  Failures print `{"error","detail"}`, with exit 1 (runtime) or 2 (arguments).

| Signal | Decoder stack (channels as wired) |
|---|---|
| USB full-speed | `-P usb_signalling:dp=0:dm=1 -P usb_packet -P usb_request` |
| USB low-speed | the same, with `:signalling=low-speed` on `usb_signalling` and `usb_packet` |
| UART | `-P 1:uart:rxtx=0:baudrate=115200` (add `num_data_bits`, `parity_type` and other options as needed) |
| I2C (+ EEPROM) | `-P 0:i2c:scl=0:sda=1` (then `-P eeprom24xx`) |
| SPI (+ flash) | `-P 0:spi:clk=0:mosi=1:miso=2:cs=3:cpol=0:cpha=0` (then `-P spiflash`) |
| CAN | `-P can:can_rx=0:bitrate=500000` |
| SWD | `-P swd:swclk=0:swdio=1` |

USB annotations: `usb_packet` gives `packet-setup`, `packet-in`, `packet-out`,
`packet-data0`/`packet-data1`, `packet-ack`, `packet-nak` and `packet-sof`.
`usb_request` gives `request-setup-read`/`-write` and
`request-bulk-read`/`-write`, each with the SETUP bytes, the data and the
handshake. Decoding 50M samples takes about a second.

## Read the results without flooding the context

Decodes easily produce tens of thousands of lines, so always redirect them
to a file. Summarize the file with `scripts/dsl_ann.py` from this skill's
directory (Python 3, standard library only):

```sh
python scripts/dsl_ann.py out.jsonl                      # counts and first/last time per annotation
python scripts/dsl_ann.py out.jsonl -a request-setup-read -n 20
python scripts/dsl_ann.py out.jsonl -a packet-setup --from 0.17 --to 0.18
python scripts/dsl_ann.py out.jsonl -a data --bytes      # UART/I2C/SPI payload as one byte string
```

It exits 1 when the decode failed. Decoder error annotations usually point to
the capture before the target: `sync-err`, `crc5-err` and `crc16-err` in USB,
or `warnings` such as frame errors in UART. Typical causes are a sample rate
too low, a wrong threshold, swapped channels or a wrong baud rate.

## View in DSView

`DSView.exe BASE.dsl` opens the capture in the GUI, where the decoders can be
added interactively. Ask before launching GUI windows; otherwise give the user
the path.

## Report

Include:
- the dslcap command and its result: device, rate, samples, trigger and
  elapsed time
- the `.dsl` path
- the decoder stack and the relevant annotations, with their times
- whether a problem lies in the capture (missing activity, decoder errors) or
  in the target

A clean decode of the wrong channels, or of a capture taken before the event,
proves nothing about the target.
