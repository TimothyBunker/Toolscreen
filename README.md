# Download [here](https://github.com/jojoe77777/Toolscreen/releases)

## Custom Branch Notes

Branch: `feature/msr-f3-stronghold-overlay`

This branch customizes `src/default.toml` for MCSR workflow:

- Enables key rebinding with `LAlt -> F3` (`fromKey = 164`, `toKey = 114`).
- Moves default `Wide` hotkey from `LAlt` to `K` to avoid key conflict.
- Adds a `windowOverlay` entry for `Stronghold Arrow Overlay` with color key `#00FF00`.
- Includes that overlay in `Fullscreen`, `EyeZoom`, `Thin`, and `Wide` mode `windowOverlayIds`.
