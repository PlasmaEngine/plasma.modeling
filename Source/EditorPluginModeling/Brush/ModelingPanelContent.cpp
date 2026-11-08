#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/ModelingPanelContent.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>

namespace
{
  using Cmd = plModelingCommand;

  const plModelingPanelContent::Shape c_Shapes[] = {
    {"Box", plBrushShape::Box, "ShapeBox"},
    {"Ramp X", plBrushShape::RampX, "ShapeRampX"},
    {"Ramp Y", plBrushShape::RampY, "ShapeRampY"},
    {"Column", plBrushShape::Column, "ShapeColumn"},
    {"Stairs X", plBrushShape::StairsX, "ShapeStairsX"},
    {"Stairs Y", plBrushShape::StairsY, "ShapeStairsY"},
    {"Arch X", plBrushShape::ArchX, "ShapeArchX"},
    {"Arch Y", plBrushShape::ArchY, "ShapeArchY"},
    {"Spiral", plBrushShape::SpiralStairs, "ShapeSpiral"},
    {"Sphere", plBrushShape::Sphere, "ShapeSphere"},
    {"Cone", plBrushShape::Cone, "ShapeCone"},
    {"Pyramid", plBrushShape::Pyramid, "ShapePyramid"},
  };

  const Cmd::Enum c_SelectCommands[] = {Cmd::SelectAll, Cmd::SelectNone, Cmd::SelectInvert, Cmd::SelectGrow, Cmd::SelectShrink, Cmd::SelectSameMaterial, Cmd::SelectLoop, Cmd::SelectRing};

  const Cmd::Enum c_EditCommands[] = {Cmd::Extrude, Cmd::Inset, Cmd::Bevel, Cmd::LoopCut, Cmd::Bridge, Cmd::Weld, Cmd::Flip, Cmd::Subdivide, Cmd::Detach, Cmd::Delete, Cmd::FillHole, Cmd::Connect, Cmd::Triangulate, Cmd::Flatten};

  const Cmd::Enum c_CsgCommands[] = {Cmd::ToggleCutter, Cmd::Hollow, Cmd::Union, Cmd::Clip, Cmd::Decompose, Cmd::Group, Cmd::BakeCuts};

  const Cmd::Enum c_PivotCommands[] = {Cmd::EditPivot, Cmd::PivotToCenter, Cmd::PivotToBottom, Cmd::PivotToSelection};

  const plModelingPanelContent::CommandGroup c_Groups[] = {
    {"Select", c_SelectCommands, "Drag a box in the viewport to select. Shift adds, Ctrl removes."},
    {"Edit", c_EditCommands,
      "Faces: extrude, inset, bridge, flip, subdivide, detach. Edges: bevel, loop cut, fill hole. Vertices: weld, connect. "
      "Extrude, inset, bevel and loop cut follow the mouse: type a value, Enter applies, Esc cancels."},
    {"CSG", c_CsgCommands,
      "Make Cutter turns a brush into a cutter: it cuts live, so moving or resizing it moves the opening, and turning it back restores the brush. "
      "A cutter through the walls of inside-out brushes (rooms) opens a passage between them. A cutter must be convex; the brush it cuts can be any closed shape. "
      "Cutters only cut brushes in their own brush group. Bake Cuts makes a cut permanent and deletes the cutter."},
    {"Pivot", c_PivotCommands, "The pivot is the brush's origin: it moves, the geometry stays. Edit Pivot shows a gizmo for it; hold Ctrl while dragging to snap it to vertices or the grid."},
  };

  const plModelingPanelContent::JustifyMode c_Justify[] = {
    {'L', "JustifyLeft", "Justify left: move the texture so a tile starts at the face's left edge"},
    {'R', "JustifyRight", "Justify right: move the texture so a tile ends at the face's right edge"},
    {'T', "JustifyTop", "Justify top: move the texture so a tile starts at the face's top edge"},
    {'B', "JustifyBottom", "Justify bottom: move the texture so a tile ends at the face's bottom edge"},
    {'C', "JustifyCenter", "Centre: move the texture so a tile is centred on the face"},
    {'F', "JustifyFit", "Fit: stretch one tile over the whole face"},
  };

