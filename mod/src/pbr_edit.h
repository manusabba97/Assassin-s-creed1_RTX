#pragma once

#include "pbr.h"

#include <cstdint>
#include <string>

// Hand edits of a PBR material, written by the Remix runtime's material editor (rtx_fork_material_editor.cpp, key M)
// to maps\<hash>\<variant>\<hash>_edit.json and applied here to the packed maps (decoded level 0 -> adjustments ->
// mips rebuilt -> re-encoded in the same block format).
namespace ac1rtx::pbr {

struct MapEdit {
  bool enabled = true;     // false: map dropped from the material (Remix uses its constant)
  float brightness = 0.f;  // albedo: multiplier - 1; grey maps: offset
  float contrast = 1.f;
  float blur = 0.f;        // gaussian radius in texels of level 0
  bool invert = false;

  bool identity() const { return brightness == 0.f && contrast == 1.f && blur == 0.f && !invert; }
};

struct Edit {
  bool present = false;  // an edit file exists
  bool enabled = true;   // whole PBR material
  MapEdit map[kMapCount];
  float albedoSaturation = 1.f;
  float tint[3] = { 1.f, 1.f, 1.f };
  float normalStrength = 1.f;
  bool pomEnabled = true;
  float pomDepthCm = -1.f;  // < 0: the workflow's depth
  float pomOutCm = 0.f;     // extra outward displacement (older edits; the editor folds it into pomDepthCm)
  float pomInCm = 0.f;      // inward displacement (Remix displaceIn): the height map's black sinks below the surface
};

std::wstring editPath(uint64_t hash);
// Last write time of the edit file (0 when missing), to detect the editor's changes.
uint64_t editStamp(uint64_t hash);
Edit readEdit(uint64_t hash);

// Applies `edit` to the packed maps in place; disabled maps are emptied.
void applyEdit(Override& maps, const Edit& edit);

// Changes the editor asks to write into the map files: <hash>_save.json ("Salva nelle texture": an edit whose
// adjustments are applied to each map) and <hash>_bake.txt (lines "height invert" / "normal invert"). The packed .ac1t
// and the newest source PNG of each map are rewritten, then the request files are deleted. Returns the mask
// (1 << Map) of the maps rewritten.
bool bakePending(uint64_t hash);
uint32_t bakeRequests(uint64_t hash);

} // namespace ac1rtx::pbr
