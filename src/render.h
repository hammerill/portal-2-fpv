// Draws libfpv's fisheye lens, OSD and F8 overlay over the game's finished
// frame (D3D9, right before Present, on the render thread).
#pragma once

#include <d3d9.h>

namespace render {

// in_game: a level is loaded (the overlay draws in the menus too).
void frame(IDirect3DDevice9 *dev, bool in_game);

// Width / height of the game's back buffer (16:9 until the first frame). Any thread.
float aspect();

// Releases the textures (device reset, core unload).
void release();

}  // namespace render
