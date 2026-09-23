# Live nvtune CLI profile round trip (2026-09-23)

The actual `nvtune.exe` CLI was run as Administrator against a TITAN Xp (GP102) at PCI `0000:01:00.0`, driver 582.66, stock VBIOS `86.02.3d.00.01`. The tested executable's SHA-256 is `af4e5fb9d975509f908046ac37f47d1cc752c397362a6ab6fb9ac232dcda4e21` and was built from the code commit `aabddc6`. A bounded CUDA memory-copy workload held the card at P2 / 5508 MHz reported while timing writes and readbacks ran. No voltage, offset, or clock-setting commands were issued. The workload stopped after the final readback.

The fresh baseline was `FAW=24`, `RFC=157`, `CL=19`. It matched an independently saved pre-test raw baseline in the broadcast aperture and all six active FBPA partitions. The CLI sequence was:

```powershell
nvtune save --device 0000:01:00.0 --profile original-profile.json
nvtune save --device 0000:01:00.0 --output original-raw.json
nvtune get FAW RFC CL --device 0000:01:00.0
nvtune dump --raw --device 0000:01:00.0
nvtune set FAW=25 RFC=158 --device 0000:01:00.0
nvtune save --device 0000:01:00.0 --profile modified-profile.json --output modified-raw.json
nvtune get FAW RFC CL --device 0000:01:00.0
nvtune dump --raw --device 0000:01:00.0
nvtune restore --device 0000:01:00.0 --input original-raw.json
nvtune daemon --profile modified-profile.json --interval 1 --verbose
# A real Windows console Ctrl-C event stopped the daemon.
nvtune save --device 0000:01:00.0 --profile postdaemon-profile.json --output postdaemon-raw.json
nvtune get FAW RFC CL --device 0000:01:00.0
nvtune dump --raw --device 0000:01:00.0
```

Actual paths and native output are in [the capture transcript](capture-transcript.txt), [daemon transcript](daemon-visible-transcript.txt), and [post-daemon transcript](postdaemon-transcript.txt). `LOCALAPPDATA` was isolated for this run so its default stock backup was taken from the fresh high-band baseline. The daemon ran alone: no second nvtune process read the device before its Ctrl-C restoration. Its verbose output showed `re-asserted RFC FAW`, followed by `restoring stock values...` and `restored` after Ctrl-C. The console event also reached the PowerShell wrapper, producing the transcript's final `pipeline has been stopped` message after nvtune printed `restored`; the subsequent fresh CLI read verified the exact baseline with `FAW=24 RFC=157 CL=19`. The wrapper transcript does not establish nvtune's exit code.

These are unaltered screenshots of visible native PowerShell/nvtune consoles during the live run:

- [Original profile export and raw CLI read](01-cli-original.png)
- [Two-field set, modified profile export, and raw CLI read](02-cli-modified.png)
- [Explicit restore before the daemon run](03-cli-pre-daemon-restored.png)
- [Daemon running from the saved modified profile](04-cli-daemon-running.png)
- [Fresh CLI read after daemon Ctrl-C restoration](05-cli-daemon-restored.png)

[comparison-cli.json](comparison-cli.json) lists all 23 exported tunable fields and 49 raw words (seven registers in each of seven scopes) at baseline, modified, and post-daemon restored states. The only field changes were `FAW` 24→25 and `RFC` 157→158. The only raw changes in each scope were `CONFIG0` `0x16489D3A`→`0x16489E3A` and `CONFIG3` `0x2200314A`→`0x2200334A`. Every field and raw word returned exactly to the baseline after daemon shutdown. The underlying CLI exports and raw captures are [original profile](capture-original-profile.json), [original raw backup](capture-original-raw.json), [modified profile](capture-modified-profile.json), [modified raw backup](capture-modified-raw.json), [post-daemon profile](postdaemon-profile.json), and [post-daemon raw capture](postdaemon-raw.json).

An earlier attempt to read the registers from another nvtune process **while** the daemon ran invalidated its BAR0 mapping; that attempt was immediately restored with an explicit raw backup and is not counted as the successful daemon lifecycle test. The pre-existing driver shares one global mapping handle across clients but unmaps it on any client's close. Concurrent CLI access can invalidate the daemon's mapping with the current driver; this separate lifetime bug was not changed here. An older 19-field trial caused display artifacts and ended with a user reboot; it is not counted as a successful test. This two-field result establishes the CLI profile/daemon round trip on this device and operating point, not long-term memory stability.
