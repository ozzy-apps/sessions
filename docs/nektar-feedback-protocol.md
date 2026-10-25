# LX49+ / LX61+ native pad feedback

Sessions implements its own launcher state machine and message builder. The wire constants below were checked against the LX49+ controller extension bundled in Bitwig Studio 6.1.4's official Linux installer. No Bitwig/Nektar code or runtime is included in Sessions. The [Nektar product description](https://nektartech.com/impact-lx49-61-plus/) and [LX+ Bitwig guide](https://support.nektartech.com/download/using_impact_lx_plus_with_bitwig/) describe the hardware's clip/scene controls; the bundled extension supplies the packet reference.

## Ports and framing

Native controls arrive on **MIDI2**, while keyboard/normal pad notes use **MIDI1**. Send feedback to **MIDI1**. Sessions pairs both enabled named LX49+/61+ ports from the same ALSA client, honours an explicit input selection, and keeps other controllers separate.

Live SysEx framing is `F0 00 01 77 7F <payload> <checksum> F7`. The payload and checksum sum to zero modulo 128. Each payload byte is 7-bit. These commands configure live host controls; they are distinct from the stored-preset/pad-map memory format.

| Action | Payload before checksum |
| --- | --- |
| USB Port Setup 2 (native split ports) | `01 05 00 01 0F 01 02` |
| USB Port Setup 0 (basic mode) | `01 05 00 01 0F 01 00` |
| Internal/performance pads | `01 09 04 01 00 01 02` |
| Launcher pads | `01 09 04 01 00 01 03` |
| Eight pad palettes | `01 06 00`, followed by `00 <pad 1..8> 01 <palette>` for each pad |

Mode indicators are CC117 (Clips) and CC118 (Scenes), on MIDI channel 16: 127 on, 0 off. Input buttons can send 1/0 or 127/0. Shift is CC108.

## Pad colours and state

Native pad notes are `60, 62, 64, 65, 67, 69, 71, 72`. Feedback uses channel 16 Note On, with velocity **0 selecting the palette's off colour** and **1 selecting its on colour**. Do not substitute ordinary RGB or APC pad velocities.

| Palette | Off colour | On colour | Sessions use |
| --- | --- | --- | --- |
| 3 | Off | Green | Empty slot |
| 5 | Yellow | Green | Loaded / playing / queued |
| 6 | Off | Red | Recording without content |
| 8 | Yellow | Red | Recording |
| 12 | Off | Orange | Selected empty bank |
| 15 | Yellow | Orange | Available / selected bank |

Queued clips alternate off/on state. A playing clip stays green; a playing scene pulses with the musical beat. Recording takes priority and lights red. Hold Clips or Scenes and press pad 1–8 to choose that mode's bank of eight existing scenes. Each mode keeps its own bank. Track −/+ selects tracks while launcher pads are active; Shift preserves the existing mixer bank controls. Shift-pad selects a clip without launching it.

## Delivery and cleanup

The UI thread sends only changes, caches a state only after delivery succeeds, waits one second before retrying failures, and resends initialization/state after the controller settles. No audio callback accesses MIDI output. Losing/disablement of a paired port stops launcher input; reconnecting restores the state.

Turning feedback/profile off or closing Sessions restores internal pads, extinguishes the Clips/Scenes indicators and sets USB Port Setup 0. It does not overwrite user presets, stored pad maps or firmware. A crash cannot run cleanup: reopen Sessions or set USB Port Setup 0 manually to return to basic mode. Use one DAW's integration with the keyboard at a time.

Automated tests cover packet fixtures/checksums, port isolation, disabled ports, explicit device selection, mode/bank control, state colours, retries, recording, group/Gate releases and custom mapping precedence. Physical pad/button/colour verification remains a separate check on the LX49+.
