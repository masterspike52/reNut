# Codegen template fragments

rexglue's codegen accepts one override folder (`template_dir` in the manifest), and each file there replaces a stock template entirely. Two modules that each shipped their own `pch_h.inja` would overwrite each other. RexFabric solves this for itself and for any other module: modules contribute fragments, and RexFabric merges all of them into the one folder.

## Adding a fragment

After including RexFabric, any module's CMake can call:

```cmake
rexfabric_template_fragment(codegen/pch_h BEFORE "<anchor line>" FILE my_fragment.inja)
rexfabric_template_fragment(codegen/pch_h AFTER "<anchor line>" FILE my_fragment.inja)
rexfabric_template_fragment(codegen/init_cpp APPEND FILE my_fragment.inja)
```

The first argument is the template's id: its path under the SDK's `resources/templates`, without `.inja`. `BEFORE` and `AFTER` place the fragment at an anchor, which must appear exactly once in the stock template. `APPEND` adds it at the end. Fragments are inja like the templates around them, so they can use the same data (`functions`, `config_flags`, `project` and the rest).

Fragments are applied in the order they were added, after `rexfabric_attach` and every other module has run.

## Where the result goes

The project's manifest names the folder:

```toml
[entrypoint]
template_dir = "generated/templates"
```

RexFabric writes only the templates that have fragments, removes ones that no longer do, and leaves a file untouched when its text hasn't changed. Codegen counts that folder's files as inputs, so an edited fragment reruns codegen on the next build.

## Finding the stock templates

The installed SDK doesn't ship its templates, so RexFabric reads them from the SDK source tree, which must match the SDK the project builds with. It uses `REXSDK_DIR` when the project builds the SDK from source; otherwise set `REXFABRIC_SDK_TEMPLATES` to `<rexglue-sdk>/resources/templates`.

## When an SDK update breaks a fragment

Configuring fails with the template, the anchor and the fragment file. Look at the new stock template, pick an anchor that still means the same place, and update the fragment if the code around it changed.
