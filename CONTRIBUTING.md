# Contributing

Thanks for wanting to make this better. The project is small on purpose: one mpv script
that does one thing, plus the adapters needed to get it into players that do not load
mpv scripts on their own.

## Reporting what works and what does not

The most useful contribution is a report from a player or build nobody has tried yet.
Open an issue with:

- player and version (mpv, mpv.net, Stremio build, Nuvio version...)
- operating system
- what you pressed and what appeared on screen (`anyaudio: ...`)
- for Nuvio: the contents of `nuvio\shim.log`

Remove streaming links from anything you paste: they can carry account tokens.

## Pull requests

1. Fork and create a branch from `main`.
2. Keep the change focused: one fix or one feature per pull request.
3. Run the test before opening it:

   ```
   python tests/test_anyaudio.py <path to libmpv-2.dll or libmpv.so>
   ```

4. Describe what you changed and on which player you tried it.

## What fits

- Support for another mpv-based player (an install section in the README, or an adapter
  folder like `nuvio/` when the player needs one)
- Fixes for sources that fail to load or lose sync
- Translations of the README (`README.<language>.md`, linked from the top of `README.md`)

## What does not fit

- Bundling third-party binaries, shaders or media
- Features unrelated to choosing which audio plays with which video

## Code style

- `anyaudio.lua` stays a single file with no dependencies beyond mpv itself.
- Comments explain why, not what.
