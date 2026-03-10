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
#include <QtMcpCommon/QMcpPromptArgument>
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

            QMcpPromptArgument pathArg;
            pathArg.setName("path"_L1);
            pathArg.setDescription("Absolute path to a PSD file, or a Figma URL (https://www.figma.com/design/...)"_L1);
            pathArg.setRequired(true);

            QMcpPromptArgument formatArg;
            formatArg.setName("format"_L1);
            formatArg.setDescription("Export format key (e.g. QtQuick, Slint). Run list_exporters to see options."_L1);
            formatArg.setRequired(false);

            prompt.setArguments({pathArg, formatArg});

            QMcpTextContent text(uR"(# Export a Design to a GUI Framework

Arguments: path = {path}, format = {format}

## Steps

### 1. Load the design file

**For PSD files:**
```
load_psd(path={path})
```

**For Figma files:**
```
import_figma(source="{path}")
```
If importing from Figma, you may first call `list_figma_pages` to see available pages, then specify `pageIndex` in the options.

This returns file information including dimensions and layer count.

### 2. Inspect the layer tree

```
get_layer_tree()
```

Review the child layers and classify them:

- **Interactive elements (buttons, nav items, etc.)** - use `baseElement: "TouchArea"` to emit as a clickable area
- **Text labels (dynamically updated)** - assign an `id` so they become property aliases
- **Decorative elements** - no hint needed (exported by default)

Use `get_layer_details(layerId=...)` to inspect individual layers for more detail (text runs, shape info, linked files, etc.).
Use `get_layer_image(layerId=...)` to visually inspect a layer's rendered appearance.

### 3. Set export hints

**Root artboard (top-level folder):**
```
set_export_hint(layerId=..., type="custom", options='{"componentName": "ScreenName"}')
```

**Interactive elements (buttons, navigation, etc.):**
```
set_export_hint(layerId=..., type="embed", options='{"id": "buttonName", "baseElement": "TouchArea"}')
```

**Text labels:**
```
set_export_hint(layerId=..., type="embed", options='{"id": "labelName"}')
```

### 4. Save hints

```
save_hints()
```

This persists export hints to a `.psd_` sidecar file next to the PSD.

### 5. Run the export

```
do_export(format="{format}", outputDir=".", options='{}')
```

If `{format}` is empty, call `list_exporters` to see available formats and ask the user to choose one.

### 6. Integrate generated files

Use a **design/logic separation** pattern to keep exported files re-exportable while developing logic independently.

**Recommended project structure (QML example):**
```
qml/
  design/     # Exported .ui.qml files (never hand-edit)
    ScreenName.ui.qml
  logic/      # Logic wrappers
    ScreenName.qml
  qmldir      # Type registration
```

**qmldir** registers the design file with a `UI` suffix and the logic file as the public type:
```
ScreenNameUI 1.0 design/ScreenName.ui.qml
ScreenName   1.0 logic/ScreenName.qml
```

**Logic wrapper** (`logic/ScreenName.qml`) inherits from the design type:
```qml
ScreenNameUI {
    // Bind property aliases exposed by the design file
    labelName.text: root.someValue
    buttonName.onClicked: doSomething()
}
```

This way:
- `design/ScreenName.ui.qml` can be re-exported at any time without losing logic
- `logic/ScreenName.qml` develops independently against stable property aliases
- The design file remains openable standalone with `qml6` for visual preview
- Consumers just use `ScreenName {}` and get the complete component

For simple design-only components (buttons, icons, etc.) that need no logic wrapper, register them directly without the `UI` suffix:
```
ButtonPower 1.0 design/ButtonPower.ui.qml
```

### 7. Verify

Build or preview to confirm the layout and interactions work correctly.

## Important notes

- Never hand-edit `.ui.qml` files. Re-export if changes are needed.
- All logic and event handlers belong in the logic wrapper, not in `.ui.qml`.
- `type: "embed"` inlines the layer into the root component file.
- `type: "custom"` generates a separate reusable component file.
- Use `qmldir` type renaming (`ComponentUI` / `Component`) to connect design and logic layers.
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
        if (!hint.properties.isEmpty()) {
            QJsonArray propsArr;
            for (const auto &prop : hint.properties)
                propsArr.append(prop);
            hintObj["properties"_L1] = propsArr;
        }
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
        if (opts.contains("properties"_L1)) {
            hint.properties.clear();
            const auto propsArr = opts["properties"_L1].toArray();
            for (const auto &val : propsArr)
                hint.properties.insert(val.toString());
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
            {"properties"_L1, propsArr},
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
        const auto sz = exporterModel.size();
        const int w = opts["width"_L1].toInt(0) > 0 ? opts["width"_L1].toInt() : sz.width();
        const int h = opts["height"_L1].toInt(0) > 0 ? opts["height"_L1].toInt() : sz.height();

        QPsdExporterPlugin::ExportConfig config;
        config.targetSize = QSize(w, h);
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

        return toJson(QJsonObject{
            {"format"_L1, format},
            {"outputDir"_L1, outputDir},
            {"width"_L1, w},
            {"height"_L1, h},
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
            {"set_export_hint/type"_L1, "Export type: embed, merge, custom, native, skip"_L1},
            {"set_export_hint/options"_L1, "JSON object with optional keys: id (string, identifier for binding — empty string to clear), visible (bool), componentName (string, for custom type), baseElement (string: Container, TouchArea, Button, Button_Highlighted, for native type), properties (array of strings: visible, color, position, text, size, image — controls which attributes are exported as bindable properties)"_L1},

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

    parser.process(app);

    McpServer server(parser.value(backendOption));
    QObject::connect(&server, &QMcpServer::finished, &app, &QCoreApplication::quit);
    server.start(parser.value(addressOption));

    return app.exec();
}

#include "main.moc"
