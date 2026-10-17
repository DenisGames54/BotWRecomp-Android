// iOS build only: Renderer.cpp includes <png.h> but calls nothing from libpng (the macOS build
// picks the header up from Homebrew and links no libpng either). Empty on purpose.
#pragma once
