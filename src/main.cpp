// Copyright (C) 2025 Signal Slot Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifdef QT_STATIC
#include <QtPlugin>
Q_IMPORT_PLUGIN(QMcpServerStdioPlugin)
#endif

#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtPsdCore/qpsdblend.h>
#include <QtMcpCommon/QMcpPrompt>
#include <QtMcpCommon/QMcpPromptMessage>
#include <QtMcpCommon/QMcpPromptMessageContent>
#include <QtMcpCommon/QMcpTextContent>
#include <QtMcpCommon/qmcprole.h>
#include <QtMcpServer/QMcpServer>
#include <QtMcpServer/QMcpServerSession>
#include <QtPsdGui/QPsdAbstractLayerItem>
#include <QtPsdGui/qpsdguiglobal.h>
#include <QtPsdGui/QPsdFolderLayerItem>
#include <QtPsdGui/QPsdFontMapper>
#include <QtPsdGui/QPsdGuiLayerTreeItemModel>
#include <QtPsdGui/QPsdImageLayerItem>
#include <QtPsdGui/QPsdShapeLayerItem>
#include <QtPsdGui/QPsdTextLayerItem>
#include <QtPsdExporter/QPsdExporterPlugin>
#include <QtPsdExporter/QPsdExporterTreeItemModel>
#include <QtPsdImporter/QPsdImporterPlugin>

using namespace Qt::StringLiterals;

