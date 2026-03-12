# mcp-design2gui

An MCP (Model Context Protocol) server for inspecting and exporting design files to GUI frameworks. Supports **PSD files** and **Figma** import. Built with [QtMcp](https://github.com/signal-slot/qtmcp) and [QtPsd](https://github.com/signal-slot/qtpsd).

## Tools

| Tool | Parameters | Description |
|------|-----------|-------------|
| `load_psd` | `path` | Load a PSD file for inspection and export |
| `import_figma` | `source`, `options` | Import a Figma design via URL or file key |
| `list_figma_pages` | `source`, `options` | List pages in a Figma file |
| `get_layer_tree` | | Get the full layer hierarchy |
| `get_layer_details` | `layerId` | Get detailed info for a layer (text runs, shape path, linked files, opacity, export hint) |
| `get_layer_image` | `layerId` | Get the rendered image of a layer (returned as MCP image content) |
| `set_export_hint` | `layerId`, `type`, `options` | Configure how a layer is exported |
| `do_export` | `format`, `outputDir`, `options` | Export to a target GUI framework |
| `list_exporters` | | List available exporter plugins |
| `save_hints` | | Persist export hints to the `.psd_` sidecar file |
| `get_fonts_used` | | List all fonts used with their resolved mappings |
| `get_font_mappings` | | Get current font mapping settings |
| `set_font_mapping` | `fromFont`, `toFont`, `global` | Set or remove a font mapping |

### import_figma

- **source** (string) — Figma URL (`https://www.figma.com/design/FILE_KEY/...`) or file key
- **options** (string) — JSON object with optional keys:
  - `apiKey` (string) — Figma API token (falls back to `FIGMA_API_KEY` or `FIGMA_ACCESS_TOKEN` env vars)
  - `imageScale` (int) — image download scale 1–4 (default: 2)
  - `pageIndex` (int) — 0-based page index (default: 0)
  - `hintFile` (string) — path to a `.psd_` hints file to load

### set_export_hint

- **layerId** (int) — Layer ID to configure
- **type** (string) — `embed`, `merge`, `custom`, `native`, or `skip`
- **options** (string) — JSON object with optional keys:
  - `id` (string) — identifier for property binding (empty string to clear)
  - `visible` (bool) — whether the layer is visible in export
  - `componentName` (string) — component name for `custom` type
  - `baseElement` (string) — `Container`, `TouchArea`, `Button`, or `Button_Highlighted` for `native` type
  - `properties` (array of strings) — bindable attributes: `visible`, `color`, `position`, `text`, `size`, `image`

### do_export

- **format** (string) — exporter plugin key (e.g. `QtQuick`, `Flutter`, `Slint`, `SwiftUI`, `ReactNative`, `LVGL`)
- **outputDir** (string) — absolute path to the output directory
- **options** (string) — JSON object with optional keys:

  | Option | Type | Default | Description |
  |--------|------|---------|-------------|
  | `width` | int | original | Output width in pixels (0 = original) |
  | `height` | int | original | Output height in pixels (0 = original) |
  | `fontScaleFactor` | double | 1.0 | Scale factor for fonts |
  | `imageScaling` | bool | false | Enable image scaling |
  | `makeCompact` | bool | false | Compact output |
  | `artboardToOrigin` | bool | false | Shift artboard to 0,0 |
  | `licenseText` | string | | License header for generated files |
  | `effectMode` | string | NoGPU | QtQuick only: `NoGPU`, `Qt5Effects`, or `EffectMaker` |

## Build

Requires Qt 6.

```bash
git clone --recursive https://github.com/signal-slot/mcp-design2gui.git
cd mcp-design2gui
./build.sh
```

This runs a CMake superbuild that builds QtPsd, QtMcp, and mcp-design2gui in order. The binary is output to `build/mcp-design2gui`.

### Rebuild after code changes

```bash
cmake --build build
```

## Usage

### stdio (default)

```bash
./build/mcp-design2gui
```

### SSE

```bash
./build/mcp-design2gui --backend sse --address 127.0.0.1:8000
```

### Figma API token

For Figma import, set the API token via environment variable:

```bash
export FIGMA_API_KEY="your-figma-personal-access-token"
```

Or pass it in the `options` parameter of `import_figma`.

### Claude Desktop / Claude Code configuration

```json
{
  "mcpServers": {
    "design2gui": {
      "command": "/path/to/mcp-design2gui/build/mcp-design2gui",
      "env": {
        "FIGMA_API_KEY": "your-figma-token"
      }
    }
  }
}
```

## Static builds

The project supports static builds for CI. See `.github/workflows/release.yml`. Pre-built static binaries for Linux (x64), macOS (arm64), and Windows (x64) are published on the [Releases](https://github.com/signal-slot/mcp-design2gui/releases) page.

## License

BSD-3-Clause
