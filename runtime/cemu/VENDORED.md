# Vendored Cemu source

Files under this directory are copied from Cemu (https://github.com/cemu-project/Cemu, commit a53cf0e,
MPL-2.0) and keep Cemu's paths so its includes resolve. They are the Latte GPU layer (PM4 command
processor, shader decompiler, texture/surface cache, Metal renderer) plus the Common headers they need.
Changes for this runtime are marked `// BotWRecomp:`; Cemu subsystems that are not vendored (config,
graphic packs, overlay, disk caches, OS) are provided by shims in `runtime/cemu_shims/`.