static QString toJson(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

class McpServer : public QMcpServer
{
    Q_OBJECT
public:
    explicit McpServer(const QString &backend = "stdio"_L1, QObject *parent = nullptr)
        : QMcpServer(backend, parent)
    {
        exporterModel.setSourceModel(&guiModel);

        connect(this, &QMcpServer::newSession, this, [](QMcpServerSession *session) {
            QMcpPrompt prompt;
            prompt.setName("export-screen"_L1);
            prompt.setDescription("Export a design (PSD or Figma) to a GUI framework (QML, Slint, Flutter, etc.)"_L1);

            QMcpTextContent text(uR"(# Export a Design to a GUI Framework

Ask the user (or pick from the conversation) for:
- the design source — an absolute path to a PSD file, or a Figma URL / file key
- the target export format — one of the keys returned by `list_exporters` (e.g. `QtQuick`, `Slint`); if it has not been specified yet, run `list_exporters` and ask

## Steps

### 1. Load the design file

**For PSD files:**
```
load_psd(path="<absolute path to the PSD file>")
```

**For Figma files:**
```
import_figma(source="<Figma URL or file key>")
```
If importing from Figma, you may first call `list_figma_pages` to see available pages, then specify `pageIndex` in the options.

This returns file information including dimensions and layer count.

### 2. Inspect the layer tree

```
get_layer_tree()
```

Review the child layers and classify them:

- **Screen-level frames (each visible screen / route)** — every top-level frame that represents a distinct screen (Home, Settings, DeviceList, ...) must be its own reusable component. Mark each one `type: "custom"` with a `componentName` derived from the screen's *role* in PascalCase. **Never** leave them as the default embed — that produces one giant `MainScreen.ui.qml` containing every screen inlined together.
- **Reusable widgets that repeat (cards, list rows, custom toggles you'll instantiate many times)** — also `type: "custom"` with a role-based `componentName`.
- **Stock UI controls — `type: "native"` (substitutive) vs. `type: "embed"` (additive)**. When a Figma node is *semantically* a standard control (toggle, dropdown, tab segment, numeric stepper, button, ...), there is a real tradeoff:
  - **`type: "native"` + matching `baseElement`** (`Switch`, `ComboBox`, `TabBar` + `TabButton`, `SpinBox`, `Slider`, `CheckBox`, `RadioButton`, `Button`, `Button_Highlighted`) gives you the framework's interaction logic for free (`checked`, `model`, `value`, `currentIndex`, ...). The catch: the layer's own visuals are dropped and the framework draws its default look, so you must build a **custom QtQuick.Controls 2 style module** to make the control actually look like the design (one `.qml` per control type), then activate it with `QQuickStyle::setStyle("YourStyle")`.
  - **`type: "embed"` + tappable layers** (see "Tappable design layers" below) keeps the design's exact visuals as images and makes them touch-sensitive, but you re-implement the control's state machinery (toggle, selection, model, etc.) yourself in the logic wrapper.
  - Pick `native` when (a) you will build a style module anyway, or one already exists for the target style, **and** (b) the framework primitive cleanly covers the control's behavior. Pick `embed` when the visuals are heavily custom or one-off and a style module would be more work than re-implementing the simple state. **Avoid `type: "custom"` for things that ARE stock controls** — re-implementing `model` / `value` / `checked` in custom QML throws away the framework's interaction logic and ages badly.
- **Tappable design layers (icons, sidebar tiles, list rows, any image/text that should react to touch)** — use `type: "embed"` + `interactive: true` so the exporter wraps the layer's rendered content in a `MouseArea` *inside* the `.ui.qml` with a stable `id`. The visual stays AND the region becomes tappable. Use this for embedded-style touchscreen UIs that don't go through `QtQuick.Controls`. **Never** overlay a `MouseArea` from the logic wrapper onto an embedded design item — every hit area lives in the design layer and is re-export-stable.
- **Bare hit-only regions (transparent overlay, enlarged hit-box beyond an icon)** — use `type: "native"` + `baseElement: "TouchArea"`. The output is a bare `MouseArea` with no visual content. Use this only when you genuinely want no visual; otherwise prefer the previous bullet.
- **Text labels (dynamically updated)** — assign an `id` so they become property aliases.
- **Decorative elements** — no hint needed (exported by default).

Use `get_layer_details(layerId=...)` to inspect individual layers for more detail (text runs, shape info, linked files, etc.).
Use `get_layer_image(layerId=...)` to visually inspect a layer's rendered appearance.

### 3. Set export hints

#### `type` values at a glance

- `embed` — keep the layer's own visuals (image / text / shape). Optional flags add behavior on top: `interactive: true` makes it tappable, `baseElement: "TouchArea"` is the same thing by another spelling. **Embed is additive** — the layer's visual is preserved and hints decorate it.
- `native` — replace the layer with a framework-provided stock control (Switch, ComboBox, Button, ...). **Native is substitutive** — the layer's own visuals are dropped; the framework draws the control via its style.
- `custom` — emit the layer (and its descendants) as a separate reusable component file (`.ui.qml`).
- `merge` — composite the layer into a single image with its parent (rare; usually set automatically by `textSource` / `imageSource`).
- `skip` — omit from export entirely (use for helper / guide / reference layers).

#### Naming rules (apply to every `id` and `componentName`)

- Derive names from the layer's **role** in the UI, not from its position in the tree. A toggle that controls Wi-Fi is `wifiToggle` / `WifiToggle`, never `toggle1` / `Switch2`.
- `componentName`: PascalCase, ends with the role noun (`HomeScreen`, `DeviceCard`, `VolumeSlider`). This is the **type name**; the instance `id` is set by whoever embeds the component, not in this hint.
- `id`: camelCase, role-based (`wifiToggle`, `signalSource`, `btnSettings`).
- **Forbidden:** numeric suffixes used as a substitute for naming (`Screen1`, `Screen2`, `btn1`, `label3`). If two siblings genuinely share a role, the design itself needs disambiguation — ask before falling back to numbers.

#### Each screen / reusable component
```
set_export_hint(layerId=..., type="custom", options='{"componentName": "HomeScreen"}')
set_export_hint(layerId=..., type="custom", options='{"componentName": "SettingsScreen"}')
set_export_hint(layerId=..., type="custom", options='{"componentName": "DeviceCard"}')
```
Do this for **every** screen frame and every repeated widget — one `type:"custom"` per generated `.ui.qml` file.

#### Stock UI controls (preferred for anything semantically standard, when QtQuick.Controls fits)

When a Figma node is semantically a standard control, replace it with the framework primitive:
```
set_export_hint(layerId=..., type="native", options='{"id": "wifiToggle", "baseElement": "Switch"}')
set_export_hint(layerId=..., type="native", options='{"id": "signalSource", "baseElement": "ComboBox"}')
set_export_hint(layerId=..., type="native", options='{"id": "triggerMode", "baseElement": "TabBar"}')
set_export_hint(layerId=..., type="native", options='{"id": "alertLevel", "baseElement": "SpinBox"}')
```
The exporter then emits e.g. `Switch { id: wifiToggle; ... }` with the standard QtQuick.Controls API (`checked`, `model`, `value`, `currentIndex`, ...). Restyle with a custom `QQuickStyle` module rather than wrapping it.

`Button_Highlighted` is the same `Button` type but with the visually emphasized state turned on (QtQuick: `highlighted: true`; Slint: `primary: true`). Use it for the primary / accent button in a screen.

#### Buttons with a caption — use `textSource`, do not assign `text` from logic code

When a Button has a sibling text layer that holds its caption, point the Button's hint at that layer with `textSource`. The exporter pulls the string straight into the generated control:

```
set_export_hint(layerId=<buttonLayerId>, type="native", options='{"id": "btnOk", "baseElement": "Button", "textSource": "OK Label"}')
```

`textSource` takes the *layer name* of a sibling text layer (visible via `get_layer_tree` / `get_layer_details`). The output becomes:

```qml
Button { id: btnOk; text: qsTr("OK") }   // QtQuick (with translatable, see below)
```

**Do not** leave the caption as a separate `embed` and assign `btnOk.text = ...` from the logic wrapper — that severs the design from the source: changing the caption in the design no longer reaches the running app without a code edit. Use `textSource` so re-export is the only step needed.

`imageSource` works the same way for native controls that need an icon/background asset from a sibling image layer.

#### Tappable design layers — image / icon that should react to touch

For embedded-style touchscreen UIs (custom artwork, no `QtQuick.Controls` look), this is the most common pattern: keep the layer's **image as the visual**, and make it tappable.

```
set_export_hint(layerId=..., type="embed", options='{"id": "btnSettings", "interactive": true}')
```
Output:
```qml
MouseArea { id: btnSettings; x: ...; y: ...; Image { source: "images/icon.png" } }
```
The image is preserved as the MouseArea's child, so the visual still renders and the whole region detects taps. Wire `btnSettings.onClicked: ...` from the logic wrapper. Do **not** add a separate `MouseArea` in the logic wrapper to make this layer tappable — the export already includes one.

#### Bare hit-only region (no visual) — extending or detaching the click area

When you need a tap region that has no visual content of its own (e.g. an enlarged hit-box that extends beyond an icon, or a transparent overlay):
```
set_export_hint(layerId=..., type="native", options='{"id": "btnSettingsHit", "baseElement": "TouchArea"}')
```
Output:
```qml
MouseArea { id: btnSettingsHit; x: ...; y: ...; }   // no children — pure hit area
```
The layer's own image / text is dropped. Use this only when you specifically want **no visual**; otherwise prefer the previous `embed + interactive: true` pattern.

#### Text labels (exposed for runtime override)
```
set_export_hint(layerId=..., type="embed", options='{"id": "labelDeviceName", "properties": ["text"]}')
```
The design's text is the placeholder; logic code overrides it via `labelDeviceName.text: device.name`. Without `"text"` in `properties`, the text is baked in as a literal and cannot be overridden.

#### `properties` vocabulary

`properties` selects which design attributes the exporter exposes as **bindable property aliases on the parent component**, so logic code can override the design's placeholder values. Add a property name only when the runtime needs to override the design's value, or when something on the layer is a placeholder you'll replace:

| value | exposes for binding |
|---|---|
| `visible` | hide / show the layer at runtime |
| `position` | `x` / `y` (mutually exclusive with `anchorMode`) |
| `size` | `width` / `height` |
| `color` | text or shape color |
| `text` | text content of a Text layer |
| `font` | font family / pixel size of a Text layer |
| `image` | source image of an Image layer |

`translatable` is **not** an alias — it changes the output template so the literal string is wrapped in `qsTr("...")` (QtQuick) or `@tr("...")` (Slint). Apply it to standalone Text layers, or to a Native Button whose caption comes via `textSource`:

**Standalone Text layer:**
```
set_export_hint(layerId=..., type="embed", options='{"id": "labelDeviceName", "properties": ["text", "translatable"]}')
```

**Native Button caption (set `translatable` on the Button itself, not on the textSource layer):**
```
set_export_hint(layerId=<buttonLayerId>, type="native", options='{"id": "btnOk", "baseElement": "Button", "textSource": "OK Label", "properties": ["translatable"]}')
```

#### Anchored elements (parent-relative positioning instead of absolute x/y)
```
set_export_hint(layerId=..., type="embed", options='{"id": "elementName", "anchorMode": "center"}')
```
Anchor modes: `none`, `topLeft`, `top`, `topRight`, `left`, `center`, `right`, `bottomLeft`, `bottom`, `bottomRight`.
Note: `anchorMode` and the `position` property are mutually exclusive.

#### Helper / reference layers — exclude from export
```
set_export_hint(layerId=..., type="skip", options='{}')
```
Use for guide grids, comments, off-canvas reference art, or anything that should not appear in the generated UI.

### 4. Save hints

```
save_hints()
```

This persists export hints to a `.psd_` sidecar file next to the PSD.

### 5. Run the export

Export directly into the project's `qml/design/` so the generated files line up with the layout in step 6 (no manual moving afterwards):
```
do_export(format="<exporter key>", outputDir="qml/design", options='{}')
```

If the format has not been chosen yet, call `list_exporters` to see available formats and ask the user to pick one.

The exporter writes:
- one `<ComponentName>.ui.qml` per `type:"custom"` (screen / reusable widget)
- an `images/` subdirectory under `outputDir/` containing every PNG/JPG asset the design references; the `.ui.qml` files refer to them by relative path (`source: "images/foo.png"`), so the layout is self-contained as long as `images/` stays next to the `.ui.qml` files

After export, confirm there is **one `.ui.qml` per screen frame** (e.g. `HomeScreen.ui.qml`, `SettingsScreen.ui.qml`). A single oversized file means a `type:"custom"` hint is missing somewhere — go back to step 3.

### 6. Integrate generated files

Use a **design/logic separation** pattern so re-export only ever touches `design/`, never `logic/`. The two-tier shape also makes every design file independently previewable.

**Core principle — logic is owned by the component, not by the parent:**
Each screen and each reusable widget owns its own logic in its own `logic/X.qml` wrapper. **Do not** funnel everything up into a single `Main.qml` that reaches into children with long property-alias chains (`mainWindow.statusBar.wifiToggle.checked: ...`). A button knows how to be a button; the screen that hosts it should not be wiring the button's internals.

**Recommended project structure (QML example):**
```
qml/
  design/                    # Exported .ui.qml files — never hand-edit, always re-exportable
    HomeScreen.ui.qml
    SettingsScreen.ui.qml
    StatusBar.ui.qml
    ButtonPower.ui.qml
  logic/                     # Hand-written wrappers — inherit the matching UI and add behavior
    HomeScreen.qml
    SettingsScreen.qml
    StatusBar.qml
  qmldir                     # Type registration — public names hide the UI/non-UI split
```

**qmldir** has three registration patterns. Use whichever fits each component:
```
# (a) Has logic wrapper — register both, public name points at the wrapper
HomeScreenUI     1.0 design/HomeScreen.ui.qml
HomeScreen       1.0 logic/HomeScreen.qml
StatusBarUI      1.0 design/StatusBar.ui.qml
StatusBar        1.0 logic/StatusBar.qml

# (b) Pure design (no behavior needed) — register the .ui.qml directly under the public name
ButtonPower      1.0 design/ButtonPower.ui.qml

# (c) Pure logic (no design source — e.g. a code-only dialog or container)
LeftWindow       1.0 logic/LeftWindow.qml
```

**Cross-component references use the public name.**
A design file composes children by their public name (no `UI` suffix). The qmldir resolves `StatusBar` to `logic/StatusBar.qml` at runtime, **and to the same wrapper in standalone preview**. Because every wrapper is structured `XUI { ... }`, the layout shows correctly either way — undefined bindings just leave properties at their defaults.

```qml
// design/HomeScreen.ui.qml — references children by public name
import QtQuick                          // imports stay framework-only (see below)

Item {
    StatusBar { id: header; ... }       // resolves via qmldir → logic/StatusBar.qml at runtime
    SettingsScreen { id: settings; ... } // works for nested screens too
    ButtonPower { id: btnPower; ... }    // pure-design components: same name, no wrapper
}
```

**Design files import only framework modules** — `QtQuick`, `QtQuick.Controls`, `QtQuick.Shapes`, etc. Never import your project's own QML module from a `.ui.qml`. That is what keeps `qml6 design/HomeScreen.ui.qml` openable standalone.

**Logic wrapper** (`logic/HomeScreen.qml`) inherits from the matching `XUI` and wires behavior **declaratively only**:
```qml
import QtQuick
import MyApp                            // project module is fine in logic/

HomeScreenUI {
    id: root

    // Property bindings into the design — never imperative assignment
    labelDeviceName.text: device.name
    wifiToggle.checked: network.wifiOn

    // Declarative signal handlers — never signal.connect()
    btnSettings.onClicked: stack.push("SettingsScreen.qml")
    wifiToggle.onToggled: network.setWifi(wifiToggle.checked)

    // Logic-only sub-objects (timers, models, helper Items) belong here, not in design/
    Timer {
        interval: 60000; running: true; repeat: true; triggeredOnStart: true
        onTriggered: root.currentTime = Qt.formatTime(new Date(), "HH:mm")
    }
}
```

**Forbidden patterns in the logic wrapper:**

```qml
// ❌ Never: imperative assignment in Component.onCompleted
Component.onCompleted: {
    labelDeviceName.text = device.name
    btnSettings.clicked.connect(openSettings)
}

// ❌ Never: overlaying a MouseArea on top of an embedded design item to make it tappable.
//          If a region needs to react to touch, set its export hint to type="embed" with
//          interactive:true (or baseElement:"TouchArea") and re-export — do not patch over
//          the design layer from logic.
//          (A top-level MouseArea covering the whole wrapper is fine if the entire
//          component is itself the click target — that is a logic-level concept, not a
//          design-layer one.)
MouseArea { anchors.fill: someEmbeddedItem; onClicked: ... }

// ❌ Never: long alias chains drilling into another component's internals from a parent
homeScreen.statusBar.wifiToggle.onToggled: ...
// ✓ Instead, that toggle is StatusBar's job — wire it inside logic/StatusBar.qml.
```

This way:
- `design/HomeScreen.ui.qml` can be re-exported at any time without touching `logic/`
- Each `logic/X.qml` lives next to the design it wraps; behavior stays close to the visuals it drives
- Every design file is openable for visual review with `qml6` — note that "standalone" here means *standalone within the project*: launch `qml6` from the directory that contains `qmldir` (or via an import path that includes it) so cross-references like `StatusBar` resolve. Bindings to project services may evaluate to `undefined` in preview, but the layout still renders.
- Consumers just write `HomeScreen {}` and get the fully-wired component — they neither know nor care that `HomeScreenUI` exists

**Dynamic content (lists, repeaters) is logic's responsibility.**
The exporter does not generate `ListView` / `Repeater` / `ListModel` from the design — there is no "this is a list template" hint. The design captures one representative item; you re-implement the view in `logic/`:
```qml
// design/DeviceListScreen.ui.qml exposes deviceCard as a single instance, plus a container
// logic/DeviceListScreen.qml replaces the static placeholder with a model-driven view
DeviceListScreenUI {
    listContainer.children: ListView {
        anchors.fill: parent
        model: deviceListModel
        delegate: DeviceCard { name: model.name; status: model.status }   // DeviceCard is its own type:"custom" export
    }
}
```
Treat the design's repeated-looking elements as **delegates**: export one of them as a `type:"custom"` component, then drive instantiation from logic.

**Re-skinning native controls** — when the export uses `type:"native"`, the generated `.ui.qml` references stock `QtQuick.Controls` types (`Switch`, `ComboBox`, etc.). Restyle them by adding a Qt Quick Controls 2 style module:

```
qml/MyStyle/             # one .qml per type (Switch.qml, ComboBox.qml, ...)
  Switch.qml             # uses T.Switch from QtQuick.Templates, draws the design's PNG assets
  ComboBox.qml
  qmldir                 # `module MyStyle` + lines like `Switch 1.0 Switch.qml`
```

In `main.cpp`, activate the style before loading QML:

```cpp
#include <QQuickStyle>
...
QQuickStyle::setStyle("MyStyle");
```

This keeps the design files re-exportable and lets the standard control APIs (`model`, `value`, `checked`, `currentIndex`, ...) drive the runtime — no custom wrappers needed.

### 7. Verify

Build or preview to confirm the layout and interactions work correctly.

## Important notes

- Never hand-edit `.ui.qml` files. Re-export if changes are needed.
- All logic and event handlers belong in the logic wrapper, not in `.ui.qml`.
- One `type:"custom"` per screen / per reusable widget — never let multiple screens collapse into one `.ui.qml`.
- **Logic is owned locally**: each component has its own `logic/X.qml`. Do not centralize behavior in `Main.qml` with deep alias chains like `mainWindow.statusBar.wifiToggle.onToggled: ...` — wire the toggle inside `logic/StatusBar.qml` instead.
- Design files (`design/*.ui.qml`) reference children by their **public name** (no `UI` suffix); the qmldir resolves it to the logic wrapper at both runtime and standalone preview.
- Design files import only framework modules (`QtQuick`, `QtQuick.Controls`, `QtQuick.Shapes`, ...) — never project modules. That keeps every `.ui.qml` openable standalone.
- Names come from the layer's role (`HomeScreen`, `wifiToggle`); numeric suffixes (`Screen1`, `btn3`) are forbidden.
- Button captions go through `textSource`, never through imperative `.text = ...` assignment in the logic wrapper.
- Tappable design layers (image/icon + tap) go through `type:"embed"` + `interactive:true` (or equivalently `baseElement:"TouchArea"`); bare hit-only regions go through `type:"native"` + `baseElement:"TouchArea"`. Never overlay a `MouseArea` from the logic wrapper to make an embedded item tappable.
- Choosing `type:"native"` for stock-control nodes is a tradeoff: you get framework semantics (`checked`, `model`, `value`, ...) but must build a `QQuickStyle` module to match the design. `type:"embed"` keeps the exact visuals but you re-implement state. Decide per-control, not as a blanket rule.
- Logic wrappers use property bindings and declarative `onXxx:` handlers only — no `Component.onCompleted` imperative assignment, no `signal.connect()`.
- `properties` selects which design attributes are exposed for runtime override (visible / position / size / color / text / font / image). `translatable` is special: it changes the output template to wrap text in `qsTr(...)` / `@tr(...)`.
- Dynamic lists are not exported as `ListView` / `Repeater`; export one delegate as `type:"custom"` and instantiate the view in the logic wrapper.
- Export with `outputDir="qml/design"` so the result drops straight into the project layout. Asset images land in `qml/design/images/` and are referenced relatively by the `.ui.qml` files.
- `type: "embed"` inlines the layer into the parent component file.
- `type: "custom"` generates a separate reusable component file.
- qmldir registers three patterns: `XUI`+`X` (has logic wrapper), `X` → `design/X.ui.qml` (pure design), `X` → `logic/X.qml` (pure logic, no design source).
)"_s);

            QMcpPromptMessageContent content(text);
            QMcpPromptMessage message;
            message.setRole(QMcpRole::user);
            message.setContent(content);

            session->appendPrompt(prompt, message);
        });
    }

    // ── PSD loading ─────────────────────────────────────────────────

    Q_INVOKABLE QString load_psd(const QString &path)
    {
        exporterModel.load(path);
        const auto err = exporterModel.errorMessage();
        if (!err.isEmpty())
            return toJson(QJsonObject{{"error"_L1, err}});

        const auto sz = exporterModel.size();
        return toJson(QJsonObject{
            {"file"_L1, exporterModel.fileName()},
            {"width"_L1, sz.width()},
            {"height"_L1, sz.height()},
            {"layerCount"_L1, countLayers({})}
        });
    }

    // ── Figma import ────────────────────────────────────────────────

    Q_INVOKABLE QString import_figma(const QString &source, const QString &options)
    {
        auto *importer = QPsdImporterPlugin::plugin("figma");
        if (!importer)
            return toJson(QJsonObject{{"error"_L1, "Figma importer plugin not available. Ensure the plugin is built and QT_PLUGIN_PATH is set."_L1}});

        QVariantMap opts = buildFigmaOptions(source, options);

        if (!importer->importFrom(&exporterModel, opts))
            return toJson(QJsonObject{{"error"_L1, "Figma import failed. Check that the URL/file key and API token are correct."_L1}});

        const auto sz = exporterModel.size();
        return toJson(QJsonObject{
            {"file"_L1, exporterModel.fileName()},
            {"width"_L1, sz.width()},
            {"height"_L1, sz.height()},
            {"layerCount"_L1, countLayers({})}
        });
    }

    Q_INVOKABLE QString list_figma_pages(const QString &source, const QString &options)
    {
        auto *importer = QPsdImporterPlugin::plugin("figma");
        if (!importer)
            return toJson(QJsonObject{{"error"_L1, "Figma importer plugin not available"_L1}});

        QVariantMap opts = buildFigmaOptions(source, options);

        if (!importer->importFrom(&exporterModel, opts))
            return toJson(QJsonObject{{"error"_L1, "Failed to fetch Figma file. Check URL and API token."_L1}});

        const auto sz = exporterModel.size();
        return toJson(QJsonObject{
            {"file"_L1, exporterModel.fileName()},
            {"width"_L1, sz.width()},
            {"height"_L1, sz.height()},
            {"layerCount"_L1, countLayers({})},
            {"note"_L1, "Use import_figma with pageIndex in options to select a specific page"_L1}
        });
    }

    // ── Layer inspection ────────────────────────────────────────────

    Q_INVOKABLE QString get_layer_tree()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No design file loaded"_L1}});

        QJsonArray tree;
        buildTree({}, tree);
        return toJson(QJsonObject{
            {"file"_L1, exporterModel.fileName()},
            {"layers"_L1, tree}
        });
    }

    Q_INVOKABLE QString get_layer_details(int layerId)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 not found"_s.arg(layerId)}});

        QJsonObject obj;
        obj["layerId"_L1] = exporterModel.layerId(index);
        obj["name"_L1] = exporterModel.layerName(index);
        const auto r = exporterModel.rect(index);
        obj["rect"_L1] = QJsonObject{
            {"x"_L1, r.x()}, {"y"_L1, r.y()},
            {"width"_L1, r.width()}, {"height"_L1, r.height()}
        };

        const auto *item = exporterModel.layerItem(index);
        if (item) {
            obj["opacity"_L1] = item->opacity();
            obj["fillOpacity"_L1] = item->fillOpacity();

            switch (item->type()) {
            case QPsdAbstractLayerItem::Text: {
                obj["type"_L1] = "text"_L1;
                const auto *text = static_cast<const QPsdTextLayerItem *>(item);
                QJsonArray runs;
                for (const auto &run : text->runs()) {
                    runs.append(QJsonObject{
                        {"text"_L1, run.text},
                        {"font"_L1, run.font.family()},
                        {"originalFont"_L1, run.originalFontName},
                        {"fontSize"_L1, run.font.pointSizeF()},
                        {"color"_L1, run.color.name()},
                    });
                }
                obj["runs"_L1] = runs;
                obj["textType"_L1] = text->textType() == QPsdTextLayerItem::TextType::PointText
                    ? "point"_L1 : "paragraph"_L1;
                break;
            }
            case QPsdAbstractLayerItem::Shape: {
                obj["type"_L1] = "shape"_L1;
                const auto *shape = static_cast<const QPsdShapeLayerItem *>(item);
                const auto pi = shape->pathInfo();
                static const char *pathTypes[] = {"none", "rectangle", "roundedRectangle", "path"};
                obj["pathType"_L1] = QString::fromLatin1(pathTypes[pi.type]);
                if (pi.type == QPsdAbstractLayerItem::PathInfo::RoundedRectangle)
                    obj["cornerRadius"_L1] = pi.radius;
                obj["brushColor"_L1] = shape->brush().color().name();
                break;
            }
            case QPsdAbstractLayerItem::Image: {
                obj["type"_L1] = "image"_L1;
                const auto lf = item->linkedFile();
                if (!lf.name.isEmpty())
                    obj["linkedFile"_L1] = lf.name;
                break;
            }
            case QPsdAbstractLayerItem::Folder: {
                obj["type"_L1] = "folder"_L1;
                const auto *folder = static_cast<const QPsdFolderLayerItem *>(item);
                obj["isOpened"_L1] = folder->isOpened();
                if (!folder->artboardPresetName().isEmpty()) {
                    obj["artboard"_L1] = QJsonObject{
                        {"presetName"_L1, folder->artboardPresetName()},
                        {"background"_L1, folder->artboardBackground().name()},
                    };
                }
                obj["childCount"_L1] = exporterModel.rowCount(index);
                break;
            }
            }
        }

        // Export hint
        const auto hint = exporterModel.layerHint(index);
        QJsonObject hintObj;
        hintObj["type"_L1] = hintTypeName(hint.type);
        if (!hint.id.isEmpty())
            hintObj["id"_L1] = hint.id;
        if (!hint.componentName.isEmpty())
            hintObj["componentName"_L1] = hint.componentName;
        if (hint.type == QPsdExporterTreeItemModel::ExportHint::Native)
            hintObj["baseElement"_L1] = QPsdExporterTreeItemModel::ExportHint::nativeCode2Name(hint.baseElement);
        hintObj["visible"_L1] = hint.visible;
        if (hint.interactive)
            hintObj["interactive"_L1] = true;
        if (!hint.properties.isEmpty()) {
            QJsonArray propsArr;
            for (const auto &prop : hint.properties)
                propsArr.append(prop);
            hintObj["properties"_L1] = propsArr;
        }
        if (hint.anchorMode != QPsdExporterTreeItemModel::ExportHint::AnchorNone)
            hintObj["anchorMode"_L1] = anchorModeName(hint.anchorMode);
        obj["exportHint"_L1] = hintObj;

        return toJson(obj);
    }

    // ── Export hints ────────────────────────────────────────────────

    Q_INVOKABLE QString set_export_hint(int layerId, const QString &type, const QString &options)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 not found"_s.arg(layerId)}});

        static const QHash<QString, QPsdExporterTreeItemModel::ExportHint::Type> typeMap = {
            {"embed"_L1,  QPsdExporterTreeItemModel::ExportHint::Embed},
            {"merge"_L1,  QPsdExporterTreeItemModel::ExportHint::Merged},
            {"custom"_L1, QPsdExporterTreeItemModel::ExportHint::Component},
            {"native"_L1, QPsdExporterTreeItemModel::ExportHint::Native},
            {"skip"_L1,   QPsdExporterTreeItemModel::ExportHint::Skip},
        };

        const auto lower = type.toLower();
        if (!typeMap.contains(lower))
            return toJson(QJsonObject{{"error"_L1, u"Unknown type: %1. Use: embed, merge, custom, native, skip"_s.arg(type)}});

        const auto opts = QJsonDocument::fromJson(options.toUtf8()).object();

        auto hint = exporterModel.layerHint(index);
        hint.type = typeMap.value(lower);
        if (opts.contains("id"_L1))
            hint.id = opts["id"_L1].toString();
        if (opts.contains("visible"_L1))
            hint.visible = opts["visible"_L1].toBool();
        if (opts.contains("componentName"_L1) && !opts["componentName"_L1].toString().isEmpty())
            hint.componentName = opts["componentName"_L1].toString();
        if (opts.contains("baseElement"_L1) && !opts["baseElement"_L1].toString().isEmpty())
            hint.baseElement = QPsdExporterTreeItemModel::ExportHint::nativeName2Code(opts["baseElement"_L1].toString());
        if (opts.contains("interactive"_L1))
            hint.interactive = opts["interactive"_L1].toBool();
        if (hint.baseElement == QPsdExporterTreeItemModel::ExportHint::TouchArea)
            hint.interactive = true;
        if (opts.contains("properties"_L1)) {
            hint.properties.clear();
            const auto propsArr = opts["properties"_L1].toArray();
            for (const auto &val : propsArr)
                hint.properties.insert(val.toString());
        }
        if (opts.contains("textSource"_L1))
            hint.textSource = opts["textSource"_L1].toString();
        if (opts.contains("imageSource"_L1))
            hint.imageSource = opts["imageSource"_L1].toString();
        if (opts.contains("anchorMode"_L1)) {
            static const QHash<QString, QPsdExporterTreeItemModel::ExportHint::AnchorMode> anchorMap = {
                {"none"_L1,        QPsdExporterTreeItemModel::ExportHint::AnchorNone},
                {"topLeft"_L1,     QPsdExporterTreeItemModel::ExportHint::AnchorTopLeft},
                {"top"_L1,         QPsdExporterTreeItemModel::ExportHint::AnchorTop},
                {"topRight"_L1,    QPsdExporterTreeItemModel::ExportHint::AnchorTopRight},
                {"left"_L1,        QPsdExporterTreeItemModel::ExportHint::AnchorLeft},
                {"center"_L1,      QPsdExporterTreeItemModel::ExportHint::AnchorCenter},
                {"right"_L1,       QPsdExporterTreeItemModel::ExportHint::AnchorRight},
                {"bottomLeft"_L1,  QPsdExporterTreeItemModel::ExportHint::AnchorBottomLeft},
                {"bottom"_L1,      QPsdExporterTreeItemModel::ExportHint::AnchorBottom},
                {"bottomRight"_L1, QPsdExporterTreeItemModel::ExportHint::AnchorBottomRight},
            };
            hint.anchorMode = anchorMap.value(opts["anchorMode"_L1].toString(), QPsdExporterTreeItemModel::ExportHint::AnchorNone);
        }

        exporterModel.setLayerHint(index, hint);

        QJsonArray propsArr;
        for (const auto &prop : hint.properties)
            propsArr.append(prop);
        return toJson(QJsonObject{
            {"layerId"_L1, layerId},
            {"id"_L1, hint.id},
            {"type"_L1, lower},
            {"componentName"_L1, hint.componentName},
            {"baseElement"_L1, QPsdExporterTreeItemModel::ExportHint::nativeCode2Name(hint.baseElement)},
            {"visible"_L1, hint.visible},
            {"interactive"_L1, hint.interactive},
            {"properties"_L1, propsArr},
            {"textSource"_L1, hint.textSource},
            {"imageSource"_L1, hint.imageSource},
            {"anchorMode"_L1, anchorModeName(hint.anchorMode)},
        });
    }

    // ── Export ───────────────────────────────────────────────────────

    Q_INVOKABLE QString do_export(const QString &format, const QString &outputDir, const QString &options)
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No design file loaded"_L1}});

        auto *plugin = QPsdExporterPlugin::plugin(format.toUtf8());
        if (!plugin)
            return toJson(QJsonObject{{"error"_L1, u"Unknown exporter: %1"_s.arg(format)}});

        QDir outDir(outputDir);
        if (!outDir.exists() && !outDir.mkpath("."_L1))
            return toJson(QJsonObject{{"error"_L1, u"Cannot create directory: %1"_s.arg(outputDir)}});

        const auto opts = QJsonDocument::fromJson(options.toUtf8()).object();
        const int optW = opts["width"_L1].toInt(0);
        const int optH = opts["height"_L1].toInt(0);

        QPsdExporterPlugin::ExportConfig config;
        if (optW > 0 || optH > 0) {
            const auto sz = exporterModel.size();
            config.targetSize = QSize(optW > 0 ? optW : sz.width(),
                                     optH > 0 ? optH : sz.height());
        }
        config.fontScaleFactor = opts["fontScaleFactor"_L1].toDouble(1.0);
        config.imageScaling = opts["imageScaling"_L1].toBool(false);
        config.makeCompact = opts["makeCompact"_L1].toBool(false);
        config.artboardToOrigin = opts["artboardToOrigin"_L1].toBool(false);
        if (opts.contains("licenseText"_L1))
            config.licenseText = opts["licenseText"_L1].toString();

        // Set plugin-specific properties (e.g. effectMode for QtQuick)
        if (opts.contains("effectMode"_L1)) {
            const auto mode = opts["effectMode"_L1].toString();
            // Map string to enum via Qt meta-object
            const auto *mo = plugin->metaObject();
            int propIndex = mo->indexOfProperty("effectMode");
            if (propIndex >= 0) {
                const auto prop = mo->property(propIndex);
                const auto enumerator = prop.enumerator();
                int enumValue = enumerator.keyToValue(mode.toLatin1().constData());
                if (enumValue >= 0)
                    plugin->setProperty("effectMode", enumValue);
            }
        }

        if (!plugin->exportTo(&exporterModel, outputDir, config))
            return toJson(QJsonObject{{"error"_L1, "Export failed"_L1}});

        const QSize reported = config.targetSize.isEmpty()
            ? exporterModel.size()
            : config.targetSize;
        return toJson(QJsonObject{
            {"format"_L1, format},
            {"outputDir"_L1, outputDir},
            {"width"_L1, reported.width()},
            {"height"_L1, reported.height()},
        });
    }

    Q_INVOKABLE QString list_exporters()
    {
        QJsonArray arr;
        for (const auto &key : QPsdExporterPlugin::keys()) {
            auto *plugin = QPsdExporterPlugin::plugin(key);
            if (!plugin)
                continue;
            arr.append(QJsonObject{
                {"key"_L1, QString::fromUtf8(key)},
                {"name"_L1, plugin->name()},
                {"type"_L1, plugin->exportType() == QPsdExporterPlugin::Directory
                     ? "directory"_L1 : "file"_L1},
            });
        }
        return toJson(QJsonObject{{"exporters"_L1, arr}});
    }

    Q_INVOKABLE QString save_hints()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No design file loaded"_L1}});

        exporterModel.save();
        return toJson(QJsonObject{{"saved"_L1, true}});
    }

    // ── Layer image ─────────────────────────────────────────────────

    Q_INVOKABLE QImage get_layer_image(int layerId)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return {};

        const auto *item = exporterModel.layerItem(index);
        if (!item)
            return {};

        if (item->type() != QPsdAbstractLayerItem::Folder)
            return item->image();

        // Folder layer: composite all visible children
        const QRect bounds = computeBoundingRect(index);
        if (bounds.isEmpty())
            return {};

        QImage canvas(bounds.size(), QImage::Format_ARGB32);
        canvas.fill(Qt::transparent);

        QPainter painter(&canvas);
        const auto blendMode = item->record().blendMode();
        const bool passThrough = (blendMode == QPsdBlend::PassThrough);
        compositeChildren(index, painter, bounds.topLeft(), passThrough);
        painter.end();

        return canvas;
    }

    // ── Fonts ───────────────────────────────────────────────────────

    Q_INVOKABLE QString get_fonts_used()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No design file loaded"_L1}});

        QSet<QString> seen;
        QJsonArray fonts;
        collectFonts({}, seen, fonts);
        return toJson(QJsonObject{{"fonts"_L1, fonts}});
    }

    Q_INVOKABLE QString get_font_mappings()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No design file loaded"_L1}});

        auto *mapper = QPsdFontMapper::instance();

        QJsonObject global;
        const auto globalMap = mapper->globalMappings();
        for (auto it = globalMap.cbegin(); it != globalMap.cend(); ++it)
            global[it.key()] = it.value();

        QJsonObject context;
        const auto contextMap = mapper->contextMappings(exporterModel.fileName());
        for (auto it = contextMap.cbegin(); it != contextMap.cend(); ++it)
            context[it.key()] = it.value();

        return toJson(QJsonObject{
            {"global"_L1, global},
            {"context"_L1, context},
        });
    }

    Q_INVOKABLE QString set_font_mapping(const QString &fromFont, const QString &toFont, bool global)
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No design file loaded"_L1}});

        auto *mapper = QPsdFontMapper::instance();

        if (global) {
            if (toFont.isEmpty())
                mapper->removeGlobalMapping(fromFont);
            else
                mapper->setGlobalMapping(fromFont, toFont);
            mapper->saveGlobalMappings();
        } else {
            auto mappings = mapper->contextMappings(exporterModel.fileName());
            if (toFont.isEmpty())
                mappings.remove(fromFont);
            else
                mappings[fromFont] = toFont;
            mapper->setContextMappings(exporterModel.fileName(), mappings);
        }

        return toJson(QJsonObject{
            {"fromFont"_L1, fromFont},
            {"toFont"_L1, toFont},
            {"global"_L1, global},
        });
    }

    QHash<QString, QString> toolDescriptions() const override
    {
        return {
            {"load_psd"_L1, "Load a PSD file for inspection and export"_L1},
            {"load_psd/path"_L1, "Absolute path to the PSD file"_L1},

            {"import_figma"_L1, "Import a Figma design for inspection and export. Requires FIGMA_API_KEY or FIGMA_ACCESS_TOKEN environment variable, or pass apiKey in options."_L1},
            {"import_figma/source"_L1, "Figma URL (https://www.figma.com/design/FILE_KEY/...) or file key"_L1},
            {"import_figma/options"_L1, "JSON object with optional keys: apiKey (string, Figma API token), imageScale (int 1-4, default 2), pageIndex (int 0-based, default 0), hintFile (string, path to .psd_ hints file)"_L1},

            {"list_figma_pages"_L1, "List pages in a Figma file. Imports the file and returns info including page count."_L1},
            {"list_figma_pages/source"_L1, "Figma URL or file key"_L1},
            {"list_figma_pages/options"_L1, "JSON object with optional keys: apiKey (string, Figma API token)"_L1},

            {"get_layer_tree"_L1, "Get the layer tree structure of the loaded design file"_L1},

            {"get_layer_details"_L1, "Get detailed information about a specific layer"_L1},
            {"get_layer_details/layerId"_L1, "Layer ID to inspect"_L1},

            {"set_export_hint"_L1, "Configure how a layer should be exported"_L1},
            {"set_export_hint/layerId"_L1, "Layer ID to configure"_L1},
            {"set_export_hint/type"_L1, "Export type: embed (inline into the parent .ui.qml), merge (composite into a single image), custom (emit a separate reusable component — name it via componentName), native (emit a stock framework control — pick the type via baseElement), skip (omit from export)"_L1},
            {"set_export_hint/options"_L1, "JSON object with optional keys: "
                                            "id (string, identifier for binding — empty string to clear); "
                                            "visible (bool, initial visibility of the layer); "
                                            "componentName (string, for type=custom — name of the generated reusable component type. Note: the parent's instance id is set by whoever embeds this component, not in the hint); "
                                            "baseElement (string, for type=native — stock framework control. The QtQuick exporter maps each value to a QtQuick or QtQuick.Controls type: "
                                            "Container (Item), TouchArea (MouseArea — invisible hit-only region; the layer's own visuals are NOT rendered), "
                                            "Button, Button_Highlighted (same Button type, but with highlighted:true / Slint primary:true — for the visually emphasized variant), "
                                            "CheckBox, ComboBox, RadioButton, Slider, SpinBox, Switch, TabBar, TabButton. "
                                            "Prefer type=native+baseElement whenever a Figma node corresponds to a stock UI control (toggle, dropdown, tab segment, numeric stepper, etc.) — let the design's visuals live in a Qt Quick Controls 2 style module activated via QQuickStyle::setStyle, instead of writing a type=custom wrapper that re-implements standard semantics like model/value/checked); "
                                            "interactive (bool, for type=embed — wraps the layer's rendered content in a MouseArea so the visual stays AND the region becomes tappable. Use this for icons/cards that should react to touch. Distinct from type=native+baseElement=TouchArea, which produces a bare MouseArea with no visual. Setting baseElement=TouchArea on any type also implies interactive=true); "
                                            "properties (array of strings — selects which design attributes are exposed as bindable property aliases on the parent component, so logic code can override the design's placeholder value at runtime. Vocabulary: visible, position (x/y), size (width/height), color, text, font, image. Special: translatable (not an alias — wraps the layer's text literal in qsTr(\"...\") for QtQuick or @tr(\"...\") for Slint; applies to standalone Text layers and to a Native Button whose caption comes from a textSource layer)); "
                                            "textSource (string, for type=native baseElement=Button/Button_Highlighted — name of a sibling text layer whose string becomes the Button's text. Use this instead of leaving the caption as a separate embed and assigning btn.text from logic code: with textSource, the design's text changes flow straight to the .ui.qml on re-export); "
                                            "imageSource (string, for type=native — name of a sibling image layer that supplies the control's icon/background asset, analogous to textSource); "
                                            "anchorMode (string: none, topLeft, top, topRight, left, center, right, bottomLeft, bottom, bottomRight — parent-relative positioning; mutually exclusive with the position property)"_L1},

            {"do_export"_L1, "Export the loaded design to a target format and directory"_L1},
            {"do_export/format"_L1, "Exporter plugin key (use list_exporters to see available ones)"_L1},
            {"do_export/outputDir"_L1, "Absolute path to the output directory"_L1},
            {"do_export/options"_L1, "JSON object with optional keys: width (int), height (int), fontScaleFactor (double), imageScaling (bool), makeCompact (bool), artboardToOrigin (bool, shift artboard to 0,0), licenseText (string, license header for generated files), effectMode (string: NoGPU, Qt5Effects, EffectMaker — QtQuick exporter only, controls how visual effects like drop shadows are rendered). Width/height 0 or omitted = original size"_L1},

            {"list_exporters"_L1, "List all available exporter plugins"_L1},

            {"save_hints"_L1, "Persist current export hints to the design sidecar file"_L1},

            {"get_layer_image"_L1, "Get the rendered image of a specific layer"_L1},
            {"get_layer_image/layerId"_L1, "Layer ID to get the image from"_L1},

            {"get_fonts_used"_L1, "List all fonts used in the loaded design file with their resolved mappings"_L1},

            {"get_font_mappings"_L1, "Get current font mapping settings (global and per-design context)"_L1},

            {"set_font_mapping"_L1, "Set or remove a font mapping"_L1},
            {"set_font_mapping/fromFont"_L1, "Original font name from the design (e.g. MyriadPro-Bold)"_L1},
            {"set_font_mapping/toFont"_L1, "Target font name to map to (empty string to remove mapping)"_L1},
            {"set_font_mapping/global"_L1, "If true, applies globally; if false, applies only to the currently loaded design"_L1},
        };
    }

