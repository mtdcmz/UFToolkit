# UFToolkit

A collection of tools for [UFunPlayer](https://github.com/mtdcmz/UFunPlayer) – the standalone Unity Web Player for Windows.

## Tools

### [UPPEditor](./UPPEditor/)
Unity WebPlayer PlayerPrefs (`.upp`) file editor.  
View, edit, add, and delete save keys (String / Int / Float).  
Integrates with UFunPlayer – auto‑opens the most recent save if UFunPlayer is running.

## Usage

1. Download the latest release, or build from source.
2. Place tools in the `Tools` folder next to `UFunPlayer.exe` to launch them directly from UFunPlayer's **Tools** menu.
3. Tools can also be run standalone.

## Building

Each tool is built with **MSYS2 MinGW32** using the provided `Makefile`.

```bash
cd UPPEditor
make
```

Requires:
- `g++` / `windres` / `strip` (MinGW32)
- Windows SDK headers and libraries (shipped with MinGW)

The resulting `.exe` is statically linked – no external runtime dependencies.

## License

GNU General Public License v3.0 – see [LICENSE](LICENSE).