  const plModelingPanelContent::Output c_Outputs[] = {
    {"Export .obj", "ExportObj", "Write the selected brushes (as rendered, cuts included) to an OBJ file with an MTL sidecar next to the scene."},
    {"Bake to mesh", "BakeMesh", "Export the selected brushes to Blockout/<name>.obj, create a mesh asset for it, and replace each brush with a mesh component using it."},
    {"Grey Boxing \xE2\x86\x92 Brush", "GreyboxToBrush", "Convert every Grey Boxing object in the scene into an equivalent brush, as one undo step."},
    {"Mesh \xE2\x86\x92 Brush", "MeshToBrush", "Turn the selected mesh components into editable brushes, keeping their UVs and materials."},
  };
} // namespace

plArrayPtr<const plModelingPanelContent::Shape> plModelingPanelContent::GetShapes()
{
  return c_Shapes;
}

const char* plModelingPanelContent::GetDrawHint()
{
  return "Click a shape, then hold Ctrl and drag on the grid. New brushes go into the selected brush group.";
}

plArrayPtr<const plModelingPanelContent::CommandGroup> plModelingPanelContent::GetCommandGroups()
{
  return c_Groups;
}

plArrayPtr<const plModelingPanelContent::JustifyMode> plModelingPanelContent::GetJustifyModes()
{
  return c_Justify;
}

plArrayPtr<const plModelingPanelContent::Output> plModelingPanelContent::GetOutputs()
{
  return c_Outputs;
}

const char* plModelingPanelContent::GetOutputHint()
{
  return "Collision, navmesh, occluder and export merging are per brush, in Properties. At scene export, cutters are baked in and static brushes of a brush group merge into one mesh per material.";
}

const char* plModelingPanelContent::GetCommandIcon(plModelingCommand::Enum cmd)
{
  switch (cmd)
  {
    // clang-format off
    case Cmd::ModeObject: return "ModeObject";
    case Cmd::ModeVertex: return "ModeVertex";
    case Cmd::ModeEdge: return "ModeEdge";
    case Cmd::ModeFace: return "ModeFace";
    case Cmd::SelectAll: return "SelectAll";
    case Cmd::SelectNone: return "SelectNone";
    case Cmd::SelectInvert: return "SelectInvert";
    case Cmd::SelectGrow: return "SelectGrow";
    case Cmd::SelectShrink: return "SelectShrink";
    case Cmd::SelectLoop: return "SelectLoop";
    case Cmd::SelectRing: return "SelectRing";
    case Cmd::SelectSameMaterial: return "SelectSameMaterial";
    case Cmd::Extrude: return "Extrude";
    case Cmd::Inset: return "Inset";
    case Cmd::Bevel: return "Bevel";
    case Cmd::LoopCut: return "LoopCut";
    case Cmd::Bridge: return "Bridge";
    case Cmd::Weld: return "Weld";
    case Cmd::Flip: return "Flip";
    case Cmd::Subdivide: return "Subdivide";
    case Cmd::Detach: return "Detach";
    case Cmd::Delete: return "Delete";
    case Cmd::FillHole: return "FillHole";
    case Cmd::Connect: return "Connect";
    case Cmd::Triangulate: return "Triangulate";
    case Cmd::Flatten: return "Flatten";
    case Cmd::ToggleCutter: return "ToggleCutter";
    case Cmd::BakeCuts: return "Carve";
    case Cmd::Hollow: return "Hollow";
    case Cmd::Union: return "Union";
    case Cmd::Clip: return "Clip";
    case Cmd::Decompose: return "Decompose";
    case Cmd::Group: return "Group";
    case Cmd::EditPivot: return "EditPivot";
    case Cmd::PivotToCenter: return "PivotCenter";
    case Cmd::PivotToBottom: return "PivotBottom";
    case Cmd::PivotToSelection: return "PivotSelection";
    default: return nullptr;
      // clang-format on
  }
}

plString plModelingPanelContent::GetLineIconPath(plStringView sName)
{
  return plStringBuilder(":/ModelingIcons/Line/", sName, ".svg");
}

plString plModelingPanelContent::GetCommandTooltip(plModelingCommand::Enum cmd)
{
  const plString sKey = plBrushActions::GetShortcutText(cmd);

  plStringBuilder sTooltip = plBrushActions::GetDisplayName(cmd);
  if (!sKey.IsEmpty())
    sTooltip.AppendFormat(" ({})", sKey);

  return sTooltip;
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_ModelingPanelContent);