private:
    QPsdGuiLayerTreeItemModel guiModel;
    QPsdExporterTreeItemModel exporterModel;

    static QVariantMap buildFigmaOptions(const QString &source, const QString &options)
    {
        QVariantMap opts;
        opts["source"_L1] = source;

        // Resolve API key from environment
        QString apiKey = qEnvironmentVariable("FIGMA_API_KEY");
        if (apiKey.isEmpty())
            apiKey = qEnvironmentVariable("FIGMA_ACCESS_TOKEN");
        if (!apiKey.isEmpty())
            opts["apiKey"_L1] = apiKey;

        opts["imageScale"_L1] = 2;

        // Override with user-provided options
        if (!options.isEmpty()) {
            const auto userOpts = QJsonDocument::fromJson(options.toUtf8()).object();
            if (userOpts.contains("apiKey"_L1))
                opts["apiKey"_L1] = userOpts["apiKey"_L1].toString();
            if (userOpts.contains("imageScale"_L1))
                opts["imageScale"_L1] = userOpts["imageScale"_L1].toInt();
            if (userOpts.contains("pageIndex"_L1))
                opts["pageIndex"_L1] = userOpts["pageIndex"_L1].toInt();
            if (userOpts.contains("hintFile"_L1))
                opts["hintFile"_L1] = userOpts["hintFile"_L1].toString();
        }

        return opts;
    }

    QModelIndex findLayerById(qint32 id, const QModelIndex &parent = {}) const
    {
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            if (exporterModel.layerId(index) == id)
                return index;
            auto found = findLayerById(id, index);
            if (found.isValid())
                return found;
        }
        return {};
    }

    int countLayers(const QModelIndex &parent) const
    {
        int count = 0;
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            count++;
            count += countLayers(exporterModel.index(row, 0, parent));
        }
        return count;
    }

    void buildTree(const QModelIndex &parent, QJsonArray &array) const
    {
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            QJsonObject obj;
            obj["layerId"_L1] = exporterModel.layerId(index);
            obj["name"_L1] = exporterModel.layerName(index);
            const auto *item = exporterModel.layerItem(index);
            if (item) {
                switch (item->type()) {
                case QPsdAbstractLayerItem::Text:   obj["type"_L1] = "text"_L1;   break;
                case QPsdAbstractLayerItem::Shape:  obj["type"_L1] = "shape"_L1;  break;
                case QPsdAbstractLayerItem::Image:  obj["type"_L1] = "image"_L1;  break;
                case QPsdAbstractLayerItem::Folder: obj["type"_L1] = "folder"_L1; break;
                }
            }

            const auto hint = exporterModel.layerHint(index);
            obj["hintType"_L1] = hintTypeName(hint.type);
            obj["visible"_L1] = hint.visible;
            if (!hint.properties.isEmpty()) {
                QJsonArray propsArr;
                for (const auto &prop : hint.properties)
                    propsArr.append(prop);
                obj["properties"_L1] = propsArr;
            }

            if (exporterModel.rowCount(index) > 0) {
                QJsonArray children;
                buildTree(index, children);
                obj["children"_L1] = children;
            }

            array.append(obj);
        }
    }

    void collectFonts(const QModelIndex &parent, QSet<QString> &seen, QJsonArray &fonts) const
    {
        const auto psdPath = exporterModel.fileName();
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            const auto *item = exporterModel.layerItem(index);
            if (item && item->type() == QPsdAbstractLayerItem::Text) {
                const auto *text = static_cast<const QPsdTextLayerItem *>(item);
                for (const auto &run : text->runs()) {
                    if (!run.originalFontName.isEmpty() && !seen.contains(run.originalFontName)) {
                        seen.insert(run.originalFontName);
                        const auto resolved = QPsdFontMapper::instance()->resolveFont(run.originalFontName, psdPath);
                        fonts.append(QJsonObject{
                            {"psdFont"_L1, run.originalFontName},
                            {"resolvedFont"_L1, resolved.family()},
                            {"resolvedStyle"_L1, resolved.styleName()},
                        });
                    }
                }
            }
            collectFonts(index, seen, fonts);
        }
    }

    static QString hintTypeName(QPsdExporterTreeItemModel::ExportHint::Type t)
    {
        static const char *names[] = {"embed", "merge", "custom", "native", "skip"};
        return QString::fromLatin1(names[t]);
    }

    static QString anchorModeName(QPsdExporterTreeItemModel::ExportHint::AnchorMode m)
    {
        static const char *names[] = {
            "none", "topLeft", "top", "topRight",
            "left", "center", "right",
            "bottomLeft", "bottom", "bottomRight"
        };
        return QString::fromLatin1(names[m]);
    }

    // Recursively compute the bounding box of all child layers under `parent`
    QRect computeBoundingRect(const QModelIndex &parent) const
    {
        QRect bounds;
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            const auto *item = exporterModel.layerItem(index);
            if (!item || !item->isVisible())
                continue;
            if (item->type() == QPsdAbstractLayerItem::Folder) {
                bounds = bounds.united(computeBoundingRect(index));
            } else {
                bounds = bounds.united(item->rect());
            }
        }
        return bounds;
    }

    // Apply transparency mask and layer mask to a layer's image
    QImage applyMasks(const QPsdAbstractLayerItem *item) const
    {
        QImage image = item->image();
        if (image.isNull())
            return image;

        // Apply transparency mask for layers without built-in alpha
        const QImage transMask = item->transparencyMask();
        if (!transMask.isNull() && !image.hasAlphaChannel()) {
            image = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < qMin(image.height(), transMask.height()); ++y) {
                QRgb *imgLine = reinterpret_cast<QRgb *>(image.scanLine(y));
                const uchar *maskLine = transMask.constScanLine(y);
                for (int x = 0; x < qMin(image.width(), transMask.width()); ++x) {
                    imgLine[x] = qRgba(qRed(imgLine[x]), qGreen(imgLine[x]),
                                       qBlue(imgLine[x]), maskLine[x]);
                }
            }
        }

        // Apply raster layer mask if present
        const QImage layerMask = item->layerMask();
        if (!layerMask.isNull()) {
            const QRect maskRect = item->layerMaskRect();
            const QRect layerRect = item->rect();
            const int defaultColor = item->layerMaskDefaultColor();

            image = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); ++y) {
                QRgb *scanLine = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    const int maskX = (layerRect.x() + x) - maskRect.x();
                    const int maskY = (layerRect.y() + y) - maskRect.y();
                    int maskValue = defaultColor;
                    if (maskX >= 0 && maskX < layerMask.width() &&
                        maskY >= 0 && maskY < layerMask.height()) {
                        maskValue = qGray(layerMask.pixel(maskX, maskY));
                    }
                    const int alpha = qAlpha(scanLine[x]);
                    const int newAlpha = (alpha * maskValue) / 255;
                    scanLine[x] = qRgba(qRed(scanLine[x]), qGreen(scanLine[x]),
                                        qBlue(scanLine[x]), newAlpha);
                }
            }
        }

        return image;
    }

    // Recursively composite visible children onto the given painter.
    // `origin` is the top-left of the canvas in document coordinates.
    // `passThrough` means children are drawn directly (no intermediate buffer).
    void compositeChildren(const QModelIndex &parent, QPainter &painter,
                           const QPoint &origin, bool passThrough) const
    {
        const int count = exporterModel.rowCount(parent);
        // Iterate bottom-to-top (last row = bottommost layer in PSD model)
        for (int row = count - 1; row >= 0; --row) {
            auto index = exporterModel.index(row, 0, parent);
            const auto *item = exporterModel.layerItem(index);
            if (!item || !item->isVisible())
                continue;

            if (item->type() == QPsdAbstractLayerItem::Folder) {
                const auto folderBlend = item->record().blendMode();
                const bool folderPassThrough = (folderBlend == QPsdBlend::PassThrough);

                if (folderPassThrough) {
                    // PassThrough: children draw directly onto the current canvas
                    compositeChildren(index, painter, origin, true);
                } else {
                    // Non-PassThrough: composite children into an intermediate buffer
                    const QRect childBounds = computeBoundingRect(index);
                    if (childBounds.isEmpty())
                        continue;

                    QImage groupCanvas(childBounds.size(), QImage::Format_ARGB32);
                    groupCanvas.fill(Qt::transparent);

                    QPainter groupPainter(&groupCanvas);
                    compositeChildren(index, groupPainter, childBounds.topLeft(), false);
                    groupPainter.end();

                    // Draw the group buffer with the folder's blend mode and opacity
                    painter.save();
                    painter.setCompositionMode(QtPsdGui::compositionMode(folderBlend));
                    painter.setOpacity(painter.opacity() * item->opacity() * item->fillOpacity());
                    painter.drawImage(childBounds.topLeft() - origin, groupCanvas);
                    painter.restore();
                }
            } else {
                // Leaf layer: apply masks, then draw with blend mode and opacity
                QImage layerImage = applyMasks(item);
                if (layerImage.isNull())
                    continue;

                painter.save();
                painter.setCompositionMode(
                    QtPsdGui::compositionMode(item->record().blendMode()));
                painter.setOpacity(painter.opacity() * item->opacity() * item->fillOpacity());
                painter.drawImage(item->rect().topLeft() - origin, layerImage);
                painter.restore();
            }
        }
    }
};

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("mcp-design2gui"_L1);
    app.setApplicationVersion("1.0"_L1);
    app.setOrganizationName("Signal Slot Inc."_L1);
    app.setOrganizationDomain("signal-slot.co.jp"_L1);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption backendOption(QStringList() << "b"_L1 << "backend"_L1,
                                    "Backend to use (stdio/sse)."_L1,
                                    "backend"_L1, "stdio"_L1);
    parser.addOption(backendOption);

    QCommandLineOption addressOption(QStringList() << "a"_L1 << "address"_L1,
                                    "Address to listen on (host:port)."_L1,
                                    "address"_L1, "127.0.0.1:8000"_L1);
    parser.addOption(addressOption);

    QCommandLineOption apiKeyOption(QStringList() << "k"_L1 << "api-key"_L1,
                                    "Figma API key (sets FIGMA_API_KEY)."_L1,
                                    "key"_L1);
    parser.addOption(apiKeyOption);

    parser.process(app);

    if (parser.isSet(apiKeyOption))
        qputenv("FIGMA_API_KEY", parser.value(apiKeyOption).toUtf8());

    McpServer server(parser.value(backendOption));
    QObject::connect(&server, &QMcpServer::finished, &app, &QCoreApplication::quit);
    server.start(parser.value(addressOption));

    return app.exec();
}

#include "main.moc"
